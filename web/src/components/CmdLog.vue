<script setup lang="ts">
import { ems } from '../composables/useEms'

const time = (ts: number) => new Date(ts).toLocaleTimeString('zh-CN', { hour12: false })
</script>

<template>
  <section class="panel">
    <h3>指令日志 <span class="sub">云端 → MQTT → 上云进程 → 边缘主进程 → 应答</span></h3>
    <div class="scroll">
      <table>
        <thead><tr><th>时间</th><th>来源</th><th>指令</th><th>结果</th><th class="r">往返</th></tr></thead>
        <tbody>
          <tr v-for="c in ems.cmds" :key="c.id">
            <td class="num muted">{{ time(c.sent_ts) }}</td>
            <td><span class="tag">{{ c.source === 'auto' ? '自动演示' : '访客' }}</span></td>
            <td class="text">{{ c.summary }} <code class="muted">{{ c.id }}</code></td>
            <td class="text">
              <span v-if="c.ok === null" class="muted">等待应答…</span>
              <span v-else :class="c.ok ? 'good' : 'bad'">{{ c.msg }}</span>
            </td>
            <td class="r num">{{ c.rtt_ms != null ? c.rtt_ms + ' ms' : '—' }}</td>
          </tr>
          <tr v-if="!ems.cmds.length"><td colspan="5" class="muted">暂无指令。在右侧控制台下发一条试试；无人操作时会自动演示。</td></tr>
        </tbody>
      </table>
    </div>
  </section>
</template>

<style scoped>
.scroll { max-height: 200px; overflow: auto; }
td.text { white-space: normal; }
.good { color: var(--good); } .bad { color: var(--bad); }
.r { text-align: right; }
</style>
