/* ============================================================================
 *  【中文注释参考版 —— 代码逻辑与 main.c 完全一致，仅注释翻译】
 *  说明：本文件仅供阅读对照，Keil 工程当前引用的仍是 main.c，不会编译本文件。
 *        你在 main.c 原位置改好后，可直接删除本文件。
 * ============================================================================
 *  时序控制器 v3 —— 三路同步输出（PA1 / PA2 / PA3）
 *  MCU ：STM32F103C8T6 @ 72 MHz
 *  外设：仅用 TIM2（三路共用一个时基）
 * ============================================================================
 *  目标波形（一个周期 = 3 个 tick = 6 微秒）：
 *
 *      时间(微秒)：0 -------- 2 -------- 4 -------- 6
 *      CH1       ：|----------------------|           （通道 1）
 *      CH2       ：|----------|                        （通道 2）
 *      CH3       ：                    |----------|     （通道 3）
 *
 *  波形定义：
 *    1) 三路都由同一个定时器驱动，因此不需要任何软件相位校正；
 *    2) 周期开始(t=0)：CH1 变高，持续 2 个 tick（4 微秒）后变低；
 *    3) CH2 在周期开始时变高，持续 1 个 tick（2 微秒）后变低；
 *    4) CH3 在周期开始后 2 个 tick（4 微秒）处变高，1 个 tick 后变低；
 *    5) 一个周期 = 3 个 tick，每个 tick = 2 微秒，合计 6 微秒。
 *
 *  引脚（v3 只使用 PA1 / PA2 / PA3）：
 *    PA1 = CH1（TIM2_CH2）
 *    PA2 = CH2（TIM2_CH3）
 *    PA3 = CH3（TIM2_CH4）
 *
 *  为什么一个定时器就够了：
 *    在早期版本里，第三路的有效窗口位于周期的【正中间】。标准 PWM 只能把
 *    窗口锚定在周期的起点（PWM1）或终点（PWM2），中间的窗口迫使我们必须
 *    用第二个定时器并预置相位。把这个窗口挪到周期的【末尾】之后，PWM2
 *    可以直接生成它，一个 TIM2 就干完了所有事。
 *
 *  时钟换算（f_cnt = 72 MHz，PSC = 0）：
 *    1 个 tick = 2 微秒 = 144 个计数
 *    周期     = 3 个 tick = 432 个计数 → ARR = 431
 *    CH1 高 [0, 288)   → PWM1，CCR2 = 288
 *    CH2 高 [0, 144)   → PWM1，CCR3 = 144
 *    CH3 高 [288, 432) → PWM2，CCR4 = 288
 *    CH1 的下降沿与 CH3 的上升沿发生在同一个计数值（288），因此它们由
 *    硬件对齐，而不是靠软件延时。
 *
 *  GPIO 速度：选用 2 MHz 驱动档（而非 50 MHz），使长导线上的边沿干净。
 * ========================================================================== */

#include "stm32f10x.h"

/* ---------- 可调参数 ---------- */
#define TIM_CLK_HZ    72000000UL
#define TICK_US       2UL                     /* 每个 tick 为 2 微秒 */
#define CNT_PER_TICK  (TIM_CLK_HZ / 1000000UL * TICK_US)   /* = 144 */
#define ACTIVE_TICKS  3UL
#define IDLE_TICKS    0UL                     /* 必须保持为 0！见下方备注 B：
                                                  PWM2 模式下拉长周期会把
                                                  第三路的高电平一直拖到周期末。*/

#define PERIOD_CNT    (CNT_PER_TICK * (ACTIVE_TICKS + IDLE_TICKS))  /* 432 */
#define ARR_VAL       (PERIOD_CNT - 1U)                             /* 431 */

#define CCR_CH1       (CNT_PER_TICK * 2U)     /* CH1 高 2 个 tick = 288 */
#define CCR_CH2       (CNT_PER_TICK * 1U)     /* CH2 高 1 个 tick = 144 */
#define CCR_CH3       (CNT_PER_TICK * 2U)     /* CH3：PWM2，CNT>=288 时为高 */

/* GPIO 速度设置：2 MHz 驱动档以抑制振铃 */
#define GPIO_SPEED_AF   0xAU                  /* 复用功能推挽 2 MHz */
#define GPIO_SPEED_OUT  0x2U                  /* 通用推挽 2 MHz */

/* 默认输出频率：72,000,000 / 166,666 = 432（整数除法），正好得到 ARR=431、
 * CCR2=288 / CCR3=144 / CCR4=288，与原定波形一致。
 * 不要用 166667 —— 它会算出 431，三个比较值全变。 */
#define DEFAULT_FREQ_HZ  166666UL

/* 在编译期检查，如果调整参数导致 ARR/CCR 过大并被被静默截断成 16 位，则报错 */
#if (PERIOD_CNT > 65535U)
  #error "PERIOD_CNT exceeds 16-bit ARR - reduce TICK_US or ACTIVE_TICKS"
#endif
#if (CCR_CH1 >= PERIOD_CNT) || (CCR_CH2 >= PERIOD_CNT) || (CCR_CH3 >= PERIOD_CNT)
  #error "CCR must be less than PERIOD_CNT - check CCR_CH1/CH2/CH3"
#endif

/* 运行时配置：供以后 UART 设参 / 状态回读使用 */
typedef struct {
    uint32_t freq_hz;      /* 当前设定频率 */
    uint16_t arr;          /* 实际写入的 ARR */
    uint16_t ccr[3];       /* CH1 / CH2 / CH3 的比较值 */
    uint8_t  running;      /* 0 = 停, 1 = 发 */
} timing_config_t;

static volatile timing_config_t g_cfg = {
    .freq_hz = DEFAULT_FREQ_HZ,
    .arr     = (uint16_t)ARR_VAL,
    .ccr     = { (uint16_t)CCR_CH1, (uint16_t)CCR_CH2, (uint16_t)CCR_CH3 },
    .running = 1U,
};


/* ---------------------------------------------------------------------------
 *  1) 上电安全态：在使能定时器之前，把三个引脚配成推挽并全部拉低，
 *     避免上电瞬间出现毛刺。
 * ------------------------------------------------------------------------- */
static void outputs_safe_low(void)
{
    RCC->APB2ENR |= (1U << 2);                          /* GPIOA 时钟 (APB2) */

    GPIOA->CRL &= ~(0xFU << 4);   GPIOA->CRL |= (GPIO_SPEED_OUT << 4);   /* PA1 */
    GPIOA->CRL &= ~(0xFU << 8);   GPIOA->CRL |= (GPIO_SPEED_OUT << 8);   /* PA2 */
    GPIOA->CRL &= ~(0xFU << 12);  GPIOA->CRL |= (GPIO_SPEED_OUT << 12);  /* PA3 */

    GPIOA->BRR = (1U << 1) | (1U << 2) | (1U << 3);     /* 三路全部拉低 */
}

/* ---------------------------------------------------------------------------
 *  2) TIM2：CH2=CH1(PWM1)  CH3=CH2(PWM1)  CH4=CH3(PWM2)
 * ------------------------------------------------------------------------- */
static void tim2_init(void)
{
    RCC->APB1ENR |= (1U << 0);                          /* TIM2 时钟 (APB1) */

    TIM2->PSC = 0;                                      /* 72 MHz，不分频 */
    TIM2->ARR = (uint16_t)ARR_VAL;                      /* 431 → 周期 6 微秒 */

    /* CH2：PWM1（CNT < CCR 时有效）→ 高电平 [0, 288) = 0~4 微秒 */
    TIM2->CCMR1 &= ~(0x7U << 12);  TIM2->CCMR1 |= (0x6U << 12);   /* OC2M=110 */
    TIM2->CCR2  = (uint16_t)CCR_CH1;

    /* CH3：PWM1 → 高电平 [0, 144) = 0~2 微秒 */
    TIM2->CCMR2 &= ~(0x7U << 4);   TIM2->CCMR2 |= (0x6U << 4);    /* OC3M=110 */
    TIM2->CCR3  = (uint16_t)CCR_CH2;

    /* CH4：PWM2（CNT >= CCR 时有效）→ 高电平 [288, 432) = 4~6 微秒 */
    TIM2->CCMR2 &= ~(0x7U << 12);  TIM2->CCMR2 |= (0x7U << 12);   /* OC4M=111 */
    TIM2->CCR4  = (uint16_t)CCR_CH3;

    /* 使能三路输出：CC2E(bit4) / CC3E(bit8) / CC4E(bit12) */
    TIM2->CCER |= (1U << 4) | (1U << 8) | (1U << 12);

    /* 产生一次更新事件：让 PSC/ARR/CCR 的影子寄存器立即装载，并把 CNT 清零，
       使第一个周期从干净的 t=0 开始。 */
    TIM2->EGR = (1U << 0);
}

/* ---------------------------------------------------------------------------
 *  3) 把三个引脚切换成复用推挽，交给定时器接管
 * ------------------------------------------------------------------------- */
static void outputs_to_af(void)
{
    GPIOA->CRL &= ~(0xFU << 4);   GPIOA->CRL |= (GPIO_SPEED_AF << 4);   /* PA1 */
    GPIOA->CRL &= ~(0xFU << 8);   GPIOA->CRL |= (GPIO_SPEED_AF << 8);   /* PA2 */
    GPIOA->CRL &= ~(0xFU << 12);  GPIOA->CRL |= (GPIO_SPEED_AF << 12);  /* PA3 */
}

/* 连续调频：保持三段波形的比例，IDLE_TICKS 仍为 0，不会拖尾 */
static void timing_set_frequency(uint32_t freq_hz)
{
    uint32_t period = TIM_CLK_HZ / freq_hz;      /* 72M / 166666 ≈ 432 */

    if (period < 3U || period > 65535U) return;  /* 越界保护 */

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


/* 主程序：时钟自检 → 初始化 → 启动输出。若卡在下面某条 while(1)，
 * 用调试器看 PC 停在哪行，即可知道是哪一步失败。 */
int main(void)
{
    /* 检查晶振与时钟配置 */
    if (!(RCC->CR & (1U << 17)))                  while (1);  /* HSERDY：外部高速时钟就绪 */
    if (!(RCC->CR & (1U << 25)))                  while (1);  /* PLLRDY：锁相环锁定 */
    if (((RCC->CFGR >> 2) & 0x3U) != 0x2U)        while (1);  /* SWS 必须是 PLL */

    outputs_safe_low();
    tim2_init();
    timing_set_frequency(DEFAULT_FREQ_HZ);          /* 设初始频率 ≈166.7 kHz，同时消除"未使用函数"警告 */
    GPIOA->BSRR = (1U << 2);                        /* 预先把 CH2 置高(仅影响启动瞬间,后续PWM会自动控制) */
    outputs_to_af();
    TIM2->CR1 |= (1U << 0);                         /* CEN：启动定时器 */

    /* 波形完全由硬件产生，CPU 保持空闲。
     *
     * 示波器检查（触发源接 PA1 上升沿，时基 1 微秒/格）：
     *   PA1：高 4 微秒，低 2 微秒，占空比 66.7%
     *   PA2：高 2 微秒
     *   PA3：高 2 微秒，上升沿与 PA1 下降沿对齐
     *   周期 6 微秒（166.7 kHz）；各路边沿无肉眼可见错位
     */
    while (1)
    {
        __WFI();    /* CPU 空闲时自动进入等待中断模式以节能 */
    }
}

/* ===========================================================================
 *  备注 A：PA0 与 PA6 故意不使用。
 *    早期的四通道版本用第二个定时器驱动 PA0 和 PA6。v3 只使用 TIM2 的
 *    CH2/CH3/CH4，因此 CH1 保持关闭（CC1E=0），第二个定时器的时钟也从不
 *    使能。这两个引脚仍可作为 GPIO 留作调试或扩展。
 *
 *  备注 B：为什么 IDLE_TICKS 必须为 0（改周期前务必先读）。
 *    CH3 使用 PWM2：只要 CNT >= CCR4，输出就保持高电平，直到计数器回绕到 0。
 *    它的下降沿是周期终点（ARR），而不是比较值 CCR。
 *      - IDLE_TICKS = 0：周期 = 432，CH3 高 [288,432) = 2 微秒（正确）
 *      - IDLE_TICKS > 0：周期被拉长到 (3+IDLE)*144，CH3 会一直高到新的
 *        周期末，也就是贯穿整个关断间隔。
 *    要加关断间隔，切勿拉长 PWM2 的周期。可选做法：
 *      1) 用第二个定时器并预置相位来生成 CH3；
 *      2) 用 TIM2 更新中断，在 CNT=432 时清除 CC4E，下一周期开始时再置回
 *         （会引入约 20~30 ns 的软件抖动）；
 *      3) 在 CH4 上使用单脉冲（OPM）模式，由软件触发。
 *    本项目是连续发射，因此 IDLE_TICKS 保持为 0。
 * =========================================================================== */
