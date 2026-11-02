#include "screen_poller.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cJSON.h>
#include <memory>

#include "board.h"
#include "display.h"
#include "lvgl_image.h"
#include "settings.h"

// 等待网络就绪：设备进入待客态 = WiFi 已连接、协议已连上云端。
// 过早发 HTTP 会踩中 lwIP 的 tcpip mbox 未初始化断言（开机即崩）。
#include "application.h"

#define TAG "ScreenPoller"

// 轮询间隔。收银场景对实时性要求不高，1.5 秒足够（结账到出码最多晚 1.5 秒）
#define POLL_INTERVAL_MS 1500

// Mac 上屏幕服务的地址。可在 NVS 里用 cashier/server_url 覆盖。
#define DEFAULT_SERVER_URL "http://10.197.1.234:8801"

// 屏幕分辨率与服务器 render_screen 的输出保持一致（ST7789 240x320）
#define FRAME_W 240
#define FRAME_H 320

static std::string GetServerBase() {
    Settings settings("cashier", false);
    std::string url = settings.GetString("server_url");
    if (url.empty()) {
        url = DEFAULT_SERVER_URL;
    }
    return url;
}

ScreenPoller& ScreenPoller::GetInstance() {
    static ScreenPoller instance;
    return instance;
}

void ScreenPoller::Start() {
    if (started_.exchange(true)) {
        return;
    }
    xTaskCreate(
        [](void* arg) {
            static_cast<ScreenPoller*>(arg)->Task();
            vTaskDelete(nullptr);
        },
        "screen_poll", 4096, this, 3, nullptr);
    ESP_LOGI(TAG, "started (server=%s)", GetServerBase().c_str());
}

void ScreenPoller::Task() {
    // 首轮必须等网络就绪，否则 lwIP 还没起会直接 assert 重启
    while (running_ &&
           Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    for (;;) {
        if (running_) {
            PollOnce();
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

bool ScreenPoller::PollOnce() {
    auto base = GetServerBase();

    // 第一步：问当前该显示什么画面
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (auto opened = http->Open("GET", base + "/next_screen"); !opened) {
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        http->Close();
        return false;
    }
    std::string json = http->ReadAll();
    http->Close();

    std::string version;
    std::string path;
    if (cJSON* root = cJSON_Parse(json.c_str()); root != nullptr) {
        auto* v = cJSON_GetObjectItem(root, "v");
        auto* u = cJSON_GetObjectItem(root, "url");
        if (cJSON_IsString(v)) version = v->valuestring;
        if (cJSON_IsString(u)) path = u->valuestring;
        cJSON_Delete(root);
    }
    if (version.empty() || path.empty()) {
        return true;  // 服务端还没登记画面，不算错误
    }
    if (version == current_version_) {
        return true;  // 没变化
    }

    // 第二步：下载原始 RGB565 帧（W*H*2 字节，约 150KB，放 PSRAM）
    auto img_http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (auto opened = img_http->Open("GET", base + path); !opened) {
        ESP_LOGE(TAG, "open frame failed: %s", path.c_str());
        return false;
    }
    auto frame_status = img_http->GetStatusCode();
    if (!frame_status || *frame_status != 200) {
        img_http->Close();
        return false;
    }
    size_t length = img_http->GetBodyLength();
    if (length < FRAME_W * FRAME_H * 2) {
        img_http->Close();
        ESP_LOGE(TAG, "frame too small: %u", (unsigned)length);
        return false;
    }
    char* data = static_cast<char*>(heap_caps_malloc(length, MALLOC_CAP_8BIT));
    if (data == nullptr) {
        img_http->Close();
        return false;
    }
    size_t total = 0;
    while (total < length) {
        auto ret = img_http->Read(data + total, length - total);
        if (!ret || *ret == 0) {
            break;
        }
        total += static_cast<size_t>(*ret);
    }
    img_http->Close();
    if (total != length) {
        heap_caps_free(data);
        ESP_LOGE(TAG, "frame download incomplete: %u/%u", (unsigned)total, (unsigned)length);
        return false;
    }

    // 第三步：常驻显示。数据所有权交给 LvglAllocatedImage，由它负责释放
    auto image = std::make_unique<LvglAllocatedImage>(data, total, FRAME_W, FRAME_H,
                                                      FRAME_W * 2, LV_COLOR_FORMAT_RGB565);
    if (!image->IsValid()) {
        ESP_LOGE(TAG, "invalid frame");
        return false;
    }
    Board::GetInstance().GetDisplay()->SetPersistentImage(std::move(image));
    current_version_ = version;
    ESP_LOGI(TAG, "screen -> %s", version.c_str());
    return true;
}
