import { reactive, shallowRef } from 'vue'
import type { CmdRecord, EmsEvent, HistoryItem, Meta, Telemetry } from '../types'

const HISTORY_MAX = 600   // 曲线保留 10 分钟

export const ems = reactive({
  connected: false,
  edge: 'unknown',
  meta: null as Meta | null,
  latest: null as Telemetry | null,
  events: [] as EmsEvent[],
  cmds: [] as CmdRecord[],
  lastRx: 0,
})
// 曲线数据量大且只追加，用 shallowRef + 版本号触发图表刷新，避免深度响应式开销
export const history = shallowRef<HistoryItem[]>([])
export const historyVersion = shallowRef(0)

function slim(t: Telemetry): HistoryItem {
  const c = t.ctl
  return {
    ts: t.ts, v: t.v,
    ctl: c ? { p_base: c.p_base, p_cmd: c.p_cmd, p_mfrr: c.p_mfrr, p_fcr: c.p_fcr, p_lfsmo: c.p_lfsmo, p_req: c.p_req } : undefined,
  }
}

function upsertCmd(c: CmdRecord) {
  const i = ems.cmds.findIndex((x) => x.id === c.id)
  if (i >= 0) ems.cmds[i] = c
  else {
    ems.cmds.unshift(c)
    if (ems.cmds.length > 50) ems.cmds.length = 50
  }
}

let ws: WebSocket | null = null
let retry = 1000

function handle(msg: any) {
  ems.lastRx = Date.now()
  switch (msg.type) {
    case 'init':
      ems.meta = msg.meta
      ems.edge = msg.status
      ems.latest = msg.latest
      ems.events = [...msg.events].reverse()
      ems.cmds = [...(msg.cmds ?? [])].reverse()
      history.value = msg.history
      historyVersion.value++
      break
    case 'meta': ems.meta = msg.meta; break
    case 'status': ems.edge = msg.status; break
    case 'telemetry': {
      ems.latest = msg.data
      const h = history.value
      h.push(slim(msg.data))
      if (h.length > HISTORY_MAX) h.splice(0, h.length - HISTORY_MAX)
      historyVersion.value++
      break
    }
    case 'cmd': upsertCmd(msg.data); break
    case 'events':
      ems.events = [...msg.data].reverse()
      break
    case 'event':
      ems.events.unshift(msg.data)
      if (ems.events.length > 200) ems.events.length = 200
      break
  }
}

export function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws'
  ws = new WebSocket(`${proto}://${location.host}/ws`)
  ws.onopen = () => { ems.connected = true; retry = 1000 }
  ws.onmessage = (e) => handle(JSON.parse(e.data))
  ws.onclose = () => {
    ems.connected = false
    setTimeout(connect, retry)
    retry = Math.min(retry * 2, 10000)
  }
}

/** 读最新值；不存在或质量不好时返回 undefined */
export function val(id: string): number | undefined {
  const t = ems.latest
  if (!t || t.bad?.includes(id)) return undefined
  return t.v[id]
}

export function fmt(v: number | undefined, digits = 1, unit = ''): string {
  if (v === undefined || Number.isNaN(v)) return '—'
  unit = unit.replace('℃', '°C')   // 部分字体没有 ℃ 字形
  return v.toFixed(digits) + (unit ? ' ' + unit : '')
}

/** 调用云端接口，返回 { ok, msg }（指令类接口还会带回往返时延等字段） */
export async function post(path: string, body: unknown): Promise<{ ok: boolean | null; msg: string; rtt_ms?: number | null }> {
  try {
    const r = await fetch(path, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) })
    const data = await r.json().catch(() => ({}))
    if (!r.ok && !data.msg) return { ok: false, msg: `请求失败（HTTP ${r.status}）` }
    return { ok: data.ok ?? r.ok, msg: data.msg ?? '', rtt_ms: data.rtt_ms }
  } catch {
    return { ok: false, msg: '网络错误' }
  }
}
