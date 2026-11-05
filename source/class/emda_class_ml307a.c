/**
 * @file emda_class_ml307a.c
 * @author Zorian(1551769443@qq.com)
 * @brief  EMDA 模组类实现：ML307A (4G Cat.1)
 *
 * 控制面状态阶梯(监控项驱动)：
 *
 *   UNKNOWN --ATE0--> INIT --AT+CPIN?--> REGISTERING --AT+CEREG?--> REGISTERED
 *           --AT+CGATT?--> READY --(CSQ/CEREG 巡检)--> LOST --AT--> INIT
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

/* ======================================================================== */
/*                          内部状态                                          */
/* ======================================================================== */

static int s_rssi = 99;
static int s_ber = 99;

/* 当前在途的 socket 操作 */
static int s_send_sock = -1;

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
        /* SIM 未插/未就绪：保持 INIT 继续重试，不把状态机打死 */
        return EMDA_EOK;
    }
    return EMDA_ERROR;
}

/* 解析 +CEREG: <n>,<stat>，stat=1(本地)/5(漫游) 视为已驻网 */
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

    if (stat == 1 || stat == 5)
    {
        emda_net_state_set(EMDA_NET_STATE_REGISTERED);
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

    if (stat != 1 && stat != 5)
    {
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

/* ======================================================================== */
/*                          控制面：模组初始化                                */
/* ======================================================================== */

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
            /* 掉网后重新探测模组，回到 INIT 重新爬梯 */
            .name = "relost",
            .cmd = "AT\r\n",
            .state = EMDA_NET_STATE_LOST,
            .interval_ms = 5000U,
            .resp_cb = ml307a_relost_cb,
            .timeout_cb = ml307a_timeout_cb,
            .timeout_ms = 1000U,
        },
    };

    (void)dev;

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

/* ---- URC 表 ---- */

static const emda_urc_t s_ml307a_urc[] = {
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
