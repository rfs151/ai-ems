<script setup lang="ts">
import { computed } from 'vue'
import { ems, fmt, val } from '../composables/useEms'
import type { DeviceMeta } from '../types'

const devices = computed(() => ems.meta?.devices ?? [])
const online = (id: string) => ems.latest?.dev?.[id]?.online ?? false

function pcsState(d: DeviceMeta) {
  if (!online(d.id)) return { text: '离线', cls: 'bad' }
  if (val(`${d.id}.fault`)) return { text: '故障', cls: 'bad' }
  if (val(`${d.id}.run`)) return { text: '运行', cls: 'good' }
  return { text: '停机', cls: '' }
}
const dioItems = [
  { key: 'estop', name: '急停', alarmOn: true },
  { key: 'fire', name: '消防', alarmOn: true },
  { key: 'door', name: '柜门', alarmOn: true },
  { key: 'grid_breaker', name: '并网开关', alarmOn: false },
  { key: 'fan', name: '风机', alarmOn: false },
]
</script>

<template>
  <section class="panel">
    <h3>站点设备 <span class="sub">{{ devices.length }} 台</span></h3>
    <div class="list">
      <div v-for="d in devices" :key="d.id" class="dev" :class="{ off: !online(d.id) }">
        <div class="head">
          <span class="dot" :class="online(d.id) ? 'good' : 'bad'"></span>
          <b>{{ d.name }}</b>
          <span class="tag">{{ d.vendor }}</span>
        </div>

        <template v-if="d.type === 'pcs'">
          <div class="row"><span class="big num">{{ fmt(val(`${d.id}.p`), 1) }}</span><span class="muted">kW</span>
            <span class="tag" :class="pcsState(d).cls" style="margin-left:auto">{{ pcsState(d).text }}</span></div>
          <div class="kv muted num">温度 {{ fmt(val(`${d.id}.temp`), 1, '°C') }} · 直流 {{ fmt(val(`${d.id}.vdc`), 0, 'V') }}</div>
        </template>

        <template v-else-if="d.type === 'bms'">
          <div class="row"><span class="big num">{{ fmt(val(`${d.id}.soc`), 1) }}</span><span class="muted">% SOC</span></div>
          <div class="bar"><i :style="{ width: (val(`${d.id}.soc`) ?? 0) + '%' }"></i></div>
          <div class="kv muted num">{{ fmt(val(`${d.id}.v`), 0, 'V') }} · {{ fmt(val(`${d.id}.i`), 1, 'A') }} · {{ fmt(val(`${d.id}.tmax`), 1, '°C') }}</div>
        </template>

        <template v-else-if="d.type === 'meter'">
          <div class="row"><span class="big num">{{ fmt(val(`${d.id}.p`), 1) }}</span><span class="muted">kW 并网点</span></div>
          <div class="kv muted num">频率 {{ fmt(val(`${d.id}.f`), 3, 'Hz') }}</div>
        </template>

        <template v-else-if="d.type === 'dio'">
          <div class="dio">
            <span v-for="it in dioItems" :key="it.key" class="tag"
                  :class="val(`${d.id}.${it.key}`) ? (it.alarmOn ? 'bad' : 'good') : ''">
              {{ it.name }} {{ val(`${d.id}.${it.key}`) ? (it.alarmOn ? '动作' : '合') : (it.alarmOn ? '正常' : '分') }}
            </span>
          </div>
        </template>
      </div>
    </div>
  </section>
</template>

<style scoped>
.list { padding: 8px; display: flex; flex-direction: column; gap: 8px; }
.dev { padding: 8px 10px; border: 1px solid var(--border); border-radius: 8px; }
.dev.off { opacity: 0.6; }
.head { display: flex; align-items: center; gap: 6px; }
.head .tag { margin-left: auto; }
.row { display: flex; align-items: baseline; gap: 4px; margin-top: 2px; }
.big { font-size: 20px; font-weight: 600; }
.kv { font-size: 12px; }
.bar { height: 5px; background: var(--chip); border-radius: 3px; overflow: hidden; margin: 4px 0 2px; }
.bar i { display: block; height: 100%; background: var(--good); transition: width .5s; }
.dio { display: flex; flex-wrap: wrap; gap: 4px; margin-top: 6px; }
</style>
