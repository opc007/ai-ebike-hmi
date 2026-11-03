#include "wifi_board.h"
#include "display/lcd_display.h"
#include "esp_lcd_co5300.h"

#include "codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "config.h"
#include "power_save_timer.h"
#include "axp2101.h"
#include "i2c_device.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include "esp_io_expander_tca9554.h"
#include "settings.h"

#include <esp_lcd_touch_cst9217.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

// 宠物动画：屏幕接管 + IMU 运动状态 + 情绪联动
#include "pet_anim.h"
#include <esp_spiffs.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <string_view>

// 宠物资源包下载地址（服务器网页端「保存同步」后，这里永远指向最新宠物）
#define PET_PACK_URL "http://10.197.1.234:8080/api/pet/latest.pack"
#define PET_PACK_PATH "/petfs/pet.pack"
#define PET_REPORT_URL "http://10.197.1.234:8080/api/pet/report"
#define PET_CMD_URL "http://10.197.1.234:8080/api/pet/cmd"

// 开机音效：网页端那段卡皮巴拉视频自带 AAC 音轨，服务端抽出后转成 Opus(OGG)
#define PET_BOOT_OGG_URL "http://10.197.1.234:8080/api/pet/latest.ogg"
#define PET_BOOT_OGG_PATH "/petfs/boot.ogg"
#define PET_BOOT_OGG_MAX (256 * 1024)

#define TAG "WaveshareEsp32s3TouchAMOLED2inch16"

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110); // PWRON > OFFLEVEL as POWEROFF Source enable
        WriteReg(0x27, 0x10);  // hold 4s to power off

        // Disable All DCs but DC1
        WriteReg(0x80, 0x01);
        // Disable All LDOs
        WriteReg(0x90, 0x00);
        WriteReg(0x91, 0x00);

        // Set DC1 to 3.3V
        WriteReg(0x82, (3300 - 1500) / 100);

        // Set ALDO1 to 3.3V
        WriteReg(0x92, (3300 - 500) / 100);

        // Enable ALDO1(MIC)
        WriteReg(0x90, 0x01);

        WriteReg(0x64, 0x02); // CV charger voltage setting to 4.1V

        WriteReg(0x61, 0x02); // set Main battery precharge current to 50mA
        WriteReg(0x62, 0x08); // set Main battery charger current to 400mA ( 0x08-200mA, 0x09-300mA, 0x0A-400mA )
        WriteReg(0x63, 0x01); // set Main battery term charge current to 25mA
    }
};

#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_READ_CMD (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

// ---------------- 宠物动画接入 ----------------
static esp_lcd_panel_handle_t s_pet_panel = nullptr;
static SemaphoreHandle_t s_pet_mux = nullptr;   // 宠物送屏专用锁，不与 LVGL 共用
static bool s_lvgl_stopped = false;             // 防重复 lvgl_port_stop（重复停会出问题）

/* 宠物帧送屏：PSRAM 解码帧 → 内部 RAM 条带缓冲 → 分条送屏。
 *
 * 这套参数是照着小智 LVGL 自己验证过的路走：
 *   lcd_display.cc 里 LVGL 用 buff_dma=1（内部 SRAM）、条带 480×20 做局部刷新，
 *   并明确注释「整帧传输没法分配 bounce buffer」。
 *   我们照着做：每条 20 行（19200B，DMA 用内部 RAM），双缓冲交替。
 *
 * 三条铁律（都是踩出来的）：
 *   1) x_end/y_end 是「排他」坐标，传 w-1 会少发一整行 → 白色横杠；
 *   2) 缓冲必须双缓冲：tx_color 是排队即返回，单缓冲会被下一次覆盖 → 错行；
 *   3) 必须持有 LVGL 锁：esp_lcd_panel_io_spi 的 trans_pool 不是线程安全的，
 *      和 LVGL 并发会报 recycle failed，随后整机卡死。
 * 因为 LVGL 已经被停掉渲染，这里持锁 20~30ms 不会影响任何东西。 */
#define PET_STRIP_LINES 32

/* SPI 驱动的 tx_color 每次进来 acquire_bus、出去 release_bus（要等 DMA 排空），
 * 也就是每个条带都是一次完整的串行往返。条带行数决定了这个往返次数：
 * 20 行 = 24 次/帧，32 行 = 15 次/帧，固定开销直接砍掉四成。
 * 再往上（40 行 = 12 次）双缓冲要吃 75KB 内部 SRAM，和 WiFi/音频抢得紧，
 * 32 行（60KB）是当前留有余量的最优解。 */

static uint16_t s_pet_strip[2][480 * PET_STRIP_LINES];
static int s_pet_strip_idx = 0;
static volatile uint32_t s_pet_frames_sent = 0;
static uint8_t *s_pack_mem = nullptr;   // 直接从网络加载的资源包（pet_anim 只存指针）

/* 宠物独占屏幕期间，LVGL 的刷新回调被换成这个「只举手不干活」的版本。
 * 原因：lvgl_port_stop() 只关掉了 LVGL 的 tick 定时器，任务本身还活着；
 * 而 LVGL 的 flush 完成回调是挂在 panel_io 的 on_color_trans_done 上的，
 * 宠物每送一条带都会把它「叫醒」。说话时 UI 有脏区，LVGL 就真的去调
 * draw_bitmap，于是两个写屏方抢同一个 SPI 事务池 → draw_bitmap 里的
 * spi_device_get_trans_result(portMAX_DELAY) 永远等不到 → 画面定死。
 * 换成空回调后，LVGL 无论怎么刷都碰不到总线。 */
static void pet_noop_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

static esp_err_t pet_flush_cb(const uint8_t *rgb565, uint16_t w, uint16_t h) {
    if (!s_pet_panel) return ESP_ERR_INVALID_STATE;
    static int dbg_frames = 0;

    /* 解码器输出 w×h（480×480 原生或 240×240 半分辨率），
     * 屏幕固定 480×480：半分辨率时做 2x 最近邻放大。
     * 最近邻是纯写内部 SRAM 的循环（每帧 5.8 万像素），~2ms，可忽略。 */
    const int SCREEN_W = 480, SCREEN_H = 480;
    if (w == 0 || h == 0 || SCREEN_W % w != 0) return ESP_ERR_INVALID_ARG;
    const int scale = SCREEN_W / w;
    if (scale != 1 && scale != 2) return ESP_ERR_NOT_SUPPORTED;

    if (!lvgl_port_lock(1000)) {
        pet_dbg_lock_fail = pet_dbg_lock_fail + 1;
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    uint32_t copy_us = 0, draw_us = 0;
    const int dst_lines = PET_STRIP_LINES;           /* 屏幕方向的条带行数 */
    const int src_lines = dst_lines / scale;         /* 对应的源行数 */
    for (int dy = 0; dy < SCREEN_H; dy += dst_lines) {
        pet_dbg_strip = dy;
        int sy0 = dy / scale;
        int n_src = (h - sy0) < src_lines ? (h - sy0) : src_lines;
        if (n_src <= 0) break;
        int n_dst = n_src * scale;
        uint16_t *dst = s_pet_strip[s_pet_strip_idx];
        s_pet_strip_idx ^= 1;
        const uint16_t *src = (const uint16_t *)(rgb565 + (size_t)sy0 * w * 2);
        int64_t t0 = esp_timer_get_time();
        if (scale == 1) {
            /* 原生：解码器已输出大端，纯 memcpy（历史上这里逐像素 bswap16） */
            memcpy(dst, src, (size_t)w * n_dst * 2);
        } else {
            /* 2x 最近邻：一行源像素展开成两行，每像素横向写两次 */
            for (int r = 0; r < n_src; r++) {
                const uint16_t *srow = src + (size_t)r * w;
                uint16_t *d0 = dst + (size_t)(r * 2) * SCREEN_W;
                uint16_t *d1 = d0 + SCREEN_W;
                for (int x = 0; x < w; x++) {
                    uint16_t px = srow[x];
                    d0[2 * x] = px; d0[2 * x + 1] = px;
                    d1[2 * x] = px; d1[2 * x + 1] = px;
                }
            }
        }
        int64_t t1 = esp_timer_get_time();
        err = esp_lcd_panel_draw_bitmap(s_pet_panel, 0, dy, SCREEN_W, dy + n_dst, dst);
        int64_t t2 = esp_timer_get_time();
        copy_us += (uint32_t)(t1 - t0);
        draw_us += (uint32_t)(t2 - t1);
        if (err != ESP_OK) {
            if (dbg_frames < 10) {
                ESP_LOGE("pet_dbg", "条带 y=%d err=%s", dy, esp_err_to_name(err));
            }
            break;
        }
    }
    lvgl_port_unlock();
    /* 滑动平均，去掉单帧抖动 */
    pet_dbg_copy_us = (pet_dbg_copy_us * 7 + copy_us) / 8;
    pet_dbg_draw_us = (pet_dbg_draw_us * 7 + draw_us) / 8;

    if (err == ESP_OK) s_pet_frames_sent = s_pet_frames_sent + 1;
    if (dbg_frames < 3) {
        const uint16_t *p = (const uint16_t *)rgb565;
        ESP_LOGW("pet_dbg", "帧#%d %ux%u err=%s TL=%04X TR=%04X C=%04X BL=%04X",
                 dbg_frames, w, h, esp_err_to_name(err),
                 p[0], p[w - 1], p[(size_t)(h / 2) * w + w / 2], p[(size_t)(h - 1) * w]);
        dbg_frames++;
    }
    return err;
}

uint32_t pet_frames_sent(void) { return s_pet_frames_sent; }

/* ---------------- 开机音效 ----------------
 * 音轨不是另外找的素材：就是网页端那段卡皮巴拉视频自己的 AAC 音轨，
 * 服务端用 ffmpeg 抽出来转成 Opus(OGG)，所以宠物一换，开机音效自动跟着换。
 *
 * 两级取用：
 *   1. 本地 /petfs/boot.ogg 有缓存 → 开机 1 秒多就响，和动画基本同步（不等 WiFi）
 *   2. 第一次烧机 / 换过宠物没缓存 → 等 WiFi 起来现拉一次再响
 * 之后每次联网都刷一遍缓存，下次开机就是最新的。
 */
static bool pet_play_ogg_mem(uint8_t *buf, size_t n) {
    if (!buf || n == 0) return false;
    /* PlaySound 只负责拆包、把每帧 Opus 拷进解码队列，返回后这块缓冲就能放掉 */
    Application::GetInstance().GetAudioService().PlaySound(
        std::string_view(reinterpret_cast<const char *>(buf), n));
    ESP_LOGI(TAG, "开机音效已播放（%u 字节）", (unsigned)n);
    return true;
}

static bool pet_play_ogg_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > PET_BOOT_OGG_MAX) {
        return false;
    }
    uint8_t *buf = (uint8_t *)heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t *)malloc(st.st_size);
    if (!buf) {
        ESP_LOGW(TAG, "开机音效内存不足（%ld 字节）", (long)st.st_size);
        return false;
    }
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(buf, 1, st.st_size, f) : 0;
    if (f) fclose(f);
    /* 上次写一半的中断文件会留下一堆杂音：认 OggS 头 + 最小体积（3 秒 Opus 约 54KB，
       就算 1 秒也在 12KB 以上；4KB 的必然是残包） */
    if (n < 12288 || memcmp(buf, "OggS", 4) != 0) {
        ESP_LOGW(TAG, "开机音效缓存不完整（%u 字节），丢弃", (unsigned)n);
        heap_caps_free(buf);
        remove(path);
        return false;
    }
    bool ok = pet_play_ogg_mem(buf, n);
    heap_caps_free(buf);
    return ok;
}

static void pet_boot_sound_task(void *arg) {
    /* 音频服务是 Application::Initialize() 里才 Initialize 的，比板级构造晚。
     * 状态机离开 kDeviceStateStarting 就说明 Initialize 已经走过 audio_service_.Start()，
     * 这时候调 PlaySound 才不会踩到空 codec_。 */
    int guard = 0;
    while (Application::GetInstance().GetDeviceState() == kDeviceStateStarting && guard++ < 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    bool played = pet_play_ogg_file(PET_BOOT_OGG_PATH);
    ESP_LOGI(TAG, "开机音效: %s", played ? "本地缓存已播" : "无缓存，等联网后现拉");

    /* 不直接等状态机到 idle——那要等激活流程走完（实测 10 秒开外），
     * 音效拖到那时早就跟开机画面脱节了。这里改成失败就重试，
     * WiFi 一拿到 IP 就能把音轨拉下来响出来（激活过程中连不上很正常，重试即可）。 */
    uint8_t *buf = nullptr;
    size_t len = 0;
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 40; i++) {
        err = pet_anim_fetch_mem(PET_BOOT_OGG_URL, PET_BOOT_OGG_MAX, &buf, &len);
        if (err == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    if (err == ESP_OK) {
        if (!played) pet_play_ogg_mem(buf, len);
        /* 只在没有缓存时落盘：SPIFFS 写 ~5KB/s，54KB 要写十来秒，
         * 每次开机都重写没必要，写失败也只是少了离线开机音效。 */
        struct stat st;
        if (stat(PET_BOOT_OGG_PATH, &st) != 0 || st.st_size != (long)len) {
            pet_anim_save_file(PET_BOOT_OGG_PATH, buf, len);
        }
        heap_caps_free(buf);
    } else {
        ESP_LOGW(TAG, "开机音效下载失败: %s（沿用旧缓存）", esp_err_to_name(err));
    }
    vTaskDelete(nullptr);
}

static void pet_pull_task(void *arg) {
    // 常驻服务循环。频率刻意压低：每多发一次 HTTP，WiFi 射频就要拉一次峰值电流，
    // 这块板之前就吃过 USB 供电不足反复重启的亏（AMOLED + WiFi 同时峰值顶不住）。
    //   每 5s：上报状态 + 取远程指令（几千字节，很轻）
    //   每 30s：比一次资源包 sha（一致就直接返回，不下载、不闪屏）
    ESP_LOGI(TAG, "pet_pull 任务启动");
    while ((int)Application::GetInstance().GetDeviceState() < (int)kDeviceStateIdle) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "网络就绪，开始拉包 / 上报");

    int tick = 0;
    uint32_t last_frames = 0;
    int stall = 0;
    for (;;) {
        pet_anim_set_device_state((int)Application::GetInstance().GetDeviceState());
        pet_anim_report(PET_REPORT_URL);
        pet_anim_fetch_cmd(PET_CMD_URL);

        /* 画面卡死兜底：送屏是在 spi_device_get_trans_result(portMAX_DELAY) 上
         * 无限等待，一旦 DMA 完成中断丢了，任何软件手段都拉不回来（事务池状态
         * 已经乱了）。15 秒一帧没送出就重启——比让用户对着冻屏强。 */
        uint32_t f = pet_frames_sent();
        if (f == last_frames) {
            if (++stall >= 3 && pet_anim_is_running()) {
                ESP_LOGE(TAG, "画面 15 秒无进展（phase=%d strip=%d lockfail=%u），重启恢复",
                         (int)pet_dbg_phase, (int)pet_dbg_strip, (unsigned)pet_dbg_lock_fail);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        } else {
            last_frames = f;
            stall = 0;
        }

        if (tick % 6 == 0) {
            ESP_LOGI(TAG, "pull: begin (frames=%u)", (unsigned)pet_frames_sent());

            struct stat st0;
            bool have_local = (stat(PET_PACK_PATH, &st0) == 0 && st0.st_size > 0);
            esp_err_t perr = ESP_FAIL;

            if (have_local) {
                /* 本地已有包：走原来的 sha 比对 + 热换包流程 */
                perr = pet_anim_pull(PET_PACK_URL, PET_PACK_PATH);
            } else {
                /* 本地没包：直接下到 PSRAM 起播。
                 * 不能「先落盘再加载」——这块板 SPIFFS 写入实测只有 5KB/s，
                 * 2MB 的包要写四分钟，这四分钟屏幕就只能停在旧界面（用户看到的
                 * 就是「刷成收银屏固件了」）。内存里加载是毫秒级的，先让画面动起来。
                 * 代价是每次开机重下一遍（局域网 2MB 约 1 秒），换宠物也更干脆。 */
                uint8_t *pack = nullptr;
                size_t plen = 0;
                perr = pet_anim_fetch_mem(PET_PACK_URL, 6 * 1024 * 1024, &pack, &plen);
                if (perr == ESP_OK) {
                    if (pet_anim_load_pack_mem(pack, plen) == ESP_OK) {
                        if (s_pack_mem) heap_caps_free(s_pack_mem);
                        s_pack_mem = pack;   /* pet_anim 只存指针，缓冲要自己保活 */
                        pet_anim_start();
                    } else {
                        heap_caps_free(pack);
                        perr = ESP_FAIL;
                    }
                }
            }

            ESP_LOGI(TAG, "pull: done err=%s running=%d", esp_err_to_name(perr),
                     (int)pet_anim_is_running());
            if (perr == ESP_OK && !pet_anim_is_running() && have_local) {
                if (pet_anim_load_pack_file(PET_PACK_PATH) == ESP_OK) pet_anim_start();
            }
            if (pet_anim_is_running() && !s_lvgl_stopped) {
                s_lvgl_stopped = true;
                lvgl_port_stop();
                ESP_LOGI(TAG, "宠物包就绪，动画播放中，LVGL 渲染已停止");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
        tick++;
    }
    vTaskDelete(nullptr);
}


static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    {0x11, (uint8_t[]){0x00}, 0, 600}, // Sleep out

    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},

    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x36, (uint8_t[]){0xA0}, 1, 0},
    {0x29, (uint8_t[]){0x00}, 0, 600},
};

// 在waveshare_amoled_1_75类之前添加新的显示类
class CustomLcdDisplay : public SpiLcdDisplay {
public:
    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = (lv_area_t* )lv_event_get_param(e);
        uint16_t x1 = area->x1;
        uint16_t x2 = area->x2;

        uint16_t y1 = area->y1;
        uint16_t y2 = area->y2;

        // round the start of coordinate down to the nearest 2M number
        area->x1 = (x1 >> 1) << 1;
        area->y1 = (y1 >> 1) << 1;
        // round the end of coordinate up to the nearest 2N+1 number
        area->x2 = ((x2 >> 1) << 1) + 1;
        area->y2 = ((y2 >> 1) << 1) + 1;
    }

    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                     esp_lcd_panel_handle_t panel_handle,
                     int width,
                     int height,
                     int offset_x,
                     int offset_y,
                     bool mirror_x,
                     bool mirror_y,
                     bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle,
                        width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
        // Note: UI customization should be done in SetupUI(), not in constructor
        // to ensure lvgl objects are created before accessing them
    }

    virtual void SetupUI() override {
        // Call parent SetupUI() first to create all lvgl objects
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES*  0.1, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES*  0.1, 0);
        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }

    // 情绪联动：小智所有表情（SetEmotion）同步喂给宠物动画
    virtual void SetEmotion(const char* emotion) override {
        pet_anim_set_emotion_name(emotion);
        SpiLcdDisplay::SetEmotion(emotion);
    }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(esp_lcd_panel_io_handle_t panel_io) : Backlight(), panel_io_(panel_io) {}

protected:
    esp_lcd_panel_io_handle_t panel_io_;

    virtual void SetBrightnessImpl(uint8_t brightness) override {
        auto display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        uint8_t data[1] = {((uint8_t)((255*  brightness) / 100))};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, &data, sizeof(data));
    }
};

class WaveshareEsp32s3TouchAMOLED2inch16 : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_ = nullptr;
    Button boot_button_;
    CustomLcdDisplay* display_;
    CustomBacklight* backlight_;
    esp_io_expander_handle_t io_expander = NULL;
    PowerSaveTimer* power_save_timer_;

    void InitializePowerSaveTimer() {
        /* 车机场景不要省电睡眠，也不要自动关机：
         *   - 原来 60 秒无操作会进省电模式 → SetPowerSaveMode(true) 会
         *     SetChatMessage("system","") 往对话区写一条空消息，LVGL 就在
         *     动画正中间画出一块半透明灰底框（就是那「一套灰色横杠」）；
         *   - 原来 300 秒会直接 pmic_->PowerOff() 关机，骑车途中会突然黑屏。
         * 两个都关掉（-1 = 不触发），屏幕常亮、设备常开。 */
        power_save_timer_ = new PowerSaveTimer(-1, -1, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(20); });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness(); });
        power_save_timer_->OnShutdownRequest([this](){ 
            pmic_->PowerOff(); });
        // 不再 SetEnabled(true)：省电/关机定时器整体停用
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        /* I2C 总线解锁：AXP2101 / 触摸 / IMU 是独立供电的从机，主控单独复位时
         * 它们可能还卡在上一次传输里把 SDA 拉低，此时新起的总线一读就永远等不到应答，
         * 表现为「Init AXP2101 之后日志就停住」或触摸初始化卡死。
         * i2c_master_bus_reset 会补发时钟脉冲把从机放掉，这一步比拔电重插省事。 */
        esp_err_t bus_rst = i2c_master_bus_reset(i2c_bus_);
        ESP_LOGW(TAG, "I2C 总线解锁: %s", esp_err_to_name(bus_rst));
    }

    void InitializeTca9554(void) {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, I2C_ADDRESS, &io_expander);
        if (ret != ESP_OK)
            ESP_LOGE(TAG, "TCA9554 create returned error");
        ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_4, IO_EXPANDER_INPUT);
        ESP_ERROR_CHECK(ret);
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
        buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
        buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
        buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
        buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH*  DISPLAY_HEIGHT*  sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        /* 40 → 80MHz：CO5300 支持 80MHz QSPI，实测带宽只有理论值的十分之一，
         * 先把这个最直接的旋钮拧到底（如果花屏说明 IO 能力不足，退回 60/40）。 */
        /* 80MHz 实测会随机挂死 SPI 事务（strip 冻在半路 5 秒~永久），
           CO5300 手册虽标支持但信号上撑不住，稳定优先回 40MHz。 */
        io_config.pclk_hz = 40 * 1000 * 1000;
        /* 原来 10：一次 draw_bitmap 会连发 10 个事务，而我们只有 2 块条带缓冲，
         * 排在后面的 DMA 读到的是已经被覆盖的数据（花屏），事务池也更容易堵死。
         * 降到 2 与缓冲数量对齐，tx_color 里会同步等上一个事务回收。 */
        io_config.trans_queue_depth = 2;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const co5300_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            }};

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void* )&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        s_pet_panel = panel;  // 宠物动画送屏用
        display_ = new CustomLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new CustomBacklight(panel_io);
        backlight_->RestoreBrightness();
    }

    void InitializeTouch() {
        /* 先探一下触摸在不在：esp_lcd_panel_io_i2c 在从机不应答时是「一直等事件」
         * 而不是超时返回，触摸一挂就把整个开机流程永久卡死在这里（日志停在
         * “Initialize touch controller”后再无输出）。probe 有明确超时，探不到就跳过，
         * 让宠物动画、语音、网络照常起——触摸坏了最多点不动，不该整机白屏。 */
        esp_err_t probe = i2c_master_probe(i2c_bus_, ESP_LCD_TOUCH_IO_I2C_CST9217_ADDRESS, 200);
        if (probe != ESP_OK) {
            ESP_LOGE(TAG, "触摸不应答（%s），跳过触摸初始化；断电重插可恢复总线",
                     esp_err_to_name(probe));
            return;
        }

        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = PIN_NUM_TOUCH_RST,
            .int_gpio_num = PIN_NUM_TOUCH_INT,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 1,
                .mirror_y = 1,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
        tp_io_config.scl_speed_hz = 400*  1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst9217(tp_io_handle, &tp_cfg, &tp));
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }

    // 宠物动画：分区挂载 → 加载本地包 → 起播 → 后台拉最新包
    void InitializePetAnim() {
        esp_vfs_spiffs_conf_t spiffs_cfg = {
            .base_path = "/petfs",
            .partition_label = "pets",
            .max_files = 2,
            .format_if_mount_failed = true,
        };
        esp_err_t ret = esp_vfs_spiffs_register(&spiffs_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "pets 分区挂载失败: %s", esp_err_to_name(ret));
            return;
        }
        {
            size_t total = 0, used = 0;
            esp_spiffs_info("pets", &total, &used);
            ESP_LOGI(TAG, "pets 分区: 总 %u KB / 已用 %u KB", total / 1024, used / 1024);
            struct stat st;
            if (stat(PET_PACK_PATH, &st) == 0) {
                ESP_LOGI(TAG, "本地包存在: %s (%ld 字节)", PET_PACK_PATH, st.st_size);
            } else {
                ESP_LOGW(TAG, "本地包不存在: %s", PET_PACK_PATH);
            }
        }

        pet_anim_config_t cfg = {};
        cfg.flush = pet_flush_cb;
        cfg.imu_sda = AUDIO_CODEC_I2C_SDA_PIN;  // 复用编解码 I2C 总线上的 QMI8658
        cfg.imu_scl = AUDIO_CODEC_I2C_SCL_PIN;
        cfg.imu_addr = 0x6B;
        cfg.i2c_bus = i2c_bus_;
        esp_err_t err = pet_anim_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "pet_anim 初始化失败: %s", esp_err_to_name(err));
            return;
        }

        s_pet_mux = xSemaphoreCreateMutex();

        /* 复位原因：判断「一开一关就退出」到底是掉电(brownout)、看门狗、还是崩溃 */
        esp_reset_reason_t rr = esp_reset_reason();
        const char *rr_name = "?";
        switch (rr) {
            case ESP_RST_POWERON:  rr_name = "上电"; break;
            case ESP_RST_EXT:      rr_name = "外部复位脚"; break;
            case ESP_RST_SW:       rr_name = "软件重启"; break;
            case ESP_RST_PANIC:    rr_name = "异常崩溃(panic)"; break;
            case ESP_RST_INT_WDT:  rr_name = "中断看门狗"; break;
            case ESP_RST_TASK_WDT: rr_name = "任务看门狗"; break;
            case ESP_RST_WDT:      rr_name = "其他看门狗"; break;
            case ESP_RST_DEEPSLEEP:rr_name = "深睡唤醒"; break;
            case ESP_RST_BROWNOUT: rr_name = "电压跌落(brownout)"; break;
            case ESP_RST_USB:      rr_name = "USB 复位"; break;
            case ESP_RST_JTAG:     rr_name = "JTAG 复位"; break;
            case ESP_RST_CPU_LOCKUP: rr_name = "CPU 死锁"; break;
            default: break;
        }
        ESP_LOGW(TAG, "上次复位原因: %d (%s)", (int)rr, rr_name);

        esp_err_t load_err = pet_anim_load_pack_file(PET_PACK_PATH);
        if (load_err == ESP_OK) {
            ESP_LOGI(TAG, "本地宠物包已加载");
        } else {
            ESP_LOGE(TAG, "本地宠物包加载失败: %s", esp_err_to_name(load_err));
        }

        // 宠物动画独占屏幕：先停 LVGL，再起播。
        // 反过来的话（先起播再停 LVGL），开头这一两秒 LVGL 还在刷界面，
        // 与宠物帧抢同一条 QSPI 总线，画面就是一堆横条。
        // 注意：只有本地包加载成功才停——否则屏幕会全白，什么都不显示。
        if (load_err == ESP_OK) {
            s_lvgl_stopped = true;
            lvgl_port_stop();
            vTaskDelay(pdMS_TO_TICKS(150));   // 等 LVGL 最后一次刷新落地
            /* 把小智 UI 的所有控件隐藏（不删除，避免应用层拿到野指针）。
             * 注意 lv_screen_active() 可能不是 UI 所在的屏幕（实测返回 0 个子控件），
             * 所以要遍历 display 的所有 screen 和它们的子控件。 */
            if (lvgl_port_lock(1000)) {
                int hidden = 0;
                for (lv_display_t *dp = lv_display_get_next(NULL); dp;
                     dp = lv_display_get_next(dp)) {
                    /* 摘掉刷新回调：LVGL 后续的脏区刷新不再写 QSPI */
                    lv_display_set_flush_cb(dp, pet_noop_flush);
                    lv_obj_t *scr = lv_display_get_screen_active(dp);
                    if (!scr) continue;
                    uint32_t n = lv_obj_get_child_count(scr);
                    ESP_LOGI(TAG, "display %p 活动屏幕有 %u 个子控件", (void *)dp, (unsigned)n);
                    for (uint32_t i = 0; i < n; i++) {
                        lv_obj_add_flag(lv_obj_get_child(scr, i), LV_OBJ_FLAG_HIDDEN);
                        hidden++;
                    }
                }
                ESP_LOGI(TAG, "LVGL 控件已隐藏（%d 个），屏幕交给宠物动画", hidden);
                lvgl_port_unlock();
            }
            ESP_LOGI(TAG, "LVGL 渲染已停止，宠物动画独占屏幕");

            /* 开机第一屏直接就是卡皮巴拉待机动画——pet_anim_start() 起的播放任务
             * 第一帧就是 idle 段第 0 帧，中间不会出现黑屏或自检图样。
             * （之前诊断用的红/条纹/蓝三连自检已删除：每次重启都闪一遍太难看。） */
        } else {
            ESP_LOGW(TAG, "本地包未加载，保留 LVGL 界面，等 pet_pull 拉到包后再切");
        }

        pet_anim_start();
        ESP_LOGI(TAG, "free heap=%u psram=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        xTaskCreate(pet_pull_task, "pet_pull", 10240, nullptr, 3, nullptr);
        // 开机音效独立任务：它要等音频服务就绪，不能在这里同步阻塞
        xTaskCreate(pet_boot_sound_task, "pet_boot_ogg", 8192, nullptr, 2, nullptr);
    }

    // 初始化工具
    void InitializeTools() {        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                EnterWifiConfigMode();
                return true;
            });
    }

public:
    WaveshareEsp32s3TouchAMOLED2inch16() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeCodecI2c();
#if CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_1_75
        InitializeTca9554();
#endif
        InitializeAxp2101();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializeButtons();
        InitializeTools();
        InitializePetAnim();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        return backlight_;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging)
        {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(WaveshareEsp32s3TouchAMOLED2inch16);
