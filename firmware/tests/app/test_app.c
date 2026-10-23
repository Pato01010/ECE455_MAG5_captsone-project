/* test_app.c - the console end to end on the PC: commands in, report lines out.
 *
 * The real app.c, shot.c, xfer.c, fit.c and patha.c run on the simulated board of tests/shot, which
 * now also makes the record (a decay plus noise) and the comparator's edges. Checked: the command
 * parser, every report line and its units (A_mV, B_nT with the shielded-proton ratio, minus_fit_nT),
 * the fit and Path A through the whole chain, run and its stop after three missed transfers, abort,
 * and the dump framing. With a simulated GPS (a pulse every true second, GGA and RMC after it, the MCU
 * clock fast by a known amount): the corrected and raw frequencies, the SHOT line's UTC and position.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "config.h"
#include "fake_hw.h"
#include "gps.h"
#include "shot.h"

static char out[400000];
static size_t out_len;
static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("    FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- the platform ---- */
void plat_write(const char *s, uint32_t n)
{
    if (out_len + n < sizeof out) { memcpy(out + out_len, s, n); out_len += n; }
}
void plat_kick(void) {}
uint32_t plat_vref_mv(void) { return 3250u; }
const char *plat_clock_name(void) { return "HSE_crystal"; }
bool plat_clock_ready(void) { return true; }
const char *plat_silicon(void) { return "X"; }
const char *plat_watchdog_mode(void) { return "runs_when_halted"; }
int plat_emu(const char *mode) { (void)mode; return 0; }

/* ---- a simulated GPS: the pulse of true second k at t_first + k x tps, GGA and RMC 150 / 200 ms later ---- */
static struct {
    bool on;
    double tps;             /* TIM2 counts per true second: the MCU clock as the GPS sees it */
    uint32_t t_first;
    long next_k;
    long sod0;              /* UTC second of the day of pulse 0 (2026-10-22) */
    int64_t now_rel;        /* the test's time in counts since t_first: TIM2 alone wraps every 25 s */
    uint32_t now32;
} gsim;

static void gsim_send(const char *body, uint32_t t)
{
    char s[128];
    unsigned sum = 0;
    for (const char *p = body; *p; p++) sum ^= (unsigned char)*p;
    snprintf(s, sizeof s, "$%s*%02X\r\n", body, sum);
    for (const char *p = s; *p; p++) gps_rx_isr(*p, t);
}

/* Everything the GPS sent up to `until`, one second at a time, as the main loop would see it. */
static void gsim_to(uint32_t until)
{
    if (gsim.on) {
        gsim.now_rel += (int64_t)(uint32_t)(until - gsim.now32);    /* time only moves forward, < 25 s a step */
        gsim.now32 = until;
    }
    while (gsim.on) {
        const double k = (double)gsim.next_k;
        const int64_t rr = llround((k + 0.20) * gsim.tps);
        if (rr > gsim.now_rel) break;
        const uint32_t tp = gsim.t_first + (uint32_t)llround(k * gsim.tps);
        const uint32_t tg = gsim.t_first + (uint32_t)llround((k + 0.15) * gsim.tps), tr = gsim.t_first + (uint32_t)rr;
        char b[160];
        const long sod = gsim.sod0 + gsim.next_k;
        gps_pps_isr(tp);
        snprintf(b, sizeof b, "GNGGA,%02ld%02ld%02ld.00,4304.27407,N,08924.07407,W,1,11,0.90,270.1,M,-33.9,M,,",
                 sod / 3600, sod / 60 % 60, sod % 60);
        gsim_send(b, tg);
        snprintf(b, sizeof b, "GNRMC,%02ld%02ld%02ld.00,A,4304.27407,N,08924.07407,W,2.430,271.4,221026,,,A,V",
                 sod / 3600, sod / 60 % 60, sod % 60);
        gsim_send(b, tr);
        gps_poll(tr);
        gsim.next_k++;
    }
    gps_poll(until);
}

static void gsim_start(double ppm, double history_s)
{
    gsim.on = true;
    gsim.tps = (double)TICK_HZ * (1.0 + ppm * 1e-6);
    gsim.t_first = F.now - (uint32_t)llround(history_s * gsim.tps);
    gsim.next_k = 0;
    gsim.sod0 = 15L * 3600 + 4 * 60;
    gsim.now_rel = 0;
    gsim.now32 = gsim.t_first;
    gps_init(gsim.t_first - 1000u);
    gsim_to(F.now);
}

static void type(const char *cmd)
{
    for (const char *p = cmd; *p; p++) app_char(*p);
    app_char('\r');
}

/* Run the simulated board until nothing is scheduled, then let the main loop report. */
static void settle(void)
{
    for (int i = 0; i < 1000; i++) {
        while (fake_advance()) gsim_to(F.now);  /* the main loop keeps polling the GPS during a shot */
        gsim_to(F.now);                         /* and before app_poll */
        app_poll();
        if (!F.wake_set) {
            app_poll();
            if (!F.wake_set) return;
        }
    }
}

/* The text of key= on the first line starting with tag ("" if absent). */
static const char *text(const char *tag, const char *key, char *dst, size_t cap)
{
    char want[64];
    snprintf(want, sizeof want, " %s=", key);
    dst[0] = '\0';
    for (const char *line = out; line && *line;) {
        const char *eol = strchr(line, '\n');
        if (strncmp(line, tag, strlen(tag)) == 0) {
            const char *k = strstr(line, want);
            if (k && (!eol || k < eol)) {
                k += strlen(want);
                size_t n = strcspn(k, " \r\n");
                if (n >= cap) n = cap - 1;
                memcpy(dst, k, n);
                dst[n] = '\0';
                return dst;
            }
        }
        line = eol ? eol + 1 : NULL;
    }
    return dst;
}

/* The value of key= on the first line starting with tag, as a double; NAN if absent. */
static double field(const char *tag, const char *key)
{
    char want[64];
    snprintf(want, sizeof want, " %s=", key);
    for (const char *line = out; line && *line;) {
        const char *eol = strchr(line, '\n');
        if (strncmp(line, tag, strlen(tag)) == 0) {
            const char *k = strstr(line, want);
            if (k && (!eol || k < eol)) return atof(k + strlen(want));
        }
        line = eol ? eol + 1 : NULL;
    }
    return NAN;
}

/* Occurrences of needle in the whole output, binary dumps (with their zero bytes) included. */
static int count(const char *needle)
{
    const size_t n = strlen(needle);
    int c = 0;
    for (size_t i = 0; i + n <= out_len; i++)
        if (memcmp(out + i, needle, n) == 0) c++;
    return c;
}

static scenario_t sc;

static void reset(void)
{
    memset(&sc, 0, sizeof sc);
    sc.name = "app";
    xfer_model_rev_de(&sc.m);
    sc.i_sense_mv = 1200.0f;
    fake_reset(&sc, 0xF0000000u);
    gsim.on = false;
    gps_init(F.now);
    out_len = 0;
    out[0] = '\0';
    app_init(3250u, "power-on");
}

int main(void)
{
    const double f_true = 2128.875, gamma = GAMMA_HZ_PER_UT;

    printf("Console end to end\n");

    /* A tuned shot with a strong decay: every line, the fit and Path A */
    reset();
    sc.fid_amp = 735.0;  sc.fid_f_hz = f_true;  sc.fid_t2_s = 1.0;  sc.fid_phase = 0.7;
    sc.noise_codes = 23.0;  sc.hyst_codes = 8.0;
    type("shot 3 tuned");
    settle();
    out[out_len] = '\0';
    printf("%s", out);
    CHECK(count("SHOT id=1") == 1 && strstr(out, "result=ok"), "SHOT line / result");
    CHECK(fabs(field("FIT", "f_Hz") - f_true) < 0.01, "f_Hz %.6f", field("FIT", "f_Hz"));
    CHECK(fabs(field("FIT", "B_nT") - field("FIT", "f_Hz") / gamma * 1000.0) < 0.002, "B_nT not f / gamma");
    CHECK(fabs(field("FIT", "A_mV") - 735.0 * 3250.0 / 4095.0) < 10.0, "A_mV %.2f", field("FIT", "A_mV"));
    CHECK(fabs(field("FIT", "T2_s") - 1.0) < 0.05, "T2_s %.3f", field("FIT", "T2_s"));
    CHECK(fabs(field("FIT", "snr") - 735.0 / 23.0) < 2.0, "snr %.2f", field("FIT", "snr"));
    CHECK(field("FIT", "noise_density") > 0.7 && field("FIT", "noise_density") < 1.4,
          "white noise: noise_density %.2f", field("FIT", "noise_density"));
    CHECK(fabs(field("FIT", "fsig_Hz") / field("FIT", "fsig_white_Hz") - sqrt(field("FIT", "noise_density"))) < 0.02,
          "fsig_Hz is not fsig_white_Hz x sqrt(noise_density)");
    CHECK(field("PATHA", "status") == 0.0, "PATHA status");
    CHECK(fabs(field("PATHA", "f_Hz") - f_true) < 0.05, "PATHA f_Hz %.6f", field("PATHA", "f_Hz"));
    CHECK(fabs(field("PATHA", "minus_fit_nT") - (field("PATHA", "f_Hz") - field("FIT", "f_Hz")) / gamma * 1000.0) < 0.01,
          "minus_fit_nT");
    CHECK(field("TIME", "rly_off_us") == 500.0 && field("TIME", "blank2_off_us") == 45000.0, "TIME line");
    CHECK(field("TIME", "tpol_us") == 3000000.0, "tpol_us");
    CHECK(fabs(field("CHECK", "a2_mV") - 61.0) < 3.0, "a2_mV");
    CHECK(count("DONE id=1") == 1, "DONE line");
    {
        char t[64];
        CHECK(strcmp(text("SHOT", "utc", t, sizeof t), "na") == 0 && strcmp(text("SHOT", "clk_ppm", t, sizeof t), "na") == 0
              && strcmp(text("SHOT", "lat", t, sizeof t), "na") == 0 && field("SHOT", "fix") == 0.0,
              "no GPS: SHOT should say utc=na clk_ppm=na lat=na fix=0");
        CHECK(strcmp(text("SHOT", "utc_at", t, sizeof t), "rec0") == 0, "utc_at %s", t);
        CHECK(strcmp(text("FIT", "corr", t, sizeof t), "none") == 0 && field("FIT", "f_raw_Hz") == field("FIT", "f_Hz"),
              "no GPS: FIT corr=none and f_raw_Hz = f_Hz");
    }

    /* With a GPS, the MCU clock 25 ppm fast. The record is what a fast clock samples: the decay at f_true
     * looks like f_true / (1 + 25 ppm) on the nominal 20 kHz. FIT and PATHA must give f_true back, and
     * f_raw_Hz the nominal-clock value; SHOT carries the clock, the UTC of sample 0 and the position. */
    reset();
    gsim_start(25.0, 18.0);             /* 18 s of pulses before the shot; the whole test stays < 25 s */
    sc.fid_amp = 735.0;  sc.fid_f_hz = f_true / (1.0 + 25e-6);  sc.fid_t2_s = 1.0;  sc.fid_phase = 0.7;
    sc.noise_codes = 23.0;  sc.hyst_codes = 8.0;
    type("shot 3 tuned");
    settle();
    out[out_len] = '\0';
    printf("%s", out);
    {
        char t[64];
        const double f = field("FIT", "f_Hz"), raw = field("FIT", "f_raw_Hz");
        CHECK(strcmp(text("FIT", "corr", t, sizeof t), "gps") == 0, "FIT corr=%s, expected gps", t);
        CHECK(fabs(f / raw - 1.0 - 25e-6) < 1e-9, "f_Hz / f_raw_Hz - 1 = %.4f ppm, expected 25", (f / raw - 1.0) * 1e6);
        CHECK(fabs(f - f_true) < 0.002, "corrected f_Hz %.6f, true %.6f", f, f_true);
        CHECK(fabs(raw - sc.fid_f_hz) < 0.002, "f_raw_Hz %.6f, nominal-clock %.6f", raw, sc.fid_f_hz);
        CHECK(fabs(field("FIT", "B_nT") - f / gamma * 1000.0) < 0.002, "B_nT not from the corrected f");
        CHECK(fabs(field("PATHA", "f_Hz") - f_true) < 0.05, "PATHA f_Hz %.6f not corrected", field("PATHA", "f_Hz"));
        CHECK(fabs(field("PATHA", "minus_fit_nT") - (field("PATHA", "f_Hz") - f) / gamma * 1000.0) < 0.01,
              "PATHA minus_fit_nT");
        CHECK(fabs(field("SHOT", "clk_ppm") - 25.0) < 0.01 && field("SHOT", "clk_s") == 16.0,
              "SHOT clk_ppm %.3f clk_s %.0f", field("SHOT", "clk_ppm"), field("SHOT", "clk_s"));
        CHECK(field("SHOT", "fix") == 1.0 && fabs(field("SHOT", "lat") - 43.0712345) < 1e-7
              && fabs(field("SHOT", "lon") + 89.4012345) < 1e-7, "SHOT position");
        /* UTC of sample 0, from the simulated receiver's own time line */
        const double want = (double)gsim.sod0 + (double)(uint32_t)(F.rt0 - gsim.t_first) / gsim.tps;
        text("SHOT", "utc", t, sizeof t);
        double got = NAN;
        if (strlen(t) == 27 && strncmp(t, "2026-10-22T", 11) == 0)
            got = atof(t + 11) * 3600.0 + atof(t + 14) * 60.0 + atof(t + 17);
        CHECK(fabs(got - want) < 2e-6, "SHOT utc %s: %.6f s of the day, expected %.6f", t, got, want);
    }

    /* An aborted shot is stamped at gate-off, or at arming when the gate never came on */
    {
        char t[64];
        reset();
        gsim_start(0.0, 10.0);
        sc.overcurrent_at_ms = 100.0f;
        F.oc_pending = true;                /* fake_reset read the scenario before this was set */
        type("shot 2");
        settle();
        CHECK(strstr(out, "reason=overcurrent") && strcmp(text("SHOT", "utc_at", t, sizeof t), "gate_off") == 0
              && strncmp(text("SHOT", "utc", t, sizeof t), "2026-10-22T", 11) == 0, "overcurrent: utc_at / utc");
        reset();
        gsim_start(0.0, 10.0);
        sc.m.xfer_dead = true;
        type("shot 2");
        settle();
        CHECK(strstr(out, "reason=xfer_dead_a") && strcmp(text("SHOT", "utc_at", t, sizeof t), "arm") == 0
              && strncmp(text("SHOT", "utc", t, sizeof t), "2026-10-22T", 11) == 0, "xfer dead: utc_at / utc");
    }
    reset();
    gsim_start(25.0, 18.0);

    /* The gps command, and gps raw */
    out_len = 0;
    type("gps");
    type("gps raw");
    out[out_len] = '\0';
    CHECK(strncmp(out, "GPS utc=2026-10-22T", 19) == 0 && strstr(out, " clk_ppm=25.000"), "gps: %s", out);
    CHECK(strstr(out, "GPS raw: echoing") != NULL, "gps raw");
    F.now += 1500000u * TICKS_PER_US;
    gsim_to(F.now);
    out[out_len] = '\0';
    CHECK(count("NMEA $GNGGA,") >= 1 && count("NMEA $GNRMC,") >= 1, "gps raw echo: %s", out);

    /* Untuned: Path A is skipped */
    reset();
    sc.fid_amp = 735.0;  sc.fid_f_hz = f_true;  sc.fid_t2_s = 1.0;  sc.noise_codes = 23.0;  sc.hyst_codes = 8.0;
    type("shot 2");
    settle();
    CHECK(strstr(out, "PATHA id=1 skipped=untuned") != NULL, "untuned shot: PATHA should be skipped");

    /* A signal generator on the bench: a steady sine pins the decay at its floor (FIT_E_DECAY). The shot is
     * no_fid, but the frequency is good and Path A still runs (BRINGUP step 8b). */
    reset();
    sc.fid_amp = 600.0;  sc.fid_f_hz = 2000.0;  sc.fid_t2_s = 1e9;  sc.noise_codes = 5.0;  sc.hyst_codes = 8.0;
    type("shot 0.2 tuned");
    settle();
    CHECK(strstr(out, "result=no_fid") != NULL, "a steady sine should be no_fid");
    CHECK(field("FIT", "status") == -4.0, "steady sine: FIT status %.0f, expected -4", field("FIT", "status"));
    CHECK(fabs(field("FIT", "f_Hz") - 2000.0) < 0.001, "steady sine: f_Hz %.6f", field("FIT", "f_Hz"));
    CHECK(field("PATHA", "status") == 0.0 && fabs(field("PATHA", "f_Hz") - 2000.0) < 0.01,
          "steady sine: PATHA should run (f %.6f)", field("PATHA", "f_Hz"));

    /* No decay in the record: every check passes, but the shot is no_fid */
    reset();
    sc.noise_codes = 23.0;
    type("shot 2");
    settle();
    CHECK(strstr(out, "result=no_fid") != NULL, "a record of noise should be no_fid");

    /* A run with K1's NC contact making late (after the +40 ms check), but working again by the next shot:
     * three missed transfers in a row, then the run stops. (A contact that never makes again is caught
     * by the next shot's check (a) instead, and that abort stops the run too.) */
    reset();
    sc.m.nc_make_ms = 45.0f;
    type("run 5 0.05");
    for (int i = 0; i < 20; i++) {
        settle();
        F.now += 1000000u * TICKS_PER_US;      /* let the 1 s gap pass */
        app_poll();
    }
    CHECK(count("reason=missed_transfer_c") == 3, "missed transfers %d, expected 3", count("reason=missed_transfer_c"));
    CHECK(strstr(out, "RUN stopped: 3 missed transfers") != NULL, "run did not stop");
    if (count("reason=missed_transfer_c") != 3) { out[out_len] = '\0'; printf("%s", out); }

    /* A dump: the header, exactly the advertised bytes, END and DONE */
    reset();
    sc.fid_amp = 300.0;  sc.fid_f_hz = 1500.0;  sc.fid_t2_s = 0.5;  sc.noise_codes = 10.0;
    type("shot 0.05 dump");
    settle();
    {
        const char *h = strstr(out, "DUMP id=1 bytes=");
        CHECK(h != NULL, "no DUMP header");
        if (h) {
            long bytes = atol(h + strlen("DUMP id=1 bytes="));
            const char *data = strstr(h, "\r\n");
            CHECK(bytes == 18800 * 2 + (long)F.n_edges * 4 + (long)field("DUMP", "pre") * 2 + 800 * 2,
                  "dump bytes %ld", bytes);
            if (data && data + 2 + bytes + 2 < out + out_len)
                CHECK(strncmp(data + 2 + bytes, "\r\nEND id=1", 10) == 0, "END not right after the data");
        }
        CHECK(count("DONE id=1") == 1, "DONE after the dump");
    }

    /* A run in progress: new shot commands are refused and change nothing, in a shot or in a gap */
    reset();
    type("run 3 0.05");                 /* one shot running, two to go */
    type("run 5 4 gap=0");
    type("shot 2");
    CHECK(count("ERR busy") == 2, "commands during a run: %d busy errors, expected 2", count("ERR busy"));
    for (int i = 0; i < 20; i++) {
        settle();
        if (i == 0) { type("run 9 4"); }    /* now in the 1 s gap */
        F.now += 1000000u * TICKS_PER_US;
        app_poll();
    }
    CHECK(count("ERR busy") == 3, "a run command in the gap was not refused");
    CHECK(count("DONE id=") == 3, "the run made %d shots, expected 3", count("DONE id="));
    CHECK(count("tpol_s=4.000") == 0, "a refused command changed the run's t_pol");

    /* A long gap is honoured (a TIM2 deadline would have wrapped at 12.6 s) */
    reset();
    type("run 2 0.05 gap=30");
    settle();
    for (int s = 0; s < 29; s++) {      /* 29 s of 1 s steps: the second shot must not start */
        F.now += 1000000u * TICKS_PER_US;
        app_poll();
        settle();
    }
    CHECK(count("DONE id=") == 1, "the second shot started before the 30 s gap ended");
    for (int s = 0; s < 3; s++) {
        F.now += 1000000u * TICKS_PER_US;
        app_poll();
        settle();
    }
    CHECK(count("DONE id=") == 2, "the second shot did not start after the 30 s gap (%d shots)", count("DONE id="));

    /* Bad commands */
    reset();
    type("shot");
    type("shot 9");
    type("run 0 2");
    type("run 2 1 gap=4000");
    type("frobnicate");
    CHECK(count("ERR") == 5, "5 errors expected, got %d", count("ERR"));

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
