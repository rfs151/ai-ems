"""储能站物理模型：2 台 PCS + 2 个电池簇 + 关口电表 + DI/DO + 电网频率。

只描述物理量，不关心寄存器格式；各厂商的寄存器映射在 devices/ 里。
功率符号约定：正 = 放电（向电网送电），负 = 充电。
"""
from __future__ import annotations

import math
import random
from dataclasses import dataclass, field


@dataclass
class Battery:
    name: str
    capacity_kwh: float = 1000.0
    soc: float = 60.0                 # %
    soh: float = 97.5                 # %
    temp_c: float = 26.0
    charge_kwh: float = 12840.0       # 累计充电量
    discharge_kwh: float = 11965.0    # 累计放电量
    cells: int = 16                   # 只模拟 16 个代表性单体
    cell_offsets_mv: list[float] = field(default_factory=list)

    def __post_init__(self) -> None:
        rnd = random.Random(self.name)
        self.cell_offsets_mv = [rnd.uniform(-6, 6) for _ in range(self.cells)]

    @property
    def ocv_cell_mv(self) -> float:
        # 磷酸铁锂平台区近似：3.20V ~ 3.40V，两端陡升陡降
        s = self.soc / 100
        return 3000 + 300 * s + 120 * math.tanh((s - 0.95) * 25) + 120 * math.tanh((s - 0.05) * 25) + 30

    @property
    def pack_v(self) -> float:
        return self.ocv_cell_mv * 240 / 1000      # 240 串

    def max_charge_kw(self) -> float:
        # SOC 接近满时降额，低温也降额
        k = 1.0 if self.soc < 90 else max(0.0, (98 - self.soc) / 8)
        return 500 * k * (0.5 if self.temp_c < 5 else 1.0)

    def max_discharge_kw(self) -> float:
        k = 1.0 if self.soc > 10 else max(0.0, (self.soc - 2) / 8)
        return 500 * k

    def cell_mv(self, i: int, current_a: float) -> float:
        return self.ocv_cell_mv + self.cell_offsets_mv[i] - current_a * 0.4


@dataclass
class Pcs:
    name: str
    rated_kw: float = 500.0
    battery: Battery | None = None
    running: bool = True
    fault_code: int = 0
    setpoint_kw: float = 0.0
    p_kw: float = 0.0
    q_kvar: float = 0.0
    temp_c: float = 32.0
    tau_s: float = 0.25               # 一阶响应时间常数
    ramp_kw_s: float = 1000.0         # 硬件爬坡上限

    @property
    def fault(self) -> bool:
        return self.fault_code != 0

    def step(self, dt: float) -> None:
        target = self.setpoint_kw if (self.running and not self.fault) else 0.0
        b = self.battery
        if b is not None:
            target = min(target, b.max_discharge_kw()) if target > 0 else max(target, -b.max_charge_kw())
        target = max(-self.rated_kw, min(self.rated_kw, target))
        delta = (target - self.p_kw) * (1 - math.exp(-dt / self.tau_s))
        lim = self.ramp_kw_s * dt
        self.p_kw += max(-lim, min(lim, delta))
        self.q_kvar = 0.0
        self.temp_c += ((30 + abs(self.p_kw) / 500 * 18) - self.temp_c) * dt / 120
        if b is not None:
            e = self.p_kw * dt / 3600                       # kWh，正 = 放电
            eff = 0.96
            if e > 0:
                b.soc -= e / eff / b.capacity_kwh * 100
                b.discharge_kwh += e
            else:
                b.soc -= e * eff / b.capacity_kwh * 100
                b.charge_kwh += -e
            b.soc = max(0.0, min(100.0, b.soc))
            b.temp_c += ((25 + abs(self.p_kw) / 500 * 8) - b.temp_c) * dt / 300

    def ac_v(self) -> float:
        return 400.0 + random.uniform(-1.5, 1.5)

    def dc_v(self) -> float:
        return self.battery.pack_v if self.battery else 768.0

    def dc_current_a(self) -> float:
        return self.p_kw * 1000 / self.dc_v() if self.battery else 0.0


class GridFrequency:
    """50Hz + 低幅随机波动（OU 过程）+ 页面注入的阶跃扰动（按时间常数恢复）。"""

    def __init__(self) -> None:
        self.noise = 0.0
        self.disturbance = 0.0
        self.recover_tau = 60.0

    def inject(self, delta_hz: float, recover_s: float = 60.0) -> None:
        self.disturbance += delta_hz
        self.recover_tau = max(5.0, recover_s)

    def step(self, dt: float) -> None:
        theta, sigma = 0.08, 0.006                  # 稳态标准差约 15mHz
        self.noise += -theta * self.noise * dt + sigma * math.sqrt(dt) * random.gauss(0, 1)
        self.disturbance *= math.exp(-dt / self.recover_tau)

    @property
    def hz(self) -> float:
        return 50.0 + self.noise + self.disturbance


class Plant:
    def __init__(self) -> None:
        self.bat1 = Battery("BAT-1", soc=62.0)
        self.bat2 = Battery("BAT-2", soc=58.0)
        self.pcs1 = Pcs("PCS-1", battery=self.bat1)
        self.pcs2 = Pcs("PCS-2", battery=self.bat2)
        self.freq = GridFrequency()
        self.site_load_kw = 35.0                    # 站用电（空调、照明等）
        self.di = {"estop": False, "fire": False, "door": False}
        self.do = {"grid_breaker": True, "fan": False}
        self.comm_down: set[str] = set()            # 故障注入：通信中断的设备

    def step(self, dt: float) -> None:
        self.freq.step(dt)
        if self.di["estop"] or self.di["fire"] or not self.do["grid_breaker"]:
            for p in (self.pcs1, self.pcs2):
                p.setpoint_kw = 0.0
        for p in (self.pcs1, self.pcs2):
            p.step(dt)
        self.site_load_kw += (35 + 10 * math.sin(self.site_load_kw) - self.site_load_kw) * dt / 30 + random.uniform(-0.3, 0.3)

    @property
    def poc_p_kw(self) -> float:
        """并网点有功：正 = 向电网送电。"""
        return self.pcs1.p_kw + self.pcs2.p_kw - self.site_load_kw
