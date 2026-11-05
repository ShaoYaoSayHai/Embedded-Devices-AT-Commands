/**
 * @file emda_def.h
 * @author Zorian(1551769443@qq.com)
 * @brief  EMDA (Embedded Module Device AT) 公共基础定义
 *
 *  - 基础类型：emda_tick_t
 *  - 统一返回码：所有层用 EMDA_Exxx，负值表示失败
 *  - 日志宏：默认关闭，使用者可自定义 EMDA_LOG 对接 printf/rt_kprintf
 *
 * @version 0.3
 * @date 2026-11-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef EMDA_DEF_H
#define EMDA_DEF_H

#include <stdint.h>
#include <stddef.h>

/* ======================================================================== */
/*                              基础类型                                     */
/* ======================================================================== */

/* 心跳tick类型(对应毫秒数，由 emda_heartbeat() 自增) */
typedef uint32_t emda_tick_t;

/* ======================================================================== */
/*                              统一返回码                                   */
/* ======================================================================== */
/*
 * 所有对外接口统一返回 int：
 *   EMDA_EOK(0) 成功，其余负值失败
 *   >= 0 的其它值留给具体接口表达长度/数量等语义
 */

#define EMDA_EOK        (0)   /* 成功 */
#define EMDA_ERROR      (-1)  /* 通用失败 */
#define EMDA_ETIMEOUT   (-2)  /* 超时 */
#define EMDA_EINVAL     (-3)  /* 参数非法 */
#define EMDA_EBUSY      (-4)  /* 忙：有在途事务/资源被占用 */
#define EMDA_ENOMEM     (-5)  /* 空间不足：队列满/缓冲不够 */
#define EMDA_ENOSYS     (-6)  /* 未实现：当前模组类不支持 */
#define EMDA_EIO        (-7)  /* 链路/硬件失败 */

/* 无限等待标志(互斥锁超时) */
#define EMDA_WAIT_FOREVER (0xFFFFFFFFU)

/* ======================================================================== */
/*                              socket 类型                                  */
/* ======================================================================== */
/*
 * 放在公共头里，避免 emda_device.h(模组类)与 emda_socket.h(socket层)互相包含。
 */

typedef enum
{
    EMDA_SOCK_STREAM = 0, /* TCP */
    EMDA_SOCK_DGRAM = 1,  /* UDP */
} emda_sock_type_t;

/* ======================================================================== */
/*                              日志接口                                     */
/* ======================================================================== */
/*
 * 默认不产生任何代码。启用方式(在编译选项或 emda_config.h 中)：
 *
 *   #define EMDA_LOG_ENABLE 1
 *   #define EMDA_LOG(fmt, ...)  printf(fmt, ##__VA_ARGS__)
 *
 * RT-Thread 下推荐：
 *   #define EMDA_LOG(fmt, ...)  rt_kprintf(fmt, ##__VA_ARGS__)
 */

#ifndef EMDA_LOG_ENABLE
#define EMDA_LOG_ENABLE (0)
#endif

#if EMDA_LOG_ENABLE
#ifndef EMDA_LOG
#define EMDA_LOG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#endif
#else
#define EMDA_LOG(fmt, ...) do { } while (0)
#endif

#endif /* EMDA_DEF_H */
