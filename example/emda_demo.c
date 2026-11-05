/**
 * @file emda_demo.c
 * @author Zorian(1551769443@qq.com)
 * @brief  EMDA 使用示例：业务层只写这么点代码
 *
 * 演示三件事：
 *   1. 绑定模组类 + 启动(模组巡检、SIM/驻网/信号全部由模组类自己负责)
 *   2. socket 收发(TCP 建链/发送/接收/关闭，全异步事件驱动)
 *   3. 业务临时 AT 指令(HTTP/MQTT 等私有指令走 emda_device_exec)
 *
 * 本文件仅作示例，不参与库编译。
 *
 * @version 0.3
 * @date 2026-11-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#include <stdio.h>
#include <string.h>

#include "emda_at.h"
#include "emda_device.h"
#include "emda_socket.h"
#include "class/emda_class_ml307a.h"

/* ======================================================================== */
/*                  示例1: socket 事件回调                                   */
/* ======================================================================== */

static int s_sock = -1;
static int s_ready_to_send = 0;

static void demo_sock_evt(int sock, emda_sock_event_t evt)
{
    (void)sock;

    switch (evt)
    {
    case EMDA_SOCK_EVT_CONNECTED:
        s_ready_to_send = 1;
        break;

    case EMDA_SOCK_EVT_SENT:
        /* 数据已交给模组，可以继续发下一包 */
        break;

    case EMDA_SOCK_EVT_CLOSED:
    case EMDA_SOCK_EVT_CONNECT_FAILED:
    case EMDA_SOCK_EVT_SEND_FAILED:
        s_ready_to_send = 0;
        s_sock = -1;
        break;

    default:
        break;
    }
}

/* ======================================================================== */
/*                  示例2: 业务临时指令(HTTP/MQTT/私有AT)                    */
/* ======================================================================== */

static int demo_http_resp(uint8_t *buffer, uint16_t len)
{
    (void)len;
    if (strstr((const char *)buffer, "OK") != NULL)
    {
        return EMDA_EOK;
    }
    return EMDA_ERROR;
}

void demo_http_get(void)
{
    /* 直接进 AT 队列，优先于巡检指令执行；解析结果走回调 */
    (void)emda_device_exec("AT+HTTPGET=\"http://api.example.com/time\"\r\n",
                           demo_http_resp, 10000U, NULL);
}

/* ======================================================================== */
/*                  示例3: 启动 + 业务主循环                                 */
/* ======================================================================== */

void demo_start(void)
{
    /* 1. 绑定模组类(换模组只改这一行 + 链接对应 class 文件) */
    if (emda_device_register(&emda_class_ml307a, NULL) != EMDA_EOK)
    {
        return;
    }

    /* 2. 启动：初始化AT核心、注册监控项、挂URC回调 */
    if (emda_device_start() != EMDA_EOK)
    {
        return;
    }

    /* 3. 之后什么都不用管，emda_poll() 会自动把模组带到 READY */
}

/**
 * @brief 业务主循环里做三件事：驱动引擎、看状态、用 socket
 *
 * 注意：这里的 socket 操作都是异步的，不要写成 while 死等。
 */
void demo_loop(void)
{
    int rssi[2] = {99, 99};

    /* 驱动整个组件：AT事务 -> socket -> 设备巡检 */
    emda_poll();

    /* 网络就绪后才建链(READY = 已驻网且附着) */
    if (emda_net_state_get() == EMDA_NET_STATE_READY && s_sock < 0)
    {
        s_sock = emda_socket_create(EMDA_SOCK_STREAM, demo_sock_evt);
        if (s_sock >= 0)
        {
            (void)emda_socket_connect(s_sock, "120.27.12.119", 2040);
        }
    }

    if (s_ready_to_send)
    {
        if (emda_socket_send(s_sock, "hello", 5) == EMDA_EOK)
        {
            s_ready_to_send = 0; /* 收到 SENT 事件后再发下一包 */
        }
    }

    /* 取接收数据(URC 上报的数据已经躺在 socket 接收缓冲里) */
    if (s_sock >= 0)
    {
        char buf[64];
        int n = emda_socket_recv(s_sock, buf, sizeof(buf));
        if (n > 0)
        {
            /* 这里处理收到的数据 */
            (void)n;
        }
    }

    /* 模组私有信息：最近一次 CSQ */
    (void)emda_device_control(EMDA_ML307A_CTRL_GET_CSQ, rssi);
}

/* ======================================================================== */
/*                  示例4: 三种运行环境的驱动方式                            */
/* ======================================================================== */

/* ---- 裸机：心跳放 SysTick 中断，poll 放主循环 ---- */
void SysTick_Handler(void)
{
    emda_heartbeat();
}

void baremetal_main(void)
{
    demo_start();

    while (1)
    {
        demo_loop();
        /* 其它业务... */
    }
}

/* ---- FreeRTOS（编译时定义 EMDA_DEMO_FREERTOS）---- */
#if defined(EMDA_DEMO_FREERTOS)
#include "FreeRTOS.h"
#include "task.h"

static void emda_task(void *arg)
{
    (void)arg;

    demo_start();

    while (1)
    {
        demo_loop();
        emda_heartbeat();
        vTaskDelay(pdMS_TO_TICKS(EMDA_HEARTBEAT_PERIOD_MS));
    }
}
#endif /* EMDA_DEMO_FREERTOS */

/* ---- RT-Thread（编译时定义 RT_USING_RTTHREAD）---- */
#ifdef RT_USING_RTTHREAD
#include <rtthread.h>

static void emda_thread_entry(void *arg)
{
    (void)arg;

    demo_start();

    while (1)
    {
        demo_loop();
        emda_heartbeat();
        rt_thread_mdelay(EMDA_HEARTBEAT_PERIOD_MS);
    }
}
#endif
