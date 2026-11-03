/*
 * pet_anim —— 宠物动画播放 + 六轴运动状态机
 *
 * 设计原则：不依赖具体 LCD 驱动与小智内部实现。
 * 画面通过 flush 回调送出去，IMU 引脚通过配置传入，方便接到任何板子。
 *
 * 典型用法：
 *   1. pet_anim_init(&cfg)         配置 flush 回调与 IMU 引脚
 *   2. pet_anim_load_pack_file()   加载 .pack 资源包
 *   3. pet_anim_start()            启动播放任务
 *   4. pet_anim_set_talking(true)  与小智的 TTS 播放状态联动
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PET_ANIM_MAX_ANIMS 8
#define PET_ANIM_NAME_LEN 12

/* 运动状态：由 IMU 自动推断，也可手动设置 */
typedef enum {
    PET_MOTION_IDLE = 0,   /* 静止 / 待机 */
    PET_MOTION_RUNNING,    /* 骑行中 */
    PET_MOTION_BRAKING,    /* 刹车（一次性，播完回 IDLE） */
    PET_MOTION_FALLEN,     /* 摔倒（一次性，最高优先级） */
    PET_MOTION_COUNT
} pet_motion_t;

/* 送屏回调：把解码好的 RGB565 缓冲推到 LCD。返回 ESP_OK 表示成功。 */
typedef esp_err_t (*pet_flush_fn)(const uint8_t *rgb565, uint16_t w, uint16_t h);

typedef struct {
    pet_flush_fn flush;      /* 必需 */

    uint16_t width;          /* 0 = 使用资源包里的尺寸 */
    uint16_t height;

    int imu_sda;             /* -1 表示不启用 IMU（仅手动切换） */
    int imu_scl;
    uint8_t imu_addr;        /* QMI8658 常见 0x6A / 0x6B，按原理图核对 */
    void *i2c_bus;           /* 已有 i2c_master_bus_handle_t；非 NULL 时复用，忽略 sda/scl */

    uint8_t task_priority;   /* 播放任务优先级，默认 4 */
    uint32_t task_stack;     /* 默认 8192 */
} pet_anim_config_t;

esp_err_t pet_anim_init(const pet_anim_config_t *cfg);

/* 资源包加载：文件路径（SPIFFS/LittleFS/SD 均可）或内存指针 */
esp_err_t pet_anim_load_pack_file(const char *path);
esp_err_t pet_anim_load_pack_mem(const uint8_t *data, size_t len);

esp_err_t pet_anim_start(void);
void pet_anim_stop(void);
bool pet_anim_is_running(void);

/* 联调诊断：把解码缓冲填成测试图样并送屏，持续 ms 毫秒。
 * 走与正常播放完全相同的 flush 路径——图样上有没有线条/错位，
 * 一眼区分「送屏链路坏」还是「解码内容坏」。需在 start 之前调用。
 * mode: 0=纯色(color 参数)  1=64px 竖条纹 R/G/B 循环（测横向错位） */
esp_err_t pet_anim_show_test(uint32_t ms, int mode, uint16_t color);

/* 停播并等播放任务真正退出（它每帧检查 running，最多等 timeout_ms）。
 * 换宠物包 / 释放解码缓冲之前必须先调这个，否则播放任务还在读旧内存 → 崩溃重启。
 * 返回 true 表示任务已退出，可以安全释放资源。 */
bool pet_anim_wait_stopped(uint32_t timeout_ms);

/* 远程拉取资源包（板子连上 WiFi 后调用）：
 *   pet_anim_pull("http://192.168.1.10:8080/api/pet/latest.pack", "/spiffs/pet.pack");
 * 下载完自动加载；若正在播放，会先停播再重启。返回 ESP_OK 表示已换成新宠物。 */
esp_err_t pet_anim_pull(const char *url, const char *save_path);

/* 状态上报（联调观测）：POST 当前播放/运动/说话状态 JSON 到服务器。
 *   pet_anim_report("http://192.168.1.10:8080/api/pet/report");
 * 失败静默返回，不影响播放。 */
esp_err_t pet_anim_report(const char *report_url);

/* 远程指令：拉取 {"motion":1,"talking":true} 形式的命令并立即生效。
 * motion: -1=恢复自动, 0=idle, 1=run, 2=brake, 3=fall；talking: true/false。
 * 只做联调手动触发，不影响正常自动逻辑。 */
esp_err_t pet_anim_fetch_cmd(const char *cmd_url);

/* 通用小文件下载（目前用于开机音效）。
 * fetch_mem: 直接下载进 PSRAM，返回的缓冲由调用方 heap_caps_free()。
 * save_file: 把内存块落盘做缓存（失败不影响播放）。
 * max_bytes=0 时默认 512KB。 */
esp_err_t pet_anim_fetch_mem(const char *url, size_t max_bytes, uint8_t **out, size_t *out_len);
esp_err_t pet_anim_save_file(const char *path, const uint8_t *data, size_t len);

/* 手动切状态（无 IMU 时用，或用于调试） */
void pet_anim_set_motion(pet_motion_t m);
pet_motion_t pet_anim_get_motion(void);

/* 远程调试：强制运动状态（压制六轴状态机），-1 恢复自动检测。
 * 用于把「画面能否切换」与「IMU 检测是否正常」分开验证。 */
esp_err_t pet_anim_force_motion(int m);

/* 语音联动：TTS 开始播放时置 true，播完置 false */
void pet_anim_set_talking(bool talking);

/* 联调辅助：把宿主设备的状态机值（listening/speaking 等）带进状态上报，
 * 纯观测用，不影响播放逻辑。 */
void pet_anim_set_device_state(int state);

/* ---------------- 情绪通道（对话内容 → 画面） ----------------
 * 小智每次调用 display->SetEmotion("happy") 时，把同一句字符串喂进来，
 * 画面就会在 hold 时长内切到对应的段，之后自动回到运动/待机状态。
 * 资源包里没有对应的段时会自动退回 talk/idle，不会黑屏。
 */
typedef enum {
    PET_EMO_NONE = 0,
    PET_EMO_NEUTRAL,
    PET_EMO_HAPPY,
    PET_EMO_SAD,
    PET_EMO_ANGRY,
    PET_EMO_SURPRISED,
    PET_EMO_THINKING,
    PET_EMO_COUNT
} pet_emotion_t;

void pet_anim_set_emotion(pet_emotion_t e);
void pet_anim_set_emotion_name(const char *name);   /* 直接喂小智的英文字符串 */
pet_emotion_t pet_anim_get_emotion(void);
void pet_anim_clear_emotion(void);

/* 状态快照：日志打印 or HTTP /api/status 回读，联调时看这一段就够 */
typedef struct {
    const char *anim;        /* 当前正在播的段名 */
    pet_motion_t motion;     /* 六轴推断的运动状态 */
    pet_emotion_t emotion;
    bool talking;
    int device_state;        /* 宿主状态机快照（观测用） */
    float sigma;             /* 抖动强度（g），判断 IMU 是否工作看这个 */
    int frame;
    int frame_count;
    uint32_t frames;         /* 累计已送出的画面帧数：涨不动说明播放任务卡了 */
    int phase;               /* 播放任务当前阶段：0=调度 1=解码 2=送屏 3=完成 */
    int strip;               /* 送屏进行到的条带 y（-1 表示没在送屏） */
} pet_anim_status_t;

void pet_anim_get_status(pet_anim_status_t *out);

/* 联调诊断变量（定义在 pet_anim.c）。板级 flush 往里写条带进度，
 * 播放卡死时回读 status 就能判断卡在解码还是送屏。 */
extern volatile uint8_t pet_dbg_phase;
extern volatile int32_t pet_dbg_strip;
extern volatile uint32_t pet_dbg_lock_fail;
extern volatile uint32_t pet_dbg_decode_us;   /* 单帧 JPEG 解码耗时（滑动平均） */
extern volatile uint32_t pet_dbg_flush_us;    /* 单帧送屏耗时（滑动平均） */
/* 送屏再拆两段，好区分是「CPU 拷内存慢」还是「SPI 真发得慢」：
 * copy_us 是 RGB 逐条带搬进内部 SRAM 的累计，draw_us 是 draw_bitmap 的累计。 */
extern volatile uint32_t pet_dbg_copy_us;
extern volatile uint32_t pet_dbg_draw_us;

/* IMU 是否已完成静止标定 */
bool pet_imu_is_calibrated(void);

/* 静止标定：让车停稳，调用后阻塞 ms 毫秒采样重力零位。首次开机必须做一次。 */
esp_err_t pet_imu_calibrate(uint32_t ms);

/* 调试用：最近一次计算的抖动强度（单位 g） */
float pet_imu_last_sigma(void);

#ifdef __cplusplus
}
#endif
