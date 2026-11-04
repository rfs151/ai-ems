#include "acq.h"

#include <errno.h>
#include <modbus/modbus.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../alarm/event.h"
#include "../rtdb/rtdb.h"
#include "../util/clock.h"
#include "../util/log.h"

#define MAX_GAP_REGS 8      /* 两个点之间空隙不超过 8 个寄存器就合并读取 */
#define MAX_BLOCK_REGS 100  /* 单次读取上限（协议上限 125） */
#define MAX_BLOCK_BITS 1000

typedef struct {
    area_t area;
    group_t group;
    uint16_t start;
    uint16_t count;
    const point_def_t **pts;
    int npts;
} block_t;

typedef struct {
    const control_def_t *ctl;
    double value;
    int pending;
} pending_write_t;

typedef struct {
    const config_t *cfg;
    const device_cfg_t *dev;
    int index;
    pthread_t thread;
    modbus_t *mb;
    int connected;
    int64_t next_connect;
    int backoff_ms;

    block_t *blocks;
    int nblocks;

    pthread_mutex_t lock;
    pthread_cond_t cond;
    pending_write_t *writes;
    int has_writes;
} worker_t;

static worker_t *g_workers;
static int g_nworkers;
static atomic_int g_running;

/* ------------------------------------------------------------------ 分块 */
static int cmp_point(const void *a, const void *b)
{
    const point_def_t *pa = *(const point_def_t *const *)a, *pb = *(const point_def_t *const *)b;
    return (int)pa->addr - (int)pb->addr;
}

static void build_blocks(worker_t *w)
{
    const device_cfg_t *d = w->dev;
    w->blocks = calloc((size_t)(d->npoints ? d->npoints : 1), sizeof *w->blocks);
    const point_def_t **tmp = calloc((size_t)(d->npoints ? d->npoints : 1), sizeof *tmp);

    for (int g = GROUP_FAST; g <= GROUP_SLOW; g++) {
        for (int a = AREA_HR; a <= AREA_DI; a++) {
            int n = 0;
            for (int i = 0; i < d->npoints; i++)
                if ((int)d->points[i].group == g && (int)d->points[i].area == a)
                    tmp[n++] = &d->points[i];
            if (!n)
                continue;
            qsort(tmp, (size_t)n, sizeof *tmp, cmp_point);

            int limit = (a == AREA_CO || a == AREA_DI) ? MAX_BLOCK_BITS : MAX_BLOCK_REGS;
            block_t *b = NULL;
            for (int i = 0; i < n; i++) {
                const point_def_t *p = tmp[i];
                int end = p->addr + point_reg_count(p);
                if (b && p->addr <= b->start + b->count + MAX_GAP_REGS && end - b->start <= limit) {
                    if (end > b->start + b->count)
                        b->count = (uint16_t)(end - b->start);
                } else {
                    b = &w->blocks[w->nblocks++];
                    b->area = (area_t)a;
                    b->group = (group_t)g;
                    b->start = p->addr;
                    b->count = (uint16_t)point_reg_count(p);
                    b->pts = calloc((size_t)n, sizeof *b->pts);
                }
                b->pts[b->npts++] = p;
            }
        }
    }
    free(tmp);
    for (int i = 0; i < w->nblocks; i++)
        LOGD("acq", "%s 块 %d：区 %d 组 %d 地址 %u 长度 %u，%d 个点", d->id, i, w->blocks[i].area,
             w->blocks[i].group, w->blocks[i].start, w->blocks[i].count, w->blocks[i].npts);
    LOGI("acq", "%s：%d 个点合并为 %d 次批量读", d->id, d->npoints, w->nblocks);
}

/* ------------------------------------------------------------------ 连接 */
static void disconnect(worker_t *w)
{
    if (w->mb) {
        modbus_close(w->mb);
        modbus_free(w->mb);
        w->mb = NULL;
    }
    w->connected = 0;
}

static int ensure_connected(worker_t *w)
{
    if (w->connected)
        return 0;
    if (mono_ms() < w->next_connect)
        return -1;

    /* modbus_new_tcp 只接受 IP；_pi 版本支持主机名（容器里用服务名访问设备） */
    char port[8];
    snprintf(port, sizeof port, "%d", w->dev->port);
    w->mb = modbus_new_tcp_pi(w->dev->host, port);
    if (w->mb) {
        modbus_set_slave(w->mb, w->dev->unit);
        int t = w->cfg->timeout_ms;
        modbus_set_response_timeout(w->mb, (uint32_t)(t / 1000), (uint32_t)(t % 1000) * 1000);
        if (modbus_connect(w->mb) == 0) {
            w->connected = 1;
            w->backoff_ms = 500;
            return 0;
        }
    }
    if (w->backoff_ms == 500)   /* 只在第一次失败时打印，避免刷屏 */
        LOGW("acq", "%s 连接 %s:%d 失败：%s", w->dev->id, w->dev->host, w->dev->port,
             modbus_strerror(errno));
    disconnect(w);
    /* 指数退避：0.5s → 1s → 2s … 最长 10s，避免设备故障时疯狂重连 */
    w->next_connect = mono_ms() + w->backoff_ms;
    w->backoff_ms = w->backoff_ms * 2 > 10000 ? 10000 : w->backoff_ms * 2;
    return -1;
}

/* Modbus 异常响应（非法地址等）说明链路是通的，保留连接；其他错误断开重连 */
static void handle_error(worker_t *w)
{
    if (errno < MODBUS_ENOBASE || errno == EMBBADDATA)
        disconnect(w);
}

/* ------------------------------------------------------------------ 读 */
static int read_block(worker_t *w, const block_t *b, int64_t ts)
{
    uint16_t regs[MAX_BLOCK_REGS + 2];
    uint8_t bits[MAX_BLOCK_BITS];
    int rc;
    switch (b->area) {
    case AREA_HR: rc = modbus_read_registers(w->mb, b->start, b->count, regs); break;
    case AREA_IR: rc = modbus_read_input_registers(w->mb, b->start, b->count, regs); break;
    case AREA_CO: rc = modbus_read_bits(w->mb, b->start, b->count, bits); break;
    default: rc = modbus_read_input_bits(w->mb, b->start, b->count, bits); break;
    }
    if (rc != b->count)
        return -1;

    for (int i = 0; i < b->npts; i++) {
        const point_def_t *p = b->pts[i];
        int off = p->addr - b->start;
        uint16_t one;
        const uint16_t *src = &regs[off];
        if (b->area == AREA_CO || b->area == AREA_DI) {
            one = bits[off];
            src = &one;
        }
        rtdb_set(p->rt_index, point_decode(p, src), Q_GOOD, ts);
    }
    return 0;
}

static void poll_group(worker_t *w, group_t g)
{
    int64_t t0 = mono_ms();
    int ok = ensure_connected(w) == 0;
    for (int i = 0; ok && i < w->nblocks; i++) {
        if (w->blocks[i].group != g)
            continue;
        if (read_block(w, &w->blocks[i], wall_ms())) {
            LOGD("acq", "%s 读取失败：%s", w->dev->id, modbus_strerror(errno));
            handle_error(w);
            ok = 0;
        }
    }

    int changed;
    rtdb_device_update(w->index, ok, (double)(mono_ms() - t0), w->cfg->offline_after, &changed);
    if (!changed)
        return;
    rt_device_t st;
    rtdb_device_get(w->index, &st);
    if (st.online) {
        event_push(EV_INFO, w->dev->id, "comm_restored", "%s 通信恢复", w->dev->name);
    } else {
        rtdb_set_device_quality(w->index, Q_OFFLINE);
        event_push(EV_ALARM, w->dev->id, "comm_lost", "%s 通信中断", w->dev->name);
    }
}

/* ------------------------------------------------------------------ 写 */
static void do_write(worker_t *w, const control_def_t *c, double value)
{
    if (ensure_connected(w))
        return;
    const point_def_t *p = &c->def;
    int rc;
    if (c->has_unlock && modbus_write_register(w->mb, c->unlock_addr, c->unlock_value) != 1) {
        LOGW("acq", "%s 写解锁字失败：%s", w->dev->id, modbus_strerror(errno));
        handle_error(w);
        return;
    }
    if (p->area == AREA_CO) {
        rc = modbus_write_bit(w->mb, p->addr, value != 0) == 1;
    } else {
        uint16_t regs[2];
        int n = point_encode(p, value, regs);
        rc = n == 1 ? modbus_write_register(w->mb, p->addr, regs[0]) == 1
                    : modbus_write_registers(w->mb, p->addr, n, regs) == n;
    }
    if (!rc) {
        LOGW("acq", "%s 写 %s=%.1f 失败：%s", w->dev->id, p->key, value, modbus_strerror(errno));
        handle_error(w);
        return;
    }
    rtdb_device_count_write(w->index);
}

static void flush_writes(worker_t *w)
{
    pending_write_t local[16];
    int n = 0;
    pthread_mutex_lock(&w->lock);
    for (int i = 0; i < w->dev->ncontrols && n < 16; i++)
        if (w->writes[i].pending) {
            local[n++] = w->writes[i];
            w->writes[i].pending = 0;
        }
    w->has_writes = 0;
    pthread_mutex_unlock(&w->lock);
    /* 在锁外执行 I/O，入队方不会被慢设备阻塞 */
    for (int i = 0; i < n; i++)
        do_write(w, local[i].ctl, local[i].value);
}

int acq_write(const char *dev_id, const char *key, double value)
{
    for (int i = 0; i < g_nworkers; i++) {
        worker_t *w = &g_workers[i];
        if (strcmp(w->dev->id, dev_id) != 0)
            continue;
        for (int c = 0; c < w->dev->ncontrols; c++) {
            if (strcmp(w->dev->controls[c].def.key, key) != 0)
                continue;
            pthread_mutex_lock(&w->lock);
            w->writes[c].value = value;
            w->writes[c].pending = 1;
            w->has_writes = 1;
            pthread_cond_signal(&w->cond);
            pthread_mutex_unlock(&w->lock);
            return 0;
        }
        return -1;
    }
    return -1;
}

/* ------------------------------------------------------------------ 线程 */
static void *worker_main(void *arg)
{
    worker_t *w = arg;
    int64_t next_fast = mono_ms(), next_slow = mono_ms();

    while (atomic_load(&g_running)) {
        flush_writes(w);
        int64_t now = mono_ms();
        if (now >= next_fast) {
            poll_group(w, GROUP_FAST);
            next_fast += w->cfg->fast_ms;
            if (next_fast < now)      /* 落后太多就不追赶，避免连续突发 */
                next_fast = now + w->cfg->fast_ms;
        }
        if (now >= next_slow) {
            poll_group(w, GROUP_SLOW);
            next_slow = now + w->cfg->slow_ms;
        }

        int64_t wake = next_fast < next_slow ? next_fast : next_slow;
        int64_t wait = wake - mono_ms();
        if (wait <= 0)
            continue;
        struct timespec dl = mono_deadline(wait);
        pthread_mutex_lock(&w->lock);
        while (!w->has_writes && atomic_load(&g_running))
            if (pthread_cond_timedwait(&w->cond, &w->lock, &dl) == ETIMEDOUT)
                break;
        pthread_mutex_unlock(&w->lock);
    }
    disconnect(w);
    return NULL;
}

int acq_start(const config_t *cfg)
{
    g_nworkers = cfg->ndevices;
    g_workers = calloc((size_t)(g_nworkers ? g_nworkers : 1), sizeof *g_workers);
    atomic_store(&g_running, 1);

    for (int i = 0; i < g_nworkers; i++) {
        worker_t *w = &g_workers[i];
        w->cfg = cfg;
        w->dev = &cfg->devices[i];
        w->index = i;
        w->backoff_ms = 500;
        w->writes = calloc((size_t)(w->dev->ncontrols ? w->dev->ncontrols : 1), sizeof *w->writes);
        for (int c = 0; c < w->dev->ncontrols; c++)
            w->writes[c].ctl = &w->dev->controls[c];
        build_blocks(w);

        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&w->cond, &ca);
        pthread_condattr_destroy(&ca);
        pthread_mutex_init(&w->lock, NULL);

        if (pthread_create(&w->thread, NULL, worker_main, w)) {
            LOGE("acq", "创建 %s 采集线程失败", w->dev->id);
            return -1;
        }
    }
    return 0;
}

void acq_stop(void)
{
    atomic_store(&g_running, 0);
    for (int i = 0; i < g_nworkers; i++) {
        pthread_mutex_lock(&g_workers[i].lock);
        pthread_cond_signal(&g_workers[i].cond);
        pthread_mutex_unlock(&g_workers[i].lock);
    }
    for (int i = 0; i < g_nworkers; i++) {
        worker_t *w = &g_workers[i];
        pthread_join(w->thread, NULL);
        for (int b = 0; b < w->nblocks; b++)
            free(w->blocks[b].pts);
        free(w->blocks);
        free(w->writes);
        pthread_mutex_destroy(&w->lock);
        pthread_cond_destroy(&w->cond);
    }
    free(g_workers);
    g_workers = NULL;
    g_nworkers = 0;
}
