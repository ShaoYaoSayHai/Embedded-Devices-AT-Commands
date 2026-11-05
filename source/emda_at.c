/**
 * @file emda_at.c
 * @author Zorian(1551769443@qq.com)
 * @brief  EMDA AT 事务核心实现
 *
 * 接收侧不再使用「时间窗 + CRLF 快判」猜边界，改为按内容定界：
 *
 *   ring 头部
 *     ├─ a) 提示符(如 ">")        -> 发送两段式数据并消费
 *     ├─ b) URC 前缀命中          -> 按 seg_len 算整段长度，到齐才回调
 *     ├─ c) 文本行(CRLF)          -> 累积进响应缓冲交给 resp_cb
 *     └─ d) 都不够                -> 原样等待，下一轮继续
 *
 * 关键点：前缀命中但整段未收全时**直接返回等待**，不会把半条 URC 当文本行，
 * 也不会把二进制 payload 交给任何文本匹配。
 *
 * @version 0.4
 * @date 2026-11-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "emda_at.h"

#include <string.h>

/* ======================================================================== */
/*                          内部数据结构                                      */
/* ======================================================================== */

/* 指令队列条目 */
typedef struct
{
    char cmd[EMDA_CMD_BUFFER_SIZE];
    uint16_t cmd_len;
    emda_at_resp_cb_t resp_cb;
    emda_at_timeout_cb_t timeout_cb;
    uint32_t timeout_ms;
} emda_at_msg_t;

/* URC 表条目(内部缓存 prefix 长度，避免每轮 strlen) */
typedef struct
{
    const char *prefix;
    uint16_t prefix_len;
    emda_urc_seg_cb_t seg_len;
    emda_urc_cb_t handler;
} emda_urc_entry_t;

/* 接收环形缓冲：保证 [head, head+len) 连续，回绕前先压缩 */
static uint8_t s_rx[EMDA_RX_BUFFER_SIZE];
static uint16_t s_rx_head = 0U;
static uint16_t s_rx_len = 0U;

/* 当前在途事务的响应文本累积(仅文本，二进制不经过这里) */
static uint8_t s_resp[EMDA_RESP_BUFFER_SIZE + 1U];
static uint16_t s_resp_len = 0U;

/* URC 表 */
static emda_urc_entry_t s_urc[EMDA_URC_TABLE_SIZE];
static uint8_t s_urc_count = 0U;

/* 指令队列(环形缓冲) */
static emda_at_msg_t s_cmd_queue[EMDA_CMD_QUEUE_SIZE];
static volatile uint8_t s_q_head = 0U;
static volatile uint8_t s_q_tail = 0U;
static volatile uint8_t s_q_count = 0U;

/* 事务状态机运行时字段 */
static emda_at_state_t s_state = EMDA_AT_STATE_IDLE;
static emda_at_resp_cb_t s_resp_cb = NULL;
static emda_at_timeout_cb_t s_timeout_cb = NULL;
static char s_cur_cmd[EMDA_CMD_BUFFER_SIZE];
static uint16_t s_cur_cmd_len = 0U;
static uint32_t s_cur_timeout_ms = 0U;
static emda_tick_t s_send_tick = 0U;
static emda_tick_t s_ok_tick = 0U;
static emda_tick_t s_step_tick = 0U;

/* 两段式发送：提示符 + 原始数据 */
static const char *s_prompt = NULL;
static uint8_t s_payload[EMDA_TX_PAYLOAD_SIZE];
static uint16_t s_payload_len = 0U;
static uint8_t s_payload_sent = 0U;

/* 前置声明 */
static void s_load_command(emda_tick_t now,
                           const char *cmd,
                           uint16_t cmd_len,
                           emda_at_resp_cb_t resp_cb,
                           uint32_t timeout_ms,
                           emda_at_timeout_cb_t timeout_cb);

/* ======================================================================== */
/*                          接收环形缓冲                                      */
/* ======================================================================== */

/**
 * @brief 保证 [head, head+len) 连续：写指针到顶且还有未消费数据时前移
 */
static void s_rx_prepare(void)
{
    if (s_rx_head == 0U)
    {
        return;
    }
    if ((uint32_t)s_rx_head + (uint32_t)s_rx_len >= EMDA_RX_BUFFER_SIZE)
    {
        if (s_rx_len > 0U)
        {
            memmove(s_rx, &s_rx[s_rx_head], s_rx_len);
        }
        s_rx_head = 0U;
    }
}

static void s_rx_consume(uint16_t len)
{
    if (len >= s_rx_len)
    {
        s_rx_head = 0U;
        s_rx_len = 0U;
        return;
    }

    s_rx_head = (uint16_t)(s_rx_head + len);
    s_rx_len = (uint16_t)(s_rx_len - len);

    if (s_rx_len == 0U)
    {
        s_rx_head = 0U;
    }
}

/**
 * @brief 把串口当前可读的字节搬进 ring(有上限，避免单轮被数据占满)
 * @note 写不下就留在 port 层的 FIFO 里，形成天然背压，不丢数据
 */
static void s_rx_pump(void)
{
    for (uint32_t i = 0U; i < EMDA_RX_READS_PER_POLL; i++)
    {
        uint16_t space;
        int n;

        s_rx_prepare();
        space = (uint16_t)(EMDA_RX_BUFFER_SIZE - s_rx_head - s_rx_len);
        if (space == 0U)
        {
            break; /* ring 满，下一轮消费后再读 */
        }

        n = emda_port_uart_read(&s_rx[s_rx_head + s_rx_len], space, 0U);
        if (n <= 0)
        {
            break; /* 0=暂无数据, <0=错误 */
        }
        if ((uint16_t)n > space)
        {
            n = (int)space;
        }
        s_rx_len = (uint16_t)(s_rx_len + (uint16_t)n);
    }
}

/**
 * @brief 跳过 URC 尾部的 CRLF(最多2字节)
 */
static void s_rx_skip_eol(void)
{
    uint8_t n = 0U;

    while (n < 2U && s_rx_len > 0U)
    {
        uint8_t ch = s_rx[s_rx_head];
        if (ch != '\r' && ch != '\n')
        {
            break;
        }
        s_rx_consume(1U);
        n++;
    }
}

/* ======================================================================== */
/*                          响应文本累积                                      */
/* ======================================================================== */

static void s_resp_reset(void)
{
    s_resp[0] = '\0';
    s_resp_len = 0U;
}

/**
 * @brief 把一行文本追加到响应缓冲；溢出时丢弃最旧数据
 */
static void s_resp_append(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0U)
    {
        return;
    }

    if (len >= EMDA_RESP_BUFFER_SIZE)
    {
        memcpy(s_resp, &data[len - EMDA_RESP_BUFFER_SIZE], EMDA_RESP_BUFFER_SIZE);
        s_resp_len = EMDA_RESP_BUFFER_SIZE;
    }
    else
    {
        uint32_t combined = (uint32_t)s_resp_len + (uint32_t)len;
        if (combined > EMDA_RESP_BUFFER_SIZE)
        {
            uint16_t discard = (uint16_t)(combined - EMDA_RESP_BUFFER_SIZE);
            memmove(s_resp, &s_resp[discard], (uint16_t)(s_resp_len - discard));
            s_resp_len = (uint16_t)(s_resp_len - discard);
        }
        memcpy(&s_resp[s_resp_len], data, len);
        s_resp_len = (uint16_t)(s_resp_len + len);
    }

    s_resp[s_resp_len] = '\0';
}

/**
 * @brief 找一行文本的结束位置(含结尾 '\n')
 * @return 0 = 还没收到完整一行
 */
static uint16_t s_find_line(const uint8_t *data, uint16_t avail)
{
    for (uint16_t i = 0U; i < avail; i++)
    {
        if (data[i] == '\n')
        {
            return (uint16_t)(i + 1U);
        }
    }
    return 0U;
}

/* ======================================================================== */
/*                          URC 匹配                                          */
/* ======================================================================== */

/**
 * @brief 在缓冲头部探测一条 URC
 *
 * @param data    缓冲头指针(连续)
 * @param avail   可用字节数
 * @param entry   输出命中的表项
 * @param seg_len 输出整条 URC 的字节数
 * @return 1=命中且完整, 0=不是URC, -1=前缀命中但数据未收全, -2=长度非法
 */
static int s_urc_probe(const uint8_t *data,
                       uint16_t avail,
                       const emda_urc_entry_t **entry,
                       uint16_t *seg_len)
{
    for (uint8_t i = 0U; i < s_urc_count; i++)
    {
        const emda_urc_entry_t *e = &s_urc[i];
        uint16_t cmp_len;
        int total;

        if (e->prefix_len == 0U)
        {
            continue;
        }

        cmp_len = (avail < e->prefix_len) ? avail : e->prefix_len;
        if (memcmp(data, e->prefix, cmp_len) != 0)
        {
            continue;
        }
        if (avail < e->prefix_len)
        {
            return -1; /* 前缀还没收全 */
        }

        total = (e->seg_len != NULL) ? e->seg_len(data, avail)
                                     : (int)s_find_line(data, avail);
        if (total == 0)
        {
            return -1; /* 整段还没收全 */
        }
        if (total < 0)
        {
            continue; /* 该表项认为格式不符，交给后续表项 */
        }
        if (total > (int)EMDA_RX_BUFFER_SIZE)
        {
            return -2; /* 超过缓冲能力，无法定界 */
        }
        if (total > (int)avail)
        {
            return -1;
        }

        *entry = e;
        *seg_len = (uint16_t)total;
        return 1;
    }

    return 0;
}

/**
 * @brief 探测并分发一条 URC
 * @return 1=已消费(consumed 有效), 0=不是URC, -1=等更多, -2=长度非法
 */
static int s_urc_try(const uint8_t *data, uint16_t avail, uint16_t *consumed)
{
    const emda_urc_entry_t *entry = NULL;
    uint16_t seg = 0U;
    int ret;

    *consumed = 0U;

    ret = s_urc_probe(data, avail, &entry, &seg);
    if (ret != 1)
    {
        return ret;
    }

    if (entry->handler != NULL)
    {
        (void)entry->handler(data, seg);
    }

    *consumed = seg;
    return 1;
}

/* ======================================================================== */
/*                          两段式发送                                        */
/* ======================================================================== */

static void s_send_payload(void)
{
    if (s_prompt == NULL || s_payload_sent != 0U || s_payload_len == 0U)
    {
        return;
    }

    if (emda_port_uart_send(s_payload, s_payload_len) == EMDA_EOK)
    {
        s_payload_sent = 1U;
        EMDA_LOG("[emda_at] payload sent %u bytes\r\n", (unsigned)s_payload_len);
    }
}

/**
 * @brief 在缓冲里找提示符(如 ">")，命中则立刻发送原始数据
 * @return 1=已命中并消费(consumed 有效), 0=未命中
 */
static int s_prompt_try(const uint8_t *data, uint16_t avail, uint16_t *consumed)
{
    uint16_t plen;

    *consumed = 0U;

    if (s_prompt == NULL || s_payload_sent != 0U)
    {
        return 0;
    }

    plen = (uint16_t)strlen(s_prompt);
    if (plen == 0U || avail < plen)
    {
        return 0;
    }

    for (uint16_t i = 0U; (uint32_t)i + (uint32_t)plen <= (uint32_t)avail; i++)
    {
        if (memcmp(&data[i], s_prompt, plen) == 0)
        {
            s_send_payload();
            *consumed = (uint16_t)(i + plen);
            return 1;
        }
    }

    return 0;
}

/* ======================================================================== */
/*                          解析主循环                                        */
/* ======================================================================== */

/**
 * @brief 按 AT 语法解析接收缓冲，能消费多少消费多少
 */
static void s_rx_parse(emda_tick_t now)
{
    for (;;)
    {
        const uint8_t *data;
        uint16_t avail;
        uint16_t consumed = 0U;
        int ret;

        s_rx_prepare();
        avail = s_rx_len;
        if (avail == 0U)
        {
            return;
        }
        data = &s_rx[s_rx_head];

        /* ---- a) 两段式发送的提示符 ---- */
        if (s_prompt_try(data, avail, &consumed) == 1)
        {
            s_rx_consume(consumed);
            continue;
        }

        /* ---- b) URC：整段到齐才消费 ---- */
        ret = s_urc_try(data, avail, &consumed);
        if (ret == -1)
        {
            return; /* 半条 URC：等更多数据，绝不落到文本分支 */
        }
        if (ret == -2)
        {
            uint16_t drop = s_find_line(data, avail);
            s_rx_consume((drop > 0U) ? drop : avail);
            EMDA_LOG("[emda_at] drop oversized urc\r\n");
            continue;
        }
        if (ret == 1)
        {
            s_rx_consume(consumed);
            s_rx_skip_eol();
            continue;
        }

        /* ---- c) 响应文本行 ---- */
        consumed = s_find_line(data, avail);
        if (consumed == 0U)
        {
            return; /* 半行，等更多 */
        }

        if (s_state == EMDA_AT_STATE_SEND || s_state == EMDA_AT_STATE_WAITING)
        {
            s_resp_append(data, consumed);
            s_rx_consume(consumed);

            if (s_resp_cb != NULL && s_resp_cb(s_resp, s_resp_len) == EMDA_EOK)
            {
                s_resp_cb = NULL;
                s_state = EMDA_AT_STATE_RESP_OK;
                s_ok_tick = now;
            }
        }
        else
        {
            /* 没有在途事务：未识别的行直接丢弃 */
            s_rx_consume(consumed);
        }
    }
}

/* ======================================================================== */
/*                          状态访问                                          */
/* ======================================================================== */

emda_at_state_t emda_at_state_get(void)
{
    return s_state;
}

uint8_t emda_at_queue_count(void)
{
    uint8_t count;

    emda_port_enter_critical();
    count = s_q_count;
    emda_port_exit_critical();
    return count;
}

uint16_t emda_at_rx_count(void)
{
    return s_rx_len;
}

int emda_at_ready(void)
{
    int ready;

    emda_port_enter_critical();
    ready = (s_state == EMDA_AT_STATE_IDLE && s_q_count == 0U) ? 1 : 0;
    emda_port_exit_critical();

    /* 缓冲头部挂着半条 URC 时不能插新指令，否则会把它的后半段当响应 */
    if (ready && s_rx_len > 0U)
    {
        const emda_urc_entry_t *entry = NULL;
        uint16_t seg = 0U;

        s_rx_prepare();
        if (s_urc_probe(&s_rx[s_rx_head], s_rx_len, &entry, &seg) == -1)
        {
            ready = 0;
        }
    }

    return ready;
}

/* ======================================================================== */
/*                          指令队列                                          */
/* ======================================================================== */

static int s_queue_push(const char *cmd,
                        uint16_t cmd_len,
                        emda_at_resp_cb_t resp_cb,
                        uint32_t timeout_ms,
                        emda_at_timeout_cb_t timeout_cb)
{
    emda_at_msg_t *slot;

    if (cmd_len >= EMDA_CMD_BUFFER_SIZE)
    {
        cmd_len = EMDA_CMD_BUFFER_SIZE - 1U;
    }

    emda_port_enter_critical();
    if (s_q_count >= EMDA_CMD_QUEUE_SIZE)
    {
        emda_port_exit_critical();
        return EMDA_ENOMEM;
    }

    slot = &s_cmd_queue[s_q_tail];
    memcpy(slot->cmd, cmd, cmd_len);
    slot->cmd[cmd_len] = '\0';
    slot->cmd_len = cmd_len;
    slot->resp_cb = resp_cb;
    slot->timeout_cb = timeout_cb;
    slot->timeout_ms = timeout_ms;

    s_q_tail = (uint8_t)((s_q_tail + 1U) % EMDA_CMD_QUEUE_SIZE);
    s_q_count++;
    emda_port_exit_critical();
    return EMDA_EOK;
}

static int s_queue_pop(emda_at_msg_t *out)
{
    if (out == NULL)
    {
        return EMDA_EINVAL;
    }

    emda_port_enter_critical();
    if (s_q_count == 0U)
    {
        emda_port_exit_critical();
        return EMDA_ERROR;
    }

    *out = s_cmd_queue[s_q_head];
    s_q_head = (uint8_t)((s_q_head + 1U) % EMDA_CMD_QUEUE_SIZE);
    s_q_count--;
    emda_port_exit_critical();
    return EMDA_EOK;
}

int emda_at_submit(const char *cmd,
                   emda_at_resp_cb_t resp_cb,
                   uint32_t timeout_ms,
                   emda_at_timeout_cb_t timeout_cb)
{
    if (cmd == NULL)
    {
        return EMDA_EINVAL;
    }

    return s_queue_push(cmd, (uint16_t)strlen(cmd), resp_cb, timeout_ms, timeout_cb);
}

int emda_at_submit_raw(const char *cmd,
                       const char *prompt,
                       const uint8_t *payload,
                       uint16_t payload_len,
                       emda_at_resp_cb_t resp_cb,
                       uint32_t timeout_ms,
                       emda_at_timeout_cb_t timeout_cb)
{
    if (cmd == NULL || prompt == NULL || payload == NULL ||
        payload_len == 0U || payload_len > EMDA_TX_PAYLOAD_SIZE)
    {
        return EMDA_EINVAL;
    }

    /* 两段式发送不能排队(数据缓冲只有一份)，必须等事务空闲 */
    if (!emda_at_ready())
    {
        return EMDA_EBUSY;
    }

    memcpy(s_payload, payload, payload_len);
    s_payload_len = payload_len;
    s_payload_sent = 0U;
    s_prompt = prompt;

    s_load_command(emda_tick_get(), cmd, (uint16_t)strlen(cmd),
                   resp_cb, timeout_ms, timeout_cb);
    return EMDA_EOK;
}

/* ======================================================================== */
/*                          URC 表管理                                        */
/* ======================================================================== */

int emda_at_urc_attach(const emda_urc_t *table, uint32_t count)
{
    if (table == NULL || count == 0U)
    {
        return EMDA_EINVAL;
    }

    for (uint32_t i = 0U; i < count; i++)
    {
        if (table[i].prefix == NULL)
        {
            continue;
        }
        if (s_urc_count >= EMDA_URC_TABLE_SIZE)
        {
            return EMDA_ENOMEM;
        }

        s_urc[s_urc_count].prefix = table[i].prefix;
        s_urc[s_urc_count].prefix_len = (uint16_t)strlen(table[i].prefix);
        s_urc[s_urc_count].seg_len = table[i].seg_len;
        s_urc[s_urc_count].handler = table[i].handler;
        s_urc_count++;
    }

    return EMDA_EOK;
}

/* ======================================================================== */
/*                       指令加载与初始化                                    */
/* ======================================================================== */

static void s_load_command(emda_tick_t now,
                           const char *cmd,
                           uint16_t cmd_len,
                           emda_at_resp_cb_t resp_cb,
                           uint32_t timeout_ms,
                           emda_at_timeout_cb_t timeout_cb)
{
    if (cmd_len >= EMDA_CMD_BUFFER_SIZE)
    {
        cmd_len = EMDA_CMD_BUFFER_SIZE - 1U;
    }

    memcpy(s_cur_cmd, cmd, cmd_len);
    s_cur_cmd[cmd_len] = '\0';
    s_cur_cmd_len = cmd_len;

    s_resp_cb = resp_cb;
    s_timeout_cb = timeout_cb;
    s_cur_timeout_ms = (timeout_ms == 0U) ? EMDA_AT_DEFAULT_TIMEOUT_MS : timeout_ms;

    s_resp_reset();
    (void)emda_port_uart_send((const uint8_t *)cmd, cmd_len);

    s_send_tick = now;
    s_state = EMDA_AT_STATE_SEND;

    EMDA_LOG("[emda_at] tx: %s", cmd);
}

int emda_at_init(void)
{
    s_state = EMDA_AT_STATE_IDLE;
    s_resp_cb = NULL;
    s_timeout_cb = NULL;
    s_cur_cmd_len = 0U;
    s_send_tick = 0U;
    s_ok_tick = 0U;
    s_step_tick = 0U;

    s_prompt = NULL;
    s_payload_len = 0U;
    s_payload_sent = 0U;

    s_resp_reset();

    s_rx_head = 0U;
    s_rx_len = 0U;

    s_urc_count = 0U;

    return emda_port_uart_init();
}

/* ======================================================================== */
/*                       主轮询(非阻塞)                                      */
/* ======================================================================== */
/*
 * 每轮：收串口字节 -> 解析(URC/响应) -> AT状态机推进一步
 *
 * 节奏控制：
 *   - 收数据与解析每轮都执行，不受冷却/推进间隔限制
 *   - AT 状态机按 EMDA_LOOP_INTERVAL_MS 推进
 *   - RESP_OK 冷却只限制下一条发送，期间仍持续处理 RX/URC
 */
void emda_at_poll(void)
{
    emda_tick_t now = emda_tick_get();
    emda_tick_t interval;

    /* ============== [1] 接收 + 解析(始终执行) ============== */
    s_rx_pump();
    s_rx_parse(now);

    /* 解析完成后才对状态机推进做节奏限制 */
    interval = emda_tick_from_ms(EMDA_LOOP_INTERVAL_MS);
    if ((now - s_step_tick) < interval)
    {
        return;
    }
    s_step_tick = now;

    /* ============== [2] AT 事务状态机 ============== */
    switch (s_state)
    {
    case EMDA_AT_STATE_IDLE:
    {
        emda_at_msg_t msg;

        if (s_queue_pop(&msg) == EMDA_EOK)
        {
            /* 普通指令不带提示符/数据 */
            s_prompt = NULL;
            s_payload_len = 0U;
            s_payload_sent = 0U;

            s_load_command(now, msg.cmd, msg.cmd_len,
                           msg.resp_cb, msg.timeout_ms, msg.timeout_cb);
        }
        /* 队列为空：由设备层(emda_device_poll)决定是否提交监控指令 */
        break;
    }

    case EMDA_AT_STATE_SEND:
    {
        /* 发送在 s_load_command 中已完成，直接转等待 */
        s_state = EMDA_AT_STATE_WAITING;
        break;
    }

    case EMDA_AT_STATE_WAITING:
    {
        /* 响应回调由解析路径触发；这里只管总超时 */
        if ((now - s_send_tick) >= emda_tick_from_ms(s_cur_timeout_ms))
        {
            EMDA_LOG("[emda_at] timeout: %s", s_cur_cmd);
            s_state = EMDA_AT_STATE_TIMEOUT;
        }
        break;
    }

    case EMDA_AT_STATE_RESP_OK:
    {
        if ((now - s_ok_tick) < emda_tick_from_ms(EMDA_RESP_COOLDOWN_MS))
        {
            break;
        }

        s_resp_reset();
        s_resp_cb = NULL;
        s_timeout_cb = NULL;
        s_prompt = NULL;
        s_payload_len = 0U;
        s_payload_sent = 0U;
        s_state = EMDA_AT_STATE_IDLE;
        break;
    }

    case EMDA_AT_STATE_TIMEOUT:
    {
        if (s_timeout_cb != NULL)
        {
            (void)s_timeout_cb(s_cur_cmd);
        }
        s_resp_reset();
        s_resp_cb = NULL;
        s_timeout_cb = NULL;
        s_prompt = NULL;
        s_payload_len = 0U;
        s_payload_sent = 0U;
        s_state = EMDA_AT_STATE_IDLE;
        break;
    }

    default:
        s_state = EMDA_AT_STATE_IDLE;
        break;
    }
}
