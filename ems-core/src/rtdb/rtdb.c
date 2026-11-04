#include "rtdb.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../util/clock.h"

static rt_point_t *g_pts;
static int g_npts;
static rt_device_t *g_devs;
static int g_ndevs;
static pthread_rwlock_t g_lock = PTHREAD_RWLOCK_INITIALIZER;

int rtdb_init(const config_t *cfg)
{
    int total = 0;
    for (int d = 0; d < cfg->ndevices; d++)
        if (cfg->devices[d].npoints > 0)
            total += cfg->devices[d].npoints;

    g_pts = calloc(total > 0 ? (size_t)total : 1u, sizeof *g_pts);
    g_devs = calloc(cfg->ndevices > 0 ? (size_t)cfg->ndevices : 1u, sizeof *g_devs);
    if (!g_pts || !g_devs)
        return -1;

    int k = 0;
    for (int d = 0; d < cfg->ndevices; d++) {
        device_cfg_t *dev = &cfg->devices[d];
        for (int i = 0; i < dev->npoints; i++) {
            rt_point_t *p = &g_pts[k];
            snprintf(p->id, sizeof p->id, "%s.%s", dev->id, dev->points[i].key);
            p->def = &dev->points[i];
            p->dev = d;
            p->quality = Q_INVALID;
            dev->points[i].rt_index = k++;
        }
    }
    g_npts = total;
    g_ndevs = cfg->ndevices;
    return 0;
}

void rtdb_free(void)
{
    free(g_pts);
    free(g_devs);
    g_pts = NULL;
    g_devs = NULL;
    g_npts = g_ndevs = 0;
}

int rtdb_count(void) { return g_npts; }

int rtdb_find(const char *id)
{
    /* 点数少（百级），启动时查一次后缓存下标，不需要哈希表 */
    for (int i = 0; i < g_npts; i++)
        if (strcmp(g_pts[i].id, id) == 0)
            return i;
    return -1;
}

const rt_point_t *rtdb_meta(int index)
{
    return (index >= 0 && index < g_npts) ? &g_pts[index] : NULL;
}

void rtdb_set(int index, double value, uint8_t quality, int64_t ts)
{
    if (index < 0 || index >= g_npts)
        return;
    pthread_rwlock_wrlock(&g_lock);
    g_pts[index].value = value;
    g_pts[index].quality = quality;
    g_pts[index].ts = ts;
    pthread_rwlock_unlock(&g_lock);
}

void rtdb_set_device_quality(int dev, uint8_t quality)
{
    pthread_rwlock_wrlock(&g_lock);
    for (int i = 0; i < g_npts; i++)
        if (g_pts[i].dev == dev)
            g_pts[i].quality = quality;
    pthread_rwlock_unlock(&g_lock);
}

int rtdb_get(int index, double *value, uint8_t *quality)
{
    if (index < 0 || index >= g_npts)
        return -1;
    pthread_rwlock_rdlock(&g_lock);
    if (value)
        *value = g_pts[index].value;
    if (quality)
        *quality = g_pts[index].quality;
    pthread_rwlock_unlock(&g_lock);
    return 0;
}

double rtdb_value(const char *id, double def)
{
    double v;
    uint8_t q;
    if (rtdb_get(rtdb_find(id), &v, &q) || q != Q_GOOD)
        return def;
    return v;
}

int rtdb_snapshot(rt_point_t *out)
{
    pthread_rwlock_rdlock(&g_lock);
    memcpy(out, g_pts, (size_t)g_npts * sizeof *out);
    pthread_rwlock_unlock(&g_lock);
    return g_npts;
}

void rtdb_device_update(int dev, int ok, double poll_ms, int offline_after, int *changed)
{
    *changed = 0;
    if (dev < 0 || dev >= g_ndevs)
        return;
    pthread_rwlock_wrlock(&g_lock);
    rt_device_t *d = &g_devs[dev];
    int was = d->known ? d->online : -1;
    d->reads++;
    if (ok) {
        d->failures = 0;
        d->online = 1;
        d->last_ok = wall_ms();
        d->poll_ms = poll_ms;
    } else {
        d->errors++;
        if (++d->failures >= offline_after) {
            d->online = 0;
            d->known = 1;
        }
    }
    if (ok)
        d->known = 1;
    /* 首次判定为在线不算变化（不报"恢复"），首次判定为离线要报"中断" */
    *changed = d->known && was != d->online && !(was == -1 && d->online);
    pthread_rwlock_unlock(&g_lock);
}

void rtdb_device_count_write(int dev)
{
    pthread_rwlock_wrlock(&g_lock);
    if (dev >= 0 && dev < g_ndevs)
        g_devs[dev].writes++;
    pthread_rwlock_unlock(&g_lock);
}

void rtdb_device_get(int dev, rt_device_t *out)
{
    pthread_rwlock_rdlock(&g_lock);
    if (dev >= 0 && dev < g_ndevs)
        *out = g_devs[dev];
    else
        memset(out, 0, sizeof *out);
    pthread_rwlock_unlock(&g_lock);
}
