/* noise.c - the residual's noise density near the fitted frequency.
 *
 * One pass over the record: residual r = x - (A exp(-R t) sin(2 pi f t + phi) + c), Hann window w,
 * and the DFT of w r at f +/- 15, 20, ... 60 Hz. For white noise of variance s2 the expected |DFT|^2
 * is s2 x sum(w^2), so the ratio of the two is the density near f over the white density.
 * Same arithmetic as fit.c: double anchors at the start of every 64-sample block, float inside.
 */
#include "noise.h"

#include <math.h>

#define BLK     64u
#define TWO_PI  6.283185307179586476925

typedef struct {
    double c, s;
} rot_t;

static void rot_step(rot_t *a, const rot_t *b)
{
    const double c = a->c * b->c - a->s * b->s;
    a->s = a->s * b->c + a->c * b->s;
    a->c = c;
}

double noise_density_ratio(const uint16_t *x, uint32_t n, double fs, const fit_result_t *fit)
{
    rot_t bin[NOISE_BINS], bin_blk[NOISE_BINS];
    float step_c[NOISE_BINS], step_s[NOISE_BINS];
    double re[NOISE_BINS] = { 0.0 }, im[NOISE_BINS] = { 0.0 };
    double r2 = 0.0, w2 = 0.0;

    if (x == NULL || fit == NULL || n < 2u * BLK || !(fs > 0.0) || !(fit->f_hz > 0.0) || !isfinite(fit->amp)
        || !isfinite(fit->phase_rad) || !isfinite(fit->decay_per_s) || !isfinite(fit->offset))
        return -1.0;

    for (int j = 0; j < NOISE_BINS; j++) {
        const int k = j / 2;                                  /* 0..9 -> 15..60 Hz */
        const double off = (15.0 + 5.0 * (double)k) * ((j & 1) ? -1.0 : 1.0);
        const double w = -TWO_PI * (fit->f_hz + off) / fs;
        bin[j].c = 1.0;
        bin[j].s = 0.0;
        step_c[j] = (float)cos(w);
        step_s[j] = (float)sin(w);
        bin_blk[j].c = cos(w * (double)BLK);
        bin_blk[j].s = sin(w * (double)BLK);
    }

    const double wm = TWO_PI * fit->f_hz / fs;                 /* model phase step */
    const rot_t m_blk = { cos(wm * (double)BLK), sin(wm * (double)BLK) };
    const float m_c = (float)cos(wm), m_s = (float)sin(wm);
    const double e_blk = exp(-fit->decay_per_s * (double)BLK / fs);
    const float e_step = (float)exp(-fit->decay_per_s / fs);
    const double ww = TWO_PI / (double)(n - 1u);               /* Hann window phase step */
    const rot_t w_blk = { cos(ww * (double)BLK), sin(ww * (double)BLK) };
    const float w_c = (float)cos(ww), w_s = (float)sin(ww);
    const double c_round = floor(fit->offset + 0.5);
    const int32_t c_int = (int32_t)c_round;
    const float c_frac = (float)(fit->offset - c_round);

    rot_t mph = { cos(fit->phase_rad), sin(fit->phase_rad) };  /* model phasor at the block start */
    rot_t wph = { 1.0, 0.0 };                                  /* window phasor */
    double env = fit->amp;

    for (uint32_t b0 = 0; b0 < n; b0 += BLK) {
        const uint32_t len = (n - b0 < BLK) ? n - b0 : BLK;
        float mc = (float)mph.c, ms = (float)mph.s, e = (float)env;
        float wc = (float)wph.c, wsn = (float)wph.s;
        float bc[NOISE_BINS], bs[NOISE_BINS], fre[NOISE_BINS], fim[NOISE_BINS];
        float fr2 = 0.0f, fw2 = 0.0f;
        for (int j = 0; j < NOISE_BINS; j++) {
            bc[j] = (float)bin[j].c;
            bs[j] = (float)bin[j].s;
            fre[j] = fim[j] = 0.0f;
        }
        for (uint32_t k = 0; k < len; k++) {
            const float r = ((float)((int32_t)x[b0 + k] - c_int) - e * ms) - c_frac;
            const float w = 0.5f - 0.5f * wc;
            const float wr = w * r;
            fr2 += r * r;
            fw2 += w * w;
            for (int j = 0; j < NOISE_BINS; j++) {
                const float nc = bc[j] * step_c[j] - bs[j] * step_s[j];
                fre[j] += wr * bc[j];
                fim[j] += wr * bs[j];
                bs[j] = bs[j] * step_c[j] + bc[j] * step_s[j];
                bc[j] = nc;
            }
            const float nmc = mc * m_c - ms * m_s;              /* advance the model and the window */
            ms = ms * m_c + mc * m_s;
            mc = nmc;
            e *= e_step;
            const float nwc = wc * w_c - wsn * w_s;
            wsn = wsn * w_c + wc * w_s;
            wc = nwc;
        }
        r2 += (double)fr2;
        w2 += (double)fw2;
        for (int j = 0; j < NOISE_BINS; j++) {
            re[j] += (double)fre[j];
            im[j] += (double)fim[j];
            rot_step(&bin[j], &bin_blk[j]);
        }
        rot_step(&mph, &m_blk);
        rot_step(&wph, &w_blk);
        env *= e_blk;
    }

    double p = 0.0;
    for (int j = 0; j < NOISE_BINS; j++) p += re[j] * re[j] + im[j] * im[j];
    p /= (double)NOISE_BINS;
    const double var = r2 / (double)n;
    if (!(var > 0.0) || !(w2 > 0.0)) return -1.0;
    return p / (var * w2);
}
