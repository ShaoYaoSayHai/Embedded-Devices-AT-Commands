/**
 * @file emda_socket.h
 * @author Zorian(1551769443@qq.com)
 * @brief  EMDA 网络层(数据面)：socket 句柄 + 收发缓冲 + 异步事件
 *
 * 对应 RT-Thread at_device 里的 at_socket.c：
 *   - 本层只做「socket 资源与缓冲管理」，不认识任何 AT 指令
 *   - 真正的 AT 报文由模组类的 socket_ops 提供
 *     (class/emda_class_xxx.c 里的 open/send/close)
 *
 * 与阻塞式 socket 的差别：
 *   本库是非阻塞轮询，connect/send 都是「发起」语义，结果通过事件回调通知：
 *     EMDA_SOCK_EVT_CONNECTED / CONNECT_FAILED / SENT / SEND_FAILED / CLOSED
 *   收到的数据由模组类解析 URC 后调用 emda_socket_input() 入环形缓冲，
 *   业务侧用 emda_socket_recv() 取走(不阻塞，取多少算多少)。
 *
 * @version 0.3
 * @date 2026-11-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef EMDA_SOCKET_H
#define EMDA_SOCKET_H

#include "emda_config.h"
#include "emda_def.h"
#include "emda_device.h"

/* ======================================================================== */
/*                          事件与状态                                       */
/* ======================================================================== */

typedef enum
{
    EMDA_SOCK_EVT_CONNECTED = 0,   /* 连接建立成功 */
    EMDA_SOCK_EVT_CONNECT_FAILED,  /* 连接失败 */
    EMDA_SOCK_EVT_SENT,            /* 数据已交给模组 */
    EMDA_SOCK_EVT_SEND_FAILED,     /* 发送失败(超时/链路断) */
    EMDA_SOCK_EVT_CLOSED,          /* 连接已关闭(主动或对端断开) */
} emda_sock_event_t;

typedef enum
{
    EMDA_SOCK_STATE_CLOSED = 0,
    EMDA_SOCK_STATE_CONNECTING,
    EMDA_SOCK_STATE_CONNECTED,
    EMDA_SOCK_STATE_CLOSING,
} emda_sock_state_t;

/**
 * @brief socket 事件回调
 * @param sock socket 句柄(emda_socket_create 的返回值)
 * @param evt  事件类型
 */
typedef void (*emda_sock_event_cb_t)(int sock, emda_sock_event_t evt);

/* ======================================================================== */
/*                              业务侧API                                    */
/* ======================================================================== */

/**
 * @brief 申请一个 socket
 * @param type TCP=EMDA_SOCK_STREAM / UDP=EMDA_SOCK_DGRAM
 * @param cb   事件回调(可为NULL)
 * @return >=0 socket句柄, <0 失败(EMDA_ENOMEM/EMDA_EINVAL)
 */
int emda_socket_create(emda_sock_type_t type, emda_sock_event_cb_t cb);

/**
 * @brief 发起连接(异步，结果通过 EMDA_SOCK_EVT_CONNECTED 通知)
 * @return EMDA_EOK=已下发, 其它=失败
 */
int emda_socket_connect(int sock, const char *host, uint16_t port);

/**
 * @brief 发送数据(异步，结果通过 EMDA_SOCK_EVT_SENT 通知)
 *
 * 数据先拷贝到该 socket 的发送缓冲，等 AT 管道空闲时自动下发，
 * 因此调用后即可释放入参。单个 socket 同时只允许一笔待发数据。
 *
 * @return EMDA_EOK=已受理, EMDA_EBUSY=上一笔还没发完, EMDA_ENOMEM=超长
 */
int emda_socket_send(int sock, const void *data, uint16_t len);

/**
 * @brief 从接收缓冲取数据(不阻塞)
 * @return >0 实际读到的字节数, 0=当前无数据, <0 参数错误
 */
int emda_socket_recv(int sock, void *buf, uint16_t len);

/**
 * @brief 关闭连接(异步，EMDA_SOCK_EVT_CLOSED 通知)
 */
int emda_socket_close(int sock);

/**
 * @brief 查询 socket 当前状态
 */
emda_sock_state_t emda_socket_state_get(int sock);

/* ======================================================================== */
/*                    供模组类(class)内部调用的接口                          */
/* ======================================================================== */

/**
 * @brief 复位 socket 层(由 emda_device_start 调用)
 */
void emda_socket_init(void);

/**
 * @brief 把 URC 收到的数据写入指定 socket 的接收缓冲
 *        缓冲满时丢弃新数据(并可通过 EMDA_LOG 观察)
 */
void emda_socket_input(int sock, const uint8_t *data, uint16_t len);

/**
 * @brief 模组类上报连接/发送结果
 */
void emda_socket_report(int sock, emda_sock_event_t evt);

/**
 * @brief 下发待发数据(由 emda_poll 调用，通常无需直接调用)
 */
void emda_socket_poll(void);

#endif /* EMDA_SOCKET_H */
