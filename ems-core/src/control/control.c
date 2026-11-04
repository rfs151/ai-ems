#include "control.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../third_party/cjson/cJSON.h"
#include "../acq/acq.h"
#include "../alarm/event.h"
#include "../rtdb/rtdb.h"
#include "../util/clock.h"
#include "../util/log.h"
#include "algo.h"

#define CYCLE_MS 100
#define MAX_PCS 8
#define RAMP_KW_S 1000.0       /* 站级爬坡率：要跟得上 FCR / LFSM-O 的快速响应 */
#define WRITE_DEADBAND_KW 1.0  /* 设定值变化小于此值不重复下发 */
#define REFRESH_MS 5000        /* 即使不变也定期重发，防止 PCS 重启后丢失设定值 */
#define SOC_MIN 10.0
#define SOC_MAX 95.0
#define SOC_DERATE_BAND 5.0    /* 接近 SOC 边界时线性降额的区间 */

typedef enum { MODE_VPP, MODE_SCHEDULE } mode_t_;

typedef struct {
    const device_cfg_t *dev;
    int i_p, i_run, i_fault;               /* PCS 点在实时库中的下标 */
    int i_soc, i_chg_max, i_dis_max;       /* 所属电池簇 */
    double last_written;
    int64_t last_write_ms;
    double share_kw;                       /* 本周期分到的功率 */
} pcs_ctl_t;

static const fcr_cfg_t FCR_CFG = {50.0, 0.01, 0.2, 200.0};
static const lfsmo_cfg_t LFSM_CFG = {50.0, 50.2, 0.02, 1000.0};
/* 演示参数：爬坡 30 秒、窗口 1 分钟（实际业务为 12.5 分钟、15 分钟） */
static const mfrr_cfg_t MFRR_CFG = {30000, 60000, 30 * 60000, 50.0, 800.0};

static const config_t *g_cfg;
static pcs_ctl_t g_pcs[MAX_PCS];
static int g_npcs;
static int g_i_estop = -1, g_i_fire = -1, g_i_breaker = -1, g_i_freq = -1;
static pthread_t g_thread;
static atomic_int g_running;

/* 以下由 g_lock 保护：控制线程、指令线程、存储线程都会访问 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static mode_t_ g_mode = MODE_VPP;
static int g_fcr_enabled = 1;
static mfrr_state_t g_mfrr;
static mfrr_phase_t g_prev_phase[MFRR_MAX_ACT];
static char g_prev_id[MFRR_MAX_ACT][40];
static struct {
    double f, p_base, p_mfrr, p_fcr, p_lfsmo, p_req, p_cmd, p_chg_max, p_dis_max;
    int f_valid;
    const char *interlock;
} g_st;

static int find_point(const device_cfg_t *dev, const char *key)
{
    char id[64];
    snprintf(id, sizeof id, "%s.%s", dev->id, key);
    return rtdb_find(id);
}

static double get(int idx, double def)
{
    double v;
    uint8_t q;
    if (idx < 0 || rtdb_get(idx, &v, &q) || q != Q_GOOD)
        return def;
    return v;
}

static int good(int idx)
{
    uint8_t q = Q_INVALID;
    return idx >= 0 && rtdb_get(idx, NULL, &q) == 0 && q == Q_GOOD;
}

/* 计划曲线模式：20 分钟一个周期，对应一天的"谷充、峰放"（时间加速 72 倍） */
static double schedule_kw(int64_t now_ms)
{
    double phase = fmod((double)now_ms / 1000.0, 1200.0) / 1200.0;
    if (phase < 0.25)
        return -400.0;
    if (phase < 0.40)
        return 0.0;
    if (phase < 0.65)
        return 500.0;
    if (phase < 0.75)
        return 150.0;
    return 0.0;
}

/* 单个电池簇的可充/可放能力：取 BMS 允许值，并在 SOC 边界附近降额 */
static void pcs_limits(const pcs_ctl_t *c, double *chg, double *dis)
{
    double rated = 500.0;
    double soc = get(c->i_soc, -1);
    *chg = fmin(rated, get(c->i_chg_max, 0));
    *dis = fmin(rated, get(c->i_dis_max, 0));
    if (soc < 0) {              /* SOC 未知：不允许动作 */
        *chg = *dis = 0;
        return;
    }
    *dis *= fmax(0, fmin(1, (soc - SOC_MIN) / SOC_DERATE_BAND));
    *chg *= fmax(0, fmin(1, (SOC_MAX - soc) / SOC_DERATE_BAND));
}

static const char *check_interlock(void)
{
    if (get(g_i_estop, 0) > 0.5)
        return "急停按下";
    if (get(g_i_fire, 0) > 0.5)
        return "消防动作";
    if (g_i_breaker >= 0 && good(g_i_breaker) && get(g_i_breaker, 1) < 0.5)
        return "并网开关分闸";
    return NULL;
}

/* mFRR 阶段变化写入事件（调用方持有 g_lock） */
static void mfrr_events(void)
{
    for (int i = 0; i < g_mfrr.n; i++) {
        const mfrr_act_t *a = &g_mfrr.act[i];
        if (strcmp(g_prev_id[i], a->id) != 0) {     /* 槽位换了新指令 */
            snprintf(g_prev_id[i], sizeof g_prev_id[i], "%s", a->id);
            g_prev_phase[i] = MFRR_PENDING;
        }
        if (a->phase == g_prev_phase[i])
            continue;
        const char *dir = a->dir > 0 ? "上调" : "下调";
        switch (a->phase) {
        case MFRR_RAMP_UP:
            event_push(EV_INFO, "mfrr", "mfrr_start", "mFRR %s %s %.0f kW 开始爬坡", a->id, dir, a->power);
            break;
        case MFRR_HOLD:
            event_push(EV_INFO, "mfrr", "mfrr_hold", "mFRR %s 达到目标 %.0f kW", a->id, a->power);
            break;
        case MFRR_RAMP_DOWN:
            event_push(EV_INFO, "mfrr", "mfrr_down", "mFRR %s 交付结束，开始降坡", a->id);
            break;
        case MFRR_DONE:
            event_push(EV_INFO, "mfrr", "mfrr_done", "mFRR %s 结束（%s）", a->id,
                       strcmp(a->end_reason, "handover") == 0   ? "由后续指令平滑接续"
                       : strcmp(a->end_reason, "cancelled") == 0 ? "已取消"
                                                                 : "正常完成");
            break;
        default: break;
        }
        g_prev_phase[i] = a->phase;
    }
}

static void cycle(double *p_out)
{
    int64_t now = wall_ms();
    double f = get(g_i_freq, NAN);
    int f_valid = !isnan(f) && f > 45 && f < 55;

    pthread_mutex_lock(&g_lock);
    double p_base = g_mode == MODE_SCHEDULE ? schedule_kw(now) : 0.0;
    double p_mfrr = mfrr_update(&g_mfrr, &MFRR_CFG, now);
    double p_fcr = (g_fcr_enabled && f_valid) ? fcr_power(&FCR_CFG, f) : 0.0;
    double p_lfsmo = f_valid ? lfsmo_power(&LFSM_CFG, f) : 0.0;
    mfrr_events();
    pthread_mutex_unlock(&g_lock);

    double p_req = p_base + p_mfrr + p_fcr + p_lfsmo;
    const char *interlock = check_interlock();
    double target = interlock ? 0.0 : p_req;

    /* 汇总可用能力：只算在线、运行、无故障的 PCS */
    double chg_total = 0, dis_total = 0, chg[MAX_PCS], dis[MAX_PCS];
    for (int i = 0; i < g_npcs; i++) {
        pcs_ctl_t *c = &g_pcs[i];
        int usable = good(c->i_p) && get(c->i_run, 0) > 0.5 && get(c->i_fault, 0) < 0.5;
        pcs_limits(c, &chg[i], &dis[i]);
        if (!usable)
            chg[i] = dis[i] = 0;
        chg_total += chg[i];
        dis_total += dis[i];
    }
    target = fmax(-chg_total, fmin(dis_total, target));

    /* 爬坡率限制（联锁时直接到 0） */
    double step = RAMP_KW_S * CYCLE_MS / 1000.0;
    double p = interlock ? 0.0 : fmax(*p_out - step, fmin(*p_out + step, target));
    *p_out = p;

    /* 按可用能力比例分配 */
    for (int i = 0; i < g_npcs; i++) {
        pcs_ctl_t *c = &g_pcs[i];
        double cap = p >= 0 ? dis[i] : chg[i], total = p >= 0 ? dis_total : chg_total;
        c->share_kw = total > 0 ? p * cap / total : 0.0;
        int64_t m = mono_ms();
        if (fabs(c->share_kw - c->last_written) >= WRITE_DEADBAND_KW || m - c->last_write_ms >= REFRESH_MS) {
            if (acq_write(c->dev->id, "p_set", c->share_kw) == 0) {
                c->last_written = c->share_kw;
                c->last_write_ms = m;
            }
        }
    }

    pthread_mutex_lock(&g_lock);
    if (!!interlock != !!g_st.interlock) {
        if (interlock)
            event_push(EV_ALARM, "ems", "interlock", "安全联锁：%s，功率降为 0", interlock);
        else
            event_push(EV_INFO, "ems", "interlock_clear", "安全联锁解除");
    }
    int lfsmo_was = g_st.p_lfsmo < 0, lfsmo_now = p_lfsmo < 0;
    if (lfsmo_was != lfsmo_now)
        event_push(lfsmo_now ? EV_WARN : EV_INFO, "ems", lfsmo_now ? "lfsmo_on" : "lfsmo_off",
                   lfsmo_now ? "频率 %.3f Hz 超过 50.2 Hz，LFSM-O 启动" : "频率 %.3f Hz 恢复，LFSM-O 退出", f);
    g_st.f = f;
    g_st.f_valid = f_valid;
    g_st.p_base = p_base;
    g_st.p_mfrr = p_mfrr;
    g_st.p_fcr = p_fcr;
    g_st.p_lfsmo = p_lfsmo;
    g_st.p_req = p_req;
    g_st.p_cmd = p;
    g_st.p_chg_max = chg_total;
    g_st.p_dis_max = dis_total;
    g_st.interlock = interlock;
    pthread_mutex_unlock(&g_lock);
}

static void *control_main(void *arg)
{
    (void)arg;
    double p = 0;
    int64_t next = mono_ms();
    while (atomic_load(&g_running)) {
        cycle(&p);
        next += CYCLE_MS;
        int64_t wait = next - mono_ms();
        if (wait > 0)
            usleep((useconds_t)wait * 1000);
        else
            next = mono_ms();      /* 超时不追赶 */
    }
    return NULL;
}

static double r1(double v) { return round(v * 10) / 10; }

void control_status(cJSON *obj)
{
    cJSON *c = cJSON_AddObjectToObject(obj, "ctl");
    int64_t now = wall_ms();
    pthread_mutex_lock(&g_lock);
    cJSON_AddStringToObject(c, "mode", g_mode == MODE_VPP ? "vpp" : "schedule");
    cJSON_AddBoolToObject(c, "fcr_enabled", g_fcr_enabled);
    cJSON_AddNumberToObject(c, "p_base", r1(g_st.p_base));
    cJSON_AddNumberToObject(c, "p_mfrr", r1(g_st.p_mfrr));
    cJSON_AddNumberToObject(c, "p_fcr", r1(g_st.p_fcr));
    cJSON_AddNumberToObject(c, "p_lfsmo", r1(g_st.p_lfsmo));
    cJSON_AddNumberToObject(c, "p_req", r1(g_st.p_req));
    cJSON_AddNumberToObject(c, "p_cmd", r1(g_st.p_cmd));
    cJSON_AddNumberToObject(c, "p_chg_max", round(g_st.p_chg_max));
    cJSON_AddNumberToObject(c, "p_dis_max", round(g_st.p_dis_max));
    cJSON_AddBoolToObject(c, "interlock", g_st.interlock != NULL);
    if (g_st.interlock)
        cJSON_AddStringToObject(c, "interlock_reason", g_st.interlock);
    cJSON *sh = cJSON_AddObjectToObject(c, "share");
    for (int i = 0; i < g_npcs; i++)
        cJSON_AddNumberToObject(sh, g_pcs[i].dev->id, r1(g_pcs[i].share_kw));

    cJSON *acts = cJSON_AddArrayToObject(c, "mfrr");
    for (int i = 0; i < g_mfrr.n; i++) {
        const mfrr_act_t *a = &g_mfrr.act[i];
        if (a->phase == MFRR_DONE && now - a->t_down > 120000)   /* 结束 2 分钟后不再上报 */
            continue;
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "id", a->id);
        cJSON_AddNumberToObject(j, "dir", a->dir);
        cJSON_AddNumberToObject(j, "power", a->power);
        cJSON_AddBoolToObject(j, "scheduled", a->scheduled);
        cJSON_AddNumberToObject(j, "t_start", (double)a->t_start);
        cJSON_AddNumberToObject(j, "t_end", (double)a->t_end);
        cJSON_AddNumberToObject(j, "start_level", r1(a->start_level));
        cJSON_AddStringToObject(j, "phase", mfrr_phase_name(a->phase));
        cJSON_AddNumberToObject(j, "level", r1(mfrr_act_level(a, &MFRR_CFG, now)));
        if (a->end_reason[0])
            cJSON_AddStringToObject(j, "end_reason", a->end_reason);
        cJSON_AddItemToArray(acts, j);
    }
    cJSON *cfg = cJSON_AddObjectToObject(c, "cfg");
    cJSON_AddNumberToObject(cfg, "mfrr_ramp_s", (double)MFRR_CFG.ramp_ms / 1000);
    cJSON_AddNumberToObject(cfg, "mfrr_window_s", (double)MFRR_CFG.window_ms / 1000);
    cJSON_AddNumberToObject(cfg, "mfrr_p_max", MFRR_CFG.p_max);
    cJSON_AddNumberToObject(cfg, "fcr_p_max", FCR_CFG.p_max);
    cJSON_AddNumberToObject(cfg, "fcr_deadband", FCR_CFG.deadband);
    cJSON_AddNumberToObject(cfg, "fcr_full_dev", FCR_CFG.full_dev);
    cJSON_AddNumberToObject(cfg, "lfsmo_start", LFSM_CFG.f_start);
    cJSON_AddNumberToObject(cfg, "lfsmo_droop", LFSM_CFG.droop);
    cJSON_AddNumberToObject(cfg, "lfsmo_p_ref", LFSM_CFG.p_ref);
    pthread_mutex_unlock(&g_lock);
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

static double jnum(const cJSON *o, const char *k, double def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(it) ? it->valuedouble : def;
}

int control_command(const char *type, const cJSON *cmd, char *msg, int msz)
{
    int rc = 0;
    int64_t now = wall_ms();
    pthread_mutex_lock(&g_lock);
    if (strcmp(type, "mfrr") == 0) {
        mfrr_cmd_t m = {
            .id = jstr(cmd, "id"),
            .dir = (int)jnum(cmd, "dir", 0),
            .power = jnum(cmd, "power", 0),
            .scheduled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cmd, "scheduled")),
            .t_start = (int64_t)jnum(cmd, "t_start", 0),
            .duration_ms = (int64_t)(jnum(cmd, "duration_s", 0) * 1000),
        };
        rc = mfrr_submit(&g_mfrr, &MFRR_CFG, &m, now, msg, msz);
        if (rc == 0)
            snprintf(msg, (size_t)msz, "已接受：%s %.0f kW，%s", m.dir > 0 ? "上调" : "下调", m.power,
                     m.scheduled ? "计划激活" : "立即执行");
    } else if (strcmp(type, "mfrr_cancel") == 0) {
        const char *target = jstr(cmd, "target");
        rc = target ? mfrr_cancel(&g_mfrr, &MFRR_CFG, target, now) : -1;
        snprintf(msg, (size_t)msz, rc == 0 ? "已取消 %s" : "找不到可取消的指令 %s", target ? target : "");
    } else if (strcmp(type, "mode") == 0) {
        const char *m = jstr(cmd, "mode");
        if (m && strcmp(m, "vpp") == 0)
            g_mode = MODE_VPP;
        else if (m && strcmp(m, "schedule") == 0)
            g_mode = MODE_SCHEDULE;
        else
            rc = -1;
        snprintf(msg, (size_t)msz, rc == 0 ? "运行模式切换为 %s" : "未知模式 %s",
                 rc == 0 ? (g_mode == MODE_VPP ? "VPP 调度" : "计划曲线") : (m ? m : ""));
    } else if (strcmp(type, "fcr") == 0) {
        g_fcr_enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cmd, "enabled"));
        snprintf(msg, (size_t)msz, "FCR 已%s", g_fcr_enabled ? "投入" : "退出");
    } else {
        rc = -1;
        snprintf(msg, (size_t)msz, "未知指令类型 %s", type);
    }
    pthread_mutex_unlock(&g_lock);
    if (rc == 0)
        event_push(EV_INFO, "cloud", type, "云端指令：%s", msg);
    else
        event_push(EV_WARN, "cloud", type, "拒绝云端指令：%s", msg);
    return rc;
}

int control_start(const config_t *cfg)
{
    g_cfg = cfg;
    g_npcs = 0;
    mfrr_init(&g_mfrr);
    for (int d = 0; d < cfg->ndevices && g_npcs < MAX_PCS; d++) {
        const device_cfg_t *dev = &cfg->devices[d];
        if (strcmp(dev->type, "dio") == 0) {
            g_i_estop = find_point(dev, "estop");
            g_i_fire = find_point(dev, "fire");
            g_i_breaker = find_point(dev, "grid_breaker");
        }
        if (strcmp(dev->type, "meter") == 0)
            g_i_freq = find_point(dev, "f");
        if (strcmp(dev->type, "pcs") != 0)
            continue;
        pcs_ctl_t *c = &g_pcs[g_npcs++];
        memset(c, 0, sizeof *c);
        c->dev = dev;
        c->i_p = find_point(dev, "p");
        c->i_run = find_point(dev, "run");
        c->i_fault = find_point(dev, "fault");
        c->i_soc = c->i_chg_max = c->i_dis_max = -1;
        for (int b = 0; b < cfg->ndevices; b++) {
            const device_cfg_t *bms = &cfg->devices[b];
            if (strcmp(bms->type, "bms") == 0 && strcmp(bms->pcs, dev->id) == 0) {
                c->i_soc = find_point(bms, "soc");
                c->i_chg_max = find_point(bms, "p_chg_max");
                c->i_dis_max = find_point(bms, "p_dis_max");
            }
        }
        if (c->i_soc < 0)
            LOGW("control", "%s 没有关联电池簇，将不参与功率分配", dev->id);
    }
    if (g_i_freq < 0)
        LOGW("control", "没有找到电表频率点，FCR / LFSM-O 不会动作");
    atomic_store(&g_running, 1);
    LOGI("control", "%d 台 PCS 参与控制，周期 %d ms，模式 VPP 调度，FCR 投入", g_npcs, CYCLE_MS);
    return pthread_create(&g_thread, NULL, control_main, NULL) ? -1 : 0;
}

void control_stop(void)
{
    if (!atomic_load(&g_running))
        return;
    atomic_store(&g_running, 0);
    pthread_join(g_thread, NULL);
    for (int i = 0; i < g_npcs; i++)       /* 退出前把设定值清零 */
        acq_write(g_pcs[i].dev->id, "p_set", 0);
}
