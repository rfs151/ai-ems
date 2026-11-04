"""云端（模拟云平台 / VPP 调度）

- 订阅边缘网关上行：ems/<site>/{meta,telemetry,event,events,status,cmd_ack}
- 下发指令：ems/<site>/cmd/<type>（带编号和过期时间），收到 cmd_ack 后计算往返时延
- 场景控制（频率扰动、故障注入）：sim/scenario，只发给设备仿真，不经过网关
- 内存保留最近 30 分钟遥测、最近 200 条事件，通过 WebSocket 推给浏览器
- 无人操作时自动演示：定期下发 mFRR 指令；长时间无人操作时恢复初始场景

    MQTT_HOST=localhost uvicorn app:app --port 8095
"""
from __future__ import annotations

import asyncio
import json
import logging
import math
import os
import random
import time
import uuid
from collections import deque
from pathlib import Path

import paho.mqtt.client as mqtt
from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

log = logging.getLogger("cloud")
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")

MQTT_HOST = os.environ.get("MQTT_HOST", "localhost")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
HISTORY_S = 30 * 60
WEB_DIST = Path(os.environ.get("WEB_DIST", Path(__file__).parent.parent / "web" / "dist"))
CMD_TTL_MS = 10_000            # 指令过期时间：网关收到时已过期则拒绝执行
ACK_WAIT_S = 3.0
RATE_LIMIT_S = 3.0             # 每个 IP 两次操作的最小间隔
AUTO_IDLE_S = 180              # 无人操作多久后开始自动演示
AUTO_EVERY_S = 150             # 自动演示的指令间隔
RESET_IDLE_S = 600             # 无人操作多久后恢复初始场景
DEVICES = {"pcs1", "pcs2", "bat1", "bat2", "meter", "dio"}


def now_ms() -> int:
    return int(time.time() * 1000)


class State:
    def __init__(self) -> None:
        self.site = "demo01"
        self.meta: dict | None = None
        self.status = "unknown"
        self.latest: dict | None = None
        self.history: deque[dict] = deque(maxlen=HISTORY_S)
        self.events: deque[dict] = deque(maxlen=200)
        self.cmds: deque[dict] = deque(maxlen=50)
        self.waiters: dict[str, asyncio.Future] = {}
        self.last_rx = 0.0
        self.last_user = 0.0             # 最近一次访客操作
        self.last_auto = 0.0
        self.auto_dir = 1
        self.scenario_dirty = False      # 访客改过场景（故障、急停等），需要定期恢复
        self.rate: dict[str, float] = {}
        self.clients: set[WebSocket] = set()

    def init_message(self) -> dict:
        return {
            "type": "init",
            "meta": self.meta,
            "status": self.status,
            "history": [slim(t) for t in list(self.history)[-600:]],
            "latest": self.latest,
            "events": list(self.events),
            "cmds": list(self.cmds),
        }


def slim(t: dict) -> dict:
    """历史曲线只需要少数几个量。"""
    v = t.get("v", {})
    keys = ("pcs1.p", "pcs2.p", "meter.p", "meter.f", "bat1.soc", "bat2.soc")
    out = {"ts": t["ts"], "v": {k: v[k] for k in keys if k in v}}
    if "ctl" in t:
        out["ctl"] = {k: t["ctl"].get(k) for k in ("p_base", "p_cmd", "p_mfrr", "p_fcr", "p_lfsmo", "p_req")}
    return out


state = State()
app = FastAPI(title="EMS Cloud", docs_url=None, redoc_url=None)
loop: asyncio.AbstractEventLoop | None = None
client: mqtt.Client | None = None


async def broadcast(msg: dict) -> None:
    if not state.clients:
        return
    data = json.dumps(msg, ensure_ascii=False, separators=(",", ":"))
    dead = []
    for ws in list(state.clients):
        try:
            await asyncio.wait_for(ws.send_text(data), timeout=2)
        except Exception:  # noqa: BLE001  慢客户端或已断开，直接移除
            dead.append(ws)
    for ws in dead:
        state.clients.discard(ws)


def push(msg: dict) -> None:
    """从 MQTT 线程安全地广播。"""
    if loop:
        asyncio.run_coroutine_threadsafe(broadcast(msg), loop)


# ------------------------------------------------------------------ 上行
def on_ack(data: dict) -> None:
    rec = next((c for c in state.cmds if c["id"] == data.get("id")), None)
    if rec is None:
        return
    rec.update(ok=bool(data.get("ok")), msg=data.get("msg", ""), ack_ts=now_ms(),
               edge_ms=data.get("edge_ms"))
    rec["rtt_ms"] = rec["ack_ts"] - rec["sent_ts"]
    push({"type": "cmd", "data": rec})
    fut = state.waiters.pop(rec["id"], None)
    if fut and loop:
        loop.call_soon_threadsafe(lambda: fut.done() or fut.set_result(rec))


def handle(topic: str, payload: bytes) -> None:
    parts = topic.split("/")
    if len(parts) < 3:
        return
    state.site, kind = parts[1], parts[-1]
    state.last_rx = time.time()
    if kind == "status":
        state.status = payload.decode(errors="replace")
        push({"type": "status", "status": state.status})
        return
    try:
        data = json.loads(payload)
    except ValueError:
        log.warning("非法 JSON：%s", topic)
        return
    if kind == "meta":
        state.meta = data
        push({"type": "meta", "meta": data})
    elif kind == "telemetry":
        state.latest = data
        state.history.append(data)
        push({"type": "telemetry", "data": data})
    elif kind == "event":
        if any(e.get("id") == data.get("id") for e in state.events):
            return
        state.events.append(data)
        push({"type": "event", "data": data})
    elif kind == "events":
        # 边缘侧最近事件快照：合并缺失的（云端重启后据此恢复）
        known = {e.get("id") for e in state.events}
        missing = [e for e in data if e.get("id") not in known]
        if missing:
            merged = sorted([*state.events, *missing], key=lambda e: e.get("id", 0))
            state.events.clear()
            state.events.extend(merged[-state.events.maxlen:])
            push({"type": "events", "data": list(state.events)})
    elif kind == "cmd_ack":
        on_ack(data)


def start_mqtt() -> mqtt.Client:
    def on_connect(c, _u, _f, rc, _p=None):
        log.info("MQTT 已连接 %s:%s（rc=%s）", MQTT_HOST, MQTT_PORT, rc)
        c.subscribe([("ems/+/meta", 1), ("ems/+/telemetry", 0), ("ems/+/event", 1),
                     ("ems/+/events", 1), ("ems/+/status", 1), ("ems/+/cmd_ack", 1)])

    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"ems-cloud-{os.getpid()}")
    c.on_connect = on_connect
    c.on_message = lambda _c, _u, m: handle(m.topic, m.payload)
    c.reconnect_delay_set(1, 10)
    c.connect_async(MQTT_HOST, MQTT_PORT, keepalive=30)
    c.loop_start()
    return c


# ------------------------------------------------------------------ 下行
async def send_cmd(kind: str, body: dict, summary: str, source: str) -> dict:
    cid = f"{'A' if source == 'auto' else 'C'}{uuid.uuid4().hex[:7]}"
    rec = {"id": cid, "type": kind, "summary": summary, "source": source, "sent_ts": now_ms(),
           "ok": None, "msg": "", "rtt_ms": None}
    state.cmds.append(rec)
    fut = asyncio.get_running_loop().create_future()
    state.waiters[cid] = fut
    payload = {**body, "id": cid, "exp": rec["sent_ts"] + CMD_TTL_MS}
    client.publish(f"ems/{state.site}/cmd/{kind}", json.dumps(payload), qos=1)
    await broadcast({"type": "cmd", "data": rec})
    try:
        return await asyncio.wait_for(fut, ACK_WAIT_S)
    except asyncio.TimeoutError:
        state.waiters.pop(cid, None)
        rec.update(ok=False, msg="网关未在 3 秒内应答")
        await broadcast({"type": "cmd", "data": rec})
        return rec


def scenario(msg: dict) -> None:
    client.publish("sim/scenario", json.dumps(msg), qos=1)


def client_ip(req: Request) -> str:
    fwd = req.headers.get("x-forwarded-for")
    return fwd.split(",")[0].strip() if fwd else (req.client.host if req.client else "?")


def check_rate(req: Request) -> JSONResponse | None:
    ip, t = client_ip(req), time.time()
    if t - state.rate.get(ip, 0) < RATE_LIMIT_S:
        return JSONResponse({"ok": False, "msg": "操作太频繁，请 3 秒后再试"}, status_code=429)
    state.rate[ip] = t
    if len(state.rate) > 5000:
        state.rate.clear()
    state.last_user = t
    return None


class MfrrReq(BaseModel):
    dir: int = Field(ge=-1, le=1)
    power: float = Field(ge=50, le=800)
    activation: str = "direct"           # direct / scheduled
    duration_s: int = Field(ge=30, le=600)


class CancelReq(BaseModel):
    target: str = Field(max_length=40)


class ModeReq(BaseModel):
    mode: str


class FcrReq(BaseModel):
    enabled: bool


class ScenarioReq(BaseModel):
    action: str
    delta_hz: float | None = None
    device: str | None = None
    down: bool | None = None
    name: str | None = None
    value: bool | None = None


@app.post("/api/cmd/mfrr")
async def cmd_mfrr(body: MfrrReq, req: Request):
    if (r := check_rate(req)) is not None:
        return r
    if body.dir not in (1, -1):
        return JSONResponse({"ok": False, "msg": "方向无效"}, status_code=400)
    scheduled = body.activation == "scheduled"
    cmd = {"dir": body.dir, "power": body.power, "scheduled": scheduled, "duration_s": body.duration_s}
    when = "立即"
    if scheduled:
        window = int(((state.latest or {}).get("ctl", {}).get("cfg", {}) or {}).get("mfrr_window_s", 60)) * 1000
        cmd["t_start"] = math.ceil((now_ms() + 10_000) / window) * window   # 下一个窗口边界（至少提前 10 秒）
        when = time.strftime("%H:%M:%S", time.localtime(cmd["t_start"] / 1000)) + " 开始"
    summary = f"mFRR {'上调' if body.dir > 0 else '下调'} {body.power:.0f} kW，{when}，交付 {body.duration_s} 秒"
    return await send_cmd("mfrr", cmd, summary, "visitor")


@app.post("/api/cmd/mfrr_cancel")
async def cmd_cancel(body: CancelReq, req: Request):
    if (r := check_rate(req)) is not None:
        return r
    return await send_cmd("mfrr_cancel", {"target": body.target}, f"取消 mFRR {body.target}", "visitor")


@app.post("/api/cmd/mode")
async def cmd_mode(body: ModeReq, req: Request):
    if (r := check_rate(req)) is not None:
        return r
    name = {"vpp": "VPP 调度", "schedule": "计划曲线"}.get(body.mode)
    if not name:
        return JSONResponse({"ok": False, "msg": "未知模式"}, status_code=400)
    return await send_cmd("mode", {"mode": body.mode}, f"切换模式：{name}", "visitor")


@app.post("/api/cmd/fcr")
async def cmd_fcr(body: FcrReq, req: Request):
    if (r := check_rate(req)) is not None:
        return r
    return await send_cmd("fcr", {"enabled": body.enabled}, f"FCR {'投入' if body.enabled else '退出'}", "visitor")


@app.post("/api/scenario")
async def post_scenario(body: ScenarioReq, req: Request):
    if (r := check_rate(req)) is not None:
        return r
    a = body.action
    if a == "freq_step" and body.delta_hz is not None and abs(body.delta_hz) <= 0.4:
        scenario({"action": a, "delta_hz": body.delta_hz, "recover_s": 45})
        text = f"电网频率阶跃 {body.delta_hz:+.2f} Hz"
    elif a == "comm" and body.device in DEVICES and body.down is not None:
        scenario({"action": a, "device": body.device, "down": body.down})
        text = f"{body.device} 通信{'中断' if body.down else '恢复'}"
    elif a == "di" and body.name in ("estop", "fire", "door") and body.value is not None:
        scenario({"action": a, "name": body.name, "value": body.value})
        text = f"DI {body.name} = {int(body.value)}"
    elif a == "reset":
        scenario({"action": "reset"})
        text = "恢复初始场景"
    else:
        return JSONResponse({"ok": False, "msg": "不支持的场景操作"}, status_code=400)
    if a != "freq_step":
        state.scenario_dirty = a != "reset"
    log.info("场景：%s（%s）", text, client_ip(req))
    return {"ok": True, "msg": text}


# ------------------------------------------------------------------ 自动演示
async def autopilot() -> None:
    await asyncio.sleep(20)
    while True:
        await asyncio.sleep(10)
        try:
            t = time.time()
            idle = t - state.last_user
            ctl = (state.latest or {}).get("ctl", {})
            live = [a for a in ctl.get("mfrr", []) if a.get("phase") != "结束"]
            if idle > AUTO_IDLE_S and not live and t - state.last_auto > AUTO_EVERY_S and state.status == "online":
                state.last_auto = t
                power = random.choice([200, 250, 300, 350, 400])
                d = state.auto_dir
                state.auto_dir = -d
                await send_cmd("mfrr", {"dir": d, "power": power, "scheduled": False, "duration_s": 90},
                               f"mFRR {'上调' if d > 0 else '下调'} {power} kW，立即，交付 90 秒", "auto")
            if idle > RESET_IDLE_S and state.scenario_dirty:
                state.scenario_dirty = False
                scenario({"action": "reset"})
                if ctl.get("mode") != "vpp":
                    await send_cmd("mode", {"mode": "vpp"}, "切换模式：VPP 调度", "auto")
                if not ctl.get("fcr_enabled", True):
                    await send_cmd("fcr", {"enabled": True}, "FCR 投入", "auto")
                log.info("长时间无人操作，已恢复初始场景")
        except Exception:  # noqa: BLE001  自动演示出错不影响主流程
            log.exception("自动演示出错")


# ------------------------------------------------------------------ 其他接口
@app.on_event("startup")
async def startup() -> None:
    global loop, client
    loop = asyncio.get_running_loop()
    client = start_mqtt()
    app.state.auto = asyncio.create_task(autopilot())


@app.on_event("shutdown")
async def shutdown() -> None:
    client.loop_stop()


@app.get("/api/health")
def health():
    fresh = time.time() - state.last_rx < 10
    return JSONResponse({"ok": fresh, "edge": state.status}, status_code=200 if fresh else 503)


@app.get("/api/meta")
def get_meta():
    return state.meta or {}


@app.get("/api/history")
def get_history(seconds: int = 300):
    seconds = max(10, min(HISTORY_S, seconds))
    return [slim(t) for t in list(state.history)[-seconds:]]


@app.get("/api/events")
def get_events():
    return list(state.events)


@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket):
    await ws.accept()
    await ws.send_text(json.dumps(state.init_message(), ensure_ascii=False, separators=(",", ":")))
    state.clients.add(ws)
    try:
        while True:
            await ws.receive_text()      # 只用于保活
    except WebSocketDisconnect:
        pass
    finally:
        state.clients.discard(ws)


if WEB_DIST.is_dir():
    app.mount("/", StaticFiles(directory=WEB_DIST, html=True), name="web")
