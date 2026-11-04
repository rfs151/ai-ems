<script setup lang="ts">
// 频率-功率特性：FCR（死区 + 线性 + 饱和）与 LFSM-O（以当前出力为参考的下垂），实时工作点随频率移动
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import * as echarts from 'echarts/core'
import { LineChart, ScatterChart } from 'echarts/charts'
import { GridComponent, LegendComponent, MarkLineComponent, TooltipComponent } from 'echarts/components'
import { CanvasRenderer } from 'echarts/renderers'
import { ems, val } from '../composables/useEms'

echarts.use([LineChart, ScatterChart, GridComponent, LegendComponent, MarkLineComponent, TooltipComponent, CanvasRenderer])

const el = ref<HTMLDivElement>()
let chart: echarts.ECharts | null = null
let ro: ResizeObserver | null = null
const css = (n: string) => getComputedStyle(document.documentElement).getPropertyValue(n).trim()

const cfg = computed(() => ems.latest?.ctl?.cfg)

function curves() {
  const c = cfg.value
  if (!c) return { fcr: [], lfsmo: [], sum: [] }
  const fcr = (f: number) => {
    const a = Math.abs(f - 50)
    if (a <= c.fcr_deadband) return 0
    return (f < 50 ? 1 : -1) * Math.min(1, (a - c.fcr_deadband) / (c.fcr_full_dev - c.fcr_deadband)) * c.fcr_p_max
  }
  // LFSM-O 以当前出力为参考：曲线按此刻的叠加功率（无 mFRR 时按 FCR 自身）画，只削减放电
  const pNow = ems.latest?.ctl?.p_pre ?? 0
  const lfsmo = (f: number) => {
    const p = pNow - fcr(fNow()) + fcr(f)
    if (!c.lfsmo_enabled || p <= 0 || f <= c.lfsmo_start) return 0
    return -Math.min(p, p * (f - c.lfsmo_start) / 50 / c.lfsmo_droop)
  }
  const xs: number[] = []
  for (let f = 49.6; f <= 50.5001; f += 0.002) xs.push(+f.toFixed(3))
  return {
    fcr: xs.map((f) => [f, +fcr(f).toFixed(2)]),
    lfsmo: xs.map((f) => [f, +lfsmo(f).toFixed(2)]),
    sum: xs.map((f) => [f, +(fcr(f) + lfsmo(f)).toFixed(2)]),
  }
}

function option(): echarts.EChartsCoreOption {
  const text = css('--muted'), grid = css('--grid'), cv = curves()
  return {
    animation: false,
    grid: { left: 56, right: 18, top: 34, bottom: 36 },
    legend: { top: 0, right: 8, textStyle: { color: text, fontSize: 12 }, itemWidth: 14, itemHeight: 8 },
    tooltip: { trigger: 'item', formatter: (p: any) => `${p.seriesName}<br>${p.value[0].toFixed(3)} Hz → ${p.value[1].toFixed(1)} kW` },
    xAxis: { type: 'value', min: 49.6, max: 50.5, name: 'Hz', nameTextStyle: { color: text }, axisLabel: { color: text },
      splitLine: { lineStyle: { color: grid } } },
    yAxis: { type: 'value', name: 'kW', nameTextStyle: { color: text }, axisLabel: { color: text },
      splitLine: { lineStyle: { color: grid } } },
    series: [
      { name: 'FCR', type: 'line', showSymbol: false, data: cv.fcr, lineStyle: { color: css('--s-fcr'), width: 1.4, type: 'dashed' }, itemStyle: { color: css('--s-fcr') } },
      { name: 'LFSM-O', type: 'line', showSymbol: false, data: cv.lfsmo, lineStyle: { color: css('--s-lfsmo'), width: 1.4, type: 'dashed' }, itemStyle: { color: css('--s-lfsmo') },
        markLine: { silent: true, symbol: 'none', lineStyle: { color: css('--warn'), type: 'dotted' }, label: { color: css('--warn'), fontSize: 11 },
          data: [{ xAxis: cfg.value?.lfsmo_start ?? 50.2, name: String(cfg.value?.lfsmo_start ?? 50.2) }], } },
      { name: '合计', type: 'line', showSymbol: false, data: cv.sum, lineStyle: { color: css('--text'), width: 2 }, itemStyle: { color: css('--text') } },
      { name: '当前工作点', type: 'scatter', symbolSize: 12, data: [], itemStyle: { color: css('--accent'), borderColor: '#fff', borderWidth: 2 }, z: 10 },
    ],
  }
}

function refresh() {
  const f = val('meter.f'), c = ems.latest?.ctl
  if (!chart || f === undefined || !c) return
  chart.setOption({ series: [{}, {}, {}, { data: [[f, (c.fcr_enabled ? c.p_fcr : 0) + c.p_lfsmo]] }] })
}
const rebuild = () => { chart?.setOption(option(), true); refresh() }
const fNow = () => val('meter.f') ?? 50

onMounted(() => {
  chart = echarts.init(el.value!)
  rebuild()
  ro = new ResizeObserver(() => chart?.resize())
  ro.observe(el.value!)
  matchMedia('(prefers-color-scheme: dark)').addEventListener('change', rebuild)
})
onBeforeUnmount(() => {
  ro?.disconnect()
  matchMedia('(prefers-color-scheme: dark)').removeEventListener('change', rebuild)
  chart?.dispose()
})
// 曲线依赖当前出力和 LFSM-O 参数，每秒重画一次
watch(() => ems.latest?.ts, rebuild)
</script>

<template><div ref="el" class="chart"></div></template>

<style scoped>
.chart { width: 100%; height: 100%; min-height: 260px; }
</style>
