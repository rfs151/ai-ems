#include "storage.h"

#include <math.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../third_party/cjson/cJSON.h"
#include "../alarm/event.h"
#include "../rtdb/rtdb.h"
#include "../util/clock.h"
#include "../util/log.h"

#define SLOW_STORE_EVERY 10      /* 慢组点每 10 秒入库一次 */
#define OUTBOX_MAX_ROWS 20000    /* 断网时发件箱上限，超出丢弃最旧的遥测（事件保留） */

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS points(id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL);"
    "CREATE TABLE IF NOT EXISTS telemetry(point INTEGER NOT NULL, ts INTEGER NOT NULL,"
    "  value REAL, quality INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS idx_telemetry_point_ts ON telemetry(point, ts);"
    "CREATE INDEX IF NOT EXISTS idx_telemetry_ts ON telemetry(ts);"
    "CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL,"
    "  level INTEGER, source TEXT, code TEXT, text TEXT);"
    "CREATE TABLE IF NOT EXISTS outbox(id INTEGER PRIMARY KEY AUTOINCREMENT, topic TEXT NOT NULL,"
    "  payload TEXT NOT NULL, qos INTEGER NOT NULL DEFAULT 0, retain INTEGER NOT NULL DEFAULT 0,"
    "  kind TEXT NOT NULL DEFAULT 'telemetry', created INTEGER NOT NULL);";

static const config_t *g_cfg;
static storage_status_fn g_status_fn;
static sqlite3 *g_db;
static pthread_t g_thread;
static atomic_int g_running;
static int *g_point_ids;          /* rtdb 下标 → points 表 id */
static rt_point_t *g_snap;

sqlite3 *storage_open(const char *path)
{
    sqlite3 *db;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        LOGE("storage", "打开数据库 %s 失败：%s", path, sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    /* WAL：读写不互斥；NORMAL：WAL 下断电最多丢最后一个事务，不会损坏 */
    sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_busy_timeout(db, 2000);
    return db;
}

static int exec(const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &err) != SQLITE_OK) {
        LOGE("storage", "SQL 失败：%s（%.60s）", err, sql);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static void outbox_put(const char *suffix, const char *payload, int qos, int retain, const char *kind)
{
    char topic[96];
    snprintf(topic, sizeof topic, "ems/%s/%s", g_cfg->site_id, suffix);
    sqlite3_stmt *st;
    sqlite3_prepare_v2(g_db,
                       "INSERT INTO outbox(topic,payload,qos,retain,kind,created) VALUES(?,?,?,?,?,?)",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, topic, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, payload, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, qos);
    sqlite3_bind_int(st, 4, retain);
    sqlite3_bind_text(st, 5, kind, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 6, wall_ms());
    if (sqlite3_step(st) != SQLITE_DONE)
        LOGW("storage", "写发件箱失败：%s", sqlite3_errmsg(g_db));
    sqlite3_finalize(st);
}

/* 数值保留 3 位小数，报文更短 */
static double r3(double v) { return round(v * 1000) / 1000; }

static const char *area_name(area_t a)
{
    static const char *n[] = {"hr", "ir", "co", "di"};
    return n[a];
}

static const char *type_name(dtype_t t)
{
    static const char *n[] = {"u16", "i16", "u32", "i32", "f32", "bit"};
    return n[t];
}

/* 站点元数据（保留消息）：云端据此展示设备列表和点表页 */
static void publish_meta(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "site", g_cfg->site_id);
    cJSON_AddStringToObject(root, "name", g_cfg->site_name);
    cJSON_AddNumberToObject(root, "ts", (double)wall_ms());
    cJSON *devs = cJSON_AddArrayToObject(root, "devices");
    for (int d = 0; d < g_cfg->ndevices; d++) {
        const device_cfg_t *dev = &g_cfg->devices[d];
        cJSON *jd = cJSON_CreateObject();
        cJSON_AddStringToObject(jd, "id", dev->id);
        cJSON_AddStringToObject(jd, "name", dev->name);
        cJSON_AddStringToObject(jd, "type", dev->type);
        cJSON_AddStringToObject(jd, "vendor", dev->vendor);
        cJSON_AddStringToObject(jd, "model", dev->model);
        cJSON_AddStringToObject(jd, "pcs", dev->pcs);
        cJSON *pts = cJSON_AddArrayToObject(jd, "points");
        for (int i = 0; i < dev->npoints; i++) {
            const point_def_t *p = &dev->points[i];
            cJSON *jp = cJSON_CreateObject();
            cJSON_AddStringToObject(jp, "key", p->key);
            cJSON_AddStringToObject(jp, "name", p->name);
            cJSON_AddStringToObject(jp, "unit", p->unit);
            cJSON_AddStringToObject(jp, "area", area_name(p->area));
            cJSON_AddNumberToObject(jp, "addr", p->addr);
            cJSON_AddStringToObject(jp, "type", type_name(p->type));
            if (point_reg_count(p) == 2)
                cJSON_AddStringToObject(jp, "order", p->word_swap ? "CDAB" : "ABCD");
            if (p->scale != 1.0)
                cJSON_AddNumberToObject(jp, "scale", p->scale);
            if (p->bit >= 0)
                cJSON_AddNumberToObject(jp, "bit", p->bit);
            if (p->has_eq)
                cJSON_AddNumberToObject(jp, "eq", (double)p->eq);
            cJSON_AddStringToObject(jp, "group", p->group == GROUP_FAST ? "fast" : "slow");
            cJSON_AddItemToArray(pts, jp);
        }
        cJSON *ctl = cJSON_AddArrayToObject(jd, "controls");
        for (int i = 0; i < dev->ncontrols; i++) {
            const control_def_t *c = &dev->controls[i];
            cJSON *jc = cJSON_CreateObject();
            cJSON_AddStringToObject(jc, "key", c->def.key);
            cJSON_AddStringToObject(jc, "name", c->def.name);
            cJSON_AddStringToObject(jc, "area", area_name(c->def.area));
            cJSON_AddNumberToObject(jc, "addr", c->def.addr);
            cJSON_AddStringToObject(jc, "type", type_name(c->def.type));
            if (point_reg_count(&c->def) == 2)
                cJSON_AddStringToObject(jc, "order", c->def.word_swap ? "CDAB" : "ABCD");
            if (c->def.scale != 1.0)
                cJSON_AddNumberToObject(jc, "scale", c->def.scale);
            if (c->has_unlock) {
                cJSON *un = cJSON_AddObjectToObject(jc, "unlock");
                cJSON_AddNumberToObject(un, "addr", c->unlock_addr);
                cJSON_AddNumberToObject(un, "value", c->unlock_value);
            }
            cJSON_AddItemToArray(ctl, jc);
        }
        cJSON_AddItemToArray(devs, jd);
    }
    char *s = cJSON_PrintUnformatted(root);
    outbox_put("meta", s, 1, 1, "meta");
    free(s);
    cJSON_Delete(root);
}

static int load_point_ids(void)
{
    int n = rtdb_count();
    g_point_ids = calloc((size_t)(n ? n : 1), sizeof *g_point_ids);
    sqlite3_stmt *ins, *sel;
    sqlite3_prepare_v2(g_db, "INSERT OR IGNORE INTO points(name) VALUES(?)", -1, &ins, NULL);
    sqlite3_prepare_v2(g_db, "SELECT id FROM points WHERE name=?", -1, &sel, NULL);
    for (int i = 0; i < n; i++) {
        const char *id = rtdb_meta(i)->id;
        sqlite3_bind_text(ins, 1, id, -1, SQLITE_STATIC);
        sqlite3_step(ins);
        sqlite3_reset(ins);
        sqlite3_bind_text(sel, 1, id, -1, SQLITE_STATIC);
        if (sqlite3_step(sel) == SQLITE_ROW)
            g_point_ids[i] = sqlite3_column_int(sel, 0);
        sqlite3_reset(sel);
    }
    sqlite3_finalize(ins);
    sqlite3_finalize(sel);
    return 0;
}

static void store_telemetry(int n, int64_t now, int store_slow)
{
    sqlite3_stmt *st;
    sqlite3_prepare_v2(g_db, "INSERT INTO telemetry(point,ts,value,quality) VALUES(?,?,?,?)", -1,
                       &st, NULL);
    for (int i = 0; i < n; i++) {
        const rt_point_t *p = &g_snap[i];
        if (p->def->group == GROUP_SLOW && !store_slow)
            continue;
        sqlite3_bind_int(st, 1, g_point_ids[i]);
        sqlite3_bind_int64(st, 2, now);
        if (p->quality == Q_GOOD)
            sqlite3_bind_double(st, 3, p->value);
        else
            sqlite3_bind_null(st, 3);
        sqlite3_bind_int(st, 4, p->quality);
        sqlite3_step(st);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
}

static void queue_telemetry(int n, int64_t now)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "ts", (double)now);
    cJSON *v = cJSON_AddObjectToObject(root, "v");
    cJSON *bad = cJSON_AddArrayToObject(root, "bad");
    for (int i = 0; i < n; i++) {
        if (g_snap[i].quality == Q_GOOD)
            cJSON_AddNumberToObject(v, g_snap[i].id, r3(g_snap[i].value));
        else
            cJSON_AddItemToArray(bad, cJSON_CreateString(g_snap[i].id));
    }
    cJSON *devs = cJSON_AddObjectToObject(root, "dev");
    for (int d = 0; d < g_cfg->ndevices; d++) {
        rt_device_t st;
        rtdb_device_get(d, &st);
        cJSON *jd = cJSON_AddObjectToObject(devs, g_cfg->devices[d].id);
        cJSON_AddBoolToObject(jd, "online", st.online);
        cJSON_AddNumberToObject(jd, "poll_ms", st.poll_ms);
        cJSON_AddNumberToObject(jd, "reads", (double)st.reads);
        cJSON_AddNumberToObject(jd, "errors", (double)st.errors);
        cJSON_AddNumberToObject(jd, "writes", (double)st.writes);
    }
    if (g_status_fn)
        g_status_fn(root);
    char *s = cJSON_PrintUnformatted(root);
    outbox_put("telemetry", s, 0, 0, "telemetry");
    free(s);
    cJSON_Delete(root);
}

/* 最近 50 条事件快照（保留消息）：云端重启或新订阅时能拿回最近事件 */
static void queue_recent_events(void)
{
    sqlite3_stmt *st;
    sqlite3_prepare_v2(g_db,
                       "SELECT id,ts,level,source,code,text FROM events ORDER BY id DESC LIMIT 50",
                       -1, &st, NULL);
    cJSON *arr = cJSON_CreateArray();
    while (sqlite3_step(st) == SQLITE_ROW) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "id", (double)sqlite3_column_int64(st, 0));
        cJSON_AddNumberToObject(j, "ts", (double)sqlite3_column_int64(st, 1));
        cJSON_AddNumberToObject(j, "level", sqlite3_column_int(st, 2));
        cJSON_AddStringToObject(j, "source", (const char *)sqlite3_column_text(st, 3));
        cJSON_AddStringToObject(j, "code", (const char *)sqlite3_column_text(st, 4));
        cJSON_AddStringToObject(j, "text", (const char *)sqlite3_column_text(st, 5));
        cJSON_AddItemToArray(arr, j);
    }
    sqlite3_finalize(st);
    char *s = cJSON_PrintUnformatted(arr);
    outbox_put("events", s, 1, 1, "events");
    free(s);
    cJSON_Delete(arr);
}

/* 返回本次入库的事件数 */
static int store_events(void)
{
    int total = 0;
    event_t evs[64];
    int n;
    sqlite3_stmt *st;
    sqlite3_prepare_v2(g_db, "INSERT INTO events(ts,level,source,code,text) VALUES(?,?,?,?,?)", -1,
                       &st, NULL);
    while ((n = event_pop(evs, 64)) > 0) {
        total += n;
        for (int i = 0; i < n; i++) {
            const event_t *e = &evs[i];
            sqlite3_bind_int64(st, 1, e->ts);
            sqlite3_bind_int(st, 2, e->level);
            sqlite3_bind_text(st, 3, e->source, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 4, e->code, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 5, e->text, -1, SQLITE_STATIC);
            sqlite3_step(st);
            sqlite3_reset(st);

            cJSON *j = cJSON_CreateObject();
            cJSON_AddNumberToObject(j, "id", (double)sqlite3_last_insert_rowid(g_db));
            cJSON_AddNumberToObject(j, "ts", (double)e->ts);
            cJSON_AddNumberToObject(j, "level", e->level);
            cJSON_AddStringToObject(j, "source", e->source);
            cJSON_AddStringToObject(j, "code", e->code);
            cJSON_AddStringToObject(j, "text", e->text);
            char *s = cJSON_PrintUnformatted(j);
            outbox_put("event", s, 1, 0, "event");
            free(s);
            cJSON_Delete(j);
        }
    }
    sqlite3_finalize(st);
    return total;
}

static void housekeeping(int64_t now)
{
    char sql[256];
    snprintf(sql, sizeof sql, "DELETE FROM telemetry WHERE ts < %lld;",
             (long long)(now - (int64_t)g_cfg->retention_hours * 3600 * 1000));
    exec(sql);
    snprintf(sql, sizeof sql,
             "DELETE FROM outbox WHERE kind='telemetry' AND id <= "
             "(SELECT id FROM outbox WHERE kind='telemetry' ORDER BY id DESC LIMIT 1 OFFSET %d);",
             OUTBOX_MAX_ROWS);
    exec(sql);
    exec("DELETE FROM events WHERE id <= (SELECT id FROM events ORDER BY id DESC LIMIT 1 OFFSET 5000);");
}

static void *storage_main(void *arg)
{
    (void)arg;
    int64_t next = mono_ms() + 1000;
    int tick = 0, events_dirty = 1, last_snapshot = -100;
    while (atomic_load(&g_running)) {
        int64_t wait = next - mono_ms();
        if (wait > 0)
            usleep((useconds_t)(wait > 200 ? 200 : wait) * 1000);  /* 分段睡眠，便于及时退出 */
        if (mono_ms() < next)
            continue;
        next += 1000;

        int64_t t0 = mono_ms(), now = wall_ms();
        int n = rtdb_snapshot(g_snap);
        exec("BEGIN");
        store_telemetry(n, now, tick % SLOW_STORE_EVERY == 0);
        queue_telemetry(n, now);
        /* 有新事件时更新快照，最多每 5 秒一次 */
        if (store_events() > 0)
            events_dirty = 1;
        if (events_dirty && tick - last_snapshot >= 5) {
            queue_recent_events();
            events_dirty = 0;
            last_snapshot = tick;
        }
        exec("COMMIT");
        if (++tick % 600 == 0)
            housekeeping(now);
        int64_t cost = mono_ms() - t0;
        if (cost > 200)
            LOGW("storage", "本轮写库耗时 %lld ms", (long long)cost);
    }
    return NULL;
}

int storage_start(const config_t *cfg, storage_status_fn status_fn)
{
    g_cfg = cfg;
    g_status_fn = status_fn;
    g_db = storage_open(cfg->db_path);
    if (!g_db || exec(SCHEMA))
        return -1;
    g_snap = calloc((size_t)(rtdb_count() ? rtdb_count() : 1), sizeof *g_snap);
    load_point_ids();
    publish_meta();
    atomic_store(&g_running, 1);
    if (pthread_create(&g_thread, NULL, storage_main, NULL))
        return -1;
    LOGI("storage", "数据库 %s（WAL），保留 %d 小时", cfg->db_path, cfg->retention_hours);
    return 0;
}

void storage_stop(void)
{
    if (!atomic_load(&g_running))
        return;
    atomic_store(&g_running, 0);
    pthread_join(g_thread, NULL);
    store_events();      /* 退出前把剩余事件落库 */
    sqlite3_close(g_db);
    free(g_point_ids);
    free(g_snap);
    g_db = NULL;
}
