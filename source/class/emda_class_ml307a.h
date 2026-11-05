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

#endif /* EMDA_CLASS_ML307A_H */
