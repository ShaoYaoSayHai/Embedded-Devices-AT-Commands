/**
 * @file emda_at.c
 * @author Zorian(1551769443@qq.com)
 * @brief  EMDA AT 事务核心实现
 *
 * 架构(非阻塞轮询)：
 *   1. 接收窗口：把串口分片聚合成一帧(CRLF 立即交付，其余静默兜底)
 *   2. URC 分发：每帧先给全部 URC 回调，再喂给在途事务
 *   3. 事务状态机：IDLE -> SEND -> WAITING -> RESP_OK / TIMEOUT
 *   4. 指令队列：业务/设备层提交的指令按 FIFO 串行执行
 *   5. 两段式发送：指令 -> 提示符 -> 原始数据
 *
 * 依赖关系：本文件只调用 emda_port.h 声明的接口。
 *
 * @version 0.3
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

/* URC 回调表条目 */
typedef struct
{
    char name[EMDA_NAME_LEN];
    emda_urc_cb_t cb;
    uint8_t is_used;
} emda_urc_entry_t;

/* 指令队列条目 */
typedef struct
{
    char cmd[EMDA_CMD_BUFFER_SIZE];
    uint16_t cmd_len;
    emda_at_resp_cb_t resp_cb;
    emda_at_timeout_cb_t timeout_cb;
    uint32_t timeout_ms;
} emda_at_msg_t;

/* UART 接收采集缓冲区：仅由接收窗口状态机写入 */
static uint8_t s_rx_collect_buffer[EMDA_RX_WINDOW_BUFFER_SIZE + 1U];

/* 当前在途事务的响应累积缓冲区：与下一轮 UART 采集完全分离 */
static uint8_t s_resp_buffer[EMDA_RX_WINDOW_BUFFER_SIZE + 1U];
static uint16_t s_resp_len = 0U;

/* 指令队列(环形缓冲) */
static emda_at_msg_t s_cmd_queue[EMDA_CMD_QUEUE_SIZE];
static volatile uint8_t s_q_head = 0U;
static volatile uint8_t s_q_tail = 0U;
static volatile uint8_t s_q_count = 0U;

/* URC 回调表 */
static emda_urc_entry_t s_urc_table[EMDA_URC_TABLE_SIZE];

/* 事务状态机运行时字段 */
static emda_at_state_t s_state = EMDA_AT_STATE_IDLE;
static emda_at_resp_cb_t s_resp_cb = NULL;
static emda_at_timeout_cb_t s_timeout_cb = NULL;
static char s_cur_cmd[EMDA_CMD_BUFFER_SIZE];
static uint16_t s_cur_cmd_len = 0U;
static uint32_t s_cur_timeout_ms = 0U;
static emda_tick_t s_send_tick = 0U;
static emda_tick_t s_ok_tick = 0U;

/* 状态机推进节奏；UART 接收与 URC 分发不受此限制 */
static emda_tick_t s_step_tick = 0U;

/* 两段式发送：提示符 + 原始数据 */
static const char *s_prompt = NULL;
static uint8_t s_payload[EMDA_TX_PAYLOAD_SIZE];
static uint16_t s_payload_len = 0U;
static uint8_t s_payload_sent = 0U;

/* 接收窗口状态 */
typedef enum
{
    RX_WIN_IDLE = 0U,
    RX_WIN_COLLECTING,
} rx_win_state_t;

static rx_win_state_t s_rx_win_state = RX_WIN_IDLE;
static emda_tick_t s_rx_win_last_tick = 0U;
static uint16_t s_rx_win_len = 0U;

/* 前置声明：加载指令(定义在后面，供 submit_raw 直接启动事务) */
static void s_load_command(emda_tick_t now,
                           const char *cmd,
                           uint16_t cmd_len,
                           emda_at_resp_cb_t resp_cb,
                           uint32_t timeout_ms,
                           emda_at_timeout_cb_t timeout_cb);

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

int emda_at_ready(void)
{
    int ready;

    emda_port_enter_critical();
    ready = (s_state == EMDA_AT_STATE_IDLE && s_q_count == 0U &&
             s_rx_win_state == RX_WIN_IDLE) ? 1 : 0;
    emda_port_exit_critical();
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

    /* 两段式发送不能被排队(数据缓冲只有一份)，必须等事务空闲 */
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
/*                          URC 回调表                                        */
/* ======================================================================== */

int emda_at_urc_attach(const char *name, emda_urc_cb_t callback)
{
    if (name == NULL || callback == NULL)
    {
        return EMDA_EINVAL;
    }

    for (uint32_t i = 0U; i < EMDA_URC_TABLE_SIZE; i++)
    {
        if (!s_urc_table[i].is_used)
        {
            s_urc_table[i].is_used = 1U;
            strncpy(s_urc_table[i].name, name, EMDA_NAME_LEN - 1U);
            s_urc_table[i].name[EMDA_NAME_LEN - 1U] = '\0';
            s_urc_table[i].cb = callback;
            return EMDA_EOK;
        }
    }
    return EMDA_ENOMEM;
}

int emda_at_urc_dispatch(const uint8_t *buffer, uint16_t len)
{
    int handled = EMDA_ERROR;

    if (buffer == NULL || len == 0U)
    {
        return EMDA_EINVAL;
    }

    for (uint32_t i = 0U; i < EMDA_URC_TABLE_SIZE; i++)
    {
        if (s_urc_table[i].is_used)
        {
            if (s_urc_table[i].cb(buffer, len) == EMDA_EOK)
            {
                /* 同一聚合窗口可能包含多个URC，命中后继续通知后续回调 */
                handled = EMDA_EOK;
            }
        }
    }
    return handled;
}

/* ======================================================================== */
/*                       接收窗口聚合                                        */
/* ======================================================================== */

static void s_resp_reset(void)
{
    s_resp_buffer[0] = '\0';
    s_resp_len = 0U;
}

static void s_rx_window_reset(void)
{
    s_rx_collect_buffer[0] = '\0';
    s_rx_win_state = RX_WIN_IDLE;
    s_rx_win_last_tick = 0U;
    s_rx_win_len = 0U;
}

static int s_rx_window_has_text_boundary(void)
{
    return (s_rx_win_len >= 2U &&
            s_rx_collect_buffer[s_rx_win_len - 2U] == '\r' &&
            s_rx_collect_buffer[s_rx_win_len - 1U] == '\n') ? 1 : 0;
}

static int s_rx_window_publish(uint16_t *out_frame_len)
{
    if (out_frame_len == NULL || s_rx_win_len == 0U)
    {
        return EMDA_ERROR;
    }

    *out_frame_len = s_rx_win_len;
    s_rx_collect_buffer[s_rx_win_len] = '\0';
    s_rx_win_state = RX_WIN_IDLE;
    s_rx_win_len = 0U;
    return EMDA_EOK;
}

/**
 * @brief 非阻塞接收采集(每次调用最多读取一帧)
 *
 * 1. 非阻塞读取一个 port 帧并追加到采集缓冲
 * 2. 数据以 CRLF 结尾或缓冲已满 -> 立即交付
 * 3. 其它数据在 EMDA_RX_WINDOW_MS 内无新分片后交付
 */
static int s_rx_window_step(emda_tick_t now, uint16_t *out_frame_len)
{
    uint16_t remaining;
    uint16_t frame_len = 0U;

    if (out_frame_len == NULL)
    {
        return EMDA_EINVAL;
    }
    *out_frame_len = 0U;

    if (s_rx_win_len >= EMDA_RX_WINDOW_BUFFER_SIZE)
    {
        return s_rx_window_publish(out_frame_len);
    }

    remaining = (uint16_t)(EMDA_RX_WINDOW_BUFFER_SIZE - s_rx_win_len);
    if (remaining > 0U &&
        emda_port_uart_read_frame(&s_rx_collect_buffer[s_rx_win_len],
                                  remaining,
                                  &frame_len,
                                  0U) == EMDA_EOK &&
        frame_len > 0U)
    {
        if (frame_len > remaining)
        {
            frame_len = remaining;
        }

        s_rx_win_len = (uint16_t)(s_rx_win_len + frame_len);
        s_rx_collect_buffer[s_rx_win_len] = '\0';
        s_rx_win_last_tick = now;
        s_rx_win_state = RX_WIN_COLLECTING;

        if (s_rx_win_len >= EMDA_RX_WINDOW_BUFFER_SIZE ||
            s_rx_window_has_text_boundary())
        {
            return s_rx_window_publish(out_frame_len);
        }
        return EMDA_ERROR;
    }

    if (s_rx_win_state == RX_WIN_COLLECTING)
    {
        if ((now - s_rx_win_last_tick) >= emda_tick_from_ms(EMDA_RX_WINDOW_MS))
        {
            return s_rx_window_publish(out_frame_len);
        }
    }

    return EMDA_ERROR;
}

/* ======================================================================== */
/*                       响应累积与分发                                      */
/* ======================================================================== */

/**
 * @brief 把新数据追加到当前事务的响应缓冲。
 *        溢出时丢弃最旧数据，始终保留最近 EMDA_RX_WINDOW_BUFFER_SIZE 字节。
 */
static void s_resp_append(const uint8_t *buffer, uint16_t len)
{
    if (buffer == NULL || len == 0U)
    {
        return;
    }

    if (len >= EMDA_RX_WINDOW_BUFFER_SIZE)
    {
        memcpy(s_resp_buffer, &buffer[len - EMDA_RX_WINDOW_BUFFER_SIZE],
               EMDA_RX_WINDOW_BUFFER_SIZE);
        s_resp_len = EMDA_RX_WINDOW_BUFFER_SIZE;
    }
    else
    {
        uint32_t combined = (uint32_t)s_resp_len + (uint32_t)len;
        if (combined > EMDA_RX_WINDOW_BUFFER_SIZE)
        {
            uint16_t discard = (uint16_t)(combined - EMDA_RX_WINDOW_BUFFER_SIZE);
            memmove(s_resp_buffer, &s_resp_buffer[discard],
                    (uint16_t)(s_resp_len - discard));
            s_resp_len = (uint16_t)(s_resp_len - discard);
        }
        memcpy(&s_resp_buffer[s_resp_len], buffer, len);
        s_resp_len = (uint16_t)(s_resp_len + len);
    }

    s_resp_buffer[s_resp_len] = '\0';
}

/**
 * @brief 检查累积响应中是否出现提示符，出现且未发过数据则发送原始数据
 */
static void s_try_send_payload(void)
{
    if (s_prompt == NULL || s_payload_sent != 0U || s_payload_len == 0U)
    {
        return;
    }

    if (strstr((const char *)s_resp_buffer, s_prompt) == NULL)
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
 * @brief 分发一段新接收数据：先通知全部URC，再喂给当前在途事务
 *        AT回调只在有新数据到达时调用，不重复解析旧缓冲。
 */
static void s_dispatch_frame(emda_tick_t now, const uint8_t *buffer, uint16_t len)
{
    if (buffer == NULL || len == 0U)
    {
        return;
    }

    /* URC既可处理模组主动上报，也可作为主动指令响应的兜底处理路径 */
    (void)emda_at_urc_dispatch(buffer, len);

    if ((s_state == EMDA_AT_STATE_SEND || s_state == EMDA_AT_STATE_WAITING) &&
        s_resp_cb != NULL)
    {
        s_resp_append(buffer, len);
        s_try_send_payload();

        if (s_resp_cb(s_resp_buffer, s_resp_len) == EMDA_EOK)
        {
            s_resp_cb = NULL;
            s_state = EMDA_AT_STATE_RESP_OK;
            s_ok_tick = now;
        }
    }
}

/* ======================================================================== */
/*                       指令加载与初始化                                    */
/* ======================================================================== */

/**
 * @brief 把一条指令加载到事务入口，进入 SEND 状态
 * @note 调用前 s_prompt / s_payload / s_payload_sent 已就绪(或为普通指令)
 */
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
    s_rx_window_reset();

    return emda_port_uart_init();
}

/* ======================================================================== */
/*                       主轮询(非阻塞)                                      */
/* ======================================================================== */
/*
 * 每轮：收串口帧 -> URC/响应分发 -> AT状态机推进一步
 *
 * 节奏控制：
 *   - UART 接收和 URC 分发每轮都执行，不受冷却/推进间隔限制
 *   - AT 状态机按 EMDA_LOOP_INTERVAL_MS 推进
 *   - RESP_OK 冷却只限制下一条发送，期间仍持续处理 RX/URC
 */
void emda_at_poll(void)
{
    emda_tick_t now = emda_tick_get();
    emda_tick_t interval;
    uint16_t frame_len = 0U;

    /* ============== [1] 串口接收 + URC/响应分发(始终执行) ============== */
    if (s_rx_window_step(now, &frame_len) == EMDA_EOK && frame_len > 0U)
    {
        s_dispatch_frame(now, s_rx_collect_buffer, frame_len);
    }

    /* RX/URC 处理完成后，才对状态机推进做节奏限制 */
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

        /* 已收到部分数据时先等它分发完，避免跨事务混入下一条响应 */
        if (s_rx_win_state == RX_WIN_COLLECTING)
        {
            break;
        }

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
        /* 响应回调由分发路径在新数据到达时触发；这里只管总超时 */
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
