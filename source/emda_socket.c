/**
 * @file emda_socket.c
 * @author Zorian(1551769443@qq.com)
 * @brief  EMDA 网络层(数据面)实现
 *
 * 层次关系：
 *   业务 -> emda_socket_send/recv  (本文件：缓冲与状态)
 *        -> 模组类 sock_ops->send  (class/emda_class_xxx.c：拼AT指令)
 *        -> emda_at_submit        (emda_at.c：串行执行事务)
 *
 * 为什么要收发缓冲：
 *   AT 事务是串行的，业务随时可能要求发送；本层用一个发送缓冲把数据接住，
 *   等管道空闲再下发，避免业务侧反复轮询 EMDA_EBUSY。
 *
 * @version 0.3
 * @date 2026-11-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "emda_socket.h"

#include <string.h>

/* ======================================================================== */
/*                          内部数据                                          */
/* ======================================================================== */

typedef struct
{
    uint8_t in_use; /* 槽位是否被占用(create 分配 / close 释放) */
    emda_sock_state_t state;
    emda_sock_type_t type;
    emda_sock_event_cb_t cb;

    /* 接收环形缓冲 */
    uint8_t rx[EMDA_SOCK_RX_BUFFER_SIZE];
    uint16_t rx_head;
    uint16_t rx_tail;
    uint16_t rx_count;

    /* 待发送数据 */
    uint8_t tx[EMDA_SOCK_TX_BUFFER_SIZE];
    uint16_t tx_len;
    uint8_t tx_pending;
} emda_sock_t;

static emda_sock_t s_socks[EMDA_SOCK_MAX];

/* ======================================================================== */
/*                          内部工具                                          */
/* ======================================================================== */

static int s_sock_valid(int sock)
{
    return (sock >= 0 && sock < (int)EMDA_SOCK_MAX) ? 1 : 0;
}

static const emda_socket_ops_t *s_sock_ops(void)
{
    const emda_device_class_t *cls = emda_device_class();

    if (cls == NULL || cls->sock_ops == NULL)
    {
        return NULL;
    }
    return cls->sock_ops;
}

static void s_sock_reset(emda_sock_t *s)
{
    s->in_use = 0U;
    s->state = EMDA_SOCK_STATE_CLOSED;
    s->type = EMDA_SOCK_STREAM;
    s->cb = NULL;
    s->rx_head = 0U;
    s->rx_tail = 0U;
    s->rx_count = 0U;
    s->tx_len = 0U;
    s->tx_pending = 0U;
}

/* ======================================================================== */
/*                          生命周期                                          */
/* ======================================================================== */

void emda_socket_init(void)
{
    for (uint32_t i = 0U; i < EMDA_SOCK_MAX; i++)
    {
        s_sock_reset(&s_socks[i]);
    }
}

int emda_socket_create(emda_sock_type_t type, emda_sock_event_cb_t cb)
{
    const emda_device_class_t *cls = emda_device_class();

    if (cls == NULL)
    {
        return EMDA_EINVAL;
    }

    for (uint32_t i = 0U; i < EMDA_SOCK_MAX; i++)
    {
        if (cls->sock_num != 0U && i >= (uint32_t)cls->sock_num)
        {
            break; /* 模组不支持这么多路连接 */
        }
        if (s_socks[i].in_use == 0U)
        {
            s_sock_reset(&s_socks[i]);
            s_socks[i].in_use = 1U;
            s_socks[i].type = type;
            s_socks[i].cb = cb;
            return (int)i;
        }
    }
    return EMDA_ENOMEM;
}

int emda_socket_connect(int sock, const char *host, uint16_t port)
{
    const emda_socket_ops_t *ops = s_sock_ops();
    int ret;

    if (!s_sock_valid(sock) || host == NULL || ops == NULL || ops->open == NULL)
    {
        return EMDA_EINVAL;
    }
    if (s_socks[sock].state != EMDA_SOCK_STATE_CLOSED)
    {
        return EMDA_EBUSY;
    }

    s_socks[sock].state = EMDA_SOCK_STATE_CONNECTING;

    ret = ops->open(emda_device_handle(), sock, s_socks[sock].type, host, port);
    if (ret != EMDA_EOK)
    {
        s_socks[sock].state = EMDA_SOCK_STATE_CLOSED;
    }
    return ret;
}

int emda_socket_send(int sock, const void *data, uint16_t len)
{
    emda_sock_t *s;

    if (!s_sock_valid(sock) || data == NULL || len == 0U)
    {
        return EMDA_EINVAL;
    }

    s = &s_socks[sock];
    if (s->state != EMDA_SOCK_STATE_CONNECTED)
    {
        return EMDA_EIO;
    }
    if (s->tx_pending != 0U)
    {
        return EMDA_EBUSY;
    }
    if (len > EMDA_SOCK_TX_BUFFER_SIZE)
    {
        return EMDA_ENOMEM;
    }

    memcpy(s->tx, data, len);
    s->tx_len = len;
    s->tx_pending = 1U;

    return EMDA_EOK;
}

int emda_socket_recv(int sock, void *buf, uint16_t len)
{
    emda_sock_t *s;
    uint16_t n = 0U;

    if (!s_sock_valid(sock) || buf == NULL || len == 0U)
    {
        return EMDA_EINVAL;
    }

    s = &s_socks[sock];

    while (n < len && s->rx_count > 0U)
    {
        ((uint8_t *)buf)[n] = s->rx[s->rx_tail];
        s->rx_tail = (uint16_t)((s->rx_tail + 1U) % EMDA_SOCK_RX_BUFFER_SIZE);
        s->rx_count--;
        n++;
    }

    return (int)n;
}

int emda_socket_close(int sock)
{
    const emda_socket_ops_t *ops = s_sock_ops();
    emda_sock_t *s;

    if (!s_sock_valid(sock))
    {
        return EMDA_EINVAL;
    }

    s = &s_socks[sock];
    if (s->state == EMDA_SOCK_STATE_CLOSED)
    {
        return EMDA_EOK;
    }

    s->state = EMDA_SOCK_STATE_CLOSING;

    if (ops == NULL || ops->close == NULL ||
        ops->close(emda_device_handle(), sock) != EMDA_EOK)
    {
        /* 模组不支持或下发失败：直接本地关闭 */
        emda_socket_report(sock, EMDA_SOCK_EVT_CLOSED);
    }
    return EMDA_EOK;
}

emda_sock_state_t emda_socket_state_get(int sock)
{
    if (!s_sock_valid(sock))
    {
        return EMDA_SOCK_STATE_CLOSED;
    }
    return s_socks[sock].state;
}

/* ======================================================================== */
/*                    模组类回调接口                                          */
/* ======================================================================== */

void emda_socket_input(int sock, const uint8_t *data, uint16_t len)
{
    emda_sock_t *s;
    uint16_t i;

    if (!s_sock_valid(sock) || data == NULL || len == 0U)
    {
        return;
    }

    s = &s_socks[sock];
    for (i = 0U; i < len; i++)
    {
        if (s->rx_count >= EMDA_SOCK_RX_BUFFER_SIZE)
        {
            EMDA_LOG("[emda_socket] sock%d rx overflow, %u bytes dropped\r\n",
                     sock, (unsigned)(len - i));
            break;
        }
        s->rx[s->rx_head] = data[i];
        s->rx_head = (uint16_t)((s->rx_head + 1U) % EMDA_SOCK_RX_BUFFER_SIZE);
        s->rx_count++;
    }
}

void emda_socket_report(int sock, emda_sock_event_t evt)
{
    emda_sock_t *s;
    emda_sock_event_cb_t cb;

    if (!s_sock_valid(sock))
    {
        return;
    }

    s = &s_socks[sock];
    if (s->in_use == 0U)
    {
        return; /* 已关闭，忽略迟到的模组上报 */
    }
    cb = s->cb;

    switch (evt)
    {
    case EMDA_SOCK_EVT_CONNECTED:
        s->state = EMDA_SOCK_STATE_CONNECTED;
        break;

    case EMDA_SOCK_EVT_CONNECT_FAILED:
        s->state = EMDA_SOCK_STATE_CLOSED;
        break;

    case EMDA_SOCK_EVT_SEND_FAILED:
        s->tx_pending = 0U;
        break;

    case EMDA_SOCK_EVT_SENT:
        s->tx_pending = 0U;
        break;

    case EMDA_SOCK_EVT_CLOSED:
        s->state = EMDA_SOCK_STATE_CLOSED;
        s->tx_pending = 0U;
        s->rx_head = 0U;
        s->rx_tail = 0U;
        s->rx_count = 0U;
        s->in_use = 0U; /* 关闭即释放槽位，可被 create 复用 */
        break;

    default:
        break;
    }

    EMDA_LOG("[emda_socket] sock%d evt=%d\r\n", sock, (int)evt);

    if (cb != NULL)
    {
        cb(sock, evt);
    }
}

/* ======================================================================== */
/*                          发送调度                                          */
/* ======================================================================== */

void emda_socket_poll(void)
{
    const emda_socket_ops_t *ops = s_sock_ops();

    if (ops == NULL || ops->send == NULL)
    {
        return;
    }

    for (uint32_t i = 0U; i < EMDA_SOCK_MAX; i++)
    {
        emda_sock_t *s = &s_socks[i];

        if (s->tx_pending == 0U || s->state != EMDA_SOCK_STATE_CONNECTED)
        {
            continue;
        }

        /* 管道忙则等下一轮；业务发送优先于监控巡检 */
        if (!emda_at_ready())
        {
            return;
        }

        if (ops->send(emda_device_handle(), (int)i, s->tx, s->tx_len) == EMDA_EOK)
        {
            /* 数据已拷入AT核心的发送缓冲，本层缓冲可立即复用 */
            s->tx_pending = 0U;
            s->tx_len = 0U;

            /* 一次 poll 只下发一笔，等管道空出来再处理下一个 socket */
            return;
        }
        else
        {
            emda_socket_report((int)i, EMDA_SOCK_EVT_SEND_FAILED);
        }
    }
}
