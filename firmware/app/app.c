/* app.c - the console, the shot reports and the binary dumps.
 *
 * Commands (one per line, 115,200 baud by default on the ST-LINK virtual COM port):
 *   help | status | config | vref | abort | selftest | gps [raw]
 *   shot <t_pol_s> [tuned] [dump]           one shot
 *   dry  <t_pol_s> [tuned] [dump]           the whole sequence with the control lines left idle
 *   run  <n> <t_pol_s> [tuned] [dump] [gap=<s>]
 * Every result is a line of key=value pairs starting with SHOT, CHECK, TIME, FIT or PATHA, so
 * tools/mag.py (or a terminal) can read it.
 * With a GPS (app/gps.h) the SHOT line carries UTC, the position and the measured clock, and FIT and
 * PATHA give frequencies corrected to the GPS second (f_true = f x ticks_per_s / TICK_HZ).
 */
#include "app.h"

#include <math.h>
#include <string.h>

#include "config.h"
#include "fit.h"
#include "fmt.h"
#include "gps.h"
#include "hw.h"
#include "noise.h"
#include "patha.h"
#include "shot.h"

#if defined(MAG_TARGET) && MAG_TARGET
#define APP_CCM __attribute__((section(".ccmram")))   /* CPU-only buffers in CCM SRAM */
#else
#define APP_CCM
#endif

static uint16_t rec_buf[RECORD_SAMPLES];
static uint32_t edge_buf[MAX_EDGES];
static uint16_t pre_buf[PRE_CAPTURE_SAMPLES];
static uint16_t post_buf[POST_CAPTURE_SAMPLES];
static float fit_work[FIT_WORK_FLOATS] APP_CCM;

static struct {
    uint32_t vref_mv;
    float mv_per_code;
    uint32_t shot_id;
    char line[96];
    uint32_t line_len;
    bool dump;                  /* dump the shot that is running */
    uint32_t run_left;          /* shots still to start in a run */
    shot_params_t run_p;
    bool run_dump;
    uint32_t run_gap_us;
    bool run_waiting;
    uint32_t run_wait_left_us;  /* counted down in app_poll: TIM2's 25 s wrap cannot shorten it */
    uint32_t run_wait_last;     /* TIM2 time the countdown last advanced to */
    uint32_t missed_in_a_row;
} a;

/* ---- Output ------------------------------------------------------------------------------------- */
static void say(const char *s)
{
    plat_write(s, (uint32_t)strlen(s));
}

static void line_out(fmt_t *f)
{
    fmt_str(f, "\r\n");
    plat_write(f->buf, f->len);
}

/* ---- Results ------------------------------------------------------------------------------------ */
static double code_mv(double code) { return code * (double)a.mv_per_code; }

/* The clock correction of one shot, decided once and used by every line of its report. */
typedef struct {
    bool on;            /* the GPS measured the clock (gps_clock) */
    double k;           /* ticks_per_s / TICK_HZ: multiplies every frequency measured on TIM2 or the ADC */
    uint32_t span_s;    /* seconds of pulses the measurement spans */
} clock_corr_t;

/* The time a shot is stamped with: sample 0 of the record, else gate-off, else arming. */
static uint32_t shot_stamp(const shot_result_t *r, const char **at)
{
    if (r->ev_done[EV_REC_START]) { *at = "rec0"; return r->t_rec0; }
    if (r->ev_done[EV_GATE_OFF]) { *at = "gate_off"; return r->t_gate_off; }
    *at = "arm";
    return r->ev_t[EV_ARM];
}

/* " key=value", or " key=na" for a check or reading that never happened */
static void kv_fix_or_na(fmt_t *f, const char *key, bool have, double v, int decimals)
{
    if (have) fmt_kv_fix(f, key, v, decimals);
    else fmt_kv_str(f, key, "na");
}

static void kv_bool_or_na(fmt_t *f, const char *key, bool have, bool v)
{
    if (have) fmt_kv_u32(f, key, v ? 1u : 0u);
    else fmt_kv_str(f, key, "na");
}

static void report_shot(const shot_result_t *r, uint32_t id, const char *result, const clock_corr_t *c)
{
    char b[640];
    const char *at;
    const uint32_t t_stamp = shot_stamp(r, &at);
    fmt_t f;
    fmt_init(&f, b, sizeof b);
    fmt_str(&f, "SHOT");
    fmt_kv_u32(&f, "id", id);
    fmt_kv_str(&f, "mode", r->p.dry ? "dry" : "shot");
    fmt_kv_u32(&f, "tuned", r->p.tuned);
    fmt_kv_fix(&f, "tpol_s", (double)r->p.t_pol_us / 1e6, 3);
    fmt_kv_str(&f, "result", result);
    fmt_kv_str(&f, "reason", shot_reason_name(r->reason));
    fmt_kv_str(&f, "clock", plat_clock_name());
    fmt_kv_str(&f, "utc_at", at);
    gps_format_fields(&f, t_stamp);
    kv_fix_or_na(&f, "clk_ppm", c->on, (c->k - 1.0) * 1e6, 3);
    fmt_kv_u32(&f, "clk_s", c->on ? c->span_s : 0u);
    line_out(&f);

    fmt_init(&f, b, sizeof b);
    fmt_str(&f, "CHECK");
    fmt_kv_u32(&f, "id", id);
    kv_fix_or_na(&f, "a_mV", r->a_ran, (double)r->a_mv, 0);
    kv_bool_or_na(&f, "a_ok", r->a_ran, r->a_ok);
    kv_fix_or_na(&f, "a2_mV", r->a2_ran, (double)r->a2_step_mv, 1);
    kv_bool_or_na(&f, "a2_ok", r->a2_ran, r->a2_ok);
    kv_fix_or_na(&f, "b_mV", r->b_ran, (double)r->b_mv, 0);
    kv_bool_or_na(&f, "b_ok", r->b_ran, r->b_ok);
    kv_fix_or_na(&f, "i_mid_mV", r->i_mid_done, code_mv(r->i_mid_code), 0);
    kv_fix_or_na(&f, "i_end_mV", r->i_end_done, code_mv(r->i_end_code), 0);
    kv_bool_or_na(&f, "i_low", r->i_end_done && !r->p.dry, code_mv(r->i_end_code) < ISENSE_LOW_MV);
    kv_fix_or_na(&f, "c1_mV", r->cd_ran, (double)r->c_mv[0], 0);
    kv_fix_or_na(&f, "c2_mV", r->cd_ran, (double)r->c_mv[1], 0);
    kv_fix_or_na(&f, "c3_mV", r->cd_ran, (double)r->c_mv[2], 0);
    kv_bool_or_na(&f, "c_ok", r->cd_ran, r->c_ok);
    kv_fix_or_na(&f, "d_rise_ms", r->cd_ran && r->d.rose, (double)r->d.t_rise_us / 1000.0, 2);
    kv_fix_or_na(&f, "d_step_mV", r->cd_ran && r->d.rose, (double)r->d.step_mv, 1);
    kv_fix_or_na(&f, "d_pick_ms", r->cd_ran && r->d.pickup_us >= 0, (double)r->d.pickup_us / 1000.0, 2);
    kv_bool_or_na(&f, "d_flag", r->cd_ran, r->d.pickup_flag);
    kv_bool_or_na(&f, "d_ok", r->cd_ran, r->d.ok);
    fmt_kv_u32(&f, "watch", r->watch_tripped);
    line_out(&f);

    /* Measured event times, microseconds from gate-off (from RLY_CMD before the gate came on). */
    static const char *const ev_name[EV_COUNT] = {
        "arm_us", "rly_on_us", "gate_on_us", "gate_off_us", "rly_off_us", "tune_on_us",
        "blank2_off_us", "rec0_us", "rec_end_us",
    };
    uint32_t anchor;
    const char *from;
    if (r->ev_done[EV_GATE_OFF]) { anchor = r->t_gate_off; from = "gate_off"; }
    else if (r->ev_done[EV_RLY_ON]) { anchor = r->t_rly; from = "rly_on"; }
    else { anchor = r->ev_t[EV_ARM] + (uint32_t)ARM_TO_RLY_US * TICKS_PER_US; from = "rly_planned"; }
    fmt_init(&f, b, sizeof b);
    fmt_str(&f, "TIME");
    fmt_kv_u32(&f, "id", id);
    fmt_kv_str(&f, "from", from);
    for (int e = 0; e < EV_COUNT; e++)
        if (r->ev_done[e]) fmt_kv_i32(&f, ev_name[e], shot_us(r->ev_t[e], anchor));
    if (r->ev_done[EV_GATE_ON] && r->ev_done[EV_GATE_OFF])
        fmt_kv_i32(&f, "tpol_us", shot_us(r->t_gate_off, r->t_gate_on));
    fmt_kv_u32(&f, "n_rec", r->n_rec);
    fmt_kv_u32(&f, "n_edges", r->n_edges);
    if (r->edges_overflow) fmt_kv_u32(&f, "edges_overflow", 1);
    line_out(&f);
}

/* density_ratio: noise_density_ratio() of the residual (<= 0 if it could not be measured).
 * f_Hz, fsig_Hz, B_nT and Bsig_nT are corrected by the measured clock; f_raw_Hz, fseed_Hz and T2_s are on
 * the nominal clock (T2 moves by the same few ppm, far below its own error). */
static void report_fit(uint32_t id, const fit_result_t *fit, double density_ratio, uint32_t ms,
                       const clock_corr_t *c)
{
    char b[400];
    fmt_t f;
    double snr = fit->resid_rms > 0.0 ? fit->amp / fit->resid_rms : 0.0;
    /* f_sigma_hz assumes white noise; scaled by the measured density near f it matches the real scatter */
    double fsig = (density_ratio > 0.0 ? fit->f_sigma_hz * sqrt(density_ratio) : fit->f_sigma_hz) * c->k;
    const double f_hz = fit->f_hz * c->k;
    fmt_init(&f, b, sizeof b);
    fmt_str(&f, "FIT");
    fmt_kv_u32(&f, "id", id);
    fmt_kv_i32(&f, "status", fit->status);
    fmt_kv_fix(&f, "f_Hz", f_hz, 6);
    fmt_kv_fix(&f, "fsig_Hz", fsig, 6);
    fmt_kv_fix(&f, "B_nT", f_hz / GAMMA_HZ_PER_UT * 1000.0, 3);
    fmt_kv_fix(&f, "Bsig_nT", fsig / GAMMA_HZ_PER_UT * 1000.0, 3);
    fmt_kv_str(&f, "corr", c->on ? "gps" : "none");
    fmt_kv_fix(&f, "f_raw_Hz", fit->f_hz, 6);
    fmt_kv_fix(&f, "fsig_white_Hz", fit->f_sigma_hz, 6);
    fmt_kv_fix(&f, "noise_density", density_ratio, 2);
    fmt_kv_fix(&f, "A_mV", code_mv(fit->amp), 2);
    fmt_kv_fix(&f, "T2_s", fit->decay_per_s > 0.0 ? 1.0 / fit->decay_per_s : 0.0, 3);
    fmt_kv_fix(&f, "noise_mV", code_mv(fit->resid_rms), 2);
    fmt_kv_fix(&f, "snr", snr, 2);
    fmt_kv_fix(&f, "offset_mV", code_mv(fit->offset), 1);
    fmt_kv_u32(&f, "iters", (uint32_t)fit->iterations);
    fmt_kv_fix(&f, "fseed_Hz", fit->f_seed_hz, 3);
    fmt_kv_u32(&f, "ms", ms);
    line_out(&f);
}

/* Frequencies corrected by the clock, as in FIT. */
static void report_patha(uint32_t id, const fit_result_t *fit, const patha_result_t *pa, const char *skipped,
                         const clock_corr_t *c)
{
    char b[300];
    fmt_t f;
    fmt_init(&f, b, sizeof b);
    fmt_str(&f, "PATHA");
    fmt_kv_u32(&f, "id", id);
    if (skipped) {
        fmt_kv_str(&f, "skipped", skipped);
    } else {
        fmt_kv_i32(&f, "status", pa->status);
        fmt_kv_fix(&f, "f_Hz", pa->f_hz * c->k, 6);
        fmt_kv_fix(&f, "f_rise_Hz", pa->f_rise_hz * c->k, 6);
        fmt_kv_fix(&f, "f_fall_Hz", pa->f_fall_hz * c->k, 6);
        fmt_kv_u32(&f, "n_rise", pa->n_rise);
        fmt_kv_u32(&f, "n_fall", pa->n_fall);
        fmt_kv_u32(&f, "rejected", pa->n_rejected);
        fmt_kv_fix(&f, "minus_fit_nT", (pa->f_hz - fit->f_hz) * c->k / GAMMA_HZ_PER_UT * 1000.0, 3);
    }
    line_out(&f);
}

/* Binary dump: a header line, the raw arrays (little-endian), then "END <id>". */
static void dump(uint32_t id, const shot_result_t *r)
{
    char b[400];
    fmt_t f;
    uint32_t crc = 0, bytes;
    const uint32_t n_rec = r->n_rec, n_edges = r->n_edges, n_pre = r->n_pre, n_post = r->n_post;
    bytes = n_rec * 2u + n_edges * 4u + n_pre * 2u + n_post * 2u;
    crc = crc32_update(crc, rec_buf, n_rec * 2u);
    crc = crc32_update(crc, edge_buf, n_edges * 4u);
    crc = crc32_update(crc, pre_buf, n_pre * 2u);
    crc = crc32_update(crc, post_buf, n_post * 2u);

    fmt_init(&f, b, sizeof b);
    fmt_str(&f, "DUMP");
    fmt_kv_u32(&f, "id", id);
    fmt_kv_u32(&f, "bytes", bytes);
    fmt_str(&f, " crc32=");
    fmt_hex32(&f, crc);
    fmt_kv_u32(&f, "rec", n_rec);
    fmt_kv_u32(&f, "edges", n_edges);
    fmt_kv_u32(&f, "pre", n_pre);
    fmt_kv_u32(&f, "post", n_post);
    fmt_kv_u32(&f, "fs_Hz", SAMPLE_HZ);
    fmt_kv_u32(&f, "tick_Hz", TICK_HZ);
    fmt_kv_u32(&f, "vref_mV", a.vref_mv);
    fmt_kv_u32(&f, "t_rec0", r->t_rec0);
    fmt_kv_i32(&f, "rec0_us", shot_us(r->t_rec0, r->t_gate_off));
    /* XFER_SENSE before the gate: times from RLY_CMD rising, or from when it would have risen */
    uint32_t rly = r->ev_done[EV_RLY_ON] ? r->t_rly : r->ev_t[EV_ARM] + (uint32_t)ARM_TO_RLY_US * TICKS_PER_US;
    fmt_kv_i32(&f, "pre0_us", shot_us(r->t_pre0, rly));
    fmt_kv_i32(&f, "post0_us", shot_us(r->t_post0, r->t_gate_off));
    fmt_kv_u32(&f, "tuned", r->p.tuned);
    line_out(&f);
    plat_write((const char *)rec_buf, n_rec * 2u);
    plat_write((const char *)edge_buf, n_edges * 4u);
    plat_write((const char *)pre_buf, n_pre * 2u);
    plat_write((const char *)post_buf, n_post * 2u);
    fmt_init(&f, b, sizeof b);
    fmt_str(&f, "\r\nEND");
    fmt_kv_u32(&f, "id", id);
    line_out(&f);
}

/* A steady sine (a signal generator on the bench) pins the decay at its floor and the fit says
 * FIT_E_DECAY, but its frequency is still good: Path A may use it. The shot itself stays no_fid. */
static bool fit_frequency_usable(const fit_result_t *fit)
{
    return fit->status == FIT_OK || (fit->status == FIT_E_DECAY && fit->decay_per_s < 0.01);
}

static void process(const shot_result_t *r)
{
    const uint32_t id = a.shot_id;
    const char *result = r->aborted ? "aborted" : (r->valid ? "ok" : "invalid");
    fit_result_t fit;
    patha_result_t pa;
    clock_corr_t clk;
    const char *at;

    memset(&fit, 0, sizeof fit);
    memset(&pa, 0, sizeof pa);
    /* The record and the edges are timed by the HSE; the GPS says how long its second really is. */
    clk.on = gps_clock(shot_stamp(r, &at), &clk.k, &clk.span_s);
    clk.k = clk.on ? clk.k / (double)TICK_HZ : 1.0;
    if (r->n_rec == RECORD_SAMPLES) {
        uint32_t t0 = hw_now();
        fit_record(rec_buf, r->n_rec, (double)SAMPLE_HZ, FIT_F_LO_HZ, FIT_F_HI_HZ, fit_work,
                   FIT_WORK_FLOATS, plat_kick, &fit);
        double density = fit_frequency_usable(&fit) ? noise_density_ratio(rec_buf, r->n_rec, (double)SAMPLE_HZ, &fit)
                                                    : -1.0;
        uint32_t ms = (uint32_t)shot_us(hw_now(), t0) / 1000u;
        double snr = fit.resid_rms > 0.0 ? fit.amp / fit.resid_rms : 0.0;
        if (r->valid && !r->p.dry && (fit.status != FIT_OK || snr < FID_MIN_SNR)) result = "no_fid";
        report_shot(r, id, result, &clk);
        report_fit(id, &fit, density, ms, &clk);
        if (!fit_frequency_usable(&fit)) report_patha(id, &fit, &pa, "no_fit", &clk);
        else if (!r->p.tuned) report_patha(id, &fit, &pa, "untuned", &clk);
        else if (snr < PATH_A_MIN_SNR) report_patha(id, &fit, &pa, "snr_below_15", &clk);
        else {
            fit_result_t usable = fit;      /* patha_estimate() takes only FIT_OK: see fit_frequency_usable() */
            usable.status = FIT_OK;
            patha_estimate(edge_buf, r->n_edges, r->t_rec0, (double)TICK_HZ, &usable, (double)SAMPLE_HZ,
                           fit.resid_rms, &pa);
            report_patha(id, &fit, &pa, NULL, &clk);
        }
    } else {
        report_shot(r, id, result, &clk);
    }
    if (a.dump) dump(id, r);
    {
        char b[40];
        fmt_t f;
        fmt_init(&f, b, sizeof b);
        fmt_str(&f, "DONE");
        fmt_kv_u32(&f, "id", id);
        line_out(&f);
    }

    /* A run stops after three missed transfers in a row: K1's NC contact (README check (c)). */
    if (r->reason == R_MISSED_TRANSFER) a.missed_in_a_row++;
    else if (!r->aborted) a.missed_in_a_row = 0;
    if (a.run_left && a.missed_in_a_row >= MAX_MISSED_IN_A_ROW) {
        say("RUN stopped: 3 missed transfers in a row - check K1's NC contact\r\n");
        a.run_left = 0;
    }
    if (a.run_left && r->aborted) {
        say("RUN stopped: shot aborted\r\n");
        a.run_left = 0;
    }
    if (a.run_left) {
        a.run_waiting = true;
        a.run_wait_left_us = a.run_gap_us;
        a.run_wait_last = hw_now();
    }
}

static bool start_shot(const shot_params_t *p, bool want_dump)
{
    if (shot_busy()) { say("ERR busy\r\n"); return false; }
    if (!plat_clock_ready()) { say("ERR clock: the HSE just stopped - wait for the CLOCK line, then retry\r\n"); return false; }
    a.dump = want_dump;
    a.shot_id++;
    if (!shot_start(p)) {
        say("ERR t_pol out of range (0.05 to 4 s)\r\n");
        a.shot_id--;
        return false;
    }
    return true;
}

/* ---- Self-test: the on-board fit against synthetic records -------------------------------------- */
static uint32_t lcg;
static float gauss(void)
{
    float s = 0.0f;
    for (int i = 0; i < 12; i++) {
        lcg = lcg * 1664525u + 1013904223u;
        s += (float)(lcg >> 8) * (1.0f / 16777216.0f);
    }
    return s - 6.0f;
}

static void selftest(void)
{
    static const float snr_case[3] = { 0.0f, 31.6f, 6.7f };    /* 0 = no noise */
    const double f_true = 2128.875, t2 = 1.0, amp = 735.0;
    char b[200];
    fmt_t f;

    if (shot_busy()) { say("ERR busy\r\n"); return; }
    for (int c = 0; c < 3; c++) {
        fit_result_t fit;
        float sigma = snr_case[c] > 0.0f ? (float)amp / snr_case[c] : 0.0f;
        lcg = 455u + (uint32_t)c;
        for (uint32_t k = 0; k < RECORD_SAMPLES; k++) {
            double t = (double)k / SAMPLE_HZ;
            double v = 2048.0 + amp * exp(-t / t2) * sin(6.283185307179586 * f_true * t + 0.7)
                       + (double)(sigma * gauss());
            rec_buf[k] = (uint16_t)(v < 0.0 ? 0.0 : (v > 4095.0 ? 4095.0 : v + 0.5));
            if ((k & 1023u) == 0u) plat_kick();
        }
        uint32_t t0 = hw_now();
        fit_record(rec_buf, RECORD_SAMPLES, (double)SAMPLE_HZ, FIT_F_LO_HZ, FIT_F_HI_HZ, fit_work,
                   FIT_WORK_FLOATS, plat_kick, &fit);
        uint32_t ms = (uint32_t)shot_us(hw_now(), t0) / 1000u;
        fmt_init(&f, b, sizeof b);
        fmt_str(&f, "SELFTEST");
        fmt_kv_fix(&f, "snr", (double)snr_case[c], 1);
        fmt_kv_i32(&f, "status", fit.status);
        fmt_kv_fix(&f, "f_Hz", fit.f_hz, 6);
        fmt_kv_fix(&f, "err_mHz", (fit.f_hz - f_true) * 1000.0, 3);
        fmt_kv_fix(&f, "fsig_mHz", fit.f_sigma_hz * 1000.0, 3);
        fmt_kv_u32(&f, "iters", (uint32_t)fit.iterations);
        fmt_kv_u32(&f, "ms", ms);
        line_out(&f);
    }
}

/* ---- Console ------------------------------------------------------------------------------------ */
static void cmd_help(void)
{
    say("Commands:\r\n"
        "  shot <t_pol_s> [tuned] [dump]       one shot (t_pol 0.05-4 s; the specification is 2-4 s)\r\n"
        "  dry <t_pol_s> [tuned] [dump]        the sequence with every control line left idle\r\n"
        "  run <n> <t_pol_s> [tuned] [dump] [gap=<s>]   n shots, default gap 1 s\r\n"
        "  abort                               stop now (also the B1 button)\r\n"
        "  emu <off|none|dead|no_release|k1_stuck|missed|no_pickup|drop|rev_c>\r\n"
        "                                      bench XFER emulator on PA4: bare Nucleo, PA4 jumpered to PB0\r\n"
        "  gps [raw]                           GPS state; raw: echo the next 20 RMC/GGA sentences\r\n"
        "  status | config | vref | selftest | help\r\n");
}

static void cmd_status(void)
{
    char b[200];
    fmt_t f;
    fmt_init(&f, b, sizeof b);
    fmt_str(&f, "STATUS");
    fmt_kv_str(&f, "fw", FW_VERSION);
    fmt_kv_str(&f, "clock", plat_clock_name());
    fmt_kv_u32(&f, "vref_mV", a.vref_mv);
    fmt_kv_str(&f, "state", shot_busy() ? "shot" : (a.run_waiting ? "run_gap" : "idle"));
    fmt_kv_u32(&f, "shots", a.shot_id);
    fmt_kv_u32(&f, "run_left", a.run_left);
    fmt_kv_str(&f, "silicon", plat_silicon());
    fmt_kv_str(&f, "watchdog", plat_watchdog_mode());
    line_out(&f);
}

/* A shot or a run (between its shots too): new shot commands are refused, never merged into it. */
static bool busy_or_running(void)
{
    if (shot_busy() || a.run_waiting || a.run_left) {
        say("ERR busy: a shot or a run is in progress (abort first)\r\n");
        return true;
    }
    return false;
}

static void cmd_gps(void)
{
    char b[700];
    fmt_t f;
    fmt_init(&f, b, sizeof b);
    gps_format_status(&f, hw_now());
    line_out(&f);
}

static void cmd_config(void)
{
    char b[600];
    fmt_t f;
    fmt_init(&f, b, sizeof b);
    fmt_str(&f, "CONFIG");
    fmt_kv_i32(&f, "arm_to_rly_us", ARM_TO_RLY_US);
    fmt_kv_i32(&f, "a2_at_us", CHECK_A2_AT_US);
    fmt_kv_i32(&f, "b_at_us", CHECK_B_AT_US);
    fmt_kv_i32(&f, "rly_to_gate_us", RLY_TO_GATE_US);
    fmt_kv_i32(&f, "rly_off_us", RLY_OFF_US);
    fmt_kv_i32(&f, "tune_on_us", TUNE_ON_US);
    fmt_kv_i32(&f, "cd_at_us", CHECK_CD_AT_US);
    fmt_kv_i32(&f, "blank2_off_us", BLANK2_OFF_US);
    fmt_kv_i32(&f, "record_start_us", RECORD_START_US);
    fmt_kv_u32(&f, "record_samples", RECORD_SAMPLES);
    fmt_kv_u32(&f, "fs_Hz", SAMPLE_HZ);
    fmt_kv_i32(&f, "xfer_high_mV", XFER_HIGH_MV);
    fmt_kv_i32(&f, "xfer_low_mV", XFER_LOW_MV);
    fmt_kv_i32(&f, "a2_min_mV", A2_MIN_STEP_MV);
    fmt_kv_i32(&f, "d_min_mV", D_MIN_STEP_MV);
    fmt_kv_i32(&f, "isense_abort_mV", ISENSE_ABORT_MV);
    fmt_kv_u32(&f, "guard_extra_us", GATE_GUARD_EXTRA_US);
    fmt_kv_fix(&f, "gamma_Hz_per_uT", GAMMA_HZ_PER_UT, 8);
    fmt_kv_u32(&f, "gps_baud", GPS_BAUD);
    line_out(&f);
}

static bool flag(char **tok, int n, int from, const char *name)
{
    for (int i = from; i < n; i++)
        if (strcmp(tok[i], name) == 0) return true;
    return false;
}

static void execute(char *line)
{
    char *tok[10];
    int n = 0;
    for (char *p = line; *p && n < 10;) {
        while (*p == ' ' || *p == '\t') *p++ = '\0';
        if (!*p) break;
        tok[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    if (n == 0) return;

    if (strcmp(tok[0], "help") == 0 || strcmp(tok[0], "?") == 0) cmd_help();
    else if (strcmp(tok[0], "status") == 0) cmd_status();
    else if (strcmp(tok[0], "config") == 0) cmd_config();
    else if (strcmp(tok[0], "abort") == 0) app_button();
    else if (strcmp(tok[0], "selftest") == 0) selftest();
    else if (strcmp(tok[0], "gps") == 0) {
        if (n >= 2 && strcmp(tok[1], "raw") == 0) {
            gps_echo(20u);
            say("GPS raw: echoing the next 20 RMC/GGA sentences\r\n");
        } else {
            cmd_gps();
        }
    }
    else if (strcmp(tok[0], "emu") == 0) {
        if (n < 2) { say("ERR usage: emu off | none | dead | no_release | k1_stuck | missed | no_pickup | drop | rev_c\r\n"); return; }
        if (shot_busy()) { say("ERR busy\r\n"); return; }
        if (plat_emu(tok[1]) != 0) { say("ERR unknown emulator mode\r\n"); return; }
        say(strcmp(tok[1], "off") == 0 ? "EMU off\r\n"
                                       : "EMU on - bench only: bare Nucleo, PA4 (CN7-32) jumpered to PB0 (CN7-34)\r\n");
    }
    else if (strcmp(tok[0], "vref") == 0) {
        if (shot_busy()) { say("ERR busy\r\n"); return; }
        a.vref_mv = plat_vref_mv();
        a.mv_per_code = (float)a.vref_mv / (float)ADC_FULL_SCALE;
        shot_set_mv_per_code(a.mv_per_code);
        cmd_status();
    } else if (strcmp(tok[0], "shot") == 0 || strcmp(tok[0], "dry") == 0) {
        shot_params_t p;
        if (busy_or_running()) return;
        if (n < 2 || parse_micro(tok[1], &p.t_pol_us) != 0) { say("ERR usage: shot <t_pol_s> [tuned] [dump]\r\n"); return; }
        p.tuned = flag(tok, n, 2, "tuned");
        p.dry = tok[0][0] == 'd';
        start_shot(&p, flag(tok, n, 2, "dump"));
    } else if (strcmp(tok[0], "run") == 0) {
        uint32_t count, gap_us = RUN_GAP_US;
        shot_params_t p;
        if (busy_or_running()) return;     /* checked before any run state is touched */
        if (n < 3 || parse_u32(tok[1], &count) != 0 || count == 0u || parse_micro(tok[2], &p.t_pol_us) != 0) {
            say("ERR usage: run <n> <t_pol_s> [tuned] [dump] [gap=<s>]\r\n");
            return;
        }
        for (int i = 3; i < n; i++)
            if (strncmp(tok[i], "gap=", 4) == 0 && (parse_micro(tok[i] + 4, &gap_us) != 0 || gap_us > RUN_GAP_MAX_US)) {
                say("ERR gap: 0 to 3600 s\r\n");
                return;
            }
        p.tuned = flag(tok, n, 3, "tuned");
        p.dry = false;
        a.run_gap_us = gap_us;
        a.run_p = p;
        a.run_dump = flag(tok, n, 3, "dump");
        a.missed_in_a_row = 0;
        if (start_shot(&p, a.run_dump)) a.run_left = count - 1u;
    } else {
        say("ERR unknown command (help)\r\n");
    }
}

void app_char(int c)
{
    if (c == '\r' || c == '\n') {
        a.line[a.line_len] = '\0';
        if (a.line_len) execute(a.line);
        a.line_len = 0;
    } else if (c == 8 || c == 127) {
        if (a.line_len) a.line_len--;
    } else if (a.line_len + 1u < sizeof a.line && c >= 32 && c < 127) {
        a.line[a.line_len++] = (char)c;
    }
}

void app_button(void)
{
    a.run_left = 0;
    a.run_waiting = false;
    if (shot_busy()) {
        shot_abort();
        say("ABORT requested\r\n");
    }
}

bool app_shot_running(void)
{
    return shot_busy();
}

void app_poll(void)
{
    shot_result_t r;
    if (shot_take_result(&r)) process(&r);
    if (a.run_waiting && !shot_busy()) {
        /* Count the rest down in microseconds: a TIM2 deadline would wrap for gaps of 12.6 s or more. */
        const uint32_t now = hw_now();
        const uint32_t d_us = (now - a.run_wait_last) / TICKS_PER_US;
        a.run_wait_last += d_us * TICKS_PER_US;
        a.run_wait_left_us = (d_us >= a.run_wait_left_us) ? 0u : a.run_wait_left_us - d_us;
        if (a.run_wait_left_us == 0u) {
            a.run_waiting = false;
            if (a.run_left) {
                a.run_left--;
                if (!start_shot(&a.run_p, a.run_dump)) {
                    say("RUN stopped: the next shot could not start\r\n");
                    a.run_left = 0;
                }
            }
        }
    }
}

void app_init(uint32_t vref_mv, const char *reset_cause)
{
    char b[200];
    fmt_t f;
    memset(&a, 0, sizeof a);
    a.vref_mv = vref_mv;
    a.mv_per_code = (float)vref_mv / (float)ADC_FULL_SCALE;
    shot_init(pre_buf, post_buf, rec_buf, edge_buf, a.mv_per_code);

    fmt_init(&f, b, sizeof b);
    fmt_str(&f, "\r\nMAGNETOMETER fw=" FW_VERSION);
    fmt_kv_str(&f, "clock", plat_clock_name());
    fmt_kv_u32(&f, "vref_mV", vref_mv);
    fmt_kv_str(&f, "reset", reset_cause);
    fmt_kv_str(&f, "silicon", plat_silicon());
    fmt_kv_str(&f, "watchdog", plat_watchdog_mode());
    line_out(&f);
    if (strcmp(plat_watchdog_mode(), "frozen_when_halted") == 0)
        say("DEBUG BUILD: a debugger halt freezes the watchdog - never halt or step with the 12 V supply on\r\n");
    say("type help\r\n");
}
