/* 功率控制算法（纯函数，不依赖线程、设备和全局状态，便于单元测试）
 *
 * 功率符号：正 = 放电（向电网送电，上调），负 = 充电（下调）。
 * 时间统一用毫秒（调用方传入单调时钟或墙钟，只要一致即可）。
 */
#ifndef EMS_ALGO_H
#define EMS_ALGO_H

#include <stdint.h>

/* ------------------------------------------------------------------ FCR 一次调频 */
typedef struct {
    double f_nom;        /* 50.0 Hz */
    double deadband;     /* 死区，0.01 Hz */
    double full_dev;     /* 满出力频偏，0.2 Hz */
    double p_max;        /* 中标容量 kW */
} fcr_cfg_t;

/* 频率低 → 放电（正），频率高 → 充电（负）；死区外线性，满出力后饱和 */
double fcr_power(const fcr_cfg_t *c, double f);

/* ------------------------------------------------------------------ LFSM-O 过频限功率 */
typedef struct {
    double f_nom;        /* 50.0 Hz */
    double f_start;      /* 启动频率 50.2 Hz */
    double droop;        /* 下垂系数，0.02 = 2% */
    double p_ref;        /* 参考功率 kW（额定容量） */
} lfsmo_cfg_t;

/* 频率超过 f_start 后按下垂减功率（储能可以一直减到充电），否则为 0 */
double lfsmo_power(const lfsmo_cfg_t *c, double f);

/* ------------------------------------------------------------------ mFRR 手动频率恢复备用 */
#define MFRR_MAX_ACT 6

typedef enum {
    MFRR_PENDING,        /* 计划激活，等待开始时刻 */
    MFRR_RAMP_UP,
    MFRR_HOLD,
    MFRR_RAMP_DOWN,
    MFRR_DONE,
} mfrr_phase_t;

typedef struct {
    int64_t ramp_ms;      /* 满功率爬坡时间（演示 30s，实际 12.5 分钟） */
    int64_t window_ms;    /* 计划激活的时间窗口粒度（演示 60s，实际 15 分钟） */
    int64_t max_lead_ms;  /* 计划激活最远可提前多久下发 */
    double p_min;         /* 单条指令最小功率 */
    double p_max;         /* 同方向所有激活之和上限 */
} mfrr_cfg_t;

typedef struct {
    char id[40];
    int dir;              /* +1 上调（放电），-1 下调（充电） */
    double power;         /* 目标功率（正数） */
    int scheduled;        /* 1 计划激活，0 直接激活 */
    int64_t t_start;      /* 开始爬坡时刻 */
    int64_t t_end;        /* 交付结束时刻（此后降坡） */
    double start_level;   /* 爬坡起点（交接时从前一条的当前值开始） */
    int64_t t_down;       /* 开始降坡时刻（取消时会提前） */
    double down_level;    /* 降坡起点 */
    mfrr_phase_t phase;
    char end_reason[16];  /* finished / cancelled / handover */
} mfrr_act_t;

typedef struct {
    mfrr_act_t act[MFRR_MAX_ACT];
    int n;
} mfrr_state_t;

typedef struct {
    const char *id;
    int dir;
    double power;
    int scheduled;
    int64_t t_start;      /* 计划激活的开始时刻；直接激活忽略 */
    int64_t duration_ms;  /* 交付时长（从开始爬坡算起） */
} mfrr_cmd_t;

void mfrr_init(mfrr_state_t *s);

/* 校验并接受指令；失败返回 -1，reason 写入中文原因 */
int mfrr_submit(mfrr_state_t *s, const mfrr_cfg_t *c, const mfrr_cmd_t *cmd, int64_t now,
                char *reason, int reason_size);

/* 取消：正在执行的从当前值降坡，未开始的直接结束 */
int mfrr_cancel(mfrr_state_t *s, const mfrr_cfg_t *c, const char *id, int64_t now);

/* 推进状态（阶段切换、交接），返回所有激活叠加后的功率（带符号） */
double mfrr_update(mfrr_state_t *s, const mfrr_cfg_t *c, int64_t now);

/* 单条激活在 now 时刻的功率（带符号），不修改状态 */
double mfrr_act_level(const mfrr_act_t *a, const mfrr_cfg_t *c, int64_t now);

const char *mfrr_phase_name(mfrr_phase_t p);

#endif
