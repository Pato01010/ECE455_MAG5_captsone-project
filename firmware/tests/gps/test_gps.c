/* test_gps.c - app/gps.c on the PC: sentences in through the interrupt entry points, answers out.
 *
 * The simulated receiver sends a PPS every true second and then GGA and RMC for that second, the way a
 * u-blox does. The MCU clock runs fast by a known amount, and the counts start near 2^32 so the 25 s wrap
 * of TIM2 happens during the tests. Checked: the clock measurement, the UTC label and its arithmetic
 * (fractions, midnight, new year), the position fields, every rejection path (checksum, glitch, gap,
 * late or early RMC, no fix, stale data, clock change) and the interrupt-side queues; and the 2026-10-22
 * review's cases: a stale calibration after a gap, a late RMC, a glitch before its pulse's RMC, a main
 * loop blocked for 20 s.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "fmt.h"
#include "gps.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("    FAIL line %d: ", __LINE__); \
                              printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the platform ---- */
static char out[4096];
static size_t out_len;
void plat_write(const char *s, uint32_t n)
{
    if (out_len + n < sizeof out) { memcpy(out + out_len, s, n); out_len += n; out[out_len] = '\0'; }
}

/* ---- the simulated receiver ---- */
static double tps;              /* the MCU's TIM2 counts per true second */
static uint32_t t0;             /* TIM2 count of second 0's pulse */

static uint32_t at(double true_s) { return t0 + (uint32_t)llround(true_s * tps); }

/* From true second s0 on, the MCU clock runs at a new rate. */
static void rebase(double s0, double ppm)
{
    const uint32_t t_s0 = at(s0);
    tps = (double)TICK_HZ * (1.0 + ppm * 1e-6);
    t0 = t_s0 - (uint32_t)llround(s0 * tps);
}

static uint32_t now_t;          /* the last main-loop pass */
static void poll(uint32_t t) { now_t = t; gps_poll(t); }

/* "$" body "*HH\r\n" with the right checksum (or a wrong one) */
static void sentence(char *dst, size_t cap, const char *body, bool bad)
{
    unsigned sum = 0;
    for (const char *p = body; *p; p++) sum ^= (unsigned char)*p;
    if (bad) sum ^= 1u;
    snprintf(dst, cap, "$%s*%02X\r\n", body, sum);
}

static void send(const char *body, uint32_t t, bool bad)
{
    char s[128];
    sentence(s, sizeof s, body, bad);
    for (const char *p = s; *p; p++) gps_rx_isr(*p, t);
}

/* Second k of the simulation: UTC 2026-10-22 15:04:00 + k (unless base_sod is moved). */
static long base_day_sod = 15 * 3600 + 4 * 60;
static int base_dd = 22, base_mm = 10, base_yy = 26;

static void hms(long sod, char *dst)
{
    sod %= 86400;
    sprintf(dst, "%02ld%02ld%02ld.00", sod / 3600, sod / 60 % 60, sod % 60);
}

/* Days since 1970 <-> y/m/d (proleptic Gregorian), written independently of gps.c's */
static long days_of(int y, int m, int d)
{
    static const int before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    long days = 0;
    for (int yy = 1970; yy < y; yy++) days += (yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0)) ? 366 : 365;
    days += before[m - 1] + d - 1;
    if (m > 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) days++;
    return days;
}

static void ymd_of(long days, int *y, int *m, int *d)
{
    for (*y = 1970;; (*y)++) {
        const long len = days_of(*y + 1, 1, 1) - days_of(*y, 1, 1);
        if (days < len) break;
        days -= len;
    }
    for (*m = 1; *m < 12 && days >= days_of(*y, *m + 1, 1) - days_of(*y, 1, 1); (*m)++) {}
    *d = (int)(days - (days_of(*y, *m, 1) - days_of(*y, 1, 1))) + 1;
}

static void rmc_body(char *b, size_t cap, long k, char status, const char *lat, const char *ns, const char *lon,
                     const char *ew)
{
    char tm[16];
    int y, m, d;
    long sod = base_day_sod + k;
    ymd_of(days_of(2000 + base_yy, base_mm, base_dd) + sod / 86400, &y, &m, &d);   /* the date rolls over */
    hms(sod, tm);
    snprintf(b, cap, "GNRMC,%s,%c,%s,%s,%s,%s,2.430,271.4,%02d%02d%02d,,,A,V", tm, status, lat, ns, lon, ew, d,
             m, y % 100);
}

static void gga_body(char *b, size_t cap, long k, int quality)
{
    char tm[16];
    hms(base_day_sod + k, tm);
    snprintf(b, cap, "GNGGA,%s,4304.27407,N,08924.07407,W,%d,11,0.90,270.1,M,-33.9,M,,", tm, quality);
}

/* One second: the pulse, GGA at +150 ms and RMC at +200 ms, then the main loop at +900 ms. */
static void second(long k, bool pulse, bool fix)
{
    char b[160];
    if (pulse) gps_pps_isr(at((double)k));
    gga_body(b, sizeof b, k, fix ? 1 : 0);
    send(b, at((double)k + 0.15), false);
    rmc_body(b, sizeof b, k, fix ? 'A' : 'V', "4304.27407", "N", "08924.07407", "W");
    send(b, at((double)k + 0.20), false);
    poll(at((double)k + 0.90));
}

static void fresh_start(double ppm)
{
    tps = (double)TICK_HZ * (1.0 + ppm * 1e-6);
    t0 = 0xF0000000u;                   /* TIM2 wraps about 1.6 s in */
    gps_init(t0 - 5u * TICK_HZ);
    poll(t0 - 1u);
    out_len = 0;
    out[0] = '\0';
}

static double field(const char *line, const char *key)
{
    char want[64];
    snprintf(want, sizeof want, " %s=", key);
    const char *k = strstr(line, want);
    if (!k || strncmp(k + strlen(want), "na", 2) == 0) return NAN;
    return atof(k + strlen(want));
}

static const char *sfield(const char *line, const char *key, char *dst, size_t cap)
{
    char want[64];
    snprintf(want, sizeof want, " %s=", key);
    const char *k = strstr(line, want);
    dst[0] = '\0';
    if (k) {
        k += strlen(want);
        size_t n = strcspn(k, " ");
        if (n >= cap) n = cap - 1;
        memcpy(dst, k, n);
        dst[n] = '\0';
    }
    return dst;
}

static void fields_at(uint32_t t, char *dst, size_t cap)
{
    fmt_t f;
    fmt_init(&f, dst, (uint32_t)cap);
    gps_format_fields(&f, t);
}

static void status(char *dst, size_t cap)
{
    fmt_t f;
    fmt_init(&f, dst, (uint32_t)cap);
    gps_format_status(&f, now_t);
}

int main(void)
{
    char line[800], s[64];
    double k_tps;
    uint32_t span;
    int64_t whole;
    double frac;

    printf("GPS: sentences, pulses, UTC and the clock\n");

    /* 1. A healthy receiver, the MCU clock 12.5 ppm fast: 20 seconds */
    fresh_start(12.5);
    for (long k = 0; k < 20; k++) second(k, true, true);
    CHECK(gps_clock(at(19.5), &k_tps, &span), "no clock measurement after 20 pulses");
    CHECK(fabs((k_tps / TICK_HZ - 1.0) * 1e6 - 12.5) < 0.01, "clock %.4f ppm, expected 12.5", (k_tps / TICK_HZ - 1.0) * 1e6);
    CHECK(span == GPS_CLK_WINDOW_S, "span %u s, expected %u", span, GPS_CLK_WINDOW_S);
    CHECK(gps_utc(at(19.25), &whole, &frac), "no UTC after 20 labelled pulses");
    {
        const int64_t want = (int64_t)20748 * 86400 + base_day_sod + 19;   /* 2026-10-22 is day 20748 */
        CHECK(whole == want && fabs(frac - 0.25) < 1e-6, "UTC %lld + %.7f, expected %lld + 0.25", (long long)whole, frac,
              (long long)want);
    }
    fields_at(at(19.25), line, sizeof line);
    printf("  %s\n", line);
    CHECK(strcmp(sfield(line, "utc", s, sizeof s), "2026-10-22T15:04:19.250000Z") == 0, "utc %s", s);
    CHECK(field(line, "fix") == 1.0 && field(line, "sats") == 11.0, "fix / sats");
    CHECK(fabs(field(line, "hdop") - 0.90) < 1e-9, "hdop");
    CHECK(fabs(field(line, "lat") - (43.0 + 4.27407 / 60.0)) < 1e-7, "lat %.8f", field(line, "lat"));
    CHECK(fabs(field(line, "lon") + (89.0 + 24.07407 / 60.0)) < 1e-7, "lon %.8f (west is negative)", field(line, "lon"));
    CHECK(fabs(field(line, "alt_m") - 270.1) < 1e-9, "alt_m");
    CHECK(fabs(field(line, "spd_mps") - 2.430 * 0.514444) < 0.01, "spd_mps %.3f", field(line, "spd_mps"));
    CHECK(fabs(field(line, "cog_deg") - 271.4) < 1e-9, "cog_deg");
    CHECK(fabs(field(line, "pos_age_s") - 0.25) < 0.005, "pos_age_s %.3f (fix of second 19, event at 19.25)",
          field(line, "pos_age_s"));
    status(line, sizeof line);
    printf("  %s\n", line);
    CHECK(field(line, "pps") == 20.0 && field(line, "labels") == 19.0 && field(line, "utc_jumps") == 0.0,
          "status counts (the first RMC is a candidate, 19 confirm it): %s", line);
    CHECK(field(line, "lag_ms") == 200.0, "lag_ms %.0f", field(line, "lag_ms"));
    CHECK(fabs(field(line, "pps_age_s") - 0.90) < 0.005 && field(line, "fix") == 1.0, "status at +0.9 s: %s", line);
    CHECK(strcmp(sfield(line, "utc", s, sizeof s), "2026-10-22T15:04:19.900000Z") == 0, "status utc %s", s);
    CHECK(field(line, "rmc") == 20.0 && field(line, "gga") == 20.0 && field(line, "bad_sum") == 0.0, "sentence counts");

    /* 2. Before GPS_CLK_MIN_S seconds of pulses there is no correction; after it there is */
    fresh_start(-3.0);
    for (long k = 0; k < (long)GPS_CLK_MIN_S; k++) second(k, true, true);
    CHECK(!gps_clock(at(3.5), NULL, NULL), "a correction from %u s of pulses", GPS_CLK_MIN_S - 1u);
    second((long)GPS_CLK_MIN_S, true, true);
    CHECK(gps_clock(at(4.5), &k_tps, &span) && span == GPS_CLK_MIN_S, "no correction after %u s", GPS_CLK_MIN_S);
    CHECK(fabs((k_tps / TICK_HZ - 1.0) * 1e6 + 3.0) < 0.05, "clock %.4f ppm, expected -3", (k_tps / TICK_HZ - 1.0) * 1e6);

    /* 3. A missed pulse is bridged; a glitch is ignored; a long gap starts a new chain */
    fresh_start(5.0);
    for (long k = 0; k < 10; k++) second(k, k != 6, true);
    status(line, sizeof line);
    CHECK(field(line, "pps_missed") == 1.0 && field(line, "pps_bad") == 0.0, "a missed pulse: %s", line);
    CHECK(gps_utc(at(9.5), &whole, &frac) && whole % 86400 == base_day_sod + 9, "UTC across a missed pulse");
    gps_pps_isr(at(10.4));                              /* a glitch between pulses */
    poll(at(10.45));
    CHECK(gps_utc(at(10.45), &whole, &frac) && whole % 86400 == base_day_sod + 10 && fabs(frac - 0.45) < 1e-6,
          "UTC lost or moved by a glitch");
    CHECK(gps_clock(at(10.45), NULL, NULL), "the clock measurement was lost by a glitch (it should be kept)");
    for (long k = 11; k < 14; k++) second(k, true, true);
    status(line, sizeof line);
    CHECK(field(line, "pps_bad") == 1.0 && field(line, "pps_missed") == 2.0 && field(line, "labels") == 11.0,
          "a glitch (pulses 6 and 10 were never sent): %s", line);
    CHECK(gps_utc(at(13.5), &whole, &frac) && whole % 86400 == base_day_sod + 13, "UTC after a glitch");
    for (long k = 14; k < 40; k++) {                    /* 26 s of no pulses, fixes still coming */
        char b[160];
        rmc_body(b, sizeof b, k, 'A', "4304.27407", "N", "08924.07407", "W");
        send(b, at((double)k + 0.2), false);
        poll(at((double)k + 0.9));
    }
    CHECK(!gps_utc(at(39.5), &whole, &frac), "UTC extrapolated 26 s past the last pulse");
    second(40, true, true);
    second(41, true, true);
    status(line, sizeof line);
    CHECK(field(line, "pps_gaps") == 1.0, "a 27 s gap: pps_gaps %.0f", field(line, "pps_gaps"));
    CHECK(gps_utc(at(41.5), &whole, &frac) && whole % 86400 == base_day_sod + 41, "UTC after a long gap");

    /* 4. An RMC that is too late, or that comes before its pulse, names no pulse */
    fresh_start(0.0);
    {
        char b[160];
        gps_pps_isr(at(0.0));
        rmc_body(b, sizeof b, 0, 'A', "4304.27407", "N", "08924.07407", "W");
        send(b, at(0.97), false);                       /* 970 ms after the pulse */
        poll(at(0.98));
        CHECK(!gps_utc(at(0.98), &whole, &frac), "an RMC 970 ms after a pulse labelled it");
        rmc_body(b, sizeof b, 1, 'A', "4304.27407", "N", "08924.07407", "W");
        send(b, at(0.99), false);                       /* RMC(1) before pulse 1 */
        gps_pps_isr(at(1.0));
        poll(at(1.05));
        CHECK(!gps_utc(at(1.05), &whole, &frac), "an RMC before its pulse labelled the previous one");
        /* queued out of order: the line's '$' came first, the pulse was queued first: time order wins */
        gps_pps_isr(at(2.0));
        rmc_body(b, sizeof b, 2, 'A', "4304.27407", "N", "08924.07407", "W");
        send(b, at(2.1), false);
        poll(at(2.5));
        CHECK(!gps_utc(at(2.5), &whole, &frac), "UTC from a single RMC (two must agree)");
        /* a receiver at 5 Hz: RMC .00 names the pulse; .20 ... .80 must not (.60 would round to the next second) */
        gps_pps_isr(at(3.0));
        for (int i = 0; i < 5; i++) {
            char tm[16];
            hms(base_day_sod + 3, tm);
            tm[7] = (char)('0' + 2 * i);                /* hhmmss.00, .20, .40, .60, .80 */
            snprintf(b, sizeof b, "GNRMC,%s,A,4304.27407,N,08924.07407,W,2.430,271.4,221026,,,A,V", tm);
            send(b, at(3.05 + 0.2 * i), false);
        }
        poll(at(3.95));
        CHECK(gps_utc(at(3.95), &whole, &frac) && whole % 86400 == base_day_sod + 3 && fabs(frac - 0.95) < 1e-6,
              "UTC with 5 Hz RMCs: %lld + %.6f", (long long)(whole % 86400), frac);
        status(line, sizeof line);
        CHECK(field(line, "utc_jumps") == 0.0 && field(line, "labels") == 1.0, "5 Hz RMCs: %s", line);
    }

    /* 5. No fix: status V, quality 0. No UTC label, no position, fix=0, but sats reported */
    fresh_start(1.0);
    for (long k = 0; k < 8; k++) second(k, false, false);
    fields_at(at(7.5), line, sizeof line);
    printf("  %s\n", line);
    CHECK(field(line, "fix") == 0.0 && isnan(field(line, "lat")) && isnan(field(line, "utc")), "no fix: %s", line);
    CHECK(field(line, "sats") == 11.0, "sats with no fix");
    CHECK(!gps_clock(at(7.5), NULL, NULL), "a clock without pulses");

    /* 6. Bad checksums, other sentences, a sentence cut by a new '$', a sentence too long */
    fresh_start(0.0);
    {
        char b[160];
        gga_body(b, sizeof b, 0, 1);
        send(b, at(0.1), true);
        send("GNGSV,3,1,11,01,40,083,46,02,17,308,41,12,07,344,39,14,22,228,45", at(0.11), false);
        send("GNGSA,A,3,01,02,12,14,,,,,,,,,1.5,0.9,1.2,1", at(0.12), false);
        const char *cut = "$GNGGA,150400.00,4304.2";
        for (const char *p = cut; *p; p++) gps_rx_isr(*p, at(0.13));
        gga_body(b, sizeof b, 0, 1);
        send(b, at(0.14), false);                       /* the cut one is replaced by this one */
        for (int i = 0; i < 120; i++) gps_rx_isr(i == 0 ? '$' : 'A', at(0.15));
        gps_rx_isr('\n', at(0.15));
        gps_rx_isr('x', at(0.16));                      /* noise between sentences */
        poll(at(0.5));
        status(line, sizeof line);
        CHECK(field(line, "bad_sum") == 1.0 && field(line, "gga") == 1.0, "checksum: %s", line);
        CHECK(field(line, "skipped") == 2.0 && field(line, "too_long") == 1.0, "ISR counts: %s", line);
    }
    /* talkers other than GN, and the south / east quadrant */
    {
        char b[160];
        gps_pps_isr(at(1.0));
        snprintf(b, sizeof b, "GPGGA,150401.00,3351.0000,S,15112.0000,E,2,07,1.4,12.0,M,,M,,");
        send(b, at(1.1), false);
        snprintf(b, sizeof b, "GPRMC,150401.00,A,3351.0000,S,15112.0000,E,0.0,,221026,,,D");
        send(b, at(1.2), false);
        poll(at(1.5));
        fields_at(at(1.5), line, sizeof line);
        CHECK(fabs(field(line, "lat") + 33.85) < 1e-9 && fabs(field(line, "lon") - 151.2) < 1e-9, "S/E: %s", line);
        CHECK(field(line, "fix") == 2.0 && isnan(field(line, "cog_deg")), "DGPS fix, empty course: %s", line);
    }

    /* 7. The queues: a full sentence queue drops, a full pulse queue drops */
    fresh_start(0.0);
    {
        char b[160];
        for (int i = 0; i < 20; i++) { gga_body(b, sizeof b, 0, 1); send(b, at(0.1 + 0.01 * i), false); }
        for (int i = 0; i < 20; i++) gps_pps_isr(at(0.5 + 0.001 * i));
        poll(at(0.9));
        status(line, sizeof line);
        CHECK(field(line, "dropped") == 5.0 && field(line, "gga") == 15.0, "sentence queue: %s", line);
        CHECK(field(line, "pps_dropped") == 5.0 && field(line, "pps") == 15.0, "pulse queue: %s", line);
    }

    /* 8. Stale: positions older than GPS_FIX_MAX_AGE_S and clocks older than GPS_CLK_MAX_AGE_S are not used */
    fresh_start(2.0);
    for (long k = 0; k < 10; k++) second(k, true, true);
    for (long k = 10; k < 10 + 6; k++) poll(at((double)k + 0.9));
    fields_at(at(15.9), line, sizeof line);
    CHECK(isnan(field(line, "lat")) && field(line, "fix") == 0.0, "a 6 s old position reported: %s", line);
    CHECK(gps_clock(at(15.9), NULL, NULL), "the clock should still be fresh after 6 s");
    for (long k = 16; k < 10 + (long)GPS_CLK_MAX_AGE_S + 2; k++) poll(at((double)k + 0.9));
    CHECK(!gps_clock(at((double)GPS_CLK_MAX_AGE_S + 11.9), NULL, NULL), "a %u s old clock measurement applied",
          GPS_CLK_MAX_AGE_S + 2u);

    /* 9. The MCU clock is re-initialized: every pulse measurement is forgotten */
    fresh_start(7.0);
    for (long k = 0; k < 10; k++) second(k, true, true);
    gps_clock_changed();
    CHECK(!gps_clock(at(9.95), NULL, NULL) && !gps_utc(at(9.95), &whole, &frac), "measurements kept after a clock change");

    /* 10. Midnight on New Year's Eve, and rounding up into the next second */
    base_day_sod = 86400 - 5;  base_dd = 31;  base_mm = 12;  base_yy = 26;
    fresh_start(0.0);
    for (long k = 0; k < 6; k++) second(k, true, true);
    fields_at(at(5.5), line, sizeof line);
    CHECK(strcmp(sfield(line, "utc", s, sizeof s), "2027-01-01T00:00:00.500000Z") == 0, "midnight: utc %s", s);
    CHECK(fabs(field(line, "pos_age_s") - 0.5) < 0.005, "pos_age_s across midnight %.3f", field(line, "pos_age_s"));
    {
        fmt_t f;
        fmt_init(&f, line, sizeof line);
        gps_format_utc(&f, (int64_t)20819 * 86400 - 1, 0.9999996);   /* 2026-12-31T23:59:59.9999996 */
        CHECK(strcmp(line, "2027-01-01T00:00:00.000000Z") == 0, "rounding: %s", line);
        fmt_init(&f, line, sizeof line);
        gps_format_utc(&f, (int64_t)11016 * 86400 + 43200, 0.0);     /* 2000-02-29 (a leap day) noon */
        CHECK(strcmp(line, "2000-02-29T12:00:00.000000Z") == 0, "leap day: %s", line);
    }
    /* a day of the month below 13: RMC's date is ddmmyy, 050326 = 5 March 2026 */
    base_day_sod = 12 * 3600;  base_dd = 5;  base_mm = 3;  base_yy = 26;
    fresh_start(0.0);
    for (long k = 0; k < 3; k++) second(k, true, true);
    fields_at(at(2.5), line, sizeof line);
    CHECK(strcmp(sfield(line, "utc", s, sizeof s), "2026-03-05T12:00:02.500000Z") == 0, "ddmmyy: utc %s", s);
    base_day_sod = 15 * 3600 + 4 * 60;  base_dd = 22;  base_mm = 10;  base_yy = 26;

    /* 12. A calibration from before a gap must not lock the pulses out when the clock has moved meanwhile
     * (review 2026-10-22): calibrate at 0 ppm, 30 s with no pulses, then +30 ppm */
    fresh_start(0.0);
    for (long k = 0; k < 20; k++) second(k, true, true);
    for (long k = 20; k < 50; k++) poll(at((double)k + 0.9));
    rebase(50.0, 30.0);
    for (long k = 50; k < 75; k++) second(k, true, true);
    status(line, sizeof line);
    CHECK(gps_clock(at(74.9), &k_tps, &span) && fabs((k_tps / TICK_HZ - 1.0) * 1e6 - 30.0) < 0.05,
          "after a gap and a 30 ppm move: %s", line);
    CHECK(field(line, "pps_bad") == 0.0 && field(line, "pps_gaps") == 1.0, "gap counters: %s", line);
    CHECK(gps_utc(at(74.5), &whole, &frac) && whole % 86400 == base_day_sod + 74, "UTC after the gap");

    /* 13. A late RMC (1.1 s, so 0.1 s after the next pulse) takes the UTC away instead of moving it */
    fresh_start(0.0);
    for (long k = 0; k < 10; k++) second(k, true, true);
    {
        char b[160];
        gps_pps_isr(at(10.0));                          /* second 10: the pulse, no RMC on time */
        poll(at(10.9));
        gps_pps_isr(at(11.0));
        rmc_body(b, sizeof b, 10, 'A', "4304.27407", "N", "08924.07407", "W");
        send(b, at(11.1), false);                       /* RMC(10), late */
        poll(at(11.15));
        CHECK(!gps_utc(at(11.15), &whole, &frac) || whole % 86400 == base_day_sod + 11,
              "a late RMC moved the UTC by a second");
        rmc_body(b, sizeof b, 11, 'A', "4304.27407", "N", "08924.07407", "W");
        send(b, at(11.2), false);
        poll(at(11.9));
        second(12, true, true);
        CHECK(gps_utc(at(12.5), &whole, &frac) && whole % 86400 == base_day_sod + 12 && fabs(frac - 0.5) < 1e-6,
              "UTC after a late RMC");
        status(line, sizeof line);
        CHECK(field(line, "utc_jumps") == 1.0, "a late RMC: %s", line);
    }

    /* 14. A glitch after a pulse but before that pulse's RMC: the RMC still names the real pulse */
    fresh_start(0.0);
    for (long k = 0; k < 10; k++) second(k, true, true);
    {
        char b[160];
        gps_pps_isr(at(10.0));
        gps_pps_isr(at(10.1));                          /* e.g. gate-off on the PPS wire */
        rmc_body(b, sizeof b, 10, 'A', "4304.27407", "N", "08924.07407", "W");
        send(b, at(10.2), false);
        poll(at(10.5));
        CHECK(gps_utc(at(10.5), &whole, &frac) && whole % 86400 == base_day_sod + 10 && fabs(frac - 0.5) < 1e-6,
              "a glitch before the RMC moved the UTC (%lld + %.6f)", (long long)(whole % 86400), frac);
    }

    /* 15. The main loop held up 21 s (more than half the TIM2 wrap): time still moves forward */
    fresh_start(0.0);
    for (long k = 0; k < 5; k++) second(k, true, true);
    poll(at(25.9));
    status(line, sizeof line);
    CHECK(fabs(field(line, "pps_age_s") - 21.9) < 0.01, "after a 21 s stall: %s", line);

    /* 11. Raw echo: the next n queued sentences, verbatim */
    fresh_start(0.0);
    gps_echo(1);
    second(0, true, true);
    CHECK(strncmp(out, "NMEA $GNGGA,", 12) == 0 && strstr(out, "\r\n") && !strstr(out, "GNRMC"), "echo: %s", out);

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
