/* xfer.h - the XFER readback checks (a), (a2), (b), (c), (d) on a captured XFER_SENSE trace.
 *
 * The rules are the acquisition rev C README's "Firmware timing table" and "The XFER readback".
 * Pure functions: no hardware access, so the PC tests run them unchanged.
 */
#ifndef XFER_H
#define XFER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const uint16_t *code;   /* ADC codes, one per sample */
    uint32_t n;             /* samples captured */
    int32_t t0_us;          /* time of sample 0 against the anchor (RLY_CMD rising, or gate-off) */
    float mv_per_code;      /* VREF+ / 4095, in mV */
} xfer_trace_t;

/* Mean / median of the samples whose time lies in [from_us, to_us). False if there are none. */
bool xfer_mean_mv(const xfer_trace_t *tr, int32_t from_us, int32_t to_us, float *mv);
bool xfer_median_mv(const xfer_trace_t *tr, int32_t from_us, int32_t to_us, float *mv);

/* (c): XFER_SENSE high at +38, +39 and +40 ms (median of the samples within +/-0.1 ms). */
bool xfer_check_c(const xfer_trace_t *tr, float mv[3]);

typedef struct {
    bool rose;              /* some sample is above VIH */
    int32_t t_rise_us;      /* first sample above VIH */
    float plateau_mv;       /* median of the samples above VIH from t_rise + 1 ms to t_rise + 3 ms */
    float held_mv;          /* mean over +38 to +40 ms */
    float step_mv;          /* plateau - held: K_XFER's pickup, healthy 45-70 mV */
    int32_t pickup_us;      /* half-step point after t_rise, -1 if none */
    bool pickup_flag;       /* pickup outside 2.5-12 ms after t_rise: log only */
    bool ok;                /* step_mv >= D_MIN_STEP_MV */
} xfer_d_t;

/* (d): did K_XFER pick up? The median over the samples above VIH ignores K1's NC contact bounce. */
void xfer_check_d(const xfer_trace_t *tr, xfer_d_t *out);

#endif
