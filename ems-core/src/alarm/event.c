#include "event.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

#include "../util/clock.h"
#include "../util/log.h"

#define QUEUE_SIZE 256

static event_t g_ring[QUEUE_SIZE];
static int g_head, g_count;
static uint64_t g_dropped;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void event_push(int level, const char *source, const char *code, const char *fmt, ...)
{
    event_t ev = {.ts = wall_ms(), .level = level};
    snprintf(ev.source, sizeof ev.source, "%s", source);
    snprintf(ev.code, sizeof ev.code, "%s", code);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev.text, sizeof ev.text, fmt, ap);
    va_end(ap);

    log_write(level == EV_ALARM ? LOG_WARN : LOG_INFO, "event", "%s %s", ev.source, ev.text);

    pthread_mutex_lock(&g_lock);
    if (g_count == QUEUE_SIZE) {          /* 满了：覆盖最旧的一条 */
        g_head = (g_head + 1) % QUEUE_SIZE;
        g_count--;
        g_dropped++;
    }
    g_ring[(g_head + g_count) % QUEUE_SIZE] = ev;
    g_count++;
    pthread_mutex_unlock(&g_lock);
}

int event_pop(event_t *out, int max)
{
    pthread_mutex_lock(&g_lock);
    int n = g_count < max ? g_count : max;
    for (int i = 0; i < n; i++)
        out[i] = g_ring[(g_head + i) % QUEUE_SIZE];
    g_head = (g_head + n) % QUEUE_SIZE;
    g_count -= n;
    pthread_mutex_unlock(&g_lock);
    return n;
}

uint64_t event_dropped(void)
{
    pthread_mutex_lock(&g_lock);
    uint64_t d = g_dropped;
    pthread_mutex_unlock(&g_lock);
    return d;
}
