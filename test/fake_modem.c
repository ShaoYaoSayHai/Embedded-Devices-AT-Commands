/**
 * @file fake_modem.c
 * @brief 主机侧模拟模组实现(同时实现 emda_port 抽象层)
 *
 * 仿真范围(足够覆盖分层逻辑)：
 *   ATE0 / AT / AT+CPIN? / AT+CEREG? / AT+CGATT? / AT+CSQ
 *   AT+MIPOPEN=<id>,"TCP",...   -> OK + URC +MIPOPEN:<id>,0
 *   AT+MIPSEND=<id>,<len>       -> '>' + 收原始数据 + +MIPSEND/OK
 *   AT+MIPCLOSE=<id>            -> OK + URC +MIPCLOSE:<id>
 *   AT+NORESP                   -> 不应答(用于验证超时)
 */

#include "fake_modem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emda_port.h"

#define MODEM_TX_SIZE  (4096U)
#define MODEM_CMD_SIZE (192U)
#define MODEM_PAY_SIZE (512U)

/* 模组 -> 主机 的发送队列 */
static uint8_t s_out[MODEM_TX_SIZE];
static uint16_t s_out_len = 0U;

/* 主机 -> 模组 的指令累积 */
static char s_cmd[MODEM_CMD_SIZE];
static uint16_t s_cmd_len = 0U;
static char s_last_cmd[MODEM_CMD_SIZE];

/* ">" 数据输入模式 */
static int s_payload_id = -1;
static int s_payload_need = 0;
static int s_payload_got = 0;
static uint8_t s_payload[MODEM_PAY_SIZE];

/* 心跳 */
static volatile emda_tick_t s_tick = 0U;

/* 单次读返回上限(模拟 DMA 分片)，0 = 不限 */
static uint16_t s_read_limit = 0U;

/* ======================================================================== */
/*                              内部工具                                     */
/* ======================================================================== */

static void modem_write(const char *text)
{
    uint16_t len = (uint16_t)strlen(text);

    if ((uint32_t)s_out_len + len > MODEM_TX_SIZE)
    {
        return;
    }
    memcpy(&s_out[s_out_len], text, len);
    s_out_len = (uint16_t)(s_out_len + len);
}

static void modem_write_raw(const char *data, uint16_t len)
{
    if ((uint32_t)s_out_len + len > MODEM_TX_SIZE)
    {
        return;
    }
    memcpy(&s_out[s_out_len], data, len);
    s_out_len = (uint16_t)(s_out_len + len);
}

static void s_handle_cmd(const char *cmd)
{
    strncpy(s_last_cmd, cmd, MODEM_CMD_SIZE - 1U);
    s_last_cmd[MODEM_CMD_SIZE - 1U] = '\0';

    if (strcmp(cmd, "AT") == 0 || strcmp(cmd, "ATE0") == 0)
    {
        modem_write("OK\r\n");
        return;
    }

    if (strcmp(cmd, "AT+CPIN?") == 0)
    {
        modem_write("+CPIN: READY\r\n\r\nOK\r\n");
        return;
    }

    if (strcmp(cmd, "AT+CEREG?") == 0)
    {
        modem_write("+CEREG: 0,1\r\n\r\nOK\r\n");
        return;
    }

    if (strcmp(cmd, "AT+CGATT?") == 0)
    {
        modem_write("+CGATT: 1\r\n\r\nOK\r\n");
        return;
    }

    if (strcmp(cmd, "AT+CSQ") == 0)
    {
        modem_write("+CSQ: 20,99\r\n\r\nOK\r\n");
        return;
    }

    if (strncmp(cmd, "AT+MIPOPEN=", 11) == 0)
    {
        char urc[48];
        int id = atoi(cmd + 11);

        modem_write("OK\r\n");
        (void)snprintf(urc, sizeof(urc), "+MIPOPEN: %d,0\r\n", id);
        modem_write(urc);
        return;
    }

    if (strncmp(cmd, "AT+MIPSEND=", 11) == 0)
    {
        int id = 0;
        int len = 0;

        (void)sscanf(cmd + 11, "%d,%d", &id, &len);
        s_payload_id = id;
        s_payload_need = len;
        s_payload_got = 0;
        modem_write(">"); /* 提示符，无 CRLF */
        return;
    }

    if (strncmp(cmd, "AT+MIPCLOSE=", 12) == 0)
    {
        char urc[32];
        int id = atoi(cmd + 12);

        modem_write("OK\r\n");
        (void)snprintf(urc, sizeof(urc), "+MIPCLOSE: %d\r\n", id);
        modem_write(urc);
        return;
    }

    if (strcmp(cmd, "AT+NORESP") == 0)
    {
        return; /* 故意不应答 */
    }

    modem_write("ERROR\r\n");
}

static void s_payload_done(void)
{
    char resp[64];

    (void)snprintf(resp, sizeof(resp), "+MIPSEND: %d,%d\r\nOK\r\n",
                   s_payload_id, s_payload_need);
    modem_write(resp);
    s_payload_id = -1;
    s_payload_need = 0;
    s_payload_got = 0;
}

/* ======================================================================== */
/*                              测试注入接口                                 */
/* ======================================================================== */

void fake_modem_reset(void)
{
    s_out_len = 0U;
    s_cmd_len = 0U;
    s_last_cmd[0] = '\0';
    s_payload_id = -1;
    s_payload_need = 0;
    s_payload_got = 0;
    s_tick = 0U;
    s_read_limit = 0U;
}

void fake_modem_set_read_limit(uint16_t limit)
{
    s_read_limit = limit;
}

void fake_modem_inject(const char *text)
{
    modem_write(text);
}

void fake_modem_inject_data(int id, const char *data)
{
    fake_modem_inject_data_bin(id, (const uint8_t *)data, (uint16_t)strlen(data));
}

void fake_modem_inject_data_bin(int id, const uint8_t *data, uint16_t len)
{
    char head[48];

    (void)snprintf(head, sizeof(head), "+MIPURC: \"rtcp\",%d,%u,",
                   id, (unsigned)len);
    modem_write(head);
    modem_write_raw((const char *)data, len);
    modem_write("\r\n");
}

void fake_modem_inject_data_part1(int id, const char *data, int part_len)
{
    char head[48];

    (void)snprintf(head, sizeof(head), "+MIPURC: \"rtcp\",%d,%u,",
                   id, (unsigned)strlen(data));
    modem_write(head);
    modem_write_raw(data, (uint16_t)part_len);
}

void fake_modem_inject_data_part2(const char *data_tail)
{
    modem_write(data_tail);
    modem_write("\r\n");
}

const char *fake_modem_last_cmd(void)
{
    return s_last_cmd;
}

/* ======================================================================== */
/*                           emda_port 实现                                  */
/* ======================================================================== */

emda_tick_t emda_heartbeat(void)
{
    s_tick++;
    return s_tick;
}

emda_tick_t emda_tick_get(void)
{
    return s_tick;
}

emda_tick_t emda_tick_from_ms(uint32_t ms)
{
    return (emda_tick_t)(ms / EMDA_HEARTBEAT_PERIOD_MS);
}

int emda_port_uart_init(void)
{
    return EMDA_EOK;
}

int emda_port_uart_send(const uint8_t *data, uint16_t len)
{
    uint16_t i;

    if (data == NULL)
    {
        return EMDA_EINVAL;
    }

    for (i = 0U; i < len; i++)
    {
        uint8_t ch = data[i];

        if (s_payload_id >= 0)
        {
            if (s_payload_got < (int)MODEM_PAY_SIZE)
            {
                s_payload[s_payload_got] = ch;
            }
            s_payload_got++;
            if (s_payload_got >= s_payload_need)
            {
                s_payload_done();
            }
            continue;
        }

        if (ch == '\n')
        {
            s_cmd[s_cmd_len] = '\0';
            if (s_cmd_len > 0U && s_cmd[s_cmd_len - 1U] == '\r')
            {
                s_cmd[s_cmd_len - 1U] = '\0';
            }
            if (s_cmd[0] != '\0')
            {
                s_handle_cmd(s_cmd);
            }
            s_cmd_len = 0U;
        }
        else if (s_cmd_len < (MODEM_CMD_SIZE - 1U))
        {
            s_cmd[s_cmd_len] = (char)ch;
            s_cmd_len++;
        }
    }

    return EMDA_EOK;
}

int emda_port_uart_read(uint8_t *buf, uint16_t size, uint32_t timeout_ms)
{
    uint16_t n;

    (void)timeout_ms;

    if (buf == NULL || size == 0U)
    {
        return EMDA_EINVAL;
    }
    if (s_out_len == 0U)
    {
        return 0;
    }

    n = (s_out_len < size) ? s_out_len : size;
    if (s_read_limit != 0U && n > s_read_limit)
    {
        n = s_read_limit; /* 模拟 DMA 分片：一次只给这么多 */
    }

    memcpy(buf, s_out, n);
    s_out_len = (uint16_t)(s_out_len - n);
    if (s_out_len > 0U)
    {
        memmove(s_out, &s_out[n], s_out_len);
    }
    return (int)n;
}

void emda_port_enter_critical(void) { }
void emda_port_exit_critical(void)  { }

emda_mutex_t emda_port_mutex_create(void) { return NULL; }
void emda_port_mutex_destroy(emda_mutex_t mutex) { (void)mutex; }
int emda_port_mutex_lock(emda_mutex_t mutex, uint32_t timeout_ms)
{
    (void)mutex;
    (void)timeout_ms;
    return EMDA_EOK;
}
int emda_port_mutex_unlock(emda_mutex_t mutex)
{
    (void)mutex;
    return EMDA_EOK;
}
