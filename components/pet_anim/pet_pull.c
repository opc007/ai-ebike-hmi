/*
 * 资源包远程拉取
 *
 * 场景：网页端生成好新宠物后，板子不用连线、不用重新烧录，
 *       开机连上 WiFi 直接从服务器把「当前宠物」拉下来。
 *
 * 流程：
 *   1. GET <pack_url 去掉 .pack> 拿 JSON 元信息（含 sha256）
 *   2. 本地包 sha256 一致 → 直接返回，不下载（SPIFFS 空间有限，
 *      装不下「旧包+新包」两份，也省流量）
 *   3. 不一致 → 先删旧包（播放中的包在 PSRAM 里，删文件不影响画面），
 *      下载到 .tmp，完整后再 rename
 *
 * 只在测试和小批量交付阶段用。量产应该走分区 OTA。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pet_anim.h"

static const char *TAG = "pet_pull";

/* ---------------- 独立 SHA-256（IDF6 的 mbedtls 头文件移进 PSA 私有目录，不折腾依赖） ---------------- */
typedef struct { uint32_t h[8]; uint64_t len; unsigned char buf[64]; size_t buflen; } mini_sha256_t;

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(mini_sha256_t *s, const unsigned char *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i-15],7) ^ ror(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ror(w[i-2],17) ^ ror(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror(e,6) ^ ror(e,11) ^ ror(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ror(a,2) ^ ror(a,13) ^ ror(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + mj;
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}

static void sha256_init(mini_sha256_t *s) {
    static const uint32_t H0[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(s->h, H0, sizeof(H0));
    s->len = 0; s->buflen = 0;
}

static void sha256_update(mini_sha256_t *s, const unsigned char *p, size_t n) {
    s->len += n;
    while (n > 0) {
        size_t take = 64 - s->buflen; if (take > n) take = n;
        memcpy(s->buf + s->buflen, p, take);
        s->buflen += take; p += take; n -= take;
        if (s->buflen == 64) { sha256_block(s, s->buf); s->buflen = 0; }
    }
}

static void sha256_final(mini_sha256_t *s, unsigned char out[32]) {
    uint64_t bits = s->len * 8;
    unsigned char pad = 0x80;
    sha256_update(s, &pad, 1);
    unsigned char z = 0;
    while (s->buflen != 56) sha256_update(s, &z, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - i * 8));
    /* 直接填尾块，不走 update（update 会改 len，但此时已无所谓） */
    memcpy(s->buf + 56, lenb, 8);
    sha256_block(s, s->buf);
    s->buflen = 0;
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (unsigned char)(s->h[i] >> 24);
        out[i*4+1] = (unsigned char)(s->h[i] >> 16);
        out[i*4+2] = (unsigned char)(s->h[i] >> 8);
        out[i*4+3] = (unsigned char)(s->h[i]);
    }
}

/* 单包上限：PSRAM 8MB，留出运行内存后 6MB 是上限，正常包在 3MB 内 */
#define PULL_MAX_BYTES (6 * 1024 * 1024)

/* 计算文件 sha256，输出 64 位小写 hex + '\0'。文件不存在返回 false。 */
static bool file_sha256(const char *path, char out_hex[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    mini_sha256_t ctx;
    sha256_init(&ctx);
    unsigned char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(4096);
    if (!buf) { fclose(f); return false; }
    size_t n;
    while ((n = fread(buf, 1, 4096, f)) > 0) sha256_update(&ctx, buf, n);
    heap_caps_free(buf);
    unsigned char digest[32];
    sha256_final(&ctx, digest);
    fclose(f);
    for (int i = 0; i < 32; i++) sprintf(out_hex + i * 2, "%02x", digest[i]);
    return true;
}

/* 拉元信息 JSON，从里面抠出 "sha256":"..."（64 hex）。
 * meta_url = pack_url 去掉结尾 ".pack"。失败时 expect 置空、返回 false。 */
static bool fetch_meta_sha(const char *pack_url, char *meta_url, size_t meta_sz, char expect[65]) {
    expect[0] = '\0';
    size_t len = strlen(pack_url);
    if (len > 5 && strcmp(pack_url + len - 5, ".pack") == 0) {
        snprintf(meta_url, meta_sz, "%.*s", (int)(len - 5), pack_url);
    } else {
        snprintf(meta_url, meta_sz, "%s", pack_url);
    }

    esp_http_client_config_t cfg = {
        .url = meta_url,
        .timeout_ms = 8000,
        .buffer_size = 2048,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;
    if (esp_http_client_open(cli, 0) != ESP_OK) {
        esp_http_client_cleanup(cli);
        return false;
    }
    esp_http_client_fetch_headers(cli);
    char body[512] = {0};
    int got = 0;
    while (got < (int)sizeof(body) - 1) {
        int r = esp_http_client_read(cli, body + got, sizeof(body) - 1 - got);
        if (r <= 0) break;
        got += r;
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    if (got <= 0) return false;
    body[got] = '\0';

    const char *key = strstr(body, "\"sha256\"");
    if (!key) return false;
    const char *colon = strchr(key, ':');
    if (!colon) return false;
    const char *q1 = strchr(colon, '"');
    if (!q1) return false;
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2 || q2 - q1 - 1 != 64) return false;
    memcpy(expect, q1 + 1, 64);
    expect[64] = '\0';
    return true;
}

esp_err_t pet_anim_pull(const char *url, const char *save_path) {
    if (!url || !save_path || !url[0] || !save_path[0]) return ESP_ERR_INVALID_ARG;

    /* 1. 先问服务器要 sha256，和本地比 */
    char meta_url[256];
    char expect[65];
    if (fetch_meta_sha(url, meta_url, sizeof(meta_url), expect)) {
        char local[65];
        struct stat st;
        if (stat(save_path, &st) == 0 && file_sha256(save_path, local)) {
            if (strcmp(local, expect) == 0) {
                ESP_LOGI(TAG, "本地包已是最新（%ld 字节），跳过下载", (long)st.st_size);
                return ESP_OK;
            }
            ESP_LOGI(TAG, "服务器有新包，本地包过期，先删旧包腾空间");
            /* 旧包已整包载入 PSRAM，删文件不影响正在播的画面 */
            remove(save_path);
        } else {
            ESP_LOGI(TAG, "服务器 sha=%s，本地无包，开始下载", expect);
        }
    } else {
        ESP_LOGW(TAG, "拿不到元信息（%s），直接尝试下载", meta_url);
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 20000,
        .buffer_size = 2048,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        ESP_LOGE(TAG, "http client 初始化失败");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "连接失败 %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        return err;
    }

    int64_t total_len = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d，服务器上没有可用资源包", status);
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_ERR_NOT_FOUND;
    }
    if (total_len <= 0 || total_len > PULL_MAX_BYTES) {
        ESP_LOGE(TAG, "包大小异常：%lld 字节", (long long)total_len);
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "开始下载 %.2f MB", total_len / 1048576.0);

    /* 先写 .tmp，校验完整后再 rename 覆盖。
       中途断网不会留下半包把旧宠物弄坏。 */
    char tmp_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", save_path);
    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "写不了 %s", tmp_path);
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_FAIL;
    }

    /* 4KB 一次，缓冲放 PSRAM，别占内部 RAM */
    const size_t CHUNK = 4096;
    char *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(CHUNK);
    if (!buf) {
        fclose(f);
        remove(tmp_path);
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_ERR_NO_MEM;
    }

    int64_t got = 0;
    while (got < total_len) {
        int r = esp_http_client_read(cli, buf, CHUNK);
        if (r <= 0) break;
        if (fwrite(buf, 1, r, f) != (size_t)r) {
            ESP_LOGE(TAG, "写入中断（存储空间不足？）");
            got = -1;
            break;
        }
        got += r;
    }
    heap_caps_free(buf);
    fclose(f);
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (got != total_len) {
        ESP_LOGE(TAG, "下载不完整：%lld / %lld", (long long)got, (long long)total_len);
        remove(tmp_path);
        return ESP_FAIL;
    }

    remove(save_path);              /* rename 不覆盖旧文件，先删 */
    if (rename(tmp_path, save_path) != 0) {
        ESP_LOGE(TAG, "改名失败");
        remove(tmp_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "下载完成，加载中…");

    /* 正在播就先停，并且必须等播放任务真的退出——它每帧还在读旧的 pack_buf，
       直接 free 会踩到野指针（表现为花屏/横条 + 立刻重启）。
       旧实现只 delay 300ms，偶尔还没退干净就换包，是重启的主因之一。 */
    bool resume = pet_anim_is_running();
    if (resume) {
        if (!pet_anim_wait_stopped(3000)) {
            ESP_LOGE(TAG, "播放任务 3 秒没停，放弃本次换包（保留旧宠物）");
            return ESP_ERR_TIMEOUT;
        }
    }
    esp_err_t load_err = pet_anim_load_pack_file(save_path);
    if (resume && load_err == ESP_OK) pet_anim_start();
    return load_err;
}

/* ---------------- 通用小文件下载（开机音效） ----------------
 * 下载进 PSRAM 直接返回，不经过文件系统——SPIFFS 写失败也不会影响「这次能不能响」。
 * 调用方负责 heap_caps_free(*out)。
 */
esp_err_t pet_anim_fetch_mem(const char *url, size_t max_bytes, uint8_t **out, size_t *out_len) {
    if (!url || !url[0] || !out || !out_len) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    *out_len = 0;
    if (max_bytes == 0) max_bytes = 512 * 1024;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
        .buffer_size = 2048,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(cli);
        return err;
    }
    int64_t total_len = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (status != 200 || total_len <= 0 || total_len > (int64_t)max_bytes) {
        ESP_LOGW(TAG, "%s 不可用（HTTP %d, %lld 字节）", url, status, (long long)total_len);
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *buf = heap_caps_malloc(total_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(total_len);
    if (!buf) {
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_ERR_NO_MEM;
    }

    int64_t got = 0;
    while (got < total_len) {
        int r = esp_http_client_read(cli, (char *)buf + got, total_len - got);
        if (r <= 0) break;
        got += r;
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (got != total_len) {
        ESP_LOGW(TAG, "下载不完整 %lld/%lld", (long long)got, (long long)total_len);
        heap_caps_free(buf);
        return ESP_FAIL;
    }
    *out = buf;
    *out_len = (size_t)got;
    ESP_LOGI(TAG, "已下载 %s（%lld 字节）", url, (long long)got);
    return ESP_OK;
}

/* 把内存块存成文件（缓存用，失败不影响播放）。返回 ESP_OK 表示已落盘。 */
esp_err_t pet_anim_save_file(const char *path, const uint8_t *data, size_t len) {
    if (!path || !data || len == 0) return ESP_ERR_INVALID_ARG;
    remove(path);   /* SPIFFS 覆盖写会因页面回收失败，先删干净 */
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "缓存写不了 %s（打不开）", path);
        return ESP_FAIL;
    }
    /* 分块写，缓冲放内部 RAM：一次大块从 PSRAM 直写 flash 容易被 SPIFFS 拒掉 */
    const size_t CHUNK = 2048;
    uint8_t *tmp = malloc(CHUNK);
    if (!tmp) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t off = 0;
    int failed_at = -1;
    int saved_errno = 0;
    while (off < len) {
        size_t n = (len - off < CHUNK) ? (len - off) : CHUNK;
        memcpy(tmp, data + off, n);
        size_t w = fwrite(tmp, 1, n, f);
        if (w != n) {
            failed_at = (int)off;
            saved_errno = errno;
            break;
        }
        off += n;
    }
    free(tmp);
    int cerr = fclose(f);
    if (cerr != 0 && saved_errno == 0) saved_errno = errno;
    if (failed_at >= 0) {
        ESP_LOGW(TAG, "缓存写入失败：已写 %d/%u 字节（errno=%d）", failed_at, (unsigned)len,
                 saved_errno);
        return ESP_FAIL;
    }
    if (cerr != 0) {
        ESP_LOGW(TAG, "缓存关闭失败（errno=%d）", saved_errno);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "缓存已保存 %s（%u 字节）", path, (unsigned)off);
    return ESP_OK;
}

/* ---------------- 状态上报（联调观测通道） ----------------
 * 把 pet_anim_get_status() 快照 POST 给服务器，管理员端/日志里能实时看到：
 *   - 当前播的段（idle/run/brake/talk）
 *   - 六轴推断的运动状态 + 抖动强度 sigma（判断 IMU 灵不灵）
 *   - talking 标志（判断 TTS 联动通不通）
 * 失败静默返回，不影响播放。
 */
esp_err_t pet_anim_report(const char *report_url) {
    if (!report_url || !report_url[0]) return ESP_ERR_INVALID_ARG;

    pet_anim_status_t st;
    pet_anim_get_status(&st);

    char json[384];
    snprintf(json, sizeof(json),
             "{\"anim\":\"%s\",\"motion\":%d,\"talking\":%s,"
             "\"emotion\":%d,\"state\":%d,\"sigma\":%.4f,"
             "\"frame\":%d,\"frame_count\":%d,\"frames\":%u,"
             "\"phase\":%d,\"strip\":%d,\"lockfail\":%u,"
             "\"dec_us\":%u,\"fl_us\":%u,\"copy_us\":%u,\"draw_us\":%u,"
             "\"running\":%s,\"free_psram\":%u}",
             st.anim ? st.anim : "-",
             (int)st.motion,
             st.talking ? "true" : "false",
             (int)st.emotion,
             st.device_state,
             st.sigma,
             st.frame, st.frame_count, (unsigned)st.frames,
             st.phase, st.strip, (unsigned)pet_dbg_lock_fail,
             (unsigned)pet_dbg_decode_us, (unsigned)pet_dbg_flush_us,
             (unsigned)pet_dbg_copy_us, (unsigned)pet_dbg_draw_us,
             pet_anim_is_running() ? "true" : "false",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_http_client_config_t cfg = {
        .url = report_url,
        .timeout_ms = 4000,
        .buffer_size = 1024,
        .method = HTTP_METHOD_POST,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_err_t err = esp_http_client_open(cli, strlen(json));
    if (err == ESP_OK) {
        esp_http_client_write(cli, json, strlen(json));
        esp_http_client_fetch_headers(cli);
        esp_http_client_close(cli);
    }
    esp_http_client_cleanup(cli);
    return err;
}

/* ---------------- 远程指令（联调手动触发） ----------------
 * GET 一个 {"motion":1,"talking":true} 形式的 JSON 并立即生效。
 * motion: -1 恢复自动 / 0 idle / 1 run / 2 brake / 3 fall
 * 解析故意做得很糙：在这个只有我们自己发的 JSON 里找 "motion" 和 "talking"。
 */
esp_err_t pet_anim_fetch_cmd(const char *cmd_url) {
    if (!cmd_url || !cmd_url[0]) return ESP_ERR_INVALID_ARG;

    esp_http_client_config_t cfg = {
        .url = cmd_url,
        .timeout_ms = 4000,
        .buffer_size = 512,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(cli);
        return err;   /* 服务器不可达就静默跳过 */
    }
    esp_http_client_fetch_headers(cli);
    char body[256] = {0};
    int got = 0, r;
    while (got < (int)sizeof(body) - 1 &&
           (r = esp_http_client_read(cli, body + got, sizeof(body) - 1 - got)) > 0) {
        got += r;
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    if (got <= 0) return ESP_FAIL;
    body[got] = '\0';

    static int last_motion = -2;
    static int last_talking = -1;

    const char *m = strstr(body, "\"motion\"");
    if (m) {
        const char *c = strchr(m, ':');
        if (c) {
            int v = atoi(c + 1);
            if (v != last_motion) {
                last_motion = v;
                pet_anim_force_motion(v);
                ESP_LOGW(TAG, "远程指令 motion=%d", v);
            }
        }
    }
    const char *t = strstr(body, "\"talking\"");
    if (t) {
        const char *v = strchr(t, ':');
        if (v) {
            const char *p = v + 1;
            while (*p == ' ') p++;
            int on = (strncmp(p, "true", 4) == 0) ? 1 : 0;
            if (on != last_talking) {
                last_talking = on;
                pet_anim_set_talking(on != 0);
                ESP_LOGW(TAG, "远程指令 talking=%d", on);
            }
        }
    }
    return ESP_OK;
}
