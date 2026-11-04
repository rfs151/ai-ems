/* 规则引擎纯逻辑单元测试：make test（make test-asan 下同时检查表达式反复编译/释放无泄漏） */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/rules/rule_core.h"

static int g_fail, g_pass;

#define CHECK(cond, ...) do {                                              \
        if (cond) { g_pass++; } else {                                     \
            g_fail++;                                                      \
            printf("  ✗ %s:%d  ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

static void test_index(void)
{
    printf("触发索引：排序 + 二分查找\n");
    enum { N = 500 };
    rule_ref_t refs[N];
    unsigned seed = 7;
    for (int i = 0; i < N; i++)
        refs[i] = (rule_ref_t){(int)((seed = seed * 1103515245u + 12345u) >> 16) % 120, i};
    rule_refs_sort(refs, N);
    int sorted = 1;
    for (int i = 1; i < N; i++)
        if (refs[i - 1].point > refs[i].point)
            sorted = 0;
    CHECK(sorted, "排序后应按触发点位升序");

    int mismatch = 0;
    for (int key = -1; key <= 121; key++)
        if (rule_refs_lower_bound(refs, N, key) != rule_refs_linear(refs, N, key))
            mismatch++;
    CHECK(mismatch == 0, "二分查找与线性查找结果不一致 %d 次", mismatch);

    /* 同一点位多条规则：从下界开始向后取，正好取完 */
    rule_ref_t few[] = {{3, 0}, {5, 1}, {5, 2}, {5, 3}, {9, 4}};
    int k = rule_refs_lower_bound(few, 5, 5), n = 0;
    while (k < 5 && few[k].point == 5) {
        n++;
        k++;
    }
    CHECK(n == 3, "点位 5 应命中 3 条规则，实际 %d", n);
    CHECK(rule_refs_lower_bound(few, 5, 4) == 1, "不存在的点位应返回插入位置");
    CHECK(rule_refs_lower_bound(few, 5, 10) == 5, "大于全部时应返回 n");
    CHECK(rule_refs_lower_bound(few, 0, 1) == 0, "空索引应返回 0");
}

static void test_suppress(void)
{
    printf("告警抑制：1 分钟 / 24 小时上限\n");
    raise_log_t log = {0};
    int64_t t = 1000000;
    int allowed = 0;
    for (int i = 0; i < 10; i++)                 /* 10 秒内抖动 10 次，每分钟最多 3 次 */
        allowed += raise_allowed(&log, t + i * 1000, 3, 0);
    CHECK(allowed == 3, "1 分钟内应只记录 3 次，实际 %d", allowed);
    CHECK(raise_allowed(&log, t + 59999, 3, 0) == 0, "第一条满 60 秒前仍应抑制");
    CHECK(raise_allowed(&log, t + 60000, 3, 0) == 1, "第一条满 60 秒后应放行");
    CHECK(raise_count(&log, t + 60000, 60000) == 3, "窗口内应有 3 条");

    raise_log_t day = {0};
    allowed = 0;
    for (int i = 0; i < 30; i++)                 /* 每 10 分钟一次，一天最多 5 次 */
        allowed += raise_allowed(&day, t + (int64_t)i * 600000, 0, 5);
    CHECK(allowed == 5, "24 小时内应只记录 5 次，实际 %d", allowed);
    CHECK(raise_allowed(&day, t + 24LL * 3600000, 0, 5) == 1, "最早一条过 24 小时后应放行");

    raise_log_t inf = {0};
    allowed = 0;
    for (int i = 0; i < RAISE_LOG_MAX + 50; i++)
        allowed += raise_allowed(&inf, t + i, 0, 0);
    CHECK(allowed == RAISE_LOG_MAX + 50 && inf.n == RAISE_LOG_MAX, "不限次数时应全部放行且日志不溢出");
}

static void test_watch(void)
{
    printf("变化检测：首次值不触发\n");
    watch_t w = {0};
    CHECK(watch_changed(&w, 1.0, 1e-6) == 0, "第一次采到的值不应算变化（否则开机就报一次变位）");
    CHECK(watch_changed(&w, 1.0, 1e-6) == 0, "值不变不触发");
    CHECK(watch_changed(&w, 0.0, 1e-6) == 1, "1 → 0 应触发");
    CHECK(watch_changed(&w, 0.0000001, 1e-6) == 0, "小于死区不触发");
    watch_t z = {0};
    CHECK(watch_changed(&z, 0.0, 1e-6) == 0, "首值恰好为 0 也不应触发");
    CHECK(watch_changed(&z, 1.0, 1e-6) == 1, "随后变化正常触发");
}

static void test_expr(void)
{
    printf("TinyExpr 表达式\n");
    double f = 50.0, door = 0, t1 = 35, t2 = 42;
    char n1[32], n2[32], n3[32], n4[32];
    rule_var_name("meter.f", n1, sizeof n1);
    rule_var_name("dio.door", n2, sizeof n2);
    rule_var_name("pcs1.temp", n3, sizeof n3);
    rule_var_name("pcs2.temp", n4, sizeof n4);
    CHECK(strcmp(n1, "meter_f") == 0, "变量名转换：%s", n1);
    te_variable vars[4 + RULE_FUNC_COUNT] = {
        {n1, &f, TE_VARIABLE, NULL}, {n2, &door, TE_VARIABLE, NULL},
        {n3, &t1, TE_VARIABLE, NULL}, {n4, &t2, TE_VARIABLE, NULL}};
    memcpy(&vars[4], rule_funcs(), RULE_FUNC_COUNT * sizeof *vars);
    int nv = 4 + RULE_FUNC_COUNT, err = 0;

    te_expr *e = te_compile("or(gt(meter_f, 50.25), lt(meter_f, 49.75))", vars, nv, &err);
    CHECK(e != NULL, "频率越限表达式编译失败，位置 %d", err);
    if (e) {
        CHECK(te_eval(e) == 0, "50.00 Hz 不越限");
        f = 50.3;
        CHECK(te_eval(e) == 1, "50.30 Hz 越上限");
        f = 49.7;
        CHECK(te_eval(e) == 1, "49.70 Hz 越下限");
        te_free(e);
    }
    e = te_compile("gt(max(pcs1_temp, pcs2_temp), 40)", vars, nv, &err);
    CHECK(e && te_eval(e) == 1, "两台 PCS 取最高温度");
    te_free(e);
    e = te_compile("if(eq(dio_door, 1), 5, 7) + 1", vars, nv, &err);
    CHECK(e && te_eval(e) == 8, "if/eq 与算术混用");
    te_free(e);

    e = te_compile("gt(meter.f, 50)", vars, nv, &err);
    CHECK(e == NULL && err > 0, "带点的变量名应编译失败并给出位置");
    e = te_compile("gt(unknown_x, 1)", vars, nv, &err);
    CHECK(e == NULL, "未定义变量应编译失败");
    e = te_compile("gt(meter_f, ", vars, nv, &err);
    CHECK(e == NULL, "括号不完整应编译失败");

    /* 反复替换规则：每次编译新树、释放旧树（ASan 下无泄漏、无重复释放） */
    te_expr *cur = NULL;
    for (int i = 0; i < 2000; i++) {
        te_expr *n = te_compile(i % 2 ? "lt(meter_f, 49.8)" : "gt(meter_f, 50.2)", vars, nv, &err);
        te_free(cur);
        cur = n;
    }
    CHECK(cur != NULL, "反复编译后表达式应可用");
    te_free(cur);
}

int main(void)
{
    test_index();
    test_suppress();
    test_watch();
    test_expr();
    printf("\n%d 项通过，%d 项失败\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
