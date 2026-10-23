/* xfer_model.h - what XFER_SENSE (PB0) does around a shot, as the READMEs describe it.
 *
 * Used by the PC tests (tests/shot/fake_hw.c) and by the bench emulator (bsp/emu.c, DAC1 on PA4),
 * so the firmware is tested on the bench against the same waveform it was tested against on the PC.
 * Levels: acquisition rev C README "The XFER readback"; times: polarization rev D README.
 */
#ifndef XFER_MODEL_H
#define XFER_MODEL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float v_idle_mv;        /* K1 released, K_XFER held: ~2900 */
    float release_step_mv;  /* (a2): the front end's Q2 drops K_XFER, XFER_SENSE rises 49-71 mV */
    float pick_step_mv;     /* (d): K_XFER picks up again, XFER_SENSE falls 45-70 mV */
    float nc_open_ms;       /* K1's NC opens this long after RLY_CMD rises */
    float nc_make_ms;       /* K1's NC makes this long after RLY_CMD falls */
    float bounce_ms;        /* NC contact bounce after it makes */
    float pick_ms;          /* K_XFER picks up this long after the NC makes */
    bool xfer_dead;         /* (a) fault: XFER never there */
    bool k1_stuck;          /* (b) fault: K1 never operates */
    bool missed_transfer;   /* (c) fault: K1's NC never makes again */
    float drop_at_ms;       /* XFER falls this long after gate-off (0 = never) */
} xfer_model_t;

/* Edges of RLY_CMD and GATE_CMD seen so far (TIM2 counts). */
typedef struct {
    bool rly_rise_seen, rly_fall_seen, gate_fall_seen;
    uint32_t t_rly_rise, t_rly_fall, t_gate_fall;
} xfer_history_t;

void xfer_model_rev_de(xfer_model_t *m);    /* healthy rev D/E stack */
float xfer_model_mv(const xfer_model_t *m, const xfer_history_t *h, uint32_t t);

#endif
