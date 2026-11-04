<script setup lang="ts">
// 设备与点表：展示同一套统一模型如何映射到不同厂商的寄存器，并显示实时换算值
import { computed, ref, watchEffect } from 'vue'
import { ems, fmt, val } from '../composables/useEms'
import type { ControlMeta, PointMeta } from '../types'

const devices = computed(() => ems.meta?.devices ?? [])
const selected = ref('')
watchEffect(() => { if (!selected.value && devices.value.length) selected.value = devices.value[0].id })
const dev = computed(() => devices.value.find((d) => d.id === selected.value))

const areaName: Record<string, string> = { hr: '保持寄存器', ir: '输入寄存器', co: '线圈', di: '离散输入' }
const areaCode: Record<string, string> = { hr: '03', ir: '04', co: '01', di: '02' }

function typeText(p: PointMeta | ControlMeta) {
  return p.order ? `${p.type} ${p.order}` : p.type
}
function convText(p: PointMeta) {
  if (p.bit !== undefined) return `取 bit${p.bit}`
  if (p.eq !== undefined) return `值 = ${p.eq} 为真`
  if (p.scale !== undefined) return `× ${p.scale}`
  return ''
}
function show(id: string, p: PointMeta) {
  const v = val(`${id}.${p.key}`)
  if (p.bit !== undefined || p.eq !== undefined || p.type === 'bit') return v === undefined ? '—' : v ? '是' : '否'
  return fmt(v, p.scale === 0.001 ? 3 : 2, p.unit)
}
const hex = (n: number) => '0x' + n.toString(16).toUpperCase().padStart(4, '0')
</script>

<template>
  <div class="wrap">
    <section class="panel intro">
      <h3>多厂商点表适配</h3>
      <p>
        上层的控制、规则、上云只使用统一模型键（如 <code>p</code>、<code>soc</code>）。各厂商的寄存器区、地址、数据类型、字序、倍率、
        位定义、枚举比较、写前解锁全部写在 JSON 点表里，接入新厂商不需要改 C 代码。
        采集时同一区、同一采集组的相邻地址自动合并为一次批量读。
      </p>
      <div class="tabs">
        <button v-for="d in devices" :key="d.id" :class="{ on: d.id === selected }" @click="selected = d.id">
          {{ d.name }} <span class="muted">{{ d.vendor }}</span>
        </button>
      </div>
    </section>

    <section v-if="dev" class="panel">
      <h3>{{ dev.name }} · {{ dev.vendor }} {{ dev.model }}
        <span class="sub">{{ dev.points.length }} 个采集点 · {{ dev.controls.length }} 个控制点</span></h3>
      <div class="scroll">
        <table>
          <thead>
            <tr><th>模型键</th><th>名称</th><th>寄存器区 / 功能码</th><th>地址</th><th>数据类型</th><th>换算</th><th>采集组</th><th class="r">实时值</th></tr>
          </thead>
          <tbody>
            <tr v-for="p in dev.points" :key="p.key">
              <td><code>{{ dev.id }}.{{ p.key }}</code></td>
              <td>{{ p.name }}</td>
              <td>{{ areaName[p.area] }} <span class="muted">FC{{ areaCode[p.area] }}</span></td>
              <td class="num">{{ p.addr }} <span class="muted">{{ hex(p.addr) }}</span></td>
              <td><code>{{ typeText(p) }}</code></td>
              <td class="muted">{{ convText(p) }}</td>
              <td><span class="tag" :class="p.group === 'fast' ? 'good' : ''">{{ p.group === 'fast' ? '快 200ms' : '慢 5s' }}</span></td>
              <td class="r num">{{ show(dev.id, p) }}</td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <section v-if="dev && dev.controls.length" class="panel">
      <h3>控制点（写）</h3>
      <table>
        <thead><tr><th>模型键</th><th>名称</th><th>区</th><th>地址</th><th>数据类型</th><th>写入时序</th></tr></thead>
        <tbody>
          <tr v-for="c in dev.controls" :key="c.key">
            <td><code>{{ dev.id }}.{{ c.key }}</code></td>
            <td>{{ c.name }}</td>
            <td>{{ areaName[c.area] }}</td>
            <td class="num">{{ c.addr }}</td>
            <td><code>{{ typeText(c) }}</code><span v-if="c.scale" class="muted"> × {{ c.scale }}</span></td>
            <td>
              <span v-if="c.unlock">先写 <code>{{ c.unlock.addr }}</code> = <code>{{ hex(c.unlock.value) }}</code>，5 秒内再写设定值</span>
              <span v-else class="muted">直接写入（{{ c.area === 'co' ? 'FC05' : c.type.endsWith('32') ? 'FC16' : 'FC06' }}）</span>
            </td>
          </tr>
        </tbody>
      </table>
    </section>
  </div>
</template>

<style scoped>
.wrap { display: flex; flex-direction: column; gap: 12px; }
.intro p { margin: 0; padding: 10px 14px 4px; color: var(--muted); max-width: 900px; }
.tabs { display: flex; flex-wrap: wrap; gap: 6px; padding: 10px 14px 14px; }
.tabs button { border: 1px solid var(--border); background: var(--panel); color: var(--text); border-radius: 6px;
  padding: 5px 10px; cursor: pointer; font: inherit; font-size: 13px; }
.tabs button.on { border-color: var(--accent); color: var(--accent); }
.scroll { overflow-x: auto; }
.r { text-align: right; }
</style>
