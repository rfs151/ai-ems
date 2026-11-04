<script setup lang="ts">
import { computed, reactive, ref } from 'vue'
import { ems, fmt, post } from '../composables/useEms'

const form = reactive({ dir: 1, power: 300, activation: 'direct', duration_s: 90 })
const busy = ref(false)
const result = ref<{ ok: boolean | null; msg: string } | null>(null)

const ctl = computed(() => ems.latest?.ctl)
const acts = computed(() => (ctl.value?.mfrr ?? []).filter((a) => a.phase !== '结束'))
const windowS = computed(() => ctl.value?.cfg?.mfrr_window_s ?? 60)
const pMax = computed(() => ctl.value?.cfg?.mfrr_p_max ?? 800)
const faults = computed(() => ({
  bat2: ems.latest?.dev?.bat2?.online === false,
  pcs1: ems.latest?.dev?.pcs1?.online === false,
  estop: (ems.latest?.v['dio.estop'] ?? 0) > 0.5,
}))

async function run(path: string, body: unknown) {
  if (busy.value) return
  busy.value = true
  const r = await post(path, body)
  result.value = { ok: r.ok, msg: r.msg + (r.rtt_ms ? `（往返 ${r.rtt_ms} ms）` : '') }
  busy.value = false
}

const sendMfrr = () => run('/api/cmd/mfrr', { ...form })
const cancel = (id: string) => run('/api/cmd/mfrr_cancel', { target: id })
const setMode = (mode: string) => run('/api/cmd/mode', { mode })
const setFcr = (enabled: boolean) => run('/api/cmd/fcr', { enabled })
const freq = (delta_hz: number) => run('/api/scenario', { action: 'freq_step', delta_hz })
const comm = (device: string, down: boolean) => run('/api/scenario', { action: 'comm', device, down })
const estop = (value: boolean) => run('/api/scenario', { action: 'di', name: 'estop', value })
const reset = () => run('/api/scenario', { action: 'reset' })

const time = (ts: number) => new Date(ts).toLocaleTimeString('zh-CN', { hour12: false })
const phaseCls: Record<string, string> = { 等待: '', 爬坡: 'warn', 保持: 'good', 降坡: 'warn' }
</script>

<template>
  <section class="panel console">
    <h3>演示控制台 <span class="sub">操作会下发到边缘网关</span></h3>

    <div class="block">
      <div class="label">运行模式</div>
      <div class="seg">
        <button :class="{ on: ctl?.mode === 'vpp' }" :disabled="busy" @click="setMode('vpp')">VPP 调度</button>
        <button :class="{ on: ctl?.mode === 'schedule' }" :disabled="busy" @click="setMode('schedule')">计划曲线</button>
      </div>
      <label class="check"><input type="checkbox" :checked="ctl?.fcr_enabled" :disabled="busy"
        @change="setFcr(($event.target as HTMLInputElement).checked)"> FCR 一次调频投入</label>
    </div>

    <div class="block">
      <div class="label">mFRR 调度指令</div>
      <div class="seg">
        <button :class="{ on: form.dir === 1 }" @click="form.dir = 1">上调（放电）</button>
        <button :class="{ on: form.dir === -1 }" @click="form.dir = -1">下调（充电）</button>
      </div>
      <div class="row">
        <span class="muted">功率</span>
        <input type="range" min="50" :max="pMax" step="50" v-model.number="form.power">
        <b class="num">{{ form.power }} kW</b>
      </div>
      <div class="row">
        <span class="muted">激活</span>
        <label><input type="radio" value="direct" v-model="form.activation"> 立即</label>
        <label><input type="radio" value="scheduled" v-model="form.activation"> 下一个 {{ windowS }}s 窗口</label>
      </div>
      <div class="row">
        <span class="muted">交付</span>
        <select v-model.number="form.duration_s">
          <option :value="60">60 秒</option><option :value="90">90 秒</option>
          <option :value="120">120 秒</option><option :value="180">180 秒</option>
        </select>
        <button class="primary" :disabled="busy" @click="sendMfrr">下发指令</button>
      </div>
      <div v-if="acts.length" class="acts">
        <div v-for="a in acts" :key="a.id" class="act">
          <div class="act-head">
            <code>{{ a.id }}</code>
            <span>{{ a.dir > 0 ? '上调' : '下调' }} {{ a.power }} kW</span>
            <span class="tag" :class="phaseCls[a.phase]">{{ a.phase }}</span>
            <button class="link" :disabled="busy" @click="cancel(a.id)">取消</button>
          </div>
          <div class="bar"><i :style="{ width: Math.min(100, Math.abs(a.level) / a.power * 100) + '%' }"></i></div>
          <div class="muted small num">{{ time(a.t_start) }} → {{ time(a.t_end) }} · 当前 {{ fmt(Math.abs(a.level), 0, 'kW') }}</div>
        </div>
      </div>
    </div>

    <div class="block">
      <div class="label">电网频率扰动 <span class="muted">（45 秒内逐渐恢复）</span></div>
      <div class="btns">
        <button :disabled="busy" @click="freq(0.3)">+0.30 Hz<small>LFSM-O</small></button>
        <button :disabled="busy" @click="freq(0.15)">+0.15 Hz<small>FCR 充电</small></button>
        <button :disabled="busy" @click="freq(-0.15)">−0.15 Hz<small>FCR 放电</small></button>
      </div>
    </div>

    <div class="block">
      <div class="label">故障注入</div>
      <label class="check"><input type="checkbox" :checked="faults.bat2" :disabled="busy"
        @change="comm('bat2', ($event.target as HTMLInputElement).checked)"> 电池簇-2 通信中断</label>
      <label class="check"><input type="checkbox" :checked="faults.pcs1" :disabled="busy"
        @change="comm('pcs1', ($event.target as HTMLInputElement).checked)"> PCS-1 通信中断</label>
      <label class="check"><input type="checkbox" :checked="faults.estop" :disabled="busy"
        @change="estop(($event.target as HTMLInputElement).checked)"> 急停按下</label>
      <button class="ghost" :disabled="busy" @click="reset">恢复初始场景</button>
    </div>

    <div v-if="result" class="result" :class="result.ok ? 'ok' : 'err'">{{ result.msg }}</div>
  </section>
</template>

<style scoped>
.block { padding: 10px 14px; border-bottom: 1px solid var(--border); display: flex; flex-direction: column; gap: 6px; }
.label { font-size: 12px; font-weight: 600; }
.seg { display: flex; border: 1px solid var(--border); border-radius: 6px; overflow: hidden; }
.seg button { flex: 1; border: 0; background: var(--panel); color: var(--muted); padding: 5px 0; font: inherit; font-size: 13px; cursor: pointer; }
.seg button + button { border-left: 1px solid var(--border); }
.seg button.on { background: var(--accent); color: #fff; }
.row { display: flex; align-items: center; gap: 8px; font-size: 13px; }
.row input[type=range] { flex: 1; accent-color: var(--accent); }
.row b { min-width: 58px; text-align: right; }
.row select { flex: 1; }
.check { font-size: 13px; display: flex; align-items: center; gap: 6px; }
button, select { font: inherit; font-size: 13px; color: var(--text); background: var(--panel); border: 1px solid var(--border);
  border-radius: 6px; padding: 4px 10px; cursor: pointer; }
button:disabled { opacity: .5; cursor: default; }
button.primary { background: var(--accent); border-color: var(--accent); color: #fff; font-weight: 600; }
button.ghost { align-self: flex-start; }
button.link { border: 0; padding: 0; color: var(--accent); margin-left: auto; background: none; }
.btns { display: grid; grid-template-columns: repeat(3, 1fr); gap: 6px; }
.btns button { display: flex; flex-direction: column; align-items: center; padding: 5px 2px; }
.btns small { font-size: 11px; color: var(--muted); }
.acts { display: flex; flex-direction: column; gap: 6px; margin-top: 4px; }
.act { border: 1px solid var(--border); border-radius: 6px; padding: 6px 8px; }
.act-head { display: flex; align-items: center; gap: 6px; font-size: 13px; }
.bar { height: 4px; background: var(--chip); border-radius: 2px; margin: 5px 0 3px; overflow: hidden; }
.bar i { display: block; height: 100%; background: var(--accent); transition: width .5s; }
.small { font-size: 12px; }
.result { margin: 10px 14px 12px; padding: 6px 10px; border-radius: 6px; font-size: 13px; background: var(--chip); }
.result.ok { color: var(--good); } .result.err { color: var(--bad); }
</style>
