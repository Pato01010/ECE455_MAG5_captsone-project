/* board.c - clocks, pins, console, watchdog.
 */
#include "board.h"

#include "config.h"
#include "gps.h"

#if (HSE_VALUE % 4000000u) != 0 || HSE_VALUE < 4000000u || HSE_VALUE > 48000000u
#error "HSE_VALUE must be a multiple of 4 MHz between 4 and 48 MHz (PLL input 4 MHz)"
#endif

static clock_source_t g_clock = CLOCK_HSI;

/* ---- Clock: SYSCLK 170 MHz from the HSE (crystal or TCXO) through the PLL; HSI16 if the HSE is
 *      missing. VCO input 4 MHz, x85 = 340 MHz, /2 = 170 MHz. ---------------------------------------- */
clock_source_t board_clock_init(void)
{
    uint32_t start;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;     /* cycle counter for board_delay_us() */
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* Run from HSI16 while the PLL is (re)configured: this is also the path after a clock failure. */
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY)) {}
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_HSI;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI) {}
    RCC->CR &= ~RCC_CR_PLLON;
    while (RCC->CR & RCC_CR_PLLRDY) {}

    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    (void)RCC->APB1ENR1;
    PWR->CR5 &= ~PWR_CR5_R1MODE;                        /* range 1 boost mode, needed above 150 MHz */
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_4WS
                 | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;
    while ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_4WS) {}

#if MAG_HSE_BYPASS
    RCC->CR |= RCC_CR_HSEBYP;                           /* TCXO on OSC_IN */
#endif
    RCC->CR |= RCC_CR_HSEON;
    start = DWT->CYCCNT;
    while (!(RCC->CR & RCC_CR_HSERDY) && (DWT->CYCCNT - start) < 1600000u) {}   /* 100 ms at 16 MHz */
    bool hse = (RCC->CR & RCC_CR_HSERDY) != 0u;
    if (!hse) RCC->CR &= ~(RCC_CR_HSEON | RCC_CR_HSEBYP);

    uint32_t src = hse ? RCC_PLLCFGR_PLLSRC_HSE : RCC_PLLCFGR_PLLSRC_HSI;
    uint32_t m = hse ? HSE_VALUE / 4000000u : 4u;
    RCC->PLLCFGR = src | ((m - 1u) << RCC_PLLCFGR_PLLM_Pos) | (85u << RCC_PLLCFGR_PLLN_Pos)
                   | RCC_PLLCFGR_PLLREN;                /* PLLR = 00: /2 */
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {}

    /* Above 80 MHz, switch through AHB /2 and hold it for 1 us (RM0440, as ST's own template does). */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_HPRE) | RCC_CFGR_HPRE_DIV2;
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {}
    start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < 200u) {}
    RCC->CFGR &= ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2);
    SystemCoreClock = SYSCLK_HZ;

    if (hse) RCC->CR |= RCC_CR_CSSON;                   /* an HSE failure raises the NMI */
    g_clock = hse ? CLOCK_HSE : CLOCK_HSI;
    return g_clock;
}

clock_source_t board_clock_source(void)
{
    return g_clock;
}

static volatile bool g_clock_lost;

void board_clock_lost(void)
{
    g_clock = CLOCK_HSI;
    g_clock_lost = true;
}

bool board_clock_lost_pending(void)
{
    bool lost = g_clock_lost;
    g_clock_lost = false;
    return lost;
}

bool board_clock_lost_peek(void)
{
    return g_clock_lost;
}

const char *board_watchdog_mode(void)
{
    return MAG_DEBUG_BUILD ? "frozen_when_halted" : "runs_when_halted";
}

/* DBGMCU_IDCODE REV_ID: which ES0430 column applies */
const char *board_silicon(void)
{
    switch (DBGMCU->IDCODE >> 16) {
    case 0x2001u: return "Z";
    case 0x2002u: return "Y";
    case 0x2003u: return "X";
    default: return "not_in_ES0430_rev9";
    }
}

void board_delay_us(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t cycles = us * (SystemCoreClock / 1000000u);
    while ((DWT->CYCCNT - start) < cycles) {}
}

/* ---- Pins ------------------------------------------------------------------------------------- */
static void pin_pull(GPIO_TypeDef *p, uint32_t n, uint32_t pull)
{
    p->PUPDR = (p->PUPDR & ~(3u << (2u * n))) | (pull << (2u * n));
}

static void pin_af(GPIO_TypeDef *p, uint32_t n, uint32_t af)
{
    volatile uint32_t *r = &p->AFR[n >> 3];
    *r = (*r & ~(15u << (4u * (n & 7u)))) | (af << (4u * (n & 7u)));
    pin_mode(p, n, 2u);
}

/* A control output: written low before it becomes an output, so it never glitches high. */
static void control_output(GPIO_TypeDef *p, uint32_t n)
{
    pin_low(p, n);
    p->OTYPER &= ~(1u << n);
    p->OSPEEDR &= ~(3u << (2u * n));
    pin_pull(p, n, 0u);
    pin_mode(p, n, 1u);
}

void board_gpio_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN | RCC_AHB2ENR_GPIOCEN | RCC_AHB2ENR_GPIODEN;
    (void)RCC->AHB2ENR;

    control_output(GATE_PORT, GATE_PIN);
    control_output(RLY_PORT, RLY_PIN);
    control_output(TUNE_PORT, TUNE_PIN);
    control_output(BLANK2_PORT, BLANK2_PIN);
    control_output(LED_PORT, LED_PIN);

    pin_mode(BTN_PORT, BTN_PIN, 0u);                    /* B1: input, HIGH when pressed (ST's BSP) */
    pin_pull(BTN_PORT, BTN_PIN, 2u);
    pin_mode(REC_PORT, REC_PIN, 3u);                    /* analog */
    pin_mode(ISNS_PORT, ISNS_PIN, 3u);
    pin_mode(XFER_PORT, XFER_PIN, 3u);
    pin_pull(CMP_PORT, CMP_PIN, 2u);                    /* pull-down: PB10 floats on a bare Nucleo */
    pin_af(CMP_PORT, CMP_PIN, CMP_AF);
    pin_pull(PPS_PORT, PPS_PIN, 2u);                    /* pull-down: no GPS, no pulses */
    pin_af(PPS_PORT, PPS_PIN, PPS_AF);                  /* TIM2_CH2; hw_init() sets up the capture */

#if MAG_N5_SHDN_PB7
    control_output(GPIOB, 7u);                          /* rev C power board: -5 V_A on */
    pin_high(GPIOB, 7u);
#endif
}

void board_led(bool on)
{
    if (on) pin_high(LED_PORT, LED_PIN);
    else pin_low(LED_PORT, LED_PIN);
}

bool board_button(void)
{
    return pin_read(BTN_PORT, BTN_PIN);
}

/* ---- Watchdog: about 0.5 s ---------------------------------------------------------------------
 * A release build lets it run while a debugger halts the core: a breakpoint during polarization would
 * otherwise hold the gate on for as long as the core stays halted (the gate guard is an interrupt, and
 * cannot run either). A halted release build therefore resets 0.5 s later, gate off. A debug build
 * freezes it, so stepping works: never halt or step a debug build with the 12 V supply on. */
void board_watchdog_start(void)
{
#if MAG_DEBUG_BUILD
    DBGMCU->APB1FZR1 |= DBGMCU_APB1FZR1_DBG_IWDG_STOP;
#else
    DBGMCU->APB1FZR1 &= ~DBGMCU_APB1FZR1_DBG_IWDG_STOP;    /* the bit survives a reset: clear it */
#endif
    IWDG->KR = 0xCCCCu;
    IWDG->KR = 0x5555u;
    IWDG->PR = 3u;              /* /32: about 1 kHz from the 32 kHz LSI */
    IWDG->RLR = 500u;
    while (IWDG->SR != 0u) {}
    IWDG->KR = 0xAAAAu;
}

void board_watchdog_kick(void)
{
    IWDG->KR = 0xAAAAu;
}

/* ---- Console: LPUART1 on the ST-LINK virtual COM port ------------------------------------------ */
#define RX_SIZE 256u
static volatile uint8_t rx_buf[RX_SIZE];
static volatile uint32_t rx_head, rx_tail;

void board_console_init(void)
{
    RCC->APB1ENR2 |= RCC_APB1ENR2_LPUART1EN;
    RCC->CCIPR &= ~RCC_CCIPR_LPUART1SEL;                /* PCLK1 = 170 MHz */
    (void)RCC->APB1ENR2;
    pin_af(VCP_TX_PORT, VCP_TX_PIN, VCP_AF);
    pin_af(VCP_RX_PORT, VCP_RX_PIN, VCP_AF);
    pin_pull(VCP_RX_PORT, VCP_RX_PIN, 1u);

    LPUART1->CR1 = 0;
    LPUART1->BRR = (uint32_t)(((uint64_t)SYSCLK_HZ * 256u + VCP_BAUD / 2u) / VCP_BAUD);
    LPUART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE_RXFNEIE;
    LPUART1->CR1 |= USART_CR1_UE;

    NVIC_SetPriority(LPUART1_IRQn, PRIO_CONSOLE);
    NVIC_EnableIRQ(LPUART1_IRQn);
}

void LPUART1_IRQHandler(void)
{
    uint32_t isr = LPUART1->ISR;
    if (isr & USART_ISR_ORE) LPUART1->ICR = USART_ICR_ORECF;
    while (LPUART1->ISR & USART_ISR_RXNE_RXFNE) {
        uint8_t c = (uint8_t)LPUART1->RDR;
        uint32_t next = (rx_head + 1u) % RX_SIZE;
        if (next != rx_tail) {
            rx_buf[rx_head] = c;
            rx_head = next;
        }
    }
    __DSB();                                            /* ES0430 2.1.3: before every interrupt return */
}

/* ---- GPS: USART1 receive on PC5 (D0), PCLK2 = 170 MHz -------------------------------------------- */
void board_gps_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    RCC->CCIPR &= ~RCC_CCIPR_USART1SEL;                 /* PCLK2 */
    (void)RCC->APB2ENR;
    pin_pull(GPS_RX_PORT, GPS_RX_PIN, 1u);              /* idle high when nothing is connected */
    pin_af(GPS_RX_PORT, GPS_RX_PIN, GPS_UART_AF);
#if MAG_GPS_TX
    pin_af(GPS_TX_PORT, GPS_TX_PIN, GPS_UART_AF);
#endif

    USART1->CR1 = 0;
    USART1->BRR = (SYSCLK_HZ + GPS_BAUD / 2u) / GPS_BAUD;   /* oversampling 16: 4427 at 38,400 baud */
    USART1->CR1 = USART_CR1_RE | USART_CR1_RXNEIE_RXFNEIE | (MAG_GPS_TX ? USART_CR1_TE : 0u);
    USART1->CR1 |= USART_CR1_UE;

    NVIC_SetPriority(USART1_IRQn, PRIO_GPS);
    NVIC_EnableIRQ(USART1_IRQn);
}

/* Each character goes to the sentence assembler with the TIM2 count it arrived at. A character lost to an
 * overrun or received with an error spoils its sentence's checksum, so the errors are only cleared. */
void USART1_IRQHandler(void)
{
    if (USART1->ISR & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE))
        USART1->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF;
    while (USART1->ISR & USART_ISR_RXNE_RXFNE) gps_rx_isr((char)USART1->RDR, TIM2->CNT);
    __DSB();                                            /* ES0430 2.1.3 */
}

int board_read_char(void)
{
    if (rx_tail == rx_head) return -1;
    int c = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1u) % RX_SIZE;
    return c;
}

void board_write(const char *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        while (!(LPUART1->ISR & USART_ISR_TXE_TXFNF)) {}
        LPUART1->TDR = (uint8_t)s[i];
        if ((i & 255u) == 255u) board_watchdog_kick();  /* a 60 KB dump takes 5 s at 115,200 baud */
    }
}
