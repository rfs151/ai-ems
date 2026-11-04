#include "rule_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ 触发索引 */
static int cmp_ref(const void *a, const void *b)
{
    const rule_ref_t *x = a, *y = b;
    if (x->point != y->point)
        return x->point < y->point ? -1 : 1;
    return x->rule < y->rule ? -1 : (x->rule > y->rule);
}

void rule_refs_sort(rule_ref_t *refs, int n)
{
    if (n > 1)
        qsort(refs, (size_t)n, sizeof *refs, cmp_ref);
}

int rule_refs_lower_bound(const rule_ref_t *refs, int n, int key)
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (refs[mid].point < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

int rule_refs_linear(const rule_ref_t *refs, int n, int key)
{
    for (int i = 0; i < n; i++)
        if (refs[i].point >= key)
            return i;
    return n;
}

/* ------------------------------------------------------------------ 告警抑制 */
#define DAY_MS (24LL * 3600 * 1000)

int raise_count(const raise_log_t *log, int64_t now, int64_t window)
{
    int c = 0;
    for (int i = 0; i < log->n; i++) {
        int64_t t = log->t[(log->head + i) % RAISE_LOG_MAX];
        if (now - t < window)
            c++;
    }
    return c;
}

int raise_allowed(raise_log_t *log, int64_t now, int limit_min, int limit_day)
{
    /* 先丢掉 24 小时以前的记录（按时间顺序存放，从最旧的开始丢） */
    while (log->n > 0 && now - log->t[log->head] >= DAY_MS) {
        log->head = (log->head + 1) % RAISE_LOG_MAX;
        log->n--;
    }
    if (limit_min > 0 && raise_count(log, now, 60 * 1000) >= limit_min)
        return 0;
    if (limit_day > 0 && log->n >= limit_day)
        return 0;
    if (log->n == RAISE_LOG_MAX) {          /* 不限次数时环形覆盖最旧的一条 */
        log->head = (log->head + 1) % RAISE_LOG_MAX;
        log->n--;
    }
    log->t[(log->head + log->n) % RAISE_LOG_MAX] = now;
    log->n++;
    return 1;
}

/* ------------------------------------------------------------------ 变化检测 */
int watch_changed(watch_t *w, double v, double eps)
{
    if (!w->seen) {
        w->seen = 1;
        w->last = v;
        return 0;
    }
    if (fabs(v - w->last) <= eps)
        return 0;
    w->last = v;
    return 1;
}

/* ------------------------------------------------------------------ 表达式 */
static double f_gt(double a, double b) { return a > b; }
static double f_lt(double a, double b) { return a < b; }
static double f_ge(double a, double b) { return a >= b; }
static double f_le(double a, double b) { return a <= b; }
static double f_eq(double a, double b) { return fabs(a - b) < 1e-9; }
static double f_ne(double a, double b) { return fabs(a - b) >= 1e-9; }
static double f_and(double a, double b) { return a != 0 && b != 0; }
static double f_or(double a, double b) { return a != 0 || b != 0; }
static double f_not(double a) { return a == 0; }
static double f_if(double c, double a, double b) { return c != 0 ? a : b; }
static double f_max(double a, double b) { return a > b ? a : b; }
static double f_min(double a, double b) { return a < b ? a : b; }

#define PURE(n) (TE_FUNCTION##n | TE_FLAG_PURE)

static const te_variable FUNCS[RULE_FUNC_COUNT] = {
    {"gt", f_gt, PURE(2), NULL},   {"lt", f_lt, PURE(2), NULL},   {"ge", f_ge, PURE(2), NULL},
    {"le", f_le, PURE(2), NULL},   {"eq", f_eq, PURE(2), NULL},   {"ne", f_ne, PURE(2), NULL},
    {"and", f_and, PURE(2), NULL}, {"or", f_or, PURE(2), NULL},   {"not", f_not, PURE(1), NULL},
    {"if", f_if, PURE(3), NULL},   {"max", f_max, PURE(2), NULL}, {"min", f_min, PURE(2), NULL},
};

const te_variable *rule_funcs(void) { return FUNCS; }

void rule_var_name(const char *id, char *out, int size)
{
    snprintf(out, (size_t)size, "%s", id);
    for (char *p = out; *p; p++)
        if (*p == '.')
            *p = '_';
}
