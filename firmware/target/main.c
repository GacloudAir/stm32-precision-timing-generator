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
 *  Waveform definition:
 *    1) All three channels are driven by the same timer, so no software
 *       phase correction is needed.
 *    2) At the start of a period (t=0): CH1 goes high and stays high for
 *       2 ticks (4 us), then goes low.
 *    3) CH2 goes high at the start of the period and stays high for
 *       1 tick (2 us), then goes low.
 *    4) CH3 goes high 2 ticks (4 us) into the period and stays high for
 *       1 tick.
 *    5) One period = 3 ticks, each tick = 2 us, for a total of 6 us.
 *
 *  Pins (v3 uses PA1 / PA2 / PA3 only):
 *    PA1 = CH1 (TIM2_CH2)
 *    PA2 = CH2 (TIM2_CH3)
 *    PA3 = CH3 (TIM2_CH4)
 *
 *  Why a single timer is enough:
 *    In earlier versions the active window of the third channel sat in the
 *    MIDDLE of the period. Standard PWM can only anchor a window at the start
 *    (PWM1) or the end (PWM2) of a period, so a middle window forced us to use
 *    a second timer with a preset phase. By moving this window to the END of
 *    the period, PWM2 can generate it directly and one TIM2 does everything.
 *
 *  Clock arithmetic (f_cnt = 72 MHz, PSC = 0):
 *    1 tick  = 2 us  = 144 counts
 *    period  = 3 ticks = 432 counts -> ARR = 431
 *    CH1 high [0, 288)   -> PWM1, CCR2 = 288
 *    CH2 high [0, 144)   -> PWM1, CCR3 = 144
 *    CH3 high [288, 432) -> PWM2, CCR4 = 288
 *    The falling edge of CH1 and the rising edge of CH3 occur at the same
 *    count value (288), so they are aligned by hardware, not by software delay.
 *
 *  GPIO speed: the 2 MHz drive setting is used (rather than 50 MHz) for clean
 *  edges on long wires.
 * ========================================================================== */

#include "stm32f10x.h"

/* ---------- Tunable parameters ---------- */
#define TIM_CLK_HZ    72000000UL
#define TICK_US       2UL                     /* each tick is 2 us */
#define CNT_PER_TICK  (TIM_CLK_HZ / 1000000UL * TICK_US)   /* = 144 */
#define ACTIVE_TICKS  3UL
#define IDLE_TICKS    0UL                     /* MUST stay 0! See note B below:
                                                  under PWM2, lengthening the
                                                  period drags channel 3 high
                                                  all the way to period end. */

#define PERIOD_CNT    (CNT_PER_TICK * (ACTIVE_TICKS + IDLE_TICKS))  /* 432 */
#define ARR_VAL       (PERIOD_CNT - 1U)                             /* 431 */

#define CCR_CH1       (CNT_PER_TICK * 2U)     /* CH1 high 2 ticks = 288 */
#define CCR_CH2       (CNT_PER_TICK * 1U)     /* CH2 high 1 tick  = 144 */
#define CCR_CH3       (CNT_PER_TICK * 2U)     /* CH3: PWM2, high when CNT>=288 */

/* GPIO speed settings: 2 MHz drive setting to suppress ringing */
#define GPIO_SPEED_AF   0xAU                  /* alternate-function push-pull 2 MHz */
#define GPIO_SPEED_OUT  0x2U                  /* general-purpose push-pull 2 MHz */

/* Default output frequency: 72,000,000 / 166,666 = 432 (integer division),
 * which yields exactly ARR=431 and CCR2=288 / CCR3=144 / CCR4=288, matching
 * the intended waveform.
 * Do NOT use 166667 -- it gives 431 and changes all three compare values. */
#define DEFAULT_FREQ_HZ  166666UL

/* Compile-time checks: fail loudly if tuning makes ARR/CCR too large and
 * silently truncates them to 16 bits. */
#if (PERIOD_CNT > 65535U)
  #error "PERIOD_CNT exceeds 16-bit ARR - reduce TICK_US or ACTIVE_TICKS"
#endif
#if (CCR_CH1 >= PERIOD_CNT) || (CCR_CH2 >= PERIOD_CNT) || (CCR_CH3 >= PERIOD_CNT)
  #error "CCR must be less than PERIOD_CNT - check CCR_CH1/CH2/CH3"
#endif

/* Runtime configuration: reserved for future UART parameter setting /
 * status readback. */
typedef struct {
    uint32_t freq_hz;      /* currently configured frequency */
    uint16_t arr;          /* ARR actually written */
    uint16_t ccr[3];       /* compare values for CH1 / CH2 / CH3 */
    uint8_t  running;      /* 0 = stopped, 1 = running */
} timing_config_t;

static volatile timing_config_t g_cfg = {
    .freq_hz = DEFAULT_FREQ_HZ,
    .arr     = (uint16_t)ARR_VAL,
    .ccr     = { (uint16_t)CCR_CH1, (uint16_t)CCR_CH2, (uint16_t)CCR_CH3 },
    .running = 1U,
};


/* ---------------------------------------------------------------------------
 *  1) Power-on safe state: before enabling the timer, configure all three
 *     pins as push-pull and drive them low, to avoid glitches at power-up.
 * ------------------------------------------------------------------------- */
static void outputs_safe_low(void)
{
    RCC->APB2ENR |= (1U << 2);                          /* GPIOA clock (APB2) */

    GPIOA->CRL &= ~(0xFU << 4);   GPIOA->CRL |= (GPIO_SPEED_OUT << 4);   /* PA1 */
    GPIOA->CRL &= ~(0xFU << 8);   GPIOA->CRL |= (GPIO_SPEED_OUT << 8);   /* PA2 */
    GPIOA->CRL &= ~(0xFU << 12);  GPIOA->CRL |= (GPIO_SPEED_OUT << 12);  /* PA3 */

    GPIOA->BRR = (1U << 1) | (1U << 2) | (1U << 3);     /* drive all three low */
}

/* ---------------------------------------------------------------------------
 *  2) TIM2: CH2=CH1(PWM1)  CH3=CH2(PWM1)  CH4=CH3(PWM2)
 * ------------------------------------------------------------------------- */
static void tim2_init(void)
{
    RCC->APB1ENR |= (1U << 0);                          /* TIM2 clock (APB1) */

    TIM2->PSC = 0;                                      /* 72 MHz, no prescaling */
    TIM2->ARR = (uint16_t)ARR_VAL;                      /* 431 -> 6 us period */

    /* CH2: PWM1 (active while CNT < CCR) -> high [0, 288) = 0~4 us */
    TIM2->CCMR1 &= ~(0x7U << 12);  TIM2->CCMR1 |= (0x6U << 12);   /* OC2M=110 */
    TIM2->CCR2  = (uint16_t)CCR_CH1;

    /* CH3: PWM1 -> high [0, 144) = 0~2 us */
    TIM2->CCMR2 &= ~(0x7U << 4);   TIM2->CCMR2 |= (0x6U << 4);    /* OC3M=110 */
    TIM2->CCR3  = (uint16_t)CCR_CH2;

    /* CH4: PWM2 (active while CNT >= CCR) -> high [288, 432) = 4~6 us */
    TIM2->CCMR2 &= ~(0x7U << 12);  TIM2->CCMR2 |= (0x7U << 12);   /* OC4M=111 */
    TIM2->CCR4  = (uint16_t)CCR_CH3;

    /* Enable the three outputs: CC2E(bit4) / CC3E(bit8) / CC4E(bit12) */
    TIM2->CCER |= (1U << 4) | (1U << 8) | (1U << 12);

    /* Generate an update event: load the shadow registers for PSC/ARR/CCR
       immediately and clear CNT, so the first period starts from a clean t=0. */
    TIM2->EGR = (1U << 0);
}

/* ---------------------------------------------------------------------------
 *  3) Switch the three pins to alternate-function push-pull, handing them
 *     over to the timer.
 * ------------------------------------------------------------------------- */
static void outputs_to_af(void)
{
    GPIOA->CRL &= ~(0xFU << 4);   GPIOA->CRL |= (GPIO_SPEED_AF << 4);   /* PA1 */
    GPIOA->CRL &= ~(0xFU << 8);   GPIOA->CRL |= (GPIO_SPEED_AF << 8);   /* PA2 */
    GPIOA->CRL &= ~(0xFU << 12);  GPIOA->CRL |= (GPIO_SPEED_AF << 12);  /* PA3 */
}

/* Continuous frequency tuning: preserves the 2:1:2 ratio of the three
 * segments; IDLE_TICKS stays 0, so there is no tail stretching. */
static void timing_set_frequency(uint32_t freq_hz)
{
    uint32_t period = TIM_CLK_HZ / freq_hz;      /* 72M / 166666 ~= 432 */

    if (period < 3U || period > 65535U) return;  /* bounds guard */

    g_cfg.freq_hz = freq_hz;
    g_cfg.arr     = (uint16_t)(period - 1U);
    g_cfg.ccr[0]  = (uint16_t)(period * 2U / 3U);
    g_cfg.ccr[1]  = (uint16_t)(period / 3U);
    g_cfg.ccr[2]  = (uint16_t)(period * 2U / 3U);

    TIM2->ARR  = g_cfg.arr;
    TIM2->CCR2 = g_cfg.ccr[0];
    TIM2->CCR3 = g_cfg.ccr[1];
    TIM2->CCR4 = g_cfg.ccr[2];
}


/* Main program: clock self-check -> init -> start output. If it stalls in
 * one of the while(1) loops below, check PC in the debugger to see which
 * step failed. */
int main(void)
{
    /* Check oscillator and clock configuration */
    if (!(RCC->CR & (1U << 17)))                  while (1);  /* HSERDY: HSE ready */
    if (!(RCC->CR & (1U << 25)))                  while (1);  /* PLLRDY: PLL locked */
    if (((RCC->CFGR >> 2) & 0x3U) != 0x2U)        while (1);  /* SWS must be PLL */

    outputs_safe_low();
    tim2_init();
    timing_set_frequency(DEFAULT_FREQ_HZ);          /* set initial frequency ~166.7 kHz; also avoids "unused function" warning */
    GPIOA->BSRR = (1U << 2);                        /* pre-set CH2 high (only affects the startup instant; PWM takes over) */
    outputs_to_af();
    TIM2->CR1 |= (1U << 0);                         /* CEN: start the timer */

    /* The waveform is generated entirely in hardware; the CPU stays idle.
     *
     * Oscilloscope check (trigger on PA1 rising edge, 1 us/div):
     *   PA1: high 4 us, low 2 us, duty 66.7%
     *   PA2: high 2 us
     *   PA3: high 2 us, rising edge aligned with PA1 falling edge
     *   Period 6 us (166.7 kHz); no visible edge misalignment between channels
     */
    while (1)
    {
        __WFI();    /* enter wait-for-interrupt mode when idle to save power */
    }
}

/* ===========================================================================
 *  Note A: PA0 and PA6 are intentionally unused.
 *    An earlier four-channel version used a second timer to drive PA0 and PA6.
 *    v3 uses only TIM2 CH2/CH3/CH4, so CH1 is left disabled (CC1E=0) and the
 *    second timer's clock is never enabled. These two pins remain available
 *    as GPIO for debugging or expansion.
 *
 *  Note B: why IDLE_TICKS must be 0 (read this before changing the period).
 *    CH3 uses PWM2: the output stays high as long as CNT >= CCR4, until the
 *    counter wraps to 0. Its falling edge is the period end (ARR), not the
 *    compare value CCR.
 *      - IDLE_TICKS = 0: period = 432, CH3 high [288,432) = 2 us (correct)
 *      - IDLE_TICKS > 0: period stretches to (3+IDLE)*144, and CH3 stays high
 *        until the new period end -- i.e. throughout the entire off interval.
 *    To add an off interval, never lengthen the PWM2 period. Alternatives:
 *      1) use a second timer with a preset phase to generate CH3;
 *      2) use the TIM2 update interrupt to clear CC4E at CNT=432 and set it
 *         again at the next period start (adds ~20~30 ns of software jitter);
 *      3) use one-pulse (OPM) mode on CH4, triggered by software.
 *    This project transmits continuously, so IDLE_TICKS stays at 0.
 * =========================================================================== */
