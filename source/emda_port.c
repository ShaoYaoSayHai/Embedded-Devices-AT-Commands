/**
 * @file emda_port.c
 * @author Zorian(1551769443@qq.com)
 * @brief  EMDA 移植层实现(参考模板)
 *
 * 心跳/时间接口已实现，其余为桩实现。移植时按目标平台填充：
 *   串口 3 个 + 临界区 2 个 + 互斥锁 4 个
 *
 * @version 0.3
 * @date 2026-11-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "emda_port.h"

/* ======================================================================== */
/*                    1. 心跳与时间(已实现)                                  */
/* ======================================================================== */

static volatile emda_tick_t s_heartbeat_tick = 0U;

emda_tick_t emda_heartbeat(void)
{
    s_heartbeat_tick++;
    return s_heartbeat_tick;
}

emda_tick_t emda_tick_get(void)
{
    return s_heartbeat_tick;
}

emda_tick_t emda_tick_from_ms(uint32_t ms)
{
    return (emda_tick_t)(ms / EMDA_HEARTBEAT_PERIOD_MS);
}

/* ======================================================================== */
/*                2. 串口收发(桩实现，移植时填充)                            */
/* ======================================================================== */
/*
 * 移植参考(STM32 HAL + DMA 空闲中断):
 *
 *   int emda_port_uart_init(void) {
 *       HAL_UARTEx_ReceiveToIdle_DMA(&huart1, rx_dma_buf, sizeof(rx_dma_buf));
 *       return EMDA_EOK;
 *   }
 *
 *   int emda_port_uart_send(const uint8_t *data, uint16_t len) {
 *       return (HAL_UART_Transmit(&huart1, (uint8_t *)data, len, 100) == HAL_OK)
 *              ? EMDA_EOK : EMDA_EIO;
 *   }
 *
 *   int emda_port_uart_read_frame(uint8_t *out_buf, uint16_t buf_size,
 *                                 uint16_t *out_frame_len, uint32_t timeout_ms) {
 *       // 从空闲中断/RingBuffer 取一帧，拷贝到 out_buf
 *       return EMDA_EOK;
 *   }
 *
 * 移植参考(RT-Thread):
 *   rt_device_read(dev, -1, out_buf, buf_size) / rt_device_write(...)
 */

int emda_port_uart_init(void)
{
    /* TODO: 移植时实现 UART + DMA 初始化 */
    return EMDA_EOK;
}

int emda_port_uart_send(const uint8_t *data, uint16_t len)
{
    (void)data;
    (void)len;
    /* TODO: 移植时实现串口发送 */
    return EMDA_EOK;
}

int emda_port_uart_read_frame(uint8_t *out_buf,
                              uint16_t buf_size,
                              uint16_t *out_frame_len,
                              uint32_t timeout_ms)
{
    (void)out_buf;
    (void)buf_size;
    (void)timeout_ms;
    if (out_frame_len != NULL)
    {
        *out_frame_len = 0U;
    }
    /* TODO: 移植时实现串口帧读取 */
    return EMDA_ETIMEOUT;
}

/* ======================================================================== */
/*                3. 临界区(按配置自动选择实现)                              */
/* ======================================================================== */

#if defined(EMDA_CRITICAL_USE_DISABLE_IRQ)

/* ---- 关中断实现(默认，安全) ---- */
/*
 * 移植参考(STM32)  : __disable_irq() / __enable_irq()
 * 移植参考(FreeRTOS): taskENTER_CRITICAL() / taskEXIT_CRITICAL()
 * 移植参考(RT-Thr)  : rt_hw_interrupt_disable() / rt_hw_interrupt_enable()
 */
void emda_port_enter_critical(void)
{
    /* TODO: 移植时实现关中断 */
}

void emda_port_exit_critical(void)
{
    /* TODO: 移植时实现开中断 */
}

#elif defined(EMDA_CRITICAL_USE_NONE)

/* ---- 空实现(确认无中断竞争时使用) ---- */
void emda_port_enter_critical(void) { }
void emda_port_exit_critical(void)  { }

#endif

/* ======================================================================== */
/*                4. 互斥锁(桩实现，移植时填充)                              */
/* ======================================================================== */
/*
 * 移植参考(FreeRTOS):
 *   emda_mutex_t emda_port_mutex_create(void) {
 *       return (emda_mutex_t)xSemaphoreCreateMutex();
 *   }
 *   int emda_port_mutex_lock(emda_mutex_t m, uint32_t timeout_ms) {
 *       return (xSemaphoreTake((SemaphoreHandle_t)m, pdMS_TO_TICKS(timeout_ms)) == pdPASS)
 *              ? EMDA_EOK : EMDA_ETIMEOUT;
 *   }
 *
 * 裸机无互斥锁时：临界区已足够保护，互斥锁直接返回成功。
 */

emda_mutex_t emda_port_mutex_create(void)
{
    /* TODO: 移植时实现互斥锁创建 */
    return NULL;
}

void emda_port_mutex_destroy(emda_mutex_t mutex)
{
    (void)mutex;
    /* TODO: 移植时实现互斥锁销毁 */
}

int emda_port_mutex_lock(emda_mutex_t mutex, uint32_t timeout_ms)
{
    (void)mutex;
    (void)timeout_ms;
    /* TODO: 移植时实现互斥锁获取 */
    return EMDA_EOK;
}

int emda_port_mutex_unlock(emda_mutex_t mutex)
{
    (void)mutex;
    /* TODO: 移植时实现互斥锁释放 */
    return EMDA_EOK;
}
