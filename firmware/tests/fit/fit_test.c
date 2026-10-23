/* fit_test.c - runs fit_record and patha_estimate on the records written by run_fit_tests.py.
 *
 *   fit_test <records.bin> <results.csv>
 *
 * First the self-tests (argument checks, workspace bounds, the record left unchanged, determinism,
 * progress calls, a flat record), then every record: fit_record, then patha_estimate with the fit and
 * noise_rms = fit.resid_rms. One CSV line per record. Exit code 0 only if every self-test passed.
 *
 * records.bin, little-endian:
 *   "MAGFIT01", u32 n_records, u32 n_samples, f64 fs_hz, f64 tick_hz, f64 f_lo_hz, f64 f_hi_hz,
 *   then per record: i32 case_id, i32 rec_id, u32 t_rec0, u32 n_edges, u16 codes[n_samples],
 *   u32 edges[n_edges].
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "fit.h"
#include "patha.h"

#define MAX_SAMPLES 65536u
#define MAX_EDGES   65536u
#define CANARY      64u

static uint16_t codes[MAX_SAMPLES];
static uint16_t codes_copy[MAX_SAMPLES];
static uint32_t edges[MAX_EDGES];
static float work_buf[CANARY + FIT_WORK_FLOATS + CANARY];
static unsigned progress_calls;
static int failures;

static void on_progress(void)
{
    progress_calls++;
}

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

static int rd(FILE *f, void *p, size_t n)
{
    return fread(p, 1, n, f) == n ? 0 : -1;
}

static void fill_canaries(void)
{
    uint32_t i;
    for (i = 0; i < CANARY; i++) {
        work_buf[i] = -12345.0f;
        work_buf[CANARY + FIT_WORK_FLOATS + i] = 54321.0f;
    }
}

static int canaries_intact(void)
{
    uint32_t i;
    for (i = 0; i < CANARY; i++)
        if (work_buf[i] != -12345.0f || work_buf[CANARY + FIT_WORK_FLOATS + i] != 54321.0f) return 0;
    return 1;
}

static float *work(void)
{
    return work_buf + CANARY;
}

static void self_tests(const uint16_t *x, uint32_t n, const uint32_t *e, uint32_t ne, uint32_t t_rec0,
                       double fs, double tick, double f_lo, double f_hi)
{
    fit_result_t r1, r2, bad;
    patha_result_t pa;
    uint32_t i;
    int rc;

    printf("self-tests (on the first record)\n");
    check(fit_record(x, n, fs, f_lo, f_hi, work(), FIT_WORK_FLOATS, NULL, NULL) == FIT_E_ARGS,
          "fit: out == NULL -> FIT_E_ARGS");
    rc = fit_record(NULL, n, fs, f_lo, f_hi, work(), FIT_WORK_FLOATS, NULL, &bad);
    check(rc == FIT_E_ARGS && bad.status == FIT_E_ARGS, "fit: x == NULL -> FIT_E_ARGS");
    check(fit_record(x, n, fs, f_lo, f_hi, NULL, FIT_WORK_FLOATS, NULL, &bad) == FIT_E_ARGS,
          "fit: work == NULL -> FIT_E_ARGS");
    check(fit_record(x, n, fs, f_lo, f_hi, work(), FIT_WORK_FLOATS - 1u, NULL, &bad) == FIT_E_ARGS,
          "fit: work_len = FIT_WORK_FLOATS - 1 -> FIT_E_ARGS");
    check(fit_record(x, FIT_SEED_FFT_N - 1u, fs, f_lo, f_hi, work(), FIT_WORK_FLOATS, NULL, &bad)
          == FIT_E_ARGS, "fit: n = FIT_SEED_FFT_N - 1 -> FIT_E_ARGS");
    check(fit_record(x, n, fs, f_hi, f_lo, work(), FIT_WORK_FLOATS, NULL, &bad) == FIT_E_ARGS,
          "fit: f_lo >= f_hi -> FIT_E_ARGS");
    check(fit_record(x, n, fs, f_lo, 0.5 * fs, work(), FIT_WORK_FLOATS, NULL, &bad) == FIT_E_ARGS,
          "fit: f_hi >= fs/2 -> FIT_E_ARGS");
    check(fit_record(x, n, 0.0, f_lo, f_hi, work(), FIT_WORK_FLOATS, NULL, &bad) == FIT_E_ARGS,
          "fit: fs = 0 -> FIT_E_ARGS");
    check(fit_record(x, n, fs, 0.0, f_hi, work(), FIT_WORK_FLOATS, NULL, &bad) == FIT_E_ARGS,
          "fit: f_lo = 0 -> FIT_E_ARGS");

    memcpy(codes_copy, x, n * sizeof x[0]);
    fill_canaries();
    progress_calls = 0;
    rc = fit_record(x, n, fs, f_lo, f_hi, work(), FIT_WORK_FLOATS, on_progress, &r1);
    printf("  first record: status %d, f %.9f Hz, %d iterations, %u progress calls\n", rc, r1.f_hz,
           r1.iterations, progress_calls);
    check(rc == FIT_OK && r1.status == FIT_OK, "fit: first record converges (FIT_OK)");
    check(canaries_intact(), "fit: nothing written outside work[0 .. FIT_WORK_FLOATS-1]");
    check(memcmp(codes_copy, x, n * sizeof x[0]) == 0, "fit: the record is unchanged");
    check(progress_calls >= (unsigned)r1.iterations + 3u, "fit: progress >= once per iteration + 3 seed calls");
    rc = fit_record(x, n, fs, f_lo, f_hi, work(), FIT_WORK_FLOATS, NULL, &r2);
    check(memcmp(&r1, &r2, sizeof r1) == 0, "fit: a second run gives a bit-identical result");

    for (i = 0; i < n; i++) codes_copy[i] = 2048u;
    rc = fit_record(codes_copy, n, fs, f_lo, f_hi, work(), FIT_WORK_FLOATS, NULL, &bad);
    check(rc == FIT_E_NOPEAK, "fit: a flat record (all 2048) -> FIT_E_NOPEAK");

    check(patha_estimate(e, ne, t_rec0, tick, &r1, fs, r1.resid_rms, NULL) == PATHA_E_ARGS,
          "patha: out == NULL -> PATHA_E_ARGS");
    check(patha_estimate(NULL, ne, t_rec0, tick, &r1, fs, r1.resid_rms, &pa) == PATHA_E_ARGS,
          "patha: edges == NULL with n_edges > 0 -> PATHA_E_ARGS");
    check(patha_estimate(e, ne, t_rec0, tick, NULL, fs, r1.resid_rms, &pa) == PATHA_E_ARGS,
          "patha: fit == NULL -> PATHA_E_ARGS");
    check(patha_estimate(e, ne, t_rec0, tick, &r1, fs, 0.0, &pa) == PATHA_E_ARGS,
          "patha: noise_rms = 0 -> PATHA_E_ARGS");
    check(patha_estimate(e, ne, t_rec0, tick, &bad, fs, r1.resid_rms, &pa) == PATHA_E_FIT,
          "patha: fit status not FIT_OK -> PATHA_E_FIT");
    check(patha_estimate(e, 0u, t_rec0, tick, &r1, fs, r1.resid_rms, &pa) == PATHA_E_FEW,
          "patha: no edges -> PATHA_E_FEW");
    check(patha_estimate(e, 15u, t_rec0, tick, &r1, fs, r1.resid_rms, &pa) == PATHA_E_FEW,
          "patha: 15 edges (< 10 per class) -> PATHA_E_FEW");
    rc = patha_estimate(e, ne, t_rec0, tick, &r1, fs, r1.resid_rms, &pa);
    printf("  first record Path A: status %d, f %.9f Hz (%u rise, %u fall, %u rejected)\n", rc, pa.f_hz,
           pa.n_rise, pa.n_fall, pa.n_rejected);
    check(rc == PATHA_OK, "patha: first record -> PATHA_OK");
}

int main(int argc, char **argv)
{
    FILE *fi, *fo;
    char magic[8];
    uint32_t n_rec, n, r;
    double fs, tick, f_lo, f_hi;
    clock_t c_fit = 0, c_pa = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: fit_test <records.bin> <results.csv>\n");
        return 2;
    }
    fi = fopen(argv[1], "rb");
    if (fi == NULL) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    if (rd(fi, magic, 8) || memcmp(magic, "MAGFIT01", 8) != 0 || rd(fi, &n_rec, 4) || rd(fi, &n, 4)
        || rd(fi, &fs, 8) || rd(fi, &tick, 8) || rd(fi, &f_lo, 8) || rd(fi, &f_hi, 8) || n > MAX_SAMPLES) {
        fprintf(stderr, "bad header in %s\n", argv[1]);
        return 2;
    }
    fo = fopen(argv[2], "w");
    if (fo == NULL) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 2;
    }
    fprintf(fo, "rec,case_id,rec_id,fit_status,iterations,progress_calls,f_hz,f_sigma_hz,amp,phase_rad,"
                "decay_per_s,offset,resid_rms,f_seed_hz,pa_status,pa_f_rise_hz,pa_f_fall_hz,pa_f_hz,"
                "pa_n_rise,pa_n_fall,pa_n_rejected\n");

    for (r = 0; r < n_rec; r++) {
        int32_t case_id, rec_id;
        uint32_t t_rec0, ne;
        fit_result_t fit;
        patha_result_t pa;
        clock_t c0;

        if (rd(fi, &case_id, 4) || rd(fi, &rec_id, 4) || rd(fi, &t_rec0, 4) || rd(fi, &ne, 4) || ne > MAX_EDGES
            || rd(fi, codes, (size_t)n * 2u) || rd(fi, edges, (size_t)ne * 4u)) {
            fprintf(stderr, "short record %u\n", r);
            return 2;
        }
        if (r == 0u) self_tests(codes, n, edges, ne, t_rec0, fs, tick, f_lo, f_hi);

        progress_calls = 0;
        c0 = clock();
        fit_record(codes, n, fs, f_lo, f_hi, work(), FIT_WORK_FLOATS, on_progress, &fit);
        c_fit += clock() - c0;
        c0 = clock();
        patha_estimate(edges, ne, t_rec0, tick, &fit, fs, fit.resid_rms > 0.0 ? fit.resid_rms : 1.0, &pa);
        c_pa += clock() - c0;
        fprintf(fo, "%u,%d,%d,%d,%d,%u,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%d,%.17g,%.17g,%.17g,"
                    "%u,%u,%u\n",
                r, case_id, rec_id, fit.status, fit.iterations, progress_calls, fit.f_hz, fit.f_sigma_hz,
                fit.amp, fit.phase_rad, fit.decay_per_s, fit.offset, fit.resid_rms, fit.f_seed_hz, pa.status,
                pa.f_rise_hz, pa.f_fall_hz, pa.f_hz, pa.n_rise, pa.n_fall, pa.n_rejected);
    }
    fclose(fi);
    fclose(fo);
    printf("%u records: fit %.1f ms per record, Path A %.3f ms per record (this PC)\n", n_rec,
           1000.0 * (double)c_fit / CLOCKS_PER_SEC / (double)(n_rec ? n_rec : 1u),
           1000.0 * (double)c_pa / CLOCKS_PER_SEC / (double)(n_rec ? n_rec : 1u));
    printf("self-tests: %s (%d failed)\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
