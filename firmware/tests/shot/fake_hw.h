/* fake_hw.h - a simulated board for running the shot sequencer on the PC.
 *
 * Time only moves when the test loop moves it. XFER_SENSE comes from app/xfer_model.c - the same
 * waveform the bench emulator (bsp/emu.c) plays on the Nucleo's DAC.
 */
#ifndef FAKE_HW_H
#define FAKE_HW_H

#include <stdbool.h>
#include <stdint.h>

#include "hw.h"
#include "xfer_model.h"

typedef struct {
    const char *name;
    uint32_t t_pol_us;
    bool tuned, dry;
    xfer_model_t m;             /* XFER_SENSE, including its faults */
    float overcurrent_at_ms;    /* after GATE_CMD rises; 0 = none */
    float stall_at_ms;          /* after GATE_CMD rises the sequencer interrupt stops being served; 0 = none */
    float abort_at_ms;          /* after the shot starts; 0 = none */
    float clock_fail_at_ms;     /* after GATE_CMD rises the HSE stops: the NMI drops GATE/TUNE; 0 = none */
    float i_sense_mv;           /* I_SENSE while polarizing */
    /* the record (tests/app): a decay A exp(-t/T2) sin(2 pi f t + phi) around code 2048, and the
     * comparator's edges from it with hysteresis; all zero = an empty record */
    double fid_amp, fid_f_hz, fid_t2_s, fid_phase, noise_codes, hyst_codes;
} scenario_t;

#define MAX_TRANSITIONS 64

typedef struct {
    uint32_t t;
    hw_out_t out;
    bool high;
} transition_t;

typedef struct {
    const scenario_t *sc;
    uint32_t now;
    bool out[OUT_COUNT];
    transition_t log[MAX_TRANSITIONS];
    int n_log;

    bool wake_set;
    uint32_t wake;

    xfer_history_t hist;        /* RLY_CMD / GATE_CMD edges for the XFER model */
    bool gate_rise_seen;
    uint32_t t_gate_rise;

    uint16_t *xb;
    uint32_t xn, xt0, xfill;
    bool xon, watch;

    bool ion;
    uint16_t awd;
    bool guard_on;
    uint32_t guard_at;

    bool ron;
    uint32_t rt0, rn, rcount, n_edges;

    uint32_t t_start;
    bool abort_pending, oc_pending, drop_pending, clock_pending;
    uint32_t noise;     /* LCG state */
} fake_t;

extern fake_t F;

void fake_reset(const scenario_t *sc, uint32_t t_start);
/* Move time to the next event (sequencer wake-up, guard, overcurrent, XFER drop, user abort) and
 * handle it the way the board would, highest priority first. False when nothing is scheduled. */
bool fake_advance(void);

#endif
