/**
 * @file emda_class_ml307a.h
 * @author Zorian(1551769443@qq.com)
 * @brief  EMDA 模组类：中移物联 ML307A (4G Cat.1)
 *
 * 一个模组类 = 一个文件，包含两面：
 *   控制面 dev_ops : 上电探测 / SIM / 驻网 / 信号 / 掉线巡检(监控Map)
 *   数据面 sock_ops: AT+MIPOPEN / AT+MIPSEND / AT+MIPCLOSE
 *
 * 命令依据：
 *   document/ML307A/TCP_IP用户手册.pdf      (MIPOPEN/MIPSEND/MIPCLOSE/MIPURC)
 *   document/ML307A/ML307A_通信流程示例.pdf (CEREG 驻网 -> MIPOPEN)
 *
 * 换模组时照此文件另写一份，业务代码与 socket 层都不用改。
 *
 * @version 0.3
 * @date 2026-11-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef EMDA_CLASS_ML307A_H
#define EMDA_CLASS_ML307A_H

#include "../emda_device.h"

/**
 * @brief ML307A 模组类描述符，用于 emda_device_register()
 */
extern const emda_device_class_t emda_class_ml307a;

/* ---- 模组私有控制命令(emda_device_control) ---- */

/**
 * @brief 读取最近一次 AT+CSQ 解析出的信号质量
 *        arg 传入 int[2]：{rssi, ber}，未取得时 rssi=99
 */
#define EMDA_ML307A_CTRL_GET_CSQ (EMDA_DEV_CTRL_CUSTOM + 0)

/* ======================================================================== */
/*                          MQTT 数据面                                      */
/* ======================================================================== */
/*
 * ML307A 的 MQTT 是模组内置协议栈，命令前缀 AT+MQTT*，和 socket 层并行。
 * 连接结果异步上报，状态有两套编号，不能混用：
 *
 *   AT+MQTTSTATE=<id> -> +MQTTSTATE: <state>     1 连接中 / 2 成功 / 3 断开
 *   URC "conn"        -> +MQTTURC: "conn",<id>,<s>  0 成功 / 1 重连 / 2 用户断开
 *                                                   3 被拒 / 4 服务器断开
 *                                                   5 Ping 超时 / 6 网络异常 / 255 未知
 *
 * 下行消息：+MQTTURC: "publish",<id>,<mid>,<topic>,<total_len>,<payload_len>,<payload>
 */

#define EMDA_ML307A_MQTT_MAX     (6)   /* connect_id 0~5 */

/* 下行消息回调（payload 是原始字节，可能含 \0，必须按 len 处理） */
typedef void (*emda_ml307a_mqtt_msg_cb_t)(int cid, const char *topic,
                                          const uint8_t *payload, uint16_t len);

/**
 * @brief 设置 PDP 上下文使用的 APN
 *
 * ML307A 第 1 路默认承载会自动配置，且手工配置**掉电不保存**，所以：
 *   - 公网卡/默认 APN：不用调这个函数；
 *   - 物联网卡(如 CMIOT)、私有 APN：必须在 emda_device_start() 之前调用一次，
 *     库会在 INIT 阶段下发 AT+CGDCONT=1,"IPV4V6","<apn>"。
 *
 * @param apn 如 "CMNET" / "CMIOT" / "CTNET" / "UNINET"，最长 32 字节
 * @return EMDA_EOK / EMDA_EINVAL
 */
int emda_ml307a_set_apn(const char *apn);

/* 注册下行消息回调（全局唯一，NULL 表示不接收） */
void emda_ml307a_mqtt_set_msg_cb(emda_ml307a_mqtt_msg_cb_t cb);

/**
 * @brief 建立 MQTT 连接（异步，结果由 "conn" URC 上报）
 * @param cid       MQTT 客户端 ID，0~5
 * @param host      服务器地址（域名或 IP）
 * @param port      端口，1883/8883/8083
 * @param client_id 终端标识
 * @param user      用户名，NULL 表示不带认证
 * @param pass      密码，user 为 NULL 时忽略
 * @return EMDA_EOK=已入队
 */
int emda_ml307a_mqtt_connect(int cid, const char *host, uint16_t port,
                             const char *client_id,
                             const char *user, const char *pass);

/**
 * @brief 订阅主题（结果由 "suback" URC 上报）
 *        `AT+MQTTSUB=<cid>,<topic>,<qos>`
 */
int emda_ml307a_mqtt_subscribe(int cid, const char *topic, int qos);

/**
 * @brief 发布消息（数据模式：先发长度，等 '>' 再送 payload）
 * @note  ML307A 单条 <msg_len> 上限 1024，本库受 EMDA_TX_PAYLOAD_SIZE 限制
 */
int emda_ml307a_mqtt_publish(int cid, const char *topic,
                             const uint8_t *payload, uint16_t len, int qos);

/**
 * @brief 主动断开，会收到 +MQTTURC: "conn",<cid>,2
 */
int emda_ml307a_mqtt_disconnect(int cid);

/**
 * @brief 查询本地维护的连接状态（由 "conn" URC 更新）
 * @return 1=已连接, 0=未连接
 */
int emda_ml307a_mqtt_connected(int cid);

/**
 * @brief 主动查询 MQTT 连接状态（AT+MQTTSTATE=<cid>）
 *        结果通过回调更新本地状态，用于 URC 丢失时兜底
 */
int emda_ml307a_mqtt_poll_state(int cid);

#endif /* EMDA_CLASS_ML307A_H */
