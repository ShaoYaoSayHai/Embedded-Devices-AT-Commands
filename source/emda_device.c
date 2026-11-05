/**
 * @file emda_device.c
 * @author Zorian(1551769443@qq.com)
 * @brief  EMDA 设备层实现：模组类绑定 + 监控调度 + 网络状态机
 *
 * 调度要点：
 *   1. 只在 AT 事务完全空闲时提交，保证任一时刻只有一条指令在途
 *   2. 「最久未执行优先」保证同状态多条监控指令公平轮转，不饿死
 *   3. 状态过滤：只为当前网络状态的监控项服务，其他状态项完全不动
 *   4. 提交成功后才刷新 last_run_tick，队列满时会自动下一轮重试
 *
 * @version 0.3
 * @date 2026-11-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "emda_device.h"
#include "emda_socket.h"

#include <string.h>

/* ======================================================================== */
/*                          内部数据                                          */
/* ======================================================================== */

/* 监控项表 */
static emda_monitor_t s_monitor[EMDA_MONITOR_MAX];

/* 设备实例(单实例：一路串口对应一个模组) */
static emda_device_t s_device;

/* ======================================================================== */
/*                          状态访问                                          */
/* ======================================================================== */

void emda_net_state_set(emda_net_state_t state)
{
    emda_net_state_t old;

    emda_port_enter_critical();
    old = s_device.net_state;
    s_device.net_state = state;
    emda_port_exit_critical();

    if (old != state)
    {
        EMDA_LOG("[emda_device] net_state %d -> %d\r\n", (int)old, (int)state);
    }
}

emda_net_state_t emda_net_state_get(void)
{
    emda_net_state_t state;

    emda_port_enter_critical();
    state = s_device.net_state;
    emda_port_exit_critical();
    return state;
}

emda_device_t *emda_device_handle(void)
{
    return (s_device.cls != NULL) ? &s_device : NULL;
}

const emda_device_class_t *emda_device_class(void)
{
    return s_device.cls;
}

/* ======================================================================== */
/*                          设备生命周期                                      */
/* ======================================================================== */

int emda_device_register(const emda_device_class_t *cls, void *user_data)
{
    if (cls == NULL || cls->name == NULL)
    {
        return EMDA_EINVAL;
    }
    if (s_device.cls != NULL)
    {
        return EMDA_EBUSY; /* 已绑定模组类，先复位再换 */
    }

    s_device.cls = cls;
    s_device.user_data = user_data;
    s_device.net_state = EMDA_NET_STATE_UNKNOWN;

    return EMDA_EOK;
}

int emda_device_start(void)
{
    int ret;

    if (s_device.cls == NULL)
    {
        return EMDA_EINVAL;
    }

    ret = emda_at_init();
    if (ret != EMDA_EOK)
    {
        return ret;
    }

    emda_socket_init();
    emda_net_state_set(EMDA_NET_STATE_UNKNOWN);

    if (s_device.cls->dev_ops != NULL && s_device.cls->dev_ops->init != NULL)
    {
        ret = s_device.cls->dev_ops->init(&s_device);
    }

    return ret;
}

int emda_device_control(int cmd, void *arg)
{
    if (s_device.cls == NULL || s_device.cls->dev_ops == NULL ||
        s_device.cls->dev_ops->control == NULL)
    {
        return EMDA_ENOSYS;
    }
    return s_device.cls->dev_ops->control(&s_device, cmd, arg);
}

/* ======================================================================== */
/*                          监控项管理                                        */
/* ======================================================================== */

int emda_monitor_add(const emda_monitor_t *item)
{
    if (item == NULL || item->cmd == NULL || item->name == NULL)
    {
        return EMDA_EINVAL;
    }

    for (uint32_t i = 0U; i < EMDA_MONITOR_MAX; i++)
    {
        if (!s_monitor[i].is_used)
        {
            s_monitor[i].name = item->name;
            s_monitor[i].cmd = item->cmd;
            s_monitor[i].state = item->state;
            s_monitor[i].interval_ms = item->interval_ms;
            s_monitor[i].resp_cb = item->resp_cb;
            s_monitor[i].timeout_cb = item->timeout_cb;
            s_monitor[i].timeout_ms = item->timeout_ms;
            s_monitor[i].last_run_tick = 0U;
            s_monitor[i].has_run = 0U;
            s_monitor[i].is_used = 1U;
            return EMDA_EOK;
        }
    }
    return EMDA_ENOMEM;
}

int emda_monitor_set_cb(const char *name,
                        emda_at_resp_cb_t resp_cb,
                        emda_at_timeout_cb_t timeout_cb)
{
    if (name == NULL)
    {
        return EMDA_EINVAL;
    }

    for (uint32_t i = 0U; i < EMDA_MONITOR_MAX; i++)
    {
        if (s_monitor[i].is_used &&
            strncmp(s_monitor[i].name, name, EMDA_NAME_LEN) == 0)
        {
            if (resp_cb != NULL)
            {
                s_monitor[i].resp_cb = resp_cb;
            }
            if (timeout_cb != NULL)
            {
                s_monitor[i].timeout_cb = timeout_cb;
            }
            return EMDA_EOK;
        }
    }
    return EMDA_EINVAL;
}

int emda_device_exec(const char *cmd,
                     emda_at_resp_cb_t resp_cb,
                     uint32_t timeout_ms,
                     emda_at_timeout_cb_t timeout_cb)
{
    return emda_at_submit(cmd, resp_cb, timeout_ms, timeout_cb);
}

/**
 * @brief 全扫描监控表，选出「状态匹配 + 已到期 + 最久未执行」的条目
 * @param now 当前 tick
 * @return 选中条目指针，无到期项返回 NULL
 */
static emda_monitor_t *s_monitor_pick(emda_tick_t now)
{
    const emda_net_state_t cur_state = emda_net_state_get();
    emda_monitor_t *picked = NULL;
    emda_tick_t max_elapsed = 0U;

    for (uint32_t i = 0U; i < EMDA_MONITOR_MAX; i++)
    {
        emda_monitor_t *item = &s_monitor[i];
        emda_tick_t elapsed;

        if (!item->is_used || item->state != cur_state)
        {
            continue;
        }

        /* 从未执行过 -> 视为最久未执行(冷启动优先) */
        if (!item->has_run)
        {
            elapsed = (emda_tick_t)-1;
        }
        else
        {
            elapsed = now - item->last_run_tick;
        }

        /* 间隔未到期则跳过 */
        if (item->has_run && item->interval_ms > 0U &&
            elapsed < emda_tick_from_ms(item->interval_ms))
        {
            continue;
        }

        if (picked == NULL || elapsed > max_elapsed)
        {
            picked = item;
            max_elapsed = elapsed;
        }
    }

    return picked;
}

/* ======================================================================== */
/*                          设备层轮询                                        */
/* ======================================================================== */

void emda_device_poll(void)
{
    emda_monitor_t *item;
    emda_tick_t now;

    if (s_device.cls == NULL)
    {
        return;
    }

    /* 只有AT事务完全空闲时才允许发起新的监控指令 */
    if (!emda_at_ready())
    {
        return;
    }

    now = emda_tick_get();
    item = s_monitor_pick(now);
    if (item == NULL)
    {
        return;
    }

    if (emda_at_submit(item->cmd, item->resp_cb, item->timeout_ms,
                       item->timeout_cb) == EMDA_EOK)
    {
        item->last_run_tick = now;
        item->has_run = 1U;
    }
    /* 提交失败(队列满)不刷新时间戳，下一轮自动重试 */
}

void emda_poll(void)
{
    /* 1. AT事务：收数据 -> URC -> 状态机推进(可能刚刚腾空管道) */
    emda_at_poll();

    /* 2. socket层：有待发数据则在管道空闲时下发(业务优先于巡检) */
    emda_socket_poll();

    /* 3. 设备层：管道仍空闲则挑一条监控指令 */
    emda_device_poll();
}
