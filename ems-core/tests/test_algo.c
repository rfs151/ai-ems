/* 控制算法单元测试：make test（ASan 版本：make test-asan） */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/control/algo.h"

static int g_fail, g_pass;

#define CHECK(cond, ...) do {                                              \
        if (cond) { g_pass++; } else {                                     \
            g_fail++;                                                      \
            printf("  ✗ %s:%d  ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)
#define NEAR(a, b, eps) (fabs((a) - (b)) <= (eps))
#define CHECK_NEAR(a, b, eps) CHECK(NEAR(a, b, eps), "期望 %.3f，实际 %.3f", (double)(b), (double)(a))

static const fcr_cfg_t FCR = {50.0, 0.01, 0.2, 200.0};
static const lfsmo_cfg_t LFSM = {50.0, 50.2, 0.02, 1000.0};
static const mfrr_cfg_t MF = {30000, 60000, 30 * 60000, 50.0, 1000.0};

static mfrr_cmd_t direct(const char *id, int dir, double p, int64_t dur)
{
    return (mfrr_cmd_t){.id = id, .dir = dir, .power = p, .scheduled = 0, .duration_ms = dur};
}

static void test_fcr(void)
{
    printf("FCR 一次调频\n");
    CHECK_NEAR(fcr_power(&FCR, 50.005), 0, 1e-9);
    CHECK_NEAR(fcr_power(&FCR, 49.99), 0, 1e-9);                 /* 死区边界 */
    CHECK_NEAR(fcr_power(&FCR, 49.9), 0.09 / 0.19 * 200, 1e-6);  /* 低频放电 */
    CHECK_NEAR(fcr_power(&FCR, 50.1), -0.09 / 0.19 * 200, 1e-6); /* 高频充电 */
    CHECK_NEAR(fcr_power(&FCR, 49.6), 200, 1e-9);                /* 饱和 */
    CHECK_NEAR(fcr_power(&FCR, 50.4), -200, 1e-9);
}

static void test_lfsmo(void)
{
    printf("LFSM-O 过频限功率\n");
    CHECK_NEAR(lfsmo_power(&LFSM, 50.2), 0, 1e-9);
    CHECK_NEAR(lfsmo_power(&LFSM, 49.8), 0, 1e-9);
    CHECK_NEAR(lfsmo_power(&LFSM, 50.3), -100, 1e-6);   /* 2% 下垂：每 0.1Hz 减 10% 额定 */
    CHECK_NEAR(lfsmo_power(&LFSM, 50.45), -250, 1e-6);
}

static void test_trajectory(void)
{
    printf("mFRR 四阶段轨迹\n");
    mfrr_state_t s;
    mfrr_init(&s);
    char why[96];
    int64_t t = 1000000;
    mfrr_cmd_t c = direct("a", 1, 400, 120000);
    CHECK(mfrr_submit(&s, &MF, &c, t, why, sizeof why) == 0, "提交失败：%s", why);
    CHECK_NEAR(mfrr_update(&s, &MF, t), 0, 1e-9);
    CHECK(s.act[0].phase == MFRR_RAMP_UP, "应在爬坡");
    CHECK_NEAR(mfrr_update(&s, &MF, t + 15000), 200, 1e-6);
    CHECK_NEAR(mfrr_update(&s, &MF, t + 30000), 400, 1e-6);
    CHECK(s.act[0].phase == MFRR_HOLD, "应在保持");
    CHECK_NEAR(mfrr_update(&s, &MF, t + 119900), 400, 1e-6);
    CHECK_NEAR(mfrr_update(&s, &MF, t + 135000), 200, 1e-6);
    CHECK(s.act[0].phase == MFRR_RAMP_DOWN, "应在降坡");
    CHECK_NEAR(mfrr_update(&s, &MF, t + 150000), 0, 1e-9);
    CHECK(s.act[0].phase == MFRR_DONE && strcmp(s.act[0].end_reason, "finished") == 0, "应正常结束");

    /* 下调方向为负值 */
    mfrr_cmd_t d = direct("b", -1, 300, 60000);
    CHECK(mfrr_submit(&s, &MF, &d, t + 200000, why, sizeof why) == 0, "提交失败：%s", why);
    mfrr_update(&s, &MF, t + 200000);
    CHECK_NEAR(mfrr_update(&s, &MF, t + 230000), -300, 1e-6);
}

static void test_validation(void)
{
    printf("mFRR 指令校验\n");
    mfrr_state_t s;
    mfrr_init(&s);
    char why[96];
    int64_t now = 600000 + 1234;   /* 窗口边界是 60000 的整数倍 */
    mfrr_cmd_t c = {.id = "s1", .dir = 1, .power = 300, .scheduled = 1, .duration_ms = 60000};

    c.t_start = now + 5000;                    /* 不在窗口边界 */
    CHECK(mfrr_submit(&s, &MF, &c, now, why, sizeof why) < 0 && strstr(why, "窗口"), "未对齐应拒绝：%s", why);
    c.t_start = 600000;                        /* 已过 */
    CHECK(mfrr_submit(&s, &MF, &c, now, why, sizeof why) < 0 && strstr(why, "已过"), "过去时间应拒绝：%s", why);
    c.t_start = 600000 + 60 * 60000;           /* 太远 */
    CHECK(mfrr_submit(&s, &MF, &c, now, why, sizeof why) < 0 && strstr(why, "太远"), "太远应拒绝：%s", why);
    c.t_start = 660000;
    CHECK(mfrr_submit(&s, &MF, &c, now, why, sizeof why) == 0, "合法计划指令被拒：%s", why);
    CHECK(mfrr_submit(&s, &MF, &c, now, why, sizeof why) < 0 && strstr(why, "重复"), "重复编号应拒绝");
    CHECK_NEAR(mfrr_update(&s, &MF, 659900), 0, 1e-9);
    CHECK(s.act[0].phase == MFRR_PENDING, "开始前应等待");
    mfrr_update(&s, &MF, 660000);
    CHECK_NEAR(mfrr_update(&s, &MF, 675000), 150, 1e-6);

    mfrr_cmd_t bad = direct("p", 1, 20, 60000);
    CHECK(mfrr_submit(&s, &MF, &bad, now, why, sizeof why) < 0, "功率过小应拒绝");
    bad = direct("q", 1, 300, 10000);
    CHECK(mfrr_submit(&s, &MF, &bad, now, why, sizeof why) < 0, "时长短于爬坡时间应拒绝");

    /* 反向：时间重叠拒绝，不重叠接受 */
    mfrr_cmd_t rev = direct("r", -1, 200, 60000);
    CHECK(mfrr_submit(&s, &MF, &rev, 680000, why, sizeof why) < 0 && strstr(why, "反向"), "反向冲突应拒绝：%s", why);
    rev = (mfrr_cmd_t){.id = "r2", .dir = -1, .power = 200, .scheduled = 1, .t_start = 900000, .duration_ms = 60000};
    CHECK(mfrr_submit(&s, &MF, &rev, 680000, why, sizeof why) == 0, "不重叠的反向计划应接受：%s", why);

    /* 容量上限 */
    mfrr_cmd_t big = direct("big", 1, 800, 60000);
    CHECK(mfrr_submit(&s, &MF, &big, 680000, why, sizeof why) < 0 && strstr(why, "超过上限"), "叠加超限应拒绝：%s", why);
}

/* 以 100ms 步长推进，检查相邻两步变化不超过 max_step（无跳变） */
static double run(mfrr_state_t *s, int64_t from, int64_t to, double max_step, double *min_v, double *max_v)
{
    double prev = mfrr_update(s, &MF, from), worst = 0;
    *min_v = *max_v = prev;
    for (int64_t t = from + 100; t <= to; t += 100) {
        double v = mfrr_update(s, &MF, t);
        double d = fabs(v - prev);
        if (d > worst)
            worst = d;
        if (v < *min_v) *min_v = v;
        if (v > *max_v) *max_v = v;
        prev = v;
    }
    CHECK(worst <= max_step + 1e-6, "出现跳变：单步变化 %.2f kW > %.2f", worst, max_step);
    return prev;
}

static void test_stacking(void)
{
    printf("mFRR 同向叠加\n");
    mfrr_state_t s;
    mfrr_init(&s);
    char why[96];
    double lo, hi;
    mfrr_cmd_t a = direct("a", 1, 300, 120000), b = direct("b", 1, 200, 60000);
    mfrr_submit(&s, &MF, &a, 0, why, sizeof why);
    run(&s, 0, 40000, 300.0 / 300, &lo, &hi);            /* 每 100ms 最多 1kW */
    CHECK(mfrr_submit(&s, &MF, &b, 40000, why, sizeof why) == 0, "叠加被拒：%s", why);
    run(&s, 40000, 55000, (300.0 + 200.0) / 300, &lo, &hi);
    CHECK_NEAR(mfrr_update(&s, &MF, 55000), 400, 1e-6);  /* 300 + 200 爬到一半 */
    CHECK_NEAR(mfrr_update(&s, &MF, 70000), 500, 1e-6);
    run(&s, 70000, 200000, (300.0 + 200.0) / 300, &lo, &hi);
    CHECK_NEAR(hi, 500, 1e-6);
    CHECK_NEAR(mfrr_update(&s, &MF, 200000), 0, 1e-9);
}

static void test_handover(void)
{
    printf("mFRR 平滑交接\n");
    mfrr_state_t s;
    mfrr_init(&s);
    char why[96];
    double lo, hi;
    mfrr_cmd_t a = direct("a", 1, 400, 60000);
    mfrr_submit(&s, &MF, &a, 0, why, sizeof why);
    mfrr_cmd_t b = {.id = "b", .dir = 1, .power = 300, .scheduled = 1, .t_start = 60000, .duration_ms = 120000};
    CHECK(mfrr_submit(&s, &MF, &b, 10, why, sizeof why) == 0, "接续指令被拒：%s", why);
    run(&s, 0, 59900, 400.0 / 300, &lo, &hi);
    CHECK_NEAR(mfrr_update(&s, &MF, 59900), 400, 1e-6);
    /* 交接点：a 本该从 400 降到 0，b 从 a 的当前值接手，只降到 300，不出现"先降到 0 再爬升" */
    run(&s, 59900, 100000, 400.0 / 300, &lo, &hi);
    CHECK(lo >= 300 - 1e-6, "交接期间功率跌到 %.1f kW（应不低于 300）", lo);
    CHECK(strcmp(s.act[0].end_reason, "handover") == 0, "前一条应标记为交接结束");
    CHECK_NEAR(mfrr_update(&s, &MF, 100000), 300, 1e-6);
}

static void test_cancel(void)
{
    printf("mFRR 取消\n");
    mfrr_state_t s;
    mfrr_init(&s);
    char why[96];
    mfrr_cmd_t a = direct("a", 1, 400, 120000);
    mfrr_submit(&s, &MF, &a, 0, why, sizeof why);
    mfrr_update(&s, &MF, 0);
    CHECK_NEAR(mfrr_update(&s, &MF, 15000), 200, 1e-6);
    CHECK(mfrr_cancel(&s, &MF, "a", 15000) == 0, "取消失败");
    CHECK_NEAR(mfrr_update(&s, &MF, 22500), 100, 1e-6);   /* 从 200 按原速率降 */
    CHECK_NEAR(mfrr_update(&s, &MF, 30000), 0, 1e-9);
    CHECK(s.act[0].phase == MFRR_DONE && strcmp(s.act[0].end_reason, "cancelled") == 0, "应为取消结束");
    CHECK(mfrr_cancel(&s, &MF, "a", 31000) < 0, "已结束的不能再取消");

    mfrr_cmd_t p = {.id = "p", .dir = 1, .power = 200, .scheduled = 1, .t_start = 120000, .duration_ms = 60000};
    mfrr_submit(&s, &MF, &p, 31000, why, sizeof why);
    CHECK(mfrr_cancel(&s, &MF, "p", 32000) == 0 && s.act[0].phase == MFRR_DONE, "未开始的应直接结束");
}

int main(void)
{
    test_fcr();
    test_lfsmo();
    test_trajectory();
    test_validation();
    test_stacking();
    test_handover();
    test_cancel();
    printf("\n%d 项通过，%d 项失败\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
