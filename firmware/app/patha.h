/* patha.h - Path A: the frequency from the comparator's edge timestamps, gated by the ADC fit.
 *
 * The rule is the acquisition rev C README's "Path A (the comparator)": tuned shots with peak/rms >= 15
 * only (the caller decides), every edge gated against the fit's period, rising and falling averaged.
 *
 * TIM2 (32-bit, tick_hz) captures both edges of the LT1719 into edges[], increasing modulo 2^32;
 * ADC sample k is at count t_rec0 + k * tick_hz / fs, so an edge's time against the fit's t = 0 is
 * the signed 32-bit count difference / tick_hz. Rising and falling interleave, but noise can add or
 * drop edges, and the capture does not record which edge it was. So:
 *   1. Edges are used while the fitted envelope A exp(-R t) >= 2 x noise_rms, and t >= 0.
 *   2. Each edge's phase 2 pi f t + phi against the fit puts it near 0 (rising) or pi (falling);
 *      the two cluster centres are measured as the mean phase offset of each cluster (hysteresis and
 *      comparator offset move them a little), then each edge goes to the nearer centre.
 *   3. Gate: an edge more than 25 % of a period from its class's predicted crossing is rejected,
 *      and of several edges of one class in one cycle only the one nearest the prediction is kept.
 *   4. Per class, a least-squares line t = a + b n (n = cycle number from the fit's phase, t = int64
 *      count difference against the class's first edge, sums in double: never a raw count in
 *      float); f = tick_hz / b; f_hz is the mean of the two classes.
 */
#ifndef PATHA_H
#define PATHA_H

#include <stdint.h>

#include "fit.h"

#define PATHA_OK            0
#define PATHA_E_ARGS        (-1)    /* NULL pointer, or tick_hz, fs_hz or noise_rms not positive */
#define PATHA_E_FIT         (-2)    /* fit->status not FIT_OK, or its f, A, phi or R unusable */
#define PATHA_E_FEW         (-3)    /* fewer than PATHA_MIN_EDGES gated edges in a class */

#define PATHA_MIN_EDGES     10u     /* per class */
#define PATHA_GATE_FRAC     0.25    /* of a period, either side of the predicted crossing */
#define PATHA_ENV_STOP      2.0     /* stop once A exp(-R t) < PATHA_ENV_STOP x noise_rms */

typedef struct {
    double f_rise_hz, f_fall_hz, f_hz;   /* f_hz = mean of the two */
    uint32_t n_rise, n_fall;             /* edges used */
    uint32_t n_rejected;                 /* edges in the usable time window that failed the gate or lost
                                            the one-per-cycle choice */
    int status;                          /* PATHA_OK = 0 or a negative PATHA_E_* code */
} patha_result_t;

/* Every field of *out is written (zeros where not reached); returns out->status, or PATHA_E_ARGS if out
 * is NULL. noise_rms is in ADC codes, like fit->amp (fit->resid_rms is the natural value). */
int patha_estimate(const uint32_t *edges, uint32_t n_edges, uint32_t t_rec0_counts, double tick_hz,
                   const fit_result_t *fit, double fs_hz, double noise_rms, patha_result_t *out);

#endif /* PATHA_H */
