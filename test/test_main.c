/**
 * @file test_main.c
 * @brief 主机侧冒烟测试：验证 AT核心 / 设备层 / socket层 / ML307A类 的协作
 *
 * 编译运行：
 *   make test
 * 或
 *   gcc -std=c99 -Wall -Wextra -Isource -Isource/class -Itest \
 *       source/emda_at.c source/emda_device.c source/emda_socket.c \
 *       source/class/emda_class_ml307a.c test/fake_modem.c test/test_main.c -o build/test_emda
 */

#include <stdio.h>
#include <string.h>

#include "emda_at.h"
#include "emda_device.h"
#include "emda_socket.h"
#include "class/emda_class_ml307a.h"
#include "fake_modem.h"

static int s_failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (cond) { printf("[ OK ] %s\r\n", msg); }              \
        else { printf("[FAIL] %s\r\n", msg); s_failures++; }     \
    } while (0)

/* socket 事件记录 */
static int s_evt_connected = 0;
static int s_evt_connect_failed = 0;
static int s_evt_sent = 0;
static int s_evt_closed = 0;

static void on_sock_evt(int sock, emda_sock_event_t evt)
{
    (void)sock;

    switch (evt)
    {
    case EMDA_SOCK_EVT_CONNECTED:
        s_evt_connected = 1;
        break;
    case EMDA_SOCK_EVT_CONNECT_FAILED:
        s_evt_connect_failed = 1;
        break;
    case EMDA_SOCK_EVT_SENT:
        s_evt_sent = 1;
        break;
    case EMDA_SOCK_EVT_CLOSED:
        s_evt_closed = 1;
        break;
    default:
        break;
    }
}

/* 业务临时指令回调 */
static int s_exec_ok = 0;
static int s_exec_timeout = 0;

static int on_exec_resp(uint8_t *buffer, uint16_t len)
{
    (void)len;
    if (strstr((const char *)buffer, "OK") != NULL)
    {
        s_exec_ok = 1;
        return EMDA_EOK;
    }
    return EMDA_ERROR;
}

static int on_exec_timeout(char *cmd)
{
    (void)cmd;
    s_exec_timeout = 1;
    return EMDA_EOK;
}

/* 推进引擎 ms 毫秒(1 tick = 1ms) */
static void run_ms(int ms)
{
    int i;

    for (i = 0; i < ms; i++)
    {
        emda_poll();
        emda_heartbeat();
    }
}

/* 等待网络状态到达目标值 */
static int wait_net_state(emda_net_state_t target, int ms)
{
    int i;

    for (i = 0; i < ms; i++)
    {
        run_ms(1);
        if (emda_net_state_get() == target)
        {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    int sock;
    int csq[2] = {0, 0};
    char buf[32];

    fake_modem_reset();

    /* ---- 1. 设备层：绑定模组类并启动 ---- */
    CHECK(emda_device_register(&emda_class_ml307a, NULL) == EMDA_EOK,
          "register ml307a class");
    CHECK(emda_device_start() == EMDA_EOK, "device start");

    /* ---- 2. 监控阶梯：UNKNOWN -> INIT -> ... -> READY ---- */
    CHECK(wait_net_state(EMDA_NET_STATE_READY, 3000) == 1,
          "monitor ladder reaches READY (AT/CPIN/CEREG/CGATT)");

    /* ---- 3. 信号质量巡检(CSQ 立即执行一次) ---- */
    run_ms(200);
    CHECK(emda_device_control(EMDA_ML307A_CTRL_GET_CSQ, csq) == EMDA_EOK,
          "device control GET_CSQ");
    CHECK(csq[0] == 20 && csq[1] == 99, "rssi parsed from +CSQ");

    /* ---- 4. socket：创建 + 建链 ---- */
    sock = emda_socket_create(EMDA_SOCK_STREAM, on_sock_evt);
    CHECK(sock == 0, "socket create returns 0");

    CHECK(emda_socket_connect(sock, "1.2.3.4", 8080) == EMDA_EOK,
          "socket connect issued");
    run_ms(300);
    CHECK(s_evt_connected == 1, "CONNECTED event from +MIPOPEN URC");
    CHECK(emda_socket_state_get(sock) == EMDA_SOCK_STATE_CONNECTED,
          "socket state == CONNECTED");

    /* ---- 5. socket：发送(两段式 '>' 模式) ---- */
    CHECK(emda_socket_send(sock, "hello", 5) == EMDA_EOK, "socket send buffered");
    run_ms(300);
    CHECK(s_evt_sent == 1, "SENT event after MIPSEND payload");
    CHECK(strstr(fake_modem_last_cmd(), "AT+MIPSEND=0,5") != NULL,
          "MIPSEND uses length form");

    /* ---- 6. socket：URC 数据分片重组 ---- */
    s_evt_sent = 0;
    fake_modem_inject_data_part1(0, "world", 2);
    run_ms(40); /* 让前半段成为一个独立帧 */
    fake_modem_inject_data_part2("rld");
    run_ms(100);

    memset(buf, 0, sizeof(buf));
    CHECK(emda_socket_recv(sock, buf, sizeof(buf)) == 5, "recv 5 bytes");
    CHECK(strcmp(buf, "world") == 0, "payload reassembled across URC fragments");

    /* ---- 7. socket：关闭 ---- */
    CHECK(emda_socket_close(sock) == EMDA_EOK, "socket close");
    run_ms(100);
    CHECK(s_evt_closed == 1, "CLOSED event");
    CHECK(strstr(fake_modem_last_cmd(), "AT+MIPCLOSE=0") != NULL,
          "close uses MIPCLOSE");

    /* ---- 8. 业务临时指令：正常响应 ---- */
    s_exec_ok = 0;
    CHECK(emda_device_exec("AT+CPIN?\r\n", on_exec_resp, 1000U, on_exec_timeout) == EMDA_EOK,
          "device exec queued");
    run_ms(500);
    CHECK(s_exec_ok == 1, "business command response handled");

    /* ---- 9. 业务临时指令：超时兜底 ---- */
    s_exec_timeout = 0;
    CHECK(emda_device_exec("AT+NORESP\r\n", on_exec_resp, 100U, on_exec_timeout) == EMDA_EOK,
          "device exec no-response queued");
    run_ms(400);
    CHECK(s_exec_timeout == 1, "timeout callback fired");

    printf("\r\n==== %s (%d failures) ====\r\n",
           (s_failures == 0) ? "PASS" : "FAIL", s_failures);
    return (s_failures == 0) ? 0 : 1;
}
