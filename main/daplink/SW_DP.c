/*
 * SW_DP.c —— SWD 时序层对外 API + 高速/慢速切换
 *
 * 两个实现并存：
 *   SW_DP_fast.c  dedicated GPIO + 展开汇编（实测写 22.7 MHz / 读 ~18 MHz）
 *   SW_DP_slow.c  普通 GPIO + 延时循环（旧版，等效 ~0.3 MHz）
 *
 * ⚠️ 切换是**编译期**的（DAP_USE_DEDIC_GPIO）：高速模式会把 SWCLK/SWDIO 这两个焊盘
 *    交给 dedicated GPIO，此时普通 GPIO 写根本到不了焊盘，慢速实现就"静默失效"了
 *    （周期数好看但引脚不动 —— dedic_gpio_bench 踩过这个坑）。
 *
 * 这里只是转发层，真正的位时序在 SW_DP_fast.c / dedic_swd.S（都是 IRAM）。
 */
#include "DAP_config.h"
#include "DAP.h"
#include "dedic_swd.h"

extern void     SWJ_Sequence_slow(uint32_t count, const uint8_t *data);
extern void     SWD_Sequence_slow(uint32_t info, const uint8_t *swdo, uint8_t *swdi);
extern uint8_t  SWD_Write_slow(uint32_t request, uint32_t *data);
extern uint8_t  SWD_Read_slow(uint32_t request, uint32_t *data);

/* 任意位序列：主机单向输出（DAP_SWJ_Sequence 命令）。
 * 🚨 fast 模式下必须也走高速路径 —— 慢速实现用普通 GPIO 写，而这两个焊盘此时
 *    归 dedicated GPIO，那些写到不了焊盘（激活序列失效 → 全部 NO ACK）。 */
#if DAP_USE_DEDIC_GPIO
extern void SWJ_Sequence_fast(uint32_t count, const uint8_t *data);
void SWJ_Sequence(uint32_t count, const uint8_t *data)
{
    SWJ_Sequence_fast(count, data);
}
#else
void SWJ_Sequence(uint32_t count, const uint8_t *data)
{
    SWJ_Sequence_slow(count, data);
}
#endif

void SWD_Sequence(uint32_t info, const uint8_t *swdo, uint8_t *swdi)
{
    SWD_Sequence_slow(info, swdo, swdi);
}

#if DAP_USE_DEDIC_GPIO

uint8_t SWD_Write(uint32_t request, uint32_t *data)
{
    return SWD_Write_fast(request, data);
}

uint8_t SWD_Read(uint32_t request, uint32_t *data)
{
    return SWD_Read_fast(request, data);
}

#else

uint8_t SWD_Write(uint32_t request, uint32_t *data)
{
    return SWD_Write_slow(request, data);
}

uint8_t SWD_Read(uint32_t request, uint32_t *data)
{
    return SWD_Read_slow(request, data);
}

#endif  /* DAP_USE_DEDIC_GPIO */
