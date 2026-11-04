/* 站点与点表配置。
 *
 * 厂商差异全部由配置描述：寄存器区、地址、数据类型、字序、倍率、位、枚举比较、写前解锁。
 * 上层（控制、规则、上云）只使用统一模型键，例如 pcs1.p、bat2.soc，不关心具体厂商。
 */
#ifndef EMS_CONFIG_H
#define EMS_CONFIG_H

#include <stdint.h>

typedef enum { AREA_HR, AREA_IR, AREA_CO, AREA_DI } area_t;
typedef enum { DT_U16, DT_I16, DT_U32, DT_I32, DT_F32, DT_BIT } dtype_t;
typedef enum { GROUP_FAST, GROUP_SLOW } group_t;
/* 32 位量的字节序（A 为最高字节）：
 *   ABCD 大端；CDAB 低字在前（字交换）；BADC 字内字节交换；DCBA 完全小端
 *   BADC/DCBA 只用于 int32/uint32（float32 只支持 ABCD/CDAB） */
typedef enum { ORD_ABCD, ORD_CDAB, ORD_BADC, ORD_DCBA } order_t;

typedef struct {
    char key[32];       /* 统一模型键，如 p、soc、cell03 */
    char name[48];
    char unit[12];
    area_t area;
    uint16_t addr;
    dtype_t type;
    order_t order;      /* 32 位量的字节序 */
    double scale;
    double offset;
    int bit;            /* >=0：取寄存器的某一位 */
    int has_eq;         /* 1：值 == eq 时为 1，否则为 0（枚举转布尔） */
    long eq;
    group_t group;
    int rt_index;       /* 在实时库中的下标 */
} point_def_t;

typedef struct {
    point_def_t def;
    int has_unlock;     /* 写设定值前先写解锁字 */
    uint16_t unlock_addr;
    uint16_t unlock_value;
} control_def_t;

typedef struct {
    char id[16];
    char name[48];
    char profile[48];
    char vendor[32];
    char model[32];
    char type[16];      /* pcs / bms / meter / dio */
    char host[64];
    char pcs[16];       /* 电池簇所属 PCS */
    int port;
    int unit;
    point_def_t *points;
    int npoints;
    control_def_t *controls;
    int ncontrols;
} device_cfg_t;

typedef struct {
    char site_id[32];
    char site_name[64];
    int fast_ms;
    int slow_ms;
    int offline_after;
    int timeout_ms;
    char db_path[256];
    int retention_hours;
    /* control 段 */
    int write_interval_ms;   /* 同一台 PCS 两次下发设定值的最小间隔 */
    int lfsm_enabled;
    double lfsm_f_start;
    double lfsm_droop;
    device_cfg_t *devices;
    int ndevices;
} config_t;

/* 读取 site.json 和 vendors/<profile>.json；失败返回 -1 并打印原因 */
int config_load(config_t *cfg, const char *config_dir);
void config_free(config_t *cfg);

const device_cfg_t *config_find_device(const config_t *cfg, const char *id);
const control_def_t *config_find_control(const device_cfg_t *dev, const char *key);
int config_device_index(const config_t *cfg, const char *id);

/* 原始寄存器 → 工程值。regs 指向该点的第一个寄存器 */
double point_decode(const point_def_t *p, const uint16_t *regs);
/* 工程值 → 原始寄存器，返回寄存器个数（1 或 2） */
int point_encode(const point_def_t *p, double value, uint16_t out[2]);
int point_reg_count(const point_def_t *p);
const char *order_name(order_t o);

#endif
