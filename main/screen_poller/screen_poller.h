#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <string>

// 收银屏轮询器：每 1.5 秒问一次 Mac 上的屏幕服务「现在该显示什么」，
// 版本号变了就拉一帧原始 RGB565 画面，全屏常驻显示（收款二维码、订单页、待客页）。
//
// 为什么不让大模型调 self.screen.preview_image：
// 1. 那是 user-only 工具，云端模型看不到也调不了
// 2. preview 只显示 5 秒就隐藏，二维码需要常驻到顾客付完款
// 把「显示什么」的控制权放在服务端，语音链路只需要调本地的收银工具。
class ScreenPoller {
public:
    static ScreenPoller& GetInstance();

    // 启动轮询任务（幂等）
    void Start();

private:
    ScreenPoller() = default;

    void Task();
    // 返回 false 表示这次轮询失败（网络/服务不可用），下轮再试
    bool PollOnce();

    std::atomic<bool> started_{false};
    std::atomic<bool> running_{true};
    std::string current_version_;  // 正在显示的画面版本（即页面路径）
};
