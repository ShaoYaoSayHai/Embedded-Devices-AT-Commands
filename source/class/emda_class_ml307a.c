/**
 * @file emda_class_ml307a.c
 * @author Zorian(1551769443@qq.com)
 * @brief  EMDA 模组类实现：ML307A (4G Cat.1)
 *
 * 控制面状态阶梯(监控项驱动)：
 *
 *   UNKNOWN --ATE0--> INIT --AT+CPIN?--> REGISTERING --AT+CEREG?--> REGISTERED
 *           --AT+CGATT?--> READY --(CSQ/CEREG/CPIN 巡检)--> LOST --AT--> INIT
 *
 * 运行期巡检覆盖三层，缺一层都会"看起来在线其实不能用"：
 *   SIM  : AT+CPIN?   运行中被拔卡/卡失效 -> 回 INIT 重新爬梯
 *   驻网 : AT+CEREG?  stat 0/2/4 -> LOST；stat 3(注册被拒) -> FAILED
 *   信号 : AT+CSQ     rssi=99 表示无信号(寻网阶段也能判读)
 *   FAILED 每 30s 用 AT 探活回 INIT，避免注册被拒后永久卡死。
 *
 * 数据面(普通接收模式 access_mode=0)：
 *   AT+MIPOPEN=<id>,"TCP",<addr>,<port>,60,0   ->  URC +MIPOPEN:<id>,0
 *   AT+MIPSEND=<id>,<len>  ->  ">"  ->  <原始数据>  ->  OK / +MIPSEND
 *   AT+MIPCLOSE=<id>                            ->  OK / URC +MIPCLOSE:<id>
 *   收到数据：URC +MIPURC: "rtcp",<id>,<len>,<data>
 *
 * 接收数据在 URC 表里声明为「带长度的整段」：AT 核心按 <len> 精确圈定
 * header + payload，二进制内容不会被当成文本行扫描，含 \r\n / 0x00 也安全。
 *
 * @version 0.3
 * @date 2026-11-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "emda_class_ml307a.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../emda_at.h"
#include "../emda_socket.h"

/* ======================================================================== */
/*                          参数配置                                          */
/* ======================================================================== */

#define ML307A_CONNECT_TIMEOUT_S (60)     /* AT+MIPOPEN <timeout> 参数(秒) */
#define ML307A_OPEN_TIMEOUT_MS   (15000U) /* 建链事务超时 */
#define ML307A_SEND_TIMEOUT_MS   (10000U) /* 发送事务超时 */
#define ML307A_CLOSE_TIMEOUT_MS  (5000U)  /* 关闭事务超时 */

/* "+MIPURC: \"rtcp\"," 的长度，用于跳过前缀直接解析字段 */
#define ML307A_RTCP_PREFIX_LEN   (16U)

/* 单条 URC 允许的最大数据长度(手册: <send_length>/单包上限 1460) */
#define ML307A_MAX_DATA_LEN      (1460)

/* MQTT 单条消息上限(手册: ML307A <msg_len> 最大 1024) */
#define ML307A_MQTT_MAX_PAYLOAD  (1024)
#define ML307A_MQTT_CMD_TIMEOUT_MS (5000U)
#define ML307A_MQTT_PUB_TIMEOUT_MS (15000U)

/* ======================================================================== */
/*                          内部状态                                          */
/* ======================================================================== */

static int s_rssi = 99;
static int s_ber = 99;

/* 当前在途的 socket 操作 */
static int s_send_sock = -1;

/* APN 配置命令(为空表示不配置，使用模组默认) */
static char s_apn_cmd[96];

/*
 * 建链请求可能排队(业务连续 create+connect 多路)，而 AT 事务串行执行，
 * 用一个小 FIFO 记录「谁在排队」，回调按顺序弹出来对号入座。
 */
static uint8_t s_open_fifo[EMDA_SOCK_MAX];
static uint8_t s_open_head = 0U;
static uint8_t s_open_count = 0U;

static void s_open_push(int sock)
{
    if (s_open_count >= EMDA_SOCK_MAX)
    {
        return;
    }
    s_open_fifo[(s_open_head + s_open_count) % EMDA_SOCK_MAX] = (uint8_t)sock;
    s_open_count++;
}

static int s_open_pop(void)
{
    int sock;

    if (s_open_count == 0U)
    {
        return -1;
    }
    sock = (int)s_open_fifo[s_open_head];
    s_open_head = (uint8_t)((s_open_head + 1U) % EMDA_SOCK_MAX);
    s_open_count--;
    return sock;
}

/* 前置声明：注册 URC 表(表与处理函数定义在后面) */
static int ml307a_urc_attach(void);

/* ======================================================================== */
/*                          小工具：整数解析                                  */
/* ======================================================================== */

/**
 * @brief 解析十进制整数，返回指向分隔符之后的指针
 * @param p   输入位置
 * @param out 输出值
 * @return 分隔符(',')之后的位置；解析失败返回 NULL
 */
static const char *s_parse_int(const char *p, int *out)
{
    int value = 0;
    int digits = 0;

    if (p == NULL || out == NULL)
    {
        return NULL;
    }

    while (*p == ' ' || *p == '"')
    {
        p++;
    }

    while (*p >= '0' && *p <= '9')
    {
        value = value * 10 + (*p - '0');
        p++;
        digits++;
    }

    if (digits == 0)
    {
        return NULL;
    }

    *out = value;

    if (*p == ',')
    {
        p++;
    }
    return p;
}

/* ======================================================================== */
/*                          控制面：监控回调                                  */
/* ======================================================================== */

static int ml307a_timeout_cb(char *cmd)
{
    EMDA_LOG("[ml307a] timeout: %s", (cmd != NULL) ? cmd : "?");
    return EMDA_EOK;
}

/* UNKNOWN: 关回显，同时确认模组在线 */
static int ml307a_ate0_cb(uint8_t *buffer, uint16_t len)
{
    (void)len;
    if (strstr((const char *)buffer, "OK") != NULL)
    {
        emda_net_state_set(EMDA_NET_STATE_INIT);
        return EMDA_EOK;
    }
    return EMDA_ERROR;
}

/* INIT: SIM 是否就绪 */
static int ml307a_cpin_cb(uint8_t *buffer, uint16_t len)
{
    (void)len;
    if (strstr((const char *)buffer, "+CPIN: READY") != NULL)
    {
        emda_net_state_set(EMDA_NET_STATE_REGISTERING);
        return EMDA_EOK;
    }
    if (strstr((const char *)buffer, "ERROR") != NULL)
    {
        /*
         * +CME ERROR: 10 = SIM 未插入, 11 = 需要 PIN, 13 = SIM 故障
         * 保持 INIT 每 3s 重试，不推进也不打死状态机
         */
        EMDA_LOG("[ml307a] sim not ready\r\n");
        return EMDA_EOK;
    }
    return EMDA_ERROR;
}

/* READY: SIM 巡检，检测运行期拔卡 */
static int ml307a_cpin_poll_cb(uint8_t *buffer, uint16_t len)
{
    (void)len;
    if (strstr((const char *)buffer, "+CPIN: READY") != NULL)
    {
        return EMDA_EOK;
    }

    /* 运行中卡被拔出/失效：退回 INIT 重新爬梯 */
    EMDA_LOG("[ml307a] sim lost\r\n");
    emda_net_state_set(EMDA_NET_STATE_INIT);
    return EMDA_EOK;
}

/*
 * 3GPP 27.007 的 <stat>：
 *   1 本地注册  5 漫游注册  6/7 仅短信注册  9 CSFB not preferred
 * 以上都表示"已驻网"，可以继续建链；0/2/4 是未注册，3 是被拒绝。
 */
static int s_cereg_registered(int stat)
{
    return (stat == 1 || stat == 5 || stat == 6 || stat == 7 || stat == 9) ? 1 : 0;
}

/* 解析 +CEREG: <n>,<stat> */
static int ml307a_parse_cereg(const char *buffer, int *stat)
{
    const char *p = strstr(buffer, "+CEREG:");
    const char *q;
    int mode = 0;

    if (p == NULL || stat == NULL)
    {
        return EMDA_ERROR;
    }

    q = s_parse_int(p + 7, &mode);
    if (q == NULL)
    {
        return EMDA_ERROR;
    }
    if (s_parse_int(q, stat) == NULL)
    {
        return EMDA_ERROR;
    }
    return EMDA_EOK;
}

/* REGISTERING: 查询驻网 */
static int ml307a_cereg_cb(uint8_t *buffer, uint16_t len)
{
    int stat = 0;

    (void)len;
    if (ml307a_parse_cereg((const char *)buffer, &stat) != EMDA_EOK)
    {
        return EMDA_ERROR;
    }

    if (s_cereg_registered(stat))
    {
        emda_net_state_set(EMDA_NET_STATE_REGISTERED);
    }
    else if (stat == 3)
    {
        /* 注册被拒(SIM 欠费/无权限/APN 不符)：每 3s 重试没有意义，转 FAILED 慢速重试 */
        EMDA_LOG("[ml307a] registration denied\r\n");
        emda_net_state_set(EMDA_NET_STATE_FAILED);
    }
    else
    {
        EMDA_LOG("[ml307a] registering, stat=%d\r\n", stat);
    }
    return EMDA_EOK;
}

/* REGISTERED: 检查 GPRS 附着 */
static int ml307a_cgatt_cb(uint8_t *buffer, uint16_t len)
{
    (void)len;
    if (strstr((const char *)buffer, "+CGATT: 1") != NULL)
    {
        emda_net_state_set(EMDA_NET_STATE_READY);
        return EMDA_EOK;
    }
    if (strstr((const char *)buffer, "+CGATT: 0") != NULL)
    {
        emda_net_state_set(EMDA_NET_STATE_LOST);
        return EMDA_EOK;
    }
    return EMDA_ERROR;
}

/* READY: 信号质量巡检 */
static int ml307a_csq_cb(uint8_t *buffer, uint16_t len)
{
    const char *p = strstr((const char *)buffer, "+CSQ:");
    const char *q;
    int rssi = 99;
    int ber = 99;

    (void)len;
    if (p == NULL)
    {
        return EMDA_ERROR;
    }

    q = s_parse_int(p + 5, &rssi);
    if (q != NULL)
    {
        (void)s_parse_int(q, &ber);
    }

    s_rssi = rssi;
    s_ber = ber;
    return EMDA_EOK;
}

/* READY: 驻网巡检，掉网则转入 LOST */
static int ml307a_cereg_poll_cb(uint8_t *buffer, uint16_t len)
{
    int stat = 0;

    (void)len;
    if (ml307a_parse_cereg((const char *)buffer, &stat) != EMDA_EOK)
    {
        return EMDA_ERROR;
    }

    if (s_cereg_registered(stat))
    {
        return EMDA_EOK;
    }

    if (stat == 3)
    {
        EMDA_LOG("[ml307a] registration denied\r\n");
        emda_net_state_set(EMDA_NET_STATE_FAILED);
    }
    else
    {
        EMDA_LOG("[ml307a] network lost, stat=%d\r\n", stat);
        emda_net_state_set(EMDA_NET_STATE_LOST);
    }
    return EMDA_EOK;
}

/* LOST: 重新探测模组，回到 INIT 重新爬梯 */
static int ml307a_relost_cb(uint8_t *buffer, uint16_t len)
{
    (void)len;
    if (strstr((const char *)buffer, "OK") != NULL)
    {
        emda_net_state_set(EMDA_NET_STATE_INIT);
        return EMDA_EOK;
    }
    return EMDA_ERROR;
}

/* FAILED: 慢速重试，避免因注册被拒而永久卡死 */
static int ml307a_recover_cb(uint8_t *buffer, uint16_t len)
{
    (void)len;
    if (strstr((const char *)buffer, "OK") != NULL)
    {
        emda_net_state_set(EMDA_NET_STATE_INIT);
        return EMDA_EOK;
    }
    return EMDA_ERROR;
}

/* ======================================================================== */
/*                          控制面：模组初始化                                */
/* ======================================================================== */

/**
 * @brief 配置 PDP 上下文 APN（可选）
 *
 * 手册：第 1 路是默认承载，模组自动配置，手工配置掉电不保存。
 * 公网卡不用配；物联网卡(CMIOT)或私有 APN 必须每次开机重配一次。
 */
int emda_ml307a_set_apn(const char *apn)
{
    if (apn == NULL || apn[0] == '\0' || strlen(apn) > 32U)
    {
        return EMDA_EINVAL;
    }

    (void)snprintf(s_apn_cmd, sizeof(s_apn_cmd),
                   "AT+CGDCONT=1,\"IPV4V6\",\"%s\"\r\n", apn);
    return EMDA_EOK;
}

static int ml307a_cgdcont_cb(uint8_t *buffer, uint16_t len)
{
    (void)len;

    if (strstr((const char *)buffer, "OK") != NULL)
    {
        EMDA_LOG("[ml307a] apn configured\r\n");
        return EMDA_EOK;
    }
    return EMDA_ERROR;
}

/**
 * @brief 注册监控项(状态阶梯) + URC 回调
 * @note 这里只「登记要做的事」，真正的执行由 emda_device_poll 按状态调度
 */
static int ml307a_init(emda_device_t *dev)
{
    static const emda_monitor_t s_items[] = {
        {
            .name = "ate0",
            .cmd = "ATE0\r\n",
            .state = EMDA_NET_STATE_UNKNOWN,
            .interval_ms = 0U,
            .resp_cb = ml307a_ate0_cb,
            .timeout_cb = ml307a_timeout_cb,
            .timeout_ms = 1000U,
        },
        {
            .name = "cpin",
            .cmd = "AT+CPIN?\r\n",
            .state = EMDA_NET_STATE_INIT,
            .interval_ms = 3000U,
            .resp_cb = ml307a_cpin_cb,
            .timeout_cb = ml307a_timeout_cb,
            .timeout_ms = 2000U,
        },
        {
            /* 寻网阶段：每 3s 查一次是否驻网成功 */
            .name = "cereg",
            .cmd = "AT+CEREG?\r\n",
            .state = EMDA_NET_STATE_REGISTERING,
            .interval_ms = 3000U,
            .resp_cb = ml307a_cereg_cb,
            .timeout_cb = ml307a_timeout_cb,
            .timeout_ms = 2000U,
        },
        {
            /* 寻网阶段同时看信号：rssi=99 说明根本没信号，不是"还在搜网" */
            .name = "csq_search",
            .cmd = "AT+CSQ\r\n",
            .state = EMDA_NET_STATE_REGISTERING,
            .interval_ms = 10000U,
            .resp_cb = ml307a_csq_cb,
            .timeout_cb = ml307a_timeout_cb,
            .timeout_ms = 2000U,
        },
        {
            /* 已驻网后再确认 GPRS 附着，就可进入 READY 建链 */
            .name = "cgatt",
            .cmd = "AT+CGATT?\r\n",
            .state = EMDA_NET_STATE_REGISTERED,
            .interval_ms = 2000U,
            .resp_cb = ml307a_cgatt_cb,
            .timeout_cb = ml307a_timeout_cb,
            .timeout_ms = 2000U,
        },
        {
            /* 运行期巡检：信号质量 */
            .name = "csq",
            .cmd = "AT+CSQ\r\n",
            .state = EMDA_NET_STATE_READY,
            .interval_ms = 30000U,
            .resp_cb = ml307a_csq_cb,
            .timeout_cb = ml307a_timeout_cb,
            .timeout_ms = 2000U,
        },
        {
            /* 运行期巡检：驻网是否还在(同一条指令，不同名字/回调) */
            .name = "cereg_poll",
            .cmd = "AT+CEREG?\r\n",
            .state = EMDA_NET_STATE_READY,
            .interval_ms = 30000U,
            .resp_cb = ml307a_cereg_poll_cb,
            .timeout_cb = ml307a_timeout_cb,
            .timeout_ms = 2000U,
        },
        {
            /* 运行期巡检：SIM 是否还在(拔卡/卡失效要能发现) */
            .name = "cpin_poll",
            .cmd = "AT+CPIN?\r\n",
            .state = EMDA_NET_STATE_READY,
            .interval_ms = 60000U,
            .resp_cb = ml307a_cpin_poll_cb,
            .timeout_cb = ml307a_timeout_cb,
            .timeout_ms = 2000U,
        },
        {
            /* 掉网后重新探测模组，回到 INIT 重新爬梯 */
            .name = "relost",
            .cmd = "AT\r\n",
            .state = EMDA_NET_STATE_LOST,
            .interval_ms = 5000U,
            .resp_cb = ml307a_relost_cb,
            .timeout_cb = ml307a_timeout_cb,
            .timeout_ms = 1000U,
        },
        {
            /* 注册被拒后不频繁重试，30s 回 INIT 重新爬梯 */
            .name = "recover",
            .cmd = "AT\r\n",
            .state = EMDA_NET_STATE_FAILED,
            .interval_ms = 30000U,
            .resp_cb = ml307a_recover_cb,
            .timeout_cb = ml307a_timeout_cb,
            .timeout_ms = 1000U,
        },
    };

    (void)dev;

    /*
     * APN 项必须排在 cpin 之前：两者都只在 INIT 状态调度，而调度器在"同样最久
     * 未执行"时取表中靠前的那条。cpin 一旦成功就把状态推到 REGISTERING，
     * 排在后面的 APN 项就永远轮不到了。
     * 间隔给一天，效果等同"每次开机跑一次"。
     */
    if (s_apn_cmd[0] != '\0')
    {
        emda_monitor_t apn_item = {
            .name = "cgdcont",
            .cmd = s_apn_cmd,
            .state = EMDA_NET_STATE_INIT,
            .interval_ms = 86400000U,
            .resp_cb = ml307a_cgdcont_cb,
            .timeout_cb = ml307a_timeout_cb,
            .timeout_ms = 3000U,
        };

        if (emda_monitor_add(&apn_item) != EMDA_EOK)
        {
            EMDA_LOG("[ml307a] monitor table full (apn)\r\n");
            return EMDA_ENOMEM;
        }
    }

    for (uint32_t i = 0U; i < sizeof(s_items) / sizeof(s_items[0]); i++)
    {
        if (emda_monitor_add(&s_items[i]) != EMDA_EOK)
        {
            EMDA_LOG("[ml307a] monitor table full\r\n");
            return EMDA_ENOMEM;
        }
    }

    s_send_sock = -1;
    s_open_head = 0U;
    s_open_count = 0U;

    return ml307a_urc_attach();
}

static int ml307a_control(emda_device_t *dev, int cmd, void *arg)
{
    (void)dev;

    switch (cmd)
    {
    case EMDA_ML307A_CTRL_GET_CSQ:
    {
        int *out = (int *)arg;
        if (out == NULL)
        {
            return EMDA_EINVAL;
        }
        out[0] = s_rssi;
        out[1] = s_ber;
        return EMDA_EOK;
    }

    default:
        return EMDA_ENOSYS;
    }
}

/* ======================================================================== */
/*                          数据面：socket 回调                               */
/* ======================================================================== */

static int ml307a_open_cb(uint8_t *buffer, uint16_t len)
{
    (void)len;

    if (strstr((const char *)buffer, "OK") != NULL)
    {
        /* 命令已被接受，建链结果等 URC +MIPOPEN:<id>,<result> */
        (void)s_open_pop();
        return EMDA_EOK;
    }

    if (strstr((const char *)buffer, "ERROR") != NULL)
    {
        int sock = s_open_pop();
        if (sock >= 0)
        {
            emda_socket_report(sock, EMDA_SOCK_EVT_CONNECT_FAILED);
        }
        return EMDA_EOK;
    }
    return EMDA_ERROR;
}

static int ml307a_open_timeout_cb(char *cmd)
{
    int sock;

    (void)cmd;
    sock = s_open_pop();
    if (sock >= 0)
    {
        emda_socket_report(sock, EMDA_SOCK_EVT_CONNECT_FAILED);
    }
    return EMDA_EOK;
}

static int ml307a_send_cb(uint8_t *buffer, uint16_t len)
{
    (void)len;

    if (strstr((const char *)buffer, "OK") != NULL)
    {
        if (s_send_sock >= 0)
        {
            emda_socket_report(s_send_sock, EMDA_SOCK_EVT_SENT);
            s_send_sock = -1;
        }
        return EMDA_EOK;
    }

    if (strstr((const char *)buffer, "ERROR") != NULL)
    {
        if (s_send_sock >= 0)
        {
            emda_socket_report(s_send_sock, EMDA_SOCK_EVT_SEND_FAILED);
            s_send_sock = -1;
        }
        return EMDA_EOK;
    }
    return EMDA_ERROR;
}

static int ml307a_send_timeout_cb(char *cmd)
{
    (void)cmd;
    if (s_send_sock >= 0)
    {
        emda_socket_report(s_send_sock, EMDA_SOCK_EVT_SEND_FAILED);
        s_send_sock = -1;
    }
    return EMDA_EOK;
}

static int ml307a_close_cb(uint8_t *buffer, uint16_t len)
{
    (void)len;

    /* 关闭是「尽力而为」：本地状态在提交时已置为关闭，这里只负责结束事务 */
    if (strstr((const char *)buffer, "OK") != NULL ||
        strstr((const char *)buffer, "ERROR") != NULL)
    {
        return EMDA_EOK;
    }
    return EMDA_ERROR;
}

/* ======================================================================== */
/*                          数据面：socket 操作                               */
/* ======================================================================== */

static int ml307a_sock_open(emda_device_t *dev, int sock, emda_sock_type_t type,
                            const char *host, uint16_t port)
{
    char cmd[EMDA_CMD_BUFFER_SIZE];
    const char *proto = (type == EMDA_SOCK_DGRAM) ? "UDP" : "TCP";
    int n;

    (void)dev;

    if (host == NULL || sock < 0)
    {
        return EMDA_EINVAL;
    }

    n = snprintf(cmd, sizeof(cmd), "AT+MIPOPEN=%d,\"%s\",\"%s\",%u,%d,0\r\n",
                 sock, proto, host, (unsigned)port, ML307A_CONNECT_TIMEOUT_S);
    if (n <= 0 || n >= (int)sizeof(cmd))
    {
        return EMDA_ENOMEM;
    }

    if (emda_at_submit(cmd, ml307a_open_cb, ML307A_OPEN_TIMEOUT_MS,
                       ml307a_open_timeout_cb) != EMDA_EOK)
    {
        return EMDA_EBUSY;
    }

    s_open_push(sock);
    return EMDA_EOK;
}

static int ml307a_sock_send(emda_device_t *dev, int sock, const void *data, uint16_t len)
{
    char cmd[EMDA_CMD_BUFFER_SIZE];
    int n;

    (void)dev;

    if (data == NULL || len == 0U || sock < 0)
    {
        return EMDA_EINVAL;
    }

    /* 两段式：先发长度，等 ">" 提示符，再送原始数据 */
    n = snprintf(cmd, sizeof(cmd), "AT+MIPSEND=%d,%u\r\n", sock, (unsigned)len);
    if (n <= 0 || n >= (int)sizeof(cmd))
    {
        return EMDA_ENOMEM;
    }

    s_send_sock = sock;
    if (emda_at_submit_raw(cmd, ">", (const uint8_t *)data, len,
                           ml307a_send_cb, ML307A_SEND_TIMEOUT_MS,
                           ml307a_send_timeout_cb) != EMDA_EOK)
    {
        s_send_sock = -1;
        return EMDA_EBUSY;
    }
    return EMDA_EOK;
}

static int ml307a_sock_close(emda_device_t *dev, int sock)
{
    char cmd[EMDA_CMD_BUFFER_SIZE];
    int n;

    (void)dev;

    if (sock < 0)
    {
        return EMDA_EINVAL;
    }

    n = snprintf(cmd, sizeof(cmd), "AT+MIPCLOSE=%d\r\n", sock);
    if (n <= 0 || n >= (int)sizeof(cmd))
    {
        return EMDA_ENOMEM;
    }

    if (emda_at_submit(cmd, ml307a_close_cb, ML307A_CLOSE_TIMEOUT_MS,
                       ml307a_timeout_cb) != EMDA_EOK)
    {
        return EMDA_EBUSY;
    }

    /* 本地立即释放 socket 资源，模组的 +MIPCLOSE URC 后到则被忽略 */
    emda_socket_report(sock, EMDA_SOCK_EVT_CLOSED);
    return EMDA_EOK;
}

/* ======================================================================== */
/*                          MQTT 接口                                        */
/* ======================================================================== */
/*
 * 连接流程（手册 MQTT 用户手册 3.2/3.3/3.5）：
 *   1. AT+MQTTCONN=<id>,"host",<port>,"clientid","user","pass"  -> OK
 *      真正的结果由 URC 上报：+MQTTURC: "conn",<id>,0 才是连上
 *   2. AT+MQTTSUB=<id>,"topic",<qos>  -> +MQTTSUB: <id>,<mid> + OK
 *      订阅结果：+MQTTURC: "suback",<id>,<mid>,<code>
 *   3. AT+MQTTPUB=<id>,"topic",<qos>,0,0,<len>  -> '>' -> payload
 *                                                  -> +MQTTPUB: <id>,<mid>,<len> + OK
 *
 * 注意：MQTTCFG/SUB/PUB 的响应前缀(+MQTTSUB:/+MQTTPUB:)不能注册成 URC，
 *       否则会被 URC 抢走、resp_cb 收不到 OK。
 */

static volatile int s_mqtt_connected[EMDA_ML307A_MQTT_MAX];
static emda_ml307a_mqtt_msg_cb_t s_mqtt_msg_cb = NULL;

/* 主动查询状态时记录 cid（AT+MQTTSTATE 的响应里没有 cid） */
static int s_mqtt_poll_cid = -1;

static int s_mqtt_cid_valid(int cid)
{
    return (cid >= 0 && cid < EMDA_ML307A_MQTT_MAX) ? 1 : 0;
}

void emda_ml307a_mqtt_set_msg_cb(emda_ml307a_mqtt_msg_cb_t cb)
{
    s_mqtt_msg_cb = cb;
}

int emda_ml307a_mqtt_connected(int cid)
{
    if (!s_mqtt_cid_valid(cid))
    {
        return 0;
    }
    return s_mqtt_connected[cid];
}

/* 通用命令响应：OK / ERROR 都算"事务结束"，具体结果看 URC */
static int s_mqtt_cmd_resp(uint8_t *buffer, uint16_t len)
{
    (void)len;
    if (strstr((const char *)buffer, "OK") != NULL)
    {
        return EMDA_EOK;
    }
    if (strstr((const char *)buffer, "ERROR") != NULL)
    {
        return EMDA_EOK;
    }
    return EMDA_ERROR;
}

static int s_mqtt_cmd_timeout(char *cmd)
{
    EMDA_LOG("[ml307a] mqtt timeout: %s", (cmd != NULL) ? cmd : "?");
    return EMDA_EOK;
}

/* +MQTTSTATE: <1连接中|2成功|3断开> */
static int s_mqtt_state_resp(uint8_t *buffer, uint16_t len)
{
    const char *p = strstr((const char *)buffer, "+MQTTSTATE:");

    (void)len;

    if (p == NULL)
    {
        return EMDA_ERROR;
    }
    if (s_mqtt_cid_valid(s_mqtt_poll_cid))
    {
        s_mqtt_connected[s_mqtt_poll_cid] = (atoi(p + 11) == 2) ? 1 : 0;
    }
    return EMDA_EOK;
}

int emda_ml307a_mqtt_connect(int cid, const char *host, uint16_t port,
                             const char *client_id,
                             const char *user, const char *pass)
{
    char cmd[EMDA_CMD_BUFFER_SIZE];
    int n;

    if (!s_mqtt_cid_valid(cid) || host == NULL || client_id == NULL)
    {
        return EMDA_EINVAL;
    }

    if (user != NULL)
    {
        n = snprintf(cmd, sizeof(cmd),
                     "AT+MQTTCONN=%d,\"%s\",%u,\"%s\",\"%s\",\"%s\"\r\n",
                     cid, host, (unsigned)port, client_id, user,
                     (pass != NULL) ? pass : "");
    }
    else
    {
        n = snprintf(cmd, sizeof(cmd), "AT+MQTTCONN=%d,\"%s\",%u,\"%s\"\r\n",
                     cid, host, (unsigned)port, client_id);
    }
    if (n <= 0 || n >= (int)sizeof(cmd))
    {
        return EMDA_ENOMEM;
    }

    return emda_at_submit(cmd, s_mqtt_cmd_resp, ML307A_MQTT_CMD_TIMEOUT_MS,
                          s_mqtt_cmd_timeout);
}

int emda_ml307a_mqtt_subscribe(int cid, const char *topic, int qos)
{
    char cmd[EMDA_CMD_BUFFER_SIZE];
    int n;

    if (!s_mqtt_cid_valid(cid) || topic == NULL || qos < 0 || qos > 2)
    {
        return EMDA_EINVAL;
    }

    n = snprintf(cmd, sizeof(cmd), "AT+MQTTSUB=%d,\"%s\",%d\r\n", cid, topic, qos);
    if (n <= 0 || n >= (int)sizeof(cmd))
    {
        return EMDA_ENOMEM;
    }

    return emda_at_submit(cmd, s_mqtt_cmd_resp, ML307A_MQTT_CMD_TIMEOUT_MS,
                          s_mqtt_cmd_timeout);
}

int emda_ml307a_mqtt_publish(int cid, const char *topic,
                             const uint8_t *payload, uint16_t len, int qos)
{
    char cmd[EMDA_CMD_BUFFER_SIZE];
    int n;

    if (!s_mqtt_cid_valid(cid) || topic == NULL || payload == NULL ||
        len == 0U || qos < 0 || qos > 2)
    {
        return EMDA_EINVAL;
    }
    if (len > ML307A_MQTT_MAX_PAYLOAD)
    {
        return EMDA_ENOMEM;
    }

    /* 省略 <message> 进入数据模式，收到 '>' 后送原始 payload */
    n = snprintf(cmd, sizeof(cmd), "AT+MQTTPUB=%d,\"%s\",%d,0,0,%u\r\n",
                 cid, topic, qos, (unsigned)len);
    if (n <= 0 || n >= (int)sizeof(cmd))
    {
        return EMDA_ENOMEM;
    }

    return emda_at_submit_raw(cmd, ">", payload, len,
                              s_mqtt_cmd_resp, ML307A_MQTT_PUB_TIMEOUT_MS,
                              s_mqtt_cmd_timeout);
}

int emda_ml307a_mqtt_disconnect(int cid)
{
    char cmd[EMDA_CMD_BUFFER_SIZE];
    int n;

    if (!s_mqtt_cid_valid(cid))
    {
        return EMDA_EINVAL;
    }

    n = snprintf(cmd, sizeof(cmd), "AT+MQTTDISC=%d\r\n", cid);
    if (n <= 0 || n >= (int)sizeof(cmd))
    {
        return EMDA_ENOMEM;
    }

    return emda_at_submit(cmd, s_mqtt_cmd_resp, ML307A_MQTT_CMD_TIMEOUT_MS,
                          s_mqtt_cmd_timeout);
}

int emda_ml307a_mqtt_poll_state(int cid)
{
    char cmd[EMDA_CMD_BUFFER_SIZE];
    int n;

    if (!s_mqtt_cid_valid(cid))
    {
        return EMDA_EINVAL;
    }

    n = snprintf(cmd, sizeof(cmd), "AT+MQTTSTATE=%d\r\n", cid);
    if (n <= 0 || n >= (int)sizeof(cmd))
    {
        return EMDA_ENOMEM;
    }

    s_mqtt_poll_cid = cid;
    return emda_at_submit(cmd, s_mqtt_state_resp, ML307A_MQTT_CMD_TIMEOUT_MS,
                          s_mqtt_cmd_timeout);
}

/* ======================================================================== */
/*                          URC 处理                                          */
/* ======================================================================== */
/*
 * 每条 URC 在表里声明「前缀 + 段长计算 + 处理函数」。
 * AT 核心保证回调拿到的是完整且连续的一条，分片由核心负责等齐。
 * 带裸数据的条目必须排在通用 "+MIPURC:" 之前，否则会被按行吃掉。
 */

/**
 * @brief 从一段非 NUL 结尾的数据里读取十进制字段
 * @param buf        数据起点
 * @param avail      可用长度
 * @param pos        输入/输出：当前偏移
 * @param out        输出值
 * @param need_comma 1=字段后必须是逗号，0=字段是最后一个
 * @return EMDA_EOK / EMDA_ETIMEOUT(数据未收全) / EMDA_ERROR(格式不符)
 */
static int s_read_field(const uint8_t *buf, uint16_t avail, uint16_t *pos,
                        int *out, int need_comma)
{
    int value = 0;
    int digits = 0;

    if (buf == NULL || pos == NULL || out == NULL)
    {
        return EMDA_EINVAL;
    }

    while (*pos < avail && (buf[*pos] == ' ' || buf[*pos] == '"'))
    {
        (*pos)++;
    }
    if (*pos >= avail)
    {
        return EMDA_ETIMEOUT;
    }

    while (*pos < avail && buf[*pos] >= '0' && buf[*pos] <= '9')
    {
        value = value * 10 + (int)(buf[*pos] - '0');
        (*pos)++;
        digits++;
    }

    if (digits == 0)
    {
        return EMDA_ERROR;
    }
    if (*pos >= avail)
    {
        return EMDA_ETIMEOUT;
    }
    if (need_comma)
    {
        if (buf[*pos] != ',')
        {
            return EMDA_ERROR;
        }
        (*pos)++;
    }

    *out = value;
    return EMDA_EOK;
}

/**
 * @brief 在一段非 NUL 结尾的数据里查找子串
 */
static int s_mem_contains(const uint8_t *buf, uint16_t len, const char *needle)
{
    uint16_t nlen = (uint16_t)strlen(needle);

    if (nlen == 0U || len < nlen)
    {
        return 0;
    }
    for (uint16_t i = 0U; (uint32_t)i + (uint32_t)nlen <= (uint32_t)len; i++)
    {
        if (memcmp(&buf[i], needle, nlen) == 0)
        {
            return 1;
        }
    }
    return 0;
}

/* ---- 数据上报：+MIPURC: "rtcp"|"rudp",<id>,<len>,<data> ---- */

/**
 * @brief 计算数据上报的整段长度
 * @return >0 总长度(header+payload)；0=等更多数据；<0=格式不认识
 */
static int s_rtcp_seg_len(const uint8_t *buf, uint16_t avail)
{
    uint16_t pos = ML307A_RTCP_PREFIX_LEN;
    int id = 0;
    int data_len = 0;
    int ret;

    ret = s_read_field(buf, avail, &pos, &id, 1);
    if (ret == EMDA_ETIMEOUT)
    {
        return 0;
    }
    if (ret != EMDA_EOK)
    {
        return -1;
    }

    ret = s_read_field(buf, avail, &pos, &data_len, 1);
    if (ret == EMDA_ETIMEOUT)
    {
        return 0;
    }
    if (ret != EMDA_EOK)
    {
        return -1;
    }

    if (data_len < 0 || data_len > ML307A_MAX_DATA_LEN)
    {
        return -1;
    }

    return (int)pos + data_len;
}

static int s_rtcp_payload(const uint8_t *data, uint16_t len)
{
    uint16_t pos = ML307A_RTCP_PREFIX_LEN;
    int id = 0;
    int data_len = 0;

    if (s_read_field(data, len, &pos, &id, 1) != EMDA_EOK)
    {
        return EMDA_ERROR;
    }
    if (s_read_field(data, len, &pos, &data_len, 1) != EMDA_EOK)
    {
        return EMDA_ERROR;
    }
    if (data_len < 0 || (uint32_t)pos + (uint32_t)data_len > (uint32_t)len)
    {
        return EMDA_ERROR;
    }

    emda_socket_input(id, &data[pos], (uint16_t)data_len);
    return EMDA_EOK;
}

/* ---- 文本 URC ---- */

static int s_urc_mipopen(const uint8_t *data, uint16_t len)
{
    uint16_t pos = 9U; /* "+MIPOPEN:" */
    int id = 0;
    int result = 0;

    if (s_read_field(data, len, &pos, &id, 1) != EMDA_EOK)
    {
        return EMDA_ERROR;
    }
    if (s_read_field(data, len, &pos, &result, 0) != EMDA_EOK)
    {
        return EMDA_ERROR;
    }

    /* result=0 成功，其余为失败 */
    emda_socket_report(id, (result == 0) ? EMDA_SOCK_EVT_CONNECTED
                                         : EMDA_SOCK_EVT_CONNECT_FAILED);
    return EMDA_EOK;
}

static int s_urc_mipclose(const uint8_t *data, uint16_t len)
{
    uint16_t pos = 10U; /* "+MIPCLOSE:" */
    int id = 0;

    if (s_read_field(data, len, &pos, &id, 0) != EMDA_EOK)
    {
        return EMDA_ERROR;
    }

    emda_socket_report(id, EMDA_SOCK_EVT_CLOSED);
    return EMDA_EOK;
}

static int s_urc_mipurc_other(const uint8_t *data, uint16_t len)
{
    /* +MIPURC: "disconn",<id>,<state> */
    if (s_mem_contains(data, len, "\"disconn\""))
    {
        uint16_t pos = 8U;
        int id = 0;

        while (pos < len && data[pos] != ',')
        {
            pos++;
        }
        if (pos < len)
        {
            pos++;
            if (s_read_field(data, len, &pos, &id, 0) == EMDA_EOK)
            {
                emda_socket_report(id, EMDA_SOCK_EVT_CLOSED);
            }
        }
        return EMDA_EOK;
    }

    if (s_mem_contains(data, len, "\"drop\""))
    {
        EMDA_LOG("[ml307a] urc drop\r\n");
    }

    return EMDA_EOK;
}

/* ---- MQTT 下行消息 ---- */
/*
 * +MQTTURC: "publish",<id>,<mid>,<topic>,<total_len>,<payload_len>,<payload>
 * 第二个数字是 <mid>（报文标识），不是 QoS；topic 带引号，payload 可能是二进制。
 */
#define ML307A_MQTT_PUB_PREFIX_LEN (20U) /* strlen("+MQTTURC: \"publish\",") */

static int s_mqtt_publish_seg_len(const uint8_t *buf, uint16_t avail)
{
    uint16_t pos = ML307A_MQTT_PUB_PREFIX_LEN;
    int id = 0;
    int mid = 0;
    int total = 0;
    int payload_len = 0;
    int ret;

    ret = s_read_field(buf, avail, &pos, &id, 1);
    if (ret == EMDA_ETIMEOUT)
    {
        return 0;
    }
    if (ret != EMDA_EOK)
    {
        return -1;
    }

    ret = s_read_field(buf, avail, &pos, &mid, 1);
    if (ret == EMDA_ETIMEOUT)
    {
        return 0;
    }
    if (ret != EMDA_EOK)
    {
        return -1;
    }

    /* topic: "..." */
    if (pos >= avail)
    {
        return 0;
    }
    if (buf[pos] != '"')
    {
        return -1;
    }
    pos++;
    while (pos < avail && buf[pos] != '"')
    {
        pos++;
    }
    if (pos >= avail)
    {
        return 0; /* topic 还没收全 */
    }
    pos++;
    if (pos >= avail)
    {
        return 0;
    }
    if (buf[pos] != ',')
    {
        return -1;
    }
    pos++;

    ret = s_read_field(buf, avail, &pos, &total, 1);
    if (ret == EMDA_ETIMEOUT)
    {
        return 0;
    }
    if (ret != EMDA_EOK)
    {
        return -1;
    }

    ret = s_read_field(buf, avail, &pos, &payload_len, 1);
    if (ret == EMDA_ETIMEOUT)
    {
        return 0;
    }
    if (ret != EMDA_EOK)
    {
        return -1;
    }

    if (payload_len < 0 || payload_len > ML307A_MQTT_MAX_PAYLOAD)
    {
        return -1;
    }

    return (int)pos + payload_len;
}

static int s_urc_mqtt_publish(const uint8_t *data, uint16_t len)
{
    uint16_t pos = ML307A_MQTT_PUB_PREFIX_LEN;
    uint16_t topic_start;
    uint16_t topic_len;
    int id = 0;
    int mid = 0;
    int total = 0;
    int payload_len = 0;
    char topic[64];

    if (s_read_field(data, len, &pos, &id, 1) != EMDA_EOK ||
        s_read_field(data, len, &pos, &mid, 1) != EMDA_EOK)
    {
        return EMDA_ERROR;
    }

    if (pos >= len || data[pos] != '"')
    {
        return EMDA_ERROR;
    }
    pos++;
    topic_start = pos;
    while (pos < len && data[pos] != '"')
    {
        pos++;
    }
    topic_len = (uint16_t)(pos - topic_start);
    if (pos >= len)
    {
        return EMDA_ERROR;
    }
    pos++;
    if (pos >= len || data[pos] != ',')
    {
        return EMDA_ERROR;
    }
    pos++;

    if (s_read_field(data, len, &pos, &total, 1) != EMDA_EOK ||
        s_read_field(data, len, &pos, &payload_len, 1) != EMDA_EOK)
    {
        return EMDA_ERROR;
    }
    if (payload_len < 0 ||
        (uint32_t)pos + (uint32_t)payload_len > (uint32_t)len)
    {
        return EMDA_ERROR;
    }

    if (topic_len >= sizeof(topic))
    {
        topic_len = (uint16_t)(sizeof(topic) - 1U);
    }
    memcpy(topic, &data[topic_start], topic_len);
    topic[topic_len] = '\0';

    if (s_mqtt_msg_cb != NULL)
    {
        s_mqtt_msg_cb(id, topic, &data[pos], (uint16_t)payload_len);
    }
    return EMDA_EOK;
}

/* ---- 其它 MQTT URC：conn / suback / puback / drop / timeout ---- */
static int s_urc_mqtt_other(const uint8_t *data, uint16_t len)
{
    /* +MQTTURC: "conn",<id>,<conn_state> */
    if (len >= 17U && memcmp(data, "+MQTTURC: \"conn\",", 17U) == 0)
    {
        uint16_t pos = 17U;
        int id = 0;
        int state = -1;

        if (s_read_field(data, len, &pos, &id, 1) != EMDA_EOK ||
            s_read_field(data, len, &pos, &state, 0) != EMDA_EOK)
        {
            return EMDA_ERROR;
        }

        if (!s_mqtt_cid_valid(id))
        {
            return EMDA_ERROR;
        }

        /* 0=连接成功，其余(1重连/2用户断开/3被拒/4服务器断开/5Ping超时/6网络异常)都算断开 */
        s_mqtt_connected[id] = (state == 0) ? 1 : 0;
        EMDA_LOG("[ml307a] mqtt conn id=%d state=%d\r\n", id, state);
        return EMDA_EOK;
    }

    /* suback / unsuback / puback / drop / timeout：交给业务日志观察即可 */
    EMDA_LOG("[ml307a] mqtt urc\r\n");
    return EMDA_EOK;
}

/* ---- URC 表 ---- */

static const emda_urc_t s_ml307a_urc[] = {
    {"+MQTTURC: \"publish\",", s_mqtt_publish_seg_len, s_urc_mqtt_publish},
    {"+MQTTURC:", NULL, s_urc_mqtt_other},
    {"+MIPURC: \"rtcp\",", s_rtcp_seg_len, s_rtcp_payload},
    {"+MIPURC: \"rudp\",", s_rtcp_seg_len, s_rtcp_payload},
    {"+MIPOPEN:", NULL, s_urc_mipopen},
    {"+MIPCLOSE:", NULL, s_urc_mipclose},
    {"+MIPURC:", NULL, s_urc_mipurc_other},
};

static int ml307a_urc_attach(void)
{
    return emda_at_urc_attach(s_ml307a_urc,
                              (uint32_t)(sizeof(s_ml307a_urc) / sizeof(s_ml307a_urc[0])));
}

/* ======================================================================== */
/*                          模组类描述符                                      */
/* ======================================================================== */

static const emda_device_ops_t s_ml307a_dev_ops = {
    .init = ml307a_init,
    .control = ml307a_control,
};

static const emda_socket_ops_t s_ml307a_sock_ops = {
    .open = ml307a_sock_open,
    .send = ml307a_sock_send,
    .close = ml307a_sock_close,
};

const emda_device_class_t emda_class_ml307a = {
    .name = "ml307a",
    .dev_ops = &s_ml307a_dev_ops,
    .sock_ops = &s_ml307a_sock_ops,
    .sock_num = EMDA_SOCK_MAX,
};
