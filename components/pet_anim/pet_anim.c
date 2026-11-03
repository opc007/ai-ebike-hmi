/*
 * 宠物动画播放器
 *
 * 两层合成模型：
 *   基础层（运动）  idle / run / brake / fall
 *   叠加层（语音）  talking
 * 优先级：fallen > talking > braking > running > idle
 *
 * 解码走 ESP32-S3 的硬件 JPEG 解码器，帧率 20fps @480x480 没有压力。
 */

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pet_anim.h"
#include "pet_pack.h"

static const char *TAG = "pet_anim";

/* 由 pet_imu.c 提供 */
extern esp_err_t pet_imu_init(int sda, int scl, uint8_t addr, void *bus_handle);
extern esp_err_t pet_imu_start(void);
extern esp_err_t pet_imu_set_motion(pet_motion_t m);
extern pet_motion_t pet_imu_get_motion(void);
extern esp_err_t pet_imu_force_motion(int m);
extern float pet_imu_last_sigma(void);
extern bool pet_imu_is_calibrated(void);

typedef struct {
    pet_anim_config_t cfg;
    pet_pack_t pack;
    uint8_t *pack_buf;     /* 自己加载时的内存（PSRAM） */
    uint8_t *rgb;          /* 解码输出缓冲（show_test 用） */
    size_t rgb_size;
    void *njd;             /* esp_new_jpeg 解码器句柄（复用，随包创建） */
    uint8_t *nj_out;       /* 解码输出（16 字节对齐，RGB565_BE） */
    int nj_out_len;        /* 当前 outbuf 容量 */
    uint16_t w, h;
    uint8_t fps;

    volatile bool running;
    volatile bool talking;
    volatile int device_state;   /* 宿主状态机快照（观测用） */
    TaskHandle_t task;

    /* 情绪通道：emo_anim 为空表示没有生效中的情绪 */
    volatile pet_emotion_t emotion;
    char emo_anim[PET_ANIM_NAME_LEN];
    int64_t emo_until_us;

    int cur_anim;
    uint16_t cur_frame;
    volatile uint32_t frames_sent;   /* 累计成功送屏帧数（观测用） */
    int64_t cooldown_until_us; /* 一次性动画播完后短暂锁定，防重复触发 */
} ctx_t;

static ctx_t s_ctx;

/* 联调诊断：播放任务卡死时，靠这几个值判断卡在哪一步（板级 flush 也会写 strip）。
 * phase: 0=调度 1=JPEG 解码 2=送屏中 3=本帧完成 */
volatile uint8_t pet_dbg_phase = 0;
volatile int32_t pet_dbg_strip = -1;
volatile uint32_t pet_dbg_lock_fail = 0;
/* 单帧耗时拆解（指数平均，单位微秒）：判断瓶颈在解码还是送屏 */
volatile uint32_t pet_dbg_decode_us = 0;
volatile uint32_t pet_dbg_flush_us = 0;
/* 送屏再拆两段：memcpy 进内部 SRAM 的累计 / draw_bitmap 的累计 */
volatile uint32_t pet_dbg_copy_us = 0;
volatile uint32_t pet_dbg_draw_us = 0;

/* pet_imu.c 里的状态机用这个名字，避免与公开接口重名 */
esp_err_t pet_imu_calibrate(uint32_t ms);

static const char *anim_name_for(pet_motion_t m, bool talking) {
    if (m == PET_MOTION_FALLEN) return "fall";
    if (talking) return "talk";
    if (m == PET_MOTION_BRAKING) return "brake";
    if (m == PET_MOTION_RUNNING) return "run";
    return "idle";
}

static bool anim_is_once(const char *name) {
    return strcmp(name, "brake") == 0 || strcmp(name, "fall") == 0;
}

/* 情绪 → 段名 + 保持时长。
 *
 * 小智的 SetEmotion 用的是固定那几十个英文词，这里只翻译成我们资源包里
 * 真有的段；没对应段的情绪（比如 cool、winking）就当作回 idle，不硬切。
 * hold 到期自动回落，避免「说了一句开心的话，后面一直傻笑」。
 */
typedef struct {
    const char *key;      /* 小智的情绪字符串 */
    const char *anim;     /* 资源包里的段名；"" 表示不切段 */
    uint32_t hold_ms;
} emo_map_t;

static const emo_map_t k_emotions[] = {
    {"neutral",     "",           0},
    {"relaxed",     "",           0},
    {"cool",        "",           0},
    {"thinking",    "thinking",   4000},
    {"confused",    "thinking",   3000},
    {"happy",       "happy",      5000},
    {"laughing",    "happy",      5000},
    {"funny",       "happy",      5000},
    {"excited",     "happy",      5000},
    {"loving",      "happy",      5000},
    {"kissy",       "happy",      4000},
    {"delicious",   "happy",      4000},
    {"confident",   "happy",      4000},
    {"winking",     "happy",      3000},
    {"silly",       "happy",      4000},
    {"sad",         "sad",        5000},
    {"crying",      "sad",        6000},
    {"embarrassed", "sad",        4000},
    {"sleepy",      "sad",        6000},
    {"angry",       "angry",      4000},
    {"shocked",     "surprised",  3000},
    {"surprised",   "surprised",  3000},
    {"fall",        "fall",       8000},
};

static void set_emo(const char *key, const char *anim, uint32_t hold_ms) {
    if (!anim || !anim[0] || hold_ms == 0) {
        s_ctx.emo_anim[0] = '\0';
        s_ctx.emo_until_us = 0;
        return;
    }
    strncpy(s_ctx.emo_anim, anim, PET_ANIM_NAME_LEN - 1);
    s_ctx.emo_anim[PET_ANIM_NAME_LEN - 1] = '\0';
    s_ctx.emo_until_us = esp_timer_get_time() + (int64_t)hold_ms * 1000;
    ESP_LOGI(TAG, "emotion %s → %s (%" PRIu32 "ms)", key, anim, hold_ms);
}

static esp_err_t decode_and_flush(const uint8_t *jpg, uint32_t len) {
    jpeg_dec_io_t io = {
        .inbuf = (uint8_t *)jpg,
        .inbuf_len = (int)len,
    };
    jpeg_dec_header_info_t info;
    pet_dbg_phase = 1;
    int64_t t_dec0 = esp_timer_get_time();

    jpeg_error_t e = jpeg_dec_parse_header(s_ctx.njd, &io, &info);
    if (e != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg parse header fail: %d", (int)e);
        pet_dbg_phase = 0;
        return ESP_FAIL;
    }
    int out_len = 0;
    if (jpeg_dec_get_outbuf_len(s_ctx.njd, &out_len) != JPEG_ERR_OK || out_len <= 0) {
        ESP_LOGW(TAG, "jpeg outbuf len fail");
        pet_dbg_phase = 0;
        return ESP_FAIL;
    }
    /* 输出缓冲按需扩：480x480 RGB565_BE = 460800B，半分辨率只要 1/4 */
    if (out_len > s_ctx.nj_out_len || !s_ctx.nj_out) {
        void *nb = heap_caps_aligned_alloc(16, out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) nb = heap_caps_aligned_alloc(16, out_len, MALLOC_CAP_8BIT);
        if (!nb) {
            ESP_LOGE(TAG, "解码输出缓冲分配失败（%d 字节）", out_len);
            pet_dbg_phase = 0;
            return ESP_ERR_NO_MEM;
        }
        if (s_ctx.nj_out) heap_caps_free(s_ctx.nj_out);
        s_ctx.nj_out = nb;
        s_ctx.nj_out_len = out_len;
        ESP_LOGI(TAG, "解码输出缓冲 %d 字节", out_len);
    }
    io.outbuf = s_ctx.nj_out;
    io.out_size = out_len;

    int count = 1;
    jpeg_dec_get_process_count(s_ctx.njd, &count);
    for (int i = 0; i < count; i++) {
        e = jpeg_dec_process(s_ctx.njd, &io);
        if (e != JPEG_ERR_OK) {
            ESP_LOGW(TAG, "jpeg decode fail: %d", (int)e);
            pet_dbg_phase = 0;
            return ESP_FAIL;
        }
    }
    int64_t t_dec1 = esp_timer_get_time();

    /* 前 5 帧打印解码尺寸：和包头对不上就是错位的来源 */
    if (s_ctx.frames_sent < 5) {
        ESP_LOGW(TAG, "帧#%u 解码 %ux%u（包头 %ux%u）",
                 (unsigned)s_ctx.frames_sent, info.width, info.height,
                 s_ctx.w, s_ctx.h);
    }
    pet_dbg_phase = 2;
    pet_dbg_strip = -1;
    esp_err_t rc = s_ctx.cfg.flush(io.outbuf, info.width, info.height);
    int64_t t_fl1 = esp_timer_get_time();
    if (rc == ESP_OK) s_ctx.frames_sent++;
    /* 8 帧滑动平均，避免单帧抖动干扰判断 */
    pet_dbg_decode_us = (pet_dbg_decode_us * 7 + (uint32_t)(t_dec1 - t_dec0)) / 8;
    pet_dbg_flush_us = (pet_dbg_flush_us * 7 + (uint32_t)(t_fl1 - t_dec1)) / 8;
    pet_dbg_phase = 3;
    return rc;
}

static void play_task(void *arg) {
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    int64_t next_us = esp_timer_get_time();
    const int64_t frame_us = 1000000 / (s_ctx.fps ? s_ctx.fps : 20);

    while (s_ctx.running) {
        pet_dbg_phase = 0;
        pet_dbg_strip = -1;
        pet_motion_t motion = pet_anim_get_motion();
        const char *want = anim_name_for(motion, s_ctx.talking);

        /* 情绪优先于「说话」和「骑行」：AI 说「今天好开心」时就该笑，
           而不是一直张着嘴播 talk。刹车/摔倒仍然压过情绪（安全优先）。 */
        if (motion != PET_MOTION_FALLEN && motion != PET_MOTION_BRAKING &&
            s_ctx.emo_anim[0] && esp_timer_get_time() < s_ctx.emo_until_us) {
            want = s_ctx.emo_anim;
        }

        /* 一次性动画播完后冷却 1.5 秒，避免刹车时反复重播第一帧 */
        if (esp_timer_get_time() < s_ctx.cooldown_until_us) {
            want = s_ctx.talking ? "talk" : "idle";
        }

        int idx = pet_pack_find(&s_ctx.pack, want);
        if (idx < 0) {
            /* 包里没有这一段（比如只生成了 4 段的包），退回说话/待机 */
            idx = pet_pack_find(&s_ctx.pack, s_ctx.talking ? "talk" : "idle");
        }
        if (idx < 0) idx = pet_pack_find(&s_ctx.pack, "idle");
        if (idx < 0) {
            ESP_LOGE(TAG, "资源包里没有 idle，退出播放");
            break;
        }

        bool switched = (idx != s_ctx.cur_anim);
        if (switched) {
            s_ctx.cur_anim = idx;
            s_ctx.cur_frame = 0;
            /* 联调关键日志：谁把画面切走了，一眼能看出来 */
            ESP_LOGI(TAG, "→ %s（motion=%d emo=%d talk=%d sigma=%.3f）",
                     s_ctx.pack.anims[idx].name, (int)motion, (int)s_ctx.emotion,
                     (int)s_ctx.talking, pet_imu_last_sigma());
        }

        const pet_pack_anim_t *a = &s_ctx.pack.anims[idx];
        if (s_ctx.cur_frame >= a->frame_count) {
            /* 一段播完了 */
            if (a->loop) {
                s_ctx.cur_frame = 0;
            } else {
                s_ctx.cooldown_until_us = esp_timer_get_time() + 1500000;
                s_ctx.cur_frame = 0;
                continue;
            }
        }

        const uint8_t *jpg = NULL;
        uint32_t jlen = 0;
        if (pet_pack_frame(&s_ctx.pack, idx, s_ctx.cur_frame, &jpg, &jlen) == ESP_OK) {
            decode_and_flush(jpg, jlen);
        }
        s_ctx.cur_frame++;

        /* 心跳：每 60 帧打一行，肉眼就能看出播放有没有卡住、实际帧率多少 */
        static int64_t hb_us = 0;
        static uint32_t hb_frames = 0;
        hb_frames++;
        if (hb_frames >= 60) {
            int64_t now_us = esp_timer_get_time();
            if (hb_us) {
                ESP_LOGI(TAG, "心跳: %u 帧 / %.1f s = %.1f fps（段=%s 帧=%u/%u）",
                         (unsigned)hb_frames, (now_us - hb_us) / 1e6,
                         hb_frames * 1e6 / (double)(now_us - hb_us),
                         s_ctx.pack.anims[idx].name, s_ctx.cur_frame,
                         s_ctx.pack.anims[idx].frame_count);
            }
            hb_us = now_us;
            hb_frames = 0;
        }

        next_us += frame_us;
        int64_t now = esp_timer_get_time();
        int64_t wait = next_us - now;
        if (wait < 0) {
            /* 落后太多就重新对齐，避免累积误差导致疯狂补帧 */
            if (wait < -frame_us * 3) next_us = now;
            wait = 0;
        }
        vTaskDelay(pdMS_TO_TICKS((wait + 999) / 1000));
    }

    if (s_ctx.task == me) s_ctx.task = NULL;
    vTaskDelete(NULL);
}

/* ---------------- 对外接口 ---------------- */

esp_err_t pet_anim_init(const pet_anim_config_t *cfg) {
    if (!cfg || !cfg->flush) return ESP_ERR_INVALID_ARG;

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.cfg = *cfg;
    s_ctx.cur_anim = -1;

    if (cfg->imu_sda >= 0 || cfg->i2c_bus) {
        esp_err_t err = pet_imu_init(cfg->imu_sda, cfg->imu_scl, cfg->imu_addr, cfg->i2c_bus);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "IMU 初始化失败（%s），退化为手动模式", esp_err_to_name(err));
        }
    }
    return ESP_OK;
}

esp_err_t pet_anim_load_pack_mem(const uint8_t *data, size_t len) {
    if (!data || !len) return ESP_ERR_INVALID_ARG;

    /* 先解析到临时结构：pet_pack_parse 会 memset 输出，
       直接喂 &s_ctx.pack 的话，一个新包解析失败就把正在播的旧包清空了。 */
    pet_pack_t tmp;
    esp_err_t err = pet_pack_parse(data, len, &tmp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "资源包解析失败: %s", esp_err_to_name(err));
        return err;
    }
    s_ctx.pack = tmp;
    s_ctx.w = s_ctx.pack.width;
    s_ctx.h = s_ctx.pack.height;
    s_ctx.fps = s_ctx.pack.fps;

    if (s_ctx.cfg.width && s_ctx.cfg.height &&
        (s_ctx.cfg.width != s_ctx.w || s_ctx.cfg.height != s_ctx.h)) {
        ESP_LOGW(TAG, "资源包 %ux%u 与屏幕 %ux%u 不一致，请重新生成",
                 s_ctx.w, s_ctx.h, s_ctx.cfg.width, s_ctx.cfg.height);
    }

    size_t need = (size_t)s_ctx.w * s_ctx.h * 2;
    /* 换宠物时会重新进来，先把上一只的缓冲还回去，否则每换一次漏 460KB */
    if (s_ctx.rgb) {
        heap_caps_free(s_ctx.rgb);
        s_ctx.rgb = NULL;
    }
    s_ctx.rgb = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ctx.rgb) s_ctx.rgb = heap_caps_malloc(need, MALLOC_CAP_8BIT);
    if (!s_ctx.rgb) {
        ESP_LOGE(TAG, "解码缓冲分配失败，需要 %u 字节", (unsigned)need);
        return ESP_ERR_NO_MEM;
    }
    s_ctx.rgb_size = need;

    /* esp_new_jpeg 解码器：比 TJpgDec（esp_jpeg/ROM 版）快，且直接输出
       RGB565_BE（屏要的大端），省掉整个颜色转换环节。
       句柄随包复用；输出缓冲 16 字节对齐、首次用时按需扩。 */
    if (!s_ctx.njd) {
        jpeg_dec_config_t dcfg = DEFAULT_JPEG_DEC_CONFIG();
        dcfg.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
        if (jpeg_dec_open(&dcfg, &s_ctx.njd) != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "esp_new_jpeg 打开失败");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "esp_new_jpeg 解码器就绪（RGB565_BE）");
    }

    ESP_LOGI(TAG, "资源包已加载 %ux%u @%ufps，%u 段，%.2f MB", s_ctx.w, s_ctx.h,
             s_ctx.fps, s_ctx.pack.anim_count, s_ctx.pack.size / 1048576.0f);
    return ESP_OK;
}

esp_err_t pet_anim_load_pack_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "打不开 %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 12 * 1024 * 1024) {
        fclose(f);
        ESP_LOGE(TAG, "文件大小异常: %ld", sz);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 先读进新缓冲、解析成功后才替换旧包。
       反过来的话，解析失败就把当前正在播的宠物弄丢了（还没法重试）。 */
    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "PSRAM 不足，%ld 字节", sz);
        return ESP_ERR_NO_MEM;
    }
    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        heap_caps_free(buf);
        return ESP_FAIL;
    }

    esp_err_t err = pet_anim_load_pack_mem(buf, sz);
    if (err != ESP_OK) {
        heap_caps_free(buf);
        return err;
    }
    uint8_t *old = s_ctx.pack_buf;
    s_ctx.pack_buf = buf;
    if (old && old != buf) heap_caps_free(old);
    return ESP_OK;
}

esp_err_t pet_anim_start(void) {
    if (s_ctx.pack.anim_count == 0) return ESP_ERR_INVALID_STATE;
    if (s_ctx.running) return ESP_OK;

    pet_imu_start();
    /* 没标定过就自动标一次（1.2 秒），否则六轴永远只输出 IDLE。
       装车后第一次上电请保持车辆静止。 */
    if (!pet_imu_is_calibrated()) {
        ESP_LOGW(TAG, "IMU 未标定，自动采样 1.2 秒，请保持静止");
        pet_imu_calibrate(1200);
    }

    s_ctx.running = true;
    uint8_t prio = s_ctx.cfg.task_priority ? s_ctx.cfg.task_priority : 4;
    uint32_t stack = s_ctx.cfg.task_stack ? s_ctx.cfg.task_stack : 8192;
    if (xTaskCreate(play_task, "pet_play", stack, NULL, prio, &s_ctx.task) != pdPASS) {
        s_ctx.running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void pet_anim_stop(void) {
    s_ctx.running = false;
    /* 任务自行退出，这里不强制删除以免死锁 */
}

bool pet_anim_wait_stopped(uint32_t timeout_ms) {
    s_ctx.running = false;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (s_ctx.task != NULL && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return s_ctx.task == NULL;
}

bool pet_anim_is_running(void) { return s_ctx.running; }

esp_err_t pet_anim_show_test(uint32_t ms, int mode, uint16_t color) {
    if (!s_ctx.rgb || !s_ctx.w) return ESP_ERR_INVALID_STATE;
    int64_t deadline = esp_timer_get_time() + (int64_t)ms * 1000;
    uint16_t *p = (uint16_t *)s_ctx.rgb;
    const uint16_t cols[3] = {0xF800, 0x07E0, 0x001F};   // R G B
    while (esp_timer_get_time() < deadline) {
        if (mode == 0) {
            for (size_t i = 0; i < (size_t)s_ctx.w * s_ctx.h; i++) p[i] = color;
        } else {
            for (uint16_t y = 0; y < s_ctx.h; y++) {
                for (uint16_t x = 0; x < s_ctx.w; x++) {
                    p[(size_t)y * s_ctx.w + x] = cols[(x / 64) % 3];
                }
            }
        }
        esp_err_t rc = s_ctx.cfg.flush(s_ctx.rgb, s_ctx.w, s_ctx.h);
        if (rc != ESP_OK) return rc;
        s_ctx.frames_sent++;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    return ESP_OK;
}

void pet_anim_set_motion(pet_motion_t m) {
    pet_imu_set_motion(m);
}

/* 远程调试：强制运动状态，-1 恢复自动。 */
esp_err_t pet_anim_force_motion(int m) {
    return pet_imu_force_motion(m);
}

void pet_anim_set_talking(bool talking) { s_ctx.talking = talking; }

void pet_anim_set_device_state(int state) { s_ctx.device_state = state; }

/* ---------------- 情绪通道 ---------------- */

static pet_emotion_t emo_of_anim(const char *a) {
    if (!a || !a[0]) return PET_EMO_NEUTRAL;
    if (!strcmp(a, "happy")) return PET_EMO_HAPPY;
    if (!strcmp(a, "sad")) return PET_EMO_SAD;
    if (!strcmp(a, "angry")) return PET_EMO_ANGRY;
    if (!strcmp(a, "surprised")) return PET_EMO_SURPRISED;
    if (!strcmp(a, "thinking")) return PET_EMO_THINKING;
    return PET_EMO_NEUTRAL;
}

void pet_anim_set_emotion_name(const char *name) {
    if (!name || !name[0]) {
        pet_anim_clear_emotion();
        return;
    }
    /* 小智传的可能是 "Happy" / "happy"，统一转小写再查表 */
    char key[24];
    size_t n = strlen(name);
    if (n >= sizeof(key)) n = sizeof(key) - 1;
    for (size_t i = 0; i < n; i++) key[i] = (char)tolower((unsigned char)name[i]);
    key[n] = '\0';

    for (size_t i = 0; i < sizeof(k_emotions) / sizeof(k_emotions[0]); i++) {
        if (strcmp(k_emotions[i].key, key) == 0) {
            s_ctx.emotion = emo_of_anim(k_emotions[i].anim);
            set_emo(key, k_emotions[i].anim, k_emotions[i].hold_ms);
            return;
        }
    }
    /* 表里没有的情绪：按中性处理，不要卡在上一张脸上 */
    ESP_LOGD(TAG, "未知情绪 %s，按中性处理", key);
    s_ctx.emotion = PET_EMO_NEUTRAL;
    set_emo(key, "", 0);
}

void pet_anim_set_emotion(pet_emotion_t e) {
    static const char *names[PET_EMO_COUNT] = {
        "none", "neutral", "happy", "sad", "angry", "surprised", "thinking"
    };
    if (e < 0 || e >= PET_EMO_COUNT) return;
    pet_anim_set_emotion_name(names[e]);
}

pet_emotion_t pet_anim_get_emotion(void) { return s_ctx.emotion; }

void pet_anim_clear_emotion(void) {
    s_ctx.emotion = PET_EMO_NONE;
    s_ctx.emo_anim[0] = '\0';
    s_ctx.emo_until_us = 0;
}

void pet_anim_get_status(pet_anim_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->motion = pet_imu_get_motion();
    out->talking = s_ctx.talking;
    out->emotion = s_ctx.emotion;
    out->device_state = s_ctx.device_state;
    out->sigma = pet_imu_last_sigma();
    out->frame = s_ctx.cur_frame;
    out->frames = s_ctx.frames_sent;
    out->phase = pet_dbg_phase;
    out->strip = pet_dbg_strip;
    if (s_ctx.cur_anim >= 0 && s_ctx.cur_anim < s_ctx.pack.anim_count) {
        const pet_pack_anim_t *a = &s_ctx.pack.anims[s_ctx.cur_anim];
        out->anim = a->name;
        out->frame_count = a->frame_count;
    } else {
        out->anim = "-";
    }
}

pet_motion_t pet_anim_get_motion(void) { return pet_imu_get_motion(); }

float pet_imu_last_sigma(void);
