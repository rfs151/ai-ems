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
  const ramp = (cfg.value?.mfrr_ramp_s ?? 24) * 1000
  const out: { from: number; to: number; label: string; color: string }[] = []
  for (const a of ctl.value?.mfrr ?? []) {
    const up = a.t_start + ramp * Math.abs(a.power - a.start_level) / a.power
    if (a.t_act && a.t_act < a.t_start) out.push({ from: a.t_act, to: a.t_start, label: `${a.id} 准备`, color: '--idle' })
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
        <h3>mFRR 激活轨迹 <span class="sub">演示参数按 15 分钟 → {{ cfg?.mfrr_window_s ?? 60 }} 秒等比缩短：准备 {{ cfg?.mfrr_prep_s ?? 6 }}s（实际 2.5 分钟）· 爬坡 {{ cfg?.mfrr_ramp_s ?? 24 }}s（实际 10 分钟）· 每 {{ cfg?.mfrr_step_s ?? 1 }}s 一档</span></h3>
        <div class="chart-box"><TimeChart :series="mfrrSeries" unit="kW" :areas="areas" :seconds="420" /></div>
        <div class="notes">
          <div><b>阶段</b>：等待 → 准备（已激活、功率仍为 0）→ 爬坡 → 保持 → 降坡。爬坡按台阶推进（实际业务 1 秒一档），速率 = 指令功率 / 爬坡时间，大小指令的爬坡时间一致。交付结束时刻本身就进入降坡（边界用 ≥，不多保持一档）。</div>
          <div><b>计划激活</b>：必须落在窗口边界、最多提前 30 分钟。窗口边界已过但还在准备期内的指令照常执行（准备期缩短），超过准备期才拒绝——不留这段余量，网络稍慢的合法指令会被误拒。</div>
          <div><b>直接激活</b>：可立即开始，也可带开始时间，提前量限 0 ~ {{ cfg?.mfrr_direct_lead_s ?? 60 }} 秒（实际 15 分钟），开始时间已过或提前太多都拒绝。云端和网关各校验一次。</div>
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
          <div><b>LFSM-O 过频限功率</b>（{{ cfg.lfsmo_enabled ? '投入' : '退出' }}）：频率超过 {{ cfg.lfsmo_start.toFixed(2) }} Hz 后，以叠加后的当前出力为参考功率，按 {{ (cfg.lfsmo_droop * 100).toFixed(0) }}% 下垂削减放电：ΔP = P × (f − {{ cfg.lfsmo_start.toFixed(2) }}) / 50 / {{ cfg.lfsmo_droop }}，最多削到 0。<b>充电或停机时不调整</b>——过频时充电本身就在帮电网，再按比例"减功率"会变成加大放电。开关、启动频率、下垂系数可在控制台远程修改，网关先整体校验再生效。</div>
          <div><b>叠加与限幅</b>：站级指令 = 计划 + mFRR + FCR + LFSM-O，再按电池允许功率和 SOC（10%~95%，边界 5% 内线性降额）限幅、按 1000 kW/s 限制爬坡，最后按两台 PCS 的可用能力分配。同一台 PCS 两次下发至少间隔 {{ cfg.write_interval_ms }} ms，期间的变化合并到下一次（联锁清零不受限）。</div>
        </div>
      </section>

      <section class="panel">
        <h3>测试覆盖 <span class="sub">tests/ · 144 项断言 · ASan/UBSan 下运行</span></h3>
        <div class="notes">
          <div>算法写成纯函数（输入状态和时间，输出功率），不依赖线程和设备，单元测试直接驱动时间。</div>
          <div>FCR 死区边界、线性段、饱和；LFSM-O 启动点与下垂斜率；mFRR 各阶段关键时刻的功率值。</div>
          <div>指令校验：窗口未对齐、时间已过、准备期内迟到、直接激活提前量越限、重复编号、功率越限、时长不足、反向冲突、叠加超容量。</div>
          <div>回归用例：LFSM-O 充电时不调整；交付结束边界不多保持一档；准备期内取消不产生功率；台阶按步长取整。</div>
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
