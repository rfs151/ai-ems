<script setup lang="ts">
import { computed, ref } from 'vue'
import { ems } from './composables/useEms'
import Overview from './components/Overview.vue'
import PointTable from './components/PointTable.vue'
import AlgoPage from './components/AlgoPage.vue'
import RulesPage from './components/RulesPage.vue'

const tabs = [
  { id: 'overview', name: '总览' },
  { id: 'algo', name: '控制算法' },
  { id: 'rules', name: '规则与告警' },
  { id: 'points', name: '设备与点表' },
]
const tab = ref(tabs.some((t) => '#' + t.id === location.hash) ? location.hash.slice(1) : 'overview')
function go(id: string) { tab.value = id; history.replaceState(null, '', '#' + id) }

const link = computed(() => {
  if (!ems.connected) return { cls: 'bad', text: '云端未连接' }
  if (ems.edge !== 'online') return { cls: 'bad', text: '网关离线' }
  return { cls: 'good', text: '云边链路在线' }
})
</script>

<template>
  <header class="top">
    <div class="brand">
      <svg viewBox="0 0 16 16" width="18" height="18"><rect x="3" y="2" width="10" height="13" rx="2" fill="var(--good)"/><rect x="6" y="0.5" width="4" height="2" rx="1" fill="var(--good)"/><path d="M8.6 5 6 9h2l-.6 3L10 8H8z" fill="#fff"/></svg>
      <b>储能 EMS 边缘网关</b>
      <span class="muted site">{{ ems.meta?.name ?? '' }}</span>
    </div>
    <nav>
      <button v-for="t in tabs" :key="t.id" :class="{ on: tab === t.id }" @click="go(t.id)">{{ t.name }}</button>
    </nav>
    <div class="link"><span class="dot" :class="link.cls"></span>{{ link.text }}</div>
  </header>
  <div class="notice">演示环境：设备为仿真，所有访客共享同一个储能站。厂商名称均为虚构。</div>
  <div class="page">
    <Overview v-if="tab === 'overview'" />
    <AlgoPage v-else-if="tab === 'algo'" />
    <RulesPage v-else-if="tab === 'rules'" />
    <PointTable v-else />
  </div>
</template>

<style scoped>
.top { display: flex; align-items: center; gap: 20px; padding: 0 16px; height: 50px; background: var(--panel);
  border-bottom: 1px solid var(--border); position: sticky; top: 0; z-index: 5; }
.brand { display: flex; align-items: center; gap: 8px; white-space: nowrap; }
.site { font-size: 13px; }
nav { display: flex; gap: 4px; }
nav button { border: 0; background: none; color: var(--muted); font: inherit; padding: 6px 12px; border-radius: 6px; cursor: pointer; }
nav button.on { background: var(--chip); color: var(--text); font-weight: 600; }
.link { margin-left: auto; display: flex; align-items: center; gap: 6px; font-size: 13px; white-space: nowrap; }
.notice { padding: 6px 16px; font-size: 12px; color: var(--muted); }
.page { padding: 0 16px 16px; max-width: 1680px; margin: 0 auto; }
@media (max-width: 760px) { .site { display: none; } .top { gap: 10px; } .link { font-size: 0; } }
</style>
