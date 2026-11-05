/**
 * @file test_main.c
 * @brief 主机侧冒烟测试：验证 AT核心 / 设备层 / socket层 / ML307A类 的协作
 *
 * 重点是接收定界：
 *   - payload 含 \r\n(且恰好落在分片边界上) 不能被切断
 *   - payload 里出现 "+MIPCLOSE: 0" 这种假 URC 不能误触发
 *   - payload 含 0x00 不能截断
 *   - 单条 1460 字节的大 URC 不能把解析器卡死
 *
 * 编译运行：
 *   make test
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

/* MQTT 下行消息记录 */
static int s_mqtt_msg_got = 0;
static char s_mqtt_topic[32];
static uint8_t s_mqtt_payload[32];
static int s_mqtt_len = 0;

static void on_mqtt_msg(int cid, const char *topic,
                        const uint8_t *payload, uint16_t len)
{
    (void)cid;

    s_mqtt_msg_got = 1;
    strncpy(s_mqtt_topic, topic, sizeof(s_mqtt_topic) - 1U);
    s_mqtt_topic[sizeof(s_mqtt_topic) - 1U] = '\0';

    s_mqtt_len = (len < (uint16_t)sizeof(s_mqtt_payload)) ? (int)len
                                                          : (int)sizeof(s_mqtt_payload);
    memcpy(s_mqtt_payload, payload, (size_t)s_mqtt_len);
}

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

/* 读空 socket 接收缓冲，返回读到的总字节数 */
static int drain_socket(int sock, uint8_t *buf, int cap)
{
    int total = 0;
    int n;

    while (total < cap &&
           (n = emda_socket_recv(sock, &buf[total], (uint16_t)(cap - total))) > 0)
    {
        total += n;
    }
    return total;
}

int main(void)
{
    int sock;
    int csq[2] = {0, 0};
    uint8_t buf[1600];
    uint8_t bin[8];

    fake_modem_reset();

    /* ---- 1. 设备层：绑定模组类并启动 ---- */
    CHECK(emda_device_register(&emda_class_ml307a, NULL) == EMDA_EOK,
          "register ml307a class");
    CHECK(emda_ml307a_set_apn("CMIOT") == EMDA_EOK, "set apn before start");
    CHECK(emda_device_start() == EMDA_EOK, "device start");

    emda_ml307a_mqtt_set_msg_cb(on_mqtt_msg);

    /* ---- 2. 监控阶梯：UNKNOWN -> ... -> READY ---- */
    CHECK(wait_net_state(EMDA_NET_STATE_READY, 3000) == 1,
          "monitor ladder reaches READY (AT/CPIN/CEREG/CGATT)");
    CHECK(fake_modem_saw_cgdcont() == 1, "AT+CGDCONT issued during INIT");

    run_ms(200);
    CHECK(emda_device_control(EMDA_ML307A_CTRL_GET_CSQ, csq) == EMDA_EOK,
          "device control GET_CSQ");
    CHECK(csq[0] == 20 && csq[1] == 99, "rssi parsed from +CSQ");

    /* ---- 3. socket：创建 + 建链 ---- */
    sock = emda_socket_create(EMDA_SOCK_STREAM, on_sock_evt);
    CHECK(sock == 0, "socket create returns 0");

    CHECK(emda_socket_connect(sock, "1.2.3.4", 8080) == EMDA_EOK,
          "socket connect issued");
    run_ms(300);
    CHECK(s_evt_connected == 1, "CONNECTED event from +MIPOPEN URC");
    CHECK(emda_socket_state_get(sock) == EMDA_SOCK_STATE_CONNECTED,
          "socket state == CONNECTED");

    /* ---- 4. socket：发送(两段式 '>' 模式) ---- */
    CHECK(emda_socket_send(sock, "hello", 5) == EMDA_EOK, "socket send buffered");
    run_ms(300);
    CHECK(s_evt_sent == 1, "SENT event after MIPSEND payload");
    CHECK(strstr(fake_modem_last_cmd(), "AT+MIPSEND=0,5") != NULL,
          "MIPSEND uses length form");

    /* ---- 5. 普通文本数据 ---- */
    fake_modem_inject_data(sock, "world");
    run_ms(100);
    memset(buf, 0, sizeof(buf));
    CHECK(drain_socket(sock, buf, (int)sizeof(buf)) == 5, "recv 5 bytes");
    CHECK(strcmp((char *)buf, "world") == 0, "plain text payload");

    /* ---- 6. payload 含 CRLF，且被串口分片切成 3 字节一段 ---- */
    fake_modem_set_read_limit(3U);
    fake_modem_inject_data(sock, "AB\r\nCD");
    run_ms(200);
    memset(buf, 0, sizeof(buf));
    CHECK(drain_socket(sock, buf, (int)sizeof(buf)) == 6,
          "recv 6 bytes across fragmented reads");
    CHECK(memcmp(buf, "AB\r\nCD", 6) == 0,
          "CRLF inside payload is not treated as boundary");
    fake_modem_set_read_limit(0U);

    /* ---- 7. payload 里伪装一条 URC，不能被误触发 ---- */
    {
        static const char disguised[] = "x\r\n+MIPCLOSE: 0\r\n";
        int expect = (int)strlen(disguised);

        fake_modem_set_read_limit(4U);
        fake_modem_inject_data(sock, disguised);
        run_ms(300);
        memset(buf, 0, sizeof(buf));
        CHECK(drain_socket(sock, buf, (int)sizeof(buf)) == expect,
              "recv disguised-URC payload in full");
        CHECK(memcmp(buf, disguised, (size_t)expect) == 0,
              "payload bytes match exactly");
    }
    CHECK(emda_socket_state_get(sock) == EMDA_SOCK_STATE_CONNECTED,
          "fake +MIPCLOSE inside payload does not close socket");
    fake_modem_set_read_limit(0U);

    /* ---- 8. payload 含 0x00 ---- */
    bin[0] = (uint8_t)'A';
    bin[1] = 0x00U;
    bin[2] = (uint8_t)'B';
    fake_modem_inject_data_bin(sock, bin, 3U);
    run_ms(100);
    memset(buf, 0, sizeof(buf));
    CHECK(drain_socket(sock, buf, (int)sizeof(buf)) == 3, "recv 3 bytes with 0x00");
    CHECK(buf[0] == 'A' && buf[1] == 0x00U && buf[2] == 'B',
          "0x00 inside payload preserved");

    /* ---- 9. 单条 1460 字节大 URC：解析器不能被卡死 ---- */
    for (int i = 0; i < 1460; i++)
    {
        buf[i] = (uint8_t)('a' + (i % 26));
    }
    fake_modem_inject_data_bin(sock, buf, 1460U);
    run_ms(400);
    memset(buf, 0, sizeof(buf));
    CHECK(drain_socket(sock, buf, (int)sizeof(buf)) == (int)EMDA_SOCK_RX_BUFFER_SIZE,
          "1460-byte URC parsed, socket buffer keeps capacity bytes");
    CHECK(emda_at_rx_count() == 0U, "rx ring drained after oversized urc");

    /* 后续事务仍然正常，说明解析器已重新同步 */
    s_evt_sent = 0;
    CHECK(emda_socket_send(sock, "ping", 4) == EMDA_EOK, "send after big urc");
    run_ms(300);
    CHECK(s_evt_sent == 1, "transaction works after big urc");

    /* ---- 10. 文本响应被拆成 2 字节一片，仍能匹配 OK ---- */
    s_exec_ok = 0;
    fake_modem_set_read_limit(2U);
    CHECK(emda_device_exec("AT+CPIN?\r\n", on_exec_resp, 2000U, on_exec_timeout) == EMDA_EOK,
          "device exec queued (fragmented read)");
    run_ms(600);
    CHECK(s_exec_ok == 1, "fragmented text response still matched");
    fake_modem_set_read_limit(0U);

    /* ---- 11. socket：关闭 ---- */
    CHECK(emda_socket_close(sock) == EMDA_EOK, "socket close");
    run_ms(100);
    CHECK(s_evt_closed == 1, "CLOSED event");
    CHECK(strstr(fake_modem_last_cmd(), "AT+MIPCLOSE=0") != NULL,
          "close uses MIPCLOSE");

    /* ---- 12. 超时兜底 ---- */
    s_exec_timeout = 0;
    CHECK(emda_device_exec("AT+NORESP\r\n", on_exec_resp, 100U, on_exec_timeout) == EMDA_EOK,
          "device exec no-response queued");
    run_ms(400);
    CHECK(s_exec_timeout == 1, "timeout callback fired");

    /* ---- 13. 驻网被拒 -> FAILED，之后慢速恢复 ---- */
    fake_modem_set_cereg_stat(3);
    CHECK(wait_net_state(EMDA_NET_STATE_FAILED, 40000) == 1,
          "CEREG stat=3 (registration denied) leads to FAILED");

    fake_modem_set_cereg_stat(1);
    CHECK(wait_net_state(EMDA_NET_STATE_READY, 40000) == 1,
          "FAILED recovers to READY by slow retry");

    /* ---- 14. MQTT：连接 -> 订阅 -> 发布 -> 下行 -> 断开 ---- */
    CHECK(emda_ml307a_mqtt_connect(0, "broker.emqx.io", 1883, "dev1", "u", "p") == EMDA_EOK,
          "mqtt connect issued");
    run_ms(400);
    CHECK(emda_ml307a_mqtt_connected(0) == 1, "mqtt conn URC marks connected");

    CHECK(emda_ml307a_mqtt_subscribe(0, "dev/down", 1) == EMDA_EOK,
          "mqtt subscribe issued");
    run_ms(300);

    CHECK(emda_ml307a_mqtt_publish(0, "dev/up", (const uint8_t *)"hello", 5, 0) == EMDA_EOK,
          "mqtt publish uses data mode");
    run_ms(400);

    fake_modem_inject_mqtt_publish(0, "dev/down", "reboot");
    run_ms(300);
    CHECK(s_mqtt_msg_got == 1, "mqtt downlink delivered");
    CHECK(strcmp(s_mqtt_topic, "dev/down") == 0, "mqtt topic parsed");
    CHECK(s_mqtt_len == 6 && memcmp(s_mqtt_payload, "reboot", 6) == 0,
          "mqtt payload parsed by length");

    CHECK(emda_ml307a_mqtt_disconnect(0) == EMDA_EOK, "mqtt disconnect issued");
    run_ms(400);
    CHECK(emda_ml307a_mqtt_connected(0) == 0, "conn state 2 marks disconnected");

    CHECK(s_evt_connect_failed == 0, "no unexpected connect failure");

    printf("\r\n==== %s (%d failures) ====\r\n",
           (s_failures == 0) ? "PASS" : "FAIL", s_failures);
    return (s_failures == 0) ? 0 : 1;
}
