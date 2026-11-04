#include "algo.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ FCR */
double fcr_power(const fcr_cfg_t *c, double f)
{
    double df = f - c->f_nom;
    double a = fabs(df);
    if (a <= c->deadband)
        return 0.0;
    double k = (a - c->deadband) / (c->full_dev - c->deadband);
    if (k > 1.0)
        k = 1.0;
    return (df < 0 ? 1.0 : -1.0) * k * c->p_max;
}

/* ------------------------------------------------------------------ LFSM-O */
double lfsmo_power(const lfsmo_cfg_t *c, double f, double p_now)
{
    /* 充电或停机时不调整：过频时充电本身就在帮电网，再按下垂"减功率"会变成加大放电 */
    if (!c->enabled || p_now <= 0 || f <= c->f_start || c->droop <= 0)
        return 0.0;
    double dp = p_now * (f - c->f_start) / c->f_nom / c->droop;
    return -(dp < p_now ? dp : p_now);
}

/* ------------------------------------------------------------------ mFRR */
const char *mfrr_phase_name(mfrr_phase_t p)
{
    static const char *n[] = {"等待", "准备", "爬坡", "保持", "降坡", "结束"};
    return (p >= MFRR_PENDING && p <= MFRR_DONE) ? n[p] : "?";
}

void mfrr_init(mfrr_state_t *s) { memset(s, 0, sizeof *s); }

/* 爬坡速率：满功率用 ramp_ms 走完，功率越小速率越小，各条指令爬坡时间一致 */
static double rate(const mfrr_act_t *a, const mfrr_cfg_t *c)
{
    return a->power / (double)c->ramp_ms;
}

/* 按台阶步长取整经过的时间：功率每 step_ms 变一档，而不是每个控制周期都变 */
static double stepped(const mfrr_cfg_t *c, int64_t dt)
{
    return (double)(c->step_ms > 0 ? dt / c->step_ms * c->step_ms : dt);
}

/* 不带符号的当前功率 */
static double level(const mfrr_act_t *a, const mfrr_cfg_t *c, int64_t now)
{
    if (a->phase == MFRR_DONE || a->phase == MFRR_PENDING || a->phase == MFRR_PREP || now < a->t_start)
        return 0.0;
    double r = rate(a, c);
    /* 交付结束时刻本身就属于降坡（用 >=）；用 > 会在结束时刻多保持一个周期 */
    if (now >= a->t_down) {
        double v = a->down_level - stepped(c, now - a->t_down) * r;
        return v > 0 ? v : 0.0;
    }
    double t = stepped(c, now - a->t_start) * r;
    if (a->start_level <= a->power) {
        double v = a->start_level + t;
        return v < a->power ? v : a->power;
    }
    double v = a->start_level - t;      /* 交接时前一条比本条大：降到本条功率 */
    return v > a->power ? v : a->power;
}

double mfrr_act_level(const mfrr_act_t *a, const mfrr_cfg_t *c, int64_t now)
{
    return a->dir * level(a, c, now);
}

static int is_live(const mfrr_act_t *a) { return a->phase != MFRR_DONE; }

static mfrr_act_t *find(mfrr_state_t *s, const char *id)
{
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->act[i].id, id) == 0)
            return &s->act[i];
    return NULL;
}

int mfrr_submit(mfrr_state_t *s, const mfrr_cfg_t *c, const mfrr_cmd_t *cmd, int64_t now,
                char *reason, int rsz)
{
#define REJECT(...) do { snprintf(reason, (size_t)rsz, __VA_ARGS__); return -1; } while (0)
    if (!cmd->id || !cmd->id[0] || strlen(cmd->id) >= sizeof s->act[0].id)
        REJECT("指令编号无效");
    if (find(s, cmd->id))
        REJECT("重复的指令编号");
    if (cmd->dir != 1 && cmd->dir != -1)
        REJECT("方向无效");
    if (cmd->power < c->p_min || cmd->power > c->p_max)
        REJECT("功率需在 %.0f~%.0f kW 之间", c->p_min, c->p_max);
    if (cmd->duration_ms < c->ramp_ms || cmd->duration_ms > 3600 * 1000)
        REJECT("交付时长需在 %lld 秒 ~ 1 小时之间", (long long)(c->ramp_ms / 1000));

    int64_t ta = now;                         /* 激活时刻 */
    if (cmd->scheduled) {
        ta = cmd->t_start;
        if (ta % c->window_ms != 0)
            REJECT("计划激活必须从时间窗口边界开始（%lld 秒整数倍）", (long long)(c->window_ms / 1000));
        /* 窗口边界已过、但仍在准备期内的指令照常执行（只是准备期变短）；
         * 超过准备期才算迟到。不留这段余量，网络稍慢的合法指令就会被拒。 */
        if (now > ta + c->prep_ms || (c->prep_ms == 0 && ta <= now))
            REJECT("计划开始时间已过");
        if (ta - now > c->max_lead_ms)
            REJECT("计划开始时间太远（最多提前 %lld 分钟）", (long long)(c->max_lead_ms / 60000));
    } else if (cmd->t_start > 0) {
        /* 直接激活带期望开始时间：只允许 0 ~ direct_max_lead 的提前量 */
        int64_t lead = cmd->t_start - now;
        if (c->direct_max_lead_ms <= 0)
            REJECT("直接激活不支持指定开始时间");
        if (lead < 0)
            REJECT("直接激活的开始时间已过");
        if (lead > c->direct_max_lead_ms)
            REJECT("直接激活最多提前 %lld 秒", (long long)(c->direct_max_lead_ms / 1000));
        ta = cmd->t_start;
    }
    int64_t t0 = ta + c->prep_ms;             /* 开始爬坡 */
    int64_t t1 = t0 + cmd->duration_ms;       /* 交付结束 */
    int64_t t2 = t1 + c->ramp_ms;             /* 降坡结束 */

    double same = 0;
    for (int i = 0; i < s->n; i++) {
        const mfrr_act_t *o = &s->act[i];
        if (!is_live(o))
            continue;
        int64_t o2 = o->t_down + c->ramp_ms;
        int overlap = o->t_start < t2 && t0 < o2;
        if (!overlap)
            continue;
        if (o->dir != cmd->dir)
            REJECT("与正在执行的反向指令 %s 冲突", o->id);
        if (o->t_down > t0)                   /* 新指令开始时仍在交付的才算叠加；在降坡的会被交接 */
            same += o->power;
    }
    if (same + cmd->power > c->p_max + 1e-6)
        REJECT("同方向叠加后 %.0f kW 超过上限 %.0f kW", same + cmd->power, c->p_max);

    /* 找空位：优先复用已结束的 */
    mfrr_act_t *a = NULL;
    for (int i = 0; i < s->n && !a; i++)
        if (!is_live(&s->act[i]))
            a = &s->act[i];
    if (!a) {
        if (s->n >= MFRR_MAX_ACT)
            REJECT("同时执行的指令过多");
        a = &s->act[s->n++];
    }
    memset(a, 0, sizeof *a);
    snprintf(a->id, sizeof a->id, "%s", cmd->id);
    a->dir = cmd->dir;
    a->power = cmd->power;
    a->scheduled = cmd->scheduled;
    a->t_act = ta;
    a->t_start = t0;
    a->t_end = t1;
    a->t_down = t1;
    a->down_level = cmd->power;
    a->phase = MFRR_PENDING;
    reason[0] = '\0';
    return 0;
#undef REJECT
}

int mfrr_cancel(mfrr_state_t *s, const mfrr_cfg_t *c, const char *id, int64_t now)
{
    mfrr_act_t *a = find(s, id);
    if (!a || !is_live(a))
        return -1;
    snprintf(a->end_reason, sizeof a->end_reason, "cancelled");
    if (a->phase == MFRR_PENDING || a->phase == MFRR_PREP || now < a->t_start) {
        a->phase = MFRR_DONE;
        return 0;
    }
    if (now < a->t_down) {                    /* 交付中：从当前值开始降坡 */
        a->down_level = level(a, c, now);
        a->t_down = now;
        a->t_end = now;
    }
    return 0;
}

double mfrr_update(mfrr_state_t *s, const mfrr_cfg_t *c, int64_t now)
{
    /* 1. 到激活时刻进入准备期；准备期满开始爬坡。
     *    同方向有正在降坡的，从它的当前值接过来（平滑交接） */
    for (int i = 0; i < s->n; i++) {
        mfrr_act_t *a = &s->act[i];
        if (a->phase == MFRR_PENDING && now >= a->t_act)
            a->phase = MFRR_PREP;
        if (a->phase != MFRR_PREP || now < a->t_start)
            continue;
        for (int j = 0; j < s->n; j++) {
            mfrr_act_t *o = &s->act[j];
            if (j == i || o->dir != a->dir || !is_live(o) || o->phase == MFRR_PENDING || o->phase == MFRR_PREP)
                continue;
            if (now >= o->t_down) {
                a->start_level += level(o, c, now);
                o->phase = MFRR_DONE;
                snprintf(o->end_reason, sizeof o->end_reason, "handover");
            }
        }
        a->phase = MFRR_RAMP_UP;
    }

    /* 2. 更新阶段并累加 */
    double total = 0;
    for (int i = 0; i < s->n; i++) {
        mfrr_act_t *a = &s->act[i];
        if (!is_live(a) || a->phase == MFRR_PENDING || a->phase == MFRR_PREP)
            continue;
        double v = level(a, c, now);
        if (now >= a->t_down) {
            if (v <= 0) {
                a->phase = MFRR_DONE;
                if (!a->end_reason[0])
                    snprintf(a->end_reason, sizeof a->end_reason, "finished");
                continue;
            }
            a->phase = MFRR_RAMP_DOWN;
        } else {
            a->phase = fabs(v - a->power) < 1e-9 ? MFRR_HOLD : MFRR_RAMP_UP;
        }
        total += a->dir * v;
    }
    return total;
}
