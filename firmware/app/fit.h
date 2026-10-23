/* fit.h - the frequency of one proton-precession record: FFT seed, then a Levenberg-Marquardt fit of
 *         v = A exp(-R t) sin(2 pi f t + phi) + c.
 *
 * The C port of est_fft / est_fit in simulations/frequency-estimation/frequency_estimation.py, tested
 * against it in tests/fit (README there). Portable C99: no malloc, no OS, no static state.
 *
 * Stages (t = k / fs, k = 0 .. n-1):
 *   1. Coarse seed: 2,048-point real FFT (Hann) of the first 2,048 samples, the in-band peak and a
 *      parabola through the log magnitudes. FIT_E_NOPEAK if the peak is not FIT_PEAK_MIN_RATIO times
 *      the mean in-band power (bins more than 3 away from the peak).
 *   2. est_fft itself, evaluated only near the coarse peak: the Hann window over the whole record,
 *      the 4x zero-padded grid (fs / 4n), the in-band local maximum and the same log parabola.
 *      Its result is f_seed_hz.
 *   3. est_fit's seed: amplitude, phase and offset by linear least squares at f_seed with T2 = 1 s.
 *   4. est_fit's Levenberg-Marquardt: lambda 1e-3, /3 on an accepted step (floor 1e-9), x5 on a
 *      rejected one, (JTJ + lambda diag(JTJ)) delta = J^T r, R floored at 1e-3 /s, stop on an
 *      accepted step with |delta_f| < 1e-7 Hz, at most 40 iterations.
 *
 * Arithmetic (Cortex-M4F: float in hardware, double in software): every pass over the record works
 * in blocks of 64 samples. The phase and exp(-R t) at each block start come from double recurrences
 * (no float phase accumulator: the record holds ~12,000 rad); inside a block, float tables of
 * cos/sin(k w) and exp(-R k / fs) rotate and scale them; each block's sums are float and are added
 * to double totals. The accept test of a step uses the double cost change when it is larger than
 * the rounding of the float model; below that it uses -(g + g_new) . delta (the exact change of a
 * quadratic cost, from the gradients J^T r at both ends), which keeps the steps of the last
 * micro-hertz well defined.
 *
 * Cost on the M4F at 170 MHz, counted from the arm-none-eabi-gcc -O2 code (not measured on hardware):
 * an LM pass is 61 instructions per sample (51 single-precision operations, no calls, no spills),
 * ~68 cycles, plus ~2,400 cycles of software double per 64-sample block: ~2.0 M cycles. The seed is
 * ~6.6 M (stage 2's pass ~4.9 M of it). A fit costs the seed + (iterations + 1) passes: ~16 M cycles
 * (0.09 s) at the host tests' mean of 3.6 iterations, ~26 M at their worst (6 iterations, two stage-2
 * passes). A fit that runs all 40 iterations costs ~89 M (0.52 s).
 */
#ifndef FIT_H
#define FIT_H

#include <stdint.h>

/* Status codes (fit_result_t.status and the return value of fit_record). */
#define FIT_OK              0
#define FIT_E_ARGS          (-1)    /* NULL pointer, workspace too small, n < FIT_SEED_FFT_N, bad band */
#define FIT_E_NOPEAK        (-2)    /* no spectral peak in [f_lo, f_hi] */
#define FIT_E_DIVERGED      (-3)    /* no convergence in 40 iterations, or non-finite values */
#define FIT_E_DECAY         (-4)    /* R pinned at its 1e-3 /s floor (wanted <= 0) or above FIT_DECAY_MAX_PER_S */

#define FIT_SEED_FFT_N      2048u   /* stage 1: real FFT length = samples used; n must be at least this */
#define FIT_WORK_FLOATS     2048u   /* workspace the caller passes, in floats (8 KB) */

#ifndef FIT_PEAK_MIN_RATIO
#define FIT_PEAK_MIN_RATIO  20.0    /* stage-1 peak power over the mean in-band noise power */
#endif
#ifndef FIT_DECAY_MAX_PER_S
#define FIT_DECAY_MAX_PER_S 100.0   /* R above this (T2* < 10 ms) is not a FID */
#endif

typedef struct {
    double f_hz;          /* fitted frequency */
    double f_sigma_hz;    /* 1 sigma from the final J^T J and the residual variance (white-noise approximation) */
    double amp;           /* A, ADC codes (>= 0: a negative fitted A is reported as -A with phi + pi) */
    double phase_rad;     /* phi at the first sample, in [-pi, pi) */
    double decay_per_s;   /* R = 1/T2* */
    double offset;        /* c, ADC codes */
    double resid_rms;     /* rms of the final residual, ADC codes */
    double f_seed_hz;     /* frequency from the seed stage (stage 2, the est_fft equivalent) */
    int    iterations;    /* LM iterations run, accepted and rejected */
    int    status;        /* FIT_OK or a negative FIT_E_* code */
} fit_result_t;

/* Fit one record of n ADC codes x[] sampled at fs_hz (sample k at t = k / fs_hz). The peak is searched
 * in (f_lo_hz, f_hi_hz), with 0 < f_lo_hz < f_hi_hz < fs_hz / 2. work[] is scratch of at least
 * FIT_WORK_FLOATS floats; x[] is only read. progress (may be NULL) is called after each seed stage and
 * once per LM iteration. Every field of *out is written (zeros where a stage did not run); returns
 * out->status, or FIT_E_ARGS if out is NULL. */
int fit_record(const uint16_t *x, uint32_t n, double fs_hz, double f_lo_hz, double f_hi_hz,
               float *work, uint32_t work_len, void (*progress)(void), fit_result_t *out);

#endif /* FIT_H */
