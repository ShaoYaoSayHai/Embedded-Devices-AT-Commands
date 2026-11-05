/**
 * @file emda_port.h
 * @author Zorian(1551769443@qq.com)
 * @brief  EMDA 移植层(硬件/RTOS 抽象)接口
 *
 * port 层只有 4 类接口，无任务管理：
 *   1. 心跳时间：emda_heartbeat / emda_tick_get / emda_tick_from_ms
 *   2. 串口收发：emda_port_uart_init / send / read_frame
 *   3. 临界区  ：emda_port_enter_critical / exit_critical
 *   4. 互斥锁  ：emda_port_mutex_create / destroy / lock / unlock
 *
 * 上面三层(emda_at / emda_device / emda_socket)只调用本文件声明的接口，
 * 不直接依赖任何 RTOS 或硬件。移植时只需实现 emda_port.c。
 *
 * @version 0.3
 * @date 2026-11-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef EMDA_PORT_H
#define EMDA_PORT_H

#include "emda_config.h"
#include "emda_def.h"

/* 互斥锁句柄(port 内部定义为具体RTOS类型) */
typedef void *emda_mutex_t;

/* ======================================================================== */
/*                    1. 心跳与时间接口                                      */
/* ======================================================================== */

/**
 * @brief 心跳函数，提供全库的时间基准
 *
 * 内部维护 volatile uint32_t 计数器，每次调用自增1并返回新值。
 * 移植者必须保证调用频率恒定，且周期 = EMDA_HEARTBEAT_PERIOD_MS。
 *
 * 典型用法(任务模式)：
 *   while (1) {
 *       emda_poll();                         // 推进各层(内部读当前tick)
 *       emda_heartbeat();                    // 心跳自增
 *       vTaskDelay(EMDA_HEARTBEAT_PERIOD_MS);
 *   }
 *
 * 典型用法(裸机中断模式)：
 *   void SysTick_Handler(void) { emda_heartbeat(); }
 *
 * @return 本次心跳后的计数器值
 */
emda_tick_t emda_heartbeat(void);

/**
 * @brief 读取当前tick(不自增)
 */
emda_tick_t emda_tick_get(void);

/**
 * @brief 毫秒转tick数(按 EMDA_HEARTBEAT_PERIOD_MS 换算)
 */
emda_tick_t emda_tick_from_ms(uint32_t ms);

/* ======================================================================== */
/*                          2. 串口收发接口                                  */
/* ======================================================================== */

/**
 * @brief 串口初始化(UART/DMA配置)，由 emda_at_init() 调用一次
 * @return EMDA_EOK / EMDA_EIO
 */
int emda_port_uart_init(void);

/**
 * @brief 串口发送数据(非阻塞或短阻塞发送)
 * @param data 待发送数据
 * @param len  数据长度
 * @return EMDA_EOK / EMDA_EIO
 */
int emda_port_uart_send(const uint8_t *data, uint16_t len);

/**
 * @brief 串口读取一帧数据(带超时)
 *         底层通常对接 DMA+空闲中断 或 环形缓冲区
 * @param out_buf        输出缓冲区
 * @param buf_size       缓冲区容量
 * @param out_frame_len  实际读到的帧长度
 * @param timeout_ms     超时时间(ms)，0=非阻塞立即返回
 * @return EMDA_EOK=读到数据, EMDA_ETIMEOUT=无数据, EMDA_EIO=错误
 */
int emda_port_uart_read_frame(uint8_t *out_buf,
                              uint16_t buf_size,
                              uint16_t *out_frame_len,
                              uint32_t timeout_ms);

/* ======================================================================== */
/*                          3. 临界区接口                                    */
/* ======================================================================== */

/**
 * @brief 进入临界区
 *         按 emda_config.h 配置：关中断实现 或 空实现
 * @note 不可嵌套，必须成对使用
 */
void emda_port_enter_critical(void);

/**
 * @brief 退出临界区
 */
void emda_port_exit_critical(void);

/* ======================================================================== */
/*                          4. 互斥锁接口                                    */
/* ======================================================================== */

/**
 * @brief 创建互斥锁
 * @return 互斥锁句柄，NULL=创建失败
 */
emda_mutex_t emda_port_mutex_create(void);

/**
 * @brief 销毁互斥锁
 */
void emda_port_mutex_destroy(emda_mutex_t mutex);

/**
 * @brief 获取互斥锁(阻塞)
 * @param mutex      互斥锁句柄
 * @param timeout_ms 超时(ms)，EMDA_WAIT_FOREVER=永久等待
 * @return EMDA_EOK=获取成功, EMDA_ETIMEOUT=超时
 */
int emda_port_mutex_lock(emda_mutex_t mutex, uint32_t timeout_ms);

/**
 * @brief 释放互斥锁
 * @return EMDA_EOK / EMDA_ERROR
 */
int emda_port_mutex_unlock(emda_mutex_t mutex);

#endif /* EMDA_PORT_H */
