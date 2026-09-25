/*
 * dedic_selftest.c —— dedicated GPIO 高速 SWD 的**开机自测/标定**
 *
 * 为什么要有它：高速位时序完全依赖“这条指令花几个周期、一次 CSR 写几个周期生效”
 * 这类微观时序。靠猜会浪费好几轮烧录（dedic_gpio_bench 就是活教材），所以
 * 开机先跑一遍微基准，把数字打在日志里，再据此设计/校准时序。
 *
 * 本文件只测、**不碰引脚**（CSR 写与是否路由到焊盘无关），所以对现有慢速 SWD 零影响。
 */
#include <stdio.h>
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_rom_gpio.h"
#include "driver/gpio.h"
#include "driver/dedic_gpio.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "riscv/csr.h"
#include "sdkconfig.h"
#include "DAP_config.h"   /* DAP_USE_DEDIC_GPIO 开关 */
#include "DAP.h"            /* DAP_Data.clock_delay */

static const char *TAG = "dedic_bench";

/* 探针引脚（与 DAP_config.h 一致）：SWCLK=GPIO47 / SWDIO=GPIO48 */
#define DEDIC_PIN_CLK   47
#define DEDIC_PIN_DIO   48

/* main/daplink/dedic_swd.S */
extern void bench_empty(void);
extern void bench_nops64(void);
extern void bench_csrw64(void);
extern void bench_csr_pair64(void);
extern void bench_lw64(const uint32_t *p);
extern void bench_csrr64(void);
extern void bench_bit_direct(void);
extern void bench_bit_lut(const uint32_t *lut);
extern void bench_bit_lut2(const uint32_t *lut);
extern void bench_bit_lut3(const uint32_t *lut);
extern void bench_bit_read(void);

#define REPEATS 5

/* 32 项查表：每项 = “SWCLK=0 + SWDIO=该 bit” 的 OUT 寄存器值。
 * 🚨 必须放 IRAM（DRAM_ATTR）：关中断跑位时序时，取表若走 flash 会因 cache miss 抖动。 */
static DRAM_ATTR uint32_t s_lut[32];

static uint32_t min_cycles(void (*fn)(void))
{
    uint32_t best = UINT32_MAX;
    for (int r = 0; r < REPEATS; r++) {
        uint32_t c0 = (uint32_t)esp_cpu_get_cycle_count();
        fn();
        uint32_t c1 = (uint32_t)esp_cpu_get_cycle_count();
        if (c1 - c0 < best) {
            best = c1 - c0;
        }
    }
    return best;
}

static uint32_t min_cycles_lut(void (*fn)(const uint32_t *))
{
    uint32_t best = UINT32_MAX;
    for (int r = 0; r < REPEATS; r++) {
        uint32_t c0 = (uint32_t)esp_cpu_get_cycle_count();
        fn(s_lut);
        uint32_t c1 = (uint32_t)esp_cpu_get_cycle_count();
        if (c1 - c0 < best) {
            best = c1 - c0;
        }
    }
    return best;
}

/* ------------------------------------------------------------------ *
 *  焊盘自证：dedicated GPIO 真的驱动了引脚、输入通道真的读得回来
 *
 *  🚨 为什么必须自证（dedic_gpio_bench 的教训）：
 *     周期数好看 ≠ 引脚在动；而浮空引脚会靠寄生电容保持上次电平，
 *     "回读成功"也证明不了"引脚在动"。所以：
 *       ① 驱动后用**普通 GPIO 输入寄存器**回读焊盘真实电平；
 *       ② 判高阻必须开**内部下拉**（下拉赢不了推挽、一定赢浮空）。
 * ------------------------------------------------------------------ */
static inline int pad_level(uint32_t pin)
{
    uint32_t v = (pin < 32) ? REG_READ(GPIO_IN_REG) : REG_READ(GPIO_IN1_REG);
    return (int)((v >> (pin & 31)) & 1u);
}

static int dedic_pad_selftest(void)
{
    int fails = 0;

#if DAP_USE_DEDIC_GPIO
    /* 高速模式：用的是**正式路径那个 bundle**（PORT_SWD_SETUP 也会调这个，幂等），
     * 所以自检不能删它 —— 删了之后焊盘路由就断了。 */
    extern void dedic_swd_pins_setup(void);
    extern int  dedic_swd_pins_ready(void);
    extern uint32_t dedic_swd_clk_mask(void);
    extern uint32_t dedic_swd_dio_mask(void);
    extern uint32_t dedic_swd_in_bit(void);

    dedic_swd_pins_setup();
    if (!dedic_swd_pins_ready()) {
        ESP_LOGE(TAG, "  [FAIL] dedicated GPIO bundle 建不起来");
        return 1;
    }
    const uint32_t clk = dedic_swd_clk_mask();
    const uint32_t dio = dedic_swd_dio_mask();
    const uint32_t in_bit = dedic_swd_in_bit();
    ESP_LOGI(TAG, "  焊盘自证（高速路径）：CLK mask=0x%02x  SWDIO mask=0x%02x  IN bit=%u",
             (unsigned)clk, (unsigned)dio, (unsigned)in_bit);

    RV_CLEAR_CSR(0x803, clk | dio);          /* OEN 低有效：先确保推挽 */
    RV_WRITE_CSR(0x805, 0);
    esp_rom_delay_us(50);
    const int lo = pad_level(DEDIC_PIN_DIO);
    RV_WRITE_CSR(0x805, dio);
    esp_rom_delay_us(50);
    const int hi = pad_level(DEDIC_PIN_DIO);
    const uint32_t in_csr = RV_READ_CSR(0x804);
    if (lo == 0 && hi == 1) {
        ESP_LOGI(TAG, "  [PASS] CSR 写真的驱动了 SWDIO 焊盘（低=%d 高=%d）", lo, hi);
    } else {
        ESP_LOGE(TAG, "  [FAIL] CSR 没驱动焊盘（低=%d 高=%d）—— 周期数会好看但引脚是死的", lo, hi);
        fails++;
    }
    const int in_val = (int)((in_csr >> in_bit) & 1u);
    if (in_val == 1) {
        ESP_LOGI(TAG, "  [PASS] dedicated IN 通道读得回焊盘电平（in bit%u=1）", (unsigned)in_bit);
    } else {
        ESP_LOGE(TAG, "  [FAIL] dedicated IN 通道读不回（0x804=0x%02x）", (unsigned)(in_csr & 0xff));
        fails++;
    }

    /* 高阻：开内部下拉，高阻时下拉应该赢（浮空引脚会保持上次电平，不能用回读判） */
    gpio_set_pull_mode(DEDIC_PIN_DIO, GPIO_PULLDOWN_ONLY);
    esp_rom_delay_us(50);
    RV_CLEAR_CSR(0x803, dio);
    esp_rom_delay_us(50);
    const int push = pad_level(DEDIC_PIN_DIO);
    RV_SET_CSR(0x803, dio);                  /* 高阻 */
    esp_rom_delay_us(50);
    const int hiz = pad_level(DEDIC_PIN_DIO);
    if (push == 1 && hiz == 0) {
        ESP_LOGI(TAG, "  [PASS] OEN CSR 能切高阻（推挽=%d 高阻=%d）—— SWD turnaround 可用", push, hiz);
    } else {
        ESP_LOGE(TAG, "  [FAIL] OEN 切不动高阻（推挽=%d 高阻=%d）", push, hiz);
        fails++;
    }
    gpio_set_pull_mode(DEDIC_PIN_DIO, GPIO_FLOATING);
    /* 交还空闲态（CLK 低、SWDIO 驱动高），bundle 保留给正式路径 */
    RV_CLEAR_CSR(0x803, clk | dio);
    RV_WRITE_CSR(0x805, dio);
    ESP_LOGI(TAG, "  焊盘自证结束：%s", fails ? "有 FAIL" : "全部 PASS");
#else
    ESP_LOGI(TAG, "  焊盘自证：慢速模式（普通 GPIO），跳过 dedicated GPIO 自证");
#endif
    return fails;
}

void dedic_selftest_run(void)
{
    const float fcpu = (float)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;


    /* 查表内容：交替图案，最坏情况 */
    for (int i = 0; i < 32; i++) {
        s_lut[i] = ((0xA5A5A5A5u >> i) & 1u) ? 0x02u : 0x00u;
    }

    const uint32_t ovh = min_cycles(bench_empty);          /* 调用开销，扣掉 */
    const uint32_t nop = min_cycles(bench_nops64) - ovh;
    const uint32_t csrw = min_cycles(bench_csrw64) - ovh;
    const uint32_t pair = min_cycles(bench_csr_pair64) - ovh;
    const uint32_t lw = 0;   /* 见下：带参函数单独测 */

    uint32_t best_lw = UINT32_MAX;
    for (int r = 0; r < REPEATS; r++) {
        uint32_t c0 = (uint32_t)esp_cpu_get_cycle_count();
        bench_lw64(s_lut);
        uint32_t c1 = (uint32_t)esp_cpu_get_cycle_count();
        if (c1 - c0 < best_lw) {
            best_lw = c1 - c0;
        }
    }
    best_lw -= ovh;

    const uint32_t bit_direct = min_cycles(bench_bit_direct) - ovh;
    const uint32_t bit_lut = min_cycles_lut(bench_bit_lut) - ovh;
    const uint32_t bit_lut2 = min_cycles_lut(bench_bit_lut2) - ovh;
    const uint32_t bit_lut3 = min_cycles_lut(bench_bit_lut3) - ovh;
    const uint32_t bit_read = min_cycles(bench_bit_read) - ovh;
    const uint32_t csrr = min_cycles(bench_csrr64) - ovh;
    (void)lw;

    ESP_LOGI(TAG, "微基准（%d 次取最小，已扣函数调用开销 %u 周期）", REPEATS, (unsigned)ovh);
    ESP_LOGI(TAG, "  1 条 nop            : %5.2f cyc/条", (float)nop / 64.0f);
    ESP_LOGI(TAG, "  1 条 csrw CSR (写)  : %5.2f cyc/条   <-- 瓶颈：一次 CSR 写占住这么多周期",
             (float)csrw / 64.0f);
    ESP_LOGI(TAG, "  1 条 csrr CSR (读)  : %5.2f cyc/条", (float)csrr / 64.0f);
    ESP_LOGI(TAG, "  1 条 lw (IRAM)      : %5.2f cyc/条", (float)best_lw / 64.0f);
    ESP_LOGI(TAG, "  拉低+拉高 一对方波  : %5.2f cyc/周期  -> %.2f MHz  <-- 周期下限",
             (float)pair / 32.0f, fcpu / ((float)pair / 32.0f));
    ESP_LOGI(TAG, "  写 bit 直接移位(6条)      : %5.2f cyc -> %5.2f MHz", (float)bit_direct / 32.0f,
             fcpu / ((float)bit_direct / 32.0f));
    ESP_LOGI(TAG, "  写 bit 查表+prefetch(5条) : %5.2f cyc -> %5.2f MHz", (float)bit_lut / 32.0f,
             fcpu / ((float)bit_lut / 32.0f));
    ESP_LOGI(TAG, "  写 bit lw+csrw+csrrs(3条) : %5.2f cyc -> %5.2f MHz", (float)bit_lut2 / 32.0f,
             fcpu / ((float)bit_lut2 / 32.0f));
    ESP_LOGI(TAG, "  写 bit 同上+展开偏移      : %5.2f cyc -> %5.2f MHz", (float)bit_lut3 / 32.0f,
             fcpu / ((float)bit_lut3 / 32.0f));
    ESP_LOGI(TAG, "  读 bit (csrw+csrrs+csrr)  : %5.2f cyc -> %5.2f MHz", (float)bit_read / 32.0f,
             fcpu / ((float)bit_read / 32.0f));
    ESP_LOGI(TAG, "  参考：慢速实现 16 KB 读 0.21 MHz / 写 0.40 MHz");

    /* 焊盘级自证（用正式路径那个 bundle，不删） */
    dedic_pad_selftest();

#if DAP_USE_DEDIC_GPIO
    /* ---- 实测 SWJ 序列（= 激活序列）的真实耗时 → 反推 SWCLK ----
     * 回答一个致命问题：DAP_SWJ_Clock 换算出来的延时**到底生效没有**。
     * 没生效的话激活序列就以 22 MHz 跑，STM32F103 跟不上 → 一直 NO ACK。 */
    extern void SWJ_Sequence_fast(uint32_t count, const uint8_t *data);
    static const uint8_t act[11] = { 0x9E, 0xE7, 0xFF, 0xFF, 0xFF, 0xFF,
                                     0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    extern uint32_t g_selftest_note;   /* 防优化 */
    uint32_t c0 = (uint32_t)esp_cpu_get_cycle_count();
    SWJ_Sequence_fast(88, act);
    uint32_t c1 = (uint32_t)esp_cpu_get_cycle_count();
    const uint32_t cyc = c1 - c0;
    g_selftest_note = cyc;
    ESP_LOGI(TAG, "  SWJ 88 位序列实测 %u 周期 -> 等效 SWCLK ≈ %.2f MHz（DAP_Data.clock_delay=%u）",
             (unsigned)cyc, fcpu / ((float)cyc / 88.0f), (unsigned)DAP_Data.clock_delay);
#endif
}

uint32_t g_selftest_note;
