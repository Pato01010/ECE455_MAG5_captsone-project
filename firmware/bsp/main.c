/* main.c - start-up and the main loop.
 *
 * Start-up order matters: the control lines are driven low before anything else runs, and the
 * watchdog starts before the console accepts a command. In the loop a clock failure is handled
 * before any command, so no shot can start on a time base that has just failed.
 */
#include <string.h>

#include "app.h"
#include "board.h"
#include "config.h"
#include "emu.h"
#include "gps.h"
#include "hw.h"

#define VREF_MIN_MV 3000u   /* a reading outside these is a fault, not the Nucleo's 3.26 V reference */
#define VREF_MAX_MV 3500u

static uint32_t g_vref_mv = VREF_NOMINAL_MV;

static const char *reset_cause(void)
{
    uint32_t csr = RCC->CSR;
    const char *cause = "power-on";
    if (csr & RCC_CSR_IWDGRSTF) cause = "WATCHDOG";
    else if (csr & RCC_CSR_WWDGRSTF) cause = "window-watchdog";
    else if (csr & RCC_CSR_SFTRSTF) cause = "software";
    else if (csr & RCC_CSR_LPWRRSTF) cause = "low-power";
    else if (csr & RCC_CSR_OBLRSTF) cause = "option-bytes";
    else if (csr & RCC_CSR_BORRSTF) cause = "power-on";
    else if (csr & RCC_CSR_PINRSTF) cause = "reset-pin";
    RCC->CSR |= RCC_CSR_RMVF;
    return cause;
}

static void say(const char *s)
{
    board_write(s, (uint32_t)strlen(s));
}

void plat_write(const char *s, uint32_t n) { board_write(s, n); }
void plat_kick(void) { board_watchdog_kick(); }
bool plat_clock_ready(void) { return !board_clock_lost_peek(); }
const char *plat_silicon(void) { return board_silicon(); }
const char *plat_watchdog_mode(void) { return board_watchdog_mode(); }

/* VREF+ sets every voltage the firmware reads, the I_SENSE trip point included: refuse a reading that
 * cannot be the Nucleo's reference. */
uint32_t plat_vref_mv(void)
{
    uint32_t v = hw_vref_mv();
    if (v < VREF_MIN_MV || v > VREF_MAX_MV) {
        say("WARN VREF+ reading out of 3000-3500 mV: using the nominal 3250 mV\r\n");
        v = VREF_NOMINAL_MV;
    }
    g_vref_mv = v;
    return v;
}

int plat_emu(const char *mode)
{
    if (strcmp(mode, "off") == 0) {
        emu_stop();
        return 0;
    }
    return emu_start(g_vref_mv, mode);
}

const char *plat_clock_name(void)
{
    if (board_clock_source() == CLOCK_HSI) return "HSI_1pct";
    return MAG_HSE_BYPASS ? "HSE_TCXO" : "HSE_crystal";
}

int main(void)
{
    const char *cause = reset_cause();

    board_gpio_init();          /* control lines low first */
    board_clock_init();
    board_console_init();
    gps_init(0u);               /* TIM2 starts from 0 in hw_init(), which also enables the PPS capture */
    hw_init();
    board_gps_init();
    uint32_t vref_mv = plat_vref_mv();
    hw_set(OUT_BLANK2, true);   /* idle: BLANK2 holds the front end's gain stage */
    board_watchdog_start();
    app_init(vref_mv, cause);

    bool btn_prev = board_button();
    uint32_t led_at = hw_now();
    bool led = false;

    for (;;) {
        int c;
        board_watchdog_kick();

        if (board_clock_lost_pending()) {
            board_clock_init();             /* retries the HSE, else the PLL from HSI16 */
            gps_clock_changed();            /* the measured clock belonged to the old one */
            say("CLOCK the HSE stopped: the shot was aborted; clock is now ");
            say(plat_clock_name());
            say("\r\n");
        }

        while ((c = board_read_char()) >= 0) app_char(c);
        gps_poll(hw_now());                 /* before app_poll: a report uses every pulse and fix so far */
        app_poll();
        emu_poll();

        bool btn = board_button();
        if (btn && !btn_prev) app_button();
        btn_prev = btn;

        /* LD2: on during a shot, a slow blink when idle */
        if (app_shot_running()) {
            board_led(true);
        } else if ((int32_t)(hw_now() - led_at) >= 0) {
            led = !led;
            board_led(led);
            led_at = hw_now() + 500000u * TICKS_PER_US;
        }
    }
}
