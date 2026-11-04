<script setup lang="ts">
import { computed } from 'vue'
import { ems, fmt } from '../composables/useEms'

const ctl = computed(() => ems.latest?.ctl)
const devices = computed(() => ems.meta?.devices ?? [])
</script>

<template>
  <section class="panel">
    <h3>控制状态</h3>
    <div class="kvs" v-if="ctl">
      <div><span class="muted">运行模式</span><b>{{ ctl.mode === 'vpp' ? 'VPP 调度' : '计划曲线' }}</b></div>
      <div><span class="muted">安全联锁</span>
        <b :class="ctl.interlock ? 'bad' : 'good'">{{ ctl.interlock ? ctl.interlock_reason : '正常' }}</b></div>
      <div><span class="muted">计划功率</span><b class="num">{{ fmt(ctl.p_base, 1, 'kW') }}</b></div>
      <div><span class="muted">mFRR 分量</span><b class="num">{{ fmt(ctl.p_mfrr, 1, 'kW') }}</b></div>
      <div><span class="muted">FCR 分量</span><b class="num">{{ ctl.fcr_enabled ? fmt(ctl.p_fcr, 1, 'kW') : '退出' }}</b></div>
      <div><span class="muted">LFSM-O 分量</span><b class="num">{{ fmt(ctl.p_lfsmo, 1, 'kW') }}</b></div>
      <div><span class="muted">站级指令</span><b class="num">{{ fmt(ctl.p_cmd, 1, 'kW') }}</b></div>
      <div><span class="muted">可充 / 可放</span><b class="num">{{ ctl.p_chg_max }} / {{ ctl.p_dis_max }} kW</b></div>
      <div v-for="(v, k) in ctl.share" :key="k"><span class="muted">{{ k.toUpperCase() }} 分配</span><b class="num">{{ fmt(v, 1, 'kW') }}</b></div>
    </div>
  </section>

  <section class="panel">
    <h3>设备通信 <span class="sub">Modbus TCP</span></h3>
    <table>
      <thead><tr><th>设备</th><th>耗时</th><th>读</th><th>错</th><th>写</th><th>异常块</th></tr></thead>
      <tbody>
        <tr v-for="d in devices" :key="d.id">
          <td><span class="dot" :class="ems.latest?.dev[d.id]?.online ? 'good' : 'bad'"></span> {{ d.id }}</td>
          <td class="num">{{ ems.latest?.dev[d.id]?.poll_ms ?? '—' }} ms</td>
          <td class="num">{{ ems.latest?.dev[d.id]?.reads ?? '—' }}</td>
          <td class="num">{{ ems.latest?.dev[d.id]?.errors ?? '—' }}</td>
          <td class="num">{{ ems.latest?.dev[d.id]?.writes ?? '—' }}</td>
          <td class="num" :class="{ warn: (ems.latest?.dev[d.id]?.bad_blocks ?? 0) > 0 }">{{ ems.latest?.dev[d.id]?.bad_blocks ?? 0 }}</td>
        </tr>
      </tbody>
    </table>
  </section>

</template>

<style scoped>
.kvs { padding: 8px 14px; display: flex; flex-direction: column; gap: 4px; }
.kvs div { display: flex; justify-content: space-between; gap: 8px; }
.kvs b { font-weight: 600; }
b.good { color: var(--good); } b.bad { color: var(--bad); }
td, th { padding: 5px 10px; font-size: 12px; }
td.warn { color: var(--warn); font-weight: 600; }
</style>
