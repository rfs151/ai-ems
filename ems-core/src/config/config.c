#include "config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../third_party/cjson/cJSON.h"
#include "../util/log.h"

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOGE("config", "无法打开 %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf)
        buf[n] = '\0';
    return buf;
}

static cJSON *parse_file(const char *path)
{
    char *text = read_file(path);
    if (!text)
        return NULL;
    cJSON *root = cJSON_Parse(text);
    if (!root)
        LOGE("config", "%s 不是合法 JSON（错误位置附近：%.20s）", path, cJSON_GetErrorPtr());
    free(text);
    return root;
}

static void copy_str(char *dst, size_t size, const cJSON *obj, const char *key, const char *def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    const char *s = cJSON_IsString(it) ? it->valuestring : def;
    snprintf(dst, size, "%s", s ? s : "");
}

static double get_num(const cJSON *obj, const char *key, double def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(it) ? it->valuedouble : def;
}

static int parse_area(const char *s, area_t *out)
{
    static const struct { const char *n; area_t a; } map[] = {
        {"hr", AREA_HR}, {"ir", AREA_IR}, {"co", AREA_CO}, {"di", AREA_DI}};
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strcmp(s, map[i].n) == 0) {
            *out = map[i].a;
            return 0;
        }
    return -1;
}

static int parse_type(const char *s, dtype_t *out)
{
    static const struct { const char *n; dtype_t t; } map[] = {
        {"u16", DT_U16}, {"i16", DT_I16}, {"u32", DT_U32},
        {"i32", DT_I32}, {"f32", DT_F32}, {"bit", DT_BIT}};
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strcmp(s, map[i].n) == 0) {
            *out = map[i].t;
            return 0;
        }
    return -1;
}

static int parse_point(const cJSON *j, point_def_t *p, const char *profile)
{
    char area[8], type[8], order[8], group[8];
    memset(p, 0, sizeof *p);
    copy_str(p->key, sizeof p->key, j, "key", "");
    copy_str(p->name, sizeof p->name, j, "name", p->key);
    copy_str(p->unit, sizeof p->unit, j, "unit", "");
    copy_str(area, sizeof area, j, "area", "hr");
    copy_str(type, sizeof type, j, "type", "u16");
    copy_str(order, sizeof order, j, "order", "ABCD");
    copy_str(group, sizeof group, j, "group", "fast");

    if (!p->key[0] || parse_area(area, &p->area) || parse_type(type, &p->type)) {
        LOGE("config", "%s：点 %s 的 key/area/type 不合法", profile, p->key);
        return -1;
    }
    if ((p->area == AREA_CO || p->area == AREA_DI) != (p->type == DT_BIT)) {
        LOGE("config", "%s：点 %s 线圈/离散输入必须用 bit 类型，寄存器不能用", profile, p->key);
        return -1;
    }
    p->addr = (uint16_t)get_num(j, "addr", 0);
    p->word_swap = strcmp(order, "CDAB") == 0;
    p->scale = get_num(j, "scale", 1.0);
    p->offset = get_num(j, "offset", 0.0);
    p->bit = (int)get_num(j, "bit", -1);
    p->has_eq = cJSON_HasObjectItem(j, "eq");
    p->eq = (long)get_num(j, "eq", 0);
    p->group = strcmp(group, "slow") == 0 ? GROUP_SLOW : GROUP_FAST;
    p->rt_index = -1;
    return 0;
}

/* 解析 points 数组；带 count 的点展开成 key01..keyNN */
static int parse_points(const cJSON *arr, point_def_t **out, int *n, const char *profile)
{
    int total = 0;
    const cJSON *j;
    cJSON_ArrayForEach(j, arr) total += (int)get_num(j, "count", 1);

    point_def_t *pts = calloc((size_t)(total ? total : 1), sizeof *pts);
    int k = 0;
    cJSON_ArrayForEach(j, arr)
    {
        point_def_t base;
        if (parse_point(j, &base, profile)) {
            free(pts);
            return -1;
        }
        int count = (int)get_num(j, "count", 1);
        if (count > 99) {
            LOGE("config", "%s：点 %s 的 count 不能超过 99", profile, base.key);
            free(pts);
            return -1;
        }
        if (count <= 1) {
            pts[k++] = base;
            continue;
        }
        for (int i = 0; i < count; i++) {
            point_def_t p = base;
            snprintf(p.key, sizeof p.key, "%.28s%02d", base.key, (i + 1) % 100);
            snprintf(p.name, sizeof p.name, "%.40s %02d", base.name, (i + 1) % 100);
            p.addr = (uint16_t)(base.addr + i * point_reg_count(&base));
            pts[k++] = p;
        }
    }
    *out = pts;
    *n = k;
    return 0;
}

static int load_profile(device_cfg_t *d, const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/vendors/%s.json", dir, d->profile);
    cJSON *root = parse_file(path);
    if (!root)
        return -1;

    copy_str(d->vendor, sizeof d->vendor, root, "vendor", "");
    copy_str(d->model, sizeof d->model, root, "model", "");
    copy_str(d->type, sizeof d->type, root, "type", "");

    int rc = parse_points(cJSON_GetObjectItemCaseSensitive(root, "points"), &d->points, &d->npoints,
                          d->profile);

    const cJSON *ctl = cJSON_GetObjectItemCaseSensitive(root, "controls");
    int nctl = cJSON_GetArraySize(ctl);
    d->controls = calloc((size_t)(nctl ? nctl : 1), sizeof *d->controls);
    const cJSON *j;
    cJSON_ArrayForEach(j, ctl)
    {
        if (rc)
            break;
        control_def_t *c = &d->controls[d->ncontrols];
        if (parse_point(j, &c->def, d->profile)) {
            rc = -1;
            break;
        }
        const cJSON *un = cJSON_GetObjectItemCaseSensitive(j, "unlock");
        if (cJSON_IsObject(un)) {
            c->has_unlock = 1;
            c->unlock_addr = (uint16_t)get_num(un, "addr", 0);
            c->unlock_value = (uint16_t)get_num(un, "value", 0);
        }
        d->ncontrols++;
    }
    cJSON_Delete(root);
    return rc;
}

int config_load(config_t *cfg, const char *dir)
{
    memset(cfg, 0, sizeof *cfg);
    char path[512];
    snprintf(path, sizeof path, "%s/site.json", dir);
    cJSON *root = parse_file(path);
    if (!root)
        return -1;

    const cJSON *site = cJSON_GetObjectItemCaseSensitive(root, "site");
    const cJSON *poll = cJSON_GetObjectItemCaseSensitive(root, "poll");
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "storage");
    copy_str(cfg->site_id, sizeof cfg->site_id, site, "id", "site");
    copy_str(cfg->site_name, sizeof cfg->site_name, site, "name", "");
    cfg->fast_ms = (int)get_num(poll, "fast_ms", 200);
    cfg->slow_ms = (int)get_num(poll, "slow_ms", 5000);
    cfg->offline_after = (int)get_num(poll, "offline_after_failures", 3);
    cfg->timeout_ms = (int)get_num(poll, "timeout_ms", 500);
    copy_str(cfg->db_path, sizeof cfg->db_path, st, "db", "data/ems.db");
    cfg->retention_hours = (int)get_num(st, "retention_hours", 24);

    /* 容器部署时用环境变量覆盖：设备地址、数据库路径 */
    const char *host_override = getenv("EMS_DEVICE_HOST");
    const char *db_override = getenv("EMS_DB");
    if (db_override)
        snprintf(cfg->db_path, sizeof cfg->db_path, "%s", db_override);

    const cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    cfg->ndevices = cJSON_GetArraySize(devs);
    cfg->devices = calloc((size_t)(cfg->ndevices ? cfg->ndevices : 1), sizeof *cfg->devices);
    int i = 0, rc = 0;
    const cJSON *j;
    cJSON_ArrayForEach(j, devs)
    {
        device_cfg_t *d = &cfg->devices[i++];
        copy_str(d->id, sizeof d->id, j, "id", "");
        copy_str(d->name, sizeof d->name, j, "name", d->id);
        copy_str(d->profile, sizeof d->profile, j, "profile", "");
        copy_str(d->host, sizeof d->host, j, "host", "127.0.0.1");
        copy_str(d->pcs, sizeof d->pcs, j, "pcs", "");
        if (host_override)
            snprintf(d->host, sizeof d->host, "%s", host_override);
        d->port = (int)get_num(j, "port", 502);
        d->unit = (int)get_num(j, "unit", 1);
        if (load_profile(d, dir)) {
            rc = -1;
            break;
        }
        LOGI("config", "设备 %-5s %s %s，%d 个采集点，%d 个控制点", d->id, d->vendor, d->model,
             d->npoints, d->ncontrols);
    }
    cJSON_Delete(root);
    if (rc)
        config_free(cfg);
    return rc;
}

void config_free(config_t *cfg)
{
    for (int i = 0; i < cfg->ndevices; i++) {
        free(cfg->devices[i].points);
        free(cfg->devices[i].controls);
    }
    free(cfg->devices);
    memset(cfg, 0, sizeof *cfg);
}

const device_cfg_t *config_find_device(const config_t *cfg, const char *id)
{
    int i = config_device_index(cfg, id);
    return i < 0 ? NULL : &cfg->devices[i];
}

int config_device_index(const config_t *cfg, const char *id)
{
    for (int i = 0; i < cfg->ndevices; i++)
        if (strcmp(cfg->devices[i].id, id) == 0)
            return i;
    return -1;
}

const control_def_t *config_find_control(const device_cfg_t *dev, const char *key)
{
    for (int i = 0; i < dev->ncontrols; i++)
        if (strcmp(dev->controls[i].def.key, key) == 0)
            return &dev->controls[i];
    return NULL;
}

/* ------------------------------------------------------------------ 编解码 */
int point_reg_count(const point_def_t *p)
{
    return (p->type == DT_U32 || p->type == DT_I32 || p->type == DT_F32) ? 2 : 1;
}

static uint32_t join32(const point_def_t *p, const uint16_t *r)
{
    return p->word_swap ? ((uint32_t)r[1] << 16 | r[0]) : ((uint32_t)r[0] << 16 | r[1]);
}

double point_decode(const point_def_t *p, const uint16_t *r)
{
    double raw;
    switch (p->type) {
    case DT_BIT: raw = r[0] ? 1 : 0; break;
    case DT_U16: raw = r[0]; break;
    case DT_I16: raw = (int16_t)r[0]; break;
    case DT_U32: raw = join32(p, r); break;
    case DT_I32: raw = (int32_t)join32(p, r); break;
    case DT_F32: {
        uint32_t u = join32(p, r);
        float f;
        memcpy(&f, &u, sizeof f);
        raw = f;
        break;
    }
    default: raw = 0;
    }
    /* 位和枚举比较得到的是 0/1，不再乘倍率 */
    if (p->bit >= 0)
        return ((uint32_t)raw >> p->bit) & 1u;
    if (p->has_eq)
        return (long)raw == p->eq;
    return raw * p->scale + p->offset;
}

int point_encode(const point_def_t *p, double value, uint16_t out[2])
{
    double raw = p->type == DT_BIT ? value : (value - p->offset) / p->scale;
    uint32_t u;
    switch (p->type) {
    case DT_BIT: out[0] = raw != 0; return 1;
    case DT_U16: out[0] = (uint16_t)fmax(0, fmin(65535, lround(raw))); return 1;
    case DT_I16: out[0] = (uint16_t)(int16_t)fmax(-32768, fmin(32767, lround(raw))); return 1;
    case DT_U32: u = (uint32_t)fmax(0, fmin(4294967295.0, llround(raw))); break;
    case DT_I32: u = (uint32_t)(int32_t)fmax(-2147483648.0, fmin(2147483647.0, llround(raw))); break;
    case DT_F32: {
        float f = (float)raw;
        memcpy(&u, &f, sizeof u);
        break;
    }
    default: return 0;
    }
    uint16_t hi = (uint16_t)(u >> 16), lo = (uint16_t)(u & 0xFFFF);
    out[0] = p->word_swap ? lo : hi;
    out[1] = p->word_swap ? hi : lo;
    return 2;
}
