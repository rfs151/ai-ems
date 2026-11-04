<script setup lang="ts">
import { ems } from '../composables/useEms'

const levelText = ['信息', '警告', '告警']
const levelCls = ['', 'warn', 'bad']
const time = (ts: number) => new Date(ts).toLocaleTimeString('zh-CN', { hour12: false })
</script>

<template>
  <section class="panel">
    <h3>告警与事件 <span class="sub">最近 {{ ems.events.length }} 条</span></h3>
    <div class="scroll">
      <table>
        <tbody>
          <tr v-for="e in ems.events" :key="e.id + '-' + e.ts">
            <td class="num muted">{{ time(e.ts) }}</td>
            <td><span class="tag" :class="levelCls[e.level]">{{ levelText[e.level] ?? e.level }}</span></td>
            <td class="muted">{{ e.source }}</td>
            <td class="text">{{ e.text }}</td>
          </tr>
          <tr v-if="!ems.events.length"><td class="muted">暂无事件</td></tr>
        </tbody>
      </table>
    </div>
  </section>
</template>

<style scoped>
.scroll { max-height: 190px; overflow: auto; }
td.text { width: 100%; white-space: normal; }
</style>
