/**
 * @file emda_at.h
 * @author Zorian(1551769443@qq.com)
 * @brief  EMDA AT 事务核心(对应 RT-Thread AT 包里的 at_client)
 *
 * 只做两件事：
 *   1. 把「一条 AT 指令 + 它的响应」可靠地跑完
 *   2. 把串口字节流按 AT 语法切成两种东西：URC 段 / 响应文本行
 *
 * 接收模型(不靠时间窗猜边界)：
 *   - 串口字节流先进环形缓冲，缓冲里始终是连续内存
 *   - 每次轮询按下面的顺序解析缓冲头部，能切出多少切多少：
 *       a) 两段式发送的提示符(如 ">")
 *       b) URC 段：前缀匹配 + 可选长度回调，整段到齐才回调
 *       c) 响应文本行：按 CRLF 切行，累积后交给 resp_cb
 *   - 前缀命中但整段还没收全时直接停住等待，绝不把半条 URC 当文本行
 *
 * 这样二进制 payload 由 URC 表按长度精确圈走，不经过任何文本匹配，
 * 也就不会出现"payload 里带 \r\n / 0x00 导致误判"的问题。
 *
 * 依赖关系：只依赖 emda_config.h / emda_def.h / emda_port.h
 *
 * @version 0.4
 * @date 2026-11-06
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef EMDA_AT_H
#define EMDA_AT_H

#include "emda_config.h"
#include "emda_def.h"
#include "emda_port.h"

/* ======================================================================== */
/*                          回调函数类型                                     */
/* ======================================================================== */

/**
 * @brief AT指令响应回调
 *
 * 每收到一整行文本就会调用一次，buffer 是本次事务至今的累积响应(以'\0'结尾)，
 * 因此可以跨行/跨分片解析，任一次返回 EMDA_EOK 即视为事务成功。
 *
 * @param buffer 累积响应文本
 * @param len    累积长度
 * @return EMDA_EOK=解析成功(结束事务), 其它=继续等待
 */
typedef int (*emda_at_resp_cb_t)(uint8_t *buffer, uint16_t len);

/**
 * @brief AT指令超时回调
 * @param cmd 超时的指令字符串(便于日志)
 * @return EMDA_EOK=已处理
 */
typedef int (*emda_at_timeout_cb_t)(char *cmd);

/**
 * @brief URC 处理回调
 *
 * data/len 是**完整的一条 URC**(含前缀、可能的裸数据、尾部)，保证连续。
 * 因为可能含二进制，解析时请按长度处理，不要对该指针用 strlen/strstr。
 *
 * @return EMDA_EOK=已处理, 其它=未处理
 */
typedef int (*emda_urc_cb_t)(const uint8_t *data, uint16_t len);

/**
 * @brief 计算一条 URC 占用的总字节数(用于带裸数据的 URC)
 *
 * 典型实现：从 header 里解析出 <len>，返回 header 长度 + <len>。
 *
 * @param buffer 缓冲头部(从前缀开始，保证连续)
 * @param avail  当前可用的总字节数
 * @return >0 该条 URC 的总长度；0 数据还没收全(等更多)；<0 格式不认识
 */
typedef int (*emda_urc_seg_cb_t)(const uint8_t *buffer, uint16_t avail);

/* ======================================================================== */
/*                          URC 表                                           */
/* ======================================================================== */

/**
 * @brief 一条 URC 描述
 *
 *  - prefix  : 必须从缓冲头部开始匹配的字符串，如 "+MIPURC: \"rtcp\","
 *  - seg_len : 计算整条长度的回调；传 NULL 表示"取到 CRLF 为止的一行"
 *  - handler : 整条到齐后的处理函数
 *
 * 表中顺序即优先级：先注册先匹配。带裸数据的条目要排在通用条目之前，
 * 例如 "+MIPURC: \"rtcp\"," 要排在 "+MIPURC:" 之前。
 */
typedef struct
{
    const char *prefix;
    emda_urc_seg_cb_t seg_len;
    emda_urc_cb_t handler;
} emda_urc_t;

/* ======================================================================== */
/*                          AT 事务状态机                                    */
/* ======================================================================== */

typedef enum
{
    EMDA_AT_STATE_IDLE = 0U, /* 空闲，等待下一条指令(来自队列或设备层) */
    EMDA_AT_STATE_SEND,      /* 正在通过UART发送AT指令 */
    EMDA_AT_STATE_WAITING,   /* 已发送，等待模组响应，期间回调解析 */
    EMDA_AT_STATE_RESP_OK,   /* 回调返回成功，响应已处理 */
    EMDA_AT_STATE_TIMEOUT,   /* 超时未收到有效响应 */
} emda_at_state_t;

/* ======================================================================== */
/*                              对外API                                      */
/* ======================================================================== */

/**
 * @brief AT 核心初始化：复位状态机/接收缓冲 + 调用 port 层 UART 初始化
 * @return EMDA_EOK / EMDA_EIO
 */
int emda_at_init(void);

/**
 * @brief 推进 AT 核心(非阻塞)
 *
 * 每轮执行：收串口字节 -> 解析 URC/响应 -> AT 状态机推进一步
 * 需高频调用；接收解析不受 EMDA_LOOP_INTERVAL_MS 限制。
 */
void emda_at_poll(void);

/**
 * @brief 提交一条 AT 指令(入队，按 FIFO 串行执行)
 *
 * 任意线程/中断上下文可调用；同一时刻只有一条指令在途。
 *
 * @param cmd        AT指令字符串(含\r\n)
 * @param resp_cb    响应回调；传 NULL 表示 fire-and-forget，
 *                   收到模组第一条响应行即结束事务(不再等超时)
 * @param timeout_ms 超时(ms)，0=使用 EMDA_AT_DEFAULT_TIMEOUT_MS
 * @param timeout_cb 超时回调(可为NULL)
 * @return EMDA_EOK=入队成功, EMDA_ENOMEM=队列满, EMDA_EINVAL=参数错误
 */
int emda_at_submit(const char *cmd,
                   emda_at_resp_cb_t resp_cb,
                   uint32_t timeout_ms,
                   emda_at_timeout_cb_t timeout_cb);

/**
 * @brief 提交一条「两段式」AT 指令(指令 -> 提示符 -> 原始数据)
 *
 * 典型场景：AT+MIPSEND=<id>,<len> 之后模组回 ">"，此时才能送数据。
 * 本函数会先发 cmd，收到 prompt 后立刻发送 payload，再继续等 RESP_OK。
 *
 * @note 要求提交时 AT 事务空闲(emda_at_ready()==1)，否则返回 EMDA_EBUSY；
 *       payload 会拷贝进内部缓冲，调用后即可释放。
 *       单次原始数据上限 = EMDA_TX_PAYLOAD_SIZE。
 */
int emda_at_submit_raw(const char *cmd,
                       const char *prompt,
                       const uint8_t *payload,
                       uint16_t payload_len,
                       emda_at_resp_cb_t resp_cb,
                       uint32_t timeout_ms,
                       emda_at_timeout_cb_t timeout_cb);

/* ---- URC ---- */

/**
 * @brief 注册一张 URC 表
 *
 * 模组类在 init 里调用一次即可。表内 prefix/seg_len/handler 都按
 * 静态生命周期使用，不做拷贝。
 *
 * @param table URC 描述数组
 * @param count 数组元素个数
 * @return EMDA_EOK / EMDA_EINVAL / EMDA_ENOMEM(表满)
 */
int emda_at_urc_attach(const emda_urc_t *table, uint32_t count);

/* ---- 状态查询 ---- */

/**
 * @brief 事务层是否可发起新指令
 *
 * 条件：状态机 IDLE、指令队列为空、且缓冲头部不是"半条 URC"。
 * 最后一条很关键——不能在半条 URC 还没收全时插进一条新指令。
 *
 * @return 1=可发起, 0=忙
 */
int emda_at_ready(void);

/**
 * @brief 获取当前 AT 状态机状态(调试用)
 */
emda_at_state_t emda_at_state_get(void);

/**
 * @brief 获取待处理指令队列长度(调试用)
 */
uint8_t emda_at_queue_count(void);

/**
 * @brief 获取接收环形缓冲里未消费的字节数(调试用)
 */
uint16_t emda_at_rx_count(void);

#endif /* EMDA_AT_H */
