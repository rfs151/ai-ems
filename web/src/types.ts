export interface PointMeta {
  key: string; name: string; unit: string; area: string; addr: number; type: string
  order?: string; scale?: number; bit?: number; eq?: number; group: string
}
export interface ControlMeta {
  key: string; name: string; area: string; addr: number; type: string
  order?: string; scale?: number; unlock?: { addr: number; value: number }
}
export interface DeviceMeta {
  id: string; name: string; type: 'pcs' | 'bms' | 'meter' | 'dio'; vendor: string; model: string
  pcs: string; points: PointMeta[]; controls: ControlMeta[]
}
export interface Meta { site: string; name: string; ts: number; devices: DeviceMeta[] }

export interface DevStatus { online: boolean; poll_ms: number; reads: number; errors: number; writes: number; bad_blocks?: number }
export interface MfrrAct {
  id: string; dir: number; power: number; scheduled: boolean; t_act: number; t_start: number; t_end: number
  start_level: number; phase: string; level: number; end_reason?: string
}
export interface CtlCfg {
  mfrr_ramp_s: number; mfrr_window_s: number; mfrr_p_max: number
  mfrr_prep_s: number; mfrr_step_s: number; mfrr_direct_lead_s: number; write_interval_ms: number
  fcr_p_max: number; fcr_deadband: number; fcr_full_dev: number
  lfsmo_enabled: boolean; lfsmo_start: number; lfsmo_droop: number
}
export interface CtlStatus {
  mode: 'vpp' | 'schedule'; fcr_enabled: boolean
  p_base: number; p_mfrr: number; p_fcr: number; p_pre: number; p_lfsmo: number; p_req: number; p_cmd: number
  p_chg_max: number; p_dis_max: number
  interlock: boolean; interlock_reason?: string; share: Record<string, number>; write_merged?: Record<string, number>
  mfrr: MfrrAct[]; cfg: CtlCfg
}
export interface RuleStatus {
  id: string; name: string; point: string; cond: string; level: number; action: string
  limit_min: number; limit_day: number; enabled: boolean
  active: boolean; hits: number; suppressed: number; evals: number; avg_us: number; last_hit: number; min_count: number
}
export interface RulesStatus {
  tick_us: number; bench: { n: number; linear_ns: number; bsearch_ns: number }; list: RuleStatus[]
}
export interface Telemetry {
  ts: number; v: Record<string, number>; bad: string[]
  dev: Record<string, DevStatus>; ctl: CtlStatus; rules?: RulesStatus
}
export interface HistoryItem {
  ts: number; v: Record<string, number>
  ctl?: { p_base: number; p_cmd: number; p_mfrr: number; p_fcr: number; p_lfsmo: number; p_req: number }
}
export interface CmdRecord {
  id: string; type: string; summary: string; source: 'visitor' | 'auto'; sent_ts: number
  ok: boolean | null; msg: string; rtt_ms: number | null; edge_ms?: number
}
export interface EmsEvent { id: number; ts: number; level: number; source: string; code: string; text: string }
