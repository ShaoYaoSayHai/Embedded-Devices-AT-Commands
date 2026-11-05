/**
 * @file fake_modem.h
 * @brief 主机侧模拟模组：实现 emda_port 串口接口，用脚本应答 AT 指令
 *
 * 仅用于 PC 上跑通「AT核心 + 设备层 + socket层 + ML307A类」的分层逻辑。
 */

#ifndef FAKE_MODEM_H
#define FAKE_MODEM_H

#include <stdint.h>

/* 复位模拟器(清空收发缓冲、tick) */
void fake_modem_reset(void);

/* 注入一段原始 URC 文本(模拟模组主动上报) */
void fake_modem_inject(const char *text);

/* 注入一条 +MIPURC: "rtcp",<id>,<len>,<data> 数据上报 */
void fake_modem_inject_data(int id, const char *data);

/* 注入数据上报的前半段(用于验证 URC 分片重组) */
void fake_modem_inject_data_part1(int id, const char *data, int part_len);

/* 注入数据上报的后半段 */
void fake_modem_inject_data_part2(const char *data_tail);

/* 最后一次收到的完整指令(便于断言) */
const char *fake_modem_last_cmd(void);

#endif /* FAKE_MODEM_H */
