/*
 * DAP_config.h —— CMSIS-DAP 的“硬件适配层”，本文件为 ESP32-S31-Function-CoreBoard-1 编写
 *
 * 分层：
 *   DAP.c（ARM CMSIS-DAP 协议层，原样复用）
 *     └─ 本文件：把 PIN_xxx 宏映射到 ESP32 的 **普通 GPIO 寄存器** 读写
 *         （第一版刻意不用 dedicated/CPU GPIO，先把链路调通，后面再换）
 *
 * 引脚（J2 空闲区，用户指定）：
 *   SWCLK/TCK = GPIO47   (J2 脚 13)
 *   SWDIO/TMS = GPIO48   (J2 脚 14)
 *   nRESET    = GPIO46   (J2 脚 16)
 *
 * ⚠️ GPIO32~63 在 ESP32 里属于 **第二组寄存器**（OUT1/ENABLE1/IN1），
 *    掩码要右移 32 位。本文件的三个脚都在这一组，所以统一用 bank1 寄存器。
 */
#ifndef __DAP_CONFIG_H__
#define __DAP_CONFIG_H__

#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"

//**************************************************************************************************
/** \defgroup DAP_Config_Debug_gr CMSIS-DAP Debug Unit Information */

/// 处理器时钟频率（用于把“请求的 SWJ 频率”换算成延时循环次数）
#define CPU_CLOCK               320000000U
/// 一次 GPIO 端口写操作占用的周期数（用于 Set_Clock_Delay 里扣掉固定开销）
#define IO_PORT_WRITE_CYCLES    2U
/// 一个“慢延时单元”消耗的 CPU 周期数，必须与下面 PIN_DELAY_SLOW 的实现一致
#define DELAY_SLOW_CYCLES       4U
/// 一个“快延时单元”消耗的 CPU 周期数
#define DELAY_FAST_CYCLES       1U

//**************************************************************************************************
/** \defgroup DAP_Config_PortIO_gr CMSIS-DAP Hardware I/O Pin Access */

/// SWD 模式可用
#define DAP_SWD                 1
/// JTAG 模式：第一版不做（省掉 TDI/TDO/nTRST 三个脚和一堆代码）
#define DAP_JTAG                0
/// JTAG 扫描链设备数（DAP_JTAG=0 时无意义，DAP.c 要求必须有定义）
#define DAP_JTAG_DEV_CNT        1U
/// 默认端口模式：1 = SWD，2 = JTAG
#define DAP_DEFAULT_PORT        1U
/// 默认 SWJ 时钟 1 MHz（DAP_SWJ_Clock 命令可以改，也可以直接改这里的初值）
#define DAP_DEFAULT_SWJ_CLOCK   1000000U
/// 单包最大字节数（DAP_Info 0xFF）
///    客户端拿它当"一条命令的请求/响应上限"（tfer_max_command_size / response_size），
///    所以这个值越大，一条命令一次能搬的字越多、过 WiFi 的往返次数越少。
///    🚨 但它同时决定了响应能有多大 —— 客户端那边的 TCP 读缓冲也是 1024，
///    超过就被判 `Packet length %d too large to fit`，所以别超过 1024。
#define DAP_PACKET_SIZE         1024U
/// 允许在途的包个数（DAP_Info 0xF0）。
/// 🚨 无线版必须报 **1**：TCP 后端拿这个值决定"能不能流水线"——
///    报 ≥2 时 OpenOCD 会同时挂多条命令、用**非阻塞读**收响应，而 ESP-IDF 那份
///    OpenOCD 0.12 fork 的 TCP 后端在流水线路径上会漏读一条响应，
///    于是下一条同步命令（如 SWJ_Sequence）读到的是遗留的旧响应 →
///    `CMSIS-DAP command mismatch. Sent 0x12 received 0x5` → 之后整条流错位（实测必现）。
///    报 1 = 严格一问一答，客户端每条命令写完就等响应 → dump/load 16KB 全通。
///    代价：没有流水线（对 WiFi 这种 RTT 主导的链路本来也重叠不了多少）。
#define DAP_PACKET_COUNT        1U
/// SWO：第一版不支持
#define SWO_UART                0
#define SWO_UART_DRIVER         0
#define SWO_UART_MAX_BAUDRATE   0U
#define SWO_MANCHESTER          0
#define SWO_BUFFER_SIZE         4096U
#define SWO_STREAM              0
/// 时间戳：不支持
#define TIMESTAMP_CLOCK         0U
/// DAP_Info 里报告“带 USB COM 口”（CDC 串口桥由 dap_main.c 提供）
#define DAP_UART_USB_COM_PORT   1

/* ------------------------------ 引脚定义 ------------------------------ */
#define DAP_PIN_SWCLK           47
#define DAP_PIN_SWDIO           48
#define DAP_PIN_nRESET          46

/* 🚨 高速位时序开关（编译期）：
 *   1 = SWD 位时序走 dedicated GPIO + 展开汇编（SW_DP_fast.c，实测写 22.7 MHz）
 *   0 = 走原来的普通 GPIO + 延时循环（SW_DP_slow.c，等效 ~0.3 MHz）
 * 切换必须重编重烧：high 模式下这两个焊盘归 dedicated GPIO，
 * 普通 GPIO 写不再到得了焊盘，慢速实现会"静默失效"（周期数好看但引脚不动）。 */
#ifndef DAP_USE_DEDIC_GPIO
#define DAP_USE_DEDIC_GPIO      1
#endif

#include "dedic_swd.h"

#define DAP_SWCLK_MASK          (1U << (DAP_PIN_SWCLK - 32))
#define DAP_SWDIO_MASK          (1U << (DAP_PIN_SWDIO - 32))
#define DAP_nRESET_MASK         (1U << (DAP_PIN_nRESET - 32))

/// ATTR_RAMFUNC：DAP.c 里给关键函数加的段属性，这里映射成“放 IRAM”
#define ATTR_RAMFUNC            IRAM_ATTR

#ifndef __STATIC_INLINE
#define __STATIC_INLINE         static inline
#endif
#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE    static inline __attribute__((always_inline))
#endif

/* ------------------------------ 延时 ------------------------------ */
/** 慢延时：烧掉 delay*DELAY_SLOW_CYCLES 个 CPU 周期。
 *  循环体 = 2 条压缩指令(addi/bnez) + 2 条 nop ≈ 4 个周期，与 DELAY_SLOW_CYCLES 对齐。 */
#define PIN_DELAY_SLOW(delay)                       \
    do {                                            \
        uint32_t _dap_dly = (uint32_t)(delay);      \
        while (_dap_dly--) {                        \
            __asm__ __volatile__("nop\n nop");      \
        }                                           \
    } while (0)

/** 快延时（fast_clock 模式用） */
#define PIN_DELAY_FAST()                            \
    do {                                            \
        __asm__ __volatile__("nop");                \
    } while (0)

/* ============================ I/O 宏实现 ============================
 * 全部用 GPIO bank1 的 W1TS/W1TC（写 1 生效）寄存器，一条 store 指令搞定，
 * 不需要读-改-写，也不会干扰同组的其它引脚。
 * ==================================================================== */

/* ---- SWCLK / TCK ---- */
__STATIC_FORCEINLINE uint32_t PIN_SWCLK_TCK_IN(void)
{
    return (REG_READ(GPIO_IN1_REG) >> (DAP_PIN_SWCLK - 32)) & 1U;
}
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_SET(void)
{
    REG_WRITE(GPIO_OUT1_W1TS_REG, DAP_SWCLK_MASK);
}
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_CLR(void)
{
    REG_WRITE(GPIO_OUT1_W1TC_REG, DAP_SWCLK_MASK);
}

/* ---- SWDIO / TMS ---- */
__STATIC_FORCEINLINE uint32_t PIN_SWDIO_TMS_IN(void)
{
    return (REG_READ(GPIO_IN1_REG) >> (DAP_PIN_SWDIO - 32)) & 1U;
}
__STATIC_FORCEINLINE void PIN_SWDIO_TMS_SET(void)
{
    REG_WRITE(GPIO_OUT1_W1TS_REG, DAP_SWDIO_MASK);
}
__STATIC_FORCEINLINE void PIN_SWDIO_TMS_CLR(void)
{
    REG_WRITE(GPIO_OUT1_W1TC_REG, DAP_SWDIO_MASK);
}
__STATIC_FORCEINLINE uint32_t PIN_SWDIO_IN(void)
{
    return (REG_READ(GPIO_IN1_REG) >> (DAP_PIN_SWDIO - 32)) & 1U;
}
__STATIC_FORCEINLINE void PIN_SWDIO_OUT(uint32_t bit)
{
    if (bit & 1U) {
        REG_WRITE(GPIO_OUT1_W1TS_REG, DAP_SWDIO_MASK);
    } else {
        REG_WRITE(GPIO_OUT1_W1TC_REG, DAP_SWDIO_MASK);
    }
}
/** 使能 SWDIO 输出（主机驱动），推挽 */
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_ENABLE(void)
{
    REG_WRITE(GPIO_ENABLE1_W1TS_REG, DAP_SWDIO_MASK);
}
/** 关闭 SWDIO 输出 → 引脚变高阻，交给目标驱动（SWD turnaround） */
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_DISABLE(void)
{
    REG_WRITE(GPIO_ENABLE1_W1TC_REG, DAP_SWDIO_MASK);
}

/* ---- nRESET ---- */
__STATIC_FORCEINLINE uint32_t PIN_nRESET_IN(void)
{
    return (REG_READ(GPIO_IN1_REG) >> (DAP_PIN_nRESET - 32)) & 1U;
}
__STATIC_FORCEINLINE void PIN_nRESET_OUT(uint32_t bit)
{
    /* nRESET 配成开漏：写 1 = 释放（靠上拉拉高），写 0 = 拉低复位 */
    if (bit & 1U) {
        REG_WRITE(GPIO_OUT1_W1TS_REG, DAP_nRESET_MASK);
    } else {
        REG_WRITE(GPIO_OUT1_W1TC_REG, DAP_nRESET_MASK);
    }
}

/* ---- JTAG 专用脚：DAP_JTAG=0，仅提供占位定义（永不会被调用） ---- */
__STATIC_FORCEINLINE uint32_t PIN_TDI_IN(void) { return 0U; }
__STATIC_FORCEINLINE void PIN_TDI_OUT(uint32_t bit) { (void)bit; }
__STATIC_FORCEINLINE uint32_t PIN_TDO_IN(void) { return 0U; }
__STATIC_FORCEINLINE uint32_t PIN_nTRST_IN(void) { return 0U; }
__STATIC_FORCEINLINE void PIN_nTRST_OUT(uint32_t bit) { (void)bit; }

//**************************************************************************************************
/** \defgroup DAP_Config_Setup_gr CMSIS-DAP Hardware Setup */

/** SWD 模式引脚初始化 */
__STATIC_INLINE void PORT_SWD_SETUP(void)
{
#if DAP_USE_DEDIC_GPIO
    /* 高速模式：SWCLK/SWDIO 交给 dedicated GPIO bundle（幂等，建一次就够）。
     * 🚨 这里**不能**再对这两个脚调 gpio_config —— 焊盘一旦路由到 dedicated GPIO 信号，
     *    普通 GPIO 的写就到不了焊盘了（dedic_gpio_bench 踩过）。
     * 只有 nRESET 仍然走普通 GPIO。 */
    dedic_swd_pins_setup();

    /* ⚠️ nRESET 只配一次：PORT_SWD_SETUP() 每次 DAP_Connect 都会走到这里，
     *    重复 gpio_config() 同一个脚会让 IDF 打 `W gpio: conflict found for GPIO[46]`
     *    （看着像出错，其实无害 —— 但它会让人以为哪里坏了）。 */
    static bool s_rst_configured = false;
    if (!s_rst_configured) {
        gpio_config_t rst = {
            .pin_bit_mask = (1ULL << DAP_PIN_nRESET),
            .mode = GPIO_MODE_INPUT_OUTPUT_OD,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&rst);
        s_rst_configured = true;
    }
    PIN_nRESET_OUT(1U);
#else
    /* SWCLK / SWDIO：推挽输出 + 输入（SWDIO 读回要用） */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << DAP_PIN_SWCLK) | (1ULL << DAP_PIN_SWDIO),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    /* nRESET：开漏 + 内部上拉（避免和目标的复位电路顶牛） */
    gpio_config_t rst = {
        .pin_bit_mask = (1ULL << DAP_PIN_nRESET),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst);

    /* 空闲电平：SWCLK=1、SWDIO=1 且主机驱动、nRESET 释放 */
    PIN_SWCLK_TCK_SET();
    PIN_SWDIO_TMS_SET();
    PIN_SWDIO_OUT_ENABLE();
    PIN_nRESET_OUT(1U);
#endif
}

/** JTAG 模式引脚初始化（本工程未启用 JTAG） */
__STATIC_INLINE void PORT_JTAG_SETUP(void)
{
    PORT_SWD_SETUP();
}

/** 释放所有调试脚（高阻） */
__STATIC_INLINE void PORT_OFF(void)
{
#if DAP_USE_DEDIC_GPIO
    /* 🚨 高速模式下**绝不能**对 SWCLK/SWDIO 调 gpio_set_direction / PIN_SWDIO_OUT_DISABLE：
     *    那会把焊盘的输出路由从 dedicated GPIO 信号上扯下来，而普通 GPIO 的写到不了那个焊盘
     *    → 之后**整个探针再也不会输出任何时钟**（DAP_Disconnect 会走到这里，所以症状是
     *      "跑完一次会话后，下一次会话连激活序列都抓不到波形"，只有复位芯片才能恢复）。
     *    正确的"释放"就是 dedicated GPIO 的 OEN 置 1（高阻）。 */
    dedic_swd_pins_release();
    gpio_set_direction(DAP_PIN_nRESET, GPIO_MODE_INPUT);
#else
    PIN_SWDIO_OUT_DISABLE();
    gpio_set_direction(DAP_PIN_SWCLK, GPIO_MODE_INPUT);
    gpio_set_direction(DAP_PIN_nRESET, GPIO_MODE_INPUT);
#endif
}

/// 设备级初始化
__STATIC_INLINE void DAP_SETUP(void)
{
    PORT_SWD_SETUP();
}

/// 目标复位序列：返回 0 = 未实现专门的目标复位时序
__STATIC_INLINE uint8_t RESET_TARGET(void)
{
    return (0U);
}

//**************************************************************************************************
/** \defgroup DAP_Config_LEDs_gr CMSIS-DAP Hardware Status LEDs */

/// 连接状态 LED：板上只有 WS2812(RMT 驱动)，热路径里不碰它，留空
__STATIC_INLINE void LED_CONNECTED_OUT(uint32_t bit) { (void)bit; }
/// 运行状态 LED
__STATIC_INLINE void LED_RUNNING_OUT(uint32_t bit) { (void)bit; }

//**************************************************************************************************
/** \defgroup DAP_Config_Timestamp_gr CMSIS-DAP Timestamp */

/// 时间戳（TIMESTAMP_CLOCK=0 时不使用）
__STATIC_INLINE uint32_t TIMESTAMP_GET(void)
{
    return 0U;
}

//**************************************************************************************************
/** \defgroup DAP_Config_Strings_gr CMSIS-DAP 字符串 */

/** 厂商 */
__STATIC_INLINE uint8_t DAP_GetVendorString(char *str)
{
    const char *s = "Espressif";
    memcpy(str, s, sizeof("Espressif"));
    return (uint8_t)sizeof("Espressif");
}

/** 产品名 */
__STATIC_INLINE uint8_t DAP_GetProductString(char *str)
{
    const char *s = "ESP32-S31 CherryDAP WiFi";
    memcpy(str, s, sizeof("ESP32-S31 CherryDAP WiFi"));
    return (uint8_t)sizeof("ESP32-S31 CherryDAP WiFi");
}

/** 序列号（DAP.c 要求是 48bit 的十六进制字符串）。由 wifi_dap_main.c 开机填芯片 MAC。 */
extern char g_dap_serial[13];
__STATIC_INLINE uint8_t DAP_GetSerNumString(char *str)
{
    memcpy(str, g_dap_serial, sizeof(g_dap_serial));
    return (uint8_t)sizeof(g_dap_serial);
}

__STATIC_INLINE uint8_t DAP_GetTargetDeviceVendorString(char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetTargetDeviceNameString(char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetTargetBoardVendorString(char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetTargetBoardNameString(char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetProductFirmwareVersionString(char *str) { (void)str; return 0U; }

#endif /* __DAP_CONFIG_H__ */
