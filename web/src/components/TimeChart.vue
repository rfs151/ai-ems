<script setup lang="ts">
// 通用时间曲线：父组件给出系列定义，历史数据更新时只刷新 data，不重建图表
import { onBeforeUnmount, onMounted, ref, watch } from 'vue'
import * as echarts from 'echarts/core'
import { LineChart } from 'echarts/charts'
import { GridComponent, LegendComponent, MarkAreaComponent, MarkLineComponent, TooltipComponent } from 'echarts/components'
import { CanvasRenderer } from 'echarts/renderers'
import { history, historyVersion } from '../composables/useEms'
import type { HistoryItem } from '../types'

echarts.use([LineChart, GridComponent, LegendComponent, MarkAreaComponent, MarkLineComponent, TooltipComponent, CanvasRenderer])

export interface SeriesDef {
  name: string
  color: string                    // CSS 变量名，如 --s-actual
  get: (h: HistoryItem) => number | undefined
  dashed?: boolean
  area?: boolean
  stack?: string
  step?: boolean
  width?: number
}

const props = defineProps<{
  series: SeriesDef[]
  unit: string
  yMin?: number | ((ext: { min: number; max: number }) => number)
  yMax?: number | ((ext: { min: number; max: number }) => number)
  digits?: number
  marks?: { y: number; label: string }[]
  seconds?: number
  areas?: { from: number; to: number; label: string; color: string }[]   // 时间区间标注
}>()

const el = ref<HTMLDivElement>()
let chart: echarts.ECharts | null = null
let ro: ResizeObserver | null = null

const css = (name: string) => getComputedStyle(document.documentElement).getPropertyValue(name).trim()

function baseOption(): echarts.EChartsCoreOption {
  const text = css('--muted'), grid = css('--grid')
  return {
    animation: false,
    grid: { left: 52, right: 16, top: 30, bottom: 26 },
    legend: { type: 'scroll', top: 0, right: 8, left: 52, align: 'right', itemWidth: 14, itemHeight: 8, textStyle: { color: text, fontSize: 12 } },
    tooltip: {
      trigger: 'axis',
      valueFormatter: (v: unknown) => (typeof v === 'number' ? `${v.toFixed(props.digits ?? 1)} ${props.unit}` : '—'),
    },
    xAxis: {
      type: 'time', axisLabel: { color: text, fontSize: 11, hideOverlap: true },
      axisLine: { lineStyle: { color: grid } }, splitLine: { show: false },
    },
    yAxis: {
      type: 'value', scale: true, min: props.yMin, max: props.yMax,
      axisLabel: { color: text, fontSize: 11 }, splitLine: { lineStyle: { color: grid } },
    },
    series: props.series.map((s, i) => ({
      name: s.name, type: 'line', showSymbol: false, step: s.step ? 'end' : undefined, stack: s.stack,
      lineStyle: { color: css(s.color), width: s.width ?? (s.area ? 0 : 1.6), type: s.dashed ? 'dashed' : 'solid' },
      itemStyle: { color: css(s.color) },
      areaStyle: s.area ? { color: css(s.color), opacity: 0.35 } : undefined,
      markLine: i === 0 && props.marks ? {
        silent: true, symbol: 'none',
        lineStyle: { color: css('--warn'), type: 'dotted' },
        label: { color: css('--warn'), fontSize: 11, formatter: '{b}', position: 'insideEndTop' },
        data: props.marks.map((m) => ({ yAxis: m.y, name: m.label })),
      } : undefined,
      data: [],
    })),
  }
}

function refresh() {
  if (!chart) return
  const all = history.value
  const n = props.seconds ?? 300
  const items = all.length > n ? all.slice(all.length - n) : all
  chart.setOption({
    series: props.series.map((s, i) => ({
      markArea: i === 0 ? {
        silent: true,
        label: { color: css('--muted'), fontSize: 11, position: 'insideTop' },
        data: (props.areas ?? []).map((a) => [
          { xAxis: a.from, name: a.label, itemStyle: { color: css(a.color), opacity: 0.12 } },
          { xAxis: a.to },
        ]),
      } : undefined,
      data: items.map((h) => {
        const v = s.get(h)
        return [h.ts, v === undefined ? null : +v.toFixed(3)]
      }),
    })),
  })
}

function rebuild() {
  chart?.setOption(baseOption(), true)
  refresh()
}

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
watch(historyVersion, refresh)
</script>

<template>
  <div ref="el" class="chart"></div>
</template>

<style scoped>
.chart { width: 100%; height: 100%; min-height: 200px; }
</style>
