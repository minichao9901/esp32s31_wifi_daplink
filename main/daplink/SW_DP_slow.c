/*
 * SW_DP_slow.c —— SWD 时序层【慢速兜底版】：普通 GPIO 寄存器 + PIN_DELAY_SLOW 延时循环
 *
 * 逻辑来自 ARM CMSIS-DAP / DAPLink 的 SW_DP.c（Apache-2.0），
 * 位操作原语改写成本工程的 PIN_xxx 宏（→ ESP32 普通 GPIO 寄存器）。
 *
 * 对外提供 DAP.h 里声明的 4 个函数：
 *   SWJ_Sequence() —— 随便打 N 个时钟（用于 line reset、JTAG-to-SWD 切换序列）
 *   SWD_Sequence() —— 双向序列
 *   SWD_Write()    —— 一次 32bit 写 + ACK
 *   SWD_Read()     —— 一次 32bit 读 + ACK
 *
 * ⚠️ 第一版全部是“慢速”实现：每个半周期靠 PIN_DELAY_SLOW 烧周期，
 *    实测上限大约几 MHz，足够 pyOCD/OpenOCD 正常用。
 *    后面换 dedicated GPIO + 展开汇编再冲 10~20 MHz。
 */

#include <string.h>
#include "DAP_config.h"
#include "DAP.h"

/* 慢速延时：DAP_Data.clock_delay 由 DAP.c 的 Set_Clock_Delay() 按 CPU_CLOCK 算好 */
#define PIN_DELAY()         PIN_DELAY_SLOW(DAP_Data.clock_delay)

#define PIN_SWCLK_SET       PIN_SWCLK_TCK_SET
#define PIN_SWCLK_CLR       PIN_SWCLK_TCK_CLR

/* 读的时候 CLK 拉低之后多等几个延时单元，等目标把 SWDIO 驱动稳 */
#define SW_READ_BIT_DELAYS  3U

static inline uint32_t GetParity(uint32_t data)
{
    data ^= data >> 16;
    data ^= data >> 8;
    data ^= data >> 4;
    data &= 0x0F;
    return (0x6996U >> data) & 1U;
}

/** 一个空时钟周期 */
static inline void SW_CLOCK_CYCLE(void)
{
    PIN_SWCLK_CLR();
    PIN_DELAY();
    PIN_SWCLK_SET();
    PIN_DELAY();
}

/** 主机驱动一位并打一个时钟（数据在 CLK 下降沿前建立） */
static inline void SW_WRITE_BIT(uint32_t bit)
{
    PIN_SWDIO_OUT(bit);
    PIN_SWCLK_CLR();
    PIN_DELAY();
    PIN_SWCLK_SET();
    PIN_DELAY();
}

/** 读一位：CLK 拉低 → 等目标驱动 → 采样 → CLK 拉高 */
static inline uint32_t SW_READ_BIT(void)
{
    uint32_t bit;
    PIN_SWCLK_CLR();
    for (uint32_t i = 0; i < SW_READ_BIT_DELAYS; i++) {
        PIN_DELAY();
    }
    bit = PIN_SWDIO_IN();
    PIN_SWCLK_SET();
    PIN_DELAY();
    return bit;
}

/**
 * @brief SWJ Sequence（任意位序列，主机单向输出）
 */
ATTR_RAMFUNC void SWJ_Sequence_slow(uint32_t count, const uint8_t *data)
{
    uint32_t pack_bytes = count / 8U;
    uint32_t tail_bits = count % 8U;
    uint8_t val;

    for (uint32_t i = 0; i < pack_bytes; i++) {
        val = *data++;
        for (uint32_t j = 0; j < 8U; j++) {
            SW_WRITE_BIT(val);
            val >>= 1;
        }
    }

    val = *data;
    for (uint32_t i = 0; i < tail_bits; i++) {
        SW_WRITE_BIT(val);
        val >>= 1;
    }
}

/**
 * @brief SWD Sequence（双向，DIN 位置 1 表示要读回）
 */
ATTR_RAMFUNC void SWD_Sequence_slow(uint32_t info, const uint8_t *swdo, uint8_t *swdi)
{
    uint32_t val, bit, n, k;

    n = info & SWD_SEQUENCE_CLK;
    if (n == 0U) {
        n = 64U;
    }

    if (info & SWD_SEQUENCE_DIN) {
        while (n) {
            val = 0U;
            for (k = 8U; k && n; k--, n--) {
                bit = SW_READ_BIT();
                val >>= 1;
                val |= bit << 7;
            }
            val >>= k;
            *swdi++ = (uint8_t)val;
        }
    } else {
        while (n) {
            val = *swdo++;
            for (k = 8U; k && n; k--, n--) {
                SW_WRITE_BIT(val);
                val >>= 1;
            }
        }
    }
}

/**
 * @brief 一次 SWD 32bit 写
 * @param request A[3:2] RnW APnDP
 * @param data    要写的数据
 * @return ACK[2:0]（DAP_TRANSFER_OK / WAIT / FAULT / ERROR）
 */
ATTR_RAMFUNC uint8_t SWD_Write_slow(uint32_t request, uint32_t *data)
{
    uint8_t header = 0x81U | ((request & 0x0FU) << 1) |
                     (((request ^ (request >> 1) ^ (request >> 2) ^ (request >> 3)) & 1U) << 5);
    uint32_t ack, bit, val, parity, n;

    /* ---- Packet Request：8 bit header，LSB 先 ---- */
    for (n = 8U; n; n--) {
        SW_WRITE_BIT(header);
        header >>= 1;
    }

    /* ---- Turnaround：主机释放 SWDIO ---- */
    PIN_SWDIO_OUT_DISABLE();
    for (n = DAP_Data.swd_conf.turnaround; n; n--) {
        SW_CLOCK_CYCLE();
    }

    /* ---- Acknowledge：读 3 bit ---- */
    bit = SW_READ_BIT();
    ack = bit << 0;
    bit = SW_READ_BIT();
    ack |= bit << 1;
    bit = SW_READ_BIT();
    ack |= bit << 2;

    if (ack == DAP_TRANSFER_OK) {
        /* 🚨 必须先认 RnW 位：ARM 的 DAP.c 会用 SWD_Write(DP_RDBUFF | DAP_TRANSFER_RnW, &data)
         * 来"借写调用读"（它靠这一招统一数据相位处理）。
         * 老版本这里无条件当写，把 *data 里**残留的上一个写传输的值**吐了回去 ——
         * 症状：批量读里第一个数据槽 = 刚写进 TAR 的地址（`0xE0042000 -> 0xE0042000`），
         * 进而 pyOCD 读核寄存器失败（收到 DCRSR 的 regsel 而不是 DHCSR）。
         * 见 tools\cmsis_dap_raw.py 的原始字节证据。 */
        if ((request & DAP_TRANSFER_RnW) != 0U) {
            /* ---- 读数据（目标驱动，主机保持高阻） ---- */
            val = 0U;
            for (n = 32U; n; n--) {
                bit = SW_READ_BIT();
                val >>= 1;
                val |= bit << 31;
            }
            bit = SW_READ_BIT();            /* Parity */
            parity = GetParity(val);
            if ((parity ^ bit) & 1U) {
                ack = DAP_TRANSFER_ERROR;
            }
            if (data) {
                *data = val;
            }

            for (n = DAP_Data.swd_conf.turnaround; n; n--) {
                SW_CLOCK_CYCLE();
            }
            PIN_SWDIO_OUT_ENABLE();

            n = DAP_Data.transfer.idle_cycles;
            if (n) {
                PIN_SWDIO_OUT(0U);
                for (; n; n--) {
                    SW_CLOCK_CYCLE();
                }
            }
            PIN_SWDIO_OUT(1U);
            return (uint8_t)ack;
        }

        /* ---- 写数据 ---- */
        /* 再等一个 turnaround，然后主机接管总线写数据 */
        for (n = DAP_Data.swd_conf.turnaround; n; n--) {
            SW_CLOCK_CYCLE();
        }
        PIN_SWDIO_OUT_ENABLE();

        val = *data;
        parity = GetParity(val);
        for (n = 32U; n; n--) {
            SW_WRITE_BIT(val);          /* WDATA[0:31] */
            val >>= 1;
        }
        SW_WRITE_BIT(parity);           /* Parity */

        /* Idle cycles */
        n = DAP_Data.transfer.idle_cycles;
        if (n) {
            PIN_SWDIO_OUT(0U);
            for (; n; n--) {
                SW_CLOCK_CYCLE();
            }
        }
        PIN_SWDIO_OUT(1U);
        return (uint8_t)ack;
    }

    if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {
        for (n = DAP_Data.swd_conf.turnaround; n; n--) {
            SW_CLOCK_CYCLE();
        }
        PIN_SWDIO_OUT_ENABLE();
        if (DAP_Data.swd_conf.data_phase) {
            PIN_SWDIO_OUT(0U);
            for (n = 32U + 1U; n; n--) {
                SW_CLOCK_CYCLE();       /* dummy WDATA + parity */
            }
        }
        PIN_SWDIO_OUT(1U);
        return (uint8_t)ack;
    }

    /* 协议错：退掉整个数据相位 */
    for (n = DAP_Data.swd_conf.turnaround + 32U + 1U; n; n--) {
        SW_CLOCK_CYCLE();
    }
    PIN_SWDIO_OUT_ENABLE();
    PIN_SWDIO_OUT(1U);
    return (uint8_t)ack;
}

/**
 * @brief 一次 SWD 32bit 读
 */
ATTR_RAMFUNC uint8_t SWD_Read_slow(uint32_t request, uint32_t *data)
{
    uint8_t header = 0x81U | ((request & 0x0FU) << 1) |
                     (((request ^ (request >> 1) ^ (request >> 2) ^ (request >> 3)) & 1U) << 5);
    uint32_t ack, bit, val, parity, n;

    for (n = 8U; n; n--) {
        SW_WRITE_BIT(header);
        header >>= 1;
    }

    PIN_SWDIO_OUT_DISABLE();
    for (n = DAP_Data.swd_conf.turnaround; n; n--) {
        SW_CLOCK_CYCLE();
    }

    bit = SW_READ_BIT();
    ack = bit << 0;
    bit = SW_READ_BIT();
    ack |= bit << 1;
    bit = SW_READ_BIT();
    ack |= bit << 2;

    if (ack == DAP_TRANSFER_OK) {
        /* 读数据（目标驱动，已在高阻态） */
        val = 0U;
        for (n = 32U; n; n--) {
            bit = SW_READ_BIT();
            val >>= 1;
            val |= bit << 31;
        }
        bit = SW_READ_BIT();            /* Parity */
        parity = GetParity(val);
        if ((parity ^ bit) & 1U) {
            ack = DAP_TRANSFER_ERROR;
        }
        if (data) {
            *data = val;
        }

        for (n = DAP_Data.swd_conf.turnaround; n; n--) {
            SW_CLOCK_CYCLE();
        }
        PIN_SWDIO_OUT_ENABLE();

        n = DAP_Data.transfer.idle_cycles;
        if (n) {
            PIN_SWDIO_OUT(0U);
            for (; n; n--) {
                SW_CLOCK_CYCLE();
            }
        }
        PIN_SWDIO_OUT(1U);
        return (uint8_t)ack;
    }

    if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {
        if (DAP_Data.swd_conf.data_phase) {
            for (n = 32U + 1U; n; n--) {
                SW_CLOCK_CYCLE();       /* dummy RDATA + parity */
            }
        }
        for (n = DAP_Data.swd_conf.turnaround; n; n--) {
            SW_CLOCK_CYCLE();
        }
        PIN_SWDIO_OUT_ENABLE();
        PIN_SWDIO_OUT(1U);
        return (uint8_t)ack;
    }

    for (n = DAP_Data.swd_conf.turnaround + 32U + 1U; n; n--) {
        SW_CLOCK_CYCLE();
    }
    PIN_SWDIO_OUT_ENABLE();
    PIN_SWDIO_OUT(1U);
    return (uint8_t)ack;
}
