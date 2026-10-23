/* test_shot.c - the shot sequencer against the timing table, on the PC.
 *
 * Every scenario runs one shot on the simulated board (fake_hw.c), starting 1.5 s before TIM2
 * wraps so every shot crosses the wrap. Checked on every scenario:
 *   - the expected outcome (valid / aborted / reason);
 *   - the control-line edges against the table, to 1 us, where the scenario reaches them;
 *   - safety invariants: GATE_CMD only after (a), (a2), (b); TUNE low at every GATE_CMD edge;
 *     RLY_CMD falls >= 0.5 ms after GATE_CMD falls; GATE_CMD never on longer than t_pol + guard;
 *     every line idle at the end; nothing moves in a dry run.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "fake_hw.h"
#include "shot.h"

#define T_START (0xFFFFFFFFu - 255000000u)   /* TIM2 wraps 1.5 s into the shot */
#define MV_PER_CODE (3250.0f / 4095.0f)

static uint16_t pre[PRE_CAPTURE_SAMPLES], post[POST_CAPTURE_SAMPLES], rec[RECORD_SAMPLES];
static uint32_t edges[MAX_EDGES];
static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("    FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* Run one shot. Returns false if the simulation got stuck. */
static bool run(const scenario_t *sc, shot_result_t *r)
{
    shot_params_t p;
    p.t_pol_us = sc->t_pol_us;
    p.tuned = sc->tuned;
    p.dry = sc->dry;

    fake_reset(sc, T_START);
    shot_init(pre, post, rec, edges, MV_PER_CODE);
    if (!shot_start(&p)) { printf("    shot_start refused\n"); return false; }

    for (int n = 0; n < 100000; n++) {
        if (!fake_advance()) { printf("    simulation stuck\n"); return false; }
        if (shot_take_result(r)) return true;
    }
    printf("    too many steps\n");
    return false;
}

/* Time of the n-th (0-based) edge of a line to a level; false if there is none. */
static bool edge(hw_out_t o, bool high, int nth, uint32_t *t)
{
    for (int i = 0; i < F.n_log; i++)
        if (F.log[i].out == o && F.log[i].high == high && nth-- == 0) { *t = F.log[i].t; return true; }
    return false;
}

static void expect_edge(const char *what, hw_out_t o, bool high, uint32_t ref, int32_t us)
{
    uint32_t t;
    if (!edge(o, high, 0, &t)) { CHECK(0, "%s: no edge", what); return; }
    int32_t got = shot_us(t, ref);
    CHECK(got >= us - 1 && got <= us + 1, "%s at %ld us, expected %ld us", what, (long)got, (long)us);
}

static void expect_no_edge(const char *what, hw_out_t o, bool high)
{
    uint32_t t;
    CHECK(!edge(o, high, 0, &t), "%s happened", what);
}

static void invariants(const scenario_t *sc, const shot_result_t *r)
{
    bool tune = false, gate = false, rly = false;
    uint32_t t_gate_rise = 0, t_gate_fall = 0;
    bool gate_fell = false;

    if (sc->dry) CHECK(F.n_log == 0, "dry run moved %d lines", F.n_log);
    for (int i = 0; i < F.n_log; i++) {
        const transition_t *e = &F.log[i];
        if (e->out == OUT_GATE) {
            CHECK(!tune, "TUNE high at a GATE_CMD edge");
            if (e->high) {
                CHECK(r->a_ok && r->a2_ok && r->b_ok, "GATE_CMD high without (a), (a2), (b)");
                t_gate_rise = e->t;
            } else if (gate) {
                int32_t on_us = shot_us(e->t, t_gate_rise);
                CHECK(on_us <= (int32_t)(sc->t_pol_us + GATE_GUARD_EXTRA_US) + 1,
                      "GATE_CMD on for %ld us", (long)on_us);
                t_gate_fall = e->t;
                gate_fell = true;
            }
            gate = e->high;
        }
        if (e->out == OUT_RLY) {
            if (!e->high && rly && gate_fell)
                CHECK(shot_us(e->t, t_gate_fall) >= RLY_OFF_US - 1,
                      "RLY_CMD fell %ld us after GATE_CMD", (long)shot_us(e->t, t_gate_fall));
            CHECK(!(e->high && gate), "RLY_CMD rose with GATE_CMD high");
            rly = e->high;
        }
        if (e->out == OUT_TUNE) tune = e->high;
    }
    CHECK(!F.out[OUT_GATE] && !F.out[OUT_RLY] && !F.out[OUT_TUNE] && F.out[OUT_BLANK2],
          "lines not idle at the end (G%d R%d T%d B%d)", F.out[OUT_GATE], F.out[OUT_RLY],
          F.out[OUT_TUNE], F.out[OUT_BLANK2]);
    CHECK(!F.guard_on && !F.ion && !F.xon && !F.ron && !F.watch, "a capture or guard left running");
}

/* The healthy timing: every edge of the table, measured from the right anchor. */
static void table_edges(const scenario_t *sc)
{
    uint32_t t_rly, t_gate, t_off;
    if (!edge(OUT_RLY, true, 0, &t_rly) || !edge(OUT_GATE, true, 0, &t_gate) || !edge(OUT_GATE, false, 0, &t_off)) {
        CHECK(0, "missing RLY/GATE edges");
        return;
    }
    expect_edge("RLY_CMD high", OUT_RLY, true, T_START, ARM_TO_RLY_US);
    expect_edge("GATE_CMD high", OUT_GATE, true, t_rly, RLY_TO_GATE_US);
    expect_edge("GATE_CMD low", OUT_GATE, false, t_gate, (int32_t)sc->t_pol_us);
    expect_edge("RLY_CMD low", OUT_RLY, false, t_off, RLY_OFF_US);
    if (sc->tuned) {
        expect_edge("TUNE high", OUT_TUNE, true, t_off, TUNE_ON_US);
        expect_edge("TUNE low", OUT_TUNE, false, t_off, (int32_t)RECORD_END_US);
    } else {
        expect_no_edge("TUNE high", OUT_TUNE, true);
    }
    expect_edge("BLANK2 low", OUT_BLANK2, false, t_off, BLANK2_OFF_US);
    expect_edge("BLANK2 high", OUT_BLANK2, true, t_off, (int32_t)RECORD_END_US);
}

static scenario_t base(const char *name)
{
    scenario_t sc;
    memset(&sc, 0, sizeof sc);
    sc.name = name;
    sc.t_pol_us = 3000000u;
    xfer_model_rev_de(&sc.m);   /* healthy rev D/E stack */
    sc.i_sense_mv = 1200.0f;
    return sc;
}

static void report(const char *name, const shot_result_t *r)
{
    printf("  %-28s valid=%d aborted=%d reason=%-20s a=%4.0f a2=%5.1f b=%4.0f c=%4.0f/%4.0f/%4.0f "
           "d_step=%5.1f pick=%5.1fms\n",
           name, r->valid, r->aborted, shot_reason_name(r->reason), (double)r->a_mv, (double)r->a2_step_mv,
           (double)r->b_mv, (double)r->c_mv[0], (double)r->c_mv[1], (double)r->c_mv[2], (double)r->d.step_mv,
           (double)r->d.pickup_us / 1000.0);
}

static void expect(const scenario_t *sc, bool valid, bool aborted, shot_reason_t reason, shot_result_t *r)
{
    int before = failures;
    if (!run(sc, r)) { failures++; printf("  %s: did not finish\n", sc->name); return; }
    report(sc->name, r);
    CHECK(r->valid == valid, "valid=%d, expected %d", r->valid, valid);
    CHECK(r->aborted == aborted, "aborted=%d, expected %d", r->aborted, aborted);
    CHECK(r->reason == reason, "reason %s, expected %s", shot_reason_name(r->reason), shot_reason_name(reason));
    invariants(sc, r);
    if (failures != before) printf("    (scenario %s)\n", sc->name);
}

int main(void)
{
    shot_result_t r;
    scenario_t sc;
    uint32_t t, t_off;

    printf("Shot sequencer against the timing table\n");

    sc = base("healthy rev D/E, untuned");
    expect(&sc, true, false, R_NONE, &r);
    table_edges(&sc);
    CHECK(fabsf(r.a2_step_mv - 61.0f) < 3.0f, "a2 step %.1f", (double)r.a2_step_mv);
    CHECK(fabsf(r.d.step_mv - 57.0f) < 3.0f, "d step %.1f", (double)r.d.step_mv);
    CHECK(r.d.pickup_us > 6000 && r.d.pickup_us < 9500 && !r.d.pickup_flag, "pickup %ld us", (long)r.d.pickup_us);
    CHECK(r.n_rec == RECORD_SAMPLES, "record %lu samples", (unsigned long)r.n_rec);
    CHECK(shot_us(r.t_rec0, r.t_gate_off) == RECORD_START_US + (int32_t)US_PER_SAMPLE,
          "record sample 0 at %ld us", (long)shot_us(r.t_rec0, r.t_gate_off));
    CHECK(r.i_end_code > 1450 && r.i_end_code < 1580, "I_SENSE code %u", r.i_end_code);

    sc = base("healthy rev D/E, tuned");
    sc.tuned = true;
    expect(&sc, true, false, R_NONE, &r);
    table_edges(&sc);

    sc = base("rev C slow relay");
    sc.m.nc_open_ms = 10.0f; sc.m.nc_make_ms = 15.0f; sc.m.bounce_ms = 7.2f; sc.m.pick_ms = 9.5f;
    sc.tuned = true;
    expect(&sc, true, false, R_NONE, &r);
    table_edges(&sc);
    CHECK(fabsf(r.d.step_mv - 57.0f) < 4.0f, "d step %.1f through the bounce", (double)r.d.step_mv);

    sc = base("rev C fast relay");
    sc.m.nc_open_ms = 2.6f; sc.m.nc_make_ms = 2.5f; sc.m.bounce_ms = 7.2f; sc.m.pick_ms = 3.8f;
    expect(&sc, true, false, R_NONE, &r);
    table_edges(&sc);

    sc = base("shortest bench shot (50 ms)");
    sc.t_pol_us = T_POL_MIN_US;
    expect(&sc, true, false, R_NONE, &r);
    table_edges(&sc);

    sc = base("(a) XFER dead");
    sc.m.xfer_dead = true;
    expect(&sc, false, true, R_XFER_DEAD, &r);
    expect_no_edge("RLY_CMD high", OUT_RLY, true);
    expect_no_edge("GATE_CMD high", OUT_GATE, true);
    CHECK(r.n_pre > 30, "(a) abort kept %lu XFER samples: the trace that explains it should be kept",
          (unsigned long)r.n_pre);
    CHECK(r.a_ran && !r.a2_ran && !r.b_ran && !r.cd_ran, "(a) abort: wrong checks marked as run");

    sc = base("(a2) no release step");
    sc.m.release_step_mv = 0.0f;
    expect(&sc, false, true, R_NO_RELEASE_STEP, &r);
    expect_no_edge("GATE_CMD high", OUT_GATE, true);
    if (edge(OUT_RLY, true, 0, &t)) expect_edge("RLY_CMD low at once", OUT_RLY, false, t, CHECK_A2_AT_US);

    sc = base("(b) K1 does not operate");
    sc.m.k1_stuck = true;
    expect(&sc, false, true, R_K1_NOT_OPERATED, &r);
    expect_no_edge("GATE_CMD high", OUT_GATE, true);
    if (edge(OUT_RLY, true, 0, &t)) expect_edge("RLY_CMD low", OUT_RLY, false, t, CHECK_B_AT_US);

    sc = base("(c) missed transfer");
    sc.m.missed_transfer = true;
    expect(&sc, false, false, R_MISSED_TRANSFER, &r);
    table_edges(&sc);

    sc = base("(d) K_XFER does not pick up");
    sc.m.pick_step_mv = 0.0f;
    expect(&sc, false, false, R_NO_PICKUP, &r);
    table_edges(&sc);

    sc = base("XFER drops in the record");
    sc.m.drop_at_ms = 500.0f;
    expect(&sc, false, false, R_XFER_DROPPED, &r);
    table_edges(&sc);

    sc = base("overcurrent at +1.2 s");
    sc.overcurrent_at_ms = 1200.0f;
    expect(&sc, false, true, R_OVERCURRENT, &r);
    if (edge(OUT_GATE, false, 0, &t_off)) {
        uint32_t t_on;
        if (edge(OUT_GATE, true, 0, &t_on))
            CHECK(shot_us(t_off, t_on) == 1200000, "gate fell at %ld us", (long)shot_us(t_off, t_on));
        expect_edge("RLY_CMD low", OUT_RLY, false, t_off, RLY_OFF_US);
    }
    expect_no_edge("BLANK2 low", OUT_BLANK2, false);

    sc = base("HSE fails while polarizing");
    sc.clock_fail_at_ms = 800.0f;
    expect(&sc, false, true, R_CLOCK, &r);
    if (edge(OUT_GATE, false, 0, &t_off)) expect_edge("RLY_CMD low 0.5 ms after the NMI's gate-off", OUT_RLY, false, t_off, RLY_OFF_US);
    CHECK(!r.i_end_done, "clock fault at 0.8 s: the end-of-gate I_SENSE reading never happened");

    sc = base("sequencer stalls: guard ends the gate");
    sc.stall_at_ms = 100.0f;
    expect(&sc, false, true, R_GATE_GUARD, &r);
    if (edge(OUT_GATE, true, 0, &t))
        expect_edge("GATE_CMD low by the guard", OUT_GATE, false, t, (int32_t)(sc.t_pol_us + GATE_GUARD_EXTRA_US));

    sc = base("user abort while arming");
    sc.abort_at_ms = 12.0f;
    expect(&sc, false, true, R_USER_ABORT, &r);
    expect_no_edge("GATE_CMD high", OUT_GATE, true);

    sc = base("user abort while polarizing");
    sc.abort_at_ms = 1500.0f;
    expect(&sc, false, true, R_USER_ABORT, &r);
    if (edge(OUT_GATE, false, 0, &t_off)) expect_edge("RLY_CMD low", OUT_RLY, false, t_off, RLY_OFF_US);

    sc = base("user abort 0.2 ms after gate-off");
    sc.abort_at_ms = 2.0f + 30.0f + 3000.0f + 0.2f;
    expect(&sc, false, true, R_USER_ABORT, &r);
    if (edge(OUT_GATE, false, 0, &t_off)) expect_edge("RLY_CMD still waits", OUT_RLY, false, t_off, RLY_OFF_US);

    sc = base("user abort in the record");
    sc.abort_at_ms = 2.0f + 30.0f + 3000.0f + 300.0f;
    sc.tuned = true;
    expect(&sc, false, true, R_USER_ABORT, &r);

    sc = base("dry run, no board");
    sc.dry = true;
    sc.m.xfer_dead = true;
    expect(&sc, true, false, R_NONE, &r);
    CHECK(!r.a_ok, "dry run: (a) should read the dead XFER as low");

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
