/* fit.c - the frequency of one proton-precession record (see fit.h for the stages and the arithmetic).
 */
#include "fit.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define BLK             64u         /* samples per block: double anchors at the start, float inside */
#define NPAR            5           /* parameter order of the reference: A, f, phi, R, c */
#define P_A             0
#define P_F             1
#define P_PHI           2
#define P_R             3
#define P_C             4

#define TWO_PI          6.283185307179586476925
#define PI_D            3.141592653589793238463
#define FLT_EPS_D       1.1920928955078125e-07  /* FLT_EPSILON (<float.h> is not in the allowed set) */

/* est_fit's constants */
#define LM_MAX_ITER     40
#define LM_LAMBDA0      1e-3
#define LM_LAMBDA_MIN   1e-9
#define LM_DOWN         3.0
#define LM_UP           5.0
#define LM_R_FLOOR      1e-3        /* 1/s */
#define LM_DF_STOP_HZ   1e-7
#define SEED_T2_S       1.0
/* Rounding allowance of a float-evaluated cost, in float ulps of the cost and of A * |r| * |e sin|. */
#define COST_TOL_ULPS   32.0

/* est_fft's grid and the search on it */
#define ZOOM_PAD        4u          /* 4x zero padding: grid fs / (4 n) */
#define ZOOM_HALF       4           /* grid points evaluated on each side of the centre */
#define ZOOM_NF         (2 * ZOOM_HALF + 1)
#define ZOOM_MAX_PASSES 6
#define PEAK_GUARD_BINS 3u          /* stage 1: bins each side of the peak left out of the noise mean */

typedef struct {
    double c, s;                    /* cos, sin */
} rot_t;

typedef struct {
    double cost;                    /* sum r^2, r = x - model */
    double g[NPAR];                 /* J^T r */
    double h[NPAR][NPAR];           /* J^T J */
} lm_eval_t;

/* a <- a * b (complex product of unit phasors) */
static void rot_step(rot_t *a, const rot_t *b)
{
    const double c = a->c * b->c - a->s * b->s;
    a->s = a->s * b->c + a->c * b->s;
    a->c = c;
}

/* tc[k] = cos(k w), ts[k] = sin(k w), k = 0 .. BLK-1, by a double recurrence */
static void fill_rot_table(float *tc, float *ts, double w)
{
    const rot_t st = { cos(w), sin(w) };
    rot_t r = { 1.0, 0.0 };
    uint32_t k;
    for (k = 0; k < BLK; k++) {
        tc[k] = (float)r.c;
        ts[k] = (float)r.s;
        rot_step(&r, &st);
    }
}

/* te[k] = exp(-rk k), k = 0 .. BLK-1 */
static void fill_exp_table(float *te, double rk)
{
    const double ed = exp(-rk);
    double ek = 1.0;
    uint32_t k;
    for (k = 0; k < BLK; k++) {
        te[k] = (float)ek;
        ek *= ed;
    }
}

/* [-pi, pi) */
static double wrap_pi(double a)
{
    return a - TWO_PI * floor((a + PI_D) / TWO_PI);
}

/* Solve M y = b for symmetric positive definite M (nd x nd, row-major, nd <= NPAR) by Cholesky.
 * Returns 0, or -1 if M is not positive definite. */
static int chol_solve(const double *m, const double *b, double *y, int nd)
{
    double l[NPAR * NPAR];
    double z[NPAR];
    int i, j, k;

    for (i = 0; i < nd; i++) {
        for (j = 0; j <= i; j++) {
            double s = m[i * nd + j];
            for (k = 0; k < j; k++) s -= l[i * nd + k] * l[j * nd + k];
            if (i == j) {
                if (!(s > 0.0)) return -1;
                l[i * nd + i] = sqrt(s);
            } else {
                l[i * nd + j] = s / l[j * nd + j];
            }
        }
    }
    for (i = 0; i < nd; i++) {
        double s = b[i];
        for (k = 0; k < i; k++) s -= l[i * nd + k] * z[k];
        z[i] = s / l[i * nd + i];
    }
    for (i = nd - 1; i >= 0; i--) {
        double s = z[i];
        for (k = i + 1; k < nd; k++) s -= l[k * nd + i] * y[k];
        y[i] = s / l[i * nd + i];
    }
    return 0;
}

/* ---- stage 1: coarse seed from a 2,048-point real FFT ------------------------------------------ */

/* In-place radix-2 complex FFT (forward, e^-j), z interleaved re/im, m a power of two. */
static void fft_radix2(float *z, uint32_t m)
{
    uint32_t i, j, k, len;

    for (i = 1u, j = 0u; i < m; i++) {      /* bit-reversal permutation */
        uint32_t bit = m >> 1;
        for (; (j & bit) != 0u; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            const float tr = z[2u * i], ti = z[2u * i + 1u];
            z[2u * i] = z[2u * j];
            z[2u * i + 1u] = z[2u * j + 1u];
            z[2u * j] = tr;
            z[2u * j + 1u] = ti;
        }
    }
    for (len = 2u; len <= m; len <<= 1) {
        const uint32_t half = len >> 1;
        const float da = -6.28318530718f / (float)len;
        for (k = 0; k < half; k++) {
            const float wr = cosf(da * (float)k), wi = sinf(da * (float)k);
            for (i = k; i < m; i += len) {
                const uint32_t p = 2u * i, q = 2u * (i + half);
                const float tr = wr * z[q] - wi * z[q + 1u];
                const float ti = wr * z[q + 1u] + wi * z[q];
                z[q] = z[p] - tr;
                z[q + 1u] = z[p + 1u] - ti;
                z[p] += tr;
                z[p + 1u] += ti;
            }
        }
    }
}

/* z holds the m-point FFT Z of the packed real sequence (x[2i] + j x[2i+1]). Writes |X[k]|^2 of the
 * 2m-point real DFT into z[2k], k = 1 .. m-1, using X[k] = E + W^k O, X[m-k] = conj(E - W^k O). */
static void real_fft_power(float *z, uint32_t m)
{
    const float da = (float)(PI_D / (double)m);     /* 2 pi / (2m) */
    uint32_t k;

    for (k = 1u; k <= m / 2u; k++) {
        const uint32_t kk = m - k;
        const float zr = z[2u * k], zi = z[2u * k + 1u];
        const float yr = z[2u * kk], yi = z[2u * kk + 1u];
        const float er = 0.5f * (zr + yr), ei = 0.5f * (zi - yi);   /* E = (Z_k + conj Z_m-k) / 2 */
        const float orr = 0.5f * (zi + yi), oi = -0.5f * (zr - yr); /* O = (Z_k - conj Z_m-k) / 2j */
        const float cw = cosf(da * (float)k), sw = sinf(da * (float)k);
        const float tr = cw * orr + sw * oi, ti = cw * oi - sw * orr; /* T = e^(-j pi k / m) O */
        const float xr = er + tr, xi = ei + ti, yr2 = er - tr, yi2 = ei - ti;
        z[2u * k] = xr * xr + xi * xi;
        z[2u * kk] = yr2 * yr2 + yi2 * yi2;
    }
}

/* Parabola through three log magnitudes (est_fft's interpolation), from three powers. */
static double log_parabola(double pa, double pb, double pc)
{
    const double a = log(sqrt(pa) + 1e-30), b = log(sqrt(pb) + 1e-30), c = log(sqrt(pc) + 1e-30);
    const double den = a - 2.0 * b + c;
    return (den < 0.0) ? 0.5 * (a - c) / den : 0.0;
}

static int seed_coarse(const uint16_t *x, double fs, double f_lo, double f_hi, float *z, int32_t *x_ref,
                       double *f1)
{
    const uint32_t nn = FIT_SEED_FFT_N, m = FIT_SEED_FFT_N / 2u;
    const float wa = (float)(TWO_PI / (double)(nn - 1u));
    uint32_t i, k, ka, kb, kmax, cnt = 0u, sum = 0u;
    double lo, hi, best, noise = 0.0;

    for (i = 0; i < nn; i++) sum += x[i];
    *x_ref = (int32_t)((sum + nn / 2u) / nn);
    for (i = 0; i < nn; i++) {              /* numpy's hanning(2048) */
        const float w = 0.5f - 0.5f * cosf(wa * (float)i);
        z[i] = w * (float)((int32_t)x[i] - *x_ref);
    }
    fft_radix2(z, m);
    real_fft_power(z, m);

    lo = floor(f_lo * (double)nn / fs) + 1.0;   /* first bin above f_lo */
    hi = ceil(f_hi * (double)nn / fs) - 1.0;    /* last bin below f_hi */
    if (lo < 2.0) lo = 2.0;
    if (hi > (double)(m - 2u)) hi = (double)(m - 2u);
    if (hi - lo + 1.0 < (double)(2u * PEAK_GUARD_BINS + 2u)) return FIT_E_ARGS;
    ka = (uint32_t)lo;
    kb = (uint32_t)hi;

    kmax = ka;
    best = (double)z[2u * ka];
    for (k = ka + 1u; k <= kb; k++) {
        if ((double)z[2u * k] > best) {
            best = (double)z[2u * k];
            kmax = k;
        }
    }
    for (k = ka; k <= kb; k++) {
        if (k + PEAK_GUARD_BINS < kmax || k > kmax + PEAK_GUARD_BINS) {
            noise += (double)z[2u * k];
            cnt++;
        }
    }
    noise /= (double)cnt;
    if (!(best > 0.0) || !(best >= FIT_PEAK_MIN_RATIO * noise)) return FIT_E_NOPEAK;

    *f1 = ((double)kmax + log_parabola((double)z[2u * (kmax - 1u)], best, (double)z[2u * (kmax + 1u)]))
          * fs / (double)nn;
    return FIT_OK;
}

/* ---- stage 2: est_fft near the coarse peak ------------------------------------------------------ */

/* pw[i] = |sum_k w_k (x_k - x_ref) e^(j 2 pi (m0 + i) df k / fs)|^2, i = 0 .. ZOOM_NF-1, with numpy's
 * hanning(n) w_k = 0.5 - 0.5 cos(2 pi k / (n - 1)): est_fft's 4x zero-padded spectrum at m0 .. m0+8. */
static void zoom_pass(const uint16_t *x, uint32_t n, double fs, int32_t x_ref, double df, int32_t m0,
                      float *tab, double *pw)
{
    float *const twc = tab, *const tws = tab + BLK;                 /* window angle */
    float *const tbc = tab + 2u * BLK, *const tbs = tab + 3u * BLK; /* first grid frequency */
    float *const tdc = tab + 4u * BLK, *const tds = tab + 5u * BLK; /* grid step */
    const double aw = TWO_PI / (double)(n - 1u);
    const double ab = TWO_PI * (double)m0 * df / fs;
    const double ad = TWO_PI * df / fs;
    const rot_t sw = { cos(aw * (double)BLK), sin(aw * (double)BLK) };
    const rot_t sb = { cos(ab * (double)BLK), sin(ab * (double)BLK) };
    const rot_t sd = { cos(ad * (double)BLK), sin(ad * (double)BLK) };
    rot_t rw = { 1.0, 0.0 }, rb = { 1.0, 0.0 }, rd = { 1.0, 0.0 };
    double re[ZOOM_NF], im[ZOOM_NF];
    uint32_t b0, k;
    int i;

    fill_rot_table(twc, tws, aw);
    fill_rot_table(tbc, tbs, ab);
    fill_rot_table(tdc, tds, ad);
    for (i = 0; i < ZOOM_NF; i++) re[i] = im[i] = 0.0;

    for (b0 = 0; b0 < n; b0 += BLK) {
        const uint32_t len = (n - b0 < BLK) ? n - b0 : BLK;
        const uint16_t *const xb = x + b0;
        const float w_c = (float)rw.c, w_s = (float)rw.s;
        const float b_c = (float)rb.c, b_s = (float)rb.s;
        const float d_c = (float)rd.c, d_s = (float)rd.s;
        float fr[ZOOM_NF], fi[ZOOM_NF];

        for (i = 0; i < ZOOM_NF; i++) fr[i] = fi[i] = 0.0f;
        for (k = 0; k < len; k++) {
            const float cw = w_c * twc[k] - w_s * tws[k];
            const float v = (0.5f - 0.5f * cw) * (float)((int32_t)xb[k] - x_ref);
            const float vr = d_c * tdc[k] - d_s * tds[k];
            const float vi = d_s * tdc[k] + d_c * tds[k];
            float ur = b_c * tbc[k] - b_s * tbs[k];
            float ui = b_s * tbc[k] + b_c * tbs[k];
            for (i = 0; i < ZOOM_NF; i++) {
                const float nr = ur * vr - ui * vi;
                fr[i] += v * ur;
                fi[i] += v * ui;
                ui = ur * vi + ui * vr;
                ur = nr;
            }
        }
        for (i = 0; i < ZOOM_NF; i++) {
            re[i] += (double)fr[i];
            im[i] += (double)fi[i];
        }
        rot_step(&rw, &sw);
        rot_step(&rb, &sb);
        rot_step(&rd, &sd);
    }
    for (i = 0; i < ZOOM_NF; i++) pw[i] = re[i] * re[i] + im[i] * im[i];
}

/* est_fft: in-band maximum of the grid m * fs / (4 n) near f1, then the log parabola. */
static double seed_zoom(const uint16_t *x, uint32_t n, double fs, double f_lo, double f_hi, int32_t x_ref,
                        double f1, float *tab, void (*progress)(void))
{
    const double df = fs / ((double)ZOOM_PAD * (double)n);
    const int32_t m_lo = (int32_t)floor(f_lo / df) + 1;     /* est_fft's band: f > f_lo and f < f_hi */
    const int32_t m_hi = (int32_t)ceil(f_hi / df) - 1;
    int32_t mc = (int32_t)floor(f1 / df + 0.5), m0 = 0;
    double pw[ZOOM_NF];
    int pass, i, ib = 0;

    if (mc < m_lo) mc = m_lo;
    if (mc > m_hi) mc = m_hi;
    for (pass = 0; pass < ZOOM_MAX_PASSES; pass++) {
        double best = -1.0;
        m0 = mc - ZOOM_HALF;
        zoom_pass(x, n, fs, x_ref, df, m0, tab, pw);
        if (progress) progress();
        for (i = 0; i < ZOOM_NF; i++) {
            const int32_t m = m0 + i;
            if (m >= m_lo && m <= m_hi && pw[i] > best) {
                best = pw[i];
                ib = i;
            }
        }
        if (ib > 0 && ib < ZOOM_NF - 1) break; /* interior: both neighbours known */
        mc = m0 + ib;                           /* on the window's edge: centre the next pass there */
    }
    if (ib > 0 && ib < ZOOM_NF - 1)
        return ((double)(m0 + ib) + log_parabola(pw[ib - 1], pw[ib], pw[ib + 1])) * df;
    return (double)(m0 + ib) * df;
}

/* ---- stage 3: est_fit's linear seed ------------------------------------------------------------- */

/* Least squares of x on [e sin(2 pi f0 t), e cos(2 pi f0 t), 1], e = exp(-t / SEED_T2_S). */
static int seed_linear(const uint16_t *x, uint32_t n, double fs, int32_t x_ref, double f0, float *tab,
                       double *p)
{
    float *const tc = tab, *const ts = tab + BLK, *const te = tab + 2u * BLK;
    const double w = TWO_PI * f0 / fs;
    const double r0 = 1.0 / SEED_T2_S;
    const rot_t bstep = { cos(w * (double)BLK), sin(w * (double)BLK) };
    const double bdecay = exp(-r0 * (double)BLK / fs);
    rot_t ph = { 1.0, 0.0 };
    double env = 1.0;
    double sss = 0.0, ssc = 0.0, scc = 0.0, ss = 0.0, sc = 0.0, sxs = 0.0, sxc = 0.0, sx = 0.0;
    double mtx[9], rhs[3], sol[3];
    uint32_t b0, k;

    fill_rot_table(tc, ts, w);
    fill_exp_table(te, r0 / fs);
    for (b0 = 0; b0 < n; b0 += BLK) {
        const uint32_t len = (n - b0 < BLK) ? n - b0 : BLK;
        const uint16_t *const xb = x + b0;
        const float s0 = (float)ph.s, c0 = (float)ph.c, e0 = (float)env;
        float fss = 0.0f, fsc = 0.0f, fcc = 0.0f, fs1 = 0.0f, fc1 = 0.0f, fxs = 0.0f, fxc = 0.0f, fx1 = 0.0f;
        for (k = 0; k < len; k++) {
            const float e = e0 * te[k];
            const float es = e * (s0 * tc[k] + c0 * ts[k]);
            const float ec = e * (c0 * tc[k] - s0 * ts[k]);
            const float xv = (float)((int32_t)xb[k] - x_ref);
            fss += es * es;
            fsc += es * ec;
            fcc += ec * ec;
            fs1 += es;
            fc1 += ec;
            fxs += xv * es;
            fxc += xv * ec;
            fx1 += xv;
        }
        sss += (double)fss;
        ssc += (double)fsc;
        scc += (double)fcc;
        ss += (double)fs1;
        sc += (double)fc1;
        sxs += (double)fxs;
        sxc += (double)fxc;
        sx += (double)fx1;
        rot_step(&ph, &bstep);
        env *= bdecay;
    }
    mtx[0] = sss; mtx[1] = ssc; mtx[2] = ss;
    mtx[3] = ssc; mtx[4] = scc; mtx[5] = sc;
    mtx[6] = ss;  mtx[7] = sc;  mtx[8] = (double)n;
    rhs[0] = sxs; rhs[1] = sxc; rhs[2] = sx;
    if (chol_solve(mtx, rhs, sol, 3) != 0) return FIT_E_DIVERGED;
    p[P_A] = hypot(sol[0], sol[1]);
    p[P_F] = f0;
    p[P_PHI] = atan2(sol[1], sol[0]);
    p[P_R] = r0;
    p[P_C] = sol[2] + (double)x_ref;
    return FIT_OK;
}

/* ---- stage 4: Levenberg-Marquardt ---------------------------------------------------------------- */

/* One pass over the record at parameters p: the cost, J^T r and J^T J. With u = t A e cos and
 * v = t A e sin, the Jacobian columns are [e sin, 2 pi u, A e cos, -v, 1]. */
static void lm_eval(const uint16_t *x, uint32_t n, double fs, const double *p, float *tab, lm_eval_t *ev)
{
    float *const tc = tab, *const ts = tab + BLK, *const te = tab + 2u * BLK, *const tt = tab + 3u * BLK;
    const double w = TWO_PI * p[P_F] / fs;
    const double rk = p[P_R] / fs;
    const rot_t bstep = { cos(w * (double)BLK), sin(w * (double)BLK) };
    const double bdecay = exp(-rk * (double)BLK);
    const double c_round = floor(p[P_C] + 0.5);
    const int32_t c_int = (int32_t)c_round;
    const float c_frac = (float)(p[P_C] - c_round);
    rot_t ph = { cos(p[P_PHI]), sin(p[P_PHI]) };
    double env = 1.0;
    double q2 = 0.0, ga2 = 0.0, gu2 = 0.0, gp2 = 0.0, gv2 = 0.0, g12 = 0.0;
    double haa2 = 0.0, hau2 = 0.0, hap2 = 0.0, hav2 = 0.0, ha12 = 0.0;
    double huu2 = 0.0, hup2 = 0.0, huv2 = 0.0, hu12 = 0.0;
    double hpp2 = 0.0, hpv2 = 0.0, hp12 = 0.0, hvv2 = 0.0, hv12 = 0.0;
    uint32_t b0, k;
    int i, j;

    fill_rot_table(tc, ts, w);
    fill_exp_table(te, rk);
    for (k = 0; k < BLK; k++) tt[k] = (float)((double)k / fs);

    for (b0 = 0; b0 < n; b0 += BLK) {
        const uint32_t len = (n - b0 < BLK) ? n - b0 : BLK;
        const uint16_t *const xb = x + b0;
        const float s0 = (float)ph.s, c0 = (float)ph.c;
        const float e0 = (float)env, ae0 = (float)(p[P_A] * env);
        const float tb = (float)((double)b0 / fs);
        float q = 0.0f, ga = 0.0f, gu = 0.0f, gp = 0.0f, gv = 0.0f, g1 = 0.0f;
        float haa = 0.0f, hau = 0.0f, hap = 0.0f, hav = 0.0f, ha1 = 0.0f;
        float huu = 0.0f, hup = 0.0f, huv = 0.0f, hu1 = 0.0f;
        float hpp = 0.0f, hpv = 0.0f, hp1 = 0.0f, hvv = 0.0f, hv1 = 0.0f;
        for (k = 0; k < len; k++) {
            const float s = s0 * tc[k] + c0 * ts[k];                /* sin(2 pi f t + phi) */
            const float c = c0 * tc[k] - s0 * ts[k];                /* cos(2 pi f t + phi) */
            const float ja = (e0 * te[k]) * s;                      /* dm/dA = e sin */
            const float ae = ae0 * te[k];                           /* A e */
            const float jp = ae * c;                                /* dm/dphi = A e cos */
            const float m = ae * s;                                 /* model without c */
            const float t = tb + tt[k];
            const float u = t * jp;                                 /* dm/df / (2 pi) */
            const float v = t * m;                                  /* -dm/dR */
            const float r = ((float)((int32_t)xb[k] - c_int) - m) - c_frac;
            q += r * r;
            ga += ja * r;
            gu += u * r;
            gp += jp * r;
            gv += v * r;
            g1 += r;
            haa += ja * ja;
            hau += ja * u;
            hap += ja * jp;
            hav += ja * v;
            ha1 += ja;
            huu += u * u;
            hup += u * jp;
            huv += u * v;
            hu1 += u;
            hpp += jp * jp;
            hpv += jp * v;
            hp1 += jp;
            hvv += v * v;
            hv1 += v;
        }
        q2 += (double)q;
        ga2 += (double)ga;
        gu2 += (double)gu;
        gp2 += (double)gp;
        gv2 += (double)gv;
        g12 += (double)g1;
        haa2 += (double)haa;
        hau2 += (double)hau;
        hap2 += (double)hap;
        hav2 += (double)hav;
        ha12 += (double)ha1;
        huu2 += (double)huu;
        hup2 += (double)hup;
        huv2 += (double)huv;
        hu12 += (double)hu1;
        hpp2 += (double)hpp;
        hpv2 += (double)hpv;
        hp12 += (double)hp1;
        hvv2 += (double)hvv;
        hv12 += (double)hv1;
        rot_step(&ph, &bstep);
        env *= bdecay;
    }

    ev->cost = q2;
    ev->g[P_A] = ga2;
    ev->g[P_F] = TWO_PI * gu2;
    ev->g[P_PHI] = gp2;
    ev->g[P_R] = -gv2;
    ev->g[P_C] = g12;
    ev->h[P_A][P_A] = haa2;
    ev->h[P_A][P_F] = TWO_PI * hau2;
    ev->h[P_A][P_PHI] = hap2;
    ev->h[P_A][P_R] = -hav2;
    ev->h[P_A][P_C] = ha12;
    ev->h[P_F][P_F] = TWO_PI * TWO_PI * huu2;
    ev->h[P_F][P_PHI] = TWO_PI * hup2;
    ev->h[P_F][P_R] = -TWO_PI * huv2;
    ev->h[P_F][P_C] = TWO_PI * hu12;
    ev->h[P_PHI][P_PHI] = hpp2;
    ev->h[P_PHI][P_R] = -hpv2;
    ev->h[P_PHI][P_C] = hp12;
    ev->h[P_R][P_R] = hvv2;
    ev->h[P_R][P_C] = -hv12;
    ev->h[P_C][P_C] = (double)n;
    for (i = 1; i < NPAR; i++)
        for (j = 0; j < i; j++) ev->h[i][j] = ev->h[j][i];
}

static int eval_finite(const lm_eval_t *ev)
{
    int i, j;
    if (!isfinite(ev->cost)) return 0;
    for (i = 0; i < NPAR; i++) {
        if (!isfinite(ev->g[i])) return 0;
        for (j = 0; j < NPAR; j++)
            if (!isfinite(ev->h[i][j])) return 0;
    }
    return 1;
}

/* Parameters lm_eval can take: finite, and c within int32 range for its integer split. */
static int params_ok(const double *p)
{
    int i;
    for (i = 0; i < NPAR; i++)
        if (!isfinite(p[i])) return 0;
    return fabs(p[P_C]) < 1e9;
}

/* est_fit's loop. p: seed in, result out; cur: the cost, gradient and J^T J at the result. */
static int lm_fit(const uint16_t *x, uint32_t n, double fs, double *p, float *tab, void (*progress)(void),
                  lm_eval_t *cur, int *iters)
{
    lm_eval_t trial;
    double lam = LM_LAMBDA0;
    int it, converged = 0;

    *iters = 0;
    if (!params_ok(p)) return FIT_E_DIVERGED;
    lm_eval(x, n, fs, p, tab, cur);
    if (!eval_finite(cur)) return FIT_E_DIVERGED;

    for (it = 0; it < LM_MAX_ITER; it++) {
        double mtx[NPAR * NPAR], d[NPAR], pn[NPAR];
        int i, j, ok, accept = 0, ambiguous = 0;

        if (progress) progress();
        for (i = 0; i < NPAR; i++) {
            for (j = 0; j < NPAR; j++) mtx[i * NPAR + j] = cur->h[i][j];
            mtx[i * NPAR + i] += lam * cur->h[i][i];
        }
        ok = (chol_solve(mtx, cur->g, d, NPAR) == 0);
        if (ok) {
            for (i = 0; i < NPAR; i++) pn[i] = p[i] + d[i];
            if (pn[P_R] < LM_R_FLOOR) pn[P_R] = LM_R_FLOOR;
            ok = params_ok(pn);
        }
        if (ok) {
            lm_eval(x, n, fs, pn, tab, &trial);
            ok = eval_finite(&trial);
        }
        if (ok) {
            /* Python accepts when the new cost is lower. The float pass resolves a cost change only
             * down to its rounding (tol); below that, the change of the quadratic through both
             * gradients, -(g + g_new) . (pn - p), decides. */
            const double d_dir = trial.cost - cur->cost;
            const double tol = COST_TOL_ULPS * FLT_EPS_D
                               * (cur->cost + fabs(p[P_A]) * sqrt(cur->cost * cur->h[P_A][P_A]));
            if (d_dir < -tol) {
                accept = 1;
            } else if (d_dir <= tol) {
                double d_quad = 0.0;
                for (i = 0; i < NPAR; i++) d_quad -= (cur->g[i] + trial.g[i]) * (pn[i] - p[i]);
                accept = (d_quad < 0.0);
                ambiguous = 1;
            }
        }
        if (accept) {
            for (i = 0; i < NPAR; i++) p[i] = pn[i];
            *cur = trial;
            lam = fmax(lam / LM_DOWN, LM_LAMBDA_MIN);
            if (fabs(d[P_F]) < LM_DF_STOP_HZ) {
                converged = 1;
                it++;
                break;
            }
        } else {
            /* a step already below the stop size that loses only within rounding: converged */
            if (ambiguous && fabs(d[P_F]) < LM_DF_STOP_HZ) {
                converged = 1;
                it++;
                break;
            }
            lam *= LM_UP;
        }
    }
    *iters = it;
    return converged ? FIT_OK : FIT_E_DIVERGED;
}

/* ---- the whole fit -------------------------------------------------------------------------------- */

int fit_record(const uint16_t *x, uint32_t n, double fs_hz, double f_lo_hz, double f_hi_hz,
               float *work, uint32_t work_len, void (*progress)(void), fit_result_t *out)
{
    double p[NPAR], f1 = 0.0, f0, amp, phase;
    lm_eval_t ev;
    int32_t x_ref = 0;
    int st, iters = 0;

    if (out == NULL) return FIT_E_ARGS;
    memset(out, 0, sizeof *out);
    memset(&ev, 0, sizeof ev);
    out->status = FIT_E_ARGS;
    if (x == NULL || work == NULL || work_len < FIT_WORK_FLOATS || n < FIT_SEED_FFT_N) return FIT_E_ARGS;
    if (!isfinite(fs_hz) || !(fs_hz > 0.0) || !(f_lo_hz > 0.0) || !(f_hi_hz > f_lo_hz)
        || !(f_hi_hz < 0.5 * fs_hz))
        return FIT_E_ARGS;

    st = seed_coarse(x, fs_hz, f_lo_hz, f_hi_hz, work, &x_ref, &f1);
    if (progress) progress();
    if (st != FIT_OK) {
        out->status = st;
        return st;
    }
    f0 = seed_zoom(x, n, fs_hz, f_lo_hz, f_hi_hz, x_ref, f1, work, progress);
    out->f_seed_hz = f0;
    out->f_hz = f0;
    st = seed_linear(x, n, fs_hz, x_ref, f0, work, p);
    if (progress) progress();
    if (st != FIT_OK) {
        out->status = st;
        return st;
    }

    st = lm_fit(x, n, fs_hz, p, work, progress, &ev, &iters);
    out->iterations = iters;

    amp = p[P_A];
    phase = p[P_PHI];
    if (amp < 0.0) {
        amp = -amp;
        phase += PI_D;
    }
    out->f_hz = p[P_F];
    out->amp = amp;
    out->phase_rad = wrap_pi(phase);
    out->decay_per_s = p[P_R];
    out->offset = p[P_C];
    if (n > 0u && isfinite(ev.cost)) out->resid_rms = sqrt(ev.cost / (double)n);
    if (st == FIT_OK) {
        double e_f[NPAR] = { 0.0, 1.0, 0.0, 0.0, 0.0 }, col[NPAR];
        if (chol_solve(&ev.h[0][0], e_f, col, NPAR) != 0 || !(col[P_F] > 0.0)) {
            st = FIT_E_DIVERGED;
        } else {
            out->f_sigma_hz = sqrt(col[P_F] * ev.cost / (double)(n - (uint32_t)NPAR));
            if (p[P_R] <= LM_R_FLOOR * (1.0 + 1e-9) || p[P_R] > FIT_DECAY_MAX_PER_S) st = FIT_E_DECAY;
        }
    }
    out->status = st;
    return st;
}
