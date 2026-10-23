/* fake_hw.c - the simulated board.
 */
#include "fake_hw.h"

#include <math.h>
#include <string.h>

#include "config.h"
#include "shot.h"

fake_t F;

#define MV_PER_CODE (3250.0f / 4095.0f)

static bool after(uint32_t t, uint32_t ref) { return (int32_t)(t - ref) >= 0; }
static uint32_t ms_ticks(float ms) { return (uint32_t)(ms * 170000.0f); }

void fake_reset(const scenario_t *sc, uint32_t t_start)
{
    memset(&F, 0, sizeof F);
    F.sc = sc;
    F.now = t_start;
    F.t_start = t_start;
    F.out[OUT_BLANK2] = true;     /* idle: BLANK2 high */
    F.noise = 12345u;
    F.abort_pending = sc->abort_at_ms > 0.0f;
    F.oc_pending = sc->overcurrent_at_ms > 0.0f;
    F.drop_pending = sc->m.drop_at_ms > 0.0f;
    F.clock_pending = sc->clock_fail_at_ms > 0.0f;
}

bool fake_advance(void)
{
    const scenario_t *sc = F.sc;
    bool have = false, stalled = false;
    uint32_t next = 0, t_oc = 0, t_drop = 0, t_clk = 0;
    const uint32_t t_abort = F.t_start + ms_ticks(sc->abort_at_ms);

    if (F.gate_rise_seen && sc->stall_at_ms > 0.0f && F.guard_on
        && after(F.wake, F.t_gate_rise + ms_ticks(sc->stall_at_ms)))
        stalled = true;   /* the sequencer's interrupt is no longer served */
    if (F.gate_rise_seen) t_oc = F.t_gate_rise + ms_ticks(sc->overcurrent_at_ms);
    if (F.gate_rise_seen) t_clk = F.t_gate_rise + ms_ticks(sc->clock_fail_at_ms);
    if (F.hist.gate_fall_seen) t_drop = F.hist.t_gate_fall + ms_ticks(sc->m.drop_at_ms);

#define CONSIDER(cond, t) do { if (cond) { if (!have || (int32_t)((t) - next) < 0) next = (t); have = true; } } while (0)
    CONSIDER(F.wake_set && !stalled, F.wake);
    CONSIDER(F.guard_on, F.guard_at);
    CONSIDER(F.oc_pending && F.ion && F.gate_rise_seen, t_oc);
    CONSIDER(F.abort_pending, t_abort);
    CONSIDER(F.drop_pending && F.watch && F.hist.gate_fall_seen, t_drop);
    CONSIDER(F.clock_pending && F.gate_rise_seen, t_clk);
#undef CONSIDER
    if (!have) return false;
    if (after(next, F.now)) F.now = next;

    if (F.clock_pending && F.gate_rise_seen && after(F.now, t_clk)) {
        F.clock_pending = false;                /* the NMI, as bsp/hw.c does it: GATE and TUNE only */
        hw_set(OUT_GATE, false);
        hw_set(OUT_TUNE, false);
        shot_fault(FAULT_CLOCK);
    }
    if (F.guard_on && after(F.now, F.guard_at)) {
        F.guard_on = false;
        hw_set(OUT_GATE, false);
        shot_fault(FAULT_GUARD);
    }
    if (F.oc_pending && F.ion && F.gate_rise_seen && after(F.now, t_oc)) {
        F.oc_pending = false;
        hw_set(OUT_GATE, false);
        F.ion = false;
        shot_fault(FAULT_OVERCURRENT);
    }
    if (F.drop_pending && F.watch && F.hist.gate_fall_seen && after(F.now, t_drop)) {
        F.drop_pending = false;
        F.watch = false;
        shot_fault(FAULT_XFER_DROP);
    }
    if (F.abort_pending && after(F.now, t_abort)) {
        F.abort_pending = false;
        shot_abort();
    }
    if (F.wake_set && !stalled && after(F.now, F.wake)) {
        F.wake_set = false;
        shot_tick();
    }
    return true;
}

static uint16_t code_of(float mv)
{
    F.noise = F.noise * 1103515245u + 12345u;
    int jitter = (int)((F.noise >> 16) % 3u) - 1;               /* +/-1 code of noise */
    float c = mv / MV_PER_CODE + (float)jitter;
    if (c < 0.0f) c = 0.0f;
    if (c > 4095.0f) c = 4095.0f;
    return (uint16_t)(c + 0.5f);
}

void hw_set(hw_out_t o, bool high)
{
    if (F.out[o] != high && F.n_log < MAX_TRANSITIONS) {
        F.log[F.n_log].t = F.now;
        F.log[F.n_log].out = o;
        F.log[F.n_log].high = high;
        F.n_log++;
    }
    if (o == OUT_RLY && high && !F.out[o]) {            /* a new shot: forget the last one, as emu.c does */
        memset(&F.hist, 0, sizeof F.hist);
        F.hist.rly_rise_seen = true;
        F.hist.t_rly_rise = F.now;
    }
    if (o == OUT_RLY && !high && F.out[o]) { F.hist.rly_fall_seen = true; F.hist.t_rly_fall = F.now; }
    if (o == OUT_GATE && high && !F.out[o]) { F.gate_rise_seen = true; F.t_gate_rise = F.now; }
    if (o == OUT_GATE && !high && F.out[o]) { F.hist.gate_fall_seen = true; F.hist.t_gate_fall = F.now; }
    F.out[o] = high;
}

bool hw_get(hw_out_t o) { return F.out[o]; }
uint32_t hw_now(void) { return F.now; }

void hw_wake_at(uint32_t t)
{
    F.wake = t;
    F.wake_set = true;
}

uint32_t hw_xfer_start(uint16_t *buf, uint32_t n)
{
    F.xb = buf;
    F.xn = n;
    F.xt0 = F.now + TICKS_PER_SAMPLE;
    F.xfill = 0;
    F.xon = true;
    return F.xt0;
}

/* Samples are made when they are read, from the control-line history up to their own time. */
static void xfer_fill(void)
{
    if (!F.xon) return;
    uint32_t avail = 0;
    if (after(F.now, F.xt0)) avail = (F.now - F.xt0) / TICKS_PER_SAMPLE + 1u;
    if (avail > F.xn) avail = F.xn;
    for (; F.xfill < avail; F.xfill++)
        F.xb[F.xfill] = code_of(xfer_model_mv(&F.sc->m, &F.hist, F.xt0 + F.xfill * TICKS_PER_SAMPLE));
}

uint32_t hw_xfer_count(void) { xfer_fill(); return F.xfill; }
void hw_xfer_stop(void) { xfer_fill(); F.xon = false; }
void hw_xfer_watch(bool on) { F.watch = on; }

void hw_isense_start(uint16_t awd_code) { F.ion = true; F.awd = awd_code; }
uint16_t hw_isense_mean(void) { return F.ion ? code_of(F.sc->i_sense_mv) : 0; }
void hw_isense_stop(void) { F.ion = false; }

void hw_guard_arm(uint32_t ticks) { F.guard_on = true; F.guard_at = F.now + ticks; }
void hw_guard_disarm(void) { F.guard_on = false; }

static double gauss(void)
{
    double u1, u2;
    F.noise = F.noise * 1103515245u + 12345u;
    u1 = ((F.noise >> 8) + 1u) / 16777217.0;
    F.noise = F.noise * 1103515245u + 12345u;
    u2 = (F.noise >> 8) / 16777216.0;
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

/* The record and its comparator edges, made at once (only their counts follow the clock). */
uint32_t hw_record_start(uint16_t *buf, uint32_t n, uint32_t *edges, uint32_t n_edges)
{
    const scenario_t *sc = F.sc;
    double prev = 0.0;
    int state = 0;      /* comparator output: 0 low, 1 high */

    F.ron = true;
    F.rt0 = F.now + TICKS_PER_SAMPLE;
    F.rn = n;
    F.rcount = 0;
    F.n_edges = 0;
    for (uint32_t k = 0; k < n; k++) {
        double t = (double)k / SAMPLE_HZ;
        double v = sc->fid_amp > 0.0
                   ? sc->fid_amp * exp(-t / sc->fid_t2_s) * sin(6.283185307179586 * sc->fid_f_hz * t + sc->fid_phase)
                   : 0.0;
        v += sc->noise_codes * gauss();
        double c = floor(2048.0 + v + 0.5);
        buf[k] = (uint16_t)(c < 0.0 ? 0.0 : (c > 4095.0 ? 4095.0 : c));
        /* comparator with hysteresis +/- h/2 around mid-scale, crossing time interpolated */
        if (sc->hyst_codes > 0.0 && k > 0 && F.n_edges < n_edges) {
            double th = state ? -sc->hyst_codes / 2.0 : sc->hyst_codes / 2.0;
            if ((!state && v >= th) || (state && v <= th)) {
                double frac = (th - prev) / (v - prev);
                double counts = ((double)(k - 1) + frac) * TICKS_PER_SAMPLE;
                edges[F.n_edges++] = F.rt0 + (uint32_t)llround(counts);
                state = !state;
            }
        }
        prev = v;
    }
    return F.rt0;
}

uint32_t hw_record_count(void)
{
    if (F.ron && after(F.now, F.rt0)) {
        uint32_t c = (F.now - F.rt0) / TICKS_PER_SAMPLE + 1u;
        F.rcount = c > F.rn ? F.rn : c;
    }
    return F.rcount;
}

uint32_t hw_edges_count(void) { return F.n_edges; }
bool hw_edges_overflow(void) { return false; }
void hw_record_stop(void) { (void)hw_record_count(); F.ron = false; }
