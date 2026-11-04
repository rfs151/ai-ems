<script setup lang="ts">
import { computed } from 'vue'
import { ems, fmt, val } from '../composables/useEms'
import TimeChart, { type SeriesDef } from './TimeChart.vue'
import DeviceList from './DeviceList.vue'
import EventList from './EventList.vue'
import StatusPanel from './StatusPanel.vue'
import Console from './Console.vue'
import CmdLog from './CmdLog.vue'

const sum = (...xs: (number | undefined)[]) => (xs.some((x) => x === undefined) ? undefined : xs.reduce<number>((a, b) => a + (b as number), 0))

const powerSeries: SeriesDef[] = [
  { name: 'PCS-1', color: '--s-pcs1', get: (h) => h.v['pcs1.p'], area: true, stack: 'p' },
  { name: 'PCS-2', color: '--s-pcs2', get: (h) => h.v['pcs2.p'], area: true, stack: 'p' },
  { name: '实际总功率', color: '--s-actual', get: (h) => sum(h.v['pcs1.p'], h.v['pcs2.p']), width: 2 },
  { name: '站级指令', color: '--s-cmd', get: (h) => h.ctl?.p_cmd, dashed: true, width: 1.2 },
]
const compSeries: SeriesDef[] = [
  { name: 'mFRR', color: '--s-mfrr', get: (h) => h.ctl?.p_mfrr, width: 1.8 },
  { name: 'FCR', color: '--s-fcr', get: (h) => h.ctl?.p_fcr, width: 1.4 },
  { name: 'LFSM-O', color: '--s-lfsmo', get: (h) => h.ctl?.p_lfsmo, width: 1.8 },
  { name: '计划', color: '--s-base', get: (h) => h.ctl?.p_base, step: true, dashed: true, width: 1 },
]
const freqSeries: SeriesDef[] = [
  { name: '电网频率', color: '--s-freq', get: (h) => h.v['meter.f'], width: 1.5 },
]
// 纵轴至少显示 49.9~50.1Hz，扰动超出时自动扩展
const freqMin = (e: { min: number }) => Math.min(49.9, +(e.min - 0.02).toFixed(2))
const freqMax = (e: { max: number }) => Math.max(50.1, +(e.max + 0.02).toFixed(2))
const freqMarks = [{ y: 50.2, label: 'LFSM-O 50.2' }, { y: 50.01, label: '' }, { y: 49.99, label: 'FCR 死区' }]

const total = computed(() => sum(val('pcs1.p'), val('pcs2.p')))
const soc = computed(() => {
  const a = val('bat1.soc'), b = val('bat2.soc')
  return a === undefined || b === undefined ? (a ?? b) : (a + b) / 2
})
const ctl = computed(() => ems.latest?.ctl)
</script>

<template>
  <div class="grid">
    <aside class="left"><DeviceList /></aside>

    <main class="center">
      <div class="tiles">
        <div class="panel tile"><span class="muted">总有功（正放负充）</span><b class="num">{{ fmt(total, 1) }}</b><i>kW</i></div>
        <div class="panel tile"><span class="muted">平均 SOC</span><b class="num">{{ fmt(soc, 1) }}</b><i>%</i></div>
        <div class="panel tile"><span class="muted">可充 / 可放</span><b class="num">{{ ctl ? `${ctl.p_chg_max} / ${ctl.p_dis_max}` : '—' }}</b><i>kW</i></div>
        <div class="panel tile"><span class="muted">并网点功率</span><b class="num">{{ fmt(val('meter.p'), 1) }}</b><i>kW</i></div>
      </div>
      <section class="panel chart-panel">
        <h3>实时功率 <span class="sub">近 5 分钟 · 正值放电、负值充电</span></h3>
        <div class="chart-box tall"><TimeChart :series="powerSeries" unit="kW" /></div>
      </section>
      <section class="panel chart-panel">
        <h3>功率分量 <span class="sub">站级指令 = 计划 + mFRR + FCR + LFSM-O，再经 SOC 限幅和爬坡限制</span></h3>
        <div class="chart-box"><TimeChart :series="compSeries" unit="kW" /></div>
      </section>
      <section class="panel chart-panel">
        <h3>电网频率 <span class="sub">关口电表 200ms 采集</span></h3>
        <div class="chart-box"><TimeChart :series="freqSeries" unit="Hz" :digits="3" :marks="freqMarks"
          :y-min="freqMin" :y-max="freqMax" /></div>
      </section>
      <CmdLog />
      <EventList />
    </main>

    <aside class="right"><Console /><StatusPanel /></aside>
  </div>
</template>

<style scoped>
.grid { display: grid; grid-template-columns: 250px minmax(0, 1fr) 290px; gap: 12px; align-items: start; }
.center, .right { display: flex; flex-direction: column; gap: 12px; }
.tiles { display: grid; grid-template-columns: repeat(4, 1fr); gap: 12px; }
.tile { padding: 10px 14px; display: flex; flex-direction: column; }
.tile span { font-size: 12px; }
.tile b { font-size: 24px; font-weight: 600; line-height: 1.3; }
.tile i { font-style: normal; color: var(--muted); font-size: 12px; }
.chart-box { height: 220px; padding: 6px 4px 0; }
.chart-box.tall { height: 290px; }
@media (max-width: 1200px) {
  .grid { grid-template-columns: 230px minmax(0, 1fr); }
  .right { grid-column: 1 / -1; display: grid; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); }
}
@media (max-width: 760px) {
  .grid { grid-template-columns: minmax(0, 1fr); }
  .tiles { grid-template-columns: repeat(2, 1fr); }
}
</style>
