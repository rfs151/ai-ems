<script setup lang="ts">
import { computed } from 'vue'
import { ems, fmt, val } from '../composables/useEms'
import TimeChart, { type SeriesDef } from './TimeChart.vue'
import DroopChart from './DroopChart.vue'
import Console from './Console.vue'

const ctl = computed(() => ems.latest?.ctl)
const cfg = computed(() => ctl.value?.cfg)

const mfrrSeries: SeriesDef[] = [
  { name: 'mFRR 分量', color: '--s-mfrr', get: (h) => h.ctl?.p_mfrr, width: 2 },
  { name: '实际总功率', color: '--s-pcs2', get: (h) => (h.v['pcs1.p'] ?? 0) + (h.v['pcs2.p'] ?? 0), width: 1.2 },
]

// 把每条 mFRR 指令的四个阶段画成背景色块
const areas = computed(() => {
  const ramp = (cfg.value?.mfrr_ramp_s ?? 30) * 1000
  const out: { from: number; to: number; label: string; color: string }[] = []
  for (const a of ctl.value?.mfrr ?? []) {
    const up = a.t_start + ramp * Math.abs(a.power - a.start_level) / a.power
    out.push({ from: a.t_start, to: up, label: `${a.id} 爬坡`, color: '--warn' })
    out.push({ from: up, to: a.t_end, label: '保持', color: '--good' })
    out.push({ from: a.t_end, to: a.t_end + ramp, label: '降坡', color: '--warn' })
  }
  return out
})
</script>

<template>
  <div class="algo">
    <div class="main">
      <section class="panel">
        <h3>mFRR 四阶段轨迹 <span class="sub">演示参数：爬坡 {{ cfg?.mfrr_ramp_s ?? 30 }}s（实际 12.5 分钟）· 计划窗口 {{ cfg?.mfrr_window_s ?? 60 }}s（实际 15 分钟）</span></h3>
        <div class="chart-box"><TimeChart :series="mfrrSeries" unit="kW" :areas="areas" :seconds="420" /></div>
        <div class="notes">
          <div><b>四阶段</b>：等待（计划激活未到开始时刻）→ 爬坡 → 保持 → 降坡。爬坡速率 = 指令功率 / 爬坡时间，大小指令的爬坡时间一致。</div>
          <div><b>计划 / 直接激活</b>：计划激活必须落在窗口边界、不能早于当前、最多提前 30 分钟；直接激活立即开始。云端和网关各校验一次。</div>
          <div><b>同向叠加</b>：交付中再收到同方向指令，各自按自己的轨迹爬坡，总功率是逐条之和，变化连续。叠加后超过容量上限的指令拒绝。</div>
          <div><b>平滑交接</b>：新指令开始时，同方向的上一条若正在降坡，新指令从它的当前值接手，不会出现"先降到 0 再爬升"的凹坑。</div>
          <div><b>反向冲突</b>：与正在执行的反向指令时间重叠时拒绝；时间不重叠的反向计划指令可以接受。取消则从当前值按原速率降坡。</div>
        </div>
      </section>

      <section class="panel">
        <h3>频率响应特性 <span class="sub">当前 {{ fmt(val('meter.f'), 3, 'Hz') }} → FCR {{ fmt(ctl?.fcr_enabled ? ctl?.p_fcr : 0, 1, 'kW') }} · LFSM-O {{ fmt(ctl?.p_lfsmo, 1, 'kW') }}</span></h3>
        <div class="chart-box"><DroopChart /></div>
        <div class="notes" v-if="cfg">
          <div><b>FCR 一次调频</b>：死区 ±{{ cfg.fcr_deadband * 1000 }} mHz，频偏 ±{{ cfg.fcr_full_dev * 1000 }} mHz 时满出力 {{ cfg.fcr_p_max }} kW，中间线性。频率低放电、频率高充电。</div>
          <div><b>LFSM-O 过频限功率</b>：频率超过 {{ cfg.lfsmo_start }} Hz 后按 {{ cfg.lfsmo_droop * 100 }}% 下垂减功率（每 0.1 Hz 减 {{ (0.1 / 50 / cfg.lfsmo_droop * cfg.lfsmo_p_ref).toFixed(0) }} kW），储能可一直减到充电。电网规范强制要求，不受模式开关影响。</div>
          <div><b>叠加与限幅</b>：站级指令 = 计划 + mFRR + FCR + LFSM-O，再按电池允许功率和 SOC（10%~95%，边界 5% 内线性降额）限幅、按 1000 kW/s 限制爬坡，最后按两台 PCS 的可用能力分配。</div>
        </div>
      </section>

      <section class="panel">
        <h3>测试覆盖 <span class="sub">tests/test_algo.c · 58 项断言 · ASan/UBSan 下运行</span></h3>
        <div class="notes">
          <div>算法写成纯函数（输入状态和时间，输出功率），不依赖线程和设备，单元测试直接驱动时间。</div>
          <div>FCR 死区边界、线性段、饱和；LFSM-O 启动点与下垂斜率；mFRR 各阶段关键时刻的功率值。</div>
          <div>指令校验：窗口未对齐、时间已过、提前太多、重复编号、功率越限、时长不足、反向冲突、叠加超容量。</div>
          <div>叠加与交接按 100ms 步长逐步推进，断言相邻两步变化不超过爬坡速率之和（无跳变），交接期间功率不跌破新目标值。</div>
        </div>
      </section>
    </div>
    <aside class="side"><Console /></aside>
  </div>
</template>

<style scoped>
.algo { display: grid; grid-template-columns: minmax(0, 1fr) 300px; gap: 12px; align-items: start; }
.main { display: flex; flex-direction: column; gap: 12px; }
.chart-box { height: 300px; padding: 6px 4px 0; }
.notes { padding: 4px 14px 12px; display: flex; flex-direction: column; gap: 6px; font-size: 13px; color: var(--muted); }
.notes b { color: var(--text); font-weight: 600; }
@media (max-width: 1000px) { .algo { grid-template-columns: minmax(0, 1fr); } }
</style>
