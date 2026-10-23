/* app.h - the console, the shot reports and the binary dumps.
 */
#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stdint.h>

#define FW_VERSION "0.2.0"

/* What the application needs from the platform (bsp/main.c on the board). */
void plat_write(const char *s, uint32_t n);
void plat_kick(void);                   /* refresh the watchdog */
uint32_t plat_vref_mv(void);            /* measure VREF+; only while no shot runs */
const char *plat_clock_name(void);
bool plat_clock_ready(void);            /* false while a clock failure waits to be handled */
const char *plat_silicon(void);         /* the STM32's silicon revision (which errata apply) */
const char *plat_watchdog_mode(void);   /* whether a debugger halt freezes the watchdog */
int plat_emu(const char *mode);         /* bench XFER emulator: "off" or a mode; -1 if unknown */

void app_init(uint32_t vref_mv, const char *reset_cause);
void app_char(int c);                   /* one received console character */
void app_poll(void);                    /* main loop: results, reports, runs */
void app_button(void);                  /* B1 pressed: abort */
bool app_shot_running(void);

#endif
