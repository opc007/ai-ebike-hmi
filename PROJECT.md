# AI 两轮车车机（卡皮巴拉数字宠物 + 小智语音）

一块 2.16 寸 AMOLED 圆角屏挂在电动两轮车把手上，屏幕上是一只卡皮巴拉：
车停下它待机，你推车它就原地跑，刹车会前倾，跟它说话它会张嘴，聊天内容带情绪还会换表情。
语音部分基于 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)（小智 AI），
宠物动画层（`components/pet_anim`）是本仓库自研部分。

## 硬件

| 项目 | 型号 |
| --- | --- |
| 主控板 | 微雪 ESP32-S3-Touch-AMOLED-2.16（16MB Flash / 8MB PSRAM） |
| 屏幕 | CO5300 QSPI AMOLED 480×480，圆角 2.16" |
| 触摸 | CST9220（I2C 0x5A） |
| 六轴 | QMI8658（I2C 0x6B，与编解码共用总线） |
| 电源管理 | AXP2101（I2C 0x34） |
| 音频 | ES8311 + ES7210，24kHz 输出 / 16kHz 输入，带回采 AEC |
| 固件 SDK | ESP-IDF v6.0.2 |

板型宏：`CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_2_16`。

## 仓库结构

```
main/boards/waveshare/esp32-s3-touch-amoled-2.16/   板级初始化 + 宠物层接线
components/pet_anim/                                宠物动画播放器（自研）
    pet_anim.c      解码 + 送屏 + 段切换 + 情绪/说话联动
    pet_imu.c       QMI8658 六轴 → 静止/骑行/刹车/摔倒
    pet_pull.c      资源包拉取、状态上报、远程指令、开机音效下载
server/                                             配套网页端（生成宠物 + 托管资源）
docs/车机架构方案.md                                 整体架构、成本模型、渠道合规
docs/显示故障根因记录.md                             这块板踩过的显示坑（白色横杠/定格/重启）
partitions/v2/16m.csv                               分区表：ota_0/ota_1 + assets + pets
```

## 快速开始

### 固件

```bash
# ESP-IDF v6.0.2
. ~/esp-idf/export.sh
idf.py set-target esp32s3

# 选择板型（或直接在 menuconfig 里选）
idf.py menuconfig   # Xiaozhi Assistant → Board Type → Waveshare ESP32-S3-Touch-AMOLED-2.16

idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

开机后设备会连 WiFi，然后用 `main/boards/.../esp32-s3-touch-amoled-2.16.cc` 顶部的
`PET_PACK_URL` / `PET_BOOT_OGG_URL` 去服务端取「当前宠物」和开机音效，改这两个宏即可指向自己的服务器。

### 服务端

```bash
cd server
cp config.example.json config.json     # 填 RunningHub api_key 和管理员密码
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/python server.py             # http://localhost:8080
```

网页端上传一张宠物照片 → 调 RunningHub 工作流生成六段动作视频 → 服务端抽帧打包 `.pack`
→ 设备下次开机自动换成新宠物。详见 `server/README.md`。

### 设备侧接口

| 接口 | 用途 |
| --- | --- |
| `GET /api/pet/latest.pack` | 资源包（`PETP` 格式，480×480 JPEG 帧，段：idle/run/brake/talk/fall/happy） |
| `GET /api/pet/latest.ogg` | 开机音效（从宠物视频音轨抽出的 Opus） |
| `POST /api/pet/report` | 设备上报播放段、运动状态、抖动 sigma、已送帧数 |
| `GET /api/pet/status` | 网页端查设备在线状态 |
| `GET/POST /api/pet/cmd` | 远程强制切段（联调用） |

## 资源包格式（PETP）

```
偏移 0    4B   "PETP" 魔数
          ...  头：'<4sBBHHBBI'（宽/高/fps/段数/总帧数/尺寸）
          段表：每段 26 字节（名字 12B + 起始帧 + 帧数 + 循环标志）
          帧表：每帧偏移/长度
数据区    每帧独立 JPEG（480×480，RGB565 解码后直接送屏）
```

## 已知限制（真实设备实测）

- **SPIFFS 写入极慢（约 5KB/s）**：2MB 资源包落盘要四分钟。所以固件采用「下载到 PSRAM 直接播」，
  每次开机重下一遍（局域网 2MB 约 1 秒），不落盘；`pet.pack` 只在手动放置时才走本地加载。
- **触摸偶发不响应**：主控单独复位（USB 重新枚举）时，独立供电的 CST9220 可能停在半截事务里，
  I2C 全总线无应答。固件用 `i2c_master_probe` 探测后跳过触摸初始化，保证不开机卡死；
  恢复办法是断电重插（拔 USB 再插）。
- **屏幕并发**：AMOLED 走 QSPI，LVGL 与宠物帧同时刷新会抢 SPI 事务池，出现白色横杠/定格。
  宠物层接管屏幕前必须 `lvgl_port_stop()`，并且每帧送屏要持 `lvgl_port_lock()` 串行化。
  细节见 `docs/显示故障根因记录.md`。
- 应用分区只剩约 60KB 余量，加功能前先看 `idf.py size`。

## 许可与致谢

本仓库基于 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)（MIT，Copyright (c) 2025
Shenzhen Xinzhi Future Technology Co., Ltd.），保留上游 `LICENSE` 与版权声明；
`components/pet_anim`、`server/`、`docs/` 下的车机相关内容同样以 MIT 发布。
