#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

// 摄像头迎宾：待客态下用本地人脸检测识别进店顾客，
// 检测到人就播欢迎语音并自动进入聆听（等同一次唤醒词触发）。
// 模型与推理全部在设备本地完成，不传图、不联网。
class Greeter {
public:
    static Greeter& GetInstance();

    // 启动检测任务（幂等）
    void Start();

    // 设备状态机变化时调用；仅待客态下做检测
    void SetState(int state);

    // 音频 VAD 回调：待客态下有人开口说话（如「有人在吗」）时触发迎宾
    void OnVoiceActivity(bool speaking);

private:
    Greeter() = default;

    void Task();
    void Trigger();
    // 摄像头/声音两条路径共用的迎宾动作；冷却期内返回 false
    bool TryGreet(const char* source);

    std::atomic<bool> started_{false};
    std::atomic<bool> running_{true};
    std::atomic<bool> active_{false};
    // 会话守护：迎宾唤醒后的这段时间里盯着"顾客还在不在"，
    // 长时间没人说话就挂断会话回到待机，让下一位顾客能被正常迎接
    std::atomic<bool> session_live_{false};
    std::atomic<int64_t> greet_us_{0};     // 最近一次迎宾唤醒的时刻
    std::atomic<int64_t> last_voice_us_{0}; // 最近一次听到顾客说话的时刻
    // 声音触发状态
    std::atomic<bool> voice_active_{false};
    std::atomic<int> voice_bursts_{0};
    std::atomic<int64_t> voice_first_us_{0};
    // 最近一次迎宾时间（摄像头与声音共享冷却）
    std::atomic<int64_t> last_greet_us_{0};
};
