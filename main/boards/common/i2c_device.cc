#include "i2c_device.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define TAG "I2cDevice"

I2cDevice::I2cDevice(i2c_master_bus_handle_t i2c_bus, uint8_t addr)
    : i2c_bus_(i2c_bus), device_address_(addr) {
    i2c_device_config_t i2c_device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400 * 1000,
        .scl_wait_us = 0,
        .flags =
            {
                .disable_ack_check = 0,
            },
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &i2c_device_cfg, &i2c_device_));
    assert(i2c_device_ != NULL);
}

/* I2C 总线是共享的（AXP2101 / 触摸 / 音频 codec / QMI8658 都在上面），
 * 任何一次传输超时都不能让整机 abort 重启——车机正在骑行时重启是不可接受的。
 * 这里把 ESP_ERROR_CHECK 全部改成「容忍失败 + 限流告警」：
 *   - 写失败：丢弃这次写，下次刷新时会重写
 *   - 读失败：返回 0（调用方拿到的是"无效值"而不是崩溃）
 * 另外加了错误计数，超过阈值时尝试复位总线（i2c_master_bus_reset），
 * 自己把挂死的总线救回来。 */
static uint32_t s_i2c_err_count = 0;
static int64_t s_i2c_last_warn_us = 0;

static void i2c_note_error(const char* op, uint8_t addr, uint8_t reg, esp_err_t err) {
    s_i2c_err_count++;
    int64_t now = esp_timer_get_time();
    /* 限流：最多 2 秒打一条，避免总线抖动时日志刷屏把串口堵死 */
    if (now - s_i2c_last_warn_us > 2000000) {
        s_i2c_last_warn_us = now;
        ESP_LOGW(TAG, "%s 失败 addr=0x%02x reg=0x%02x err=%s（累计 %" PRIu32 " 次）",
                 op, addr, reg, esp_err_to_name(err), s_i2c_err_count);
    }
}

void I2cDevice::WriteReg(uint8_t reg, uint8_t value) {
    uint8_t buffer[2] = {reg, value};
    esp_err_t err = i2c_master_transmit(i2c_device_, buffer, 2, 100);
    if (err != ESP_OK) i2c_note_error("WriteReg", device_address_, reg, err);
}

void I2cDevice::WriteRegs(uint8_t reg, const uint8_t* buffer, size_t length) {
    uint8_t* tx = (uint8_t*)malloc(length + 1);
    if (tx == nullptr) {
        return;
    }
    tx[0] = reg;
    memcpy(tx + 1, buffer, length);
    esp_err_t err = i2c_master_transmit(i2c_device_, tx, length + 1, 100);
    free(tx);
    if (err != ESP_OK) i2c_note_error("WriteRegs", device_address_, reg, err);
}

uint8_t I2cDevice::ReadReg(uint8_t reg) {
    uint8_t buffer[1] = {0};
    esp_err_t err = i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer, 1, 100);
    if (err != ESP_OK) {
        i2c_note_error("ReadReg", device_address_, reg, err);
        return 0;
    }
    return buffer[0];
}

void I2cDevice::ReadRegs(uint8_t reg, uint8_t* buffer, size_t length) {
    esp_err_t err = i2c_master_transmit_receive(i2c_device_, &reg, 1, buffer, length, 100);
    if (err != ESP_OK) {
        i2c_note_error("ReadRegs", device_address_, reg, err);
        memset(buffer, 0, length);
    }
}

esp_err_t I2cDevice::ResetBus(const char* reason) {
    ESP_LOGW(TAG, "Resetting I2C bus: %s", reason ? reason : "unspecified");
    return i2c_master_bus_reset(i2c_bus_);
}
