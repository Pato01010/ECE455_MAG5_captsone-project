/* test_noise.c - does f_sigma_hz x sqrt(noise_density_ratio) match the real scatter of f?
 *
 * 80 records per case: a decay (peak/rms 31.6 and 6.7, 2128.875 Hz, T2* 1 s, random phase) on noise of
 * 23 codes rms, either white or band-limited by a 4th-order band-pass at 0.8-3 kHz (two RBJ biquads),
 * quantized to 12 bits. Pass: the rms error of f over the mean corrected sigma lies in 0.8-1.25 (an rms
 * over 80 records scatters about +/-8 %); the density ratio is about 1 on white noise.
 */
#include <math.h>
#include <stdio.h>

#include "fit.h"
#include "noise.h"

#define N       18800u
#define FS      20000.0
#define TWO_PI  6.283185307179586

static uint16_t rec[N];
static float work[FIT_WORK_FLOATS];
static double noise_buf[N];
static unsigned long long rng = 0x9E3779B97F4A7C15ull;
static int failures;

static double uniform(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return ((double)(rng >> 11) + 0.5) / 9007199254740992.0;
}

static double gauss(void)
{
    return sqrt(-2.0 * log(uniform())) * cos(TWO_PI * uniform());
}

/* RBJ band-pass biquad (0 dB peak), centre f0, quality q */
typedef struct { double b0, b1, b2, a1, a2, z1, z2; } biquad_t;

static void bp_init(biquad_t *f, double f0, double q)
{
    const double w = TWO_PI * f0 / FS, alpha = sin(w) / (2.0 * q), a0 = 1.0 + alpha;
    f->b0 = alpha / a0; f->b1 = 0.0; f->b2 = -alpha / a0;
    f->a1 = -2.0 * cos(w) / a0; f->a2 = (1.0 - alpha) / a0;
    f->z1 = f->z2 = 0.0;
}

static double bp_run(biquad_t *f, double v)
{
    const double y = f->b0 * v + f->z1;
    f->z1 = f->b1 * v - f->a1 * y + f->z2;
    f->z2 = f->b2 * v - f->a2 * y;
    return y;
}

static void make_noise(int band_limited, double rms)
{
    biquad_t s1, s2;
    double sum2 = 0.0;
    bp_init(&s1, sqrt(800.0 * 3000.0), sqrt(800.0 * 3000.0) / 2200.0);
    bp_init(&s2, sqrt(800.0 * 3000.0), sqrt(800.0 * 3000.0) / 2200.0);
    for (unsigned k = 0; k < 4000u; k++) (void)bp_run(&s2, bp_run(&s1, gauss()));   /* settle the filter */
    for (unsigned k = 0; k < N; k++) {
        double v = gauss();
        if (band_limited) v = bp_run(&s2, bp_run(&s1, v));
        noise_buf[k] = v;
        sum2 += v * v;
    }
    const double scale = rms / sqrt(sum2 / N);
    for (unsigned k = 0; k < N; k++) noise_buf[k] *= scale;
}

static void run_case(const char *name, int band_limited, double snr, double ratio_lo, double ratio_hi)
{
    const double f_true = 2128.875, sigma_codes = 23.0, amp = snr * sigma_codes;
    const int records = 80;
    double se = 0.0, sw = 0.0, sc = 0.0, sr = 0.0;
    int used = 0;

    for (int r = 0; r < records; r++) {
        fit_result_t fit;
        const double ph = TWO_PI * uniform();
        make_noise(band_limited, sigma_codes);
        for (unsigned k = 0; k < N; k++) {
            const double t = k / FS;
            double v = 2048.0 + amp * exp(-t) * sin(TWO_PI * f_true * t + ph) + noise_buf[k];
            v = floor(v + 0.5);
            rec[k] = (uint16_t)(v < 0.0 ? 0.0 : (v > 4095.0 ? 4095.0 : v));
        }
        if (fit_record(rec, N, FS, 900.0, 3000.0, work, FIT_WORK_FLOATS, NULL, &fit) != FIT_OK) continue;
        const double ratio = noise_density_ratio(rec, N, FS, &fit);
        if (!(ratio > 0.0)) { failures++; printf("  FAIL: ratio %g\n", ratio); continue; }
        se += (fit.f_hz - f_true) * (fit.f_hz - f_true);
        sw += fit.f_sigma_hz;
        sc += fit.f_sigma_hz * sqrt(ratio);
        sr += ratio;
        used++;
    }
    const double rms = sqrt(se / used), mw = sw / used, mc = sc / used, mr = sr / used;
    const double q = rms / mc;
    printf("  %-34s fits %d/%d  rms err %.4f mHz  white sigma %.4f (x%.2f)  corrected %.4f (x%.2f)  ratio %.2f\n",
           name, used, records, rms * 1e3, mw * 1e3, rms / mw, mc * 1e3, q, mr);
    if (used < records * 9 / 10) { failures++; printf("  FAIL: too few fits\n"); }
    if (q < 0.8 || q > 1.25) { failures++; printf("  FAIL: rms / corrected sigma %.2f outside 0.8-1.25\n", q); }
    if (mr < ratio_lo || mr > ratio_hi) { failures++; printf("  FAIL: density ratio %.2f outside %.2f-%.2f\n", mr, ratio_lo, ratio_hi); }
}

int main(void)
{
    printf("Honest sigma: f_sigma_hz x sqrt(noise_density_ratio) against the real scatter\n");
    run_case("white noise, peak/rms 31.6", 0, 31.6, 0.85, 1.15);
    run_case("band-limited 0.8-3 kHz, peak/rms 31.6", 1, 31.6, 2.0, 10.0);
    run_case("band-limited 0.8-3 kHz, peak/rms 6.7", 1, 6.7, 2.0, 10.0);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
