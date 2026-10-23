/* gps.c - NMEA position and UTC, and the clock measured against the 1 PPS. See gps.h.
 *
 * Portable C99, no heap. Times are TIM2 counts extended to 64 bits ("uptime"), so the 25 s wrap of the
 * 32-bit count never matters inside this file: every count handed in is at most a few seconds old.
 */
#include "gps.h"

#include <math.h>
#include <string.h>

#include "app.h"
#include "config.h"

/* An interrupt publishes a queue entry only after writing it. On a single-core Cortex-M4 (and in the PC
 * tests, which have no interrupts) a compiler barrier is all that ordering needs. */
#define BARRIER()       __asm__ __volatile__("" ::: "memory")

#define LINE_MAX        96u     /* NMEA allows 82 characters, '$' and CR LF included */
#define LINE_SLOTS      16u     /* 7.5 s of RMC + GGA: a dump at 115,200 baud blocks the loop ~5 s */
#define PPS_SLOTS       16u
#define WIN_POINTS      (GPS_CLK_WINDOW_S + 1u)
#define MAX_FIELDS      20
#define UP_START        ((uint64_t)1 << 40)     /* uptime origin: counts before gps_init stay positive */
#define KNOT_MPS        0.514444444444

/* ---- Interrupt side ------------------------------------------------------------------------------ */
static struct {
    char cur[LINE_MAX];                 /* the sentence being received (ISR only) */
    uint32_t cur_len, cur_t;
    bool in_sentence;
    char line[LINE_SLOTS][LINE_MAX];
    uint32_t line_len[LINE_SLOTS], line_t[LINE_SLOTS];
    volatile uint32_t l_head, l_tail;   /* head written by the ISR, tail by the main loop */
    uint32_t pps[PPS_SLOTS];
    volatile uint32_t p_head, p_tail;
    volatile uint32_t dropped;          /* RMC/GGA lost: the queue was full */
    volatile uint32_t too_long, skipped, pps_dropped, pps_lost;
} q;

/* Only RMC and GGA are queued: "$ttRMC" / "$ttGGA" for any two-letter talker (GN, GP, GA, GB ...). */
static bool wanted(const char *s, uint32_t n)
{
    if (n < 6u || s[0] != '$') return false;
    return (s[3] == 'R' && s[4] == 'M' && s[5] == 'C') || (s[3] == 'G' && s[4] == 'G' && s[5] == 'A');
}

void gps_rx_isr(char c, uint32_t now)
{
    if (c == '$') {                     /* a new sentence, even if the last one never ended */
        q.in_sentence = true;
        q.cur_len = 0;
        q.cur_t = now;
    }
    if (!q.in_sentence || c == '\r') return;
    if (c == '\n') {
        q.in_sentence = false;
        if (!wanted(q.cur, q.cur_len)) { q.skipped++; return; }
        const uint32_t h = q.l_head, next = (h + 1u) % LINE_SLOTS;
        if (next == q.l_tail) { q.dropped++; return; }
        memcpy(q.line[h], q.cur, q.cur_len);
        q.line_len[h] = q.cur_len;
        q.line_t[h] = q.cur_t;
        BARRIER();
        q.l_head = next;
        return;
    }
    if (q.cur_len >= LINE_MAX) {
        q.in_sentence = false;
        q.too_long++;
        return;
    }
    q.cur[q.cur_len++] = c;
}

void gps_pps_isr(uint32_t t)
{
    const uint32_t h = q.p_head, next = (h + 1u) % PPS_SLOTS;
    if (next == q.p_tail) { q.pps_dropped++; return; }
    q.pps[h] = t;
    BARRIER();
    q.p_head = next;
}

void gps_pps_lost_isr(void)
{
    q.pps_lost++;
}

/* ---- Main-loop state ----------------------------------------------------------------------------- */
static struct {
    uint64_t up;                /* TIM2 counts since gps_init, plus UP_START */
    uint32_t last_now;

    /* pulses */
    bool have_pps;
    uint32_t pps_t;             /* TIM2 count of the last pulse */
    uint64_t pps_up;
    int64_t sec;                /* the last pulse's second, counted along the current chain */
    uint64_t win_up[WIN_POINTS];
    int64_t win_sec[WIN_POINTS];
    uint32_t win_n;
    uint32_t bad_run;           /* edges in a row that were not part of the chain */
    bool chain_cal;             /* the calibration below was measured on the current chain */
    bool have_cal;
    double cal_tps;             /* TIM2 counts per true second */
    uint64_t cal_up;
    uint32_t cal_span;
    uint32_t n_pps, pps_bad, pps_gaps, pps_missed;

    /* UTC: base_utc is the UTC second of chain second base_sec; cand_* is a label waiting for a second one */
    bool base_valid, cand_valid;
    int64_t base_utc, base_sec, cand_utc, cand_sec;
    uint32_t lag_us, labels, utc_jumps;

    /* the latest of each sentence */
    bool gga_have;
    uint64_t gga_up;
    uint32_t gga_quality, gga_sats;
    bool gga_has_sats, gga_has_hdop, gga_has_alt;
    double gga_hdop, gga_alt;

    bool rmc_have;
    uint64_t rmc_up;
    char rmc_status, rmc_mode;
    bool rmc_has_speed, rmc_has_course;
    double rmc_speed_mps, rmc_course;

    bool pos_have;              /* from whichever of RMC (status A) and GGA (quality > 0) came last */
    uint64_t pos_up;
    double pos_lat, pos_lon, pos_sod;   /* sod: the sentence's UTC second of the day */

    uint32_t n_rmc, n_gga, bad_sum, echo_left;
} g;

/* A TIM2 count as uptime, for events as they are processed: a count after the last one seen moves
 * uptime forward. gps_poll runs every main-loop pass, so no count is ever 12.6 s (2^31) away. */
static uint64_t up_at(uint32_t t)
{
    if ((int32_t)(t - g.last_now) > 0) {
        g.up += (uint32_t)(t - g.last_now);
        g.last_now = t;
    }
    return g.up - (uint32_t)(g.last_now - t);
}

/* The same for a query: never moves anything. */
static uint64_t up_of(uint32_t t)
{
    return g.up + (uint64_t)(int64_t)(int32_t)(t - g.last_now);
}

static uint64_t up_diff(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

static bool fresh(bool have, uint64_t when, uint64_t ref, uint32_t max_s)
{
    return have && up_diff(when, ref) <= (uint64_t)max_s * TICK_HZ;
}

void gps_init(uint32_t now)
{
    memset(&q, 0, sizeof q);
    memset(&g, 0, sizeof g);
    g.up = UP_START;
    g.last_now = now;
}

void gps_clock_changed(void)
{
    g.have_pps = false;
    g.win_n = 0;
    g.chain_cal = false;
    g.have_cal = false;
    g.base_valid = false;
    g.cand_valid = false;
}

void gps_echo(uint32_t n)
{
    g.echo_left = n;
}

/* ---- Pulses -------------------------------------------------------------------------------------- */
/* A pulse of the chain: the clock window, and the clock once it spans GPS_CLK_MIN_S. */
static void chain_pulse(uint32_t t, uint64_t u)
{
    g.have_pps = true;
    g.pps_t = t;
    g.pps_up = u;
    if (g.win_n == WIN_POINTS) {
        memmove(&g.win_up[0], &g.win_up[1], (WIN_POINTS - 1u) * sizeof g.win_up[0]);
        memmove(&g.win_sec[0], &g.win_sec[1], (WIN_POINTS - 1u) * sizeof g.win_sec[0]);
        g.win_n--;
    }
    g.win_up[g.win_n] = u;
    g.win_sec[g.win_n] = g.sec;
    g.win_n++;
    const int64_t span = g.win_sec[g.win_n - 1u] - g.win_sec[0];
    if (span >= (int64_t)GPS_CLK_MIN_S) {
        g.cal_tps = (double)(g.win_up[g.win_n - 1u] - g.win_up[0]) / (double)span;
        g.cal_span = (uint32_t)span;
        g.cal_up = u;
        g.have_cal = true;
        g.chain_cal = true;
    }
}

/* A new chain from this edge: seconds and UTC start again. The last calibration stays usable for the
 * correction (GPS_CLK_MAX_AGE_S), but the new chain is checked against nominal until it has its own:
 * an old measurement must never lock the pulses out. */
static void new_chain(uint32_t t, uint64_t u)
{
    g.win_n = 0;
    g.sec = 0;
    g.bad_run = 0;
    g.chain_cal = false;
    g.base_valid = false;
    g.cand_valid = false;
    chain_pulse(t, u);
}

static void on_pps(uint32_t t)
{
    const uint64_t u = up_at(t);

    g.n_pps++;
    if (!g.have_pps) { new_chain(t, u); return; }
    if (u <= g.pps_up) { g.pps_bad++; return; }         /* not after the last pulse: cannot happen */
    const double d = (double)(u - g.pps_up);
    const double est = g.chain_cal ? g.cal_tps : (double)TICK_HZ;
    const double tol_ppm = g.chain_cal ? GPS_PPS_TOL_CAL_PPM : GPS_PPS_TOL_PPM;
    const double n = floor(d / est + 0.5);
    if (n > (double)GPS_PPS_MAX_GAP_S) {
        g.pps_gaps++;
        new_chain(t, u);
        return;
    }
    if (n < 1.0 || fabs(d - n * est) > tol_ppm * 1e-6 * n * (double)TICK_HZ) {
        g.pps_bad++;                                    /* a glitch: ignored */
        if (++g.bad_run >= GPS_PPS_RESYNC) new_chain(t, u);   /* or the chain started on one */
        return;
    }
    g.bad_run = 0;
    g.sec += (int64_t)n;
    g.pps_missed += (uint32_t)n - 1u;
    chain_pulse(t, u);
}

/* ---- Sentences ----------------------------------------------------------------------------------- */
/* A decimal like "123.456" or "-1.5". False for an empty field or anything else. */
static bool parse_num(const char *s, double *out)
{
    double v = 0.0, scale = 1.0;
    bool neg = false, digits = false, dot = false;
    if (*s == '-' || *s == '+') neg = (*s++ == '-');
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            const int dgt = *s - '0';
            digits = true;
            if (dot) { scale *= 0.1; v += dgt * scale; }
            else v = v * 10.0 + dgt;
        } else if (*s == '.' && !dot) {
            dot = true;
        } else {
            return false;
        }
    }
    if (!digits) return false;
    *out = neg ? -v : v;
    return true;
}

static bool two_digits(const char *s, uint32_t *out)
{
    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') return false;
    *out = (uint32_t)(s[0] - '0') * 10u + (uint32_t)(s[1] - '0');
    return true;
}

/* "hhmmss" or "hhmmss.ss": seconds of the day, and whether it is a whole second. */
static bool parse_time(const char *s, double *sod, bool *whole)
{
    uint32_t hh, mm, ss;
    double frac = 0.0;
    if (!two_digits(s, &hh) || !two_digits(s + 2, &mm) || !two_digits(s + 4, &ss)) return false;
    if (hh > 23u || mm > 59u || ss > 60u) return false;
    if (s[6] != '\0' && (s[6] != '.' || !parse_num(s + 6, &frac))) return false;
    *sod = (double)(hh * 3600u + mm * 60u + ss) + frac;
    *whole = frac < 0.0005;
    return true;
}

/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil). */
static int64_t days_from_civil(int64_t y, uint32_t m, uint32_t d)
{
    y -= m <= 2u;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);
    const uint32_t mp = m > 2u ? m - 3u : m + 9u;
    const uint32_t doy = (153u * mp + 2u) / 5u + d - 1u;
    const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, uint32_t *m, uint32_t *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const uint32_t mp = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp < 10u ? mp + 3u : mp - 9u;
    *y = (int64_t)yoe + era * 400 + (*m <= 2u);
}

/* "ddmmyy" -> days since 1970 (years 2000-2099) */
static bool parse_date(const char *s, int64_t *days)
{
    uint32_t dd, mo, yy;
    if (!two_digits(s, &dd) || !two_digits(s + 2, &mo) || !two_digits(s + 4, &yy) || s[6] != '\0') return false;
    if (dd < 1u || dd > 31u || mo < 1u || mo > 12u) return false;
    *days = days_from_civil(2000 + (int64_t)yy, mo, dd);
    return true;
}

/* "ddmm.mmmm" with "N"/"S" (max_deg 90), or "dddmm.mmmm" with "E"/"W" (max_deg 180) -> signed degrees */
static bool parse_coord(const char *v, const char *hemi, double max_deg, double *out)
{
    double x;
    if (!parse_num(v, &x) || x < 0.0) return false;
    const double deg = floor(x / 100.0), min = x - deg * 100.0;
    if (min >= 60.0 || deg + min / 60.0 > max_deg) return false;
    double r = deg + min / 60.0;
    if (hemi[0] == 'S' || hemi[0] == 'W') r = -r;
    else if (hemi[0] != 'N' && hemi[0] != 'E') return false;
    *out = r;
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* An RMC names the last pulse's second. Two on different pulses must agree before the chain has UTC; one
 * that disagrees with the chain's UTC removes it until two agree again. */
static void label_pps(int64_t utc, uint64_t u_arrival)
{
    if (!g.have_pps || u_arrival <= g.pps_up
        || u_arrival - g.pps_up > (uint64_t)GPS_LAG_MAX_US * TICKS_PER_US)
        return;
    g.lag_us = (uint32_t)((u_arrival - g.pps_up) / TICKS_PER_US);
    if (g.base_valid) {
        if (g.base_utc + (g.sec - g.base_sec) == utc) { g.labels++; return; }
        g.utc_jumps++;
        g.base_valid = false;
    } else if (g.cand_valid && g.cand_sec != g.sec && g.cand_utc + (g.sec - g.cand_sec) == utc) {
        g.base_utc = utc;
        g.base_sec = g.sec;
        g.base_valid = true;
        g.cand_valid = false;
        g.labels++;
        return;
    }
    g.cand_utc = utc;
    g.cand_sec = g.sec;
    g.cand_valid = true;
}

static void set_pos(double lat, double lon, double sod, uint64_t u)
{
    g.pos_have = true;
    g.pos_lat = lat;
    g.pos_lon = lon;
    g.pos_sod = sod;
    g.pos_up = u;
}

/* $xxRMC,time,status,lat,N/S,lon,E/W,speed_kn,course,date,magvar,E/W,mode[,navstatus] */
static void on_rmc(char **fl, int n, uint64_t u)
{
    double sod, lat, lon, v;
    bool whole, have_time;
    int64_t days;

    g.n_rmc++;
    g.rmc_have = true;
    g.rmc_up = u;
    g.rmc_status = n > 2 ? fl[2][0] : 'V';
    g.rmc_mode = n > 12 ? fl[12][0] : '\0';
    g.rmc_has_speed = g.rmc_has_course = false;
    if (g.rmc_status != 'A') return;

    have_time = n > 1 && parse_time(fl[1], &sod, &whole);
    if (have_time && n > 6 && parse_coord(fl[3], fl[4], 90.0, &lat) && parse_coord(fl[5], fl[6], 180.0, &lon))
        set_pos(lat, lon, sod, u);
    if (n > 7 && parse_num(fl[7], &v) && v >= 0.0) { g.rmc_speed_mps = v * KNOT_MPS; g.rmc_has_speed = true; }
    if (n > 8 && parse_num(fl[8], &v) && v >= 0.0 && v < 360.0) { g.rmc_course = v; g.rmc_has_course = true; }
    if (have_time && whole && n > 9 && parse_date(fl[9], &days))
        label_pps(days * 86400 + (int64_t)floor(sod + 0.5), u);
}

/* $xxGGA,time,lat,N/S,lon,E/W,quality,sats,hdop,alt,M,sep,M,age,station */
static void on_gga(char **fl, int n, uint64_t u)
{
    double sod, lat, lon, v;
    bool whole;

    g.n_gga++;
    g.gga_have = true;
    g.gga_up = u;
    g.gga_quality = (n > 6 && parse_num(fl[6], &v) && v >= 0.0 && v <= 9.0) ? (uint32_t)v : 0u;
    g.gga_has_sats = n > 7 && parse_num(fl[7], &v) && v >= 0.0 && v < 100.0;
    if (g.gga_has_sats) g.gga_sats = (uint32_t)v;
    g.gga_has_hdop = n > 8 && parse_num(fl[8], &g.gga_hdop) && g.gga_hdop >= 0.0;
    g.gga_has_alt = g.gga_quality > 0u && n > 9 && parse_num(fl[9], &g.gga_alt);
    if (g.gga_quality > 0u && n > 5 && parse_time(fl[1], &sod, &whole)
        && parse_coord(fl[2], fl[3], 90.0, &lat) && parse_coord(fl[4], fl[5], 180.0, &lon))
        set_pos(lat, lon, sod, u);
}

static void on_line(char *s, uint32_t len, uint32_t t)
{
    char *fl[MAX_FIELDS];
    int n = 0;
    uint32_t i, sum = 0;
    const uint64_t u = up_at(t);

    s[len] = '\0';
    if (g.echo_left) {
        g.echo_left--;
        plat_write("NMEA ", 5u);
        plat_write(s, len);
        plat_write("\r\n", 2u);
    }
    for (i = 1u; i < len && s[i] != '*'; i++) sum ^= (uint8_t)s[i];
    if (i + 3u != len) { g.bad_sum++; return; }         /* no '*', or not exactly two digits after it */
    const int hi = hex_digit(s[i + 1u]), lo = hex_digit(s[i + 2u]);
    if (hi < 0 || lo < 0 || (uint32_t)(hi * 16 + lo) != sum) { g.bad_sum++; return; }
    s[i] = '\0';

    fl[n++] = s;
    for (char *p = s; *p && n < MAX_FIELDS; p++)
        if (*p == ',') { *p = '\0'; fl[n++] = p + 1; }
    for (int k = n; k < MAX_FIELDS; k++) fl[k] = s + i;   /* missing fields read as empty */

    if (fl[0][3] == 'R') on_rmc(fl, n, u);
    else on_gga(fl, n, u);
}

void gps_poll(uint32_t now)
{
    char line[LINE_MAX + 1u];
    /* now is the newest count: move uptime to it, unless an event processed just before was captured a
     * moment after it was read (then it is at most a few microseconds behind) */
    const uint32_t d = now - g.last_now;
    if (d < 0xFF000000u) {                              /* forward, up to 25 s */
        g.up += d;
        g.last_now = now;
    }
    for (;;) {
        /* lines first: a pulse captured before a queued line's '$' is then always seen with it */
        const bool hl = q.l_tail != q.l_head;
        const bool hp = q.p_tail != q.p_head;
        if (!hp && !hl) break;
        BARRIER();
        const uint32_t tp = hp ? q.pps[q.p_tail] : 0u, tl = hl ? q.line_t[q.l_tail] : 0u;
        if (hp && (!hl || (int32_t)(tp - tl) <= 0)) {
            q.p_tail = (q.p_tail + 1u) % PPS_SLOTS;
            on_pps(tp);
        } else {
            const uint32_t len = q.line_len[q.l_tail];
            memcpy(line, q.line[q.l_tail], len);
            BARRIER();
            q.l_tail = (q.l_tail + 1u) % LINE_SLOTS;
            on_line(line, len, tl);
        }
    }
}

/* ---- Queries ------------------------------------------------------------------------------------- */
bool gps_clock(uint32_t t, double *ticks_per_s, uint32_t *span_s)
{
    if (!g.have_cal || !fresh(true, g.cal_up, up_of(t), GPS_CLK_MAX_AGE_S)) return false;
    if (ticks_per_s) *ticks_per_s = g.cal_tps;
    if (span_s) *span_s = g.cal_span;
    return true;
}

bool gps_utc(uint32_t t, int64_t *whole, double *frac)
{
    if (!g.base_valid || !g.have_pps) return false;
    const uint64_t u = up_of(t);
    double tps;
    if (!gps_clock(t, &tps, NULL)) tps = (double)TICK_HZ;
    const double dt = (u >= g.pps_up) ? (double)(u - g.pps_up) / tps : -(double)(g.pps_up - u) / tps;
    if (fabs(dt) > GPS_UTC_MAX_EXTRAP_S) return false;
    const double w = floor(dt);
    *whole = g.base_utc + (g.sec - g.base_sec) + (int64_t)w;
    *frac = dt - w;
    return true;
}

void gps_format_utc(fmt_t *f, int64_t whole, double frac)
{
    int64_t y;
    uint32_t mo, d;
    uint32_t us = (uint32_t)(frac * 1e6 + 0.5);
    if (us >= 1000000u) { us -= 1000000u; whole++; }
    int64_t days = whole / 86400, sod = whole % 86400;
    if (sod < 0) { sod += 86400; days--; }
    civil_from_days(days, &y, &mo, &d);
    fmt_u32_pad(f, (uint32_t)y, 4);
    fmt_str(f, "-");
    fmt_u32_pad(f, mo, 2);
    fmt_str(f, "-");
    fmt_u32_pad(f, d, 2);
    fmt_str(f, "T");
    fmt_u32_pad(f, (uint32_t)(sod / 3600), 2);
    fmt_str(f, ":");
    fmt_u32_pad(f, (uint32_t)(sod / 60 % 60), 2);
    fmt_str(f, ":");
    fmt_u32_pad(f, (uint32_t)(sod % 60), 2);
    fmt_str(f, ".");
    fmt_u32_pad(f, us, 6);
    fmt_str(f, "Z");
}

/* GGA's quality while GGA is fresh; else 1 (2 for differential) while RMC is fresh with status A; else 0. */
static uint32_t fix_quality(uint64_t ref)
{
    if (fresh(g.gga_have, g.gga_up, ref, GPS_FIX_MAX_AGE_S)) return g.gga_quality;
    if (fresh(g.rmc_have, g.rmc_up, ref, GPS_FIX_MAX_AGE_S) && g.rmc_status == 'A')
        return g.rmc_mode == 'D' ? 2u : 1u;
    return 0u;
}

static void kv_na(fmt_t *f, const char *key) { fmt_kv_str(f, key, "na"); }

void gps_format_fields(fmt_t *f, uint32_t t)
{
    int64_t whole;
    double frac;
    const uint64_t ref = up_of(t);
    const bool have_utc = gps_utc(t, &whole, &frac);
    const uint32_t fix = fix_quality(ref);
    const bool gga = fresh(g.gga_have, g.gga_up, ref, GPS_FIX_MAX_AGE_S);
    const bool rmc = fresh(g.rmc_have, g.rmc_up, ref, GPS_FIX_MAX_AGE_S) && g.rmc_status == 'A';
    const bool pos = fix > 0u && fresh(g.pos_have, g.pos_up, ref, GPS_FIX_MAX_AGE_S);

    if (have_utc) { fmt_str(f, " utc="); gps_format_utc(f, whole, frac); }
    else kv_na(f, "utc");
    fmt_kv_u32(f, "fix", fix);
    if (gga && g.gga_has_sats) fmt_kv_u32(f, "sats", g.gga_sats); else kv_na(f, "sats");
    if (gga && g.gga_has_hdop) fmt_kv_fix(f, "hdop", g.gga_hdop, 2); else kv_na(f, "hdop");
    if (pos) { fmt_kv_fix(f, "lat", g.pos_lat, 7); fmt_kv_fix(f, "lon", g.pos_lon, 7); }
    else { kv_na(f, "lat"); kv_na(f, "lon"); }
    if (gga && g.gga_has_alt && fix > 0u) fmt_kv_fix(f, "alt_m", g.gga_alt, 1); else kv_na(f, "alt_m");
    if (rmc && g.rmc_has_speed) fmt_kv_fix(f, "spd_mps", g.rmc_speed_mps, 2); else kv_na(f, "spd_mps");
    if (rmc && g.rmc_has_course) fmt_kv_fix(f, "cog_deg", g.rmc_course, 1); else kv_na(f, "cog_deg");
    if (pos && have_utc) {
        /* the event's UTC second of the day minus the position's: positive = the position is older */
        double age = (double)(whole % 86400) + frac - g.pos_sod;
        if (age > 43200.0) age -= 86400.0;
        if (age < -43200.0) age += 86400.0;
        fmt_kv_fix(f, "pos_age_s", age, 2);
    } else {
        kv_na(f, "pos_age_s");
    }
}

void gps_format_status(fmt_t *f, uint32_t now)
{
    double tps;
    uint32_t span;
    const uint64_t ref = up_of(now);

    fmt_str(f, "GPS");
    gps_format_fields(f, now);
    fmt_kv_u32(f, "pps", g.n_pps);
    if (g.have_pps) fmt_kv_fix(f, "pps_age_s", (double)(ref - g.pps_up) / (double)TICK_HZ, 2);
    else kv_na(f, "pps_age_s");
    if (gps_clock(now, &tps, &span)) {
        fmt_kv_fix(f, "clk_ppm", (tps / (double)TICK_HZ - 1.0) * 1e6, 3);
        fmt_kv_u32(f, "clk_s", span);
        fmt_kv_fix(f, "clk_age_s", (double)up_diff(ref, g.cal_up) / (double)TICK_HZ, 1);
    } else {
        kv_na(f, "clk_ppm");
        fmt_kv_u32(f, "clk_s", 0u);
        kv_na(f, "clk_age_s");
    }
    if (g.labels) fmt_kv_u32(f, "lag_ms", g.lag_us / 1000u); else kv_na(f, "lag_ms");
    fmt_kv_u32(f, "labels", g.labels);
    fmt_kv_u32(f, "utc_jumps", g.utc_jumps);
    fmt_kv_u32(f, "pps_bad", g.pps_bad);
    fmt_kv_u32(f, "pps_gaps", g.pps_gaps);
    fmt_kv_u32(f, "pps_missed", g.pps_missed);
    fmt_kv_u32(f, "rmc", g.n_rmc);
    fmt_kv_u32(f, "gga", g.n_gga);
    fmt_kv_u32(f, "bad_sum", g.bad_sum);
    fmt_kv_u32(f, "skipped", q.skipped);
    fmt_kv_u32(f, "dropped", q.dropped);
    fmt_kv_u32(f, "too_long", q.too_long);
    fmt_kv_u32(f, "pps_dropped", q.pps_dropped);
    fmt_kv_u32(f, "pps_lost", q.pps_lost);
}
