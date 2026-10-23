/* shot.c - the shot sequencer.
 *
 * One state per timed step of the timing table (config.h). shot_tick() runs every step that is
 * due, then asks the board to wake it for the next one. Interrupts above it (analog watchdog,
 * gate guard, XFER watch, clock failure) only make the line safe and call shot_fault(); the fault
 * is handled at the top of the next pass, which the board triggers at once.
 *
 * Safety rules kept here:
 *  - GATE_CMD goes high in exactly one place, after checks (a), (a2) and (b);
 *  - RLY_CMD falls only RLY_OFF_US after GATE_CMD fell, on every path (K1 must open cold). The board's
 *    NMI and HardFault handlers keep the same order; only a watchdog reset drops both at once, through
 *    the pull-downs, and K1 still takes milliseconds to open;
 *  - TUNE is forced low before every GATE_CMD edge.
 */
#include "shot.h"

#include <string.h>

#include "config.h"
#include "hw.h"

typedef enum {
    ST_IDLE, ST_RLY_ON, ST_CHECK_A2, ST_CHECK_B, ST_GATE_ON, ST_ILOG_MID, ST_ILOG_END,
    ST_GATE_OFF, ST_RLY_OFF, ST_TUNE_ON, ST_CHECK_CD, ST_BLANK2_OFF, ST_REC_START, ST_REC_END,
    ST_ABORT_RLY_OFF, ST_DONE
} state_t;

typedef enum { XF_NONE, XF_PRE, XF_POST } xfer_capture_t;

static struct {
    volatile state_t st;
    uint32_t next_at;
    bool gate_on;           /* the sequencer holds GATE_CMD high (or would, in a dry run) */
    bool rly_on;
    xfer_capture_t xfer;    /* which XFER_SENSE capture is running */
    uint16_t *pre, *post, *rec;
    uint32_t *edges;
    float mv_per_code;
    uint16_t awd_code;
    shot_result_t r;
} s;

static uint32_t pending_faults;     /* accessed only through __atomic builtins */
static uint32_t abort_request;

static const char *const reason_names[R_COUNT] = {
    "none", "user_abort", "xfer_dead_a", "no_release_step_a2", "k1_not_operated_b",
    "overcurrent", "gate_guard", "clock_fail", "missed_transfer_c", "no_pickup_d",
    "xfer_dropped", "record_short",
};

const char *shot_reason_name(shot_reason_t r)
{
    return (r < R_COUNT) ? reason_names[r] : "?";
}

static uint32_t at(uint32_t base, int32_t us)
{
    return base + (uint32_t)(us * (int32_t)TICKS_PER_US);
}

static xfer_trace_t trace(const uint16_t *buf, uint32_t n, uint32_t t_sample0, uint32_t anchor)
{
    xfer_trace_t tr;
    tr.code = buf;
    tr.n = n;
    tr.t0_us = shot_us(t_sample0, anchor);
    tr.mv_per_code = s.mv_per_code;
    return tr;
}

/* Drive a control line (not in a dry run) and note the time of the event. */
static void out(hw_out_t o, bool high, shot_event_t ev)
{
    if (!s.r.p.dry) hw_set(o, high);
    if (ev < EV_COUNT) {
        s.r.ev_t[ev] = hw_now();
        s.r.ev_done[ev] = true;
    }
}

static void set_reason(shot_reason_t why)
{
    if (s.r.reason == R_NONE) s.r.reason = why;
}

static void invalidate(shot_reason_t why)
{
    s.r.valid = false;
    set_reason(why);
}

static void finish_idle(void)
{
    if (s.rly_on) {
        out(OUT_RLY, false, EV_RLY_OFF);
        s.rly_on = false;
    }
    out(OUT_TUNE, false, EV_COUNT);
    out(OUT_BLANK2, true, EV_COUNT);
    s.st = ST_DONE;
}

/* End the shot early from any state, keeping the gate-off -> RLY_CMD order. */
static void stop_early(shot_reason_t why)
{
    /* The gate first: nothing below (a peripheral that is slow to stop) may delay it. */
    out(OUT_TUNE, false, EV_COUNT);
    if (s.gate_on) {
        out(OUT_GATE, false, EV_GATE_OFF);
        s.gate_on = false;
        s.r.t_gate_off = s.r.ev_t[EV_GATE_OFF];
    }
    invalidate(why);
    s.r.aborted = true;

    /* Keep what was captured: an aborted shot's XFER trace is the one that explains it. */
    if (s.xfer == XF_PRE) s.r.n_pre = hw_xfer_count();
    else if (s.xfer == XF_POST) s.r.n_post = hw_xfer_count();
    s.xfer = XF_NONE;
    if (s.st == ST_REC_END) {
        s.r.n_rec = hw_record_count();
        s.r.n_edges = hw_edges_count();
    }
    hw_guard_disarm();
    hw_isense_stop();
    hw_xfer_stop();
    hw_xfer_watch(false);
    hw_record_stop();

    if (s.rly_on && s.r.ev_done[EV_GATE_OFF]
        && (int32_t)(hw_now() - at(s.r.t_gate_off, RLY_OFF_US)) < 0) {
        s.st = ST_ABORT_RLY_OFF;
        s.next_at = at(s.r.t_gate_off, RLY_OFF_US);
        return;
    }
    finish_idle();
}

static void on_faults(uint32_t f)
{
    if (f & FAULT_XFER_DROP) {
        if (s.st == ST_BLANK2_OFF || s.st == ST_REC_START || s.st == ST_REC_END)
            s.r.watch_tripped = true;
    }
    if (f & FAULT_CLOCK) {
        stop_early(R_CLOCK);
        return;
    }
    if (f & (FAULT_OVERCURRENT | FAULT_GUARD)) {
        const shot_reason_t why = (f & FAULT_OVERCURRENT) ? R_OVERCURRENT : R_GATE_GUARD;
        if (s.gate_on) stop_early(why);
        else if (s.r.ev_done[EV_GATE_ON]) invalidate(why);  /* tripped as the gate came off: still reported */
    }
}

static void step(void)
{
    xfer_trace_t tr;
    const shot_params_t *p = &s.r.p;

    switch (s.st) {
    case ST_RLY_ON:
        /* (a): XFER_SENSE high just before RLY_CMD, or XFER is dead and there is no shot. */
        tr = trace(s.pre, hw_xfer_count(), s.r.t_pre0, s.next_at);
        s.r.a_ok = xfer_mean_mv(&tr, CHECK_A_FROM_US, CHECK_A_TO_US, &s.r.a_mv)
                   && s.r.a_mv >= (float)XFER_HIGH_MV;
        s.r.a_ran = true;
        if (!s.r.a_ok && !p->dry) { stop_early(R_XFER_DEAD); return; }
        out(OUT_RLY, true, EV_RLY_ON);
        s.rly_on = true;
        s.r.t_rly = s.r.ev_t[EV_RLY_ON];
        s.st = ST_CHECK_A2;
        s.next_at = at(s.r.t_rly, CHECK_A2_AT_US);
        break;

    case ST_CHECK_A2: {
        /* (a2): the front end's Q2 has dropped K_XFER - XFER_SENSE stepped up. */
        float pre, post;
        tr = trace(s.pre, hw_xfer_count(), s.r.t_pre0, s.r.t_rly);
        bool have = xfer_mean_mv(&tr, CHECK_A2_PRE_FROM_US, CHECK_A2_PRE_TO_US, &pre)
                    && xfer_mean_mv(&tr, CHECK_A2_POST_FROM_US, CHECK_A2_POST_TO_US + 1, &post);
        s.r.a2_step_mv = have ? post - pre : 0.0f;
        s.r.a2_ok = have && s.r.a2_step_mv >= (float)A2_MIN_STEP_MV;
        s.r.a2_ran = true;
        if (!s.r.a2_ok && !p->dry) { stop_early(R_NO_RELEASE_STEP); return; }
        s.st = ST_CHECK_B;
        s.next_at = at(s.r.t_rly, CHECK_B_AT_US);
        break;
    }

    case ST_CHECK_B:
        /* (b): K1 has operated - its NC contact no longer feeds XFER. */
        s.r.n_pre = hw_xfer_count();
        tr = trace(s.pre, s.r.n_pre, s.r.t_pre0, s.r.t_rly);
        s.r.b_ok = xfer_mean_mv(&tr, CHECK_B_FROM_US, CHECK_B_TO_US + 1, &s.r.b_mv)
                   && s.r.b_mv <= (float)XFER_LOW_MV;
        s.r.b_ran = true;
        hw_xfer_stop();
        s.xfer = XF_NONE;
        if (!s.r.b_ok && !p->dry) { stop_early(R_K1_NOT_OPERATED); return; }
        s.st = ST_GATE_ON;
        s.next_at = at(s.r.t_rly, RLY_TO_GATE_US);
        break;

    case ST_GATE_ON:
        /* A dry run never drives the gate, so its watchdog threshold is out of reach: a floating PA1
         * on a bare Nucleo must not abort it. */
        hw_isense_start(p->dry ? (uint16_t)ADC_FULL_SCALE : s.awd_code);
        out(OUT_TUNE, false, EV_COUNT);
        out(OUT_GATE, true, EV_GATE_ON);
        s.gate_on = true;
        s.r.t_gate_on = s.r.ev_t[EV_GATE_ON];
        hw_guard_arm((p->t_pol_us + GATE_GUARD_EXTRA_US) * TICKS_PER_US);
        s.st = ST_ILOG_MID;
        s.next_at = at(s.r.t_gate_on, (int32_t)(p->t_pol_us / 2u));
        break;

    case ST_ILOG_MID:
        s.r.i_mid_code = hw_isense_mean();
        s.r.i_mid_done = true;
        s.st = ST_ILOG_END;
        s.next_at = at(s.r.t_gate_on, (int32_t)p->t_pol_us - ISENSE_LOG_END_US);
        break;

    case ST_ILOG_END:
        s.r.i_end_code = hw_isense_mean();
        s.r.i_end_done = true;
        s.st = ST_GATE_OFF;
        s.next_at = at(s.r.t_gate_on, (int32_t)p->t_pol_us);
        break;

    case ST_GATE_OFF:
        out(OUT_TUNE, false, EV_COUNT);
        out(OUT_GATE, false, EV_GATE_OFF);
        s.gate_on = false;
        s.r.t_gate_off = s.r.ev_t[EV_GATE_OFF];
        hw_guard_disarm();
        hw_isense_stop();
        s.st = ST_RLY_OFF;
        s.next_at = at(s.r.t_gate_off, RLY_OFF_US);
        break;

    case ST_RLY_OFF:
        out(OUT_RLY, false, EV_RLY_OFF);
        s.rly_on = false;
        s.r.t_post0 = hw_xfer_start(s.post, POST_CAPTURE_SAMPLES);
        s.xfer = XF_POST;
        s.st = p->tuned ? ST_TUNE_ON : ST_CHECK_CD;
        s.next_at = at(s.r.t_gate_off, p->tuned ? TUNE_ON_US : CHECK_CD_AT_US);
        break;

    case ST_TUNE_ON:
        out(OUT_TUNE, true, EV_TUNE_ON);
        s.st = ST_CHECK_CD;
        s.next_at = at(s.r.t_gate_off, CHECK_CD_AT_US);
        break;

    case ST_CHECK_CD:
        /* (c) the transfer happened, (d) K_XFER picked up; then watch XFER through the record. */
        s.r.n_post = hw_xfer_count();
        hw_xfer_stop();
        s.xfer = XF_NONE;
        tr = trace(s.post, s.r.n_post, s.r.t_post0, s.r.t_gate_off);
        s.r.c_ok = xfer_check_c(&tr, s.r.c_mv);
        xfer_check_d(&tr, &s.r.d);
        s.r.cd_ran = true;
        if (!p->dry) {
            if (!s.r.c_ok) invalidate(R_MISSED_TRANSFER);
            else if (!s.r.d.ok) invalidate(R_NO_PICKUP);
        }
        if (s.r.c_ok) hw_xfer_watch(true);
        s.st = ST_BLANK2_OFF;
        s.next_at = at(s.r.t_gate_off, BLANK2_OFF_US);
        break;

    case ST_BLANK2_OFF:
        out(OUT_BLANK2, false, EV_BLANK2_OFF);
        s.st = ST_REC_START;
        s.next_at = at(s.r.t_gate_off, RECORD_START_US);
        break;

    case ST_REC_START:
        s.r.t_rec0 = hw_record_start(s.rec, RECORD_SAMPLES, s.edges, MAX_EDGES);
        s.r.ev_t[EV_REC_START] = s.r.t_rec0;
        s.r.ev_done[EV_REC_START] = true;
        s.st = ST_REC_END;
        s.next_at = at(s.r.t_gate_off, (int32_t)RECORD_END_US);
        break;

    case ST_REC_END:
        s.r.n_rec = hw_record_count();
        s.r.n_edges = hw_edges_count();
        s.r.edges_overflow = hw_edges_overflow();
        hw_record_stop();
        hw_xfer_watch(false);
        out(OUT_TUNE, false, EV_COUNT);
        out(OUT_BLANK2, true, EV_REC_END);
        if (s.r.n_rec < RECORD_SAMPLES) invalidate(R_RECORD_SHORT);
        if (s.r.watch_tripped) invalidate(R_XFER_DROPPED);
        s.st = ST_DONE;
        break;

    case ST_ABORT_RLY_OFF:
        finish_idle();
        break;

    default:
        s.st = ST_DONE;
        break;
    }
}

void shot_tick(void)
{
    for (;;) {
        uint32_t f = __atomic_exchange_n(&pending_faults, 0u, __ATOMIC_SEQ_CST);
        bool user_abort = __atomic_exchange_n(&abort_request, 0u, __ATOMIC_SEQ_CST) != 0u;
        state_t st = s.st;
        if (st == ST_IDLE || st == ST_DONE) return;
        if (f) on_faults(f);
        if (user_abort && s.st != ST_DONE && s.st != ST_ABORT_RLY_OFF) stop_early(R_USER_ABORT);
        if (s.st == ST_DONE) return;
        if ((int32_t)(s.next_at - hw_now()) > 0) {
            hw_wake_at(s.next_at);
            return;
        }
        step();
    }
}

void shot_fault(uint32_t bits)
{
    __atomic_fetch_or(&pending_faults, bits, __ATOMIC_SEQ_CST);
    state_t st = s.st;
    if (st != ST_IDLE && st != ST_DONE) hw_wake_at(hw_now());
}

void shot_abort(void)
{
    __atomic_store_n(&abort_request, 1u, __ATOMIC_SEQ_CST);
    state_t st = s.st;
    if (st != ST_IDLE && st != ST_DONE) hw_wake_at(hw_now());
}

void shot_init(uint16_t *pre, uint16_t *post, uint16_t *rec, uint32_t *edges, float mv_per_code)
{
    memset(&s, 0, sizeof s);
    s.st = ST_IDLE;
    s.pre = pre;
    s.post = post;
    s.rec = rec;
    s.edges = edges;
    shot_set_mv_per_code(mv_per_code);
}

void shot_set_mv_per_code(float mv_per_code)
{
    float code = (float)ISENSE_ABORT_MV / mv_per_code + 0.5f;
    s.mv_per_code = mv_per_code;
    s.awd_code = (code >= (float)ADC_FULL_SCALE) ? (uint16_t)ADC_FULL_SCALE : (uint16_t)code;
}

bool shot_start(const shot_params_t *p)
{
    if (s.st != ST_IDLE) return false;
    if (p->t_pol_us < T_POL_MIN_US || p->t_pol_us > T_POL_MAX_US) return false;

    memset(&s.r, 0, sizeof s.r);
    s.r.p = *p;
    s.r.valid = true;
    s.gate_on = false;
    s.rly_on = false;
    __atomic_store_n(&pending_faults, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&abort_request, 0u, __ATOMIC_SEQ_CST);

    s.r.ev_t[EV_ARM] = hw_now();
    s.r.ev_done[EV_ARM] = true;
    s.r.t_pre0 = hw_xfer_start(s.pre, PRE_CAPTURE_SAMPLES);
    s.xfer = XF_PRE;
    s.next_at = at(s.r.ev_t[EV_ARM], ARM_TO_RLY_US);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);    /* everything above is written before the interrupt can see st */
    s.st = ST_RLY_ON;
    hw_wake_at(s.next_at);
    return true;
}

bool shot_busy(void)
{
    return s.st != ST_IDLE;
}

bool shot_take_result(shot_result_t *out_r)
{
    if (s.st != ST_DONE) return false;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);    /* read the result only after seeing DONE */
    *out_r = s.r;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    s.st = ST_IDLE;
    return true;
}
