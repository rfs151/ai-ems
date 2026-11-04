"""设备仿真入口：每台设备一个 Modbus TCP 服务，物理模型 20Hz 步进。

场景控制（频率扰动、通信中断、DI 变位）走 MQTT 主题 sim/scenario，与 EMS 数据通道无关，
只供演示页面的"故障注入"使用。

    python -m sim                       # 默认监听 0.0.0.0:15021-15026
    SIM_HOST=127.0.0.1 MQTT_HOST=localhost python -m sim
"""
from __future__ import annotations

import asyncio
import json
import logging
import os
import time

from pymodbus.datastore import ModbusServerContext
from pymodbus.server import ModbusTcpServer

from .devices import ALL
from .plant import Plant

log = logging.getLogger("sim")
STEP_S = 0.05


def apply_scenario(plant: Plant, msg: dict) -> None:
    """场景指令：{"action": "freq_step", "delta_hz": 0.3, "recover_s": 60} 等。"""
    act = msg.get("action")
    if act == "freq_step":
        delta = max(-0.5, min(0.5, float(msg.get("delta_hz", 0))))
        plant.freq.inject(delta, float(msg.get("recover_s", 60)))
    elif act == "comm":
        dev, down = str(msg.get("device")), bool(msg.get("down"))
        (plant.comm_down.add if down else plant.comm_down.discard)(dev)
    elif act == "block":
        dev, bad = str(msg.get("device")), bool(msg.get("bad"))
        (plant.bad_block.add if bad else plant.bad_block.discard)(dev)
    elif act == "di":
        key = msg.get("name")
        if key in plant.di:
            plant.di[key] = bool(msg.get("value"))
    elif act == "reset":
        fresh = Plant()
        plant.bat1.soc, plant.bat2.soc = fresh.bat1.soc, fresh.bat2.soc
        plant.freq.disturbance = 0.0
        plant.comm_down.clear()
        plant.bad_block.clear()
        plant.di.update(fresh.di)
        plant.do.update(fresh.do)
        for p in (plant.pcs1, plant.pcs2):
            p.running, p.fault_code = True, 0
    log.info("scenario %s", msg)


def start_scenario_listener(plant: Plant, loop: asyncio.AbstractEventLoop) -> None:
    host = os.environ.get("MQTT_HOST")
    if not host:
        log.info("未设置 MQTT_HOST，场景控制关闭")
        return
    import paho.mqtt.client as mqtt

    def on_connect(client, _userdata, _flags, _rc, _props=None):
        client.subscribe("sim/scenario", qos=1)

    def on_message(_client, _userdata, m):
        try:
            msg = json.loads(m.payload)
        except ValueError:
            return
        loop.call_soon_threadsafe(apply_scenario, plant, msg)

    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="device-sim")
    c.on_connect, c.on_message = on_connect, on_message
    c.connect_async(host, int(os.environ.get("MQTT_PORT", 1883)))
    c.loop_start()


async def physics(plant: Plant, devices) -> None:
    last = time.monotonic()
    while True:
        await asyncio.sleep(STEP_S)
        now = time.monotonic()
        plant.step(min(now - last, 0.5))
        last = now
        for d in devices:
            d.refresh()


async def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    logging.getLogger("pymodbus").setLevel(logging.WARNING)
    host = os.environ.get("SIM_HOST", "0.0.0.0")
    plant = Plant()
    devices = [cls(plant) for cls in ALL]
    for d in devices:
        d.refresh()
    start_scenario_listener(plant, asyncio.get_running_loop())

    servers = []
    for d in devices:
        ctx = ModbusServerContext(slaves=d.context(), single=True)
        srv = ModbusTcpServer(ctx, address=(host, d.port))
        servers.append(asyncio.create_task(srv.serve_forever()))
        log.info("%-6s Modbus TCP %s:%d", d.name, host, d.port)
    await asyncio.gather(physics(plant, devices), *servers)


if __name__ == "__main__":
    asyncio.run(main())
