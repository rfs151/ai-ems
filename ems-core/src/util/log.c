#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

static int g_level = LOG_INFO;

void log_set_level(int level) { g_level = level; }

void log_write(int level, const char *mod, const char *fmt, ...)
{
    static const char *names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    if (level < g_level)
        return;

    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    fprintf(stderr, "%02d:%02d:%02d.%03ld %-5s [%s] %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
            (long)(tv.tv_usec / 1000), names[level], mod, msg);
}
