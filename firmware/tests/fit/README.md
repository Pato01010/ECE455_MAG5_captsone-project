# Host tests of the frequency fit (`app/fit.c`) and Path A (`app/patha.c`)

**Date:** 2026-10-15

The C fit is the port of `est_fft` / `est_fit` in
[`simulations/frequency-estimation/frequency_estimation.py`](../../../simulations/frequency-estimation/frequency_estimation.py).
These tests generate records with that file's `make_record`, fit the same 12-bit codes in C and in Python, and
compare. They also run Path A on comparator edges synthesised from the same records, and compile both modules for the
Cortex-M4F. The tests only read `simulations/`: the driver imports the reference with bytecode writing turned off.

## How to run

From this folder, with Cygwin's `make` and `gcc` on the PATH:

```
py -3.14 run_fit_tests.py            # builds (make), runs everything, about 50 s; exit code 0 = all passed
py -3.14 run_fit_tests.py --per-case 10 --no-build
make                                 # only the build: out/fit_test.exe plus fit.c and patha.c for the M4F
make clean                           # removes out/
```

`make` stops if any compile prints a diagnostic. It compiles the following, and `.DELETE_ON_ERROR` makes the next
`make` check a failed object again:

- host: `gcc -std=c99 -O2 -Wall -Wextra -Wdouble-promotion -Wshadow`;
- M4F: the same flags with `-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16`, using STM32CubeCLT 1.21.0's
  `arm-none-eabi-gcc` 14.3 (`ARM_BIN=...` on the make line points elsewhere).

It also writes the M4F disassembly to `out/arm/*.lst`.

**Tracked files:** `Makefile`, `fit_test.c`, `run_fit_tests.py`, `README.md`. Everything in `out/` is generated and
regenerable (`firmware/.gitignore` already ignores `tests/*/out/`):

| `out/` file | What |
|---|---|
| `records.bin`, `records_truth.csv` | the records (codes and edge counts) and their true f, A, T2*, phase and seed |
| `results_c.csv`, `results_py.csv` | one line per record from `fit_test.exe` and from the reference |
| `summary.txt`, `summary.json` | the tables below and the self-test log (the driver also prints them) |
| `host/`, `arm/`, `fit_test.exe` | objects, compile logs (empty = warning-free), M4F disassembly |

## The test set

- **Records:** `make_record(A, T2, en, f, phi, n = 18,800, quantize = False)` at 20 kSPS, then
  `clip(round(v / 0.7935 mV) + 2048, 0, 4095)` (VREF+ 3.25 V). `en` is set so that the band-limited noise is
  **23.0 codes rms per sample** (measured mean over the records: 23.00); its white-equivalent sigma, `en x sqrt(fs/2)`,
  is 48.4 codes. Each record has its own noise seed and a random phase.
- **Cases:**
  - peak/rms (A / 23 codes) 54.8, 31.6, 19.1 and 6.7, then untuned 5.3, 3.2 and 1.5, all at T2* = 1 s;
  - T2* = 0.3 s and 3 s at peak/rms 31.6;
  - each at 1,064, 2,128.875 and 2,767.5 Hz;
  - 20 records per case, 540 records with a signal;
  - plus 20 noise-only records, which must give `FIT_E_NOPEAK`.
- **Reference:** `est_fit(t, codes)` with its own `est_fft` seed, on the same codes as floats. A line-for-line copy
  (`est_fit_traced`) also reports whether `est_fit` stopped on its `|step_f| < 1e-7 Hz` test. Its result is checked
  to be bit-identical to `est_fit` on every record.
- **Edges for Path A:**
  - The same signal and noise between the samples. The noise is periodic and band-limited by construction, so it is
    FFT-interpolated exactly to 160 kHz; the interpolation matches the samples to 6e-14 codes.
  - A comparator with thresholds at mid-scale +/-2 codes (4 codes of hysteresis), crossings interpolated linearly at
    160 kHz, converted to 170 MHz counts.
  - `t_rec0 = 2^32 - 85,000,000`, so TIM2 wraps 0.5 s into every record.
  - Per record, 1 % of the edges dropped and 1 % extra close pairs (2-20 us apart) at random times.
  - `patha_estimate` gets the C fit and `noise_rms = fit.resid_rms`.
- **C self-tests** (`fit_test.c`, on the first record):
  - every `FIT_E_ARGS` / `PATHA_E_*` argument case;
  - a flat record gives `FIT_E_NOPEAK`;
  - canaries around the workspace are untouched (nothing is written outside `work[0 .. FIT_WORK_FLOATS-1]`);
  - the record is unchanged;
  - a second run is bit-identical;
  - `progress` is called at least once per iteration plus three times in the seed.

## Results (2026-10-15, 20 records per case)

- **Acceptance 1: PASS.** |f_C - f_Py| < 0.1 x CRLB on every record where `est_fit` converged. The largest ratio is
  **2.5e-5**, which is 4.5 nHz. CRLB = `crlb_f(A, T2, 23 codes, 18,800)`, the stricter of the two sigmas.
  Over all 540 signal records, including the 8 where `est_fit` ran its 40 iterations, the largest ratio is also 2.5e-5.
- **Acceptance 2: PASS.** The C fit's rms error against the true f is within 10 % of the reference's in every case: the
  ratio is 1.0000 everywhere (0.9999995 to 1.0000009).
- **Seed:** `f_seed_hz` matches `est_fft` within 0.2 uHz on every record.
- **Status:** all 540 signal records return `FIT_OK`, and all 20 noise-only records return `FIT_E_NOPEAK`.
- **Self-tests:** all passed.
- **Build:** all five compiles are warning-free (host `fit.c`, `patha.c`, `fit_test.c`; M4F `fit.c`, `patha.c`).

| Case (peak/rms, T2*, f) | est_fit converged | max \|f_C - f_Py\| / CRLB | rms error C (mHz) | rms error Py (mHz) | C / Py | CRLB (mHz) | CRLB ref (mHz) | iterations C mean / max | iterations Py mean / max |
|---|---|---|---|---|---|---|---|---|---|
| 54.8, T2 1 s, 1064 Hz | 20/20 | 4.6e-06 | 0.294 | 0.294 | 1.0000 | 0.179 | 0.376 | 2.9 / 3 | 3.4 / 6 |
| 54.8, T2 1 s, 2128.88 Hz | 20/20 | 2.0e-05 | 0.337 | 0.337 | 1.0000 | 0.179 | 0.376 | 3.0 / 3 | 5.4 / 16 |
| 54.8, T2 1 s, 2767.5 Hz | 19/20 | 2.5e-05 | 0.363 | 0.363 | 1.0000 | 0.179 | 0.376 | 3.0 / 3 | 6.3 / 40 |
| 31.6, T2 1 s, 1064 Hz | 20/20 | 3.7e-06 | 0.849 | 0.849 | 1.0000 | 0.310 | 0.653 | 3.0 / 3 | 3.0 / 4 |
| 31.6, T2 1 s, 2128.88 Hz | 19/20 | 8.3e-06 | 0.574 | 0.574 | 1.0000 | 0.310 | 0.653 | 3.0 / 3 | 6.3 / 40 |
| 31.6, T2 1 s, 2767.5 Hz | 20/20 | 2.8e-06 | 0.556 | 0.556 | 1.0000 | 0.310 | 0.653 | 3.0 / 3 | 3.8 / 8 |
| 19.1, T2 1 s, 1064 Hz | 20/20 | 1.7e-06 | 0.887 | 0.887 | 1.0000 | 0.513 | 1.080 | 3.0 / 3 | 3.2 / 6 |
| 19.1, T2 1 s, 2128.88 Hz | 20/20 | 1.2e-05 | 1.127 | 1.127 | 1.0000 | 0.513 | 1.080 | 3.0 / 3 | 3.5 / 9 |
| 19.1, T2 1 s, 2767.5 Hz | 18/20 | 2.1e-05 | 0.805 | 0.805 | 1.0000 | 0.513 | 1.080 | 3.0 / 3 | 7.8 / 40 |
| 6.7, T2 1 s, 1064 Hz | 19/20 | 1.4e-06 | 2.473 | 2.473 | 1.0000 | 1.463 | 3.078 | 3.2 / 4 | 5.6 / 40 |
| 6.7, T2 1 s, 2128.88 Hz | 20/20 | 1.7e-06 | 2.622 | 2.622 | 1.0000 | 1.463 | 3.078 | 3.2 / 4 | 4.2 / 16 |
| 6.7, T2 1 s, 2767.5 Hz | 20/20 | 2.6e-06 | 2.970 | 2.970 | 1.0000 | 1.463 | 3.078 | 3.3 / 4 | 3.6 / 7 |
| 5.3 untuned, T2 1 s, 1064 Hz | 20/20 | 9.2e-07 | 3.668 | 3.668 | 1.0000 | 1.849 | 3.892 | 3.7 / 4 | 4.7 / 10 |
| 5.3 untuned, T2 1 s, 2128.88 Hz | 20/20 | 4.1e-06 | 3.295 | 3.295 | 1.0000 | 1.849 | 3.892 | 3.6 / 4 | 4.3 / 11 |
| 5.3 untuned, T2 1 s, 2767.5 Hz | 20/20 | 2.7e-06 | 3.395 | 3.395 | 1.0000 | 1.849 | 3.892 | 3.5 / 4 | 4.3 / 12 |
| 3.2 untuned, T2 1 s, 1064 Hz | 19/20 | 3.2e-06 | 5.086 | 5.086 | 1.0000 | 3.063 | 6.446 | 3.9 / 5 | 7.8 / 40 |
| 3.2 untuned, T2 1 s, 2128.88 Hz | 20/20 | 1.4e-06 | 7.943 | 7.943 | 1.0000 | 3.063 | 6.446 | 3.8 / 5 | 5.0 / 17 |
| 3.2 untuned, T2 1 s, 2767.5 Hz | 20/20 | 1.3e-06 | 4.964 | 4.964 | 1.0000 | 3.063 | 6.446 | 3.7 / 4 | 4.1 / 8 |
| 1.5 untuned, T2 1 s, 1064 Hz | 20/20 | 1.3e-06 | 10.891 | 10.891 | 1.0000 | 6.533 | 13.751 | 4.3 / 6 | 5.9 / 18 |
| 1.5 untuned, T2 1 s, 2128.88 Hz | 20/20 | 1.3e-06 | 10.516 | 10.516 | 1.0000 | 6.533 | 13.751 | 4.4 / 5 | 5.4 / 13 |
| 1.5 untuned, T2 1 s, 2767.5 Hz | 20/20 | 1.2e-06 | 10.490 | 10.490 | 1.0000 | 6.533 | 13.751 | 4.0 / 5 | 4.6 / 9 |
| 31.6, T2 0.3 s, 1064 Hz | 20/20 | 1.0e-05 | 2.141 | 2.141 | 1.0000 | 0.902 | 1.899 | 5.0 / 5 | 6.7 / 21 |
| 31.6, T2 0.3 s, 2128.88 Hz | 19/20 | 2.6e-06 | 1.885 | 1.885 | 1.0000 | 0.902 | 1.899 | 5.0 / 5 | 7.2 / 40 |
| 31.6, T2 0.3 s, 2767.5 Hz | 19/20 | 2.9e-06 | 1.692 | 1.692 | 1.0000 | 0.902 | 1.899 | 5.0 / 5 | 7.0 / 40 |
| 31.6, T2 3 s, 1064 Hz | 20/20 | 2.5e-06 | 0.464 | 0.464 | 1.0000 | 0.224 | 0.472 | 4.0 / 4 | 4.0 / 5 |
| 31.6, T2 3 s, 2128.88 Hz | 20/20 | 4.1e-06 | 0.480 | 0.480 | 1.0000 | 0.224 | 0.472 | 4.0 / 4 | 4.0 / 4 |
| 31.6, T2 3 s, 2767.5 Hz | 20/20 | 4.7e-06 | 0.406 | 0.406 | 1.0000 | 0.224 | 0.472 | 4.0 / 4 | 4.1 / 5 |

Path A, with the C fit as the gate (1 nT = 42.6 mHz):

| Case | Path A ok | bias vs true f (mHz) | rms vs true f (mHz) | bias vs C fit (mHz) | rms vs C fit (mHz) | edges used rise / fall | rejected |
|---|---|---|---|---|---|---|---|
| 54.8, T2 1 s, 1064 Hz | 20/20 | -0.60 | 0.76 | -0.49 | 0.65 | 991 / 990 | 40 |
| 54.8, T2 1 s, 2128.88 Hz | 20/20 | -0.38 | 0.49 | -0.44 | 0.48 | 1982 / 1980 | 80 |
| 54.8, T2 1 s, 2767.5 Hz | 20/20 | -0.32 | 0.53 | -0.40 | 0.43 | 2575 / 2576 | 103 |
| 31.6, T2 1 s, 1064 Hz | 20/20 | -0.57 | 0.97 | -0.73 | 0.97 | 991 / 990 | 40 |
| 31.6, T2 1 s, 2128.88 Hz | 20/20 | -0.93 | 1.09 | -0.72 | 0.80 | 1982 / 1981 | 80 |
| 31.6, T2 1 s, 2767.5 Hz | 20/20 | -0.85 | 1.10 | -0.71 | 0.76 | 2577 / 2575 | 104 |
| 19.1, T2 1 s, 1064 Hz | 20/20 | -1.32 | 1.61 | -1.29 | 1.57 | 989 / 991 | 40 |
| 19.1, T2 1 s, 2128.88 Hz | 20/20 | -1.23 | 1.76 | -1.20 | 1.29 | 1980 / 1982 | 80 |
| 19.1, T2 1 s, 2767.5 Hz | 20/20 | -1.21 | 1.41 | -1.34 | 1.41 | 2575 / 2576 | 104 |
| 6.7, T2 1 s, 1064 Hz | 20/20 | -3.02 | 3.70 | -3.93 | 4.65 | 990 / 991 | 66 |
| 6.7, T2 1 s, 2128.88 Hz | 20/20 | -3.65 | 5.11 | -3.63 | 4.03 | 1980 / 1980 | 81 |
| 6.7, T2 1 s, 2767.5 Hz | 20/20 | -4.00 | 5.22 | -3.56 | 3.77 | 2573 / 2573 | 104 |
| 5.3 untuned, T2 1 s, 1064 Hz | 20/20 | -2.34 | 5.74 | -2.35 | 3.59 | 990 / 991 | 130 |
| 5.3 untuned, T2 1 s, 2128.88 Hz | 20/20 | -4.25 | 6.31 | -4.02 | 5.00 | 1975 / 1976 | 87 |
| 5.3 untuned, T2 1 s, 2767.5 Hz | 20/20 | -4.65 | 5.90 | -4.02 | 4.54 | 2562 / 2562 | 107 |
| 3.2 untuned, T2 1 s, 1064 Hz | 20/20 | -4.62 | 16.03 | -4.15 | 13.44 | 499 / 500 | 121 |
| 3.2 untuned, T2 1 s, 2128.88 Hz | 20/20 | -7.37 | 17.19 | -4.14 | 15.64 | 989 / 990 | 51 |
| 3.2 untuned, T2 1 s, 2767.5 Hz | 20/20 | -5.13 | 17.62 | -4.57 | 17.69 | 1272 / 1272 | 56 |
| 1.5 untuned, T2 1 s (all three f) | 0/20 each, `PATHA_E_FEW` | | | | | | |
| 31.6, T2 0.3 s, 1064 Hz | 20/20 | -4.33 | 6.24 | -4.61 | 6.23 | 872 / 872 | 71 |
| 31.6, T2 0.3 s, 2128.88 Hz | 20/20 | -7.17 | 8.23 | -7.25 | 8.04 | 1745 / 1745 | 75 |
| 31.6, T2 0.3 s, 2767.5 Hz | 20/20 | -7.55 | 8.24 | -7.75 | 8.49 | 2263 / 2262 | 93 |
| 31.6, T2 3 s, 1064 Hz | 20/20 | -0.03 | 0.62 | -0.10 | 0.41 | 990 / 991 | 40 |
| 31.6, T2 3 s, 2128.88 Hz | 20/20 | -0.04 | 0.48 | -0.14 | 0.19 | 1982 / 1981 | 79 |
| 31.6, T2 3 s, 2767.5 Hz | 20/20 | -0.16 | 0.48 | -0.18 | 0.21 | 2577 / 2574 | 103 |
| noise only | 0/20, `PATHA_E_FIT` (no fit) | | | | | | |

## Reading the numbers

- **Two CRLBs.** "CRLB" uses the per-sample rms of the band-limited noise (23 codes), and the acceptance uses it.
  "CRLB ref" uses the reference's own convention in `main()`, `en x sqrt(fs/2)` (48.4 codes), which describes the
  in-band density. Both fits' rms errors are 0.75-1.3 x CRLB ref, which is the expected scatter of an rms over
  20 records. `f_sigma_hz` is computed as the API asks (J^T J and the residual variance, white noise), so it lands on
  the smaller CRLB. For this band-limited noise the true scatter is about 48.4 / 23 = 2.1 x `f_sigma_hz`; measured per
  case, rms error / mean `f_sigma_hz` is 1.6 to 2.7.
- **`est_fit`'s 40-iteration records** (8 of 540) are not failures to converge. It reaches its answer in 2-3 steps;
  then its strict `cost_new < cost` test rejects the next step of a few nHz at float64 rounding, and every later one
  (traced on one record: +1.3e-6 on a cost of 9.7e6, then exactly 0). The C fit agrees with those records within
  1e-8 Hz. The C fit stops on such a step, which is why it needs fewer iterations (mean 3.6, at most 6).
- **The C LM follows `est_fit`'s path.**
  - A development trace (not repeated by these scripts) found the same accept/reject decision on every step of
    1e-7 Hz or more, on all 540 records.
  - The C decides a step by its double cost change when that change is larger than the float rounding of the pass
    (32 ulps of the cost and of A x rms(r) x rms(e sin)). Below that rounding it uses -(g + g_new) . delta, the exact
    change of a quadratic cost from the gradients at both ends.
  - Over one run (the 540 records plus the self-tests' two extra fits) there were 1,977 decisions. The cost change
    decided 687, all accepted; the gradient rule decided 1,290: 1,285 accepted, and 5 rejected steps already below
    1e-7 Hz, which end the fit as converged.
- **Path A reads low because of the hysteresis, by a fixed amount.** The comparator switches a little after each zero
  crossing, by a phase of asin(2 codes / envelope). That delay grows as the FID decays, so it stretches the measured
  period.
  It delays rising and falling edges alike, so averaging the two does not cancel it. A development check with
  noise-free records (not repeated by these scripts) measured:

  | Case | Bias with 4 codes of hysteresis | Bias with ~0 hysteresis |
  |---|---|---|
  | 54.8, T2 1 s | -0.41 mHz | |
  | 19.1, T2 1 s | -1.19 mHz | |
  | 31.6, T2 0.3 s | -8.9 mHz | |
  | 31.6, T2 3 s | -0.17 mHz | |
  | all four | | 0.002 mHz or less |

  The noisy biases in the table match the 4-code column. The board's thresholds are larger and asymmetric (+5.5 /
  -13.8 mV at BP_OUT in the acquisition rev C README, about +7 / -17 codes), so its bias will be larger. This test does
  not model them.
- **Glitches.** About 99 % of the injected extra edges are rejected: 792 of 800 at 1,064 Hz and 2,065 of 2,080 at
  2,767.5 Hz, over the 20 records of the peak/rms 54.8 cases. The rest land, inside the +/-25 % gate, in a cycle whose
  own edge was one of the dropped ones, and stand in for it; the edges used exceed the genuine edges kept by exactly
  that number. The low-SNR cases add the comparator's own noise-driven chatter.
- **Counter wrap.** A development run (not repeated by these scripts) gave bit-identical Path A results with
  `t_rec0 = 1000`, so the result does not depend on where TIM2 wraps.
- **Speed on this PC:** about 1.6-2.0 ms per fit (x86, Cygwin gcc -O2).

The M4F cost is in `app/fit.h`. It is counted from `out/arm/fit.lst`, not measured on hardware: ~2.0 M cycles per LM
pass and ~16 M cycles for a typical fit.

## Not verified

- Anything on the STM32G474 itself: the cycle counts are estimates.
- Whether newlib's `sin`, `cos`, `exp`, `cosf` and `sinf` on the M4F give exactly the host's values. The float
  arithmetic is IEEE single on both, with no contraction under `-std=c99`; only the library functions could differ, in
  the last bit. That is expected to stay far inside these tolerances, but it was not checked.
