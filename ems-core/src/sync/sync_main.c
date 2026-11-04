/* ems-sync：上云进程（与 ems-core 分离，崩溃互不影响）
 *
 *   ems-sync [-c 配置目录] [-v]
 *   环境变量 MQTT_HOST / MQTT_PORT（默认 localhost:1883）
 *
 * - 单实例锁（flock）：防止误启动两个进程重复上报
 * - 遗嘱消息：异常断开时 broker 自动发布 offline
 * - 从 SQLite 发件箱按顺序读取、发布、确认后删除；断网时数据留在发件箱，恢复后按顺序补传
 * - 下行指令：校验过期时间、去重、按类型检查必填字段后经 Unix 套接字转给 ems-core，结果发布到 cmd_ack
 * - 断线重连：2s 起指数退避到 60s；设置 MQTT_CAFILE 时启用 TLS（服务端证书校验）
 */
#include <errno.h>
#include <fcntl.h>
#include <mosquitto.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../third_party/cjson/cJSON.h"
#include "../cmd/cmd.h"
#include "../config/config.h"
#include "../util/clock.h"
#include "../storage/storage.h"
#include "../util/log.h"

#define BATCH 200
#define POLL_MS 200
#define MAX_CMD_BYTES 4096
/* 超过这么久没连上 broker 就主动重连。broker 刚启动、还没监听时连接被拒绝，
 * libmosquitto 的线程循环不会再重试（开机时 ems-sync 比 mosquitto 早几十毫秒就会这样，
 * 之后永远不上云）。比退避上限 60s 略长，正常断线仍按退避节奏重连。 */
#define RECONNECT_MS 65000

static volatile sig_atomic_t g_stop;
static atomic_int g_connected;
static char g_status_topic[96];
static char g_cmd_topic[96];
static char g_ack_topic[96];
static char g_core_sock[108];
static int g_cmd_fd = -1;

/* 最近处理过的指令编号，用于去重（QoS1 可能重复投递） */
#define SEEN_MAX 64
static char g_seen[SEEN_MAX][48];
static int g_seen_pos;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void on_connect(struct mosquitto *m, void *ud, int rc)
{
    (void)ud;
    if (rc != 0) {
        LOGW("sync", "MQTT 连接被拒绝：%s", mosquitto_connack_string(rc));
        return;
    }
    mosquitto_publish(m, NULL, g_status_topic, 6, "online", 1, true);
    mosquitto_subscribe(m, NULL, g_cmd_topic, 1);
    atomic_store(&g_connected, 1);
    LOGI("sync", "MQTT 已连接");
}

static void on_disconnect(struct mosquitto *m, void *ud, int rc)
{
    (void)m;
    (void)ud;
    atomic_store(&g_connected, 0);
    if (!g_stop)
        LOGW("sync", "MQTT 断开（%s），数据暂存发件箱", mosquitto_strerror(rc));
}

/* 各类下行指令的必填字段（类型 n = 数字，s = 字符串，b = 布尔，o = 对象）。
 * 在边缘入口先挡掉格式不对的指令，返回明确原因，不让 ems-core 拿默认值去执行 */
static const struct {
    const char *type;
    const char *fields;
} REQUIRED[] = {
    {"mfrr", "n:dir n:power n:duration_s b:scheduled"},
    {"mfrr_cancel", "s:target"},
    {"mode", "s:mode"},
    {"fcr", "b:enabled"},
    {"lfsm", ""},
    {"rule_set", "o:rule"},
    {"rule_del", "s:rule_id"},
    {"rule_reset", ""},
};

/* 校验通过返回 NULL，否则返回原因（写入 buf） */
static const char *validate(const char *type, const cJSON *j, char *buf, size_t bsz)
{
    for (size_t i = 0; i < sizeof REQUIRED / sizeof REQUIRED[0]; i++) {
        if (strcmp(REQUIRED[i].type, type) != 0)
            continue;
        char spec[128];
        snprintf(spec, sizeof spec, "%s", REQUIRED[i].fields);
        for (char *save = NULL, *f = strtok_r(spec, " ", &save); f; f = strtok_r(NULL, " ", &save)) {
            const cJSON *it = cJSON_GetObjectItemCaseSensitive(j, f + 2);
            int ok = f[0] == 'n' ? cJSON_IsNumber(it) : f[0] == 's' ? cJSON_IsString(it)
                   : f[0] == 'b' ? cJSON_IsBool(it) : cJSON_IsObject(it);
            if (!ok) {
                snprintf(buf, bsz, "缺少或类型错误的字段 %s", f + 2);
                return buf;
            }
        }
        return NULL;
    }
    snprintf(buf, bsz, "不支持的指令类型 %s", type);
    return buf;
}

static int seen(const char *id)
{
    for (int i = 0; i < SEEN_MAX; i++)
        if (strcmp(g_seen[i], id) == 0)
            return 1;
    snprintf(g_seen[g_seen_pos], sizeof g_seen[0], "%s", id);
    g_seen_pos = (g_seen_pos + 1) % SEEN_MAX;
    return 0;
}

static void publish_ack(struct mosquitto *m, const char *id, const char *type, int ok, const char *msg,
                        int64_t rx)
{
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "id", id);
    cJSON_AddStringToObject(a, "type", type);
    cJSON_AddBoolToObject(a, "ok", ok);
    cJSON_AddStringToObject(a, "msg", msg);
    cJSON_AddNumberToObject(a, "edge_rx", (double)rx);
    cJSON_AddNumberToObject(a, "edge_ms", (double)(wall_ms() - rx));
    char *s = cJSON_PrintUnformatted(a);
    mosquitto_publish(m, NULL, g_ack_topic, (int)strlen(s), s, 1, false);
    free(s);
    cJSON_Delete(a);
}

/* 转给 ems-core 并等待回复（最多 1 秒） */
static int forward(const char *req, char *reply, size_t rsz)
{
    struct sockaddr_un to = {.sun_family = AF_UNIX};
    snprintf(to.sun_path, sizeof to.sun_path, "%s", g_core_sock);
    char drain[8];
    while (recv(g_cmd_fd, drain, sizeof drain, MSG_DONTWAIT) > 0)   /* 丢弃上次超时后迟到的回复 */
        ;
    if (sendto(g_cmd_fd, req, strlen(req), 0, (struct sockaddr *)&to, sizeof to) < 0)
        return -1;
    ssize_t n = recv(g_cmd_fd, reply, rsz - 1, 0);
    if (n <= 0)
        return -1;
    reply[n] = '\0';
    return 0;
}

static void on_message(struct mosquitto *m, void *ud, const struct mosquitto_message *msg)
{
    (void)ud;
    int64_t rx = wall_ms();
    const char *type = strrchr(msg->topic, '/');
    type = type ? type + 1 : "";
    if (msg->payloadlen > MAX_CMD_BYTES) {
        LOGW("sync", "丢弃过长的指令（%s，%d 字节）", msg->topic, msg->payloadlen);
        return;
    }
    cJSON *j = cJSON_ParseWithLength(msg->payload, (size_t)msg->payloadlen);
    const cJSON *jid = j ? cJSON_GetObjectItemCaseSensitive(j, "id") : NULL;
    if (!cJSON_IsString(jid) || !jid->valuestring[0]) {
        LOGW("sync", "丢弃无编号的指令（%s）", msg->topic);
        cJSON_Delete(j);
        return;
    }
    const char *id = jid->valuestring;
    const cJSON *exp = cJSON_GetObjectItemCaseSensitive(j, "exp");
    char why[96];
    const char *bad = NULL;
    if (seen(id)) {
        LOGD("sync", "重复指令 %s，忽略", id);
    } else if (!cJSON_IsNumber(exp) || exp->valuedouble < (double)rx) {
        publish_ack(m, id, type, 0, "指令已过期或缺少过期时间", rx);
    } else if ((bad = validate(type, j, why, sizeof why)) != NULL) {
        LOGW("sync", "拒绝指令 %s（%s）：%s", id, type, bad);
        publish_ack(m, id, type, 0, bad, rx);
    } else {
        cJSON_DeleteItemFromObjectCaseSensitive(j, "type");
        cJSON_AddStringToObject(j, "type", type);
        char *req = cJSON_PrintUnformatted(j);
        char reply[512];
        if (forward(req, reply, sizeof reply)) {
            publish_ack(m, id, type, 0, "边缘主进程无响应", rx);
        } else {
            cJSON *r = cJSON_Parse(reply);
            const cJSON *ok = r ? cJSON_GetObjectItemCaseSensitive(r, "ok") : NULL;
            const cJSON *rm = r ? cJSON_GetObjectItemCaseSensitive(r, "msg") : NULL;
            publish_ack(m, id, type, cJSON_IsTrue(ok), cJSON_IsString(rm) ? rm->valuestring : "", rx);
            cJSON_Delete(r);
        }
        free(req);
    }
    cJSON_Delete(j);
}

static int open_cmd_socket(const config_t *cfg)
{
    cmd_socket_path(cfg, g_core_sock, sizeof g_core_sock);
    char self[108];
    snprintf(self, sizeof self, "%.100s.sync", g_core_sock);   /* 自己的地址，用于接收回复 */
    g_cmd_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", self);
    unlink(self);
    if (g_cmd_fd < 0 || bind(g_cmd_fd, (struct sockaddr *)&addr, sizeof addr)) {
        LOGE("sync", "绑定 %s 失败：%s", self, strerror(errno));
        return -1;
    }
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(g_cmd_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return 0;
}

static int single_instance(const char *db_path)
{
    char path[300];
    snprintf(path, sizeof path, "%s.sync.lock", db_path);
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB)) {
        LOGE("sync", "已有 ems-sync 在运行（锁文件 %s）", path);
        return -1;
    }
    return fd;   /* 进程退出时自动释放 */
}

/* 发送一批，返回发送条数；出错返回 -1 */
static int pump(sqlite3 *db, struct mosquitto *m)
{
    sqlite3_stmt *sel;
    sqlite3_prepare_v2(db, "SELECT id,topic,payload,qos,retain FROM outbox ORDER BY id LIMIT ?", -1,
                       &sel, NULL);
    sqlite3_bind_int(sel, 1, BATCH);
    sqlite3_int64 last = -1;
    int n = 0, rc = 0;
    while (sqlite3_step(sel) == SQLITE_ROW) {
        const char *topic = (const char *)sqlite3_column_text(sel, 1);
        const void *payload = sqlite3_column_text(sel, 2);
        int len = sqlite3_column_bytes(sel, 2);
        int r = mosquitto_publish(m, NULL, topic, len, payload, sqlite3_column_int(sel, 3),
                                  sqlite3_column_int(sel, 4) != 0);
        if (r != MOSQ_ERR_SUCCESS) {
            rc = -1;
            break;
        }
        last = sqlite3_column_int64(sel, 0);
        n++;
    }
    sqlite3_finalize(sel);
    if (last >= 0) {
        char sql[96];
        snprintf(sql, sizeof sql, "DELETE FROM outbox WHERE id <= %lld", (long long)last);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    return rc ? -1 : n;
}

int main(int argc, char **argv)
{
    const char *config_dir = "config";
    int opt;
    while ((opt = getopt(argc, argv, "c:vh")) != -1) {
        switch (opt) {
        case 'c': config_dir = optarg; break;
        case 'v': log_set_level(LOG_DEBUG); break;
        default:
            fprintf(stderr, "用法：%s [-c 配置目录] [-v]\n", argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    config_t cfg;
    if (config_load(&cfg, config_dir))
        return 1;
    if (single_instance(cfg.db_path) < 0) {
        config_free(&cfg);
        return 1;
    }

    sqlite3 *db = NULL;
    while (!g_stop && !(db = storage_open(cfg.db_path)))
        sleep(1);
    /* ems-core 可能还没建表，等到发件箱存在 */
    while (!g_stop && sqlite3_exec(db, "SELECT 1 FROM outbox LIMIT 1", NULL, NULL, NULL) != SQLITE_OK)
        sleep(1);

    const char *host = getenv("MQTT_HOST") ? getenv("MQTT_HOST") : "localhost";
    int port = getenv("MQTT_PORT") ? atoi(getenv("MQTT_PORT")) : 1883;
    char client_id[64];
    snprintf(client_id, sizeof client_id, "ems-%s", cfg.site_id);
    snprintf(g_status_topic, sizeof g_status_topic, "ems/%s/status", cfg.site_id);
    snprintf(g_cmd_topic, sizeof g_cmd_topic, "ems/%s/cmd/+", cfg.site_id);
    snprintf(g_ack_topic, sizeof g_ack_topic, "ems/%s/cmd_ack", cfg.site_id);
    if (open_cmd_socket(&cfg)) {
        config_free(&cfg);
        return 1;
    }

    mosquitto_lib_init();
    struct mosquitto *m = mosquitto_new(client_id, true, NULL);
    mosquitto_will_set(m, g_status_topic, 7, "offline", 1, true);
    mosquitto_connect_callback_set(m, on_connect);
    mosquitto_disconnect_callback_set(m, on_disconnect);
    mosquitto_message_callback_set(m, on_message);
    /* 断线后 2s、4s、8s … 最长 60s 重试：broker 故障时不会每秒狂连，恢复后也能较快连上 */
    mosquitto_reconnect_delay_set(m, 2, 60, true);
    const char *cafile = getenv("MQTT_CAFILE");
    if (cafile && cafile[0]) {
        int r = mosquitto_tls_set(m, cafile, NULL, getenv("MQTT_CERT"), getenv("MQTT_KEY"), NULL);
        if (r != MOSQ_ERR_SUCCESS) {
            LOGE("sync", "加载 TLS 证书 %s 失败：%s", cafile, mosquitto_strerror(r));
            return 1;
        }
        LOGI("sync", "MQTT 使用 TLS，CA %s", cafile);
    }
    mosquitto_connect_async(m, host, port, 30);
    mosquitto_loop_start(m);
    LOGI("sync", "上云进程启动，broker %s:%d，站点 %s", host, port, cfg.site_id);

    int64_t last_ok = mono_ms();     /* 最近一次处于已连接状态的时间 */
    while (!g_stop) {
        int sent = 0;
        if (atomic_load(&g_connected)) {
            last_ok = mono_ms();
            sent = pump(db, m);
        } else if (mono_ms() - last_ok > RECONNECT_MS) {
            LOGW("sync", "MQTT %d 秒未连上，重新连接 %s:%d", RECONNECT_MS / 1000, host, port);
            mosquitto_reconnect_async(m);
            last_ok = mono_ms();
        }
        if (sent < BATCH)          /* 积压时连续发送，空闲时轮询 */
            usleep(POLL_MS * 1000);
    }

    mosquitto_publish(m, NULL, g_status_topic, 7, "offline", 1, true);
    mosquitto_disconnect(m);
    mosquitto_loop_stop(m, false);
    mosquitto_destroy(m);
    mosquitto_lib_cleanup();
    sqlite3_close(db);
    close(g_cmd_fd);
    char self[108];
    snprintf(self, sizeof self, "%.100s.sync", g_core_sock);
    unlink(self);
    config_free(&cfg);
    return 0;
}
