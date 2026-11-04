#include "rules.h"

#include <math.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../third_party/cjson/cJSON.h"
#include "../acq/acq.h"
#include "../alarm/event.h"
#include "../control/control.h"
#include "../rtdb/rtdb.h"
#include "../storage/storage.h"
#include "../util/clock.h"
#include "../util/log.h"
#include "rule_core.h"

#define MAX_RULES 32
#define TICK_MS 200
#define BENCH_RULES 1000

typedef enum { ACT_ALARM, ACT_DO, ACT_STOP } act_kind_t;

typedef struct {
    /* 配置（存库） */
    char id[16];
    char name[48];
    char point[48];          /* 触发点位，如 bat2.tmax */
    char cond[192];          /* TinyExpr 表达式；空 = 变位规则 */
    int level;               /* EV_INFO / EV_WARN / EV_ALARM */
    char action[40];         /* alarm / do:fan_cmd=1 / stop */
    int limit_min, limit_day;
    int enabled;
    /* 运行时 */
    int pidx;
    te_expr *expr;
    act_kind_t act;
    char do_dev[16], do_key[32];
    int do_val;
    int primed;              /* 条件规则：已按首个有效值求过一次 */
    int active;              /* 条件当前为真 */
    int recorded;            /* 本次触发是否产生了告警记录（被抑制的不发恢复） */
    uint64_t evals, hits, suppressed;
    double cost_ns;          /* 累计求值耗时 */
    int64_t last_hit;
    raise_log_t log;
} rule_t;

static const config_t *g_cfg;
static sqlite3 *g_db;
static pthread_t g_thread;
static atomic_int g_running;

/* 以下由 g_lock 保护：规则线程求值，指令线程增删改，存储线程读状态 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static rule_t g_rules[MAX_RULES];
static int g_nrules;
static rule_ref_t g_refs[MAX_RULES];
static int g_nrefs;
static int g_npts;
static double *g_vals;                   /* 表达式变量绑定的地址：每个点一个 double */
static watch_t *g_watch;
static char (*g_names)[48];
static te_variable *g_vars;
static int g_nvars;
static rt_point_t *g_snap;
static char g_block[64];                 /* 当前生效的停机闭锁原因 */
static double g_tick_us;                 /* 最近一轮耗时 */
static struct { double linear_ns, bsearch_ns; int n; } g_bench;

/* ------------------------------------------------------------------ 默认规则 */
static const struct {
    const char *id, *name, *point, *cond;
    int level;
    const char *action;
    int limit_min, limit_day;
} DEFAULTS[] = {
    {"R1", "并网点频率越限", "meter.f", "or(gt(meter_f, 50.25), lt(meter_f, 49.75))", EV_WARN, "alarm", 3, 50},
    {"R2", "储能舱门打开禁止运行", "dio.door", "eq(dio_door, 1)", EV_ALARM, "stop", 0, 0},
    {"R3", "PCS 温度高启动风机", "pcs1.temp", "gt(max(pcs1_temp, pcs2_temp), 40)", EV_WARN, "do:fan_cmd=1", 0, 0},
    {"R4", "电池簇-1 SOC 低", "bat1.soc", "lt(bat1_soc, 15)", EV_WARN, "alarm", 0, 0},
    {"R5", "并网开关变位", "dio.grid_breaker", "", EV_INFO, "alarm", 0, 0},
    {"R6", "电池簇-1 单体压差大", "bat1.cell01", "gt(max(bat1_cell01, bat1_cell16) - min(bat1_cell01, bat1_cell16), 0.05)",
     EV_WARN, "alarm", 2, 20},
};

/* ------------------------------------------------------------------ 编译与校验 */
static void free_rule(rule_t *r)
{
    te_free(r->expr);         /* 替换或删除规则时释放旧的语法树，否则每次修改都泄漏一棵 */
    r->expr = NULL;
}

/* 解析动作和触发点、编译表达式；失败返回 -1，why 写原因。调用方持有 g_lock */
static int prepare(rule_t *r, char *why, int wsz)
{
    r->pidx = rtdb_find(r->point);
    if (r->pidx < 0) {
        snprintf(why, (size_t)wsz, "触发点位 %s 不存在", r->point);
        return -1;
    }
    if (strcmp(r->action, "alarm") == 0) {
        r->act = ACT_ALARM;
    } else if (strcmp(r->action, "stop") == 0) {
        r->act = ACT_STOP;
    } else if (strncmp(r->action, "do:", 3) == 0) {
        char key[32];
        int v;
        if (sscanf(r->action + 3, "%31[a-z_]=%d", key, &v) != 2 || (v != 0 && v != 1)) {
            snprintf(why, (size_t)wsz, "动作格式应为 do:<DO 点>=0|1");
            return -1;
        }
        r->do_dev[0] = '\0';
        for (int d = 0; d < g_cfg->ndevices; d++)
            if (config_find_control(&g_cfg->devices[d], key)) {
                snprintf(r->do_dev, sizeof r->do_dev, "%s", g_cfg->devices[d].id);
                break;
            }
        if (!r->do_dev[0]) {
            snprintf(why, (size_t)wsz, "没有名为 %s 的 DO 点", key);
            return -1;
        }
        snprintf(r->do_key, sizeof r->do_key, "%s", key);
        r->do_val = v;
        r->act = ACT_DO;
    } else {
        snprintf(why, (size_t)wsz, "未知动作 %s（alarm / stop / do:<点>=0|1）", r->action);
        return -1;
    }
    te_expr *e = NULL;
    if (r->cond[0]) {
        int err = 0;
        e = te_compile(r->cond, g_vars, g_nvars, &err);
        if (!e) {
            snprintf(why, (size_t)wsz, "表达式第 %d 个字符附近有误（变量名用 bat2_tmax 的写法）", err);
            return -1;
        }
    }
    free_rule(r);
    r->expr = e;
    return 0;
}

static void rebuild_index(void)
{
    g_nrefs = 0;
    for (int i = 0; i < g_nrules; i++)
        if (g_rules[i].enabled && g_rules[i].pidx >= 0)
            g_refs[g_nrefs++] = (rule_ref_t){g_rules[i].pidx, i};
    rule_refs_sort(g_refs, g_nrefs);
}

/* ------------------------------------------------------------------ 存储 */
static void db_save(const rule_t *r)
{
    sqlite3_stmt *st;
    sqlite3_prepare_v2(g_db,
                       "INSERT OR REPLACE INTO rules(id,name,point,cond,level,action,enabled,limit_min,limit_day)"
                       " VALUES(?,?,?,?,?,?,?,?,?)",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, r->id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, r->name, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, r->point, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, r->cond, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 5, r->level);
    sqlite3_bind_text(st, 6, r->action, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 7, r->enabled);
    sqlite3_bind_int(st, 8, r->limit_min);
    sqlite3_bind_int(st, 9, r->limit_day);
    if (sqlite3_step(st) != SQLITE_DONE)
        LOGW("rules", "保存规则 %s 失败：%s", r->id, sqlite3_errmsg(g_db));
    sqlite3_finalize(st);
}

static void load_defaults(void)
{
    sqlite3_exec(g_db, "DELETE FROM rules", NULL, NULL, NULL);
    for (size_t i = 0; i < sizeof DEFAULTS / sizeof DEFAULTS[0]; i++) {
        rule_t r = {0};
        snprintf(r.id, sizeof r.id, "%s", DEFAULTS[i].id);
        snprintf(r.name, sizeof r.name, "%s", DEFAULTS[i].name);
        snprintf(r.point, sizeof r.point, "%s", DEFAULTS[i].point);
        snprintf(r.cond, sizeof r.cond, "%s", DEFAULTS[i].cond);
        snprintf(r.action, sizeof r.action, "%s", DEFAULTS[i].action);
        r.level = DEFAULTS[i].level;
        r.limit_min = DEFAULTS[i].limit_min;
        r.limit_day = DEFAULTS[i].limit_day;
        r.enabled = 1;
        db_save(&r);
    }
}

static int db_load(void)
{
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_db,
                           "SELECT id,name,point,cond,level,action,enabled,limit_min,limit_day FROM rules ORDER BY id",
                           -1, &st, NULL) != SQLITE_OK)
        return -1;
    for (int i = 0; i < g_nrules; i++)
        free_rule(&g_rules[i]);
    g_nrules = 0;
    while (sqlite3_step(st) == SQLITE_ROW && g_nrules < MAX_RULES) {
        rule_t *r = &g_rules[g_nrules];
        memset(r, 0, sizeof *r);
#define COL(i) ((const char *)sqlite3_column_text(st, i) ? (const char *)sqlite3_column_text(st, i) : "")
        snprintf(r->id, sizeof r->id, "%s", COL(0));
        snprintf(r->name, sizeof r->name, "%s", COL(1));
        snprintf(r->point, sizeof r->point, "%s", COL(2));
        snprintf(r->cond, sizeof r->cond, "%s", COL(3));
        snprintf(r->action, sizeof r->action, "%s", COL(5));
#undef COL
        r->level = sqlite3_column_int(st, 4);
        r->enabled = sqlite3_column_int(st, 6);
        r->limit_min = sqlite3_column_int(st, 7);
        r->limit_day = sqlite3_column_int(st, 8);
        char why[96];
        if (prepare(r, why, sizeof why)) {
            LOGW("rules", "规则 %s 无效，已停用：%s", r->id, why);
            r->enabled = 0;
            r->pidx = -1;
        }
        g_nrules++;
    }
    sqlite3_finalize(st);
    rebuild_index();
    return 0;
}

/* ------------------------------------------------------------------ 求值 */
static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static const char *point_text(int idx, char *buf, int size)
{
    const rt_point_t *m = rtdb_meta(idx);
    snprintf(buf, (size_t)size, "%s=%.4g%s", m->id, g_vals[idx], m->def->unit);
    return buf;
}

static void do_action(rule_t *r, int on)
{
    if (r->act == ACT_DO)
        acq_write(r->do_dev, r->do_key, on ? r->do_val : !r->do_val);
}

static void eval_cond(rule_t *r, int64_t now)
{
    double t0 = now_ns();
    double v = te_eval(r->expr);
    r->cost_ns += now_ns() - t0;
    r->evals++;
    int state = !isnan(v) && v != 0;
    char pt[80];
    if (state && !r->active) {
        r->active = 1;
        r->hits++;
        r->last_hit = now;
        r->recorded = raise_allowed(&r->log, now, r->limit_min, r->limit_day);
        if (r->recorded)
            event_push(r->level, r->id, "alarm_raise", "规则 %s「%s」触发（%s）", r->id, r->name,
                       point_text(r->pidx, pt, sizeof pt));
        else
            r->suppressed++;         /* 超过次数上限：动作照常执行，只是不再刷告警记录 */
        do_action(r, 1);
    } else if (!state && r->active) {
        r->active = 0;
        if (r->recorded)
            event_push(EV_INFO, r->id, "alarm_clear", "规则 %s「%s」恢复（%s）", r->id, r->name,
                       point_text(r->pidx, pt, sizeof pt));
        do_action(r, 0);
    }
}

static void eval_change(rule_t *r, int64_t now, double before)
{
    r->evals++;
    r->hits++;
    r->last_hit = now;
    if (raise_allowed(&r->log, now, r->limit_min, r->limit_day))
        event_push(r->level, r->id, "point_change", "规则 %s「%s」：%s 由 %.4g 变为 %.4g", r->id, r->name,
                   rtdb_meta(r->pidx)->id, before, g_vals[r->pidx]);
    else
        r->suppressed++;
}

static void tick(void)
{
    double t0 = now_ns();
    int64_t now = wall_ms();
    rtdb_snapshot(g_snap);
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_npts; i++) {
        int good = g_snap[i].quality == Q_GOOD;
        double before = g_watch[i].last;
        g_vals[i] = good ? g_snap[i].value : NAN;
        /* 只有质量好的值参与变化检测；首次采到的值只记录（启动时不会误报一次"变位"） */
        if (!good || !watch_changed(&g_watch[i], g_vals[i], 1e-6))
            continue;
        /* 二分查找：找到以该点为触发点的第一条规则，向后取完同一点的所有规则 */
        for (int k = rule_refs_lower_bound(g_refs, g_nrefs, i); k < g_nrefs && g_refs[k].point == i; k++) {
            rule_t *r = &g_rules[g_refs[k].rule];
            if (r->expr)
                eval_cond(r, now);
            else
                eval_change(r, now, before);
            r->primed = 1;
        }
    }
    /* 新加载或刚修改的条件规则：用首个有效值求一次，开机时已经满足的条件也能及时告警 */
    for (int i = 0; i < g_nrules; i++) {
        rule_t *r = &g_rules[i];
        if (r->enabled && r->expr && !r->primed && r->pidx >= 0 && !isnan(g_vals[r->pidx])) {
            eval_cond(r, now);
            r->primed = 1;
        }
    }
    /* 停机闭锁：任一 stop 规则处于触发状态就闭锁，原因取第一条 */
    char block[64] = "";
    for (int i = 0; i < g_nrules; i++)
        if (g_rules[i].enabled && g_rules[i].act == ACT_STOP && g_rules[i].active) {
            snprintf(block, sizeof block, "规则 %s %s", g_rules[i].id, g_rules[i].name);
            break;
        }
    int block_changed = strcmp(block, g_block) != 0;
    if (block_changed)
        snprintf(g_block, sizeof g_block, "%s", block);
    g_tick_us = (now_ns() - t0) / 1000;
    pthread_mutex_unlock(&g_lock);
    if (block_changed)
        control_rule_block(block);
}

static void *rules_main(void *arg)
{
    (void)arg;
    int64_t next = mono_ms();
    while (atomic_load(&g_running)) {
        tick();
        next += TICK_MS;
        int64_t wait = next - mono_ms();
        if (wait > 0)
            usleep((useconds_t)wait * 1000);
        else
            next = mono_ms();
    }
    return NULL;
}

/* 启动时测一次：1000 条规则下线性查找与二分查找的平均耗时（技术要点页展示） */
static void bench(void)
{
    rule_ref_t *refs = malloc(BENCH_RULES * sizeof *refs);
    if (!refs)
        return;
    unsigned seed = 12345;
    for (int i = 0; i < BENCH_RULES; i++)
        refs[i] = (rule_ref_t){(int)((seed = seed * 1103515245u + 12345u) >> 8) % 2000, i};
    rule_refs_sort(refs, BENCH_RULES);
    const int loops = 200000;
    volatile int sink = 0;
    double t0 = now_ns();
    for (int i = 0; i < loops; i++)
        sink += rule_refs_linear(refs, BENCH_RULES, (i * 7919) % 2000);
    double t1 = now_ns();
    for (int i = 0; i < loops; i++)
        sink += rule_refs_lower_bound(refs, BENCH_RULES, (i * 7919) % 2000);
    double t2 = now_ns();
    (void)sink;
    g_bench.n = BENCH_RULES;
    g_bench.linear_ns = (t1 - t0) / loops;
    g_bench.bsearch_ns = (t2 - t1) / loops;
    free(refs);
    LOGI("rules", "%d 条规则查找：线性 %.0f ns，二分 %.0f ns", BENCH_RULES, g_bench.linear_ns, g_bench.bsearch_ns);
}

/* ------------------------------------------------------------------ 接口 */
int rules_start(const config_t *cfg)
{
    g_cfg = cfg;
    g_db = storage_open(cfg->db_path);
    if (!g_db)
        return -1;
    /* 第一版表结构没有次数上限两列；老数据库启动时原地补列 */
    sqlite3_exec(g_db,
                 "CREATE TABLE IF NOT EXISTS rules(id TEXT PRIMARY KEY, name TEXT NOT NULL, point TEXT NOT NULL,"
                 " cond TEXT NOT NULL DEFAULT '', level INTEGER NOT NULL DEFAULT 1,"
                 " action TEXT NOT NULL DEFAULT 'alarm', enabled INTEGER NOT NULL DEFAULT 1)",
                 NULL, NULL, NULL);
    if (storage_ensure_column(g_db, "rules", "limit_min", "INTEGER NOT NULL DEFAULT 0") < 0 ||
        storage_ensure_column(g_db, "rules", "limit_day", "INTEGER NOT NULL DEFAULT 0") < 0)
        return -1;

    g_npts = rtdb_count();
    size_t n = (size_t)(g_npts ? g_npts : 1);
    g_vals = calloc(n, sizeof *g_vals);
    g_watch = calloc(n, sizeof *g_watch);
    g_names = calloc(n, sizeof *g_names);
    g_snap = calloc(n, sizeof *g_snap);
    g_vars = calloc(n + RULE_FUNC_COUNT, sizeof *g_vars);
    if (!g_vals || !g_watch || !g_names || !g_snap || !g_vars)
        return -1;
    for (int i = 0; i < g_npts; i++) {
        g_vals[i] = NAN;
        rule_var_name(rtdb_meta(i)->id, g_names[i], sizeof g_names[i]);
        g_vars[i] = (te_variable){g_names[i], &g_vals[i], TE_VARIABLE, NULL};
    }
    memcpy(&g_vars[g_npts], rule_funcs(), RULE_FUNC_COUNT * sizeof *g_vars);
    g_nvars = g_npts + RULE_FUNC_COUNT;

    sqlite3_stmt *st;
    int count = 0;
    if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM rules", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            count = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    pthread_mutex_lock(&g_lock);
    if (count == 0)
        load_defaults();
    db_load();
    pthread_mutex_unlock(&g_lock);
    bench();

    atomic_store(&g_running, 1);
    LOGI("rules", "已加载 %d 条规则，%d 个触发索引，%d 个变量", g_nrules, g_nrefs, g_npts);
    return pthread_create(&g_thread, NULL, rules_main, NULL) ? -1 : 0;
}

void rules_stop(void)
{
    if (!atomic_load(&g_running))
        return;
    atomic_store(&g_running, 0);
    pthread_join(g_thread, NULL);
    for (int i = 0; i < g_nrules; i++)
        free_rule(&g_rules[i]);
    g_nrules = 0;
    sqlite3_close(g_db);
    g_db = NULL;
    free(g_vals);
    free(g_watch);
    free(g_names);
    free(g_snap);
    free(g_vars);
    g_vals = NULL;
    g_watch = NULL;
    g_names = NULL;
    g_snap = NULL;
    g_vars = NULL;
}

void rules_status(cJSON *obj)
{
    cJSON *o = cJSON_AddObjectToObject(obj, "rules");
    int64_t now = wall_ms();
    pthread_mutex_lock(&g_lock);
    cJSON_AddNumberToObject(o, "tick_us", round(g_tick_us * 10) / 10);
    cJSON *b = cJSON_AddObjectToObject(o, "bench");
    cJSON_AddNumberToObject(b, "n", g_bench.n);
    cJSON_AddNumberToObject(b, "linear_ns", round(g_bench.linear_ns));
    cJSON_AddNumberToObject(b, "bsearch_ns", round(g_bench.bsearch_ns));
    cJSON *list = cJSON_AddArrayToObject(o, "list");
    for (int i = 0; i < g_nrules; i++) {
        const rule_t *r = &g_rules[i];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "id", r->id);
        cJSON_AddStringToObject(j, "name", r->name);
        cJSON_AddStringToObject(j, "point", r->point);
        cJSON_AddStringToObject(j, "cond", r->cond);
        cJSON_AddNumberToObject(j, "level", r->level);
        cJSON_AddStringToObject(j, "action", r->action);
        cJSON_AddNumberToObject(j, "limit_min", r->limit_min);
        cJSON_AddNumberToObject(j, "limit_day", r->limit_day);
        cJSON_AddBoolToObject(j, "enabled", r->enabled);
        cJSON_AddBoolToObject(j, "active", r->active);
        cJSON_AddNumberToObject(j, "hits", (double)r->hits);
        cJSON_AddNumberToObject(j, "suppressed", (double)r->suppressed);
        cJSON_AddNumberToObject(j, "evals", (double)r->evals);
        cJSON_AddNumberToObject(j, "avg_us", r->evals ? round(r->cost_ns / (double)r->evals / 10) / 100 : 0);
        cJSON_AddNumberToObject(j, "last_hit", (double)r->last_hit);
        cJSON_AddNumberToObject(j, "min_count", raise_count(&r->log, now, 60 * 1000));
        cJSON_AddItemToArray(list, j);
    }
    pthread_mutex_unlock(&g_lock);
}

static rule_t *find_rule(const char *id)
{
    for (int i = 0; i < g_nrules; i++)
        if (strcmp(g_rules[i].id, id) == 0)
            return &g_rules[i];
    return NULL;
}

static const char *jstr(const cJSON *o, const char *k, const char *def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(it) ? it->valuestring : def;
}

static int jint(const cJSON *o, const char *k, int def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(it))
        return cJSON_IsTrue(it);
    return cJSON_IsNumber(it) ? (int)it->valuedouble : def;
}

/* 新增或修改：先在临时副本上校验、编译，成功后再替换，失败不影响正在运行的规则 */
static int rule_set(const cJSON *j, char *msg, int msz)
{
    if (!cJSON_IsObject(j)) {
        snprintf(msg, (size_t)msz, "缺少 rule 对象");
        return -1;
    }
    const char *id = jstr(j, "id", "");
    rule_t *old = id[0] ? find_rule(id) : NULL;
    rule_t r = {0};
    if (old) {
        snprintf(r.id, sizeof r.id, "%s", old->id);
    } else {
        if (g_nrules >= MAX_RULES) {
            snprintf(msg, (size_t)msz, "规则数已达上限 %d", MAX_RULES);
            return -1;
        }
        if (id[0] && (strlen(id) >= sizeof r.id || strspn(id, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != strlen(id))) {
            snprintf(msg, (size_t)msz, "规则编号只能用大写字母、数字和下划线");
            return -1;
        }
        if (id[0]) {
            snprintf(r.id, sizeof r.id, "%s", id);
        } else {
            for (int k = 1; k < 1000; k++) {
                snprintf(r.id, sizeof r.id, "R%d", k);
                if (!find_rule(r.id))
                    break;
            }
        }
    }
    const char *name = jstr(j, "name", old ? old->name : "");
    const char *point = jstr(j, "point", old ? old->point : "");
    const char *cond = jstr(j, "cond", old ? old->cond : "");
    const char *action = jstr(j, "action", old ? old->action : "alarm");
    if (!name[0] || strlen(name) >= sizeof r.name || strlen(point) >= sizeof r.point ||
        strlen(cond) >= sizeof r.cond || strlen(action) >= sizeof r.action) {
        snprintf(msg, (size_t)msz, "名称不能为空，各字段不能超长");
        return -1;
    }
    snprintf(r.name, sizeof r.name, "%s", name);
    snprintf(r.point, sizeof r.point, "%s", point);
    snprintf(r.cond, sizeof r.cond, "%s", cond);
    snprintf(r.action, sizeof r.action, "%s", action);
    r.level = jint(j, "level", old ? old->level : EV_WARN);
    r.limit_min = jint(j, "limit_min", old ? old->limit_min : 0);
    r.limit_day = jint(j, "limit_day", old ? old->limit_day : 0);
    r.enabled = jint(j, "enabled", old ? old->enabled : 1);
    if (r.level < EV_INFO || r.level > EV_ALARM || r.limit_min < 0 || r.limit_min > 60 || r.limit_day < 0 ||
        r.limit_day > RAISE_LOG_MAX) {
        snprintf(msg, (size_t)msz, "级别 0~2，每分钟上限 0~60，每天上限 0~%d", RAISE_LOG_MAX);
        return -1;
    }
    if (prepare(&r, msg, msz))
        return -1;

    if (old) {
        if (old->active)
            do_action(old, 0);      /* 修改前撤销旧规则的动作，新规则会重新求值 */
        free_rule(old);
        *old = r;
    } else {
        g_rules[g_nrules++] = r;
    }
    db_save(&r);
    rebuild_index();
    snprintf(msg, (size_t)msz, "规则 %s「%s」已%s", r.id, r.name, old ? "更新" : "新增");
    return 0;
}

int rules_command(const char *type, const cJSON *cmd, char *msg, int msz)
{
    int rc = 0;
    pthread_mutex_lock(&g_lock);
    if (strcmp(type, "rule_set") == 0) {
        rc = rule_set(cJSON_GetObjectItemCaseSensitive(cmd, "rule"), msg, msz);
    } else if (strcmp(type, "rule_del") == 0) {
        const char *id = jstr(cmd, "rule_id", "");
        rule_t *r = find_rule(id);
        if (!r) {
            rc = -1;
            snprintf(msg, (size_t)msz, "规则 %s 不存在", id);
        } else {
            if (r->active)
                do_action(r, 0);
            free_rule(r);
            char sql[96];
            snprintf(sql, sizeof sql, "DELETE FROM rules WHERE id='%s'", r->id);   /* id 已在新增时限定字符集 */
            sqlite3_exec(g_db, sql, NULL, NULL, NULL);
            snprintf(msg, (size_t)msz, "规则 %s 已删除", r->id);
            int idx = (int)(r - g_rules);
            memmove(r, r + 1, (size_t)(g_nrules - idx - 1) * sizeof *r);
            g_nrules--;
            rebuild_index();
        }
    } else if (strcmp(type, "rule_reset") == 0) {
        for (int i = 0; i < g_nrules; i++)
            if (g_rules[i].active)
                do_action(&g_rules[i], 0);
        load_defaults();
        db_load();
        snprintf(msg, (size_t)msz, "规则已恢复为默认的 %d 条", g_nrules);
    } else {
        rc = -1;
        snprintf(msg, (size_t)msz, "未知指令类型 %s", type);
    }
    pthread_mutex_unlock(&g_lock);
    event_push(rc ? EV_WARN : EV_INFO, "cloud", type, "%s：%s", rc ? "拒绝云端指令" : "云端指令", msg);
    return rc;
}
