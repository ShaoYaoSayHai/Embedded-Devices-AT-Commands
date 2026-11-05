/**
 * @file emda_at.h
 * @author Zorian(1551769443@qq.com)
 * @brief  EMDA AT 事务核心(对应 RT-Thread AT 包里的 at_client)
 *
 * 只做一件事：把「一条 AT 指令 + 它的响应」可靠地跑完，并把模组主动上报
 * (URC) 分发给注册者。不关心业务语义，不认识任何具体模组。
 *
 * 关键机制：
 *   1. 事务状态机 IDLE -> SEND -> WAITING -> RESP_OK / TIMEOUT
 *      任一时刻只有一条指令在途，响应经回调解析，成功即结束事务
 *   2. 指令队列：业务临时指令入队，IDLE 时按 FIFO 取出执行
 *   3. 接收窗口聚合：DMA 分帧的零散数据在时间窗内拼成完整响应
 *      文本帧(以CRLF结尾)立即分发，其余以 EMDA_RX_WINDOW_MS 静默兜底
 *   4. URC 全局回调表：每帧数据先分发给所有 URC 回调，再喂给在途事务
 *   5. 两段式发送：指令 -> 等待提示符(如">") -> 发送原始数据
 *
 * 依赖关系：只依赖 emda_config.h / emda_def.h / emda_port.h
 *
 * @version 0.3
 * @date 2026-11-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef EMDA_AT_H
#define EMDA_AT_H

#include "emda_config.h"
#include "emda_def.h"
#include "emda_port.h"

/* ======================================================================== */
/*                          回调函数类型                                     */
/* ======================================================================== */

/**
 * @brief AT指令响应回调
 *
 * 每次有新数据到达都会被调用，buffer 是本次事务至今的累积响应(以'\0'结尾)。
 * 因此可以跨多帧分片解析，只要任一次返回 EMDA_EOK 即视为事务成功。
 *
 * @param buffer 累积响应数据
 * @param len    累积数据长度
 * @return EMDA_EOK=解析成功(结束事务), 其它=继续等待
 */
typedef int (*emda_at_resp_cb_t)(uint8_t *buffer, uint16_t len);

/**
 * @brief AT指令超时回调
 * @param cmd 超时的指令字符串(便于日志)
 * @return EMDA_EOK=已处理
 */
typedef int (*emda_at_timeout_cb_t)(char *cmd);

/**
 * @brief URC 回调
 * @param buffer 本次收到的数据分片(不是累积缓冲)
 * @param len    数据长度
 * @return EMDA_EOK=已处理, 其它=未处理
 */
typedef int (*emda_urc_cb_t)(const uint8_t *buffer, uint16_t len);

/* ======================================================================== */
/*                          AT 事务状态机                                    */
/* ======================================================================== */

typedef enum
{
    EMDA_AT_STATE_IDLE = 0U, /* 空闲，等待下一条指令(来自队列或设备层) */
    EMDA_AT_STATE_SEND,      /* 正在通过UART发送AT指令 */
    EMDA_AT_STATE_WAITING,   /* 已发送，等待模组响应，期间回调解析 */
    EMDA_AT_STATE_RESP_OK,   /* 回调返回成功，响应已处理 */
    EMDA_AT_STATE_TIMEOUT,   /* 超时未收到有效响应 */
} emda_at_state_t;

/* ======================================================================== */
/*                              对外API                                      */
/* ======================================================================== */

/**
 * @brief AT 核心初始化：复位状态机 + 调用 port 层 UART 初始化
 * @return EMDA_EOK / EMDA_EIO
 */
int emda_at_init(void);

/**
 * @brief 推进 AT 核心(非阻塞)
 *
 * 每轮执行：收串口帧 -> URC分发 -> AT状态机推进一步 -> 空闲时从队列取指令
 * 需高频调用；RX/URC 不受 EMDA_LOOP_INTERVAL_MS 限制。
 */
void emda_at_poll(void);

/**
 * @brief 提交一条 AT 指令(入队，按 FIFO 串行执行)
 *
 * 任意线程/中断上下文可调用；同一时刻只有一条指令在途。
 *
 * @param cmd        AT指令字符串(含\r\n)
 * @param resp_cb    响应回调(可为NULL，表示不解析响应)
 * @param timeout_ms 超时(ms)，0=使用 EMDA_AT_DEFAULT_TIMEOUT_MS
 * @param timeout_cb 超时回调(可为NULL)
 * @return EMDA_EOK=入队成功, EMDA_ENOMEM=队列满, EMDA_EINVAL=参数错误
 */
int emda_at_submit(const char *cmd,
                   emda_at_resp_cb_t resp_cb,
                   uint32_t timeout_ms,
                   emda_at_timeout_cb_t timeout_cb);

/**
 * @brief 提交一条「两段式」AT 指令(指令 -> 提示符 -> 原始数据)
 *
 * 典型场景：AT+MIPSEND=<id>,<len> 之后模组回 ">"，此时才能送数据。
 * 本函数会先发 cmd，收到 prompt 后立刻发送 payload，再继续等 RESP_OK。
 *
 * @note 要求提交时 AT 事务空闲(emda_at_ready()==1)，否则返回 EMDA_EBUSY；
 *       payload 会拷贝进内部缓冲，调用后即可释放。
 *       单次原始数据上限 = EMDA_TX_PAYLOAD_SIZE。
 */
int emda_at_submit_raw(const char *cmd,
                       const char *prompt,
                       const uint8_t *payload,
                       uint16_t payload_len,
                       emda_at_resp_cb_t resp_cb,
                       uint32_t timeout_ms,
                       emda_at_timeout_cb_t timeout_cb);

/* ---- URC ---- */

/**
 * @brief 注册 URC 回调
 * @param name     注册名称(便于调试)
 * @param callback 回调函数
 * @return EMDA_EOK=成功, EMDA_ENOMEM=表满
 */
int emda_at_urc_attach(const char *name, emda_urc_cb_t callback);

/**
 * @brief URC 全局分发(引擎内部调用，通常无需直接调用)
 *        同一帧会通知所有已注册回调，不会因首个命中而停止。
 */
int emda_at_urc_dispatch(const uint8_t *buffer, uint16_t len);

/* ---- 状态查询 ---- */

/**
 * @brief 事务层是否完全空闲(无在途指令、队列为空、无半帧数据)
 *        设备层/网络层据此决定是否发起新事务
 * @return 1=空闲, 0=忙
 */
int emda_at_ready(void);

/**
 * @brief 获取当前 AT 状态机状态(调试用)
 */
emda_at_state_t emda_at_state_get(void);

/**
 * @brief 获取待处理指令队列长度(调试用)
 */
uint8_t emda_at_queue_count(void);

#endif /* EMDA_AT_H */
