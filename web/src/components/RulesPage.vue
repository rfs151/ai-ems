<script setup lang="ts">
import { computed, reactive, ref } from 'vue'
import { ems, post } from '../composables/useEms'
import type { RuleStatus } from '../types'
import Console from './Console.vue'

const rules = computed(() => ems.latest?.rules)
const points = computed(() => (ems.meta?.devices ?? []).flatMap((d) => d.points.map((p) => `${d.id}.${p.key}`)))
const levelText = ['信息', '警告', '告警']
const levelCls = ['', 'warn', 'bad']

const empty = { id: '', name: '', point: 'meter.f', cond: '', level: 1, action: 'alarm', limit_min: 0, limit_day: 0, enabled: true }
const form = reactive({ ...empty })
const busy = ref(false)
const result = ref<{ ok: boolean | null; msg: string } | null>(null)

function edit(r: RuleStatus) {
  Object.assign(form, { id: r.id, name: r.name, point: r.point, cond: r.cond, level: r.level, action: r.action,
    limit_min: r.limit_min, limit_day: r.limit_day, enabled: r.enabled })
}
function fresh() { Object.assign(form, empty) }

async function run(path: string, body: unknown) {
  if (busy.value) return
  busy.value = true
  const r = await post(path, body)
  result.value = { ok: r.ok, msg: r.msg }
  busy.value = false
}
const save = () => run('/api/cmd/rule_set', { ...form, id: form.id || undefined })
const del = (id: string) => run('/api/cmd/rule_del', { rule_id: id })
const toggle = (r: RuleStatus) => run('/api/cmd/rule_set', { ...r, enabled: !r.enabled })
const reset = () => run('/api/cmd/rule_reset', {})

const actionText = (a: string) => a === 'stop' ? '停止功率输出' : a.startsWith('do:') ? `写 DO ${a.slice(3)}` : '仅告警'
const time = (ts: number) => (ts ? new Date(ts).toLocaleTimeString('zh-CN', { hour12: false }) : '—')
</script>

<template>
  <div class="rules">
    <div class="main">
      <section class="panel">
        <h3>规则表 <span class="sub">每 200ms 一轮 · 本轮耗时 {{ rules?.tick_us ?? '—' }} µs</span></h3>
        <div class="scroll">
          <table>
            <thead><tr><th>编号</th><th>名称</th><th>触发点位</th><th>条件（TinyExpr）</th><th>动作</th><th>级别</th>
              <th>状态</th><th>命中</th><th>抑制</th><th>求值</th><th>上限</th><th></th></tr></thead>
            <tbody>
              <tr v-for="r in rules?.list ?? []" :key="r.id" :class="{ off: !r.enabled }">
                <td><code>{{ r.id }}</code></td>
                <td>{{ r.name }}</td>
                <td><code>{{ r.point }}</code></td>
                <td class="cond"><code>{{ r.cond || '（变位规则：点位每变化一次记一条）' }}</code></td>
                <td>{{ actionText(r.action) }}</td>
                <td><span class="tag" :class="levelCls[r.level]">{{ levelText[r.level] }}</span></td>
                <td><span class="tag" :class="r.active ? 'bad' : 'good'">{{ !r.enabled ? '停用' : r.active ? '触发中' : '正常' }}</span></td>
                <td class="num">{{ r.hits }}</td>
                <td class="num" :class="{ warn: r.suppressed > 0 }">{{ r.suppressed }}</td>
                <td class="num muted">{{ r.evals }} 次 · {{ r.avg_us }} µs</td>
                <td class="num muted">{{ r.limit_min || '∞' }}/分 · {{ r.limit_day || '∞' }}/天</td>
                <td class="ops">
                  <button class="link" @click="edit(r)">编辑</button>
                  <button class="link" :disabled="busy" @click="toggle(r)">{{ r.enabled ? '停用' : '启用' }}</button>
                  <button class="link" :disabled="busy" @click="del(r.id)">删除</button>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>

      <section class="panel">
        <h3>{{ form.id ? `编辑规则 ${form.id}` : '新增规则' }}
          <span class="sub"><button class="link" @click="fresh">清空</button> · <button class="link" :disabled="busy" @click="reset">恢复默认规则</button></span></h3>
        <div class="form">
          <label>名称<input v-model="form.name" maxlength="40" placeholder="如：电池簇-2 SOC 低"></label>
          <label>触发点位
            <select v-model="form.point"><option v-for="p in points" :key="p" :value="p">{{ p }}</option></select></label>
          <label class="wide">条件表达式<input v-model="form.cond" maxlength="190" placeholder="如 lt(bat2_soc, 20)；留空为变位规则"></label>
          <label>动作
            <select v-model="form.action">
              <option value="alarm">仅告警</option><option value="stop">停止功率输出</option>
              <option value="do:fan_cmd=1">写 DO：启动风机</option>
            </select></label>
          <label>级别
            <select v-model.number="form.level"><option :value="0">信息</option><option :value="1">警告</option><option :value="2">告警</option></select></label>
          <label>每分钟上限<input type="number" min="0" max="60" v-model.number="form.limit_min"></label>
          <label>每天上限<input type="number" min="0" max="256" v-model.number="form.limit_day"></label>
          <div class="wide row">
            <button class="primary" :disabled="busy || !form.name" @click="save">{{ form.id ? '保存修改' : '新增规则' }}</button>
            <span v-if="result" :class="result.ok ? 'ok' : 'err'">{{ result.msg }}</span>
          </div>
        </div>
        <div class="notes">
          <div><b>写法</b>：变量名把点位的「.」换成「_」，如 <code>bat2_soc</code>；可用 gt lt ge le eq ne and or not if max min 和四则运算。表达式在网关上编译，写错会返回出错位置，不影响正在运行的规则。</div>
          <div><b>触发</b>：只有触发点位变化时才对相关规则求值。规则按触发点位排序，点位变化时二分查找命中的规则，不遍历全部规则；首次采到的值只记录，开机时不会误报"变位"。</div>
          <div><b>抑制</b>：同一条规则 1 分钟 / 24 小时内的告警次数有上限（频率在阈值附近抖动时不会刷屏），超出只计入「抑制」，联动动作照常执行。</div>
          <div><b>动作</b>：条件由假变真执行，恢复时撤销（风机关闭、功率闭锁解除）。规则存 SQLite，表结构升级时检查列是否存在再补列，老数据库原地升级。</div>
        </div>
      </section>

      <section class="panel" v-if="rules?.bench">
        <h3>规则查找耗时 <span class="sub">网关启动时实测 · {{ rules.bench.n }} 条规则</span></h3>
        <div class="bench">
          <div><span class="muted">线性查找</span><b class="num">{{ rules.bench.linear_ns }} ns</b></div>
          <div><span class="muted">排序 + 二分查找</span><b class="num good">{{ rules.bench.bsearch_ns }} ns</b></div>
          <div><span class="muted">提升</span><b class="num">{{ rules.bench.bsearch_ns ? (rules.bench.linear_ns / rules.bench.bsearch_ns).toFixed(0) : '—' }} 倍</b></div>
        </div>
      </section>

      <section class="panel">
        <h3>规则告警 <span class="sub">网关 alarm 主题 · 最近 {{ ems.alarms.length }} 条</span></h3>
        <div class="scroll short">
          <table><tbody>
            <tr v-for="a in ems.alarms" :key="a.id">
              <td class="num muted">{{ time(a.ts) }}</td>
              <td><span class="tag" :class="a.code === 'alarm_clear' ? 'good' : levelCls[a.level]">{{ a.code === 'alarm_clear' ? '恢复' : a.code === 'point_change' ? '变位' : levelText[a.level] }}</span></td>
              <td class="text">{{ a.text }}</td>
            </tr>
            <tr v-if="!ems.alarms.length"><td class="muted">暂无告警。可在控制台打开储能舱门、注入频率阶跃来触发规则。</td></tr>
          </tbody></table>
        </div>
      </section>
    </div>
    <aside class="side"><Console /></aside>
  </div>
</template>

<style scoped>
.rules { display: grid; grid-template-columns: minmax(0, 1fr) 300px; gap: 12px; align-items: start; }
.main { display: flex; flex-direction: column; gap: 12px; min-width: 0; }
.scroll { overflow: auto; }
.scroll.short { max-height: 260px; }
td, th { font-size: 12px; padding: 5px 8px; }
td.cond { white-space: normal; min-width: 220px; }
td.text { white-space: normal; width: 100%; }
td.warn { color: var(--warn); font-weight: 600; }
tr.off td { opacity: .5; }
.ops { display: flex; gap: 8px; }
button { font: inherit; font-size: 13px; color: var(--text); background: var(--panel); border: 1px solid var(--border); border-radius: 6px; padding: 4px 10px; cursor: pointer; }
button:disabled { opacity: .5; cursor: default; }
button.link { border: 0; padding: 0; color: var(--accent); background: none; font-size: 12px; }
button.primary { background: var(--accent); border-color: var(--accent); color: #fff; font-weight: 600; }
.form { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 8px 12px; padding: 12px 14px; }
.form label { display: flex; flex-direction: column; gap: 3px; font-size: 12px; color: var(--muted); }
.form .wide { grid-column: span 4; }
.form input, .form select { font: inherit; font-size: 13px; color: var(--text); background: var(--panel); border: 1px solid var(--border); border-radius: 6px; padding: 4px 8px; }
.row { display: flex; align-items: center; gap: 10px; font-size: 13px; }
.ok { color: var(--good); } .err { color: var(--bad); }
.notes { padding: 0 14px 12px; display: flex; flex-direction: column; gap: 6px; font-size: 13px; color: var(--muted); }
.notes b { color: var(--text); font-weight: 600; }
.bench { display: grid; grid-template-columns: repeat(3, 1fr); padding: 12px 14px; gap: 12px; }
.bench div { display: flex; flex-direction: column; gap: 2px; }
.bench b { font-size: 20px; } .bench b.good { color: var(--good); }
@media (max-width: 1000px) { .rules { grid-template-columns: minmax(0, 1fr); } .form { grid-template-columns: 1fr 1fr; } .form .wide { grid-column: span 2; } }
</style>
