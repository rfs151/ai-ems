/* 规则引擎的纯逻辑部分（不依赖线程、实时库和数据库，单元测试直接调用）
 *
 * - 触发索引：规则按触发点位排序，点位变化时二分查找命中的规则，而不是遍历全部规则
 * - 告警抑制：同一条规则 1 分钟、24 小时内的告警次数上限，超过只计数不再产生告警记录
 * - 变化检测：首次采到的值只记录不触发，避免启动时把"初始值"当成一次变位
 * - 表达式：TinyExpr 编译，注册 gt/lt/and/or/if 等比较与逻辑函数
 */
#ifndef EMS_RULE_CORE_H
#define EMS_RULE_CORE_H

#include <stdint.h>

#include "../../third_party/tinyexpr/tinyexpr.h"

/* ------------------------------------------------------------------ 触发索引 */
typedef struct {
    int point;          /* 触发点位在实时库中的下标 */
    int rule;           /* 规则在规则表中的下标 */
} rule_ref_t;

void rule_refs_sort(rule_ref_t *refs, int n);
/* 返回第一个 point >= key 的位置（不存在时返回 n） */
int rule_refs_lower_bound(const rule_ref_t *refs, int n, int key);
/* 对照用的线性查找（技术要点页的耗时对比） */
int rule_refs_linear(const rule_ref_t *refs, int n, int key);

/* ------------------------------------------------------------------ 告警抑制 */
#define RAISE_LOG_MAX 256

typedef struct {
    int64_t t[RAISE_LOG_MAX];   /* 最近的告警时刻（环形） */
    int head, n;
} raise_log_t;

/* 按 1 分钟 / 24 小时上限判断这次告警是否记录；允许时把 now 记入日志并返回 1。
 * limit <= 0 表示不限制。 */
int raise_allowed(raise_log_t *log, int64_t now_ms, int limit_min, int limit_day);
/* 统计窗口内的次数（不修改日志） */
int raise_count(const raise_log_t *log, int64_t now_ms, int64_t window_ms);

/* ------------------------------------------------------------------ 变化检测 */
typedef struct {
    double last;
    int seen;           /* 0 = 还没采到过值 */
} watch_t;

/* 值变化超过 eps 返回 1；第一次调用只记录，返回 0 */
int watch_changed(watch_t *w, double v, double eps);

/* ------------------------------------------------------------------ 表达式 */
#define RULE_FUNC_COUNT 12

/* 规则表达式可用的函数（gt/lt/ge/le/eq/ne/and/or/not/if/max/min），追加到变量表末尾 */
const te_variable *rule_funcs(void);

/* 点位编号转成表达式变量名：bat2.tmax → bat2_tmax */
void rule_var_name(const char *point_id, char *out, int size);

#endif
