/* xfer.c - the XFER readback checks.
 */
#include "xfer.h"

#include "config.h"

#define MEDIAN_MAX 64u

/* Sample index range [k0, k1) for the times [from_us, to_us). */
static void index_range(const xfer_trace_t *tr, int32_t from_us, int32_t to_us, uint32_t *k0, uint32_t *k1)
{
    int32_t a = from_us - tr->t0_us;
    int32_t b = to_us - tr->t0_us;
    int32_t ka = (a <= 0) ? 0 : (a + (int32_t)US_PER_SAMPLE - 1) / (int32_t)US_PER_SAMPLE;
    int32_t kb = (b <= 0) ? 0 : (b + (int32_t)US_PER_SAMPLE - 1) / (int32_t)US_PER_SAMPLE;
    if (ka > (int32_t)tr->n) ka = (int32_t)tr->n;
    if (kb > (int32_t)tr->n) kb = (int32_t)tr->n;
    *k0 = (uint32_t)ka;
    *k1 = (uint32_t)(kb > ka ? kb : ka);
}

static int32_t time_of(const xfer_trace_t *tr, uint32_t k)
{
    return tr->t0_us + (int32_t)(k * US_PER_SAMPLE);
}

bool xfer_mean_mv(const xfer_trace_t *tr, int32_t from_us, int32_t to_us, float *mv)
{
    uint32_t k0, k1, sum = 0;
    index_range(tr, from_us, to_us, &k0, &k1);
    if (k1 <= k0) return false;
    for (uint32_t k = k0; k < k1; k++) sum += tr->code[k];
    *mv = (float)sum / (float)(k1 - k0) * tr->mv_per_code;
    return true;
}

static void sort_u16(uint16_t *v, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) {
        uint16_t x = v[i];
        uint32_t j = i;
        while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; }
        v[j] = x;
    }
}

static float median_of(uint16_t *v, uint32_t n)
{
    sort_u16(v, n);
    if (n & 1u) return (float)v[n / 2];
    return 0.5f * ((float)v[n / 2 - 1] + (float)v[n / 2]);
}

/* Median of the samples in [k0, k1) whose code is at least min_code. False if none. */
static bool median_above(const xfer_trace_t *tr, uint32_t k0, uint32_t k1, uint16_t min_code, float *code)
{
    uint16_t v[MEDIAN_MAX];
    uint32_t m = 0;
    for (uint32_t k = k0; k < k1 && m < MEDIAN_MAX; k++)
        if (tr->code[k] >= min_code) v[m++] = tr->code[k];
    if (m == 0) return false;
    *code = median_of(v, m);
    return true;
}

bool xfer_median_mv(const xfer_trace_t *tr, int32_t from_us, int32_t to_us, float *mv)
{
    uint32_t k0, k1;
    float c;
    index_range(tr, from_us, to_us, &k0, &k1);
    if (!median_above(tr, k0, k1, 0, &c)) return false;
    *mv = c * tr->mv_per_code;
    return true;
}

bool xfer_check_c(const xfer_trace_t *tr, float mv[3])
{
    static const int32_t at[3] = { CHECK_C1_US, CHECK_C2_US, CHECK_C3_US };
    bool all_high = true;
    for (int i = 0; i < 3; i++) {
        if (!xfer_median_mv(tr, at[i] - CHECK_C_HALF_US, at[i] + CHECK_C_HALF_US + 1, &mv[i])) {
            mv[i] = 0.0f;
            all_high = false;
        } else if (mv[i] < (float)XFER_HIGH_MV) {
            all_high = false;
        }
    }
    return all_high;
}

void xfer_check_d(const xfer_trace_t *tr, xfer_d_t *out)
{
    const uint16_t vih = (uint16_t)((float)XFER_HIGH_MV / tr->mv_per_code + 0.5f);
    const uint32_t win = 1000u / US_PER_SAMPLE;      /* 1 ms of samples for the half-step search */
    uint32_t k, k0, k1;
    float c;

    out->rose = false;
    out->t_rise_us = -1;
    out->plateau_mv = out->held_mv = out->step_mv = 0.0f;
    out->pickup_us = -1;
    out->pickup_flag = true;
    out->ok = false;

    for (k = 0; k < tr->n; k++)
        if (tr->code[k] > vih) break;
    if (k == tr->n) return;
    out->rose = true;
    out->t_rise_us = time_of(tr, k);

    index_range(tr, out->t_rise_us + D_PLATEAU_FROM_US, out->t_rise_us + D_PLATEAU_TO_US + 1, &k0, &k1);
    if (!median_above(tr, k0, k1, (uint16_t)(vih + 1u), &c)) return;
    out->plateau_mv = c * tr->mv_per_code;
    if (!xfer_mean_mv(tr, D_HELD_FROM_US, D_HELD_TO_US + 1, &out->held_mv)) return;
    out->step_mv = out->plateau_mv - out->held_mv;
    out->ok = out->step_mv >= (float)D_MIN_STEP_MV;

    /* Half-step point: the first 1 ms window (stepped by 0.25 ms) whose median above VIH has fallen
     * below plateau - step/2. Log only. */
    if (out->step_mv > 5.0f) {
        const float half = (out->plateau_mv - 0.5f * out->step_mv) / tr->mv_per_code;
        index_range(tr, out->t_rise_us + D_PLATEAU_FROM_US, D_HELD_TO_US, &k0, &k1);
        for (k = k0; k + win <= k1; k += win / 4u) {
            if (median_above(tr, k, k + win, (uint16_t)(vih + 1u), &c) && c <= half) {
                out->pickup_us = time_of(tr, k + win / 2u) - out->t_rise_us;
                break;
            }
        }
    }
    out->pickup_flag = out->pickup_us < D_PICKUP_MIN_US || out->pickup_us > D_PICKUP_MAX_US;
}
