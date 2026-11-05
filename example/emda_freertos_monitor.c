/**
 * @file emda_freertos_monitor.c
 * @author Zorian(1551769443@qq.com)
 * @brief  EMDA 在 FreeRTOS 下的实际用法：模组存活 / SIM / IP / MQTT 状态监控
 *
 * 这个文件和真实项目里的写法一致，可以直接改改就用：
 *
 *   需求                指令                监控状态                     间隔
 *   ------------------  ------------------  ---------------------------  --------
 *   模组是否还活着       AT                  UNKNOWN / LOST               3s / 5s
 *   SIM 是否就绪         AT+CPIN?            INIT                         3s
 *   是否驻网             AT+CEREG?           REGISTERING                  3s
 *   数据附着             AT+CGATT?           REGISTERED                   2s
 *   信号质量             AT+CSQ              READY                        30s
 *   模组 IP 地址         AT+CGPADDR=1        READY                        60s
 *   MQTT 连接状态        AT+MQTTSTATE=0      READY                        30s
 *
 * 前 5 行由模组类 class/emda_class_ml307a.c 内置（ATE0/CPIN/CEREG/CGATT/CSQ），
 * 本文件只追加后 2 行，并处理 MQTT 的连接结果 URC 与重连。
 *
 * 编译：需要 FreeRTOS 头文件，且 EMDA_HEARTBEAT_PERIOD_MS 必须等于 RTOS tick 周期。
 *
 * @version 0.4
 * @date 2026-11-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#include <stdio.h>
#include <stdlib.h>
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

#define APP_MQTT_ID           (0)
#define APP_MQTT_HOST         "mqtt.example.com"
#define APP_MQTT_PORT         (1883)
#define APP_MQTT_CLIENT_ID    "dev-0001"
#define APP_MQTT_USER         "device"
#define APP_MQTT_PASS         "secret"
#define APP_MQTT_TOPIC_UP     "dev/0001/up"

#define APP_MQTT_RETRY_MS     (5000U)  /* 重连间隔 */
#define APP_MQTT_PROBE_MS     (30000U) /* MQTT 状态巡检间隔 */
#define APP_IP_PROBE_MS       (60000U) /* IP 巡检间隔 */
#define APP_CMD_TIMEOUT_MS    (2000U)
#define APP_MQTT_CONN_TO_MS   (30000U) /* 建链最多等 30s(URC 才给结果) */

#define APP_TASK_STACK_WORDS  (1536U)
#define APP_TASK_PRIO         (4U)     /* 比串口中断低，比业务任务略高即可 */

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

static volatile int s_mqtt_connected = 0;  /* 业务视角：MQTT 可用 */
static volatile int s_mqtt_pending = 0;    /* 已下发 AT+MQTTCONN，等 URC */
static emda_tick_t s_last_conn_tick = 0;

/* ======================================================================== */
/*                          MQTT 状态解析                                     */
/* ======================================================================== */
/*
 * 注意两个状态枚举完全不同，不能共用解析：
 *   AT+MQTTSTATE -> +MQTTSTATE: <state>   1=连接中/重连 2=连接成功 3=连接断开
 *   URC          -> +MQTTURC: "conn",<id>,<conn_state>
 *                                          0=连接成功 1=正在重连 2=客户端断开
 *                                          3=服务器拒绝 4=服务器断开
 */

/* 从字符串里跳过逗号读一个整数(用于解析 URC 的数值字段) */
static const char *s_int_field(const char *p, int *out)
{
    while (*p != '\0' && *p != ',')
    {
        p++;
    }
    if (*p != ',')
    {
        return NULL;
    }

    *out = atoi(p + 1);
    return p + 1;
}

/**
 * @brief MQTT URC：+MQTTURC: "conn",<id>,<conn_state>
 *
 * 注意这里是"行模式"解析：publish 的 payload 里可能含 \r\n，
 * 如果需要订阅大报文，应按长度定界写法(参考 class 里的 s_rtcp_seg_len)。
 */
static int urc_mqtt(const uint8_t *data, uint16_t len)
{
    char line[96];
    uint16_t n;
    const char *p;
    int id = 0;
    int state = -1;

    if (data == NULL || len == 0U)
    {
        return EMDA_ERROR;
    }

    n = (len < (uint16_t)(sizeof(line) - 1U)) ? len : (uint16_t)(sizeof(line) - 1U);
    memcpy(line, data, n);
    line[n] = '\0';

    if (strstr(line, "\"conn\"") != NULL)
    {
        p = s_int_field(line, &id);
        if (p == NULL || s_int_field(p, &state) == NULL)
        {
            return EMDA_ERROR;
        }

        switch (state)
        {
        case 0: /* 连接成功 */
            s_mqtt_connected = 1;
            s_mqtt_pending = 0;
            break;

        case 1: /* 正在重连：等模组自己恢复，不重复下发 MQTTCONN */
            s_mqtt_connected = 0;
            s_mqtt_pending = 1;
            break;

        default: /* 2=客户端断开 3=服务器拒绝 4=服务器断开 */
            s_mqtt_connected = 0;
            s_mqtt_pending = 0;
            break;
        }
        return EMDA_EOK;
    }

    if (strstr(line, "\"drop\"") != NULL || strstr(line, "\"timeout\"") != NULL)
    {
        EMDA_LOG("[app] mqtt urc: %s\r\n", line);
        return EMDA_EOK;
    }

    return EMDA_ERROR;
}

static const emda_urc_t s_app_urc[] = {
    {"+MQTTURC:", NULL, urc_mqtt},
};

/* ======================================================================== */
/*                          监控项响应回调                                    */
/* ======================================================================== */

/**
 * @brief AT+CGPADDR=1 -> +CGPADDR: 1,"10.12.34.56"
 */
static int cb_ipaddr(uint8_t *buffer, uint16_t len)
{
    const char *p = strstr((const char *)buffer, "+CGPADDR:");
    const char *q;
    size_t n;

    (void)len;

    if (p == NULL)
    {
        return EMDA_ERROR;
    }

    p = strchr(p, ',');
    if (p == NULL)
    {
        return EMDA_ERROR;
    }
    p++;
    while (*p == '"')
    {
        p++;
    }

    q = p;
    while (*q != '\0' && *q != '"' && *q != '\r' && *q != '\n')
    {
        q++;
    }

    n = (size_t)(q - p);
    if (n == 0U || n >= sizeof(s_ip))
    {
        return EMDA_ERROR;
    }

    memcpy(s_ip, p, n);
    s_ip[n] = '\0';
    s_ip_valid = 1;
    return EMDA_EOK;
}

/**
 * @brief AT+MQTTSTATE=0 -> +MQTTSTATE: 2
 */
static int cb_mqtt_state(uint8_t *buffer, uint16_t len)
{
    const char *p = strstr((const char *)buffer, "+MQTTSTATE:");

    (void)len;

    if (p == NULL)
    {
        return EMDA_ERROR;
    }

    s_mqtt_connected = (atoi(p + 11) == 2) ? 1 : 0;
    return EMDA_EOK;
}

static int cb_mon_timeout(char *cmd)
{
    EMDA_LOG("[app] timeout: %s", (cmd != NULL) ? cmd : "?");
    return EMDA_EOK;
}

/* ======================================================================== */
/*                          应用层追加的监控项                                */
/* ======================================================================== */

/*
 * 这些项在 emda_device_start() 之后追加，不需要改库。
 * state 字段表示"只在模组处于该状态时才调度"。
 */
static const emda_monitor_t s_app_monitors[] = {
    {
        .name = "ipaddr",
        .cmd = "AT+CGPADDR=1\r\n",
        .state = EMDA_NET_STATE_READY,
        .interval_ms = APP_IP_PROBE_MS,
        .resp_cb = cb_ipaddr,
        .timeout_cb = cb_mon_timeout,
        .timeout_ms = APP_CMD_TIMEOUT_MS,
    },
    {
        .name = "mqtt_state",
        .cmd = "AT+MQTTSTATE=0\r\n",
        .state = EMDA_NET_STATE_READY,
        .interval_ms = APP_MQTT_PROBE_MS,
        .resp_cb = cb_mqtt_state,
        .timeout_cb = cb_mon_timeout,
        .timeout_ms = APP_CMD_TIMEOUT_MS,
    },
};

/* ======================================================================== */
/*                          MQTT 连接管理                                    */
/* ======================================================================== */

static int mqtt_conn_resp(uint8_t *buffer, uint16_t len)
{
    (void)len;

    if (strstr((const char *)buffer, "OK") != NULL)
    {
        /* 命令已受理，真正的结果由 +MQTTURC: "conn" 上报 */
        return EMDA_EOK;
    }
    if (strstr((const char *)buffer, "ERROR") != NULL)
    {
        s_mqtt_pending = 0;
        return EMDA_EOK;
    }
    return EMDA_ERROR;
}

static int mqtt_conn_timeout(char *cmd)
{
    (void)cmd;
    s_mqtt_pending = 0;
    return EMDA_EOK;
}

/**
 * @brief 下发 MQTT 建链（业务层动作，不在 AT 回调里做）
 * @return EMDA_EOK=已入队
 */
static int mqtt_connect(void)
{
    char cmd[EMDA_CMD_BUFFER_SIZE];
    int n;

    n = snprintf(cmd, sizeof(cmd),
                 "AT+MQTTCONN=%d,\"%s\",%d,\"%s\",\"%s\",\"%s\"\r\n",
                 APP_MQTT_ID, APP_MQTT_HOST, APP_MQTT_PORT,
                 APP_MQTT_CLIENT_ID, APP_MQTT_USER, APP_MQTT_PASS);
    if (n <= 0 || n >= (int)sizeof(cmd))
    {
        return EMDA_ENOMEM;
    }

    if (emda_device_exec(cmd, mqtt_conn_resp, APP_MQTT_CONN_TO_MS,
                         mqtt_conn_timeout) != EMDA_EOK)
    {
        return EMDA_EBUSY;
    }

    s_mqtt_pending = 1;
    s_last_conn_tick = emda_tick_get();
    return EMDA_EOK;
}

/**
 * @brief 发布一条上行数据（QoS0，直接内联 payload）
 *
 * resp_cb 传 NULL 表示不关心响应内容：AT 核心收到模组第一条响应行即结束事务。
 */
int app_mqtt_publish(const char *payload, uint16_t len)
{
    char cmd[EMDA_CMD_BUFFER_SIZE];
    int n;

    n = snprintf(cmd, sizeof(cmd), "AT+MQTTPUB=%d,\"%s\",0,0,0,%u,\"%s\"\r\n",
                 APP_MQTT_ID, APP_MQTT_TOPIC_UP, (unsigned)len, payload);
    if (n <= 0 || n >= (int)sizeof(cmd))
    {
        return EMDA_ENOMEM;
    }

    return emda_device_exec(cmd, NULL, APP_CMD_TIMEOUT_MS, NULL);
}

/**
 * @brief 业务侧状态机：网络就绪 + MQTT 掉线 -> 按间隔重连
 *
 * 放在 poll 循环里每轮调用，非阻塞。
 */
static void mqtt_service(void)
{
    if (emda_net_state_get() != EMDA_NET_STATE_READY)
    {
        return;
    }
    if (s_mqtt_connected || s_mqtt_pending)
    {
        return;
    }
    if ((emda_tick_get() - s_last_conn_tick) < emda_tick_from_ms(APP_MQTT_RETRY_MS))
    {
        return;
    }

    (void)mqtt_connect();
}

/* ======================================================================== */
/*                          引擎任务                                         */
/* ======================================================================== */

static void emda_task(void *arg)
{
    TickType_t last_wake;
    uint32_t i;

    (void)arg;

    /* 1. 绑定模组类 + 启动（内部完成串口初始化与状态阶梯注册） */
    if (emda_device_register(&emda_class_ml307a, NULL) != EMDA_EOK)
    {
        vTaskDelete(NULL);
        return;
    }
    if (emda_device_start() != EMDA_EOK)
    {
        vTaskDelete(NULL);
        return;
    }

    /* 2. 追加应用层监控项（IP / MQTT 状态） */
    for (i = 0U; i < sizeof(s_app_monitors) / sizeof(s_app_monitors[0]); i++)
    {
        (void)emda_monitor_add(&s_app_monitors[i]);
    }

    /* 3. 注册 MQTT URC（表满时调大 EMDA_URC_TABLE_SIZE） */
    (void)emda_at_urc_attach(s_app_urc,
                              (uint32_t)(sizeof(s_app_urc) / sizeof(s_app_urc[0])));

    /* 4. 固定节拍驱动引擎 */
    last_wake = xTaskGetTickCount();
    for (;;)
    {
        emda_poll();     /* 收数据 + 解析 URC/响应 + 巡检调度 */
        mqtt_service();  /* 业务侧连接状态机 */
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
    return s_mqtt_connected;
}

const char *app_get_ip(void)
{
    return s_ip_valid ? s_ip : "";
}
