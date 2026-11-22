/* ============================================================================
 *  Timing controller - three synchronized output channels (PA1 / PA2 / PA3)
 *  + UART host interface (USART1, custom frame protocol, CRC16)
 * ----------------------------------------------------------------------------
 *  MCU : STM32F103C8T6 @ 72 MHz
 *  Peripherals: TIM2 (waveform generation) + USART1 (host link)
 *
 *  This file merges the Level-1 waveform core (original main.c) with the
 *  Level-2 UART layer (protocol.h + uart_protocol_ref.c) into one buildable
 *  firmware. Three integration fixes were applied:
 *    1) g_cfg is defined once here; the extern declaration in the reference
 *       file is removed (a file-scope static symbol cannot be extern-linked).
 *    2) tim2_start() / tim2_stop() are implemented (CEN bit of TIM2->CR1).
 *    3) main() now services frame_ready and snapshots the frame with
 *       interrupts briefly disabled, avoiding the rx_frame overwrite race.
 *
 *  Frame format (authoritative: docs/PROTOCOL.md):
 *    [AA][55][LEN][CMD][PAYLOAD...][CRClo][CRChi]
 *    LEN = CMD + PAYLOAD; total = LEN + 5; CRC16 covers [LEN..PAYLOAD end]
 *    Multi-byte values are big-endian; CRC low byte is sent first.
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
 *  Pins:
 *    PA1 = CH1 (TIM2_CH2), PA2 = CH2 (TIM2_CH3), PA3 = CH3 (TIM2_CH4)
 *    PA9 = USART1_TX, PA10 = USART1_RX
 *
 *  Why a single timer is enough:
 *    In earlier versions the active window of the third channel sat in the
 *    MIDDLE of the period. Standard PWM can only anchor a window at the start
 *    (PWM1) or the end (PWM2) of a period. By moving this window to the END of
 *    the period, PWM2 generates it directly and one TIM2 does everything.
 *
 *  Clock arithmetic (f_cnt = 72 MHz, PSC = 0):
 *    1 tick  = 2 us  = 144 counts
 *    period  = 3 ticks = 432 counts -> ARR = 431
 *    CH1 high [0, 288)   -> PWM1, CCR2 = 288
 *    CH2 high [0, 144)   -> PWM1, CCR3 = 144
 *    CH3 high [288, 432) -> PWM2, CCR4 = 288
 * ========================================================================== */

#include "stm32f10x.h"
/* Shared protocol constants. Kept as a separate header so the C++ host can
 * include the same file; Keil resolves this path relative to main.c. */
#include "protocol.h"

/* ---------- Waveform / timer parameters ---------- */
#define TIM_CLK_HZ    72000000UL
#define TICK_US       2UL                     /* each tick is 2 us */
#define CNT_PER_TICK  (TIM_CLK_HZ / 1000000UL * TICK_US)   /* = 144 */
#define ACTIVE_TICKS  3UL
#define IDLE_TICKS    0UL                     /* MUST stay 0! See Note B:
                                                  under PWM2, lengthening the
                                                  period drags channel 3 high
                                                  all the way to period end. */

#define PERIOD_CNT    (CNT_PER_TICK * (ACTIVE_TICKS + IDLE_TICKS))  /* 432 */
#define ARR_VAL       (PERIOD_CNT - 1U)                             /* 431 */

#define CCR_CH1       (CNT_PER_TICK * 2U)     /* CH1 high 2 ticks = 288 */
#define CCR_CH2       (CNT_PER_TICK * 1U)     /* CH2 high 1 tick  = 144 */
#define CCR_CH3       (CNT_PER_TICK * 2U)     /* CH3: PWM2, high when CNT>=288 */

#define GPIO_SPEED_AF   0xAU                  /* AF push-pull 2 MHz */
#define GPIO_SPEED_OUT  0x2U                  /* GP push-pull 2 MHz */

/* 72,000,000 / 166,666 = 432 -> ARR=431. Do NOT use 166667 (gives 431). */
#define DEFAULT_FREQ_HZ  166666UL

#if (PERIOD_CNT > 65535U)
  #error "PERIOD_CNT exceeds 16-bit ARR - reduce TICK_US or ACTIVE_TICKS"
#endif
#if (CCR_CH1 >= PERIOD_CNT) || (CCR_CH2 >= PERIOD_CNT) || (CCR_CH3 >= PERIOD_CNT)
  #error "CCR must be less than PERIOD_CNT - check CCR_CH1/CH2/CH3"
#endif

/* ---------- Runtime configuration ---------- */
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

/* ---------- Frame receive state (ISR writes, main loop reads) ---------- */
typedef enum {
    ST_WAIT_M0 = 0,
    ST_WAIT_M1,
    ST_GET_LEN,
    ST_GET_BODY
} parse_state_t;

static parse_state_t    rx_state  = ST_WAIT_M0;
static uint8_t          rx_frame[FRAME_MAX];
static uint8_t          rx_len    = 0;     /* LEN declared by current frame */
static uint8_t          rx_index  = 0;     /* next write position */
static volatile uint8_t frame_ready = 0;   /* ISR <-> main loop handshake */


/* ===========================================================================
 *  Level 1 - waveform generation (TIM2)
 * ========================================================================= */

/* 1) Power-on safe state: drive all three pins low before enabling TIM2. */
static void outputs_safe_low(void)
{
    RCC->APB2ENR |= (1U << 2);                          /* GPIOA clock (APB2) */

    GPIOA->CRL &= ~(0xFU << 4);   GPIOA->CRL |= (GPIO_SPEED_OUT << 4);   /* PA1 */
    GPIOA->CRL &= ~(0xFU << 8);   GPIOA->CRL |= (GPIO_SPEED_OUT << 8);   /* PA2 */
    GPIOA->CRL &= ~(0xFU << 12);  GPIOA->CRL |= (GPIO_SPEED_OUT << 12);  /* PA3 */

    GPIOA->BRR = (1U << 1) | (1U << 2) | (1U << 3);     /* drive all three low */
}

/* 2) TIM2: CH2=CH1(PWM1)  CH3=CH2(PWM1)  CH4=CH3(PWM2) */
static void tim2_init(void)
{
    RCC->APB1ENR |= (1U << 0);                          /* TIM2 clock (APB1) */

    TIM2->PSC = 0;                                      /* 72 MHz, no prescaling */
    TIM2->ARR = (uint16_t)ARR_VAL;                      /* 431 -> 6 us period */

    /* CH2: PWM1 -> high [0, 288) = 0~4 us */
    TIM2->CCMR1 &= ~(0x7U << 12);  TIM2->CCMR1 |= (0x6U << 12);   /* OC2M=110 */
    TIM2->CCR2  = (uint16_t)CCR_CH1;

    /* CH3: PWM1 -> high [0, 144) = 0~2 us */
    TIM2->CCMR2 &= ~(0x7U << 4);   TIM2->CCMR2 |= (0x6U << 4);    /* OC3M=110 */
    TIM2->CCR3  = (uint16_t)CCR_CH2;

    /* CH4: PWM2 -> high [288, 432) = 4~6 us */
    TIM2->CCMR2 &= ~(0x7U << 12);  TIM2->CCMR2 |= (0x7U << 12);   /* OC4M=111 */
    TIM2->CCR4  = (uint16_t)CCR_CH3;

    /* CC2E(bit4) / CC3E(bit8) / CC4E(bit12) */
    TIM2->CCER |= (1U << 4) | (1U << 8) | (1U << 12);

    /* Update event: load shadow registers and clear CNT for a clean t=0. */
    TIM2->EGR = (1U << 0);
}

/* 3) Hand the three pins over to the timer (alternate-function push-pull). */
static void outputs_to_af(void)
{
    GPIOA->CRL &= ~(0xFU << 4);   GPIOA->CRL |= (GPIO_SPEED_AF << 4);   /* PA1 */
    GPIOA->CRL &= ~(0xFU << 8);   GPIOA->CRL |= (GPIO_SPEED_AF << 8);   /* PA2 */
    GPIOA->CRL &= ~(0xFU << 12);  GPIOA->CRL |= (GPIO_SPEED_AF << 12);  /* PA3 */
}

/* Continuous frequency tuning: preserves the 2:1:2 ratio; IDLE_TICKS stays 0. */
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

/* Start / stop the counter without losing the ARR/CCR configuration.
 * These two functions were referenced by the Level-2 reference file but did
 * not exist in the Level-1 core - now provided here (CEN = bit0 of CR1). */
static void tim2_start(void) { TIM2->CR1 |=  (1U << 0); }   /* CEN = 1 */
static void tim2_stop(void)  { TIM2->CR1 &= ~(1U << 0); }   /* CEN = 0 */


/* ===========================================================================
 *  Level 2 - UART host interface (USART1 @ 115200 8N1)
 * ========================================================================= */

/* [1] USART1 init: PA9=TX (AF push-pull), PA10=RX (floating input), RXNE IRQ.
 *     BRR = 0x0271 for 115200 baud at APB2=72 MHz.
 *     CMSIS standard API NVIC_EnableIRQ/NVIC_SetPriority is used instead of
 *     touching NVIC->IP/IPR directly (those names differ between CMSIS 5/6). */
static void usart1_init(void)
{
    RCC->APB2ENR |= (1U << 2) | (1U << 14);      /* IOPAEN | USART1EN */

    /* PA9/PA10 live in CRH (pins 8-15), NOT CRL (pins 0-7).
     * Writing CRL here would clobber PA1/PA2 and leave the UART pins unconfigured. */
    GPIOA->CRH &= ~(0xFU << 4);   GPIOA->CRH |= (0xAU << 4);   /* PA9  TX, AF push-pull 50MHz */
    GPIOA->CRH &= ~(0xFU << 8);   GPIOA->CRH |= (0x4U << 8);   /* PA10 RX, floating input */

    USART1->BRR = 0x0271U;                        /* 115200 @ 72MHz */

    /* UE | TE | RE | RXNEIE */
    USART1->CR1 = (1U << 13) | (1U << 3) | (1U << 2) | (1U << 5);

    NVIC_EnableIRQ(USART1_IRQn);                  /* IRQ37, CMSIS 5/6 compatible */
    NVIC_SetPriority(USART1_IRQn, 1U);
}

/* [1b] Board LED (PC13, active-low): a visible "firmware is running" beacon.
 *      Turns on only after all init is done, so a lit LED = main loop reached. */
static void led_on(void)
{
    RCC->APB2ENR |= (1U << 4);                    /* GPIOC clock (APB2) */
    GPIOC->CRH &= ~(0xFU << 20);                  /* PC13 = CRH bits [23:20] */
    GPIOC->CRH |= (0x3U << 20);                   /* push-pull output 50MHz */
    GPIOC->BRR = (1U << 13);                      /* drive low -> LED on */
}

/* [1c] TIM3 CH1 input capture on PA6 - measures the *actual* output period.
 *      Wire PA1 (OE, 6us period) to PA6 to enable the SELF_TEST measurement.
 *      TIM3 runs at 72 MHz (PSC=0), so 1 count = 13.9 ns. */
static void tim3_capture_init(void)
{
    RCC->APB1ENR |= (1U << 1);                    /* TIM3 clock (APB1) */

    GPIOA->CRL &= ~(0xFU << 24);                  /* PA6 = CRL bits [27:24] */
    GPIOA->CRL |= (0x4U << 24);                   /* floating input */

    TIM3->PSC   = 0;                              /* 72 MHz */
    TIM3->ARR   = 0xFFFFU;
    TIM3->CCMR1 &= ~(0x3U << 0);                  /* CC1S = 01: IC1 <- TI1 */
    TIM3->CCMR1 |=  (0x1U << 0);
    TIM3->CCMR1 &= ~(0xFU << 4);                  /* IC1F = 0: no filter */
    TIM3->CCER  &= ~(0x1U << 1);                  /* CC1P = 0: rising edge */
    TIM3->CCER  |=  (0x1U << 0);                  /* CC1E = 1: enable */
    TIM3->CR1   |=  (0x1U << 0);                  /* CEN = 1 */
}

/* Average period in timer counts over N rising edges, where N is chosen so
 * that N periods fit inside one 16-bit counter pass (N * period < 65535).
 * At 166666 Hz that gives N = 16; at low frequencies N shrinks, down to 1.
 * Returns 0 on timeout - i.e. no signal on PA6 (the PA1->PA6 jumper is missing). */
static uint16_t measure_period_counts(void)
{
    uint32_t per = (g_cfg.freq_hz != 0U) ? (TIM_CLK_HZ / g_cfg.freq_hz) : 432U;
    uint32_t timeout;
    uint16_t t0, t1;
    uint8_t  n, i;

    /* Keep the measurement window inside one counter pass. 64000 leaves
     * margin for the +/-1 count the real period may differ by. */
    n = (uint8_t)(64000UL / (per ? per : 1U));
    if (n > 16U) n = 16U;
    if (n < 1U)  n = 1U;

    TIM3->SR = 0;                                 /* clear CC1IF */
    timeout = 200000U;
    while (!(TIM3->SR & (1U << 1))) {             /* 1st rising edge */
        if (--timeout == 0U) return 0U;
    }
    TIM3->SR &= ~(1U << 1);
    t0 = (uint16_t)TIM3->CCR1;

    for (i = 0; i < n; i++) {
        timeout = 200000U;
        while (!(TIM3->SR & (1U << 1))) {         /* next rising edge */
            if (--timeout == 0U) return 0U;
        }
        TIM3->SR &= ~(1U << 1);
    }
    t1 = (uint16_t)TIM3->CCR1;

    /* uint16 subtraction wraps correctly for a single counter pass,
     * which the adaptive N guarantees. */
    return (uint16_t)((uint16_t)(t1 - t0) / n);
}

/* [2] Byte-level transmit: wait for TXE (SR bit7) before writing DR. */static void uart_send_byte(uint8_t b)
{
    while (!(USART1->SR & (1U << 7))) { }
    USART1->DR = b;
}

/* Polling receive helper - kept for reference; the firmware receives via
 * the RXNE interrupt, so this function is not called in the current build. */
__attribute__((unused))
static uint8_t uart_recv_byte(void)
{
    while (!(USART1->SR & (1U << 5))) { }
    return (uint8_t)USART1->DR;                  /* reading DR clears RXNE */
}

/* [3] CRC16 (Modbus): init 0xFFFF, reflected polynomial 0xA001.
 *     Covers [LEN .. PAYLOAD end]; low byte is transmitted first. */
static uint16_t crc16_modbus(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFU;
    uint16_t i;
    uint8_t  j;

    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (j = 0; j < 8; j++)
        {
            if (crc & 0x0001U)
                crc = (crc >> 1) ^ 0xA001U;
            else
                crc >>= 1;
        }
    }
    return crc;
}

/* [4] Frame state machine: cut the boundary-less byte stream into frames.
 *     WAIT_M0 --AA--> WAIT_M1 --55--> GET_LEN --valid--> GET_BODY
 *     Called from the ISR with exactly one byte; must stay very short. */
static void protocol_feed(uint8_t b)
{
    switch (rx_state)
    {
        case ST_WAIT_M0:
            if (b == MAGIC0) {
                rx_frame[0] = b;
                rx_state = ST_WAIT_M1;
            }
            break;

        case ST_WAIT_M1:
            if (b == MAGIC1) {
                rx_frame[1] = b;
                rx_state = ST_GET_LEN;
            } else {
                /* Not 0x55: if it is 0xAA, treat it as a new frame header. */
                rx_state = (b == MAGIC0) ? ST_WAIT_M1 : ST_WAIT_M0;
                if (rx_state == ST_WAIT_M1) rx_frame[0] = MAGIC0;
            }
            break;

        case ST_GET_LEN:
            if (b >= 1U && b <= LEN_MAX) {
                rx_frame[2] = b;
                rx_len  = b;
                rx_index = 3;                 /* next write position */
                rx_state = ST_GET_BODY;
            } else {
                rx_state = ST_WAIT_M0;        /* illegal length, resync */
            }
            break;

        case ST_GET_BODY:
            rx_frame[rx_index++] = b;
            /* Total = LEN + 5; rx_index starts at 3, so full == LEN + 5. */
            if (rx_index >= (uint8_t)(rx_len + FRAME_OVERHEAD))
            {
                frame_ready = 1;
                rx_state = ST_WAIT_M0;        /* reset immediately */
                rx_index = 0;
            }
            break;

        default:
            rx_state = ST_WAIT_M0;
            break;
    }
}

/* [5] Build and send a response frame. PAYLOAD[0] is always RESULT. */
static void send_response(uint8_t cmd, const uint8_t *payload, uint8_t plen)
{
    uint8_t  frame[FRAME_MAX];
    uint8_t  len = (uint8_t)(1U + plen);       /* LEN = CMD + PAYLOAD */
    uint16_t crc;
    uint8_t  i;

    frame[0] = MAGIC0;
    frame[1] = MAGIC1;
    frame[2] = len;
    frame[3] = cmd | CMD_RESP_BIT;             /* response: set bit7 */
    for (i = 0; i < plen; i++)
        frame[4 + i] = payload[i];

    crc = crc16_modbus(&frame[2], (uint16_t)(len + 1U));
    frame[4 + plen]     = (uint8_t)(crc & 0xFFU);   /* low byte first */
    frame[4 + plen + 1] = (uint8_t)(crc >> 8);

    for (i = 0; i < (uint8_t)(len + FRAME_OVERHEAD); i++)
        uart_send_byte(frame[i]);
}

/* Process one complete frame. Takes the frame as a parameter (snapshot from
 * the main loop) instead of reading rx_frame directly, so a newly arriving
 * frame cannot overwrite the one being processed. */
static void process_frame(const uint8_t *f)
{
    uint8_t   len = f[2];
    uint8_t   cmd = f[3];
    const uint8_t *payload = &f[4];
    uint8_t   plen = (uint8_t)(len - 1U);      /* LEN includes CMD */
    uint16_t  crc_recv, crc_calc;
    uint8_t   resp[8];
    uint8_t   rlen = 0;
    uint32_t  freq;

    /* 1. CRC check - never execute a corrupted frame. */
    crc_recv = (uint16_t)f[3 + len] | ((uint16_t)f[4 + len] << 8);
    crc_calc = crc16_modbus(&f[2], (uint16_t)(len + 1U));
    if (crc_recv != crc_calc) {
        resp[0] = RES_ERR_CRC;
        send_response(cmd, resp, 1);
        return;
    }

    /* 2. Command dispatch. */
    switch (cmd)
    {
        case CMD_SET_PARAM:
            if (plen < 4U) break;              /* param_id(1) + freq(3) */
            if (payload[0] == PARAM_FREQ) {
                freq = ((uint32_t)payload[1] << 16) |
                       ((uint32_t)payload[2] << 8) |
                       (uint32_t)payload[3];
                timing_set_frequency(freq);
                resp[0] = RES_OK;
            } else {
                resp[0] = RES_ERR_RANGE;
            }
            send_response(cmd, resp, 1);
            break;

        case CMD_GET_PARAM:
            if (plen < 1U) break;
            if (payload[0] == PARAM_FREQ) {
                uint32_t v = g_cfg.freq_hz;
                resp[0] = RES_OK;              /* RESULT first */
                resp[1] = (uint8_t)(v >> 16);  /* 3-byte big-endian */
                resp[2] = (uint8_t)(v >> 8);
                resp[3] = (uint8_t)(v & 0xFFU);
                send_response(cmd, resp, 4);
            } else {
                resp[0] = RES_ERR_RANGE;
                send_response(cmd, resp, 1);
            }
            break;

        case CMD_START:
            tim2_start();
            g_cfg.running = 1U;
            resp[0] = RES_OK;
            send_response(cmd, resp, 1);
            break;

        case CMD_STOP:
            tim2_stop();
            g_cfg.running = 0U;
            resp[0] = RES_OK;
            send_response(cmd, resp, 1);
            break;

        case CMD_GET_STATUS:
            /* PAYLOAD = RESULT + run + freq(3) + err(1) */
            resp[0] = RES_OK;
            resp[1] = g_cfg.running;
            resp[2] = (uint8_t)(g_cfg.freq_hz >> 16);
            resp[3] = (uint8_t)(g_cfg.freq_hz >> 8);
            resp[4] = (uint8_t)(g_cfg.freq_hz);
            resp[5] = 0U;                      /* err: reserved for Level 3 */
            rlen = 6;
            send_response(cmd, resp, rlen);
            break;

        case CMD_SELF_TEST:
            /* Measure the real output period on PA6 and report the deviation
             * in permille. Returns err=0 when the PA1->PA6 jumper is absent
             * (capture times out), so a missing jumper is distinguishable from
             * a genuinely perfect output only by checking the wiring. */
            {
                uint16_t period_cnt = measure_period_counts();
                uint16_t permille   = 0U;

                if (period_cnt != 0U && g_cfg.freq_hz != 0U) {
                    uint32_t actual_hz = TIM_CLK_HZ / period_cnt;
                    uint32_t target_hz = g_cfg.freq_hz;
                    uint32_t diff = (actual_hz > target_hz)
                                  ? (actual_hz - target_hz)
                                  : (target_hz - actual_hz);
                    permille = (uint16_t)((diff * 1000UL) / target_hz);
                }
                resp[0] = RES_OK;
                resp[1] = (uint8_t)(permille >> 8);        /* err_permille, big-endian */
                resp[2] = (uint8_t)(permille & 0xFFU);
                send_response(cmd, resp, 3);
            }
            break;

        default:
            resp[0] = RES_ERR_UNK;
            send_response(cmd, resp, 1);
            break;
    }
}

/* USART1 ISR: read one byte, feed the state machine, nothing else. */
void USART1_IRQHandler(void)
{
    if (USART1->SR & (1U << 5))                /* RXNE */
    {
        uint8_t b = (uint8_t)USART1->DR;       /* reading DR clears the flag */
        protocol_feed(b);
    }
}


/* ===========================================================================
 *  Main: clock self-check -> init -> run output -> serve host commands
 * ========================================================================= */
int main(void)
{
    uint8_t i;
    uint8_t frame_snap[FRAME_MAX];

    /* oscillator / clock configuration */
    if (!(RCC->CR & (1U << 17)))                  while (1);  /* HSERDY */
    if (!(RCC->CR & (1U << 25)))                  while (1);  /* PLLRDY */
    if (((RCC->CFGR >> 2) & 0x3U) != 0x2U)        while (1);  /* SWS = PLL */

    outputs_safe_low();
    tim2_init();
    timing_set_frequency(DEFAULT_FREQ_HZ);
    GPIOA->BSRR = (1U << 2);                      /* pre-set CH2 high */
    outputs_to_af();
    tim2_start();                                 /* CEN = 1 */
    tim3_capture_init();                          /* input capture on PA6 */
    usart1_init();                                /* UART host link */
    led_on();                                     /* init done -> LED on */

    while (1)
    {
        if (frame_ready)
        {
            /* Snapshot the frame with interrupts briefly disabled: the ISR
             * may otherwise start writing the next frame into rx_frame while
             * process_frame() is still reading the current one. */
            __disable_irq();
            for (i = 0; i < FRAME_MAX; i++)
                frame_snap[i] = rx_frame[i];
            frame_ready = 0;
            __enable_irq();

            process_frame(frame_snap);
        }
    }
}

/* ===========================================================================
 *  Note A: PA0 and PA6 are intentionally unused (leftover from an earlier
 *          four-channel version). They remain available as GPIO.
 *  Note B: IDLE_TICKS must stay 0. CH3 uses PWM2, so its falling edge is the
 *          period end (ARR), not CCR. Lengthening the period keeps CH3 high
 *          through the whole off interval. To add an off interval, use a
 *          second timer with preset phase, or gate CC4E in the update ISR,
 *          or use one-pulse mode - never stretch the PWM2 period.
 * =========================================================================== */
