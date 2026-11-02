# 网页端 —— 宠物动画生成

用户填表上传照片 → 调 RunningHub 生成视频 → 服务器转 MJPEG 帧 → 打包 `.pack` → 设备下载。

## 启动

```bash
cd web
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python server.py
```

打开 http://localhost:8080

**所有接口参数都在网页右上角「⚙ 管理设置」里改，不需要动代码、不需要重启。**
`config.json` 只是持久化载体，管理员日常不改它。

## RunningHub 接口实测结论（2026-11）

这个账号和这个工作流的真实行为，全部是实测出来的，不要再靠猜：

| 项 | 实测值 |
|---|---|
| API Key | 已配置（管理面板可改） |
| 工作流 ID | `2085393570013474817` |
| 工作流名 | `minimax-h3-reference-to-video-dual-image-15s` |
| 单次成本 | **¥0.499 / 段**，余额 ¥11.55 起 |
| 单次耗时 | **约 450 秒（7.5 分钟）** |
| 输出 | 15 秒 mp4，1 张或 2 张参考图 → 视频 |

### 两套接口的分工（重要）

```
创建：POST /openapi/v2/run/workflow/{workflowId}      Bearer 鉴权，返回扁平 JSON
查询：POST /task/openapi/status                       apiKey 走 body，返回 data 字符串
取件：POST /task/openapi/outputs                      apiKey 走 body
上传：POST /task/openapi/upload                       multipart，返回 fileName
```

**v2 创建的任务可以用 v1 接口查询**——这是实测确认过的，别去找 v2 的 query 路径，那些都会返
`errorCode 1001 Invalid URL`。

### 三个会让人卡半天的坑

1. `status` 返回的 `data` 是**纯字符串**（`"SUCCESS"` / `"RUNNING"` / `"FAILED"`），
   不是对象，按 `data.status` 取会抛 `AttributeError`。
2. 业务错误一律 **HTTP 200 + `code != 0`**，只看状态码会以为成功。
3. 失败的 `outputs` 返回 `code=805`，真实原因藏在 `data.failedReason.exception_message`。

### 节点映射（已探明）

用错误的 nodeId 去试探会把合法节点和字段名逼出来（错误信息会明确说是
`node_not_found_in_workflow` 还是 `field_not_found_in_node_inputs`）：

| nodeId | 字段名 | 用途 |
|---|---|---|
| 3 | `image` | 参考图 1（主体） |
| 4 | `image` | 参考图 2（双图工作流，可选） |
| 22 | `prompt` | 提示词 |
| 24 | `seed` | 随机种子 |
| 26 | `images` / 27: `video` | 输出汇聚节点，不用管 |

工作流里存在的节点：`3,4,5,6,7,14,21,22,23,24,25,26,27,28,29`。

## 提示规范（防止乱生成的核心）

五段固定结构，顺序不能乱，因为模型对靠前的约束更敏感：

```
① 角色锁定   必须是同一只：毛色/花纹/眼睛/比例/配饰，不许换脸、不许混入第二只
② 动作       每段唯一的变量（待机/奔跑/刹车/说话）
③ 镜头锁定   固定机位、正面平视、不推拉摇移、全身入镜、居中占高 65~75%
④ 背景光照   纯色背景、无场景无投影、光位色温全程不变
⑤ 禁止项     无文字水印、无形变多腿、无转场分屏、节奏不能快到首尾接不上
```

全部在「管理设置 → 提示规范」里逐段可编辑，右边能实时预览拼出来的最终提示词。

**为什么要这么严**：这类图生视频模型自由度很高，不锁死机位和构图，四段动画的
比例、光位、角色大小都会飘，拼成一个资源包在屏幕上切换时会明显跳。锁死之后，
切换只差动作本身，观感才连续。


## 关键参数（`config.json`）

| 键 | 默认 | 说明 |
|---|---|---|
| `media.size` | 480 | 必须与屏幕一致 |
| `media.fps` | 20 | 实测上限 25，留余量 |
| `media.jpeg_quality` | 6 | ffmpeg `-q:v`，越小越清晰也越大 |
| `media.max_clip_seconds` | 2.0 | 单段最长秒数 |
| `anims.basic` | 4 段 | idle / run / brake / talk |
| `anims.full` | 6 段 | 再加 fall / happy |

**体积控制**：4 段 × 2 秒 × 20fps × 45KB ≈ 7MB。若要让设备全量加载进 PSRAM，建议把 `max_clip_seconds` 降到 1.2，或 `jpeg_quality` 调到 7。

## 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/submit` | 表单提交，返回 `task_id` |
| GET | `/api/task/{id}` | 查状态与帧列表 |
| GET | `/api/frame/{id}/{anim}/{0001.jpg}` | 单帧图片（前端预览用） |
| GET | `/api/pack/{id}.pack` | 资源包下载（设备用这个） |
| GET | `/api/tasks` | 最近 50 个任务 |

## 资源包格式

```
[20B 头]
  magic "PETP" | version | anim_count | width | height | fps | reserved
  header_size u32 | total_size u32

[26B × anim_count 目录]
  name[12] | loop u8 | reserved | frame_count u16 | reserved
  frame_table_offset u32 | data_offset u32

[各动画帧长表] uint32 × frame_count
[各动画帧数据] 连续 JPEG
```

帧长表是「每帧字节数」，设备端累加即可定位任意帧。固件侧解析在 `firmware/components/pet_anim/pet_pack.c`。

## 部署到服务器

```bash
# 生产环境用 uvicorn 多进程 + nginx 反代
.venv/bin/uvicorn server:app --host 127.0.0.1 --port 8080 --workers 2
```

`storage/` 会越来越大（每个任务几 MB 到十几 MB），上线前加一个定时清理，只保留最近 N 天。

## 后续要补的

- [ ] 任务状态持久化到 SQLite（现在重启会丢正在跑的任务，但已完成的有 `meta.json`）
- [ ] 图片自动抠图（现在直接给 AI 原图，背景杂会带进去）
- [ ] 用户手机号验证与结果短信通知
- [ ] 存储清理与配额
- [ ] 生成失败的自动重试
