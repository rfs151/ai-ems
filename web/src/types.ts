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

export interface DevStatus { online: boolean; poll_ms: number; reads: number; errors: number; writes: number }
export interface MfrrAct {
  id: string; dir: number; power: number; scheduled: boolean; t_start: number; t_end: number
  start_level: number; phase: string; level: number; end_reason?: string
}
export interface CtlCfg {
  mfrr_ramp_s: number; mfrr_window_s: number; mfrr_p_max: number
  fcr_p_max: number; fcr_deadband: number; fcr_full_dev: number
  lfsmo_start: number; lfsmo_droop: number; lfsmo_p_ref: number
}
export interface CtlStatus {
  mode: 'vpp' | 'schedule'; fcr_enabled: boolean
  p_base: number; p_mfrr: number; p_fcr: number; p_lfsmo: number; p_req: number; p_cmd: number
  p_chg_max: number; p_dis_max: number
  interlock: boolean; interlock_reason?: string; share: Record<string, number>
  mfrr: MfrrAct[]; cfg: CtlCfg
}
export interface Telemetry {
  ts: number; v: Record<string, number>; bad: string[]
  dev: Record<string, DevStatus>; ctl: CtlStatus
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
