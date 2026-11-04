/* 点表编解码单元测试：make test */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../src/config/config.h"
#include "../third_party/cjson/cJSON.h"

static int g_fail, g_pass;

#define CHECK(cond, ...) do {                                              \
        if (cond) { g_pass++; } else {                                     \
            g_fail++;                                                      \
            printf("  ✗ %s:%d  ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

static point_def_t pt(dtype_t t, order_t o, double scale)
{
    point_def_t p;
    memset(&p, 0, sizeof p);
    p.type = t;
    p.order = o;
    p.scale = scale;
    p.bit = -1;
    return p;
}

static void test_orders(void)
{
    printf("32 位字节序\n");
    /* 0x11223344 在四种字节序下的寄存器排列 */
    static const struct { order_t o; uint16_t r0, r1; } cases[] = {
        {ORD_ABCD, 0x1122, 0x3344},
        {ORD_CDAB, 0x3344, 0x1122},
        {ORD_BADC, 0x2211, 0x4433},
        {ORD_DCBA, 0x4433, 0x2211},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        point_def_t p = pt(DT_U32, cases[i].o, 1.0);
        uint16_t regs[2] = {cases[i].r0, cases[i].r1}, out[2];
        CHECK(point_decode(&p, regs) == (double)0x11223344, "%s 解码得到 %.0f", order_name(p.order),
              point_decode(&p, regs));
        CHECK(point_encode(&p, (double)0x11223344, out) == 2 && out[0] == regs[0] && out[1] == regs[1],
              "%s 编码得到 %04X %04X", order_name(p.order), out[0], out[1]);
    }
    /* 负数在每种字节序下都能往返；float 只用 ABCD/CDAB */
    for (int o = ORD_ABCD; o <= ORD_DCBA; o++) {
        point_def_t p = pt(DT_I32, (order_t)o, 0.1);
        uint16_t r[2];
        point_encode(&p, -12345.6, r);
        CHECK(fabs(point_decode(&p, r) + 12345.6) < 1e-6, "%s int32 往返得到 %.3f", order_name(p.order),
              point_decode(&p, r));
        if (o != ORD_ABCD && o != ORD_CDAB)
            continue;
        p = pt(DT_F32, (order_t)o, 1.0);
        point_encode(&p, 49.987f, r);
        CHECK(fabs(point_decode(&p, r) - 49.987) < 1e-4, "%s float32 往返得到 %.4f", order_name(p.order),
              point_decode(&p, r));
    }
}

static void test_scalar(void)
{
    printf("16 位、位、枚举\n");
    point_def_t p = pt(DT_I16, ORD_ABCD, 0.1);
    uint16_t r[2] = {0xFF9C, 0};                      /* -100 */
    CHECK(fabs(point_decode(&p, r) + 10.0) < 1e-9, "int16 负数解码 %.2f", point_decode(&p, r));
    point_encode(&p, 5000.0, r);                      /* 50000 超出 int16，应饱和 */
    CHECK(r[0] == 0x7FFF, "int16 编码应饱和，得到 %04X", r[0]);

    p = pt(DT_U16, ORD_ABCD, 1.0);
    p.bit = 3;
    r[0] = 0x0008;
    CHECK(point_decode(&p, r) == 1.0, "bit3 应为 1");
    r[0] = 0x0007;
    CHECK(point_decode(&p, r) == 0.0, "bit3 应为 0");

    p = pt(DT_U16, ORD_ABCD, 10.0);
    p.has_eq = 1;
    p.eq = 3;
    r[0] = 3;
    CHECK(point_decode(&p, r) == 1.0, "枚举相等应为 1（且不乘倍率）");
    r[0] = 2;
    CHECK(point_decode(&p, r) == 0.0, "枚举不等应为 0");
}

/* 写一个只含一个点的最小配置，检查字节序拼错时启动失败 */
static void test_bad_order(void)
{
    printf("配置校验\n");
    char dir[] = "/tmp/ems-codec-XXXXXX";
    if (!mkdtemp(dir)) {
        CHECK(0, "无法创建临时目录");
        return;
    }
    char path[256];
    snprintf(path, sizeof path, "%s/vendors", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/site.json", dir);
    FILE *f = fopen(path, "w");
    fputs("{\"site\":{\"id\":\"t\"},\"devices\":[{\"id\":\"d\",\"profile\":\"p\"}]}", f);
    fclose(f);
    snprintf(path, sizeof path, "%s/vendors/p.json", dir);
    f = fopen(path, "w");
    fputs("{\"type\":\"meter\",\"points\":[{\"key\":\"e\",\"type\":\"u32\",\"order\":\"DCAB\"}]}", f);
    fclose(f);
    config_t cfg;
    CHECK(config_load(&cfg, dir) != 0, "拼错的字节序 DCAB 应导致加载失败");

    f = fopen(path, "w");
    fputs("{\"type\":\"meter\",\"points\":[{\"key\":\"e\",\"type\":\"u32\",\"order\":\"DCBA\"}]}", f);
    fclose(f);
    int ok = config_load(&cfg, dir) == 0;
    CHECK(ok && cfg.devices[0].points[0].order == ORD_DCBA, "DCBA 应正常加载");
    if (ok)
        config_free(&cfg);

    f = fopen(path, "w");
    fputs("{\"type\":\"meter\",\"points\":[{\"key\":\"e\",\"type\":\"f32\",\"order\":\"DCBA\"}]}", f);
    fclose(f);
    CHECK(config_load(&cfg, dir) != 0, "float32 用 DCBA 应拒绝");

    snprintf(path, sizeof path, "rm -rf %s", dir);
    if (system(path) != 0)
        printf("  （清理临时目录失败）\n");
}

int main(void)
{
    test_orders();
    test_scalar();
    test_bad_order();
    printf("\n%d 项通过，%d 项失败\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
