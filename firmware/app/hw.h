/* hw.h - what the shot sequencer needs from the board.
 *
 * bsp/hw.c implements this on the NUCLEO-G474RE; tests/shot/fake_hw.c implements it on the PC.
 * Times are TIM2 counts (TICK_HZ), unsigned and wrapping every 25.3 s: compare them only by
 * signed difference, (int32_t)(a - b).
 */
#ifndef HW_H
#define HW_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { OUT_GATE, OUT_RLY, OUT_TUNE, OUT_BLANK2, OUT_COUNT } hw_out_t;

/* Control lines. GATE_CMD can also be dropped by the board itself (analog watchdog, guard timer). */
void hw_set(hw_out_t out, bool high);
bool hw_get(hw_out_t out);

uint32_t hw_now(void);
void hw_wake_at(uint32_t t);            /* run shot_tick() at t (at once if t has passed) */

/* XFER_SENSE (PB0) on ADC3 at SAMPLE_HZ into buf. Returns the time of sample 0. */
uint32_t hw_xfer_start(uint16_t *buf, uint32_t n);
uint32_t hw_xfer_count(void);           /* samples written so far */
void hw_xfer_stop(void);
void hw_xfer_watch(bool on);            /* PB0 as a digital input: a falling edge calls shot_fault() */

/* I_SENSE (PA1) with the analog watchdog: above awd_code the board drops GATE_CMD itself. */
void hw_isense_start(uint16_t awd_code);
uint16_t hw_isense_mean(void);          /* ADC code, averaged over the last ~1 ms */
void hw_isense_stop(void);

/* Hard limit on the gate-on time, independent of the sequencer. */
void hw_guard_arm(uint32_t ticks);
void hw_guard_disarm(void);

/* The record: ADC1 (PA0) at SAMPLE_HZ and TIM2_CH3 (PB10) edges, both by DMA.
 * Returns the time of sample 0. */
uint32_t hw_record_start(uint16_t *buf, uint32_t n, uint32_t *edges, uint32_t n_edges);
uint32_t hw_record_count(void);
uint32_t hw_edges_count(void);
bool hw_edges_overflow(void);
void hw_record_stop(void);

#endif
