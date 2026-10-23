/* gps.h - the GPS: position and UTC from its NMEA sentences, and the true clock rate from its 1 PPS.
 *
 * The receiver is a u-blox NEO-F10N (SparkFun GPS-24114, HANDOFF item 72) on the Nucleo's Arduino sockets:
 * its TX into USART1_RX (PC5, D0) at GPS_BAUD, its PPS into TIM2_CH2 (PB3, D3). It is optional: with
 * nothing connected every GPS field reads na and shots run as before.
 *
 * Two interrupt entry points fill two queues, and the main loop drains both in time order:
 *   gps_rx_isr(c, now)   USART1 receive. Assembles sentences; RMC and GGA are queued with the TIM2 count
 *                        of their '$' (other sentences are counted and dropped here).
 *   gps_pps_isr(t)       TIM2_CH2 capture of the PPS rising edge, in the same 170 MHz count as everything else.
 *   gps_poll(now)        main loop. Everything else is main loop only.
 *
 * The clock: TIM2 counts between PPS edges are the true clock rate. Over the last GPS_CLK_WINDOW_S + 1
 * pulses, ticks_per_s = (count difference) / (seconds between them). A frequency measured on the nominal
 * clock (TICK_HZ) is corrected by f_true = f x ticks_per_s / TICK_HZ (1 ppm = 0.05 nT at 50 uT).
 *
 * Pulses: an edge a whole number of seconds (1 to GPS_PPS_MAX_GAP_S) after the chain's last pulse extends the
 * chain. Any other edge (a glitch) is counted and ignored, so it can move neither the chain nor its UTC;
 * GPS_PPS_RESYNC of them in a row, or a longer gap, start a new chain from the latest edge.
 *
 * UTC: u-blox sends an epoch's sentences after that epoch's pulse, so an RMC with status A and a whole
 * second, arriving at most GPS_LAG_MAX_US after a pulse, names that pulse's second. A chain gets its UTC
 * from two such RMCs that agree (on two different pulses); one that disagrees takes the UTC away until two
 * agree again (utc_jumps). The UTC of any TIM2 count within GPS_UTC_MAX_EXTRAP_S of the last pulse follows
 * from the measured clock. Check the order once on a logic analyzer (BRINGUP.md step 10d).
 *
 * Every TIM2 count handed to the query functions must be within 12 s of the last gps_poll() count.
 */
#ifndef GPS_H
#define GPS_H

#include <stdbool.h>
#include <stdint.h>

#include "fmt.h"

/* ---- Interrupt side (one producer each) ---- */
void gps_rx_isr(char c, uint32_t now);
void gps_pps_isr(uint32_t t);
void gps_pps_lost_isr(void);        /* a second edge came before the first was read (TIM2 overcapture) */

/* ---- Main loop ---- */
void gps_init(uint32_t now);   /* before the GPS interrupts are enabled; now = the TIM2 count */
void gps_poll(uint32_t now);
void gps_clock_changed(void);       /* the MCU clock was re-initialized: forget every PPS measurement */
void gps_echo(uint32_t n);          /* print the next n queued sentences as "NMEA <sentence>" lines */

/* The measured clock for an event at TIM2 count t: true if a calibration of at least GPS_CLK_MIN_S, at
 * most GPS_CLK_MAX_AGE_S from t, exists. ticks_per_s and span_s (seconds it spans) may be NULL. */
bool gps_clock(uint32_t t, double *ticks_per_s, uint32_t *span_s);

/* UTC of TIM2 count t as whole seconds since 1970-01-01 and a fraction in [0, 1). */
bool gps_utc(uint32_t t, int64_t *whole, double *frac);

/* Report fields for an event at TIM2 count t: " utc= fix= sats= hdop= lat= lon= alt_m= spd_mps= cog_deg=
 * pos_age_s=", each "na" when unknown. */
void gps_format_fields(fmt_t *f, uint32_t t);

/* The "GPS ..." status line (no line ending). now is the current TIM2 count. */
void gps_format_status(fmt_t *f, uint32_t now);

/* "2026-10-22T15:04:07.412345Z" */
void gps_format_utc(fmt_t *f, int64_t whole, double frac);

#endif
