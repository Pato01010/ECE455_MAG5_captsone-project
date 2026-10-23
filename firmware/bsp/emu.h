/* emu.h - bench emulator of XFER_SENSE on PA4 (bare Nucleo only: jumper PA4 to PB0).
 */
#ifndef EMU_H
#define EMU_H

#include <stdbool.h>
#include <stdint.h>

/* fault: none, dead, no_release, k1_stuck, missed, no_pickup, drop, rev_c. Returns -1 if unknown. */
int emu_start(uint32_t vref_mv, const char *fault);
void emu_stop(void);
bool emu_on(void);
void emu_poll(void);    /* from the main loop, as often as possible */

#endif
