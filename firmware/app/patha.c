/* patha.c - Path A: the frequency from the comparator's edge timestamps (see patha.h).
 */
#include "patha.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define TWO_PI      6.283185307179586476925
#define INV_TWO_PI  0.159154943091895335769
#define PI_D        3.141592653589793238463

typedef struct {
    double s1, sn, snn, st, snt;    /* line-fit sums; n against n0, counts against e0 */
    int64_t n0;                     /* cycle number of the class's first used edge */
    uint32_t e0;                    /* its count */
    uint32_t used;
    int pending;                    /* a candidate is held for cycle n_p ... */
    int64_t n_p;
    double d_p;                     /* ... with this |phase from the predicted crossing| */
    uint32_t e_p;                   /* ... and this count */
} cls_t;

/* [-pi, pi) */
static double wrap_pi(double a)
{
    return a - TWO_PI * floor(a * INV_TWO_PI + 0.5);
}

/* e - ref as a signed count, for |e - ref| < 2^31 (12.6 s at 170 MHz) */
static int64_t count_diff(uint32_t e, uint32_t ref)
{
    const uint32_t d = e - ref;
    return (d >= 0x80000000u) ? (int64_t)d - INT64_C(4294967296) : (int64_t)d;
}

static void commit(cls_t *c)
{
    double dn, dt;
    if (!c->pending) return;
    if (c->used == 0u) {
        c->n0 = c->n_p;
        c->e0 = c->e_p;
    }
    dn = (double)(c->n_p - c->n0);
    dt = (double)(int64_t)(uint32_t)(c->e_p - c->e0);    /* exact: an integer below 2^32 */
    c->s1 += 1.0;
    c->sn += dn;
    c->snn += dn * dn;
    c->st += dt;
    c->snt += dn * dt;
    c->used++;
    c->pending = 0;
}

/* One gated edge of this class: at most one per cycle, the one nearest the predicted crossing. */
static void offer(cls_t *c, int64_t ncyc, double dabs, uint32_t e, uint32_t *rejected)
{
    if (c->pending && ncyc == c->n_p) {
        (*rejected)++;
        if (dabs < c->d_p) {
            c->d_p = dabs;
            c->e_p = e;
        }
        return;
    }
    commit(c);
    c->pending = 1;
    c->n_p = ncyc;
    c->d_p = dabs;
    c->e_p = e;
}

int patha_estimate(const uint32_t *edges, uint32_t n_edges, uint32_t t_rec0_counts, double tick_hz,
                   const fit_result_t *fit, double fs_hz, double noise_rms, patha_result_t *out)
{
    cls_t cl[2];
    double amp, ph, f, rate, t_stop, inv_tick, cen[2], sum_d[2] = { 0.0, 0.0 };
    double slope[2];
    uint32_t i, cnt[2] = { 0u, 0u };
    int k;

    if (out == NULL) return PATHA_E_ARGS;
    memset(out, 0, sizeof *out);
    out->status = PATHA_E_ARGS;
    if ((edges == NULL && n_edges > 0u) || fit == NULL || !isfinite(tick_hz) || !(tick_hz > 0.0)
        || !isfinite(fs_hz) || !(fs_hz > 0.0) || !isfinite(noise_rms) || !(noise_rms > 0.0))
        return PATHA_E_ARGS;

    out->status = PATHA_E_FIT;
    if (fit->status != FIT_OK) return PATHA_E_FIT;
    f = fit->f_hz;
    amp = fit->amp;
    ph = fit->phase_rad;
    rate = fit->decay_per_s;
    if (!(f > 0.0) || !(f < 0.5 * fs_hz) || !isfinite(amp) || amp == 0.0 || !isfinite(ph)
        || !isfinite(rate) || !(rate > 0.0))
        return PATHA_E_FIT;
    if (amp < 0.0) {
        amp = -amp;
        ph += PI_D;
    }
    t_stop = (amp > PATHA_ENV_STOP * noise_rms) ? log(amp / (PATHA_ENV_STOP * noise_rms)) / rate : 0.0;
    inv_tick = 1.0 / tick_hz;

    /* Pass 1: the cluster centres. Against the nominal crossings 0 (rising) and pi (falling) every edge
     * is within +/-pi/2 of one of them, so the mean offset is well defined (no trigonometry per edge). */
    for (i = 0; i < n_edges; i++) {
        const double t = (double)count_diff(edges[i], t_rec0_counts) * inv_tick;
        double th;
        if (t < 0.0 || t >= t_stop) continue;
        th = wrap_pi(TWO_PI * f * t + ph);
        if (fabs(th) <= 0.5 * PI_D) {
            k = 0;
        } else {
            k = 1;
            th += (th > 0.0) ? -PI_D : PI_D;
        }
        sum_d[k] += th;
        cnt[k]++;
    }
    cen[0] = cnt[0] ? sum_d[0] / (double)cnt[0] : 0.0;
    cen[1] = PI_D + (cnt[1] ? sum_d[1] / (double)cnt[1] : 0.0);

    /* Pass 2: classify, gate, one per cycle per class, accumulate the line fits. */
    memset(cl, 0, sizeof cl);
    for (i = 0; i < n_edges; i++) {
        const double t = (double)count_diff(edges[i], t_rec0_counts) * inv_tick;
        double th, d0, d1, d;
        int64_t ncyc;
        if (t < 0.0 || t >= t_stop) continue;
        th = TWO_PI * f * t + ph;
        d0 = wrap_pi(th - cen[0]);
        d1 = wrap_pi(th - cen[1]);
        k = (fabs(d0) <= fabs(d1)) ? 0 : 1;
        d = (k == 0) ? d0 : d1;
        if (fabs(d) > TWO_PI * PATHA_GATE_FRAC) {
            out->n_rejected++;
            continue;
        }
        ncyc = (int64_t)floor((th - cen[k] - d) * INV_TWO_PI + 0.5);
        offer(&cl[k], ncyc, fabs(d), edges[i], &out->n_rejected);
    }
    commit(&cl[0]);
    commit(&cl[1]);
    out->n_rise = cl[0].used;
    out->n_fall = cl[1].used;

    out->status = PATHA_E_FEW;
    if (cl[0].used < PATHA_MIN_EDGES || cl[1].used < PATHA_MIN_EDGES) return PATHA_E_FEW;
    for (k = 0; k < 2; k++) {
        const cls_t *c = &cl[k];
        slope[k] = (c->snt - c->sn * c->st / c->s1) / (c->snn - c->sn * c->sn / c->s1);  /* counts/cycle */
        if (!(slope[k] > 0.0)) return PATHA_E_FEW;
    }
    out->f_rise_hz = tick_hz / slope[0];
    out->f_fall_hz = tick_hz / slope[1];
    out->f_hz = 0.5 * (out->f_rise_hz + out->f_fall_hz);
    out->status = PATHA_OK;
    return PATHA_OK;
}
