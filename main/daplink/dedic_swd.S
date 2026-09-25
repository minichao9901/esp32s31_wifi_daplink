/*
 * dedic_swd.S —— ESP32-S31 dedicated GPIO（CPU GPIO / fast GPIO）版 SWD 位时序
 *
 * 硬件事实（来自 IDF components/esp_hal_gpio/esp32s31/include/hal/dedic_gpio_cpu_ll.h，
 * 以及 dedic_gpio_bench 的实测）：
 *   0x803  CSR_GPIO_OEN_USER  输出使能，**低有效**：bit=0 输出，bit=1 高阻(Hi-Z)
 *   0x804  CSR_GPIO_IN_USER   读引脚输入（低 8 位 = 8 个输入通道）
 *   0x805  CSR_GPIO_OUT_USER  写引脚输出（低 8 位 = 8 个输出通道）—— **一次写 = 一次电平更新**
 *
 * bundle（C 侧 dedic_gpio_new_bundle 建）：pins {47, 48} →
 *   out ch0 = SWCLK(47) mask 0x01 ；out ch1 = SWDIO(48) mask 0x02
 *   in  ch0 = SWCLK      ；in  ch1 = SWDIO      （in_offset = 0）
 *   OEN bit1 = SWDIO 高阻控制（1 = 释放给目标驱动）
 *
 * 🚨 设计要点（全部来自 dedic_gpio_bench 的实测结论）：
 *   ① 一条 CSR 写从发射到焊盘生效约 **5 个周期**（16 ns）——这就是引脚最短脉冲；
 *   ② 两个沿之间的间隔 ≈ 5 + 2×(中间夹的指令数)（实测拟合出来的经验式）；
 *   ③ 所以：**一次 store = 一个时钟沿**，把计算塞进两个沿之间当"免费延时"，
 *      并且整段展开（去掉循环的 addi/bne），传输期间关中断（mstatus.MIE=0）。
 *      这套路子抄自 资料\akaLinkPro\firmware\application_5331\src\dap\SW_DP\SW_DP_GPIO_ASM_*.S
 *      （HPM5331 用同样的思路做到 48~80 MHz）。
 *   ④ 本文件全部放 .iram1：既避免 flash cache 抖动，也避免关中断期间取指走 flash。
 *
 * 约定：SWD 是 **LSB first**；SWCLK 上升沿目标采样，下降沿主机换数据。
 */

    .section .iram1,"ax"
    .option norvc

/* ------------------------------------------------------------------ *
 * 常量
 * ------------------------------------------------------------------ */
    .equ CSR_OUT,   0x805
    .equ CSR_IN,    0x804
    .equ CSR_OEN,   0x803
    .equ M_CLK,     0x01
    .equ M_DIO,     0x02

/* ==================================================================
 *  微基准：先量清楚这块核的"每条指令几个周期 / 一次 CSR 写几个周期"
 *  —— 时序设计不能靠猜。全部展开、无循环，测出来再除以指令数。
 * ================================================================== */

/* void bench_csrr64(void) —— 64 条背靠背 CSR 读（读 IN 寄存器）：量采样单价 */
    .global bench_csrr64
    .type   bench_csrr64, @function
    .balign 16
bench_csrr64:
    .rept 64
    csrr    t0, CSR_IN
    .endr
    ret

/* void bench_bit_lut2(const uint32_t *lut) —— 3 条指令/bit 的写法：
 *     lw（取 CLK=0+DIO=bit 的值） / csrw（下降沿+换数据） / csrrs（上升沿，只置 CLK）
 *   —— 目标是撞到“2 条 CSR 写 = 10 周期”的下限。 */
    .global bench_bit_lut2
    .type   bench_bit_lut2, @function
    .balign 16
bench_bit_lut2:
    li      t1, M_CLK
    lw      t3, 0(a0)
    .rept 31
    csrw    CSR_OUT, t3
    csrrs   zero, CSR_OUT, t1
    lw      t3, 4(a0)
    addi    a0, a0, 4
    .endr
    csrw    CSR_OUT, t3
    csrrs   zero, CSR_OUT, t1
    ret

/* void bench_bit_lut3(const uint32_t *lut) —— 与上面相同，但用 .set 造立即数偏移，彻底去掉 addi */
    .global bench_bit_lut3
    .type   bench_bit_lut3, @function
    .balign 16
bench_bit_lut3:
    li      t1, M_CLK
    .set    L3OFF, 0
    lw      t3, L3OFF(a0)
    .rept 31
    .set    L3OFF, L3OFF + 4
    csrw    CSR_OUT, t3
    csrrs   zero, CSR_OUT, t1
    lw      t3, L3OFF(a0)
    .endr
    csrw    CSR_OUT, t3
    csrrs   zero, CSR_OUT, t1
    ret

/* void bench_bit_read(const uint32_t *lut) —— 读 bit 的三个 CSR（写低/写高/采样） */
    .global bench_bit_read
    .type   bench_bit_read, @function
    .balign 16
bench_bit_read:
    li      t1, M_CLK
    .rept 32
    csrw    CSR_OUT, zero
    csrrs   zero, CSR_OUT, t1
    csrr    t3, CSR_IN
    .endr
    ret

/* void bench_empty(void) —— 空函数：用来扣掉函数调用本身的开销 */
    .global bench_empty
    .type   bench_empty, @function
    .balign 16
bench_empty:
    ret

/* void bench_nops64(void) —— 64 条 nop */
    .global bench_nops64
    .type   bench_nops64, @function
    .balign 16
bench_nops64:
    .rept 64
    nop
    .endr
    ret

/* void bench_csrw64(void) —— 64 条背靠背 CSR 写（写同一个 OUT 寄存器） */
    .global bench_csrw64
    .type   bench_csrw64, @function
    .balign 16
bench_csrw64:
    li      t0, 0
    .rept 64
    csrw    CSR_OUT, t0
    .endr
    ret

/* void bench_csr_pair64(void) —— 32 组“拉低+拉高”，模拟一个 bit 的两个沿 */
    .global bench_csr_pair64
    .type   bench_csr_pair64, @function
    .balign 16
bench_csr_pair64:
    li      t0, 0
    li      t1, M_CLK
    .rept 32
    csrw    CSR_OUT, t0
    csrw    CSR_OUT, t1
    .endr
    ret

/* void bench_lw64(const uint32_t *p) —— 64 条背靠背 load（IRAM 里） */
    .global bench_lw64
    .type   bench_lw64, @function
    .balign 16
bench_lw64:
    .rept 64
    lw      t0, 0(a0)
    .endr
    ret

/* void bench_bit_direct(const uint32_t *dummy, uint32_t iters)
 *   “直接移位”版写一个 bit：
 *     andi t3,a0,1 / slli t3,t3,1 / csrw OUT,t3 / ori t4,t3,1 / srli a0,a0,1 / csrw OUT,t4
 *   返回：一条 bit 的周期数（调用方除 iters）——这里 iters 固定 32（展开 32 次） */
    .global bench_bit_direct
    .type   bench_bit_direct, @function
    .balign 16
bench_bit_direct:
    li      a0, 0xA5A5A5A5
    .rept 32
    andi    t3, a0, 1
    slli    t3, t3, 1
    csrw    CSR_OUT, t3
    ori     t4, t3, 1
    srli    a0, a0, 1
    csrw    CSR_OUT, t4
    .endr
    ret

/* void bench_bit_lut(const uint32_t *lut) —— “查表+预取”版写 32 个 bit
 *   查表项 = 该 bit 的“CLK=0 + DIO=bit”OUT 值；下一位的值在本位高电平期间预取。
 *   lut 必须放在 IRAM（DRAM_ATTR），否则关中断期间取表会走 flash。 */
    .global bench_bit_lut
    .type   bench_bit_lut, @function
    .balign 16
bench_bit_lut:
    lw      t3, 0(a0)
    .rept 31
    csrw    CSR_OUT, t3          /* 拉低 + 数据 */
    ori     t4, t3, M_CLK
    csrw    CSR_OUT, t4          /* 拉高 */
    lw      t3, 4(a0)            /* 预取下一个 bit（1 条指令 = 低电平剩下的部分） */
    addi    a0, a0, 4
    .endr
    csrw    CSR_OUT, t3
    ori     t4, t3, M_CLK
    csrw    CSR_OUT, t4
    ret

/* ==================================================================
 *  带延时的参数化原语（给 DAP_SWJ_Clock 用：目标跟不上时可以降速）
 *
 *  每 bit 结构：CLK 低(+数据) → 延时 → [采样] → CLK 高 → 延时
 *  延时循环 = addi+bnez = 2 周期/次，所以 bit 周期 ≈ 14 + 4×delay（两个相位各 2×delay）。
 *  delay=0 时 ≈ 14 cyc/bit（~22 MHz）；delay≈150 时 ≈ 1 MHz。
 * ================================================================== */

/* void dedic_write_bits32_lut_d(const uint32_t *lut, uint32_t delay) */
    .global dedic_write_bits32_lut_d
    .type   dedic_write_bits32_lut_d, @function
    .balign 16
dedic_write_bits32_lut_d:
    li      t1, M_CLK
    .rept 32
    lw      t3, 0(a0)              /* 数据位（CLK=0 的预置值） */
    csrw    CSR_OUT, t3            /* 下降沿 + 换数据 */
    mv      t5, a1
    beqz    t5, 2f
1:  addi    t5, t5, -1
    bnez    t5, 1b
2:  csrrs   zero, CSR_OUT, t1      /* 上升沿 */
    mv      t5, a1
    beqz    t5, 4f
3:  addi    t5, t5, -1
    bnez    t5, 3b
4:  addi    a0, a0, 4
    .endr
    ret

/* void dedic_write_bit1_d(uint32_t bit, uint32_t delay) */
    .global dedic_write_bit1_d
    .type   dedic_write_bit1_d, @function
    .balign 16
dedic_write_bit1_d:
    andi    t3, a0, 1
    slli    t3, t3, 1
    csrw    CSR_OUT, t3
    mv      t5, a1
    beqz    t5, 2f
1:  addi    t5, t5, -1
    bnez    t5, 1b
2:  csrrsi  zero, CSR_OUT, M_CLK
    mv      t5, a1
    beqz    t5, 4f
3:  addi    t5, t5, -1
    bnez    t5, 3b
4:  ret

/* uint32_t dedic_read_bits32_d(uint32_t delay)
 *   采样放在**低电平期间**（与慢速实现同语义：目标在下降沿驱动，等够了再采），
 *   这也是实机上验证过能用的采样点。
 *
 * 🚨 延时在 **a0**（本函数只有一个参数！），先搬进 t6 再拿 a0 当累加器。
 *    老版本写的是 `mv t5, a1` —— 而 a1 是**调用者的第二个参数**，本函数根本没有它。
 *    读路径当时能跑纯属侥幸：SWD_Read_fast 在调用它之前刚调过 write_bits8()，
 *    而 write_bits8 的最后一条调用把 d 留在了 a1 里，正好等于该传的延时。
 *    换句话说：**CodeGen 一改（或调用顺序一变）读时序就会悄悄变成别的时间**，
 *    而且不报错、不崩溃 —— 属于必须掐死的定时炸弹。 */
    .global dedic_read_bits32_d
    .type   dedic_read_bits32_d, @function
    .balign 16
dedic_read_bits32_d:
    li      t1, M_CLK
    mv      t6, a0                 /* t6 = delay（a0 是唯一参数） */
    li      a0, 0
    .rept 32
    csrw    CSR_OUT, zero          /* 下降沿（DIO 已高阻，由目标驱动） */
    mv      t5, t6
    beqz    t5, 2f
1:  addi    t5, t5, -1
    bnez    t5, 1b
2:  csrr    t3, CSR_IN             /* 采样 */
    csrrs   zero, CSR_OUT, t1      /* 上升沿 */
    andi    t3, t3, 2              /* in ch1 = SWDIO */
    srli    a0, a0, 1
    slli    t3, t3, 30
    or      a0, a0, t3
    mv      t5, t6
    beqz    t5, 4f
3:  addi    t5, t5, -1
    bnez    t5, 3b
4:
    .endr
    ret

/* void dedic_clock_n_d(uint32_t n, uint32_t delay) —— n 个空时钟（带延时）
 *
 * 🚨 这里踩过一个非常隐蔽的坑：第二个延时循环原来写成
 *        beqz t5, 8f / 8: addi t5,t5,-1 / bnez t5,8b
 *    —— `8:` 本身就是循环体，delay=0 时 `beqz` 跳进去把 t5 减成 0xFFFFFFFF，
 *    于是**循环 40 亿次（关中断下约 25 秒）**：USB 不应答、引脚不出波形，
 *    看起来就像"探针跑高速跑死了"（实测满速档第一次调用它就是这个下场）。
 *    "跳过循环"的目标标签**必须在循环体之外**（这里是 10:）。 */
    .global dedic_clock_n_d
    .type   dedic_clock_n_d, @function
    .balign 16
dedic_clock_n_d:
    beqz    a0, 9f
    li      t1, M_CLK
5:  csrw    CSR_OUT, zero
    mv      t5, a1
    beqz    t5, 7f
6:  addi    t5, t5, -1
    bnez    t5, 6b
7:  csrrs   zero, CSR_OUT, t1
    mv      t5, a1
    beqz    t5, 10f
8:  addi    t5, t5, -1
    bnez    t5, 8b
10: addi    a0, a0, -1
    bnez    a0, 5b
9:  ret

/* ==================================================================
 *  正式原语
 * ================================================================== */

/* void dedic_write_bits32_lut(const uint32_t *lut)
 *   32 bit，LSB first。每 bit 只有 3 条指令：
 *     lw    —— 取“CLK=0 + DIO=bit”的预置 OUT 值
 *     csrw  —— 下降沿 + 换数据（顺手把 CLK 位清零）
 *     csrrs —— 上升沿：**只置 CLK 位**，数据位不动
 *   实测 14.09 cyc/bit = 22.7 MHz（旧查表写法 19.9 cyc/bit）。
 *   lut 必须放 IRAM（DRAM_ATTR）——关中断期间取表不能走 flash。 */
    .global dedic_write_bits32_lut
    .type   dedic_write_bits32_lut, @function
    .balign 16
dedic_write_bits32_lut:
    li      t1, M_CLK
    .set    L32OFF, 0
    lw      t3, L32OFF(a0)
    .rept 31
    .set    L32OFF, L32OFF + 4
    csrw    CSR_OUT, t3
    csrrs   zero, CSR_OUT, t1
    lw      t3, L32OFF(a0)
    .endr
    csrw    CSR_OUT, t3
    csrrs   zero, CSR_OUT, t1
    ret

/* uint32_t dedic_read_bits32_fast(void) —— 32 bit，LSB first，返回组装好的值
 *   csrw 下降沿 / csrrs 上升沿（目标驱动）/ csrr 采样 + 4 条组装指令。 */
    .global dedic_read_bits32_fast
    .type   dedic_read_bits32_fast, @function
    .balign 16
dedic_read_bits32_fast:
    li      t1, M_CLK
    li      a0, 0
    .rept 32
    csrw    CSR_OUT, zero          /* 下降沿 */
    csrrs   zero, CSR_OUT, t1      /* 上升沿：目标在这一沿之后驱动 SWDIO */
    csrr    t3, CSR_IN             /* 采样（高电平末尾） */
    andi    t3, t3, 2              /* in ch1 = SWDIO */
    srli    a0, a0, 1              /* LSB first 组装 */
    slli    t3, t3, 30             /* bit1 -> bit31 */
    or      a0, a0, t3
    .endr
    ret

/* ==================================================================
 *  🚀 极限速度版原语（配 DEDIC_SWD_MAX_SPEED）
 *
 *  与上面"带延时"版本的区别：**没有任何延时循环、没有任何条件判断**。
 *  每个相位都是直的：写 CSR →（固定的 nop 填充）→ 采样 / 下一个 CSR 写。
 *  代价是不可调速 —— 要降速就切回带延时的版本（编译期开关）。
 *
 *  🚨 采样点由 SAMPLE_NOPS_* 决定：目标是在 SWCLK **下降沿**之后才开始驱动
 *     SWDIO 的，"填几条 nop"就是"等它多久"。这两个常数是**唯一**需要按目标
 *     微调的地方（每条 nop = 1 周期 = 3.125 ns，另加 CSR 写生效的 ~5 周期）。
 * ================================================================== */
    .equ SAMPLE_NOPS_ACK,  4        /* ACK 相位：沿后 ~4 周期再采 */
    .equ SAMPLE_NOPS_DATA, 4        /* 数据相位：同上（>=4 走"流水版"，<4 走"直落版"） */

/* void dedic_x_clock1(void) —— 1 个空时钟（turnaround 用），无分支 */
    .global dedic_x_clock1
    .type   dedic_x_clock1, @function
    .balign 16
dedic_x_clock1:
    csrw    CSR_OUT, zero
    csrrsi  zero, CSR_OUT, 1
    ret

/* uint32_t dedic_x_read_ack3(void) —— turnaround + 3 位 ACK（LSB first），返回 0..7 */
    .global dedic_x_read_ack3
    .type   dedic_x_read_ack3, @function
    .balign 16
dedic_x_read_ack3:
    csrw    CSR_OUT, zero
    csrrsi  zero, CSR_OUT, 1        /* turnaround 时钟 */
    li      a0, 0
    .rept 3
    csrw    CSR_OUT, zero           /* 下降沿：目标开始驱动这一位 */
    .rept SAMPLE_NOPS_ACK
    nop
    .endr
    csrr    t3, CSR_IN              /* 采样 */
    csrrsi  zero, CSR_OUT, 1        /* 上升沿 */
    andi    t3, t3, 2               /* in ch1 = SWDIO */
    srli    a0, a0, 1               /* LSB first：先采到的进低位 */
    slli    t3, t3, 1               /* bit1 -> bit2 */
    or      a0, a0, t3
    .endr
    ret

/* uint32_t dedic_x_read_bit(void) —— 1 个采样时钟（校验位用），返回 0/1 */
    .global dedic_x_read_bit
    .type   dedic_x_read_bit, @function
    .balign 16
dedic_x_read_bit:
    csrw    CSR_OUT, zero
    .rept SAMPLE_NOPS_DATA
    nop
    .endr
    csrr    a0, CSR_IN
    csrrsi  zero, CSR_OUT, 1
    andi    a0, a0, 2
    srli    a0, a0, 1
    ret

/* uint32_t dedic_x_read_bits32(void) —— 32 bit，LSB first，无分支无延时循环
 *
 * 🚀 两种写法，按 SAMPLE_NOPS_DATA 编译期选：
 *   >=4 —— **流水版**：把上一拍的 5 条组装指令塞进"等目标"的空档，采样点不变但每位省 ~4 周期。
 *   < 4 —— **直落版**：等够 nops 就采、采完再组装（等待窗口里塞不下组装指令时用这个）。
 *
 * 📌 采样点要等多久，**取决于目标**：LA 实测 STM32F103 的 SWDIO 是**紧贴下降沿**换的
 *    （DIO 跳变相对下降沿中位 -10 ns，±66 ns 是 2ns 量化误差），所以等待可以压到很小；
 *    慢目标就把它调大。这是整份代码里唯一真正"按目标标定"的常数。 */
    .global dedic_x_read_bits32
    .type   dedic_x_read_bits32, @function
    .balign 16
dedic_x_read_bits32:
    li      a0, 0
.if SAMPLE_NOPS_DATA >= 4
    /* ---- 流水版 ---- */
    csrw    CSR_OUT, zero           /* 第 1 位：只采样，没有上一拍要组装 */
    .rept SAMPLE_NOPS_DATA
    nop
    .endr
    csrr    t3, CSR_IN
    csrrsi  zero, CSR_OUT, 1
    andi    t3, t3, 2
    .rept 31
    csrw    CSR_OUT, zero           /* 下降沿 */
    .rept SAMPLE_NOPS_DATA - 4
    nop
    .endr
    srli    a0, a0, 1               /* ← 上一拍的组装（藏在这里） */
    slli    t3, t3, 30
    or      a0, a0, t3
    csrr    t3, CSR_IN              /* 采样 */
    csrrsi  zero, CSR_OUT, 1
    andi    t3, t3, 2
    .endr
    srli    a0, a0, 1               /* 最后一拍的组装 */
    slli    t3, t3, 30
    or      a0, a0, t3
.else
    /* ---- 直落版 ---- */
    .rept 32
    csrw    CSR_OUT, zero
    .rept SAMPLE_NOPS_DATA
    nop
    .endr
    csrr    t3, CSR_IN
    csrrsi  zero, CSR_OUT, 1
    andi    t3, t3, 2
    srli    a0, a0, 1
    slli    t3, t3, 30
    or      a0, a0, t3
    .endr
.endif
    ret

/* 注意：写 32 位查表版 / n 个空时钟 直接用现成的
 * dedic_write_bits32_lut() 和 dedic_clock_n()（本来就是无分支展开的）。 */

/* ==================================================================
 *  🚀🚀 整笔传输合成版：一个函数跑完一整笔 SWD 传输
 *
 *  背景（LA 定量实测）：字间固件开销 140~160 周期，占每字 12% —— 里面是
 *  "从 C 调 6 次子函数（jal/ret 各 ~8 周期）+ C 的进出场 + 包头计算 + 分支"。
 *  合并成一个汇编函数后这些全没了：**不用保存任何 s 寄存器、不用栈**。
 * ================================================================== */

/* uint32_t dedic_x_read_word(uint32_t request, uint32_t *out) */
    .global dedic_x_read_word
    .type   dedic_x_read_word, @function
    .balign 16
dedic_x_read_word:
    /* --- 包头 = 0x81 | ((req&15)<<1) | (parity4(req&15)<<5) --- */
    andi    t0, a0, 15
    srli    t1, t0, 2
    xor     t1, t1, t0
    srli    t3, t1, 1
    xor     t1, t1, t3
    andi    t1, t1, 1
    slli    t1, t1, 5
    slli    t0, t0, 1
    or      t1, t1, t0
    ori     a0, t1, 0x81
    mv      t6, a1                 /* out 指针（可为 NULL） */
    /* --- 8 位包头 --- */
    .rept 8
    andi    t3, a0, 1
    slli    t3, t3, 1
    csrw    CSR_OUT, t3            /* 下降沿 + 数据 */
    ori     t4, t3, M_CLK
    srli    a0, a0, 1
    csrw    CSR_OUT, t4            /* 上升沿 */
    .endr
    li      t0, M_DIO
    csrs    CSR_OEN, t0            /* 释放 SWDIO（turnaround） */
    /* --- turnaround + 3 位 ACK → a1 --- */
    csrw    CSR_OUT, zero
    csrrsi  zero, CSR_OUT, 1
    li      a1, 0
    .rept 3
    csrw    CSR_OUT, zero
    .rept SAMPLE_NOPS_ACK
    nop
    .endr
    csrr    t3, CSR_IN             /* 采样 */
    csrrsi  zero, CSR_OUT, 1
    andi    t3, t3, 2
    srli    a1, a1, 1
    slli    t3, t3, 1
    or      a1, a1, t3
    .endr
    li      t0, 1
    bne     a1, t0, .Lxrw_ret      /* 非 OK：原样返回，总线后续交给 C 侧 */
    /* --- 32 位数据 → t5（LSB first）---
     * 🚀 这里**不掩码**：`slli t3,t3,30` 把 IN 的 bit1(SWDIO) 挪到 bit31，
     *    同时把 bit0(SWCLK) 挪到 bit30、bit2~7 全移出去。而采样瞬间 SWCLK 一定是低
     *    （前一条 csrw 刚把它拉低，等了几条 nop 才采），所以 bit30 恒为 0 ——
     *    省掉的那条 `andi t3,t3,2` 就是每位 1 个周期（32 位 = 32 周期）。
     *    ⚠️ 这条推理的前提是"CLK 回读为 0"；万一不成立，数据会成片出错，
     *       16KB 逐字节校验会立刻发现（不会静默劣化）。 */
    li      t5, 0
.if SAMPLE_NOPS_DATA >= 4
    /* 流水版：上一拍的组装藏进"等目标"的窗口 */
    csrw    CSR_OUT, zero
    .rept SAMPLE_NOPS_DATA
    nop
    .endr
    csrr    t3, CSR_IN
    csrrsi  zero, CSR_OUT, 1
    .rept 31
    csrw    CSR_OUT, zero
    .rept SAMPLE_NOPS_DATA - 4
    nop
    .endr
    srli    t5, t5, 1
    slli    t3, t3, 30
    or      t5, t5, t3
    csrr    t3, CSR_IN
    csrrsi  zero, CSR_OUT, 1
    .endr
    srli    t5, t5, 1
    slli    t3, t3, 30
    or      t5, t5, t3
.else
    .rept 32
    csrw    CSR_OUT, zero
    .rept SAMPLE_NOPS_DATA
    nop
    .endr
    csrr    t3, CSR_IN
    csrrsi  zero, CSR_OUT, 1
    srli    t5, t5, 1
    slli    t3, t3, 30
    or      t5, t5, t3
    .endr
.endif
    /* --- 校验位 → t4 --- */
    csrw    CSR_OUT, zero
    .rept SAMPLE_NOPS_DATA
    nop
    .endr
    csrr    t4, CSR_IN
    csrrsi  zero, CSR_OUT, 1
    srli    t4, t4, 1
    andi    t4, t4, 1
    /* --- 存数据（校验错也存，与慢速参考实现一致）--- */
    beqz    t6, .Lxrw_nostore
    sw      t5, 0(t6)
.Lxrw_nostore:
    /* --- parity32(t5) ^ 收到的校验位 → 错则 ack = 8 --- */
    srli    t1, t5, 16
    xor     t1, t1, t5
    srli    t3, t1, 8
    xor     t1, t1, t3
    srli    t3, t1, 4
    xor     t1, t1, t3
    andi    t1, t1, 15
    li      t3, 0x6996
    srl     t1, t3, t1
    andi    t1, t1, 1
    xor     t1, t1, t4
    andi    t1, t1, 1
    beqz    t1, .Lxrw_fin
    li      a1, 8                  /* DAP_TRANSFER_ERROR */
.Lxrw_fin:
    /* --- turnaround 回来 + 接管 + 空闲（CLK 低、SWDIO 驱动高）--- */
    csrw    CSR_OUT, zero
    csrrsi  zero, CSR_OUT, 1
    li      t0, M_DIO
    csrc    CSR_OEN, t0
    csrw    CSR_OUT, t0
.Lxrw_ret:
    mv      a0, a1
    ret

/* uint32_t dedic_x_write_word(uint32_t request, const uint32_t *lut)
 *   lut[0..32]：第 i 项 = "CLK=0 + DIO=第 i 位" 的 OUT 值，第 32 项 = 校验位 */
    .global dedic_x_write_word
    .type   dedic_x_write_word, @function
    .balign 16
dedic_x_write_word:
    andi    t0, a0, 15
    srli    t1, t0, 2
    xor     t1, t1, t0
    srli    t3, t1, 1
    xor     t1, t1, t3
    andi    t1, t1, 1
    slli    t1, t1, 5
    slli    t0, t0, 1
    or      t1, t1, t0
    ori     a0, t1, 0x81
    mv      t6, a1                 /* LUT 基址 */
    .rept 8
    andi    t3, a0, 1
    slli    t3, t3, 1
    csrw    CSR_OUT, t3
    ori     t4, t3, M_CLK
    srli    a0, a0, 1
    csrw    CSR_OUT, t4
    .endr
    li      t0, M_DIO
    csrs    CSR_OEN, t0
    csrw    CSR_OUT, zero
    csrrsi  zero, CSR_OUT, 1
    li      a1, 0
    .rept 3
    csrw    CSR_OUT, zero
    .rept SAMPLE_NOPS_ACK
    nop
    .endr
    csrr    t3, CSR_IN
    csrrsi  zero, CSR_OUT, 1
    andi    t3, t3, 2
    srli    a1, a1, 1
    slli    t3, t3, 1
    or      a1, a1, t3
    .endr
    li      t0, 1
    bne     a1, t0, .Lxww_ret      /* 非 OK：原样返回，总线后续交给 C 侧 */
    /* --- turnaround + 接管 --- */
    csrw    CSR_OUT, zero
    csrrsi  zero, CSR_OUT, 1
    li      t0, M_DIO
    csrc    CSR_OEN, t0
    /* --- 33 位查表输出（32 数据 + 校验位）：用 .set 造立即数偏移，省掉 addi --- */
    .set    XWOFF, 0
    .rept 33
    lw      t3, XWOFF(t6)
    csrw    CSR_OUT, t3            /* 下降沿 + 换数据 */
    csrrsi  zero, CSR_OUT, 1       /* 上升沿（立即数形式） */
    .set    XWOFF, XWOFF + 4
    .endr
    /* --- 空闲 --- */
    li      t0, M_DIO
    csrw    CSR_OUT, t0
    li      a1, 1
.Lxww_ret:
    mv      a0, a1
    ret

/* void dedic_write_bit1(uint32_t bit) —— 只发 1 位（数据校验位用） */
    .global dedic_write_bit1
    .type   dedic_write_bit1, @function
    .balign 16
dedic_write_bit1:
    andi    t3, a0, 1
    slli    t3, t3, 1              /* t3 = DIO 值（0 或 2） */
    csrw    CSR_OUT, t3            /* 拉低 + 数据 */
    csrrsi  zero, CSR_OUT, M_CLK   /* 拉高（立即数形式，不用先装寄存器） */
    ret

/* void dedic_write_bits8(uint32_t value)
 *   8 bit，LSB first：低位先出。上升沿目标采样，下降沿换数据。 */
    .global dedic_write_bits8
    .type   dedic_write_bits8, @function
    .balign 16
dedic_write_bits8:
    .rept 8
    andi    t3, a0, 1
    slli    t3, t3, 1            /* t3 = DIO 值（0 或 2） */
    csrw    CSR_OUT, t3          /* 拉低 + 数据 */
    ori     t4, t3, M_CLK
    srli    a0, a0, 1
    csrw    CSR_OUT, t4          /* 拉高：目标在这一沿采样 */
    .endr
    ret

/* void dedic_write_bits8_d(uint32_t value, uint32_t delay)
 *   包头专用：8 bit + 两相各带延时。**必须用汇编版**：
 *   C 版 write_bits8() 每位要一次函数调用 + 一次 cur_delay()，
 *   实测每 8 拍就多出 228ns 的"字节边界"（106ns/bit → 平均被拖到 130ns+），
 *   而且把时钟换算（bit ≈ 14 + 4·d）也搞歪了。 */
    .global dedic_write_bits8_d
    .type   dedic_write_bits8_d, @function
    .balign 16
dedic_write_bits8_d:
    li      t1, M_CLK
    .rept 8
    andi    t3, a0, 1
    slli    t3, t3, 1
    csrw    CSR_OUT, t3          /* 下降沿 + 数据 */
    mv      t5, a1
    beqz    t5, 2f
1:  addi    t5, t5, -1
    bnez    t5, 1b
2:  ori     t4, t3, M_CLK
    srli    a0, a0, 1
    csrw    CSR_OUT, t4          /* 上升沿 */
    mv      t5, a1
    beqz    t5, 4f
3:  addi    t5, t5, -1
    bnez    t5, 3b
4:
    .endr
    ret

/* uint32_t dedic_read_ack3_d(uint32_t delay)
 *   1 个 turnaround 时钟 + 3 位 ACK（LSB first），返回 0..7。
 *   采样点放在**低电平末尾**（目标在下降沿开始驱动，等够了再采）。 */
    .global dedic_read_ack3_d
    .type   dedic_read_ack3_d, @function
    .balign 16
dedic_read_ack3_d:
    li      t1, M_CLK
    mv      t6, a0               /* t6 = delay */
    csrw    CSR_OUT, zero        /* ---- turnaround 时钟（不采样）---- */
    mv      t5, t6
    beqz    t5, 2f
1:  addi    t5, t5, -1
    bnez    t5, 1b
2:  csrrs   zero, CSR_OUT, t1
    mv      t5, t6
    beqz    t5, 4f
3:  addi    t5, t5, -1
    bnez    t5, 3b
4:  li      a0, 0
    .rept 3
    csrw    CSR_OUT, zero        /* 下降沿 */
    mv      t5, t6
    beqz    t5, 6f
5:  addi    t5, t5, -1
    bnez    t5, 5b
6:  csrr    t3, CSR_IN           /* 采样 */
    csrrs   zero, CSR_OUT, t1    /* 上升沿 */
    andi    t3, t3, 2            /* in ch1 = SWDIO */
    srli    a0, a0, 1            /* LSB first：先采到的进低位 */
    slli    t3, t3, 1            /* bit1 -> bit2 */
    or      a0, a0, t3
    mv      t5, t6
    beqz    t5, 8f
7:  addi    t5, t5, -1
    bnez    t5, 7b
8:
    .endr
    ret

/* uint32_t dedic_read_bit_d(uint32_t delay) —— 1 个采样时钟（校验位用），返回 0/1 */
    .global dedic_read_bit_d
    .type   dedic_read_bit_d, @function
    .balign 16
dedic_read_bit_d:
    li      t1, M_CLK
    mv      t6, a0
    csrw    CSR_OUT, zero        /* 下降沿 */
    mv      t5, t6
    beqz    t5, 2f
1:  addi    t5, t5, -1
    bnez    t5, 1b
2:  csrr    a0, CSR_IN           /* 采样 */
    csrrs   zero, CSR_OUT, t1    /* 上升沿 */
    andi    a0, a0, 2
    srli    a0, a0, 1
    mv      t5, t6
    beqz    t5, 4f
3:  addi    t5, t5, -1
    bnez    t5, 3b
4:  ret

/* void dedic_write_bits32(uint32_t value) —— 32 bit，LSB first（数据段 + 校验位由 C 拼好） */
    .global dedic_write_bits32
    .type   dedic_write_bits32, @function
    .balign 16
dedic_write_bits32:
    .rept 32
    andi    t3, a0, 1
    slli    t3, t3, 1
    csrw    CSR_OUT, t3
    ori     t4, t3, M_CLK
    srli    a0, a0, 1
    csrw    CSR_OUT, t4
    .endr
    ret

/* uint32_t dedic_read_bits32(void) —— 32 bit，LSB first，返回组装好的值
 *
 * 🚨 采样时机：目标在上一个上升沿之后才驱动 SWDIO，所以**不能在拉高后立刻读**。
 *    这里把采样放在本 bit 高电平的末尾（紧挨下一次拉低之前），
 *    并用“软件流水”把上一位的提取放到下一位的低电平期间 —— 采样晚、提取不额外花时间。 */
    .global dedic_read_bits32
    .type   dedic_read_bits32, @function
    .balign 16
dedic_read_bits32:
    li      t2, 0                /* CLK 低：OUT = 0（SWDIO 高阻，由目标驱动） */
    li      t1, M_CLK            /* CLK 高 */
    li      a0, 0
    .rept 32
    csrw    CSR_OUT, t2          /* 拉低 */
    csrw    CSR_OUT, t1          /* 拉高 */
    csrr    t3, CSR_IN           /* 采样（高电平末尾） */
    srli    t3, t3, 1            /* in ch1 = SWDIO */
    andi    t3, t3, 1
    srli    a0, a0, 1            /* LSB first 组装 */
    slli    t3, t3, 31
    or      a0, a0, t3
    .endr
    ret

/* void dedic_clock_n(uint32_t n) —— n 个 SWCLK 周期（turnaround / idle 用），保持 OUT=0 */
    .global dedic_clock_n
    .type   dedic_clock_n, @function
    .balign 16
dedic_clock_n:
    li      t1, M_CLK
1:  csrw    CSR_OUT, zero
    csrw    CSR_OUT, t1
    addi    a0, a0, -1
    bnez    a0, 1b
    ret

/* void dedic_dio_release(void) —— SWDIO 切高阻（OEN bit1 = 1），交给目标驱动 */
    .global dedic_dio_release
    .type   dedic_dio_release, @function
    .balign 16
dedic_dio_release:
    li      t0, M_DIO
    csrrs   zero, CSR_OEN, t0
    ret

/* void dedic_dio_drive(void) —— SWDIO 恢复推挽输出（OEN bit1 = 0） */
    .global dedic_dio_drive
    .type   dedic_dio_drive, @function
    .balign 16
dedic_dio_drive:
    li      t0, M_DIO
    csrrc   zero, CSR_OEN, t0
    ret

/* void dedic_clk_idle(void) —— SWCLK 拉低、SWDIO 由主机驱动为高（SWD 空闲态） */
    .global dedic_clk_idle
    .type   dedic_clk_idle, @function
    .balign 16
dedic_clk_idle:
    li      t0, M_DIO
    csrw    CSR_OUT, t0
    csrrc   zero, CSR_OEN, t0
    ret
