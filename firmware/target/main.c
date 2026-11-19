/* ============================================================================
 *  Timing controller v3 - three synchronized output channels (PA1 / PA2 / PA3)
 *  MCU : STM32F103C8T6 @ 72 MHz
 *  Peripheral: TIM2 only (all three channels share one time base)
 * ============================================================================
 *  Target waveform (one period = 3 ticks = 6 us):
 *
 *      time(us):  0 -------- 2 -------- 4 -------- 6
 *      CH1      :  |----------------------|            (channel 1)
 *      CH2      :  |----------|                         (channel 2)
 *      CH3      :                      |----------|      (channel 3)
 *
 *  Definition:
 *    1) All channels are driven by a single timer, so no software phase
 *       correction is needed.
 *    2) Period start (t=0): CH1 goes high, clears after 2 ticks (4 us).
 *    3) CH2 goes high at period start and clears after 1 tick (2 us).
 *    4) CH3 goes high 2 ticks into the period (4 us) and clears 1 tick later.
 *    5) One period = 3 ticks, 1 tick = 2 us  ->  6 us total.
 *
 *  Pins (v3: only PA1 / PA2 / PA3 are used):
 *    PA1 = CH1 (TIM2_CH2)
 *    PA2 = CH2 (TIM2_CH3)
 *    PA3 = CH3 (TIM2_CH4)
 *
 *  Why a single timer is enough:
 *    In an earlier revision the third channel's active window sat in the
 *    MIDDLE of the period. Standard PWM can only anchor a window at the start
 *    (PWM1) or the end (PWM2) of a period, so a middle window forced us to use
 *    a second timer with a phase preset. By moving that window to the END of
 *    the period, PWM2 generates it directly and one TIM2 does all the work.
 *
 *  Clock math (f_cnt = 72 MHz, PSC = 0):
 *    1 tick   = 2 us = 144 counts
 *    period   = 3 ticks = 432 counts -> ARR = 431
 *    CH1 high [0, 288)   -> PWM1, CCR2 = 288
 *    CH2 high [0, 144)   -> PWM1, CCR3 = 144
 *    CH3 high [288, 432) -> PWM2, CCR4 = 288
 *    CH1 falling edge and CH3 rising edge occur at the same count (288), so
 *    they are aligned by hardware rather than by software delay.
 *
 *  GPIO speed: 2 MHz drive setting (not 50 MHz) for clean edges on long wires.
 * ========================================================================== */

#include "stm32f10x.h"

/* ---------- Tunable parameters ---------- */
#define TIM_CLK_HZ    72000000UL
#define TICK_US       2UL                     /* 2 us per tick */
#define CNT_PER_TICK  (TIM_CLK_HZ / 1000000UL * TICK_US)   /* = 144 */
#define ACTIVE_TICKS  3UL
#define IDLE_TICKS    0UL                     /* Must stay 0! See note B below:
                                                  with PWM2, lengthening the
                                                  period stretches the third
                                                  channel high to period end. */

#define PERIOD_CNT    (CNT_PER_TICK * (ACTIVE_TICKS + IDLE_TICKS))  /* 432 */
#define ARR_VAL       (PERIOD_CNT - 1U)                             /* 431 */

#define CCR_CH1       (CNT_PER_TICK * 2U)     /* CH1 high 2 ticks = 288 */
#define CCR_CH2       (CNT_PER_TICK * 1U)     /* CH2 high 1 tick  = 144 */
#define CCR_CH3       (CNT_PER_TICK * 2U)     /* CH3: PWM2, high when CNT>=288 */

/* GPIO speed settings: 2 MHz drive to suppress ringing */
#define GPIO_SPEED_AF   0xAU                  /* alternate-function push-pull 2 MHz */
#define GPIO_SPEED_OUT  0x2U                  /* general push-pull 2 MHz */

/* ---------------------------------------------------------------------------
 *  1) Power-on safe state: configure all three pins as push-pull and drive
 *     them low before the timer is enabled, avoiding power-on glitches.
 * ------------------------------------------------------------------------- */
static void outputs_safe_low(void)
{
    RCC->APB2ENR |= (1U << 2);                          /* GPIOA clock (APB2) */

    GPIOA->CRL &= ~(0xFU << 4);   GPIOA->CRL |= (GPIO_SPEED_OUT << 4);   /* PA1 */
    GPIOA->CRL &= ~(0xFU << 8);   GPIOA->CRL |= (GPIO_SPEED_OUT << 8);   /* PA2 */
    GPIOA->CRL &= ~(0xFU << 12);  GPIOA->CRL |= (GPIO_SPEED_OUT << 12);  /* PA3 */

    GPIOA->BRR = (1U << 1) | (1U << 2) | (1U << 3);     /* drive all low */
}

/* ---------------------------------------------------------------------------
 *  2) TIM2: CH2=CH1(PWM1)  CH3=CH2(PWM1)  CH4=CH3(PWM2)
 * ------------------------------------------------------------------------- */
static void tim2_init(void)
{
    RCC->APB1ENR |= (1U << 0);                          /* TIM2 clock (APB1) */

    TIM2->PSC = 0;                                      /* 72 MHz, no prescaler */
    TIM2->ARR = (uint16_t)ARR_VAL;                      /* 431 -> 6 us period */

    /* CH2: PWM1 (active while CNT < CCR) -> high [0, 288) = 0..4 us */
    TIM2->CCMR1 &= ~(0x7U << 12);  TIM2->CCMR1 |= (0x6U << 12);   /* OC2M=110 */
    TIM2->CCR2  = (uint16_t)CCR_CH1;

    /* CH3: PWM1 -> high [0, 144) = 0..2 us */
    TIM2->CCMR2 &= ~(0x7U << 4);   TIM2->CCMR2 |= (0x6U << 4);    /* OC3M=110 */
    TIM2->CCR3  = (uint16_t)CCR_CH2;

    /* CH4: PWM2 (active while CNT >= CCR) -> high [288, 432) = 4..6 us */
    TIM2->CCMR2 &= ~(0x7U << 12);  TIM2->CCMR2 |= (0x7U << 12);   /* OC4M=111 */
    TIM2->CCR4  = (uint16_t)CCR_CH3;

    /* Enable outputs: CC2E(bit4) / CC3E(bit8) / CC4E(bit12) */
    TIM2->CCER |= (1U << 4) | (1U << 8) | (1U << 12);

    /* Generate an update event: load PSC/ARR/CCR shadow registers immediately
       and clear CNT, so the first period starts cleanly at t=0. */
    TIM2->EGR = (1U << 0);
}

/* ---------------------------------------------------------------------------
 *  3) Switch the three pins to alternate-function push-pull, hand to timer
 * ------------------------------------------------------------------------- */
static void outputs_to_af(void)
{
    GPIOA->CRL &= ~(0xFU << 4);   GPIOA->CRL |= (GPIO_SPEED_AF << 4);   /* PA1 */
    GPIOA->CRL &= ~(0xFU << 8);   GPIOA->CRL |= (GPIO_SPEED_AF << 8);   /* PA2 */
    GPIOA->CRL &= ~(0xFU << 12);  GPIOA->CRL |= (GPIO_SPEED_AF << 12);  /* PA3 */
}

int main(void)
{
    outputs_safe_low();
    tim2_init();
    GPIOA->BSRR = (1U << 2);                            /* pre-set CH2 high */
    outputs_to_af();
    TIM2->CR1 |= (1U << 0);                             /* CEN: start */

    while (1)
    {
        /* Waveform is generated entirely by hardware; CPU stays idle.
         *
         * Scope check (trigger on PA1 rising edge, 1 us/div):
         *   PA1: high 4 us, low 2 us, 66.7% duty
         *   PA2: high 2 us
         *   PA3: high 2 us, rising edge aligned with PA1 falling edge
         *   period 6 us (166.7 kHz); no visible edge skew
         */
    }
}

/* ===========================================================================
 *  Note A: PA0 and PA6 are intentionally unused.
 *    An earlier four-channel version used PA0 and PA6 with a second timer.
 *    v3 uses only TIM2 CH2/CH3/CH4, so CH1 stays disabled (CC1E=0) and the
 *    second timer's clock is never enabled. Both pins remain available as
 *    GPIO for debugging or expansion.
 *
 *  Note B: why IDLE_TICKS must be 0 (read this before changing the period).
 *    CH3 uses PWM2: the output stays high whenever CNT >= CCR4, until the
 *    counter wraps to 0. Its falling edge is the period end (ARR), NOT CCR.
 *      - IDLE_TICKS = 0: period = 432, CH3 high [288,432) = 2 us  (correct)
 *      - IDLE_TICKS > 0: period stretches to (3+IDLE)*144, and CH3 stays
 *        high until the new period end, i.e. through the whole idle gap.
 *    To add an idle gap, do NOT stretch the PWM2 period. Options:
 *      1) generate CH3 on a second timer with a phase preset;
 *      2) use a TIM2 update interrupt to clear CC4E at CNT=432 and set it
 *         again at the next period start (adds ~20-30 ns of software jitter);
 *      3) use one-pulse (OPM) mode on CH4 with a software trigger.
 *    This project runs continuously, so IDLE_TICKS stays 0.
 * =========================================================================== */
