/* board.h - the NUCLEO-G474RE on the acquisition carrier: pins, clocks, interrupt priorities.
 *
 * Pins are the acquisition rev C / rev E README "Pin assignments". A pin change is one line here.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "stm32g4xx.h"

/* ---- Control lines (all held LOW by pull-downs on the boards in reset) ------------------------- */
#define TUNE_PORT       GPIOC   /* CN7-1,  bus 15: HIGH = tuning bank across the coil */
#define TUNE_PIN        10u
#define BLANK2_PORT     GPIOC   /* CN7-2,  bus 16: HIGH holds the front end's gain stage */
#define BLANK2_PIN      11u
#define GATE_PORT       GPIOC   /* CN7-3,  bus 17: polarization gate */
#define GATE_PIN        12u
#define RLY_PORT        GPIOD   /* CN7-4,  bus 18: polarization K1, and the front end's K_XFER release */
#define RLY_PIN         2u

/* ---- Inputs ------------------------------------------------------------------------------------ */
#define REC_PORT        GPIOA   /* CN7-28, bus 9 (BP_OUT): ADC1_IN1, the record */
#define REC_PIN         0u
#define REC_ADC         ADC1
#define REC_CHANNEL     1u
#define ISNS_PORT       GPIOA   /* CN7-30, bus 11: I_SENSE, 1.000 V/A */
#define ISNS_PIN        1u
#define ISNS_ADC        ADC2    /* PA1 is ADC12_IN2: ADC2 leaves ADC1 set up for the record */
#define ISNS_CHANNEL    2u
#define XFER_PORT       GPIOB   /* CN7-34, bus 13 through R11/R12: XFER_SENSE */
#define XFER_PIN        0u
#define XFER_ADC        ADC3
#define XFER_CHANNEL    12u
#define CMP_PORT        GPIOB   /* CN10-25: CMP_OUT, TIM2_CH3 (AF1) */
#define CMP_PIN         10u
#define CMP_AF          1u

/* ---- Nucleo ------------------------------------------------------------------------------------ */
#define LED_PORT        GPIOA   /* LD2 */
#define LED_PIN         5u
#define BTN_PORT        GPIOC   /* B1 (USER), high when pressed: aborts a shot */
#define BTN_PIN         13u
#define VCP_TX_PORT     GPIOA   /* ST-LINK virtual COM port on LPUART1, AF12 */
#define VCP_TX_PIN      2u
#define VCP_RX_PORT     GPIOA
#define VCP_RX_PIN      3u
#define VCP_AF          12u
/* 115200 is safe on any bridge. The STLINK-V3E's own limits are in UM2448 (not on this laptop): try
 * -DMAG_VCP_BAUD=921600 (a dump then takes 0.7 s instead of 5 s) once the terminal shows it works. */
#ifndef MAG_VCP_BAUD
#define MAG_VCP_BAUD    115200u
#endif
#define VCP_BAUD        MAG_VCP_BAUD

/* ---- GPS: SparkFun NEO-F10N (GPS-24114) wired to the Nucleo's Arduino sockets, no carrier change -----
 * (HANDOFF item 72; UM2505 Tables 10 and 15). The rev E carrier leaves CN10-6, -31, -34 and -35 open. */
#define GPS_RX_PORT     GPIOC   /* D0 (CN9-1; also CN10-6): USART1_RX, AF7, from the GPS's TX. Nucleo default
                                   (SB19 on). Pulled up, so a bare Nucleo reads an idle line */
#define GPS_RX_PIN      5u
#define GPS_TX_PORT     GPIOC   /* D1 (CN9-2; also CN10-34): USART1_TX, AF7. The firmware never sends, so it
                                   stays an input unless MAG_GPS_TX: nothing drives the GPS's RX */
#define GPS_TX_PIN      4u
#define GPS_UART_AF     7u
#define PPS_PORT        GPIOB   /* D3 (CN9-4; also CN10-31): TIM2_CH2, AF1, the GPS's PPS. PB3 also goes to
                                   the ST-LINK's SWO input (SB15), unused. Pulled down: a bare Nucleo sees none */
#define PPS_PIN         3u
#define PPS_AF          1u
#ifndef MAG_GPS_TX
#define MAG_GPS_TX      0
#endif

/* Rev C stacks only: PB7 (CN7-21, bus 19) enabled the old power board's -5 V_A. Rev E leaves it spare. */
#ifndef MAG_N5_SHDN_PB7
#define MAG_N5_SHDN_PB7 0
#endif

/* ---- Clock ------------------------------------------------------------------------------------- */
/* HSE_VALUE comes from the build (CMakeLists.txt): the Nucleo's crystal, or a TCXO in bypass
 * (MAG_HSE_BYPASS=1, after the solder-bridge changes of HANDOFF item 27). */
#define SYSCLK_HZ       170000000u

/* ---- Interrupt priorities (0 = most urgent) ----------------------------------------------------- */
#define PRIO_SAFETY     0u      /* I_SENSE analog watchdog, gate guard: drop GATE_CMD */
#define PRIO_SEQUENCER  1u      /* TIM2 compare (shot_tick), XFER watch; TIM2_CH2 (PPS) shares this vector */
#define PRIO_CONSOLE    3u      /* LPUART1 receive */
#define PRIO_GPS        3u      /* USART1 receive */

typedef enum { CLOCK_HSE, CLOCK_HSI } clock_source_t;

clock_source_t board_clock_init(void);
clock_source_t board_clock_source(void);
void board_clock_lost(void);            /* from the NMI: the HSE stopped */
bool board_clock_lost_pending(void);    /* true once per loss, and clears it */
bool board_clock_lost_peek(void);       /* a loss not yet handled by the main loop */
const char *board_watchdog_mode(void);
const char *board_silicon(void);        /* Z, Y or X from DBGMCU_IDCODE */

#ifndef MAG_DEBUG_BUILD
#define MAG_DEBUG_BUILD 0
#endif
void board_gpio_init(void);
void board_console_init(void);
void board_gps_init(void);              /* USART1 receive into gps_rx_isr(); after gps_init() */
void board_watchdog_start(void);
void board_watchdog_kick(void);
void board_delay_us(uint32_t us);
void board_led(bool on);
bool board_button(void);

/* Console */
void board_write(const char *s, uint32_t n);
int board_read_char(void);              /* -1 if none */

/* Analog and timing (hw.c). HardFault_Handler in hw.c writes GATE/TUNE/RLY through hard-coded GPIOC/GPIOD
 * BSRR addresses (it may not use the stack): a pin change here needs a change there; the build checks it. */
void hw_init(void);
uint32_t hw_vref_mv(void);              /* VREF+ from VREFINT and its factory calibration */

/* Simple GPIO helpers */
static inline void pin_high(GPIO_TypeDef *p, uint32_t n) { p->BSRR = 1u << n; }
static inline void pin_low(GPIO_TypeDef *p, uint32_t n) { p->BSRR = 1u << (n + 16u); }
static inline bool pin_read(GPIO_TypeDef *p, uint32_t n) { return (p->IDR >> n) & 1u; }
static inline bool pin_out(GPIO_TypeDef *p, uint32_t n) { return (p->ODR >> n) & 1u; }
/* mode: 0 input, 1 output, 2 alternate function, 3 analog */
static inline void pin_mode(GPIO_TypeDef *p, uint32_t n, uint32_t mode)
{
    p->MODER = (p->MODER & ~(3u << (2u * n))) | (mode << (2u * n));
}

#endif
