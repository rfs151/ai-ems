"""各厂商设备的寄存器映射（全部为虚构布局，用于演示点表适配）。

每台设备一个 Modbus TCP 端口、站号 1。地址从 0 开始（协议地址，不是 4xxxx 编号）。
寄存器表与 docs/point-tables/ 和 ems-core/config/vendors/ 保持一致。
"""
from __future__ import annotations

import struct
import time
from typing import Callable

from pymodbus.datastore import ModbusSequentialDataBlock, ModbusSlaveContext

from .plant import Plant


# ------------------------------------------------------------------ 编码
def u16(v: float) -> int:
    return int(round(v)) & 0xFFFF


def i16(v: float) -> int:
    v = max(-32768, min(32767, int(round(v))))
    return v & 0xFFFF


def f32_abcd(v: float) -> list[int]:
    hi, lo = struct.unpack(">HH", struct.pack(">f", v))
    return [hi, lo]


def f32_cdab(v: float) -> list[int]:
    hi, lo = f32_abcd(v)
    return [lo, hi]


def u32_abcd(v: float) -> list[int]:
    n = int(round(v)) & 0xFFFFFFFF
    return [n >> 16, n & 0xFFFF]


def decode_f32_cdab(regs: list[int]) -> float:
    return struct.unpack(">f", struct.pack(">HH", regs[1], regs[0]))[0]


# ------------------------------------------------------------------ 数据块
class Block(ModbusSequentialDataBlock):
    """可挂写回调的数据块；设备通信中断时拒绝访问（主站收到异常响应）。"""

    def __init__(self, size: int, online: Callable[[], bool], on_write=None):
        super().__init__(0, [0] * size)
        self._online = online
        self._on_write = on_write

    def validate(self, address, count=1):
        return self._online() and super().validate(address, count)

    def setValues(self, address, values):  # noqa: N802 (pymodbus 接口名)
        super().setValues(address, values)
        if self._on_write is not None:
            self._on_write(address, values if isinstance(values, list) else [values])

    def put(self, address: int, values: list[int] | int) -> None:
        """仿真刷新用，不触发写回调。"""
        super().setValues(address, values if isinstance(values, list) else [values])


class Device:
    name = ""
    port = 0

    def __init__(self, plant: Plant):
        self.plant = plant

    def online(self) -> bool:
        return self.name not in self.plant.comm_down

    def context(self) -> ModbusSlaveContext:
        raise NotImplementedError

    def refresh(self) -> None:
        raise NotImplementedError


def _empty(online) -> Block:
    return Block(1, online)


# ------------------------------------------------------------------ 厂商 A · PCS
class VendorAPcs(Device):
    """厂商 A PCS：全部为保持寄存器，int16 × 0.1，大端。

    0 运行状态(0停机 1运行 2故障)  1 有功 0.1kW  2 无功 0.1kvar  3 交流电压 0.1V
    4 直流电压 0.1V  5 IGBT 温度 0.1℃  6 故障码
    100 启停命令(1启动 0停机)  101 有功设定 0.1kW（int16）
    """

    name, port = "pcs1", 15021

    def __init__(self, plant: Plant):
        super().__init__(plant)
        self.pcs = plant.pcs1
        self.hr = Block(128, self.online, self._write)

    def _write(self, addr: int, values: list[int]) -> None:
        for i, raw in enumerate(values):
            a = addr + i
            if a == 100:
                self.pcs.running = raw == 1
            elif a == 101:
                self.pcs.setpoint_kw = (raw - 0x10000 if raw & 0x8000 else raw) / 10

    def context(self) -> ModbusSlaveContext:
        return ModbusSlaveContext(di=_empty(self.online), co=_empty(self.online),
                                  hr=self.hr, ir=_empty(self.online), zero_mode=True)

    def refresh(self) -> None:
        p = self.pcs
        status = 2 if p.fault else (1 if p.running else 0)
        self.hr.put(0, [status, i16(p.p_kw * 10), i16(p.q_kvar * 10), u16(p.ac_v() * 10),
                        u16(p.dc_v() * 10), i16(p.temp_c * 10), p.fault_code])


# ------------------------------------------------------------------ 厂商 B · PCS
class VendorBPcs(Device):
    """厂商 B PCS：float32，低字在前（CDAB）。

    0-1 有功 kW  2-3 无功 kvar  4-5 交流电压 V  6-7 直流电压 V  8-9 温度 ℃
    10 状态字 bit0 运行 bit1 故障 bit2 远方 bit3 设定值被拒绝
    200 使能字：写 0xA5A5 后 5 秒内允许写设定值
    201 启停(1运行 0停机)  202-203 有功设定 kW（float32 CDAB）
    """

    name, port = "pcs2", 15022
    UNLOCK = 0xA5A5

    def __init__(self, plant: Plant):
        super().__init__(plant)
        self.pcs = plant.pcs2
        self.hr = Block(256, self.online, self._write)
        self.unlock_until = 0.0
        self.rejected = False

    def _write(self, addr: int, values: list[int]) -> None:
        end = addr + len(values)
        if addr <= 200 < end and values[200 - addr] == self.UNLOCK:
            self.unlock_until = time.monotonic() + 5
        if addr <= 201 < end:
            self.pcs.running = values[201 - addr] == 1
        if addr <= 202 and end >= 204:
            if time.monotonic() <= self.unlock_until:
                self.pcs.setpoint_kw = decode_f32_cdab(values[202 - addr:204 - addr])
                self.rejected = False
            else:
                self.rejected = True

    def context(self) -> ModbusSlaveContext:
        return ModbusSlaveContext(di=_empty(self.online), co=_empty(self.online),
                                  hr=self.hr, ir=_empty(self.online), zero_mode=True)

    def refresh(self) -> None:
        p = self.pcs
        word = (1 if p.running else 0) | (2 if p.fault else 0) | 4 | (8 if self.rejected else 0)
        regs = (f32_cdab(p.p_kw) + f32_cdab(p.q_kvar) + f32_cdab(p.ac_v())
                + f32_cdab(p.dc_v()) + f32_cdab(p.temp_c) + [word])
        self.hr.put(0, regs)


# ------------------------------------------------------------------ 厂商 B · 电池簇
class VendorBBattery(Device):
    """厂商 B 电池簇：输入寄存器（只读）。

    0 SOC 0.1%  1 SOH 0.1%  2 总压 0.1V  3 电流 0.1A(int16，正=放电)
    4 最高温度 0.1℃  5 最低温度 0.1℃  6 状态(0待机 1充电 2放电 3故障)
    7 允许充电功率 kW  8 允许放电功率 kW
    20-35 单体电压 mV（16 个代表性单体）
    """

    name, port = "bat1", 15023

    def __init__(self, plant: Plant):
        super().__init__(plant)
        self.bat, self.pcs = plant.bat1, plant.pcs1
        self.ir = Block(64, self.online)

    def context(self) -> ModbusSlaveContext:
        return ModbusSlaveContext(di=_empty(self.online), co=_empty(self.online),
                                  hr=_empty(self.online), ir=self.ir, zero_mode=True)

    def refresh(self) -> None:
        b, cur = self.bat, self.pcs.dc_current_a()
        state = 2 if self.pcs.p_kw > 2 else (1 if self.pcs.p_kw < -2 else 0)
        self.ir.put(0, [u16(b.soc * 10), u16(b.soh * 10), u16(b.pack_v * 10), i16(cur * 10),
                        i16((b.temp_c + 1.8) * 10), i16((b.temp_c - 1.5) * 10), state,
                        u16(b.max_charge_kw()), u16(b.max_discharge_kw())])
        self.ir.put(20, [u16(b.cell_mv(i, cur / 2)) for i in range(b.cells)])


# ------------------------------------------------------------------ 厂商 C · 电池簇
class VendorCBattery(Device):
    """厂商 C 电池簇：保持寄存器，累计量用 uint32（ABCD），状态按位打包，告警用代码表。

    0-1 累计充电量 0.1kWh  2-3 累计放电量 0.1kWh  4 SOC 0.01%  5 总压 0.1V
    6 电流 0.1A(int16，正=放电)  7 平均温度 0.1℃
    8 状态位 bit0 就绪 bit1 充电中 bit2 放电中 bit3 告警
    9 告警码(0 无 0x11 单体过压 0x12 单体欠压 0x21 过温 0x31 绝缘低)
    10 允许充电功率 kW  11 允许放电功率 kW
    """

    name, port = "bat2", 15024

    def __init__(self, plant: Plant):
        super().__init__(plant)
        self.bat, self.pcs = plant.bat2, plant.pcs2
        self.hr = Block(64, self.online)

    def context(self) -> ModbusSlaveContext:
        return ModbusSlaveContext(di=_empty(self.online), co=_empty(self.online),
                                  hr=self.hr, ir=_empty(self.online), zero_mode=True)

    def refresh(self) -> None:
        b, p = self.bat, self.pcs.p_kw
        alarm = 0x21 if b.temp_c > 45 else (0x12 if b.soc < 3 else 0)
        bits = 1 | (2 if p < -2 else 0) | (4 if p > 2 else 0) | (8 if alarm else 0)
        self.hr.put(0, u32_abcd(b.charge_kwh * 10) + u32_abcd(b.discharge_kwh * 10)
                    + [u16(b.soc * 100), u16(b.pack_v * 10), i16(self.pcs.dc_current_a() * 10),
                       i16(b.temp_c * 10), bits, alarm, u16(b.max_charge_kw()), u16(b.max_discharge_kw())])


# ------------------------------------------------------------------ 关口电表
class Meter(Device):
    """并网点电表：输入寄存器 float32（ABCD）。

    0-1 有功 kW(正=上网)  2-3 无功 kvar  4-5 频率 Hz  6-7 A 相电压 V
    """

    name, port = "meter", 15025

    def __init__(self, plant: Plant):
        super().__init__(plant)
        self.ir = Block(16, self.online)

    def context(self) -> ModbusSlaveContext:
        return ModbusSlaveContext(di=_empty(self.online), co=_empty(self.online),
                                  hr=_empty(self.online), ir=self.ir, zero_mode=True)

    def refresh(self) -> None:
        pl = self.plant
        self.ir.put(0, f32_abcd(pl.poc_p_kw) + f32_abcd(0.0) + f32_abcd(pl.freq.hz)
                    + f32_abcd(10.5 + (pl.freq.hz - 50) * 0.02))


# ------------------------------------------------------------------ DI/DO
class Dio(Device):
    """DI/DO 模块：离散输入 0 急停 1 消防 2 门禁；线圈 0 并网开关 1 风机。"""

    name, port = "dio", 15026
    DI = ("estop", "fire", "door")
    DO = ("grid_breaker", "fan")

    def __init__(self, plant: Plant):
        super().__init__(plant)
        self.di = Block(8, self.online)
        self.co = Block(8, self.online, self._write)

    def _write(self, addr: int, values: list[int]) -> None:
        for i, v in enumerate(values):
            if addr + i < len(self.DO):
                self.plant.do[self.DO[addr + i]] = bool(v)

    def context(self) -> ModbusSlaveContext:
        return ModbusSlaveContext(di=self.di, co=self.co, hr=_empty(self.online),
                                  ir=_empty(self.online), zero_mode=True)

    def refresh(self) -> None:
        self.di.put(0, [int(self.plant.di[k]) for k in self.DI])
        self.co.put(0, [int(self.plant.do[k]) for k in self.DO])


ALL = (VendorAPcs, VendorBPcs, VendorBBattery, VendorCBattery, Meter, Dio)
