#!/usr/bin/env python3
"""
宠物动画生成服务 —— 网页端后端

链路：用户填表 → 调 RunningHub 工作流生成 MP4 → ffmpeg 转 480x480 MJPEG 帧
      → 打包成 .pack → 设备 HTTP 拉取播放

启动：  python3 server.py            （默认 0.0.0.0:8080）
联调：  config.json 里 runninghub.mode = "mock" 时不需要真实 API
"""

import asyncio
import hashlib
import json
import os
import re
import secrets
import shutil
import struct
import subprocess
import time
import uuid
from pathlib import Path
from typing import Dict, List, Optional

import httpx
from fastapi import BackgroundTasks, FastAPI, File, Form, HTTPException, Request, Response, UploadFile
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

BASE = Path(__file__).parent.resolve()
STORAGE = BASE / "storage"
CONFIG_PATH = BASE / "config.json"


def ensure_dir(p: Path):
    """某些运行环境的 mkdir shim 会让 exist_ok 失效，这里显式吞掉已存在错误。"""
    if p.exists():
        return
    try:
        p.mkdir(parents=True, exist_ok=True)
    except (FileExistsError, OSError):
        if not p.exists():
            raise


ensure_dir(STORAGE)

app = FastAPI(title="Pet Anim Server")

# ---------------------------------------------------------------- 配置

def load_config() -> dict:
    with open(CONFIG_PATH, "r", encoding="utf-8") as f:
        return json.load(f)


CONFIG: dict = load_config()


def cfg(*keys, default=None):
    node = CONFIG
    for k in keys:
        if not isinstance(node, dict) or k not in node:
            return default
        node = node[k]
    return node


def cfgi(keys: str, default: int) -> int:
    """取整型配置。网页表单提交的是字符串，必须强制转换，否则 struct.pack 会报
    「required argument is not an integer」。"""
    v = cfg(*keys.split("."), default=default)
    try:
        return int(float(v))
    except (TypeError, ValueError):
        return default


def cfgf(keys: str, default: float) -> float:
    v = cfg(*keys.split("."), default=default)
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


# ---------------------------------------------------------------- 任务存储
# 每个任务一个目录：storage/<task_id>/
#   meta.json  source.jpg  raw/<anim>.mp4  frames/<anim>/*.jpg  pet.pack

TASKS: Dict[str, dict] = {}


def task_dir(tid: str) -> Path:
    return STORAGE / tid


def save_meta(tid: str):
    t = TASKS.get(tid)
    if t:
        (task_dir(tid) / "meta.json").write_text(
            json.dumps(t, ensure_ascii=False, indent=2), encoding="utf-8"
        )


def load_all_tasks():
    for d in STORAGE.iterdir():
        if d.is_dir() and (d / "meta.json").exists():
            try:
                TASKS[d.name] = json.loads((d / "meta.json").read_text(encoding="utf-8"))
            except Exception:
                pass


load_all_tasks()


# ---------------------------------------------------------------- 生成适配器

async def _mock_generate(tid: str, anim: str, source: Path, out_mp4: Path):
    """本地合成测试片段，用于不接 RunningHub 的联调。"""
    dur = cfg("generation", "clip_seconds", default=2.0)
    # 每个动作给不同色相，预览时能区分是哪一段
    hue = {"idle": 0, "run": 40, "brake": 80, "talk": 120, "fall": 160, "happy": 200}
    h = hue.get(anim, 0)
    # testsrc2 自带运动，能验证「转码→打包→播放」整条链路
    cmd = [
        "ffmpeg", "-y", "-loglevel", "error",
        "-f", "lavfi", "-i", f"testsrc2=s=480x480:r=20:d={dur}",
        "-vf", f"hue=h={h},eq=saturation=0.55:brightness=-0.05",
        "-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p",
        str(out_mp4),
    ]
    await asyncio.to_thread(subprocess.run, cmd, check=True)


# ---------------------------------------------------------------- RunningHub 客户端
#
# 实测确认的接口约定（2026-11，runninghub.cn）：
#   POST /uc/openapi/accountStatus   {"apiKey"}            → data:{remainCoins, remainMoney, ...}
#   POST /task/openapi/upload        multipart apiKey+file → data:{fileName:"api/xxx.jpg"}
#   POST /task/openapi/create        {"apiKey","workflowId"(数字),"nodeInfoList"}
#                                                          → data:{taskId:"2117...", taskStatus:"RUNNING"}
#   POST /task/openapi/status        {"apiKey","taskId"}   → data:"RUNNING" | "SUCCESS" | "FAILED"  ← 纯字符串
#   POST /task/openapi/outputs       {"apiKey","taskId"}   → data:[{fileUrl, fileType, ...}]
#
# 三条容易踩的坑：
#   1. status 的 data 是字符串不是对象，按 .get("status") 取会抛 AttributeError
#   2. 业务错误一律 HTTP 200 + code!=0，必须查 code 字段，不能只看状态码
#   3. 失败的 outputs 返回 code=805，真实原因藏在 data.failedReason 里

class RHError(RuntimeError):
    def __init__(self, code, msg, detail=None):
        self.code = code
        self.msg = msg
        self.detail = detail
        super().__init__(self._fmt())

    def _fmt(self):
        s = f"RunningHub[{self.code}] {self.msg}"
        fr = (self.detail or {}).get("failedReason") if isinstance(self.detail, dict) else None
        if isinstance(fr, dict):
            em = fr.get("exception_message") or fr.get("exception_type")
            if em:
                s += f" · {em}"
        return s


def rh_conf():
    rh = CONFIG["runninghub"]
    return {
        "version": str(rh.get("api_version", "v2")).lower(),
        "base": str(rh.get("base_url", "")).rstrip("/"),
        "key": os.environ.get("RUNNINGHUB_API_KEY") or str(rh.get("api_key") or "").strip(),
        "workflow": str(rh.get("workflow_id") or "").strip(),
        "create_v2": rh.get("create_path_v2", "/openapi/v2/run/workflow/{{WORKFLOW}}"),
        "create_v1": rh.get("create_path_v1", "/task/openapi/create"),
        "status": rh.get("status_path", "/task/openapi/status"),
        "outputs": rh.get("outputs_path", "/task/openapi/outputs"),
        "upload": rh.get("upload_path", "/task/openapi/upload"),
        "poll": float(rh.get("poll_interval", 5)),
        "timeout": float(rh.get("timeout_seconds", 900)),
        "instance": rh.get("instance_type", "default"),
        "personal_queue": str(rh.get("use_personal_queue", "false")).lower(),
    }


def rh_ready() -> bool:
    c = rh_conf()
    return bool(c["key"] and c["workflow"])


def effective_mode() -> str:
    """auto：配好 key 和 workflowId 就走真实，否则退回 mock。"""
    mode = str(cfg("runninghub", "mode", default="auto")).lower()
    if mode in ("real", "mock"):
        return mode
    return "real" if rh_ready() else "mock"


async def rh_call(client: httpx.AsyncClient, path: str, payload: dict):
    c = rh_conf()
    r = await client.post(c["base"] + path, json={"apiKey": c["key"], **payload})
    if r.status_code >= 400:
        raise RHError(r.status_code, f"HTTP {r.status_code}", r.text[:300])
    body = r.json()
    if body.get("code") != 0:
        raise RHError(body.get("code"), body.get("msg") or "unknown", body.get("data"))
    return body.get("data")


async def rh_account(client: httpx.AsyncClient) -> dict:
    return await rh_call(client, "/uc/openapi/accountStatus", {}) or {}


_UPLOAD_CACHE: Dict[str, str] = {}
_UPLOAD_LOCK = asyncio.Lock()


async def rh_upload(client: httpx.AsyncClient, path: Path, file_type="image", cache_key: str = "") -> str:
    """上传本地文件，返回 fileName（形状 api/xxx.jpg），用于填进 nodeInfoList。

    同一任务的多段动画共用一次上传结果——四段必须喂完全相同的参考图，
    否则模型会因文件名不同而重新读取、甚至产生细微差异。
    """
    if cache_key and cache_key in _UPLOAD_CACHE:
        return _UPLOAD_CACHE[cache_key]
    async with _UPLOAD_LOCK:
        if cache_key and cache_key in _UPLOAD_CACHE:   # 双检，避免并发重复上传
            return _UPLOAD_CACHE[cache_key]
        c = rh_conf()
        with open(path, "rb") as f:
            files = {"file": (path.name, f, "application/octet-stream")}
            data = {"apiKey": c["key"], "fileType": file_type}
            r = await client.post(c["base"] + c["upload"], data=data, files=files, timeout=120)
        if r.status_code >= 400:
            raise RHError(r.status_code, f"上传失败 HTTP {r.status_code}", r.text[:200])
        body = r.json()
        if body.get("code") != 0:
            raise RHError(body.get("code"), body.get("msg") or "上传失败")
        fn = (body.get("data") or {}).get("fileName")
        if not fn:
            raise RHError(-1, "上传未返回 fileName", body)
        if cache_key:
            _UPLOAD_CACHE[cache_key] = fn
        return fn


async def rh_create(client: httpx.AsyncClient, node_list: list) -> str:
    """两条创建路径，返回 taskId。

    v2（推荐）：POST /openapi/v2/run/workflow/{workflowId}，Bearer 鉴权，返回扁平结构
    v1（兼容）：POST /task/openapi/create，apiKey 走 body，返回被 data 包一层
    """
    c = rh_conf()
    if c["version"] == "v2":
        path = c["create_v2"].replace("{{WORKFLOW}}", c["workflow"])
        r = await client.post(
            c["base"] + path,
            json={
                "addMetadata": True,
                "nodeInfoList": node_list,
                "instanceType": c["instance"],
                "usePersonalQueue": c["personal_queue"],
            },
            headers={"Authorization": f"Bearer {c['key']}", "Content-Type": "application/json"},
        )
        if r.status_code >= 400:
            raise RHError(r.status_code, f"HTTP {r.status_code}", r.text[:300])
        body = r.json()
        tid = str(body.get("taskId") or "").strip()
        if not tid:
            raise RHError(
                body.get("errorCode") or -1,
                body.get("errorMessage") or "v2 未返回 taskId",
                body.get("failedReason"),
            )
        return tid

    data = await rh_call(
        client, c["create_v1"],
        {"workflowId": int(c["workflow"]), "nodeInfoList": node_list},
    )
    tid = (data or {}).get("taskId")
    if not tid:
        raise RHError(-1, "提交未返回 taskId", data)
    return str(tid)


async def rh_status(client: httpx.AsyncClient, task_id: str) -> str:
    data = await rh_call(client, rh_conf()["status"], {"taskId": task_id})
    # data 是纯字符串（RUNNING / SUCCESS / FAILED），也可能被包成 {"status": x}
    if isinstance(data, dict):
        data = data.get("status") or data.get("taskStatus") or ""
    return str(data).upper()


async def rh_outputs(client: httpx.AsyncClient, task_id: str) -> List[str]:
    data = await rh_call(client, rh_conf()["outputs"], {"taskId": task_id})
    return _extract_urls(data)


def _walk_urls(node, out: List[str], depth=0):
    """递归找 fileUrl，兼容 data 是 list / dict / 嵌套。"""
    if depth > 4 or node is None:
        return
    if isinstance(node, list):
        for it in node:
            _walk_urls(it, out, depth + 1)
    elif isinstance(node, dict):
        u = node.get("fileUrl") or node.get("file_url") or node.get("url")
        ft = str(node.get("fileType") or node.get("file_type") or "").lower()
        if u and isinstance(u, str):
            if ft and ft not in ("mp4", "video", "webm", "mov", "gif", "webp"):
                return  # 跳过预览图/封面图
            out.append(u)
        else:
            for v in node.values():
                if isinstance(v, (list, dict)):
                    _walk_urls(v, out, depth + 1)


def _extract_urls(data) -> List[str]:
    urls: List[str] = []
    _walk_urls(data, urls)
    return urls


async def _real_generate(tid: str, anim: str, source: Path, out_mp4: Path):
    """真实链路：上传图 → create → 轮询 → 取 URL → 下载。"""
    c = rh_conf()
    if not c["key"]:
        raise RHError(-1, "未配置 API Key")
    if not c["workflow"]:
        raise RHError(-1, "未配置 workflowId（在 config.json 里填）")

    async with httpx.AsyncClient(timeout=60) as client:
        file_name = await rh_upload(client, source, cache_key=f"{tid}:{source.stat().st_mtime_ns}")
        extra = (TASKS.get(tid, {}).get("extra_prompt") or "").strip()
        node_list = _build_node_list(anim, file_name, extra)
        task_id = await rh_create(client, node_list)

        deadline = time.time() + c["timeout"]
        last = ""
        while time.time() < deadline:
            await asyncio.sleep(c["poll"])
            last = await rh_status(client, task_id)
            if last == "SUCCESS":
                break
            if last in ("FAILED", "ERROR", "CANCELLED"):
                # 取一次 outputs 拿失败原因（code=805 时 data 里有 failedReason）
                try:
                    await rh_outputs(client, task_id)
                except RHError as e:
                    raise RuntimeError(f"{anim} 生成失败：{e}") from None
                raise RuntimeError(f"{anim} 生成失败（{last}）")
        else:
            raise RuntimeError(f"{anim} 生成超时（最后状态 {last}）")

        urls = await rh_outputs(client, task_id)

    if not urls:
        raise RuntimeError(f"{anim} 未取到输出文件")

    async with httpx.AsyncClient(timeout=180, follow_redirects=True) as client:
        resp = await client.get(urls[0])
        resp.raise_for_status()
        out_mp4.write_bytes(resp.content)


def compose_prompt(anim: str, extra: str = "") -> str:
    """提示词直传（实测约束越少效果越好）：

    用户写了什么就发什么；没写就用该段的预设动作描述（一句话级别）。
    不再叠加角色锁定/镜头/光照/负面清单——参考图本身就是最强约束。
    """
    spec = CONFIG["runninghub"].get("prompt_spec") or {}
    actions = spec.get("actions") or {}
    if extra and extra.strip():
        return extra.strip()
    return (actions.get(anim) or actions.get("_default") or "自然待机动作").strip()


def _build_node_list(anim: str, file_name: str, extra: str = "") -> list:
    """按模板拼 nodeInfoList。占位符：{{ANIM}} {{IMAGE_NAME}} {{PROMPT}}"""
    rh = CONFIG["runninghub"]
    tpl = json.loads(json.dumps(rh.get("node_info_template", [])))
    prompt = compose_prompt(anim, extra)
    for node in tpl:
        # 兼容两种写法：单字段 fieldValue，或字段数组 fieldValues
        if isinstance(node.get("fieldValues"), list):
            targets = node["fieldValues"]
        else:
            targets = [node]
        for f in targets:
            if isinstance(f.get("fieldValue"), str):
                f["fieldValue"] = (
                    f["fieldValue"]
                    .replace("{{IMAGE_NAME}}", file_name)
                    .replace("{{ANIM}}", anim)
                    .replace("{{PROMPT}}", prompt)
                )
    bad = [
        n for n in tpl
        if not str(n.get("nodeId", "")).strip() or str(n.get("nodeId")).strip().upper() == "TODO"
    ]
    if bad:
        raise RHError(
            -1,
            "工作流节点未配置：请在右上角「管理设置」里填写 nodeId 与 fieldName（留空或 TODO 不算配好）",
        )
    return tpl


async def generate_anim(tid: str, anim: str, source: Path):
    raw = task_dir(tid) / "raw"
    ensure_dir(raw)
    mp4 = raw / f"{anim}.mp4"
    if effective_mode() == "mock":
        await _mock_generate(tid, anim, source, mp4)
    else:
        await _real_generate(tid, anim, source, mp4)
    return mp4


# ---------------------------------------------------------------- 转码

def anim_seconds(anim: str) -> float:
    """按段取时长（media.anim_seconds）。

    待机、行驶给 5 秒耐看；说话、表情 3 秒就够——这些段都是循环播，
    多出来的秒数不会带来新信息，只会白占 PSRAM 和 Flash。
    """
    m = cfg("media", "anim_seconds", default={}) or {}
    v = m.get(anim, m.get("_default"))
    try:
        return float(v)
    except (TypeError, ValueError):
        return cfgf("media.max_clip_seconds", 3.0)

def transcode_to_frames(mp4: Path, out_dir: Path, loops: bool,
                        fps=None, max_sec=None, quality=None):
    """
    MP4 → 480x480 MJPEG 帧序列。
    等比缩放后居中裁切（fit=crop）或黑边填充（fit=pad）。

    fps / max_sec / quality 传 None 时取配置值；run_pipeline 的体积预算
    降档会显式传入更小的值。

    这里刻意「不清空目录」：运行环境把 rmtree/unlink 换成了安全删除，
    批量删几百个 jpg 会被保护机制直接打断请求。改成 -frames:v 定长覆盖，
    目录里超出本次帧数的旧文件不参与打包（下次更长的段会自然覆盖掉）。
    """
    ensure_dir(out_dir)

    fps = int(fps or cfgi("media.fps", 12))
    max_sec = max_sec or cfgf("media.max_clip_seconds", 3.0)
    quality = int(quality or cfgi("media.jpeg_quality", 9))
    want = max(1, int(round(float(max_sec) * fps)))
    # 工作流出的是 15 秒长片，开头常有入画/静帧过渡，默认跳过前 0.4 秒
    start = cfgf("media.trim_start", 0.4)
    size = cfgi("media.size", 480)
    # fit=crop：放大铺满再居中裁切（主体占满屏，适合竖屏源）；
    # fit=pad：等比缩放黑边填充（完整保留画面，主体偏小）
    fit = str(cfg("media", "fit", default="crop")).lower()
    # 裁切焦点：0=顶部 0.5=居中。动物头在上半部，默认 0.35 保住耳朵
    focus = min(max(cfgf("media.crop_focus", 0.35), 0.0), 1.0)

    if fit == "pad":
        vf = (
            f"scale={size}:{size}:force_original_aspect_ratio=decrease,"
            f"pad={size}:{size}:(ow-iw)/2:(oh-ih)/2:color=0x0A0A0A"
        )
    else:
        vf = (
            f"scale={size}:{size}:force_original_aspect_ratio=increase,"
            f"crop={size}:{size}:(iw-{size})/2:(ih-{size})*{focus}"
        )
    cmd = [
        "ffmpeg", "-y", "-loglevel", "error",
        "-ss", str(start),
        "-i", str(mp4),
        "-t", str(max_sec),
        "-vf", vf,
        "-r", str(fps),
        "-frames:v", str(want),      # 定长：从这里控制段时长，不靠删目录
        "-q:v", str(quality),
        str(out_dir / "%04d.jpg"),
    ]
    subprocess.run(cmd, check=True)
    frames = sorted(out_dir.glob("*.jpg"))[:want]
    if not frames:
        raise RuntimeError(f"{mp4.name} 转码后没有帧")
    return frames


# ---------------------------------------------------------------- 资源包

MAGIC = b"PETP"
PACK_VERSION = 1
ANIM_DIR_FMT = struct.Struct("<12sBBHHII")   # 26 字节
HEADER_FMT = struct.Struct("<4sBBHHBBII")    # 20 字节


def build_pack(tid: str, anim_frames: Dict[str, List[Path]], out_path: Path, fps=None):
    """
    打包格式（小端）：

      [20B header]
        magic[4] "PETP" | version u8 | anim_count u8
        width u16 | height u16 | fps u8 | reserved u8
        header_size u32 | total_size u32
      [26B * anim_count 目录]
        name[12] | loop u8 | reserved[3] | frame_count u16 | reserved2 u16
        frame_table_offset u32 | data_offset u32
      [各动画帧表] uint32 * frame_count（每帧字节数）
      [各动画帧数据] 连续 JPEG
    """
    fps = int(fps or cfgi("media.fps", 15))
    width = height = cfgi("media.size", 480)
    loop_map = {k: (1 if v else 0) for k, v in cfg("anims", "loop", default={}).items()}

    anims = list(anim_frames.keys())
    header_size = HEADER_FMT.size + ANIM_DIR_FMT.size * len(anims)

    blobs: List[bytes] = []
    tables: List[List[int]] = []
    for name in anims:
        data = [p.read_bytes() for p in anim_frames[name]]
        blobs.append(b"".join(data))
        tables.append([len(b) for b in data])

    # 计算各段偏移
    cursor = header_size
    table_offsets, data_offsets = [], []
    for t in tables:
        table_offsets.append(cursor)
        cursor += 4 * len(t)
    for b in blobs:
        data_offsets.append(cursor)
        cursor += len(b)
    total_size = cursor

    buf = bytearray()
    buf += HEADER_FMT.pack(
        MAGIC, PACK_VERSION, len(anims), width, height, fps, 0, header_size, total_size
    )
    for i, name in enumerate(anims):
        buf += ANIM_DIR_FMT.pack(
            name.encode()[:12], loop_map.get(name, 1), 0,
            len(tables[i]), 0, table_offsets[i], data_offsets[i],
        )
    for t in tables:
        for n in t:
            buf += struct.pack("<I", n)
    for b in blobs:
        buf += b

    out_path.write_bytes(bytes(buf))
    return {
        "size": total_size,
        "anims": {n: {"frames": len(tables[i]), "bytes": len(blobs[i])} for i, n in enumerate(anims)},
    }


# ---------------------------------------------------------------- 后台流水线

async def run_pipeline(tid: str):
    t = TASKS[tid]
    try:
        t["status"] = "generating"
        save_meta(tid)

        source = task_dir(tid) / "source.jpg"
        anims: List[str] = t["anims_plan"]
        if effective_mode() == "mock":
            # mock 模式只为验证链路，生成一套即可复用
            first = await generate_anim(tid, anims[0], source)
            for a in anims[1:]:
                shutil.copy(first, task_dir(tid) / "raw" / f"{a}.mp4")
        else:
            # SHARED 类型账号有并发上限，默认 2 路，避免排队超时
            sem = asyncio.Semaphore(int(cfg("runninghub", "max_concurrent", default=2)))

            async def _one(a):
                async with sem:
                    return await generate_anim(tid, a, source)

            results = await asyncio.gather(*[_one(a) for a in anims], return_exceptions=True)
            errs = [(a, r) for a, r in zip(anims, results) if isinstance(r, Exception)]
            if errs:
                detail = "；".join(f"{a}: {e}" for a, e in errs[:3])
                raise RuntimeError(f"{len(errs)}/{len(anims)} 段生成失败 → {detail}")

        t["status"] = "packing"
        t["progress"] = 60
        save_meta(tid)

        # 体积预算降档：16MB Flash 里还要留 OTA 和资产，包不能无限长。
        # 梯度优先砍时长和帧率（视觉损失小），最后才降 JPEG 质量。
        budget = cfgf("media.budget_mb", 6.0) * 1024 * 1024
        ladder = [
            (None, None, None),
            (None, 1.0, None),
            (15, 1.0, 10),
            (12, 1.0, 12),
            (12, 0.8, 12),
        ]
        anim_frames: Dict[str, List[Path]] = {}
        used = {"fps": cfgi("media.fps", 15),
                "clip": cfgf("media.max_clip_seconds", 1.2),
                "q": cfgi("media.jpeg_quality", 8)}
        for step, (lf, lc, lq) in enumerate(ladder):
            anim_frames = {}
            for a in anims:
                mp4 = task_dir(tid) / "raw" / f"{a}.mp4"
                if not mp4.exists():
                    raise RuntimeError(f"缺少 {a}.mp4")
                anim_frames[a] = transcode_to_frames(
                    mp4, task_dir(tid) / "frames" / a,
                    bool(cfg("anims", "loop", default={}).get(a, 1)),
                    fps=lf, max_sec=lc or anim_seconds(a), quality=lq,
                )
            used = {
                "fps": lf or cfgi("media.fps", 12),
                "q": lq or cfgi("media.jpeg_quality", 11),
                # 降档时 lc 会统一压短；正常情况下按段各取各的时长
                "seconds": {a: (lc or anim_seconds(a)) for a in anims},
            }
            est = sum(sum(p.stat().st_size for p in fs) for fs in anim_frames.values())
            if est <= budget or step == len(ladder) - 1:
                break
            t["note"] = f"包体 {est/1048576:.1f}MB 超预算，已自动降到第 {step+2} 档"

        info = build_pack(tid, anim_frames, task_dir(tid) / "pet.pack", fps=used["fps"])
        info["applied"] = used
        info["budget_mb"] = round(budget / 1048576, 2)

        t["status"] = "done"
        t["progress"] = 100
        t["pack"] = info
        t["anims"] = {
            a: {
                "frames": [f"/api/frame/{tid}/{a}/{p.name}" for p in fs],
                "count": len(fs),
            }
            for a, fs in anim_frames.items()
        }
        save_meta(tid)
    except Exception as e:
        t["status"] = "failed"
        t["error"] = str(e)
        save_meta(tid)


# ---------------------------------------------------------------- 登录与会话
# 用户端 / 管理员端分离：管理员能看接口配置、余额、工作流；用户只看前台。
# 账号放在 config.json 的 auth.accounts 里，管理员可以直接改文件换密码。

SESSIONS: Dict[str, dict] = {}          # token -> {user, role, at}
SESSION_COOKIE = "pet_session"
SESSION_TTL = 7 * 24 * 3600             # 7 天

DEFAULT_ACCOUNTS = {
    "admin": {"password": "admin", "role": "admin"},
    "user": {"password": "1234", "role": "user"},
}


def accounts() -> dict:
    a = cfg("auth", "accounts", default=None)
    return a if isinstance(a, dict) and a else DEFAULT_ACCOUNTS


def _session(request: Request) -> Optional[dict]:
    s = SESSIONS.get(request.cookies.get(SESSION_COOKIE, ""))
    if s and time.time() - s.get("at", 0) > SESSION_TTL:
        SESSIONS.pop(request.cookies.get(SESSION_COOKIE, ""), None)
        return None
    return s


def require(request: Request, role: str = None) -> dict:
    s = _session(request)
    if not s:
        raise HTTPException(401, "请先登录")
    if role == "admin" and s.get("role") != "admin":
        raise HTTPException(403, "需要管理员权限")
    return s


@app.post("/api/login")
async def login(payload: dict, response: Response):
    u = str(payload.get("username") or "").strip()
    p = str(payload.get("password") or "")
    acc = accounts().get(u)
    if not acc or str(acc.get("password")) != p:
        raise HTTPException(401, "账号或密码不对")
    token = secrets.token_hex(16)
    role = str(acc.get("role") or "user")
    SESSIONS[token] = {"user": u, "role": role, "at": time.time()}
    response.set_cookie(
        SESSION_COOKIE, token,
        max_age=SESSION_TTL, samesite="lax",
    )
    return {"ok": True, "user": u, "role": role}


@app.get("/api/me")
async def me(request: Request):
    s = _session(request)
    if not s:
        raise HTTPException(401, "未登录")
    return {"ok": True, "user": s["user"], "role": s["role"]}


@app.post("/api/logout")
async def logout(request: Request, response: Response):
    SESSIONS.pop(request.cookies.get(SESSION_COOKIE, ""), None)
    response.delete_cookie(SESSION_COOKIE)
    return {"ok": True}


# ---------------------------------------------------------------- API

@app.post("/api/submit")
async def submit(
    request: Request,
    nickname: str = Form(...),
    phone: str = Form(""),
    pet_name: str = Form(...),
    pet_age: str = Form(""),
    pet_kind: str = Form("animal"),
    persona: str = Form(""),
    extra_prompt: str = Form(""),
    anim_count: int = Form(4),
    image: UploadFile = File(...),
):
    require(request)
    tid = uuid.uuid4().hex[:12]
    d = task_dir(tid)
    ensure_dir(d)

    raw = await image.read()
    if len(raw) > 8 * 1024 * 1024:
        raise HTTPException(400, "图片超过 8MB")
    (d / "source.jpg").write_bytes(raw)

    basic = cfg("anims", "basic", default=["idle", "run", "brake", "talk"])
    full = cfg("anims", "full", default=list(basic) + ["fall", "happy"])
    plan = (full if anim_count > len(basic) else basic)[: max(1, min(anim_count, len(full)))]

    TASKS[tid] = {
        "id": tid,
        "status": "queued",
        "progress": 0,
        "created": time.time(),
        "nickname": nickname,
        "phone": phone,
        "pet_name": pet_name,
        "pet_age": pet_age,
        "pet_kind": pet_kind,
        "persona": persona,
        "extra_prompt": (extra_prompt or "").strip(),
        "anims_plan": plan,
    }
    save_meta(tid)
    asyncio.create_task(run_pipeline(tid))
    return {"task_id": tid, "anims_plan": plan}


@app.get("/api/task/{tid}")
async def get_task(tid: str, request: Request):
    require(request)
    t = TASKS.get(tid)
    if not t:
        raise HTTPException(404, "任务不存在")
    return {
        "id": tid,
        "status": t["status"],
        "progress": t.get("progress", 0),
        "error": t.get("error"),
        "pet_name": t.get("pet_name"),
        "anims": t.get("anims"),
        "pack_url": f"/api/pack/{tid}.pack" if t["status"] == "done" else None,
        "pack": t.get("pack"),
    }


@app.post("/api/repack/{tid}")
async def repack(tid: str, request: Request):
    require(request)
    """
    用已生成的 raw/*.mp4 重新转码打包，不重新调用 RunningHub。

    用途：调参（帧率/时长/质量/裁切）、修 bug 后补包。视频生成是唯一花钱的
    环节，已经付过钱的结果不该因为打包失败而作废。
    """
    t = TASKS.get(tid)
    if not t:
        raise HTTPException(404, "任务不存在")
    raw = task_dir(tid) / "raw"
    anims = [a for a in t.get("anims_plan", []) if (raw / f"{a}.mp4").exists()]
    if not anims:
        raise HTTPException(400, "没有可用的原始视频，需重新生成")

    t["status"] = "packing"
    t["progress"] = 60
    t.pop("error", None)
    save_meta(tid)
    try:
        anim_frames = {}
        budget = cfgf("media.budget_mb", 6.0) * 1024 * 1024
        for a in anims:
            anim_frames[a] = transcode_to_frames(
                raw / f"{a}.mp4", task_dir(tid) / "frames" / a,
                bool(cfg("anims", "loop", default={}).get(a, 1)),
                max_sec=anim_seconds(a),
            )
        info = build_pack(tid, anim_frames, task_dir(tid) / "pet.pack")
        info["applied"] = {
            "fps": cfgi("media.fps", 12),
            "q": cfgi("media.jpeg_quality", 11),
            "seconds": {a: anim_seconds(a) for a in anims},
        }
        info["budget_mb"] = round(budget / 1048576, 2)
        t.update(
            status="done", progress=100, pack=info,
            anims={a: {"frames": [f"/api/frame/{tid}/{a}/{p.name}" for p in fs],
                       "count": len(fs)} for a, fs in anim_frames.items()},
        )
        save_meta(tid)
        return {"ok": True, "pack": info}
    except Exception as e:
        t.update(status="failed", error=f"重新打包失败：{e}")
        save_meta(tid)
        raise HTTPException(500, str(e))


@app.post("/api/regen/{tid}/{anim}")
async def regen_segment(tid: str, anim: str, request: Request, payload: dict | None = None):
    require(request)
    """
    只重生成某一段（其余段保留）。这是定制化收费的核心动作：
    用户对「骑行」不满意就重做这一条，不该为其它三条再付一次钱。

    payload 可选：
      {"prompt": "..."}        该段完整提示词（直传 RunningHub，覆盖预设）
      {"extra_prompt": "..."}  兼容旧字段，等价于 prompt
    """
    t = TASKS.get(tid)
    if not t:
        raise HTTPException(404, "任务不存在")
    if anim not in (t.get("anims_plan") or []):
        raise HTTPException(400, f"{anim} 不在这个任务的动作列表里")
    if t.get("regen") and t["regen"].get("status") == "generating":
        raise HTTPException(409, "上一段还在生成，等它结束")

    prompt = (payload or {}).get("prompt") or (payload or {}).get("extra_prompt") or ""
    t["regen"] = {"anim": anim, "status": "queued", "started": time.time()}
    save_meta(tid)
    asyncio.create_task(_regen_one(tid, anim, prompt))
    return {"ok": True, "anim": anim}


async def _regen_one(tid: str, anim: str, extra=None):
    t = TASKS[tid]
    try:
        t["regen"] = {"anim": anim, "status": "generating", "started": time.time()}
        save_meta(tid)

        # 提示词直传：只影响这一段这一次；记下来供前端回显
        saved = t.get("extra_prompt", "")
        if extra:
            t["extra_prompt"] = extra
            t.setdefault("anim_prompts", {})[anim] = extra
        await generate_anim(tid, anim, task_dir(tid) / "source.jpg")
        if extra:
            t["extra_prompt"] = saved
        t["regen"] = {"anim": anim, "status": "packing", "started": time.time()}
        save_meta(tid)

        result = await asyncio.to_thread(_repack_sync, tid)
        t.update(status="done", progress=100, pack=result["pack"], anims=result["anims"])
        t["regen"] = {"anim": anim, "status": "done", "finished": time.time()}
        t.pop("error", None)
        save_meta(tid)
    except Exception as e:
        t["regen"] = {"anim": anim, "status": "failed", "error": str(e)}
        t.pop("error", None)
        save_meta(tid)


def _repack_sync(tid: str) -> dict:
    """把某任务所有已存在的 raw 段重新转码打包（同步实现，供线程池调用）。"""
    t = TASKS[tid]
    raw = task_dir(tid) / "raw"
    anims = [a for a in t["anims_plan"] if (raw / f"{a}.mp4").exists()]
    anim_frames = {}
    for a in anims:
        anim_frames[a] = transcode_to_frames(
            raw / f"{a}.mp4", task_dir(tid) / "frames" / a,
            bool((cfg("anims", "loop", default={}) or {}).get(a, 1)),
            max_sec=anim_seconds(a),
        )
    info = build_pack(tid, anim_frames, task_dir(tid) / "pet.pack")
    return {
        "pack": info,
        "anims": {a: {"frames": [f"/api/frame/{tid}/{a}/{p.name}" for p in fs], "count": len(fs)}
                  for a, fs in anim_frames.items()},
    }


@app.get("/api/frame/{tid}/{anim}/{name}")
async def get_frame(tid: str, anim: str, name: str):
    if not re.fullmatch(r"\w+", anim) or not re.fullmatch(r"\d{4}\.jpg", name):
        raise HTTPException(400, "非法路径")
    p = task_dir(tid) / "frames" / anim / name
    if not p.exists():
        raise HTTPException(404)
    return FileResponse(p, media_type="image/jpeg")


@app.get("/api/pack/{tid}.pack")
async def get_pack(tid: str):
    p = task_dir(tid) / "pet.pack"
    if not p.exists():
        raise HTTPException(404, "资源包未生成")
    return FileResponse(p, media_type="application/octet-stream", filename=f"{tid}.pack")


@app.get("/api/tasks")
async def list_tasks(request: Request):
    require(request)
    items = sorted(TASKS.values(), key=lambda x: x.get("created", 0), reverse=True)[:50]
    return [
        {"id": i["id"], "pet_name": i.get("pet_name"), "status": i["status"],
         "created": i.get("created")}
        for i in items
    ]


# ---------------------------------------------------------------- 宠物库
# 「生成好的东西挂在网页上，而不是写死在系统里」——这里把 storage 里每一只
# 已完成的宠物读出来给前端列表用，封面直接取待机第一帧。

@app.get("/api/gallery")
async def gallery(request: Request):
    require(request)
    items = []
    for t in TASKS.values():
        if t.get("status") != "done":
            continue
        d = task_dir(t["id"])
        if not (d / "pet.pack").exists():
            continue
        anims = list((t.get("anims") or {}).keys())
        cover_anim = "idle" if "idle" in anims else (anims[0] if anims else "")
        cover = ""
        if cover_anim and (d / "frames" / cover_anim / "0001.jpg").exists():
            cover = f"/api/frame/{t['id']}/{cover_anim}/0001.jpg"
        items.append({
            "id": t["id"],
            "pet_name": t.get("pet_name") or "未命名",
            "kind": t.get("pet_kind") or "animal",
            "created": t.get("created", 0),
            "cover": cover,
            "anims": anims,
            "frames": sum(v.get("count", 0) for v in (t.get("anims") or {}).values()),
            "size": (t.get("pack") or {}).get("size", 0),
            "fps": (t.get("pack") or {}).get("applied", {}).get("fps"),
        })
    items.sort(key=lambda x: x["created"], reverse=True)
    return items


# ---------------------------------------------------------------- 同步到设备
# 设备侧契约（ESP32 端实现同样的三个接口即可）：
#   GET  /api/info        → {"ok":true,"name":"...","fw":"...","free_kb":N}
#   POST /api/pet         → 原始二进制 body 是 .pack，返回 {"ok":true,"size":N,"sha256":"..."}
#   GET  /api/pet/meta    → 当前包里宠物名/段数，用于回读校验

async def _device_request(host: str, port: int, path: str, *, body=None, timeout=20.0):
    url = f"http://{host}:{int(port)}{path}"
    headers = {"Content-Type": "application/octet-stream"} if body is not None else {}
    async with httpx.AsyncClient(timeout=timeout) as c:
        r = await c.request("POST" if body is not None else "GET", url, content=body, headers=headers)
    try:
        data = r.json()
    except Exception:
        data = {"raw": r.text[:300]}
    return r.status_code, data


@app.get("/api/device/ping")
async def device_ping(request: Request, host: str, port: int = 80, path: str = "/api/info"):
    require(request)
    if not host:
        raise HTTPException(400, "请填写设备地址")
    try:
        code, data = await _device_request(host, port, path)
    except Exception as e:
        return {"ok": False, "error": f"连不上 {host}:{port} —— {e}"}
    return {"ok": code == 200, "http": code, "device": data}


@app.post("/api/sync/{tid}")
async def sync_to_device(tid: str, request: Request, payload: dict):
    """
    把指定宠物的 .pack 推给 ESP32。

    body: {host, port, path, name}
    推完做一次大小与 sha256 校验，避免「显示成功但设备只写了一半」。
    """
    require(request)
    t = TASKS.get(tid)
    if not t:
        raise HTTPException(404, "任务不存在")
    pack = task_dir(tid) / "pet.pack"
    if not pack.exists():
        raise HTTPException(400, "资源包还没生成，先点「重新打包」")

    host = str(payload.get("host") or "").strip()
    port = int(payload.get("port") or 80)
    path = str(payload.get("path") or "/api/pet")
    if not host:
        raise HTTPException(400, "请填写设备地址（ESP32 的 IP）")

    raw = pack.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    try:
        code, data = await _device_request(host, port, path, body=raw, timeout=90.0)
    except Exception as e:
        return {"ok": False, "error": f"推送失败：{e}"}

    ok = code == 200 and (data.get("ok") is not False)
    verified = None
    if ok and isinstance(data, dict) and data.get("sha256"):
        verified = str(data["sha256"]).lower() == digest
    t.setdefault("syncs", []).append({
        "at": int(time.time()), "host": host, "port": port,
        "size": len(raw), "sha256": digest, "ok": bool(ok), "verified": verified,
    })
    save_meta(tid)
    return {
        "ok": bool(ok), "http": code, "bytes": len(raw), "sha256": digest,
        "verified": verified, "device": data,
        "error": None if ok else f"设备返回 HTTP {code}：{data}",
    }


# -------- 虚拟设备：没有硬件时先把同步链路跑通（固件做好后换成真机 IP 即可）

VDEV = STORAGE / "vdev"


@app.get("/vdev/api/info")
async def vdev_info():
    f = VDEV / "pet.pack"
    return {
        "ok": True, "name": "虚拟设备（web 模拟）", "fw": "vdev-1",
        "has_pet": f.exists(), "size": f.stat().st_size if f.exists() else 0,
    }


@app.post("/vdev/api/pet")
async def vdev_upload(request: Request):
    body = await request.body()
    if len(body) < 20 or body[:4] != MAGIC:
        raise HTTPException(400, "不是合法的 PETP 资源包")
    ensure_dir(VDEV)
    (VDEV / "pet.pack").write_bytes(body)
    return {"ok": True, "size": len(body), "sha256": hashlib.sha256(body).hexdigest()}


@app.get("/vdev/api/pet")
async def vdev_download():
    f = VDEV / "pet.pack"
    if not f.exists():
        raise HTTPException(404)
    return FileResponse(f, media_type="application/octet-stream")


# -------- 设备拉包：固定地址，板子不需要知道 task_id
# 网页上生成/同步好的最新一只宠物就是「当前宠物」，固件开机拉这个地址即可：
#   pet_anim_pull("http://<服务器>:8080/api/pet/latest.pack", "/spiffs/pet.pack");

def _latest_done() -> Optional[dict]:
    done = [
        t for t in TASKS.values()
        if t.get("status") == "done" and (task_dir(t["id"]) / "pet.pack").exists()
    ]
    return max(done, key=lambda t: t.get("created", 0)) if done else None


@app.get("/api/pet/latest.pack")
async def pet_latest_pack():
    t = _latest_done()
    if not t:
        raise HTTPException(404, "还没有生成好的资源包")
    return FileResponse(
        task_dir(t["id"]) / "pet.pack",
        media_type="application/octet-stream", filename="pet.pack",
    )


# ---------------- 开机音效 ----------------
# 生成动画用的视频本身带音轨（RunningHub 出来的 mp4 一般有 AAC 音频）。
# 这里从 idle 源视频里截前 N 秒，转成 OGG/Opus（单声道 24kHz 32kbps），
# 固件开机时读出来喂给小智的 AudioService::PlaySound() 播一次。
# 选 Opus 而不是 PCM：3 秒 24kHz 单声道 PCM 是 144KB，Opus 只要 ~58KB，
# SPIFFS 分区里 pack 已经占了 1.96MB，省一半空间更稳。

def boot_sound_file(tid: str):
    """返回该任务的 boot.ogg 路径；不存在就当场从源视频转一个。没有音轨返回 None。"""
    d = task_dir(tid)
    ogg = d / "boot.ogg"
    if ogg.exists() and ogg.stat().st_size > 0:
        return ogg

    raw = d / "raw"
    # 优先 idle（开机就是待机画面），没有就用任意带音轨的段
    cands = [raw / "idle.mp4"] + sorted(raw.glob("*.mp4"))
    seconds = float(cfgi("media.boot_sound_seconds", 3))
    for src in cands:
        if not src.exists():
            continue
        # 先确认有音轨，避免 ffmpeg 白跑
        try:
            probe = subprocess.run(
                ["ffprobe", "-v", "error", "-select_streams", "a",
                 "-show_entries", "stream=codec_type", "-of", "csv=p=0", str(src)],
                capture_output=True, text=True, timeout=20,
            )
            if "audio" not in (probe.stdout or ""):
                continue
        except Exception:
            continue
        try:
            subprocess.run(
                ["ffmpeg", "-y", "-loglevel", "error",
                 "-i", str(src), "-t", str(seconds), "-vn",
                 "-ac", "1", "-ar", "24000",
                 "-c:a", "libopus", "-b:a", "32k",
                 str(ogg)],
                check=True, timeout=60,
            )
        except Exception as e:
            print(f"[boot_sound] {src.name} 转码失败: {e}")
            continue
        if ogg.exists() and ogg.stat().st_size > 0:
            print(f"[boot_sound] 从 {src.name} 生成开机音效 {ogg.stat().st_size} 字节")
            return ogg
    return None


@app.get("/api/pet/latest.ogg")
async def pet_latest_ogg():
    t = _latest_done()
    if not t:
        raise HTTPException(404, "还没有生成好的资源包")
    ogg = boot_sound_file(t["id"])
    if not ogg:
        raise HTTPException(404, "这个宠物没有可用音轨")
    return FileResponse(ogg, media_type="audio/ogg", filename="boot.ogg")


@app.get("/api/pet/latest")
async def pet_latest_info():
    """设备/网页都能用它确认「当前宠物是哪一只、多大、校验对不对」。"""
    t = _latest_done()
    if not t:
        raise HTTPException(404, "还没有生成好的资源包")
    p = task_dir(t["id"]) / "pet.pack"
    raw = p.read_bytes()
    pack = t.get("pack") or {}
    info = {
        "task_id": t["id"],
        "pet_name": t.get("pet_name"),
        "size": len(raw),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "fps": (pack.get("applied") or {}).get("fps"),
        "anims": {k: v.get("count") for k, v in (t.get("anims") or {}).items()},
    }
    # 音效信息（懒生成：第一次问就会去转，之后有缓存）
    try:
        ogg = boot_sound_file(t["id"])
        if ogg:
            info["sound_size"] = ogg.stat().st_size
            info["sound_sha256"] = hashlib.sha256(ogg.read_bytes()).hexdigest()
    except Exception:
        pass
    return info


# ---------------- 设备状态上报（联调观测通道） ----------------
# 设固件每 ~3.3s POST 一次运行状态：当前段/运动状态/说话/IMU sigma。
# 用途：摇动不切、说话不切这类问题不用串口就能远程定位。
PET_REPORT: dict = {"ts": 0.0}


@app.post("/api/pet/report")
async def pet_report(request: Request):
    try:
        body = await request.json()
    except Exception:
        raise HTTPException(400, "需要 JSON body")
    if not isinstance(body, dict):
        raise HTTPException(400, "需要 JSON object")
    PET_REPORT.clear()
    PET_REPORT.update(body)
    PET_REPORT["ts"] = time.time()
    return {"ok": True}


@app.get("/api/pet/status")
async def pet_status():
    """最近一次设备上报的状态；ts 距今超过 15s 视为设备离线/未上报。"""
    out = dict(PET_REPORT)
    out["online"] = (time.time() - out.get("ts", 0)) < 15
    return out


# 远程手动触发：把「画面能不能切」与「IMU / 说话检测通不通」分开验证。
# motion: -1 恢复自动 / 0 idle / 1 run / 2 brake / 3 fall
PET_CMD: dict = {"motion": -1, "talking": False}


@app.get("/api/pet/cmd")
async def pet_cmd_get():
    return PET_CMD


@app.post("/api/pet/cmd")
async def pet_cmd_set(request: Request):
    try:
        body = await request.json()
    except Exception:
        raise HTTPException(400, "需要 JSON body")
    if "motion" in body:
        PET_CMD["motion"] = int(body["motion"])
    if "talking" in body:
        PET_CMD["talking"] = bool(body["talking"])
    return PET_CMD


@app.post("/api/reload-config")
async def reload_config(request: Request):
    require(request, role="admin")
    global CONFIG
    CONFIG = load_config()
    return {"ok": True, "mode": effective_mode()}


# 用户端也要在同步弹窗里带出上次保存的设备地址，但只给 device 段，
# 不暴露 runninghub 配置。设备地址属于用户自己的车机，不算运营机密。
@app.get("/api/device-defaults")
async def device_defaults(request: Request):
    require(request)
    return {"device": CONFIG.get("device", {})}


# ---------------------------------------------------------------- 配置读写（管理员）
# 设计原则：所有可变参数都由网页表单维护，代码里不留硬编码。
# 前端只提交「改了的部分」，后端做深合并；api_key 回显时打码，提交时留空或含 **** 表示不修改。

MASK = "****"
EDITABLE = ("runninghub", "media", "generation", "anims", "device")
SECRET_FIELDS = (("runninghub", "api_key"),)


def _deep_merge(dst: dict, src: dict) -> dict:
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            _deep_merge(dst[k], v)
        else:
            dst[k] = v
    return dst


def _masked_config() -> dict:
    pub = json.loads(json.dumps(CONFIG))
    for path in SECRET_FIELDS:
        node = pub
        for k in path[:-1]:
            node = node.get(k, {}) if isinstance(node, dict) else {}
        if isinstance(node, dict) and node.get(path[-1]):
            v = str(node[path[-1]])
            node[path[-1]] = (v[:4] + MASK + v[-4:]) if len(v) > 8 else MASK
    return pub


def _is_masked(v) -> bool:
    return (not v) or (MASK in str(v))


@app.get("/api/config")
async def get_cfg_api(request: Request):
    require(request, role="admin")
    c = rh_conf()
    return {
        "config": _masked_config(),
        "mode": effective_mode(),
        "ready": rh_ready(),
        "key_set": bool(c["key"]),
        "workflow_id": c["workflow"],
        "api_version": c["version"],
    }


@app.post("/api/config")
async def set_cfg_api(request: Request, payload: dict):
    require(request, role="admin")
    global CONFIG
    # 先把打码的密钥还原成真值，避免整体覆盖时被写成 ****
    body = json.loads(json.dumps(payload))
    for path in SECRET_FIELDS:
        node = body
        for k in path[:-1]:
            node = node.get(k) if isinstance(node, dict) else None
        if isinstance(node, dict) and _is_masked(node.get(path[-1])):
            node.pop(path[-1], None)

    merged = json.loads(json.dumps(CONFIG))
    for k in EDITABLE:
        if isinstance(body.get(k), dict):
            _deep_merge(merged[k], body[k])

    # 数字字段做一次收敛，防止前端传字符串进 config
    rh = merged.get("runninghub", {})
    try:
        rh["workflow_id"] = str(rh.get("workflow_id", "")).strip()
        rh["poll_interval"] = float(rh.get("poll_interval", 5))
        rh["timeout_seconds"] = float(rh.get("timeout_seconds", 900))
        rh["max_concurrent"] = int(rh.get("max_concurrent", 2))
    except (TypeError, ValueError):
        raise HTTPException(400, "数值字段格式不对")

    # media 段同理：前端表单一律是字符串，这里存成真数字，
    # 避免 struct.pack 收到 "15" 这类值时报 not an integer
    md = merged.get("media", {})
    for k in ("size", "fps", "jpeg_quality"):
        if k in md:
            try:
                md[k] = int(float(md[k]))
            except (TypeError, ValueError):
                raise HTTPException(400, f"media.{k} 必须是整数")
    for k in ("max_clip_seconds", "trim_start", "crop_focus", "budget_mb", "loop_crossfade"):
        if k in md:
            try:
                md[k] = float(md[k])
            except (TypeError, ValueError):
                raise HTTPException(400, f"media.{k} 必须是数字")

    tmp = CONFIG_PATH.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(merged, ensure_ascii=False, indent=2), encoding="utf-8")
    tmp.replace(CONFIG_PATH)
    CONFIG = merged

    return {
        "ok": True,
        "mode": effective_mode(),
        "ready": rh_ready(),
        "restart_needed": False,
    }


@app.get("/api/prompt-preview")
async def prompt_preview(request: Request, anim: str = "idle", extra: str = ""):
    """给管理员看最终要发给模型的完整提示词。"""
    require(request, role="admin")
    return {"anim": anim, "extra": extra, "prompt": compose_prompt(anim, extra)}


@app.get("/api/rh/status")
async def rh_status_api(request: Request):
    """诊断用：key 是否有效、余额多少、workflowId 是否配好。不消耗生成额度。

    余额、工作流 ID 属于运营信息，只对管理员开放；用户端不显示这些。"""
    require(request, role="admin")
    c = rh_conf()
    out = {
        "mode": effective_mode(),
        "key_set": bool(c["key"]),
        "key_tail": c["key"][-6:] if c["key"] else "",
        "workflow_id": c["workflow"],
        "base_url": c["base"],
        "ready": rh_ready(),
    }
    if not c["key"]:
        out["error"] = "未配置 api_key"
        return out
    try:
        async with httpx.AsyncClient(timeout=20) as client:
            acc = await rh_account(client)
        out.update(
            coins=acc.get("remainCoins"),
            money=acc.get("remainMoney"),
            currency=acc.get("currency"),
            api_type=acc.get("apiType"),
            running_tasks=acc.get("currentTaskCounts"),
        )
    except Exception as e:
        out["error"] = str(e)
    return out


app.mount("/", StaticFiles(directory=BASE / "static", html=True), name="static")


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=cfg("server", "port", default=8080))
