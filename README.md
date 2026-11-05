# EMDA (Embedded Module Device AT) —— 通用 AT 模组组件

> 版本：0.3 ｜ 更新日期：2026-11-06
> 协议：Apache License 2.0

EMDA 是一个**可移植、可复用、非阻塞**的 AT 模组组件，结构参照 RT-Thread 的 `at_device`
（AT 事务核心 / 模组设备层 / socket 网络层 三层分工），但**不依赖 RTOS 任务与阻塞等待**，
裸机、FreeRTOS、RT-Thread 都能用同一套代码。

---

## 目录

- [一、为什么这么分层](#一为什么这么分层)
- [二、仓库结构](#二仓库结构)
- [三、架构总览](#三架构总览)
- [四、四层各自的职责](#四四层各自的职责)
- [五、核心机制](#五核心机制)
- [六、API 速查](#六api-速查)
- [七、移植指南](#七移植指南)
- [八、新增一种模组](#八新增一种模组)
- [九、配置项](#九配置项)
- [十、主机测试](#十主机测试)
- [十一、开源协议](#十一开源协议)

---

## 一、为什么这么分层

`at_device` 的做法是把一件事拆成两半，EMDA 沿用了这个划分：

| 面 | 关心什么 | at_device 的位置 | EMDA 的位置 |
| --- | --- | --- | --- |
| **控制面**（模组监控） | 上电、SIM、驻网、信号、掉线重连 | `at_device.c` + `class/xxx/at_device_xxx.c` | `emda_device.c` + `class/emda_class_xxx.c` 的 `dev_ops` |
| **数据面**（网络通信） | 建链、发数据、收数据、断链 | `at_socket.c` + `class/xxx/at_socket_xxx.c` | `emda_socket.c` + `class/emda_class_xxx.c` 的 `sock_ops` |
| 两者共同的底座 | 一条指令怎么发、响应怎么收、URC 怎么分发 | `at_client.c`（AT 包） | `emda_at.c` |

好处很直接：

- **换模组只改 class 文件**，业务代码和 socket 层一行都不用动；
- **监控与通信互不干扰**：巡检指令和业务指令共用同一条 AT 管道，但由队列串行化；
- **socket 层不认识 AT 指令**，它只管缓冲和状态，AT 报文全在 class 里；
- 控制面/数据面可以分别裁剪（不要网络通信时，只编译 `emda_at` + `emda_device`）。

---

## 二、仓库结构

```
Embedded-Devices-AT-Commands/
├── source/
│   ├── emda_def.h              公共基础：tick类型 / 返回码 / 日志宏 / socket类型
│   ├── emda_config.h           配置中心：心跳、缓冲区、容量、节奏超时
│   ├── emda_port.h/.c          移植层：心跳、串口、临界区、互斥锁
│   ├── emda_at.h/.c            AT 事务核心（对应 at_client）
│   ├── emda_device.h/.c        设备层·控制面（对应 at_device + 设备类）
│   ├── emda_socket.h/.c        网络层·数据面（对应 at_socket）
│   └── class/
│       ├── emda_class_ml307a.h
│       └── emda_class_ml307a.c 参考模组类：中移 ML307A（4G Cat.1）
├── example/
│   ├── emda_demo.c             入门示例（裸机 / FreeRTOS / RT-Thread 三种驱动）
│   └── emda_freertos_monitor.c 实际工程示例：存活/SIM/IP/MQTT 监控 + 断线重连
├── test/
│   ├── fake_modem.h/.c         主机侧模拟模组（实现 emda_port 串口接口）
│   └── test_main.c             冒烟测试：监控阶梯 / 建链 / 收发 / URC / 超时
├── document/                   模组手册与旧版设计说明
├── Makefile
├── build.ps1 / build.cmd
├── LICENSE
└── README.md
```

---

## 三、架构总览

```
+--------------------------------------------------------------------------+
|                          业务层（使用者）                                 |
|  emda_device_register/start    emda_socket_*     emda_device_exec        |
+-----------------+--------------------+-------------------+---------------+
                  |                    |                   |
+-----------------v--------------------v-------------------v---------------+
| emda_device.c          控制面：监控 Map 调度 + 网络状态机                  |
|   UNKNOWN->INIT->REGISTERING->REGISTERED->READY / LOST / FAILED          |
+-----------------+------------------------------------+-------------------+
                  | 提交监控指令                        | 调用 sock_ops
+-----------------v------------------+   +-------------v-------------------+
| emda_at.c         AT 事务核心       |   | emda_socket.c    数据面          |
|  指令队列 · 事务状态机 · 环形缓冲    |   |  句柄 · 收发缓冲 · 异步事件      |
|  URC 表(前缀+长度) · 两段式发送     |   +-------------+-------------------+
+-----------------+------------------+                 |
                  |                                    | 模组类 sock_ops
                  |   +--------------------------------+------------------+
                  |   | class/emda_class_xxx.c                            |
                  +-->|  dev_ops : 巡检指令表 + 状态解析                   |
                      |  sock_ops: MIPOPEN / MIPSEND / MIPCLOSE / URC     |
                      +------------------------+--------------------------+
                                               |
+----------------------------------------------v--------------------------+
| emda_port.c   移植层：心跳 · 串口 · 临界区 · 互斥锁                      |
+----------------------------------------------+--------------------------+
                                               |
                              裸机 / FreeRTOS / RT-Thread
```

**依赖方向是单向的**：

```
emda_socket.h ──► emda_device.h ──► emda_at.h ──► emda_port.h ──► emda_config.h ──► emda_def.h
```

---

## 四、四层各自的职责

### 4.1 `emda_at`：AT 事务核心

只做一件事——把「一条 AT 指令 + 它的响应」跑完。

- **指令队列**：业务/设备层提交的指令按 FIFO 串行执行，任一时刻只有一条在途；
- **事务状态机**：`IDLE → SEND → WAITING → RESP_OK / TIMEOUT`，超时统一兜底；
- **按内容定界**：接收环形缓冲里按「提示符 / URC 前缀+长度 / 文本行 CRLF」切分，
  不依赖时间窗，也不依赖串口分片边界；
- **URC 表**：`前缀 + 段长回调 + 处理函数`，整段到齐才回调，二进制不参与文本匹配；
- **两段式发送**：`AT+MIPSEND=<id>,<len>` → 等 `>` → 送原始数据 → 等 `OK`。

### 4.2 `emda_device`：控制面（模组监控）

- **模组类注册**：`emda_device_register(&emda_class_ml307a, NULL)`；
- **监控 Map**：每条巡检指令注册为 `emda_monitor_t`（指令 + 状态过滤 + 间隔 + 回调），
  每轮挑「状态匹配 + 已到期 + 最久未执行」的一条提交，保证公平不饿死；
- **网络状态机**：`UNKNOWN / INIT / REGISTERING / REGISTERED / READY / LOST / BUSY / FAILED`，
  状态由巡检回调推进，监控项只在对应状态下才调度；
- **业务临时指令**：`emda_device_exec()` 直接进 AT 队列，优先于巡检执行。

同一条 AT 指令可以在不同状态下注册多次，绑定不同的解析回调：

| 指令 | 监控项名 | 状态 | 间隔 | 用途 |
| --- | --- | --- | --- | --- |
| `AT+CEREG?` | `cereg` | REGISTERING | 3s | 寻网阶段判断是否驻网成功 |
| `AT+CEREG?` | `cereg_poll` | READY | 30s | 运行期巡检是否掉网 |

### 4.3 `emda_socket`：数据面（网络通信）

- socket 句柄管理（`EMDA_SOCK_MAX` 路，对应模组的 connect_id）；
- 每路一个接收环形缓冲，URC 收到数据后由模组类调用 `emda_socket_input()` 写入；
- 每路一个待发送缓冲，业务 `emda_socket_send()` 立即返回，管道空闲时自动下发；
- 事件回调：`CONNECTED / CONNECT_FAILED / SENT / SEND_FAILED / CLOSED`。

与阻塞式 socket 的差别：`connect/send` 是**发起**语义，结果靠事件回调。业务不要死等。

### 4.4 `class/emda_class_xxx`：具体模组

一个文件描述一种模组，包含两面：

```c
static const emda_device_ops_t  s_ml307a_dev_ops  = { .init, .control };
static const emda_socket_ops_t  s_ml307a_sock_ops = { .open, .send, .close };

const emda_device_class_t emda_class_ml307a = {
    .name = "ml307a",
    .dev_ops = &s_ml307a_dev_ops,
    .sock_ops = &s_ml307a_sock_ops,
    .sock_num = EMDA_SOCK_MAX,
};
```

ML307A 的状态阶梯（全部由监控项驱动，没有一行阻塞代码）：

```
UNKNOWN --ATE0--> INIT --AT+CPIN?--> REGISTERING --AT+CEREG?--> REGISTERED
        --AT+CGATT?--> READY --(CSQ/CEREG 巡检)--> LOST --AT--> INIT
```

数据面命令依据 `document/ML307A/` 手册：

```
AT+MIPOPEN=<id>,"TCP",<addr>,<port>,60,0   ->  URC +MIPOPEN:<id>,0
AT+MIPSEND=<id>,<len>  ->  ">"  ->  原始数据  ->  +MIPSEND / OK
AT+MIPCLOSE=<id>                           ->  OK / URC +MIPCLOSE:<id>
收到数据: +MIPURC: "rtcp",<id>,<len>,<data>
```

---

## 五、核心机制

### 5.1 心跳与轮询

```c
/* 心跳：提供时间基准，周期 = EMDA_HEARTBEAT_PERIOD_MS */
emda_tick_t emda_heartbeat(void);

/* 顶层轮询：AT事务 -> socket发送 -> 设备巡检 */
void emda_poll(void);
```

任务模式下顺序固定为 `poll → heartbeat → delay`：

```c
while (1) {
    emda_poll();                                   /* 1. 读当前tick，推进各层 */
    emda_heartbeat();                              /* 2. 自增tick，为下一轮准备 */
    vTaskDelay(pdMS_TO_TICKS(EMDA_HEARTBEAT_PERIOD_MS));  /* 3. 延时与心跳同周期 */
}
```

裸机模式下心跳放定时器中断，`emda_poll()` 放主循环。

### 5.2 接收定界：按内容，不按时间

串口字节流先进环形缓冲，每轮轮询在缓冲头部依次尝试：

| 顺序 | 判定 | 条件 | 处理 |
| --- | --- | --- | --- |
| a | 提示符 | 出现 `>` 等两段式发送的提示符 | 立刻发送原始数据并消费该段 |
| b | URC | 前缀匹配 + `seg_len` 算出整段长度 | 整段到齐才回调，未到齐就等 |
| c | 响应行 | 缓冲里出现 `\n` | 累积进响应文本缓冲，交给 `resp_cb` |
| d | 都不满足 | — | 原样保留，下一轮继续 |

三条关键性质：

1. **前缀命中但整段未到齐时直接返回等待**，半条 URC 绝不会被当成文本行处理；
2. **二进制由 `seg_len` 精确圈定**，payload 里的 `\r\n`、`0x00`、甚至伪造的
   `+MIPCLOSE: 0` 都只是普通数据，不参与任何文本匹配；
3. **与串口分片边界无关**：一次给 1 字节还是 2048 字节，解析结果一致。

代价是环形缓冲必须能装下一条最长的 URC，因此 `EMDA_RX_BUFFER_SIZE` 默认 2048
（ML307A 单条最大 1460 字节数据 + 约 20 字节 header）。若模组单包更大，
调大该值；若 RAM 紧张，可改成分块模式（`seg_len` 只圈 header，payload 分批喂）。

### 5.3 调度优先级

`emda_poll()` 内部顺序决定了优先级：

1. `emda_at_poll()`：收数据、解析 URC/响应、推进事务（可能刚刚腾空管道）；
2. `emda_socket_poll()`：有待发数据且管道空闲 → 下发（**业务优先**）；
3. `emda_device_poll()`：管道仍空闲 → 挑一条到期巡检指令（**监控兜底**）。

因此巡检永远不会把业务数据饿死。

### 5.4 异步事务与超时

- 每条指令带 `resp_cb` 与 `timeout_cb`；
- `resp_cb` 每收到一整行都会被调用，看到的是**累积响应文本**，可以跨行解析；
- 任一帧返回 `EMDA_EOK` 即结束事务，进入 50ms 冷却（冷却只限制下一条发送，不暂停接收）；
- 超时则调 `timeout_cb`，事务复位，后续指令继续执行。

### 5.5 两段式发送（`>` 提示符）

```c
emda_at_submit_raw("AT+MIPSEND=0,5\r\n", ">", data, 5,
                   send_cb, 10000, send_timeout_cb);
```

AT 核心先发指令，在接收缓冲里发现提示符后立刻把原始数据写出去，再继续等 `OK`。
原始数据会拷贝进 `EMDA_TX_PAYLOAD_SIZE` 大小的内部缓冲，调用后即可释放入参。

---

## 六、API 速查

### 设备层

| API | 说明 |
| --- | --- |
| `emda_device_register(cls, user_data)` | 绑定模组类 |
| `emda_device_start()` | 初始化 AT 核心 + 调用模组类 `init` |
| `emda_poll()` | 顶层轮询（必须高频调用） |
| `emda_device_control(cmd, arg)` | 模组私有控制命令 |
| `emda_device_exec(cmd, cb, timeout, timeout_cb)` | 提交业务临时指令 |
| `emda_monitor_add(item)` | 追加一条监控项（模组类内部用） |
| `emda_monitor_set_cb(name, cb, timeout_cb)` | 运行时替换监控项回调 |
| `emda_net_state_get() / _set()` | 读取/推进网络状态 |

### socket 层

| API | 说明 |
| --- | --- |
| `emda_socket_create(type, cb)` | 申请 socket，返回句柄 |
| `emda_socket_connect(sock, host, port)` | 发起连接（异步） |
| `emda_socket_send(sock, data, len)` | 缓冲 + 异步发送 |
| `emda_socket_recv(sock, buf, len)` | 从接收缓冲取数据（不阻塞） |
| `emda_socket_close(sock)` | 关闭连接 |
| `emda_socket_state_get(sock)` | 查询状态 |

### AT 核心（一般业务不直接用）

| API | 说明 |
| --- | --- |
| `emda_at_init()` | 复位状态机 + 初始化串口 |
| `emda_at_poll()` | 推进事务（`emda_poll` 已包含） |
| `emda_at_submit(cmd, cb, timeout, timeout_cb)` | 提交指令 |
| `emda_at_submit_raw(...)` | 两段式提交 |
| `emda_at_urc_attach(table, count)` | 注册 URC 表（前缀+段长+处理函数） |
| `emda_at_ready()` | 管道是否可发起新指令 |
| `emda_at_rx_count()` | 接收缓冲未消费字节数（调试） |

### 回调类型

```c
typedef int (*emda_at_resp_cb_t)   (uint8_t *buffer, uint16_t len); /* EOK=解析成功 */
typedef int (*emda_at_timeout_cb_t)(char *cmd);                     /* EOK=已处理  */
typedef int (*emda_urc_cb_t)       (const uint8_t *data, uint16_t len); /* 完整一条 URC */
typedef int (*emda_urc_seg_cb_t)   (const uint8_t *buf, uint16_t avail); /* 算整段长度 */
typedef void (*emda_sock_event_cb_t)(int sock, emda_sock_event_t evt);
```

URC 表项与模组类里的用法：

```c
static const emda_urc_t s_ml307a_urc[] = {
    /* 带裸数据：seg_len 从 header 解析长度，整段到齐才回调 */
    {"+MIPURC: \"rtcp\",", s_rtcp_seg_len, s_rtcp_payload},
    {"+MIPURC: \"rudp\",", s_rtcp_seg_len, s_rtcp_payload},
    /* 纯文本行：seg_len = NULL 表示"取到 CRLF 为止的一行" */
    {"+MIPOPEN:", NULL, s_urc_mipopen},
    {"+MIPCLOSE:", NULL, s_urc_mipclose},
    {"+MIPURC:", NULL, s_urc_mipurc_other},
};
```

表中**顺序即优先级**，带长度的条目必须排在通用前缀之前。

### 返回码

| 宏 | 值 | 含义 |
| --- | --- | --- |
| `EMDA_EOK` | 0 | 成功 |
| `EMDA_ERROR` | -1 | 通用失败 |
| `EMDA_ETIMEOUT` | -2 | 超时 |
| `EMDA_EINVAL` | -3 | 参数非法 |
| `EMDA_EBUSY` | -4 | 有在途事务 |
| `EMDA_ENOMEM` | -5 | 队列满/缓冲不够 |
| `EMDA_ENOSYS` | -6 | 模组类不支持 |
| `EMDA_EIO` | -7 | 链路/硬件失败 |

---

## 七、移植指南

只需实现 `emda_port.c` 里的 **9 个函数**（心跳/时间 3 个已实现）。

**先记住一条铁律：port 层只搬运字节，不做任何定界。**
不要按 CRLF 切、不要按 DMA 空闲切、不要认 URC，有多少字节给多少字节；
消息边界全部由 `emda_at.c` 判定。

| 类别 | 接口 | 状态 |
| --- | --- | --- |
| 心跳 | `emda_heartbeat` / `emda_tick_get` / `emda_tick_from_ms` | 已实现 |
| 串口 | `emda_port_uart_init` / `emda_port_uart_send` / `emda_port_uart_read` | 待填 |
| 临界区 | `emda_port_enter_critical` / `emda_port_exit_critical` | 按宏切换 |
| 互斥锁 | `emda_port_mutex_create/destroy/lock/unlock` | 待填（裸机可留空实现） |

步骤：

1. 把 `source/` 整个加入工程（含 `class/`），头文件路径加 `source/` 和 `source/class/`；
2. 改 `emda_config.h`：心跳周期、缓冲区、socket 数量；
3. 实现 `emda_port.c` 的串口/临界区/互斥锁；
4. `emda_device_register(&emda_class_ml307a, NULL)` → `emda_device_start()`；
5. 主循环里 `emda_poll()` + `emda_heartbeat()`。

裸机移植参考（STM32 HAL）：

```c
int emda_port_uart_init(void) {
    HAL_UARTEx_ReceiveToIdle_DMA(&huart1, rx_dma, sizeof(rx_dma));
    return EMDA_EOK;
}
int emda_port_uart_send(const uint8_t *data, uint16_t len) {
    return (HAL_UART_Transmit(&huart1, (uint8_t *)data, len, 100) == HAL_OK)
           ? EMDA_EOK : EMDA_EIO;
}
int emda_port_uart_read(uint8_t *buf, uint16_t size, uint32_t timeout_ms) {
    (void)timeout_ms;
    /* 从空闲中断填的字节 FIFO 里搬，有多少给多少，不切帧 */
    return fifo_read(buf, size);
}
```

---

## 八、新增一种模组

以 ESP8266（`AT+CIPSTART/CIPSEND/CIPCLOSE`）为例：

1. 复制 `source/class/emda_class_ml307a.{h,c}` 为 `emda_class_esp8266.{h,c}`；
2. 改 `init()` 里的监控项表：状态阶梯、指令字符串、解析回调；
3. 改三个 `sock_ops`：`open/send/close` 换成对应 AT 指令；
4. 改 URC 表：`+IPD,<len>:` 这类用 `seg_len` 从 header 解析长度（长度段整段到齐才回调），
   `CLOSED`/`WIFI GOT IP` 这类纯文本行 `seg_len` 传 NULL；数据交给 `emda_socket_input()`，
   状态变化交给 `emda_socket_report()`；
5. 业务侧只改一行：`emda_device_register(&emda_class_esp8266, NULL)`。

**新模组的检查清单**：

- [ ] 指令是否带 `\r\n`
- [ ] 建链结果是同步 `OK` 还是异步 URC（异步就用 class 内部的 FIFO 对号入座）
- [ ] 发送是否需要 `>` 提示符（需要就用 `emda_at_submit_raw`）
- [ ] 接收数据是内联 URC 还是缓存读取（内联就在 URC 表里给 `seg_len`，按长度圈定整段）
- [ ] 带长度的表项是否排在通用前缀之前（顺序即优先级）
- [ ] 掉网 URC 是否要立即把状态推到 `LOST`

---

## 九、配置项

全部在 `emda_config.h`，均 `#ifndef` 保护，可在编译选项覆盖。

| 配置项 | 默认 | 说明 |
| --- | --- | --- |
| `EMDA_HEARTBEAT_PERIOD_MS` | 1 | 心跳周期（ms） |
| `EMDA_CRITICAL_USE_DISABLE_IRQ` | 启用 | 临界区用关中断实现 |
| `EMDA_CRITICAL_USE_NONE` | 关闭 | 临界区空实现 |
| `EMDA_CMD_BUFFER_SIZE` | 256 | 单条 AT 指令最大长度 |
| `EMDA_RX_BUFFER_SIZE` | 2048 | 接收环形缓冲（需容纳最长一条 URC） |
| `EMDA_RESP_BUFFER_SIZE` | 1024 | 单条响应累积文本缓冲 |
| `EMDA_RX_READS_PER_POLL` | 4 | 单次轮询最多读几批串口数据 |
| `EMDA_TX_PAYLOAD_SIZE` | 512 | 两段式发送的原始数据上限 |
| `EMDA_MONITOR_MAX` | 16 | 监控项容量 |
| `EMDA_CMD_QUEUE_SIZE` | 8 | 指令队列容量 |
| `EMDA_URC_TABLE_SIZE` | 8 | URC 回调表容量 |
| `EMDA_NAME_LEN` | 24 | 监控项/URC 名称长度 |
| `EMDA_SOCK_MAX` | 4 | socket 路数（受模组 connect_id 限制） |
| `EMDA_SOCK_RX_BUFFER_SIZE` | 512 | 单路接收缓冲 |
| `EMDA_SOCK_TX_BUFFER_SIZE` | 256 | 单路发送缓冲 |
| `EMDA_RESP_COOLDOWN_MS` | 50 | RESP_OK 后下一条发送的冷却 |
| `EMDA_LOOP_INTERVAL_MS` | 5 | AT 状态机最小推进间隔 |
| `EMDA_AT_DEFAULT_TIMEOUT_MS` | 2000 | 提交时 timeout=0 的默认值 |
| `EMDA_UART_READ_TIMEOUT_MS` | 10 | 串口读取默认超时（供阻塞式移植使用） |
| `EMDA_MUTEX_WAIT_TIMEOUT_MS` | 1000 | 互斥锁等待超时 |

日志默认关闭，需要时定义：

```c
#define EMDA_LOG_ENABLE 1
#define EMDA_LOG(fmt, ...) rt_kprintf(fmt, ##__VA_ARGS__)
```

---

## 十、主机测试

`test/` 里有一个**模拟模组**（实现 `emda_port` 串口接口，用脚本应答 AT 指令，
还能限制每次读的字节数来模拟 DMA 分片），可以在 PC 上把整条链路跑一遍：

- 监控阶梯 → 建链 → 发送 → 关闭 → 超时
- **payload 含 `\r\n` 且恰好被分片切断**
- **payload 里伪造 `+MIPCLOSE: 0` 不能误关连接**
- **payload 含 `0x00` 不截断**
- **单条 1460 字节 URC 不卡死解析器**
- **文本响应拆成 2 字节一片仍能匹配 `OK`**

```bash
make test
# 或
.\build test
```

期望输出：

```
[ OK ] register ml307a class
[ OK ] device start
[ OK ] monitor ladder reaches READY (AT/CPIN/CEREG/CGATT)
...
==== PASS (0 failures) ====
```

改代码后先跑这个测试，能挡掉大部分分层/时序引入的低级错误。

---

## 十一、开源协议

本项目采用 **Apache License 2.0**，完整文本见 [LICENSE](LICENSE)。

```
Copyright 2026 Zorian (1551769443@qq.com)
Licensed under the Apache License, Version 2.0
```
