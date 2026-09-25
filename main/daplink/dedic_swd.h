/*
 * dedic_swd.h —— dedicated GPIO 高速 SWD 位时序（ESP32-S31）
 *
 * 分层：
 *   dedic_swd.S     —— 纯汇编原语（全部 .iram1，展开，无循环）
 *   SW_DP_fast.c    —— 用原语拼出 SWD 一次传输（header/turnaround/ACK/data/parity）
 *   SW_DP.c         —— 对外 API + 高速/慢速切换
 *
 * 🚨 关中断：整段传输在 mstatus.MIE=0 下跑（这是它不受 USB 中断抖动影响的原因）。
 *    单次传输只有几微秒，注意别在临界区里做别的事。
 */
#pragma once

#include <stdint.h>

/* ---- dedic_swd.S ---- */
void     dedic_write_bit1(uint32_t bit);                 /* 1 bit（校验位用） */
void     dedic_write_bits8(uint32_t value);              /* 8 bit，LSB first */
/* 带延时的参数化版本（delay = 每个相位烧几次 2 周期循环；0 = 跑满速） */
void     dedic_write_bit1_d(uint32_t bit, uint32_t delay);
void     dedic_write_bits8_d(uint32_t value, uint32_t delay);   /* 包头：8 bit + 延时 */
void     dedic_write_bits32_lut_d(const uint32_t *lut, uint32_t delay);
uint32_t dedic_read_bits32_d(uint32_t delay);
uint32_t dedic_read_ack3_d(uint32_t delay);              /* turnaround + 3 位 ACK → 0..7 */
uint32_t dedic_read_bit_d(uint32_t delay);               /* 1 个采样时钟 → 0/1（校验位） */
void     dedic_clock_n_d(uint32_t n, uint32_t delay);
/* ---- 🚀 极限速度版（DEDIC_SWD_MAX_SPEED）：无分支、无延时循环 ----
 * 采样点靠固定的 nop 填充（dedic_swd.S 里的 SAMPLE_NOPS_ACK/DATA 可调）。
 * 写 32 位查表 / n 个空时钟 直接复用 dedic_write_bits32_lut() / dedic_clock_n()。 */
void     dedic_x_clock1(void);
uint32_t dedic_x_read_ack3(void);
uint32_t dedic_x_read_bit(void);
uint32_t dedic_x_read_bits32(void);

/* ---- 🚀🚀 整笔传输合成版：一个函数跑完一整笔 SWD 传输 ----
 * 目的：把"包头 + release + ACK + 数据 + 校验 + turnaround + 接管 + 空闲"全塞进一个
 * 汇编函数里 —— 原来每笔要从 C 调 6 次子函数（每次 jal/ret + 进出场 ~8 周期），
 * 再加 C 的进出场/包头计算，实测字间要花 140~160 周期（占每字 12%）。
 *
 * 返回值：ACK（1=OK 2=WAIT 4=FAULT 7=NO-ACK 8=校验错）
 * 非 OK 时**不碰总线后续**（保持"CLK 高、SWDIO 高阻"），交给 C 侧按参考实现补
 * dummy 数据相位 / turnaround / 协议错退相位。
 * read：out 可为 NULL；ACK=OK 或校验错时写 *out（与慢速参考实现一致）。
 * write：lut[0..32]，第 i 项 = "CLK=0 + DIO=第 i 位" 的 OUT 值，第 32 项 = 校验位。 */
uint32_t dedic_x_read_word(uint32_t request, uint32_t *out);
uint32_t dedic_x_write_word(uint32_t request, const uint32_t *lut);
void     dedic_write_bits32(uint32_t value);             /* 32 bit，LSB first（直接移位版） */
void     dedic_write_bits32_lut(const uint32_t *lut);    /* 32 bit，查表版：14.09 cyc/bit */
uint32_t dedic_read_bits32(void);                        /* 32 bit，LSB first（直接版） */
uint32_t dedic_read_bits32_fast(void);                   /* 32 bit，LSB first（3 条 CSR 版） */
void     dedic_clock_n(uint32_t n);                      /* n 个空时钟周期 */
void     dedic_dio_release(void);                        /* SWDIO 切高阻（turnaround） */
void     dedic_dio_drive(void);                          /* SWDIO 恢复推挽 */
void     dedic_clk_idle(void);                           /* 空闲态：CLK 低、DIO 驱动高 */

/* ---- SW_DP_fast.c ---- */
uint8_t  SWD_Write_fast(uint32_t request, uint32_t *data);
uint8_t  SWD_Read_fast(uint32_t request, uint32_t *data);

/* 建 dedicated GPIO bundle（fast 模式下由 PORT_SWD_SETUP() 调用）。
 * 🚨 每次调用都会**重新建**（先 del 再 new）：因为 PORT_OFF()/gpio_set_direction()
 *    会把焊盘的输出路由从 dedicated GPIO 上扯下来，只有重建才能装回去。
 * 建好之后这两个焊盘就归 dedicated GPIO 了，普通 GPIO 写不再到得了焊盘。 */
void     dedic_swd_pins_setup(void);
/* 释放（两脚高阻）——给 PORT_OFF 用：只动 OEN CSR，不碰焊盘函数选择 */
void     dedic_swd_pins_release(void);
int      dedic_swd_pins_ready(void);
/* 通道掩码/输入位（自检用） */
uint32_t dedic_swd_clk_mask(void);
uint32_t dedic_swd_dio_mask(void);
uint32_t dedic_swd_in_bit(void);
/* 诊断：CSR + 焊盘路由寄存器快照（不产生时钟）；焊盘驱动/回读自检（会产生两个时钟！） */
void     dedic_swd_state_dump(const char *where);
uint32_t dedic_swd_pad_probe(void);
