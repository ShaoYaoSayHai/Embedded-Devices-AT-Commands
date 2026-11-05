/**
 * @file emda_device.h
 * @author Zorian(1551769443@qq.com)
 * @brief  EMDA 设备层(控制面)：模组类框架 + 监控调度 + 网络状态机
 *
 * 对应 RT-Thread at_device 里的 at_device.c 与 class/xxx/at_device_xxx.c，
 * 但把「阻塞式设备线程」换成了本库的「非阻塞监控 Map」。
 *
 * 职责边界：
 *   - 认识「模组」：注册设备类、上电初始化、网络注册/信号/掉线巡检
 *   - 认识「状态」：UNKNOWN -> INIT -> REGISTERING -> REGISTERED -> READY
 *   - 不认识「数据」：socket 收发由 emda_socket 负责(数据面)
 *
 * 监控调度模型：
 *   每条监控指令注册为一个 emda_monitor_t(指令 + 状态过滤 + 间隔 + 回调)，
 *   每轮在「AT事务空闲」时挑出「状态匹配 + 已到期 + 最久未执行」的一条提交。
 *   同一条 AT 指令可在不同状态下注册多次，绑定不同解析回调。
 *
 * @version 0.3
 * @date 2026-11-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef EMDA_DEVICE_H
#define EMDA_DEVICE_H

#include "emda_at.h"
#include "emda_config.h"
#include "emda_def.h"

/* ======================================================================== */
/*                          网络/模组状态                                    */
/* ======================================================================== */
/*
 * 这是一条通用的「模组就绪度」阶梯，WIFI / 蜂窝模组都能映射：
 *
 *   UNKNOWN      上电未知，先探测模组是否响应
 *   INIT         模组已响应，检查 SIM / 基础配置
 *   REGISTERING  正在寻网/连接AP，尚未注册成功
 *   REGISTERED   已注册(蜂窝驻网成功 / WIFI已连AP)，可以开始建链
 *   READY        已就绪(拿到IP/PDP)，可自由收发
 *   LOST         运行中丢失网络，需重新爬梯
 *   BUSY         模组忙(长流程执行中)，暂停常规巡检
 *   FAILED       不可恢复错误(SIM 异常/模组无响应)
 *
 * 监控项通过 state 字段声明「只在某个状态下调度」。
 */
typedef enum
{
    EMDA_NET_STATE_UNKNOWN = 0U,
    EMDA_NET_STATE_INIT,
    EMDA_NET_STATE_REGISTERING,
    EMDA_NET_STATE_REGISTERED,
    EMDA_NET_STATE_READY,
    EMDA_NET_STATE_LOST,
    EMDA_NET_STATE_BUSY,
    EMDA_NET_STATE_FAILED,
    EMDA_NET_STATE_MAX,
} emda_net_state_t;

/* ======================================================================== */
/*                          监控项                                           */
/* ======================================================================== */

/**
 * @brief 一条监控指令(状态过滤 + 触发间隔 + 响应解析)
 *
 *  - name        : 监控项名称，同一条指令在不同上下文用不同名字区分
 *  - cmd         : AT指令字符串(含\r\n)，如 "AT+CEREG?\r\n"
 *  - state       : 仅在模组处于此状态时参与调度
 *  - interval_ms : 两次执行的最小间隔
 *  - resp_cb     : 响应解析回调(返回 EMDA_EOK 即视为本次完成)
 *  - timeout_cb  : 超时回调
 *  - timeout_ms  : 单次指令超时
 *  - 运行时字段  : 由设备层维护，注册时无需关心
 */
typedef struct
{
    const char *name;
    const char *cmd;
    emda_net_state_t state;
    uint32_t interval_ms;
    emda_at_resp_cb_t resp_cb;
    emda_at_timeout_cb_t timeout_cb;
    uint32_t timeout_ms;

    /* ---- 运行时字段(设备层维护) ---- */
    emda_tick_t last_run_tick;
    uint8_t is_used;
    uint8_t has_run;
} emda_monitor_t;

/* ======================================================================== */
/*                          设备类(模组抽象)                                 */
/* ======================================================================== */

struct emda_device;
typedef struct emda_device emda_device_t;

/**
 * @brief 控制面操作(模组无关框架 -> 具体模组)
 */
typedef struct
{
    /**
     * @brief 模组初始化：基础配置 + 注册监控项
     *        (非阻塞实现：把要做的事变成监控项，而不是在这里死等)
     */
    int (*init)(emda_device_t *dev);

    /**
     * @brief 厂商私有控制命令(透传)，cmd 建议用 EMDA_DEV_CTRL_CUSTOM 起偏移
     */
    int (*control)(emda_device_t *dev, int cmd, void *arg);
} emda_device_ops_t;

/**
 * @brief 数据面操作(socket 层 -> 具体模组)
 *
 * 这些接口都是「发起」语义：把 AT 指令交给 emda_at_submit()，
 * 真正的结果通过 emda_sock_report() 异步上报给 socket 层。
 */
typedef struct
{
    int (*open)(emda_device_t *dev, int sock, emda_sock_type_t type,
                const char *host, uint16_t port);
    int (*send)(emda_device_t *dev, int sock, const void *data, uint16_t len);
    int (*close)(emda_device_t *dev, int sock);
} emda_socket_ops_t;

/**
 * @brief 模组类描述符：一个文件描述一种模组
 */
typedef struct
{
    const char *name;                  /* 模组类名，如 "ml307a" */
    const emda_device_ops_t *dev_ops;  /* 控制面 */
    const emda_socket_ops_t *sock_ops; /* 数据面 */
    uint8_t sock_num;                  /* 支持的最大连接数 */
} emda_device_class_t;

/**
 * @brief 设备实例(当前为单实例设计：一路串口 <-> 一个模组)
 */
struct emda_device
{
    const emda_device_class_t *cls;
    void *user_data;
    emda_net_state_t net_state;
};

/* 厂商私有控制命令从这里开始编号 */
#define EMDA_DEV_CTRL_CUSTOM (0x0100)

/* ======================================================================== */
/*                              设备层API                                    */
/* ======================================================================== */

/**
 * @brief 绑定模组类并创建设备实例
 * @param cls       模组类描述符(静态常量)
 * @param user_data 使用者私有数据(可为NULL)
 * @return EMDA_EOK / EMDA_EINVAL / EMDA_EBUSY
 */
int emda_device_register(const emda_device_class_t *cls, void *user_data);

/**
 * @brief 启动设备：初始化AT核心 + 调用模组类 init
 * @return EMDA_EOK / EMDA_EINVAL / 模组类返回值
 */
int emda_device_start(void);

/**
 * @brief 设备层轮询：AT空闲时挑一条到期的监控指令提交
 *        通常由 emda_poll() 统一调用，无需单独调用
 */
void emda_device_poll(void);

/**
 * @brief 顶层轮询入口：驱动 AT事务 -> socket -> 设备监控
 *
 * 移植者只需在任务/主循环里反复调用本函数。
 */
void emda_poll(void);

/**
 * @brief 执行模组类自定义控制命令(透传给 cls->dev_ops->control)
 */
int emda_device_control(int cmd, void *arg);

/* ---- 监控项管理 ---- */

/**
 * @brief 注册一条监控指令
 * @return EMDA_EOK / EMDA_EINVAL / EMDA_ENOMEM
 */
int emda_monitor_add(const emda_monitor_t *item);

/**
 * @brief 按名称替换监控项的解析回调(运行时切换上下文用)
 * @param name       监控项名称
 * @param resp_cb    新的响应回调(传NULL表示不修改)
 * @param timeout_cb 新的超时回调(传NULL表示不修改)
 * @return EMDA_EOK / EMDA_EINVAL
 */
int emda_monitor_set_cb(const char *name,
                        emda_at_resp_cb_t resp_cb,
                        emda_at_timeout_cb_t timeout_cb);

/**
 * @brief 提交一条业务临时指令(HTTP/MQTT/私有AT等)
 *        直接进入 AT 队列，优先于监控指令执行
 */
int emda_device_exec(const char *cmd,
                     emda_at_resp_cb_t resp_cb,
                     uint32_t timeout_ms,
                     emda_at_timeout_cb_t timeout_cb);

/* ---- 状态访问 ---- */

/**
 * @brief 设置网络/模组状态(线程安全)，监控回调里用它推进状态机
 */
void emda_net_state_set(emda_net_state_t state);

/**
 * @brief 获取当前网络/模组状态(线程安全)
 */
emda_net_state_t emda_net_state_get(void);

/**
 * @brief 获取设备实例(可为NULL，表示尚未注册模组类)
 */
emda_device_t *emda_device_handle(void);

/**
 * @brief 获取已绑定的模组类
 */
const emda_device_class_t *emda_device_class(void);

#endif /* EMDA_DEVICE_H */
