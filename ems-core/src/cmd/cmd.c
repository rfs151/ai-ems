#include "cmd.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../third_party/cjson/cJSON.h"
#include "../control/control.h"
#include "../util/log.h"

static int g_fd = -1;
static pthread_t g_thread;
static atomic_int g_running;
static char g_path[108];

/* 请求：{"type":"mfrr","id":"...",...}；回复：{"id":"...","ok":true,"msg":"..."} */
static void handle(const char *req, int len, char *reply, size_t rsz)
{
    char msg[160] = "";
    int ok = 0;
    const char *id = "";
    cJSON *j = cJSON_ParseWithLength(req, (size_t)len);
    if (!j) {
        snprintf(msg, sizeof msg, "指令不是合法 JSON");
    } else {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(j, "type");
        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(j, "id");
        if (cJSON_IsString(jid))
            id = jid->valuestring;
        if (!cJSON_IsString(type))
            snprintf(msg, sizeof msg, "缺少指令类型");
        else
            ok = control_command(type->valuestring, j, msg, sizeof msg) == 0;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "id", id);
    cJSON_AddBoolToObject(r, "ok", ok);
    cJSON_AddStringToObject(r, "msg", msg);
    if (!cJSON_PrintPreallocated(r, reply, (int)rsz, 0))
        snprintf(reply, rsz, "{\"ok\":false,\"msg\":\"reply too long\"}");
    cJSON_Delete(r);
    cJSON_Delete(j);
}

static void *cmd_main(void *arg)
{
    (void)arg;
    char req[2048], reply[512];
    while (atomic_load(&g_running)) {
        struct sockaddr_un from;
        socklen_t flen = sizeof from;
        ssize_t n = recvfrom(g_fd, req, sizeof req, 0, (struct sockaddr *)&from, &flen);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                LOGW("cmd", "接收失败：%s", strerror(errno));
            continue;       /* 超时：回到循环检查退出标志 */
        }
        handle(req, (int)n, reply, sizeof reply);
        if (flen > sizeof(sa_family_t))
            sendto(g_fd, reply, strlen(reply), 0, (struct sockaddr *)&from, flen);
    }
    return NULL;
}

int cmd_start(const config_t *cfg)
{
    cmd_socket_path(cfg, g_path, sizeof g_path);
    g_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", g_path);
    unlink(g_path);
    if (g_fd < 0 || bind(g_fd, (struct sockaddr *)&addr, sizeof addr)) {
        LOGE("cmd", "绑定指令套接字 %s 失败：%s", g_path, strerror(errno));
        return -1;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 300 * 1000};
    setsockopt(g_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    atomic_store(&g_running, 1);
    LOGI("cmd", "指令通道 %s", g_path);
    return pthread_create(&g_thread, NULL, cmd_main, NULL) ? -1 : 0;
}

void cmd_stop(void)
{
    if (!atomic_load(&g_running))
        return;
    atomic_store(&g_running, 0);
    pthread_join(g_thread, NULL);
    close(g_fd);
    unlink(g_path);
}
