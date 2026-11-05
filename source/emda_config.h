/**
 * @file emda_config.h
 * @author Zorian(1551769443@qq.com)
 * @brief  EMDA (Embedded Module Device AT) 配置中心
 *         心跳基准、临界区策略、缓冲区、监控/队列容量、socket 资源
 *
 * 所有宏均用 #ifndef 保护，可在编译选项中覆盖，无需改动源码。
 *
 * 运行模式：
 *   EMDA 统一采用「非阻塞轮询」，不创建任务。移植者只需反复调用：
 *     - emda_poll()       驱动全部层(AT事务 + socket + 设备监控)
 *     - emda_heartbeat()  周期调用(周期 = EMDA_HEARTBEAT_PERIOD_MS)
 *
 *   RTOS：在任务 while(1) 里 poll -> heartbeat -> delay
 *   裸机：heartbeat 放定时器中断，poll 放 main 循环
 *
 * @version 0.3
 * @date 2026-11-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef EMDA_CONFIG_H
#define EMDA_CONFIG_H

#include "emda_def.h"

/* ======================================================================== */
/*                        心跳时间基准配置                                    */
/* ======================================================================== */
/*
 * emda_heartbeat() 内部维护 volatile 计数器，每次调用自增1。
 * 移植者必须保证调用频率恒定，且周期 = EMDA_HEARTBEAT_PERIOD_MS。
 * 引擎所有时间判断(超时/间隔/冷却)都以该计数器为基准。
 */

#ifndef EMDA_HEARTBEAT_PERIOD_MS
#define EMDA_HEARTBEAT_PERIOD_MS (1U) /* 心跳周期(ms)，默认1ms一次 */
#endif

/* ======================================================================== */
/*                        临界区策略配置                                     */
/* ======================================================================== */
/*
 *   EMDA_CRITICAL_USE_DISABLE_IRQ - 关中断实现(默认，安全)
 *     port 层用 __disable_irq()/__enable_irq() 或 taskENTER_CRITICAL()
 *   EMDA_CRITICAL_USE_NONE        - 空实现(确认无中断竞争时用)
 */

#ifndef EMDA_CRITICAL_USE_DISABLE_IRQ
#ifndef EMDA_CRITICAL_USE_NONE
#define EMDA_CRITICAL_USE_DISABLE_IRQ /* 默认启用关中断临界区 */
#endif
#endif

#if defined(EMDA_CRITICAL_USE_DISABLE_IRQ) && defined(EMDA_CRITICAL_USE_NONE)
#error "emda_config: EMDA_CRITICAL_USE_DISABLE_IRQ 与 EMDA_CRITICAL_USE_NONE 不能同时启用"
#endif

/* ======================================================================== */
/*                        缓冲区大小配置                                     */
/* ======================================================================== */

/* AT指令发送缓冲区(单条指令最大长度，含\r\n和'\0') */
#ifndef EMDA_CMD_BUFFER_SIZE
#define EMDA_CMD_BUFFER_SIZE (256U)
#endif

/* AT响应接收窗口缓冲区(一次接收窗口聚合的最大数据量) */
#ifndef EMDA_RX_WINDOW_BUFFER_SIZE
#define EMDA_RX_WINDOW_BUFFER_SIZE (1024U)
#endif

/*
 * 「指令 -> 提示符 -> 原始数据」两段式发送的数据缓冲。
 * 用于 AT+MIPSEND 之类的 ">" 提示符场景；单次原始数据上限。
 */
#ifndef EMDA_TX_PAYLOAD_SIZE
#define EMDA_TX_PAYLOAD_SIZE (512U)
#endif

/* ======================================================================== */
/*                        容量配置                                           */
/* ======================================================================== */

/* 监控项最大条目数(每个状态下的轮询指令) */
#ifndef EMDA_MONITOR_MAX
#define EMDA_MONITOR_MAX (16U)
#endif

/* 动态指令队列最大条目数(业务临时指令) */
#ifndef EMDA_CMD_QUEUE_SIZE
#define EMDA_CMD_QUEUE_SIZE (8U)
#endif

/* URC回调表最大条目数 */
#ifndef EMDA_URC_TABLE_SIZE
#define EMDA_URC_TABLE_SIZE (8U)
#endif

/* 名称最大长度(监控项名/URC注册名，含'\0') */
#ifndef EMDA_NAME_LEN
#define EMDA_NAME_LEN (24U)
#endif

/* socket 最大个数(受模组 connect_id 数量限制，ML307A 为 6 路) */
#ifndef EMDA_SOCK_MAX
#define EMDA_SOCK_MAX (4U)
#endif

/* 单个 socket 接收缓冲(URC 上报数据先入环形缓冲，再由 recv 取走) */
#ifndef EMDA_SOCK_RX_BUFFER_SIZE
#define EMDA_SOCK_RX_BUFFER_SIZE (512U)
#endif

/* 单个 socket 待发送缓冲(发送期间数据暂存，发送完成后释放) */
#ifndef EMDA_SOCK_TX_BUFFER_SIZE
#define EMDA_SOCK_TX_BUFFER_SIZE (256U)
#endif

/* ======================================================================== */
/*                        节奏与超时配置                                     */
/* ======================================================================== */
/*
 * 以下参数都是「时间戳判断」，不是延时。移植者仍需高频调用 emda_poll()，
 * 引擎内部按这些参数控制实际节奏。
 *
 *   EMDA_RX_WINDOW_MS     - 无明确文本边界(CRLF)时的帧间静默兜底时间
 *   EMDA_RESP_COOLDOWN_MS - RESP_OK 后限制下一条发送的冷却；不暂停 RX/URC
 *   EMDA_LOOP_INTERVAL_MS - AT状态机最小推进间隔(0=每次都跑)
 */

#ifndef EMDA_RX_WINDOW_MS
#define EMDA_RX_WINDOW_MS (20U)
#endif

#ifndef EMDA_RESP_COOLDOWN_MS
#define EMDA_RESP_COOLDOWN_MS (50U)
#endif

#ifndef EMDA_LOOP_INTERVAL_MS
#define EMDA_LOOP_INTERVAL_MS (5U)
#endif

/* 单条AT指令的默认超时(提交时传0则使用此值) */
#ifndef EMDA_AT_DEFAULT_TIMEOUT_MS
#define EMDA_AT_DEFAULT_TIMEOUT_MS (2000U)
#endif

/* 串口读帧默认超时(ms) */
#ifndef EMDA_UART_READ_TIMEOUT_MS
#define EMDA_UART_READ_TIMEOUT_MS (10U)
#endif

/* 互斥锁等待超时(ms) */
#ifndef EMDA_MUTEX_WAIT_TIMEOUT_MS
#define EMDA_MUTEX_WAIT_TIMEOUT_MS (1000U)
#endif

#endif /* EMDA_CONFIG_H */
