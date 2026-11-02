/*
 * QMI8658 六轴 + 运动状态机
 *
 * 核心思路（与常见做法不同）：判断「动没动」不看加速度大小，看抖动。
 * 静止时加速度恒等于重力（模长 1g），方差接近 0；
 * 骑行时路面震动让方差升到 0.05g 以上。用方差做判据，比用模长可靠得多。
 *
 * 刹车则用模长突变：减速时 |a| 会降到 0.85g 以下，且与安装方向无关。
 */

#include <math.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pet_anim.h"

static const char *TAG = "pet_imu";

/* ---------------- QMI8658 寄存器 ---------------- */
#define QMI_ADDR_DEFAULT 0x6A
#define QMI_WHO_AM_I 0x00
#define QMI_CTRL1 0x02
#define QMI_CTRL2 0x03
#define QMI_CTRL3 0x04
#define QMI_CTRL7 0x08
#define QMI_AX_L 0x35

#define ACC_LSB_PER_G (32768.0f / 4.0f) /* ±4g 量程 */

/* ---------------- 判据参数（需实测微调，集中放这里） ---------------- */
/* 采样率不能高：QMI8658 和 AXP2101 / 触摸 / 音频 codec 挂在同一条 I2C 总线上，
 * 50Hz 轮询会把别人的传输挤到超时（实测导致 Axp2101::IsCharging() 读超时 → 整机 abort 重启）。
 * 20Hz 足够：判据是 0.6 秒窗口内的方差，不是峰值，不需要高频。 */
#define WIN_SIZE 12              /* 12 点 @20Hz = 0.6 秒窗口 */
#define SAMPLE_HZ 20
#define SAMPLE_MS (1000 / SAMPLE_HZ)

#define SIGMA_ENTER 0.060f       /* 方差超过此值 → 判定为运动 */
#define SIGMA_EXIT 0.030f        /* 低于此值 → 判定为静止（滞回，防抖） */
#define BRAKE_MAG 0.820f         /* 模长低于此值视为减速 */
#define BRAKE_FRAMES 5           /* 连续 5 帧（250ms）才算刹车，避免误触 */
#define FALL_MAG 1.500f          /* 冲击峰值 */
#define FALL_ANGLE 45.0f         /* 姿态角偏离零位超过 45° */
#define FALL_HOLD_MS 800         /* 倾斜持续 800ms 才判定摔倒 */

/* 单次传输超时：20ms 快速失败。总线忙时立刻让出，
 * 不要用 100ms 死等——那会把后面的 AXP2101 / 触摸读一起拖死。 */
#define QMI_XFER_TIMEOUT_MS 20

typedef struct {
    float ax[WIN_SIZE], ay[WIN_SIZE], az[WIN_SIZE];
    int idx;
    int filled;
} window_t;

static struct {
    bool inited;
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    bool own_bus;

    window_t win;
    volatile pet_motion_t motion;
    volatile float sigma;

    /* 标定：重力零位与初始姿态角 */
    float gx, gy, gz;
    float pitch0, roll0;
    bool calibrated;

    volatile bool calibrating;
    TaskHandle_t task;
    volatile bool running;
    volatile int force_motion;   /* >=0 时压制状态机，强制输出该运动状态（远程调试用） */
} s_imu;

static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static esp_err_t qmi_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_imu.dev, buf, 2, QMI_XFER_TIMEOUT_MS);
}

static esp_err_t qmi_read_accel(float *ax, float *ay, float *az) {
    uint8_t reg = QMI_AX_L;
    uint8_t b[6];
    esp_err_t err = i2c_master_transmit_receive(s_imu.dev, &reg, 1, b, 6, QMI_XFER_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    *ax = (int16_t)rd16(b + 0) / ACC_LSB_PER_G;
    *ay = (int16_t)rd16(b + 2) / ACC_LSB_PER_G;
    *az = (int16_t)rd16(b + 4) / ACC_LSB_PER_G;
    return ESP_OK;
}

static void window_push(float ax, float ay, float az) {
    window_t *w = &s_imu.win;
    w->ax[w->idx] = ax;
    w->ay[w->idx] = ay;
    w->az[w->idx] = az;
    w->idx = (w->idx + 1) % WIN_SIZE;
    if (w->filled < WIN_SIZE) w->filled++;
}

/* 三轴方差之和的平方根，单位 g */
static float window_sigma(void) {
    window_t *w = &s_imu.win;
    int n = w->filled;
    if (n < 4) return 0.0f;

    float mx = 0, my = 0, mz = 0;
    for (int i = 0; i < n; i++) {
        mx += w->ax[i];
        my += w->ay[i];
        mz += w->az[i];
    }
    mx /= n;
    my /= n;
    mz /= n;

    float vx = 0, vy = 0, vz = 0;
    for (int i = 0; i < n; i++) {
        float dx = w->ax[i] - mx, dy = w->ay[i] - my, dz = w->az[i] - mz;
        vx += dx * dx;
        vy += dy * dy;
        vz += dz * dz;
    }
    return sqrtf(vx / n + vy / n + vz / n);
}

static inline float pitch_of(float ax, float ay, float az) {
    return atan2f(-ax, sqrtf(ay * ay + az * az)) * 57.2958f;
}
static inline float roll_of(float ay, float az) {
    return atan2f(ay, az) * 57.2958f;
}

/* ---------------- 采样任务 ---------------- */
static void imu_task(void *arg) {
    TickType_t last = xTaskGetTickCount();
    int brake_cnt = 0;
    int fall_cnt = 0;
    bool was_braking = false;

    while (s_imu.running) {
        float ax, ay, az;
        if (qmi_read_accel(&ax, &ay, &az) != ESP_OK) {
            vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_MS));
            continue;
        }
        window_push(ax, ay, az);

        float sigma = window_sigma();
        s_imu.sigma = sigma;
        float mag = sqrtf(ax * ax + ay * ay + az * az);

        if (!s_imu.calibrated) {
            /* 未标定：顺手累积零位，但不输出运动状态 */
            s_imu.gx = ax;
            s_imu.gy = ay;
            s_imu.gz = az;
            s_imu.pitch0 = pitch_of(ax, ay, az);
            s_imu.roll0 = roll_of(ay, az);
            vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_MS));
            continue;
        }

        pet_motion_t m = s_imu.motion;

        /* 远程调试：压制状态机，直接输出指定状态（-1 恢复自动） */
        if (s_imu.force_motion >= 0) {
            m = (pet_motion_t)s_imu.force_motion;
            if (m != s_imu.motion) {
                ESP_LOGW(TAG, "force motion %d -> %d", s_imu.motion, m);
                s_imu.motion = m;
            }
            vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_MS));
            continue;
        }

        /* 1) 摔倒：最高优先级，冲击或持续失衡 */
        float pitch = pitch_of(ax, ay, az);
        float roll = roll_of(ay, az);
        bool tilted = fabsf(pitch - s_imu.pitch0) > FALL_ANGLE ||
                      fabsf(roll - s_imu.roll0) > FALL_ANGLE;
        if ((mag > FALL_MAG && tilted) || tilted) {
            fall_cnt++;
            if (fall_cnt * SAMPLE_MS >= FALL_HOLD_MS) m = PET_MOTION_FALLEN;
        } else {
            fall_cnt = 0;
        }

        /* 2) 刹车：模长骤降，且当时确实在动 */
        if (m == PET_MOTION_RUNNING || m == PET_MOTION_BRAKING) {
            if (mag < BRAKE_MAG) {
                brake_cnt++;
                if (brake_cnt >= BRAKE_FRAMES) {
                    m = PET_MOTION_BRAKING;
                    was_braking = true;
                }
            } else {
                brake_cnt = 0;
                if (was_braking) {
                    was_braking = false;
                    if (m == PET_MOTION_BRAKING) m = PET_MOTION_RUNNING;
                }
            }
        } else {
            brake_cnt = 0;
        }

        /* 3) 骑行 / 静止：方差 + 滞回 */
        if (m != PET_MOTION_FALLEN && m != PET_MOTION_BRAKING) {
            if (m == PET_MOTION_IDLE && sigma > SIGMA_ENTER) {
                m = PET_MOTION_RUNNING;
            } else if (m == PET_MOTION_RUNNING && sigma < SIGMA_EXIT) {
                m = PET_MOTION_IDLE;
            }
        }

        if (m != s_imu.motion) {
            ESP_LOGI(TAG, "motion %d -> %d (sigma=%.3f mag=%.2f)", s_imu.motion, m, sigma, mag);
            s_imu.motion = m;
        }

        vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_MS));
    }
    s_imu.task = NULL;
    vTaskDelete(NULL);
}

/* ---------------- 对外接口 ---------------- */
esp_err_t pet_imu_init(int sda, int scl, uint8_t addr, void *bus_handle) {
    if (s_imu.inited) return ESP_OK;
    memset(&s_imu, 0, sizeof(s_imu));
    s_imu.force_motion = -1;
    if (addr == 0) addr = QMI_ADDR_DEFAULT;

    esp_err_t err;
    if (bus_handle) {
        s_imu.bus = (i2c_master_bus_handle_t)bus_handle;
        s_imu.own_bus = false;
    } else {
        if (sda < 0 || scl < 0) return ESP_ERR_INVALID_ARG;
        i2c_master_bus_config_t bc = {
            .i2c_port = -1,
            .sda_io_num = sda,
            .scl_io_num = scl,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        err = i2c_new_master_bus(&bc, &s_imu.bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c bus: %s", esp_err_to_name(err));
            return err;
        }
        s_imu.own_bus = true;
    }

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(s_imu.bus, &dc, &s_imu.dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t who = 0;
    uint8_t reg = QMI_WHO_AM_I;
    i2c_master_transmit_receive(s_imu.dev, &reg, 1, &who, 1, QMI_XFER_TIMEOUT_MS);
    ESP_LOGI(TAG, "WHO_AM_I=0x%02x (QMI8658 应为 0x05)", who);

    qmi_write(QMI_CTRL1, 0x60); /* 地址自增 */
    qmi_write(QMI_CTRL2, 0x13); /* ±4g, 1000Hz */
    qmi_write(QMI_CTRL3, 0x53); /* ±512dps, 1000Hz */
    qmi_write(QMI_CTRL7, 0x03); /* 使能加速度 + 陀螺仪 */
    vTaskDelay(pdMS_TO_TICKS(50));

    s_imu.motion = PET_MOTION_IDLE;
    s_imu.inited = true;
    return ESP_OK;
}

esp_err_t pet_imu_start(void) {
    if (!s_imu.inited) return ESP_ERR_INVALID_STATE;
    if (s_imu.running) return ESP_OK;
    s_imu.running = true;
    if (xTaskCreate(imu_task, "pet_imu", 4096, NULL, 3, &s_imu.task) != pdPASS) {
        s_imu.running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t pet_imu_calibrate(uint32_t ms) {
    if (!s_imu.inited) return ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, "开始标定，请保持车辆静止 %" PRIu32 " ms", ms);

    s_imu.calibrated = false;
    s_imu.task = NULL;

    /* 采样一段取平均，比单点更稳 */
    float sx = 0, sy = 0, sz = 0;
    int n = 0;
    TickType_t last = xTaskGetTickCount();
    uint32_t elapsed = 0;
    while (elapsed < ms) {
        float ax, ay, az;
        if (qmi_read_accel(&ax, &ay, &az) == ESP_OK) {
            sx += ax;
            sy += ay;
            sz += az;
            n++;
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_MS));
        elapsed += SAMPLE_MS;
    }
    if (n < 4) return ESP_FAIL;

    s_imu.gx = sx / n;
    s_imu.gy = sy / n;
    s_imu.gz = sz / n;
    s_imu.pitch0 = pitch_of(s_imu.gx, s_imu.gy, s_imu.gz);
    s_imu.roll0 = roll_of(s_imu.gy, s_imu.gz);

    memset(&s_imu.win, 0, sizeof(s_imu.win));
    s_imu.calibrated = true;
    ESP_LOGI(TAG, "标定完成 g=(%.3f, %.3f, %.3f) pitch0=%.1f roll0=%.1f",
             s_imu.gx, s_imu.gy, s_imu.gz, s_imu.pitch0, s_imu.roll0);
    return ESP_OK;
}

pet_motion_t pet_imu_get_motion(void) {
    return s_imu.inited ? s_imu.motion : PET_MOTION_IDLE;
}

esp_err_t pet_imu_set_motion(pet_motion_t m) {
    if (m >= PET_MOTION_COUNT) return ESP_ERR_INVALID_ARG;
    s_imu.motion = m;
    return ESP_OK;
}

/* 远程调试：强制运动状态，-1 恢复自动检测。
 * 用来把「画面能不能切」和「IMU 检测通不通」分开验证。 */
esp_err_t pet_imu_force_motion(int m) {
    if (m < -1 || m >= PET_MOTION_COUNT) return ESP_ERR_INVALID_ARG;
    s_imu.force_motion = m;
    if (m >= 0) s_imu.motion = (pet_motion_t)m;
    ESP_LOGW(TAG, "force_motion = %d", m);
    return ESP_OK;
}

float pet_imu_last_sigma(void) { return s_imu.sigma; }

bool pet_imu_is_calibrated(void) { return s_imu.inited && s_imu.calibrated; }
