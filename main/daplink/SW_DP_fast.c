/*
 * SW_DP_fast.c —— dedicated GPIO + 展开汇编 的高速 SWD 时序层
 *
 * 一次传输的流程严格照抄慢速版（SW_DP_slow.c）的顺序，只把“位”换成
 * 一条 CSR 指令一个沿：
 *   包头 8 位 → 释放 SWDIO → 1 个 turnaround 时钟 → 读 3 位 ACK
 *   写：ACK=OK → 1 个 turnaround 时钟 → 接管 SWDIO → 32 位数据 + 校验位 → 空闲
 *   读：ACK=OK → 32 位数据（目标驱动）+ 校验位 → 校验 → turnaround 回来
 *
 * 实测（开机微基准，320 MHz）：写 14.09 cyc/bit = 22.7 MHz（delay=0 档）。
 *
 * 🚨 22.7 MHz 不是所有目标都吃得住：STM32F103 在这个速度下时好时坏（NO ACK），
 *    所以必须支持降速 —— DAP_SWJ_Clock 就是干这个的，本文件把它映射成每相位的延时次数。
 */
#include "DAP_config.h"
#include "DAP.h"
#include "dedic_swd.h"
#include "esp_attr.h"
#include "riscv/csr.h"
#include "driver/dedic_gpio.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

/* dedicated GPIO 的 CSR（见 dedic_gpio_cpu_ll.h / dedic_gpio_bench 实测） */
#define CSR_OUT     0x805
#define CSR_IN      0x804
#define CSR_OEN     0x803
#define M_CLK       0x01u
#define M_DIO       0x02u

/* ================================================================== *
 *  两种模式（编译期二选一）
 *
 *   DEDIC_SWD_MAX_SPEED = 1 —— 🚀 极限速度（默认）
 *       所有位时序都走**无分支、无延时循环**的展开汇编原语，
 *       完全不理 DAP_SWJ_Clock / DAP_Data.clock_delay：请求什么频率都跑满速。
 *       实测 SWCLK ~11.6MHz 中位 / 14.7MHz 峰值（500MS/s 逻辑分析仪量的）。
 *
 *   DEDIC_SWD_MAX_SPEED = 0 —— 可调速（伺候慢目标 / 排查用）
 *       按 DAP_Data.clock_delay 在每个相位烧延时，pyOCD 里设 1MHz 就真给 1MHz。
 *       这条路径完整保留（下面带 #else 的那两份实现）。
 * ================================================================== */
#define DEDIC_SWD_MAX_SPEED  1

/* ---------------------------------------------------------------- *
 *  可调速模式的时钟档位（DEDIC_SWD_MAX_SPEED=0 时才编译）
 *
 *  DAP.c 的 Set_Clock_Delay()（由 DAP_SWJ_Clock 命令 / DAP_Setup 触发）已经把
 *  用户要的频率换算成“半周期要烧多少个 DELAY_SLOW_CYCLES”，存在 DAP_Data.clock_delay。
 *
 *  下面两个常数是**实测拟合**出来的（不是理论值）：
 *    一个延时单位（addi+bnez 循环，分支命中还有额外开销）≈ 3 个 CPU 周期；
 *    每 bit 的固定开销（两条 CSR 写 + 分支）≈ 22 周期。
 *  于是  bit ≈ 6·d + 22 周期；要让 bit = 2×half（half = 目标半周期），d = (half - 11) / 3。
 *  🚨 老版本按"每单位 2 周期、固定 14 周期"算 → **实际频率只有请求的 0.65 倍**
 *     （请求 1MHz 量出来 0.65MHz）。逻辑分析仪一量就现形。
 * ---------------------------------------------------------------- */
#if !DEDIC_SWD_MAX_SPEED
#define DELAY_UNIT_CYCLES   3u          /* 一个延时单位实际烧掉的 CPU 周期 */
#define BIT_FIXED_CYCLES    22u         /* 每 bit 的固定开销（周期） */

/* 🚨 读相位的最小延时：目标是在 SWCLK **下降沿**之后才开始驱动 SWDIO 的，
 *    采样点最早也得在沿后几十 ns。没有这个下限时，满速档会在下降沿之后
 *    几条指令就采样 —— 读回的全是上一次的电平（ACK 变 FAULT、IDCODE 变错）。
 *    12 单位 ≈ 36 周期 ≈ 113ns。 */
#define READ_MIN_DELAY      12u

static inline uint32_t cur_delay(void)
{
    const uint32_t half = DAP_Data.clock_delay * DELAY_SLOW_CYCLES;   /* 目标半周期（周期） */
    if (half <= BIT_FIXED_CYCLES / 2u) {
        return 0u;                                                 /* 已经比目标快，跑满速 */
    }
    return (half - BIT_FIXED_CYCLES / 2u) / DELAY_UNIT_CYCLES;
}

/* 采样相位用的延时：在写延时基础上加下限（见 READ_MIN_DELAY） */
static inline uint32_t cur_delay_read(void)
{
    const uint32_t d = cur_delay();
    return d < READ_MIN_DELAY ? READ_MIN_DELAY : d;
}
#endif  /* !DEDIC_SWD_MAX_SPEED */

/* 一个相位里的延时（不追求精确周期数，够慢就行） */
static inline void delay_ticks(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        __asm__ volatile("nop");
    }
}

/* ---------------------------------------------------------------- *
 *  dedicated GPIO bundle：SWCLK=GPIO47 -> out ch0，SWDIO=GPIO48 -> out ch1
 *  in ch0/ch1 同样对应两个焊盘（in_offset=0），SWDIO 在 in ch1。
 * ---------------------------------------------------------------- */
static dedic_gpio_bundle_handle_t s_bundle;
static uint32_t s_clk_mask, s_dio_mask, s_in_bit;

void dedic_swd_pins_setup(void)
{
    /* 🚨 必须**每次重新装**，不能"建过就返回"：
     *    `PORT_OFF()`（DAP_Disconnect 会调）里有 gpio_set_direction(SWCLK, INPUT)，
     *    那会把焊盘的输出路由从 dedicated GPIO 信号上扯下来；而普通 GPIO 的写
     *    到不了归 dedicated GPIO 的焊盘 —— 于是**下一次会话全程没有任何时钟**
     *    （实测：跑完一次带 Disconnect 的会话后，再开会话连激活序列都不出波形，
     *      逻辑分析仪连触发都等不到）。重新 new_bundle 会把焊盘函数重新选回去。 */
    if (s_bundle) {
        dedic_gpio_del_bundle(s_bundle);
        s_bundle = NULL;
    }
    const int pins[] = { DAP_PIN_SWCLK, DAP_PIN_SWDIO };
    dedic_gpio_bundle_config_t cfg = {
        .gpio_array = pins,
        .array_size = 2,
        .flags = { .out_en = 1, .in_en = 1 },
    };
    if (dedic_gpio_new_bundle(&cfg, &s_bundle) != ESP_OK) {
        s_bundle = NULL;
        return;
    }
    uint32_t out_off = 0, in_off = 0;
    dedic_gpio_get_out_offset(s_bundle, &out_off);
    dedic_gpio_get_in_offset(s_bundle, &in_off);
    s_clk_mask = 1u << (out_off + 0);
    s_dio_mask = 1u << (out_off + 1);
    s_in_bit = in_off + 1;
    /* 初始：CLK 低、SWDIO 驱动高（SWD 空闲态），OEN 清掉 = 推挽 */
    RV_CLEAR_CSR(CSR_OEN, s_dio_mask);
    RV_WRITE_CSR(CSR_OUT, s_dio_mask);
    dedic_swd_state_dump("setup");
}

/* 释放两个脚（高阻）—— 给 PORT_OFF 用。
 * 🚨 这里**只能**写 dedicated GPIO 的 OEN CSR，绝不能调 gpio_set_direction：
 *    那会把焊盘路由改回普通 GPIO，后面就再也出不了波形了（见上面的注释）。 */
void dedic_swd_pins_release(void)
{
    if (!s_bundle) {
        return;
    }
    RV_SET_CSR(CSR_OEN, s_clk_mask | s_dio_mask);   /* OEN=1 → 两脚都高阻 */
}

int dedic_swd_pins_ready(void)      { return s_bundle != NULL; }
uint32_t dedic_swd_clk_mask(void)  { return s_clk_mask; }
uint32_t dedic_swd_dio_mask(void)  { return s_dio_mask; }
uint32_t dedic_swd_in_bit(void)    { return s_in_bit; }

/* ---------------------------------------------------------------- *
 *  诊断快照（DEDIC_SWD_TRACE 打开时用；排查完关掉即可）
 *
 *  🚨 只在**关键点**打：进/出每笔高速传输（限前 TRACE_N 笔）、
 *     以及任何非 OK 应答之后。理由是 printf 会毁掉位时序 ——
 *     打点必须放在临界区之外。
 * ---------------------------------------------------------------- */
/* 🚨 排查完请置 0：每条日志都是**同步**写 UART0 + USB-Serial/JTAG（各 ~1ms），
 *    会把事务间隔从 ~0.3ms 拉长到 ~3ms —— 逻辑分析仪的窗口就抓不全整个序列了
 *    （本工程排查 FAULT 那次就是这样，抓包里只剩激活段）。 */
#define DEDIC_SWD_TRACE     0
#define TRACE_N             24

#if DEDIC_SWD_TRACE
#include "esp_log.h"

/* 下面三个小工具在后面定义；这里先声明，好让诊断块能用（setup 也要调 dump）。 */
static inline uint32_t ints_off(void);
static inline void     ints_restore(uint32_t m);
static inline void     delay_ticks(uint32_t n);

static int s_trace_left = TRACE_N;

/* CSR + 两个焊盘的路由/使能寄存器：引脚"死掉"时，一眼看出是哪一层断的。
 * 🚨 焊盘 47/48 在 GPIO_OUT1/ENABLE1 里是 **bit15/bit16**（那一组寄存器装的是 pad32~63）。 */
void dedic_swd_state_dump(const char *where)
{
    const uint32_t oen = RV_READ_CSR(CSR_OEN) & 0xFFu;
    const uint32_t out = RV_READ_CSR(CSR_OUT) & 0xFFu;
    const uint32_t in  = RV_READ_CSR(CSR_IN) & 0xFFu;
    const uint32_t out1 = REG_READ(GPIO_OUT1_REG);
    const uint32_t en1  = REG_READ(GPIO_ENABLE1_REG);
    const uint32_t f47 = REG_READ(GPIO_FUNC47_OUT_SEL_CFG_REG);
    const uint32_t f48 = REG_READ(GPIO_FUNC48_OUT_SEL_CFG_REG);
    ESP_EARLY_LOGI("dedic",
                   "%-8s OEN=%02X OUT=%02X IN=%02X | OUT1:47=%u 48=%u | EN1:47=%u 48=%u | F47=%08X F48=%08X",
                   where, (unsigned)oen, (unsigned)out, (unsigned)in,
                   (unsigned)((out1 >> (47 - 32)) & 1u), (unsigned)((out1 >> (48 - 32)) & 1u),
                   (unsigned)((en1 >> (47 - 32)) & 1u), (unsigned)((en1 >> (48 - 32)) & 1u),
                   (unsigned)f47, (unsigned)f48);
}

/* 焊盘自检：推挽输出 00 再输出 11，分别回读。
 * 返回 bit0 = "输出 00 时两脚都读到 0"，bit1 = "输出 11 时两脚都读到 1"；3 = 焊盘活着。
 * ⚠️ 它会**额外产生两个时钟**，所以只在"已经出错"或"会话开始前"调用。 */
uint32_t dedic_swd_pad_probe(void)
{
    const uint32_t m = ints_off();
    RV_WRITE_CSR(CSR_OEN, 0);                 /* 两通道都推挽 */
    RV_WRITE_CSR(CSR_OUT, 0);
    delay_ticks(80);
    const uint32_t a = RV_READ_CSR(CSR_IN);
    RV_WRITE_CSR(CSR_OUT, 0x03);
    delay_ticks(80);
    const uint32_t b = RV_READ_CSR(CSR_IN);
    RV_WRITE_CSR(CSR_OUT, M_DIO);             /* 回到 SWD 空闲态：CLK 低、DIO 高 */
    RV_CLEAR_CSR(CSR_OEN, M_DIO);
    ints_restore(m);
    uint32_t r = 0;
    if ((a & 0x03u) == 0u)    r |= 1u;
    if ((b & 0x03u) == 0x03u) r |= 2u;
    return r;
}

static void tx_trace(const char *what, uint32_t req, uint32_t ack)
{
    if (s_trace_left <= 0) {
        return;
    }
    s_trace_left--;
    ESP_EARLY_LOGI("dedic", "%-6s req=%02X ack=%u", what, (unsigned)req, (unsigned)ack);
}
#else
void dedic_swd_state_dump(const char *where) { (void)where; }
uint32_t dedic_swd_pad_probe(void) { return 3u; }
static void tx_trace(const char *what, uint32_t req, uint32_t ack)
{ (void)what; (void)req; (void)ack; }
#endif  /* DEDIC_SWD_TRACE */

/* ---------------------------------------------------------------- *
 *  小工具
 * ---------------------------------------------------------------- */
static inline uint32_t ints_off(void)
{
    uint32_t m;
    __asm__ volatile("csrrci %0, mstatus, 8" : "=r"(m));   /* 清 MIE，返回旧值 */
    return m;
}

static inline void ints_restore(uint32_t m)
{
    if (m & 8u) {
        __asm__ volatile("csrsi mstatus, 8");
    }
}

/* 4 位请求头的奇偶（SWD 包头的奇校验） */
static inline uint32_t parity4(uint32_t v)
{
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1u;
}

/* 32 位数据奇偶（校验位） */
static inline uint32_t parity32(uint32_t v)
{
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v &= 0x0Fu;
    return (0x6996u >> v) & 1u;
}

/* 32 项查表：每项 = “CLK=0 + SWDIO=该 bit”的 OUT 寄存器值。
 * 🚨 放 IRAM：关中断跑位时序时取表不能走 flash。
 * 用 16 项半字节表拼，比逐位循环快（8×5 条 vs 32×5 条）。 */
static const uint32_t s_nib[16][4] DRAM_ATTR = {
    { 0, 0, 0, 0 }, { 2, 0, 0, 0 }, { 0, 2, 0, 0 }, { 2, 2, 0, 0 },
    { 0, 0, 2, 0 }, { 2, 0, 2, 0 }, { 0, 2, 2, 0 }, { 2, 2, 2, 0 },
    { 0, 0, 0, 2 }, { 2, 0, 0, 2 }, { 0, 2, 0, 2 }, { 2, 2, 0, 2 },
    { 0, 0, 2, 2 }, { 2, 0, 2, 2 }, { 0, 2, 2, 2 }, { 2, 2, 2, 2 },
};
static uint32_t s_lut[33] DRAM_ATTR;      /* 32 位数据 + 第 33 项 = 校验位 */

/* 32 项查表：一次填满 32 个 OUT 值。**完全展开**（省掉循环的 addi/bnez/blt
 * —— 每笔写事务都要填一次，8 轮循环的开销在白捡的 ns 里算贵的）。 */
static inline void lut_build(uint32_t v)
{
    const uint32_t *e;
    e = s_nib[(v >>  0) & 0xFu]; s_lut[ 0] = e[0]; s_lut[ 1] = e[1]; s_lut[ 2] = e[2]; s_lut[ 3] = e[3];
    e = s_nib[(v >>  4) & 0xFu]; s_lut[ 4] = e[0]; s_lut[ 5] = e[1]; s_lut[ 6] = e[2]; s_lut[ 7] = e[3];
    e = s_nib[(v >>  8) & 0xFu]; s_lut[ 8] = e[0]; s_lut[ 9] = e[1]; s_lut[10] = e[2]; s_lut[11] = e[3];
    e = s_nib[(v >> 12) & 0xFu]; s_lut[12] = e[0]; s_lut[13] = e[1]; s_lut[14] = e[2]; s_lut[15] = e[3];
    e = s_nib[(v >> 16) & 0xFu]; s_lut[16] = e[0]; s_lut[17] = e[1]; s_lut[18] = e[2]; s_lut[19] = e[3];
    e = s_nib[(v >> 20) & 0xFu]; s_lut[20] = e[0]; s_lut[21] = e[1]; s_lut[22] = e[2]; s_lut[23] = e[3];
    e = s_nib[(v >> 24) & 0xFu]; s_lut[24] = e[0]; s_lut[25] = e[1]; s_lut[26] = e[2]; s_lut[27] = e[3];
    e = s_nib[(v >> 28) & 0xFu]; s_lut[28] = e[0]; s_lut[29] = e[1]; s_lut[30] = e[2]; s_lut[31] = e[3];
    /* 第 33 项 = 校验位：折进同一张表，写相位就是 33 拍齐步走，不用再单独发一位 */
    s_lut[32] = parity32(v) ? 2u : 0u;
}

/* 目标配置里的一次 turnaround 要几个时钟（DAP_SWD_Configure 设的；至少 1） */
static inline uint32_t turn_n(void)
{
    const uint32_t t = DAP_Data.swd_conf.turnaround;
    return t ? t : 1u;
}

/* CMSIS-DAP 的 idle cycles（DAP_TransferConfigure 设的）：主机把 SWDIO 拉低空打 N 拍，
 * 之后 dedic_clk_idle() 会把它拉回高。**默认是 0**（本工程实测 pyOCD/裸脚本都是 0），
 * 所以正常情况下这个分支一次都不会进。 */
static inline void idle_cycles(void)
{
    const uint32_t n = DAP_Data.transfer.idle_cycles;
    if (n) {
        RV_WRITE_CSR(CSR_OUT, 0);
#if DEDIC_SWD_MAX_SPEED
        dedic_clock_n(n);
#else
        dedic_clock_n_d(n, cur_delay());
#endif
    }
}

/* 非 OK 应答后的现场快照（只读寄存器，**不产生时钟**，所以不会干扰下一笔） */
static inline void fail_dump(uint32_t ack)
{
#if DEDIC_SWD_TRACE
    if (ack != DAP_TRANSFER_OK) {
        dedic_swd_state_dump("fail");
    }
#else
    (void)ack;
#endif
}

/* ---------------------------------------------------------------- *
 *  🚀 小原语的**行内汇编**版（极限档专用）
 *
 *  这些动作一笔传输里要出现 6 次以上（turnaround 2~3 次、release/drive 各一次、
 *  收尾 idle 一次）。调 `dedic_x_clock1()` 之类的函数每次要花 jal/ret ~8 周期，
 *  6 次就是 ~50 周期 —— 而它们本身只有 2~3 条指令。用行内汇编把它们摊平。
 *
 *  LA 实测佐证：一笔读传输里 turnaround 那段"拍周期"是 112ns(36 周期)、
 *  ACK 之后的 turnaround 是 126ns(40 周期)，而两条 CSR 写只要 10 周期 ——
 *  多出来的就是调用开销 + release/drive 的 CSR 操作。
 * ---------------------------------------------------------------- */
#define X_TURNAROUND()   __asm__ volatile("csrw 0x805, zero\n\tcsrrsi zero, 0x805, 1" ::: "memory")
#define X_DIO_RELEASE()  __asm__ volatile("li t0, 2\n\tcsrs 0x803, t0" ::: "t0", "memory")
#define X_DIO_DRIVE()    __asm__ volatile("li t0, 2\n\tcsrc 0x803, t0" ::: "t0", "memory")
#define X_CLK_IDLE()     __asm__ volatile("li t0, 2\n\tcsrw 0x805, t0\n\tcsrc 0x803, t0" \
                                           ::: "t0", "memory")

/* ---------------------------------------------------------------- *
 *  SWJ_Sequence：任意位序列（主机单向输出）——**必须也走高速路径**
 *
 * 🚨 这一条是踩出来的：fast 模式下 SWCLK/SWDIO 归 dedicated GPIO，
 *    慢速实现的普通 GPIO 写**到不了焊盘** → 激活序列（线复位 + JTAG-to-SWD）
 *    等于没发 → 目标一直停在 JTAG 模式 → 所有传输 NO ACK。
 * ---------------------------------------------------------------- */
#if DEDIC_SWD_MAX_SPEED
/* ================================================================== *
 *  🚀 极限速度版（默认）
 *
 *  与下面可调速版的唯一区别：所有位时序都用**无分支、无延时循环**的展开原语，
 *  连"这个相位要不要烧延时"这个判断都没有了 —— 每条指令都直接产生沿。
 *  代价：DAP_SWJ_Clock 完全失效（请求任何频率都跑满速）。
 *  实测：SWCLK 中位 ~11.6MHz / 峰值 ~14.7MHz（500MS/s 逻辑分析仪，STM32F103）。
 * ================================================================== */

void SWJ_Sequence_fast(uint32_t count, const uint8_t *data)
{
    const uint32_t m = ints_off();

    while (count >= 8u) {
        dedic_write_bits8(*data++);          /* 16 条指令一个字节，无分支 */
        count -= 8u;
    }
    if (count) {
        uint32_t v = *data;
        for (uint32_t i = 0; i < count; i++) {
            dedic_write_bit1(v & 1u);
            v >>= 1;
        }
    }
    dedic_clk_idle();
    ints_restore(m);
    tx_trace("SWJ", 0, 0);
}

uint8_t SWD_Write_fast(uint32_t request, uint32_t *data)
{
    /* 🚨 必须先认 RnW：ARM 的 DAP.c 会用 SWD_Write(DP_RDBUFF | DAP_TRANSFER_RnW, &data)
     * 这个"带 RnW 的写调用"来**读**（借写做读、取回挂起读的结果）。
     * 不认的话数据相位会主机推挽去写、目标同时在驱动读数据（对顶），*data 也永不填。
     * 症状（实测）：pyocd commander `reg pc` 直接失败。 */
    if (request & DAP_TRANSFER_RnW) {
        return SWD_Read_fast(request, data);
    }

    const uint32_t m = ints_off();

    /* 查表 + 整笔传输全在汇编里（lut_build 也展开，不再有包头计算/6 次子函数调用） */
    lut_build(*data);
    uint32_t ack = dedic_x_write_word(request, s_lut);

    if (ack != DAP_TRANSFER_OK) {
        /* 罕见路径：与慢速参考实现同序 */
        if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {
            /* 🚨🚨 这一拍不能省：**非 OK 也必须先走一个 turnaround 才接管总线**。
             *   目标会一直驱动 ACK 的最后一位，直到它之后的**下降沿**才放手；
             *   直接 dio_drive() → 两个推挽对顶，且 SWDIO 在 CLK 高电平期间跳变
             *   （SWD 禁止）→ 目标状态机错位 → 此后一路 NO-ACK（本工程踩过）。 */
            X_TURNAROUND();
            X_DIO_DRIVE();
            if (DAP_Data.swd_conf.data_phase) {
                RV_WRITE_CSR(CSR_OUT, 0);        /* dummy WDATA = 0 */
                dedic_clock_n(32u + 1u);
            }
        } else {
            /* 协议错（包头被拒）：退掉整个数据相位，全程保持高阻 */
            dedic_clock_n(33u);
            X_DIO_DRIVE();
        }
    }
    idle_cycles();
    X_CLK_IDLE();                            /* 空闲：CLK 低、SWDIO 驱动高（行内） */
    ints_restore(m);
    tx_trace("WRITE", request, ack);
    fail_dump(ack);
    return (uint8_t)ack;
}

uint8_t SWD_Read_fast(uint32_t request, uint32_t *data)
{
    const uint32_t m = ints_off();

    /* 整笔传输（包头→ACK→32 位数据→校验→turnaround→接管→空闲）在**一个**汇编函数里 */
    uint32_t ack = dedic_x_read_word(request, data);

    if ((ack != DAP_TRANSFER_OK) && (ack != DAP_TRANSFER_ERROR)) {
        /* 罕见路径。⚠️ 校验错（ERROR=8）走的是"正常收尾"，与参考实现一致，
         *    上面的汇编函数已经做完了，这里不能重复处理。 */
        if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {
            /* 与慢速参考实现同序：dummy 读数据（目标驱动、主机仍高阻）→ turnaround → 接管 */
            if (DAP_Data.swd_conf.data_phase) {
                dedic_clock_n(32u + 1u);
            }
            X_TURNAROUND();
        } else {
            dedic_clock_n(33u);
        }
        X_DIO_DRIVE();
    }
    idle_cycles();
    X_CLK_IDLE();
    ints_restore(m);
    tx_trace("READ", request, ack);
    fail_dump(ack);
    return (uint8_t)ack;
}

#else   /* ---------------- 可调速版（DEDIC_SWD_MAX_SPEED=0）---------------- */

void SWJ_Sequence_fast(uint32_t count, const uint8_t *data)
{
    const uint32_t m = ints_off();
    const uint32_t d = cur_delay();

    while (count >= 8u) {
        dedic_write_bits8_d(*data++, d);
        count -= 8u;
    }
    if (count) {
        uint32_t v = *data;
        for (uint32_t i = 0; i < count; i++) {
            dedic_write_bit1_d(v & 1u, d);
            v >>= 1;
        }
    }
    dedic_clk_idle();
    ints_restore(m);
    tx_trace("SWJ", 0, 0);
}

/* ---------------------------------------------------------------- *
 *  写传输
 * ---------------------------------------------------------------- */
uint8_t SWD_Write_fast(uint32_t request, uint32_t *data)
{
    if (request & DAP_TRANSFER_RnW) {     /* 见上方极限版的注释 */
        return SWD_Read_fast(request, data);
    }

    const uint8_t header = (uint8_t)(0x81u | ((request & 0x0Fu) << 1) |
                                     ((parity4(request & 0x0Fu) & 1u) << 5));
    const uint32_t m = ints_off();
    const uint32_t d = cur_delay();        /* 写相位延时（数据在下拉沿换） */
    const uint32_t dr = cur_delay_read();  /* 采样相位延时（要留够目标驱动时间） */
    uint32_t ack;

    dedic_write_bits8_d(header, d);        /* 8 位包头（汇编版） */
    dedic_dio_release();                   /* 释放 SWDIO */
    ack = dedic_read_ack3_d(dr);           /* turnaround + 3 位 ACK */

    if (ack == DAP_TRANSFER_OK) {
        dedic_clock_n_d(turn_n(), d);      /* turnaround：主机接管前 */
        dedic_dio_drive();
        lut_build(*data);
        dedic_write_bits32_lut_d(s_lut, d);      /* 32 位数据 */
        dedic_write_bit1_d(parity32(*data), d);  /* 校验位 */
    } else if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {
        dedic_clock_n_d(turn_n(), d);            /* turnaround：让目标先放手 */
        dedic_dio_drive();
        if (DAP_Data.swd_conf.data_phase) {
            RV_WRITE_CSR(CSR_OUT, 0);            /* dummy WDATA = 0 */
            dedic_clock_n_d(32u + 1u, d);        /* 32 位 dummy + 校验位 */
        }
    } else {
        /* 协议错（包头被拒）：退掉整个数据相位，全程保持高阻 */
        dedic_clock_n_d(turn_n() + 32u + 1u, d);
        dedic_dio_drive();
    }
    idle_cycles();
    dedic_clk_idle();                 /* 空闲：CLK 低、SWDIO 驱动高 */
    ints_restore(m);
    tx_trace("WRITE", request, ack);
    fail_dump(ack);
    return (uint8_t)ack;
}

/* ---------------------------------------------------------------- *
 *  读传输
 * ---------------------------------------------------------------- */
uint8_t SWD_Read_fast(uint32_t request, uint32_t *data)
{
    const uint8_t header = (uint8_t)(0x81u | ((request & 0x0Fu) << 1) |
                                     ((parity4(request & 0x0Fu) & 1u) << 5));
    const uint32_t m = ints_off();
    const uint32_t d = cur_delay();
    const uint32_t dr = cur_delay_read();
    uint32_t ack;

    dedic_write_bits8_d(header, d);
    dedic_dio_release();
    ack = dedic_read_ack3_d(dr);

    if (ack == DAP_TRANSFER_OK) {
        uint32_t val = dedic_read_bits32_d(dr);            /* 32 位数据（目标驱动） */
        uint32_t par = dedic_read_bit_d(dr);               /* 校验位 */
        if ((parity32(val) ^ par) & 1u) {
            ack = DAP_TRANSFER_ERROR;
        }
        if (data) {
            *data = val;
        }
        dedic_clock_n_d(turn_n(), d);                      /* turnaround 回来 */
        dedic_dio_drive();
    } else if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {
        /* 与慢速参考实现同序：dummy 读数据（目标驱动、主机仍高阻）→ turnaround → 接管 */
        if (DAP_Data.swd_conf.data_phase) {
            dedic_clock_n_d(32u + 1u, dr);
        }
        dedic_clock_n_d(turn_n(), d);
        dedic_dio_drive();
    } else {
        dedic_clock_n_d(turn_n() + 32u + 1u, d);
        dedic_dio_drive();
    }
    idle_cycles();
    dedic_clk_idle();
    ints_restore(m);
    tx_trace("READ", request, ack);
    fail_dump(ack);
    return (uint8_t)ack;
}
#endif  /* DEDIC_SWD_MAX_SPEED */
