#include "greeter.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#include "esp32_camera.h"
#include "human_face_detect.hpp"

#define GREETER_TAG "Greeter"

namespace {

constexpr int kDetectIntervalMs = 120;      // 相邻两次检测的间隔（取帧+推理仅~70ms，可高频扫描）
constexpr int kConfirmHits = 2;             // 连续检出次数（约 0.7s 内稳定出现）
constexpr int kCooldownSeconds = 25;        // 两次迎宾的最小间隔（门店进出频繁，不能太久）
constexpr float kMinFaceWidthRatio = 0.04f; // 人脸最小宽度 / 画面宽（1米外也能检出）
constexpr float kMinFaceScore = 0.45f;      // 检测置信度阈值（背光场景放宽）
constexpr int kDetectWidth = 160;           // 检测输入分辨率（与 MSR 模型匹配）
constexpr int kDetectHeight = 120;
constexpr int64_t kSessionSilenceUs = 30LL * 1000000;  // 会话中 30 秒没有任何语音 → 判定顾客离开

}  // namespace

namespace {

// 调试用：把检测帧原样 POST 到电脑，肉眼确认摄像头到底拍到了什么
void UploadDebugFrame(const uint16_t* px, int w, int h) {
    esp_http_client_config_t cfg = {};
    cfg.url = "http://10.197.1.234:8801/debug_frame";
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 5000;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        return;
    }
    esp_http_client_set_header(cli, "Content-Type", "application/octet-stream");
    esp_http_client_set_post_field(cli, reinterpret_cast<const char*>(px), w * h * 2);
    esp_err_t err = esp_http_client_perform(cli);
    ESP_LOGI(GREETER_TAG, "debug upload: %s", esp_err_to_name(err));
    esp_http_client_cleanup(cli);
}

}  // namespace

Greeter& Greeter::GetInstance() {
    static Greeter instance;
    return instance;
}

void Greeter::Start() {
    if (started_.exchange(true)) {
        return;
    }
    xTaskCreate(
        [](void* arg) {
            static_cast<Greeter*>(arg)->Task();
            vTaskDelete(nullptr);
        },
        "greeter", 6144, this, 3, nullptr);
    ESP_LOGI(GREETER_TAG, "greeter task started");
}

void Greeter::SetState(int state) {
    // 只有待客态才做检测；对话、播放、唤醒中一律暂停
    active_ = (state == static_cast<int>(kDeviceStateIdle));
    if (!active_) {
        voice_bursts_ = 0;
        voice_active_ = false;
    }
}

void Greeter::OnVoiceActivity(bool speaking) {
    // 任何状态下都记录"最后听到人声"的时刻：会话守护要靠它判断顾客是否离开
    if (speaking) {
        last_voice_us_ = esp_timer_get_time();
    }
    if (!active_) {
        return;
    }
    if (speaking) {
        // 统计 3 秒窗口内的语音段数：一段话「有人在吗」通常有多个 VAD 突发
        int64_t now = esp_timer_get_time();
        if (!voice_active_.exchange(true)) {
            int bursts = voice_bursts_ + 1;
            if (now - voice_first_us_.load() > 3000000) {
                bursts = 1;
                voice_first_us_ = now;
            }
            voice_bursts_ = bursts;
        }
    } else {
        voice_active_ = false;
    }
}

bool Greeter::TryGreet(const char* source) {
    int64_t now = esp_timer_get_time();
    int64_t last = last_greet_us_.load();
    if (now - last < static_cast<int64_t>(kCooldownSeconds) * 1000000) {
        return false;
    }
    if (!last_greet_us_.compare_exchange_strong(last, now)) {
        return false;
    }
    active_ = false;
    Trigger();
    ESP_LOGI(GREETER_TAG, "%s, greeting", source);
    // 迎宾话术只由小智（云端 TTS）说一种，这里不再播本地 OGG，避免两个声音。
    // 唤醒后 AI 会在几秒内主动说后台配置的欢迎语。
    greet_us_ = esp_timer_get_time();
    session_live_ = true;  // 开始会话守护
    vTaskDelay(pdMS_TO_TICKS(300));  // 略缓冲，避免与摄像头切帧抢总线
    Application::GetInstance().WakeWordInvoke("greeter");
    return true;
}

void Greeter::Task() {
    auto* camera = static_cast<Esp32Camera*>(Board::GetInstance().GetCamera());
    if (!camera) {
        ESP_LOGW(GREETER_TAG, "board has no camera, greeter disabled");
        return;
    }

    HumanFaceDetect* detector = nullptr;
    uint16_t* frame = nullptr;
    int hits = 0;
    int dbg_tick = 0;
    int dbg_uploads_ = 0;

    while (running_) {
        vTaskDelay(pdMS_TO_TICKS(kDetectIntervalMs));

        // ── 会话守护：解决顾客进进出出的轮换问题 ──
        // 顾客拿了东西就走、或进来瞄一眼就走，设备还挂在 Listening 等人说话，
        // 会话不结束下一位顾客就迎不进来。规则：
        //   - 设备回到 Idle：会话已自然结束，迎宾立刻重新武装
        //   - 还在 Listening 且 30 秒没听到任何人声（唤醒后没开口，
        //     或对话中途顾客走开）：主动挂断回待机
        //   - Speaking/Connecting 等中间态不动它，让流程自己走完
        if (session_live_.load()) {
            auto& app = Application::GetInstance();
            DeviceState st = app.GetDeviceState();
            if (st == kDeviceStateIdle) {
                session_live_ = false;
                ESP_LOGI(GREETER_TAG, "session ended, greeter re-armed");
            } else if (st == kDeviceStateListening) {
                int64_t anchor = greet_us_.load();
                int64_t lv = last_voice_us_.load();
                if (lv > anchor) {
                    anchor = lv;
                }
                if (esp_timer_get_time() - anchor > kSessionSilenceUs) {
                    ESP_LOGI(GREETER_TAG, "customer gone (30s silence), closing session");
                    app.ToggleChatState();  // Listening 态下会关闭音频通道回待机
                    session_live_ = false;
                }
            }
        }

        if (!active_) {
            hits = 0;
            if (++dbg_tick % 20 == 0) {
                ESP_LOGI(GREETER_TAG, "paused: device not idle (tick=%d)", dbg_tick);
            }
            continue;
        }

        if (!detector) {
            ESP_LOGI(GREETER_TAG, "loading face detect model");
            detector = new HumanFaceDetect();
        }
        if (!frame) {
            frame = static_cast<uint16_t*>(
                heap_caps_malloc(kDetectWidth * kDetectHeight * sizeof(uint16_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!frame) {
                ESP_LOGE(GREETER_TAG, "alloc detect frame failed");
                return;
            }
        }

        camera_fb_t* fb = camera->AcquireFrame(80);
        if (!fb) {
            if (++dbg_tick % 20 == 0) {
                ESP_LOGW(GREETER_TAG, "no frame from camera (tick=%d)", dbg_tick);
            }
            continue;
        }

        bool face = false;
        bool strong_face = false;
        if (fb->width >= kDetectWidth && fb->height >= kDetectHeight) {
            // VGA RGB565 等比抽点降采样，并把字节序转成 LE
            const int step_x = fb->width / kDetectWidth;
            const int step_y = fb->height / kDetectHeight;
            const uint16_t* src = reinterpret_cast<const uint16_t*>(fb->buf);
            for (int y = 0; y < kDetectHeight; y++) {
                const uint16_t* srow = src + static_cast<size_t>(y) * step_y * fb->width;
                uint16_t* drow = frame + static_cast<size_t>(y) * kDetectWidth;
                for (int x = 0; x < kDetectWidth; x++) {
                    drow[x] = __builtin_bswap16(srow[x * step_x]);
                }
            }

            dl::image::img_t img = {
                .data = frame,
                .width = kDetectWidth,
                .height = kDetectHeight,
                .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
            };
            if (dbg_tick % 30 == 0) {  // 调试：每 10 秒传一帧
                dbg_uploads_++;
                UploadDebugFrame(frame, kDetectWidth, kDetectHeight);
            }
            auto& candidates = detector->run(img);
            if (++dbg_tick % 10 == 0) {
                int best_score = -1, best_w = 0;
                for (auto& r : candidates) {
                    int sc = static_cast<int>(r.score * 100);
                    int ww = static_cast<int>(r.box[2] - r.box[0]);
                    if (sc > best_score) { best_score = sc; best_w = ww; }
                }
                ESP_LOGI(GREETER_TAG, "scan: faces=%d best_score=%d best_width=%d/%d",
                         static_cast<int>(candidates.size()), best_score, best_w, kDetectWidth);
            }
            for (auto& r : candidates) {
                float w = r.box[2] - r.box[0];
                if (r.score >= kMinFaceScore && w / kDetectWidth >= kMinFaceWidthRatio) {
                    face = true;
                    if (r.score >= 0.60f && w / kDetectWidth >= 0.10f) {
                        strong_face = true;  // 高置信大脸：单帧立即迎宾，不等确认
                    }
                    break;
                }
            }
        }

        camera->ReleaseFrame(fb);

        if (face) {
            if (strong_face || ++hits >= kConfirmHits) {
                hits = 0;
                TryGreet(strong_face ? "customer detected (strong)" : "customer detected");
            }
        } else {
            hits = 0;
        }

        // 声音触发：3 秒窗口内出现 ≥2 段语音突发（如喊「有人在吗」）
        if (active_ && voice_bursts_.load() >= 2 &&
            esp_timer_get_time() - voice_first_us_.load() < 3000000) {
            voice_bursts_ = 0;
            voice_active_ = false;
            TryGreet("voice detected");
        }
    }
}

void Greeter::Trigger() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display) {
        display->SetChatMessage("assistant", "欢迎光临，需要点什么？");
    }
}
