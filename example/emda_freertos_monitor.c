/**
 * @file emda_freertos_monitor.c
 * @author Zorian(1551769443@qq.com)
 * @brief  EMDA 在 FreeRTOS 下的实际用法：开机自检 -> 驻网 -> IP -> MQTT
 *
 * 完整链路（指令依据 document/ML307A/ 手册）：
 *
 *   [0] 模组在线   AT / ATE0
 *   [1] SIM        AT+CPIN?            (10 未插卡 11 需要PIN 13 卡失效 14 卡忙)
 *   [2] 信号       AT+CSQ              (99 = 不可检测)
 *   [3] 驻网       AT+CEREG?           (1/5/6/7/9 已注册，3 被拒)
 *   [4] 附着+IP    AT+CGATT? / AT+MIPCALL?
 *   [5] MQTT       AT+MQTTCONN / AT+MQTTSUB / AT+MQTTPUB / AT+MQTTDISC
 *
 * [0]~[4] 全部由 class/emda_class_ml307a.c 的状态阶梯自动完成，
 * 本文件只做三件事：追加 IP 巡检、驱动 MQTT 连接状态机、处理下行消息。
 *
 * 注意：ML307A 是自动拨号，手册明确不建议用 AT+CGACT 手工激活 PDP，
 *       第 1 路默认承载由模组自动配置，所以这里不出现 CGDCONT/CGACT。
 *
 * 编译：需要 FreeRTOS 头文件，且 EMDA_HEARTBEAT_PERIOD_MS 必须等于 RTOS tick 周期。
 *
 * @version 0.5
 * @date 2026-11-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "emda_at.h"
#include "emda_config.h"
#include "emda_device.h"
#include "emda_socket.h"
#include "class/emda_class_ml307a.h"

/* ======================================================================== */
/*                              应用配置                                     */
/* ======================================================================== */

/*
 * APN 是选择性配置，不是必须的。
 *
 * eSIM 只是卡的物理形态(嵌入式芯片 + 远程写 profile)，和 APN 无关：
 *   - eSIM 里装的是公网 profile -> 一般用模组默认 APN 即可，不用配；
 *   - eSIM 里装的是物联网 profile(CMIOT/CTNET/UNINET 等) 且模组自动配置
 *     没对上 -> 才需要手工配一次。
 *
 * 判断方法：先不配 APN 跑一遍，若 AT+MIPCALL? 长期是 state=0 或地址为空，
 * 再打开下面这两行（宏 + emda_task 里的调用）试。
 * 注意手工配置的 APN 掉电不保存，库会在每次开机的 INIT 阶段重配一次。
 */
/* #define APP_APN               "CMIOT" */

#define APP_MQTT_ID           (0)
#define APP_MQTT_HOST         "broker.emqx.io"
#define APP_MQTT_PORT         (1883)
#define APP_MQTT_CLIENT_ID    "dev-0001"
#define APP_MQTT_USER         "device"
#define APP_MQTT_PASS         "secret"
#define APP_MQTT_TOPIC_UP     "dev/0001/up"
#define APP_MQTT_TOPIC_DOWN   "dev/0001/down"

#define APP_MQTT_RETRY_MS     (5000U)  /* 掉线重连间隔 */
#define APP_MQTT_PROBE_MS     (30000U) /* 已连接时的状态兜底巡检 */
#define APP_IP_PROBE_MS       (60000U) /* IP 巡检间隔 */
#define APP_CMD_TIMEOUT_MS    (2000U)

#define APP_TASK_STACK_WORDS  (1536U)
#define APP_TASK_PRIO         (4U)

/*
 * 关键：EMDA_HEARTBEAT_PERIOD_MS 必须等于 FreeRTOS 的 tick 周期，
 * 否则所有 timeout / interval 会按比例失真。
 *
 *   configTICK_RATE_HZ = 1000 -> EMDA_HEARTBEAT_PERIOD_MS = 1
 *   configTICK_RATE_HZ = 100  -> EMDA_HEARTBEAT_PERIOD_MS = 10
 */
#if defined(configTICK_RATE_HZ)
#if (1000 / configTICK_RATE_HZ) != EMDA_HEARTBEAT_PERIOD_MS
#error "EMDA_HEARTBEAT_PERIOD_MS 必须等于 FreeRTOS tick 周期(ms)"
#endif
#endif

/* ======================================================================== */
/*                              应用状态                                     */
/* ======================================================================== */

static volatile int s_ip_valid = 0;
static char s_ip[24];

static emda_tick_t s_last_conn_tick = 0;
static emda_tick_t s_last_probe_tick = 0;
static int s_subscribed = 0;
static int s_was_connected = 0;

/* ======================================================================== */
/*                          下行消息回调                                     */
/* ======================================================================== */

/**
 * @brief MQTT 下行消息（payload 是原始字节，可能含 0x00，必须按 len 处理）
 */
static void on_mqtt_msg(int cid, const char *topic,
                        const uint8_t *payload, uint16_t len)
{
    char text[64];
    uint16_t n = (len < (uint16_t)(sizeof(text) - 1U)) ? len : (uint16_t)(sizeof(text) - 1U);

    memcpy(text, payload, n);
    text[n] = '\0';

    EMDA_LOG("[app] mqtt rx cid=%d topic=%s len=%u: %s\r\n",
             cid, topic, (unsigned)len, text);

    /* 业务处理：命令下发、OTA 通知等 */
}

/* ======================================================================== */
/*                          监控项（应用层追加）                              */
/* ======================================================================== */

/**
 * @brief AT+MIPCALL? -> +MIPCALL: <cid>,<state>,"<ipv4>"[,"<ipv6>"]
 *
 * 这是 ML307A 应用层拨号状态，比标准 3GPP 的 +CGPADDR 更贴合模组模型：
 *   state=1 表示该路 PDP 已拨号成功，后面跟实际分配的地址。
 *   CEREG=1 只代表注册成功，不代表已拨号拿到 IP。
 *
 * 注意：ML307A 是自动拨号，别用 AT+CGACT 去激活 PDP（手册明确不建议），
 *       需要断开时用 AT+MIPCALL=0,<cid>，需要去激活用 AT+CFUN=4。
 */
static int cb_mipcall(uint8_t *buffer, uint16_t len)
{
    const char *p = strstr((const char *)buffer, "+MIPCALL:");
    const char *q;
    size_t n;

    (void)len;

    if (p == NULL)
    {
        return EMDA_ERROR;
    }

    p += 9; /* 跳过 "+MIPCALL:" */
    while (*p == ' ')
    {
        p++;
    }

    /* <cid> */
    if (*p < '0' || *p > '9')
    {
        return EMDA_ERROR;
    }
    while (*p >= '0' && *p <= '9')
    {
        p++;
    }
    if (*p != ',')
    {
        return EMDA_ERROR;
    }
    p++;

    /* <state>：0=未拨号 1=已连接 */
    if (*p != '1')
    {
        s_ip_valid = 0;
        return EMDA_EOK;
    }
    while (*p >= '0' && *p <= '9')
    {
        p++;
    }
    if (*p != ',')
    {
        return EMDA_ERROR;
    }
    p++;

    /* "<ipv4>" */
    if (*p == '"')
    {
        p++;
    }
    q = p;
    while (*q != '\0' && *q != '"')
    {
        q++;
    }

    n = (size_t)(q - p);
    if (n == 0U || n >= sizeof(s_ip))
    {
        s_ip_valid = 0;
        return EMDA_EOK;
    }

    memcpy(s_ip, p, n);
    s_ip[n] = '\0';
    s_ip_valid = 1;
    return EMDA_EOK;
}

static int cb_mon_timeout(char *cmd)
{
    EMDA_LOG("[app] timeout: %s", (cmd != NULL) ? cmd : "?");
    return EMDA_EOK;
}

static const emda_monitor_t s_app_monitors[] = {
    {
        .name = "ipaddr",
        .cmd = "AT+MIPCALL?\r\n",
        .state = EMDA_NET_STATE_READY,
        .interval_ms = APP_IP_PROBE_MS,
        .resp_cb = cb_mipcall,
        .timeout_cb = cb_mon_timeout,
        .timeout_ms = APP_CMD_TIMEOUT_MS,
    },
};

/* ======================================================================== */
/*                          MQTT 连接管理                                    */
/* ======================================================================== */

/**
 * @brief 发布一条上行数据
 * @note  底层是数据模式：AT+MQTTPUB 后等 '>' 再送 payload
 */
int app_mqtt_publish(const char *topic, const char *payload, uint16_t len)
{
    return emda_ml307a_mqtt_publish(APP_MQTT_ID, topic,
                                    (const uint8_t *)payload, len, 0);
}

/**
 * @brief MQTT 状态机：网络就绪后建链 -> 订阅 -> 掉线重连
 *
 * 非阻塞，放在 poll 循环里每轮调用。
 */
static void mqtt_service(void)
{
    emda_tick_t now = emda_tick_get();
    int connected;

    if (emda_net_state_get() != EMDA_NET_STATE_READY)
    {
        return;
    }

    connected = emda_ml307a_mqtt_connected(APP_MQTT_ID);

    /* 连接状态发生 0 -> 1 变化：需要重新订阅 */
    if (connected && !s_was_connected)
    {
        s_subscribed = 0;
        s_last_probe_tick = now;
    }
    s_was_connected = connected;

    if (!connected)
    {
        if ((now - s_last_conn_tick) >= emda_tick_from_ms(APP_MQTT_RETRY_MS))
        {
            s_last_conn_tick = now;
            (void)emda_ml307a_mqtt_connect(APP_MQTT_ID, APP_MQTT_HOST,
                                           APP_MQTT_PORT, APP_MQTT_CLIENT_ID,
                                           APP_MQTT_USER, APP_MQTT_PASS);
        }
        return;
    }

    /* 已连接：订阅一次 */
    if (!s_subscribed)
    {
        if (emda_ml307a_mqtt_subscribe(APP_MQTT_ID, APP_MQTT_TOPIC_DOWN, 1) == EMDA_EOK)
        {
            s_subscribed = 1;
        }
    }

    /* 已连接：低频查一次状态，兜底 URC 丢失 */
    if ((now - s_last_probe_tick) >= emda_tick_from_ms(APP_MQTT_PROBE_MS))
    {
        s_last_probe_tick = now;
        (void)emda_ml307a_mqtt_poll_state(APP_MQTT_ID);
    }
}

/* ======================================================================== */
/*                          引擎任务                                         */
/* ======================================================================== */

static void emda_task(void *arg)
{
    TickType_t last_wake;
    uint32_t i;

    (void)arg;

    /* 1. 绑定模组类 + 启动（内部完成 0~4 步的状态阶梯） */
    if (emda_device_register(&emda_class_ml307a, NULL) != EMDA_EOK)
    {
        vTaskDelete(NULL);
        return;
    }

    /*
     * APN 必须在 start 之前设置：init 时才会插入 AT+CGDCONT 监控项。
     * 默认不配置（走模组自动配置），需要时连同上面 APP_APN 一起打开。
     */
#if defined(APP_APN)
    (void)emda_ml307a_set_apn(APP_APN);
#endif

    if (emda_device_start() != EMDA_EOK)
    {
        vTaskDelete(NULL);
        return;
    }

    /* 2. 追加应用层监控项（IP） */
    for (i = 0U; i < sizeof(s_app_monitors) / sizeof(s_app_monitors[0]); i++)
    {
        (void)emda_monitor_add(&s_app_monitors[i]);
    }

    /* 3. 注册 MQTT 下行消息回调 */
    emda_ml307a_mqtt_set_msg_cb(on_mqtt_msg);

    /* 4. 固定节拍驱动引擎 */
    last_wake = xTaskGetTickCount();
    for (;;)
    {
        emda_poll();     /* 收数据 + 解析 URC/响应 + 巡检调度 */
        mqtt_service();  /* 业务侧 MQTT 状态机 */
        emda_heartbeat();/* 一次 = EMDA_HEARTBEAT_PERIOD_MS */

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(EMDA_HEARTBEAT_PERIOD_MS));
    }
}

/**
 * @brief 在 main() 里调用一次，创建引擎任务
 */
void app_start(void)
{
    (void)xTaskCreate(emda_task, "emda",
                      APP_TASK_STACK_WORDS,
                      NULL, APP_TASK_PRIO, NULL);
}

/* ======================================================================== */
/*                          业务侧查询接口                                    */
/* ======================================================================== */

int app_mqtt_is_online(void)
{
    return emda_ml307a_mqtt_connected(APP_MQTT_ID);
}

const char *app_get_ip(void)
{
    return s_ip_valid ? s_ip : "";
}
