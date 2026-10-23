"""
Host tests of firmware/app/fit.c and patha.c against the Python reference.

1. Records from the reference's make_record (n = 18,800, fs = 20 kHz, band-limited noise), converted to
   12-bit codes around mid-scale 2048 (VREF 3.25 V), plus comparator edges synthesised from the same
   noisy signal (hysteresis, TIM2 counts at 170 MHz wrapping during the record, injected glitches).
2. `make` builds out/fit_test.exe (and the M4F objects; any warning stops it); fit_test fits every record
   with fit_record and runs patha_estimate.
3. The reference fits the same codes (as floats) with est_fit seeded by est_fft, and the two are compared:
   |f_C - f_Py| against 0.1 x CRLB on every record where est_fit converged, rms errors against the true f,
   Path A's bias and rms.

Run:  py -3.14 run_fit_tests.py [--per-case 20] [--no-build]
Everything it writes goes to out/ (regenerable). It reads simulations/ and writes nothing there.
"""
import argparse
import json
import math
import os
import struct
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(HERE, "out")

sys.dont_write_bytecode = True      # importing the reference must not write anything under simulations/
sys.path.insert(0, os.path.join(ROOT, "simulations", "tools"))
sys.path.insert(0, os.path.join(ROOT, "simulations", "frequency-estimation"))
import frequency_estimation as FE  # noqa: E402

FS = FE.FS                          # 20,000 Hz
N = 18_800                          # 0.94 s
VREF = 3.25
LSB = VREF / 4096                   # 0.7935 mV per code
MID = 2048
NOISE_CODES = 23.0                  # per-sample rms of the band-limited noise
TICK_HZ = 170e6
T_REC0 = 2**32 - 85_000_000         # TIM2 wraps 0.5 s into the record
HYST_CODES = 4.0                    # comparator thresholds at mid-scale +/- 2 codes
UPSAMPLE = 8                        # edges found on a 160 kHz band-limited interpolation
GLITCH_FRAC = 0.01                  # per record: 1 % extra close edge pairs, 1 % of edges dropped
F_LO, F_HI = 800.0, 3000.0          # est_fft's band
FREQS = (1064.0, 2128.875, 2767.5)

FIT_STATUS = {0: "OK", -1: "E_ARGS", -2: "E_NOPEAK", -3: "E_DIVERGED", -4: "E_DECAY"}
PA_STATUS = {0: "OK", -1: "E_ARGS", -2: "E_FIT", -3: "E_FEW"}


def cases():
    """(label, peak/rms, T2*, f). Peak/rms = A / NOISE_CODES."""
    out = []
    for snr, tag in ((54.8, ""), (31.6, ""), (19.1, ""), (6.7, ""), (5.3, " untuned"), (3.2, " untuned"),
                     (1.5, " untuned")):
        for f in FREQS:
            out.append((f"{snr:g}{tag}, T2 1 s, {f:g} Hz", snr, 1.0, f))
    for t2 in (0.3, 3.0):
        for f in FREQS:
            out.append((f"31.6, T2 {t2:g} s, {f:g} Hz", 31.6, t2, f))
    out.append(("noise only (expect E_NOPEAK)", 0.0, 1.0, 2128.875))
    return out


def noise_density():
    """en_adc (V/rtHz) that gives NOISE_CODES rms per sample after make_record's band mask."""
    h2 = FE.band_mask(N, FS) ** 2
    frac = (h2[0] + 2.0 * h2[1:-1].sum() + h2[-1]) / N       # variance kept by the mask (N even)
    en = NOISE_CODES * LSB / math.sqrt(FS / 2 * frac)
    return en, en * math.sqrt(FS / 2) / LSB                   # and the white-equivalent sigma, codes


def comparator(y, dt, lo, hi):
    """Hysteresis comparator: high once y rises above hi, low once it falls below lo. Switching times,
    linearly interpolated between the samples y[i] (spacing dt)."""
    up = np.flatnonzero((y[:-1] <= hi) & (y[1:] > hi))
    dn = np.flatnonzero((y[:-1] >= lo) & (y[1:] < lo))
    idx = np.concatenate([up, dn])
    kind = np.concatenate([np.ones(len(up), int), -np.ones(len(dn), int)])
    order = np.argsort(idx, kind="stable")
    state = 1 if y[0] > hi else 0
    times = []
    for i, k in zip(idx[order].tolist(), kind[order].tolist()):
        if k == 1 and state == 0:
            th, state = hi, 1
        elif k == -1 and state == 1:
            th, state = lo, 0
        else:
            continue
        times.append((i + (th - y[i]) / (y[i + 1] - y[i])) * dt)
    return np.array(times)


def make_case_record(a_codes, t2, f, seed, en):
    """One record: (codes uint16, edges uint32, truth dict)."""
    phi = float(np.random.default_rng([seed, 1]).uniform(0, 2 * np.pi))
    FE.rng = np.random.default_rng(seed)          # make_record draws its noise from the module's generator
    t, v = FE.make_record(a_codes * LSB, t2, en, f=f, phi=phi, n=N, quantize=False)
    codes = np.clip(np.round(v / LSB) + MID, 0, 4095).astype(np.uint16)

    # Comparator input: the same signal and noise between the samples. The noise is periodic and
    # band-limited by construction (irfft of a masked spectrum), so its FFT interpolation is exact.
    s = a_codes * LSB * np.exp(-t / t2) * np.sin(2 * np.pi * f * t + phi)
    noise = v - s
    w = np.fft.rfft(noise) * UPSAMPLE
    w[-1] *= 0.5                                   # the Nyquist bin splits between +/- fs/2
    wp = np.zeros(N * UPSAMPLE // 2 + 1, complex)
    wp[:len(w)] = w
    nf = np.fft.irfft(wp, N * UPSAMPLE)
    interp_err = float(np.max(np.abs(nf[::UPSAMPLE] - noise)) / LSB)
    tf = np.arange(N * UPSAMPLE) / (FS * UPSAMPLE)
    y = (a_codes * LSB * np.exp(-tf / t2) * np.sin(2 * np.pi * f * tf + phi) + nf) / LSB
    te = comparator(y, 1.0 / (FS * UPSAMPLE), -HYST_CODES / 2, HYST_CODES / 2)
    cnt = np.round(te * TICK_HZ).astype(np.int64)  # counts after t_rec0

    rs = np.random.default_rng([seed, 2])
    n_g = max(1, int(round(GLITCH_FRAC * len(cnt)))) if len(cnt) else 0
    if n_g:
        cnt = np.delete(cnt, rs.choice(len(cnt), size=n_g, replace=False))
        at = np.round(rs.uniform(0, N / FS, n_g) * TICK_HZ).astype(np.int64)
        gap = np.round(rs.uniform(2e-6, 20e-6, n_g) * TICK_HZ).astype(np.int64)
        cnt = np.sort(np.concatenate([cnt, at, at + gap]))
    edges = ((cnt + T_REC0) % 2**32).astype(np.uint32)
    truth = {"f": f, "A": a_codes, "T2": t2, "phi": phi, "seed": seed, "n_comparator_edges": len(te),
             "n_dropped": n_g, "n_extra": 2 * n_g, "interp_err_codes": interp_err,
             "noise_rms_codes": float(np.std(noise) / LSB)}
    return codes, edges, truth


def est_fit_traced(t, v, T2_0=1.0, iters=40):
    """est_fit line for line, also returning whether it stopped on |step_f| < 1e-7 and the iterations run."""
    f0 = FE.est_fft(t, v)
    e = np.exp(-t / T2_0)
    M = np.stack([e * np.sin(2 * np.pi * f0 * t), e * np.cos(2 * np.pi * f0 * t), np.ones_like(t)], axis=1)
    a, b, c = np.linalg.lstsq(M, v, rcond=None)[0]
    p = np.array([math.hypot(a, b), f0, math.atan2(b, a), 1 / T2_0, c])
    lam = 1e-3
    r = v - FE.model(p, t)
    cost = r @ r
    converged, n_it = False, 0
    for _ in range(iters):
        n_it += 1
        J = FE.jac(p, t)
        JTJ = J.T @ J
        g = J.T @ r
        step = np.linalg.solve(JTJ + lam * np.diag(np.diag(JTJ)), g)
        pn = p + step
        pn[3] = max(pn[3], 1e-3)
        rn = v - FE.model(pn, t)
        cn = rn @ rn
        if cn < cost:
            p, r, cost = pn, rn, cn
            lam = max(lam / 3, 1e-9)
            if abs(step[1]) < 1e-7:
                converged = True
                break
        else:
            lam *= 5
    return p[1], p, converged, n_it


def read_c_results(path):
    with open(path) as fh:
        head = fh.readline().strip().split(",")
        rows = []
        for line in fh:
            vals = line.strip().split(",")
            rows.append({k: (float(x) if any(ch in x for ch in ".eEn") else int(x)) for k, x in zip(head, vals)})
    return rows


def rms(x):
    x = np.asarray(x, float)
    return float(np.sqrt(np.mean(x ** 2))) if len(x) else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per-case", type=int, default=20, help="records per case (>= 10)")
    ap.add_argument("--seed", type=int, default=455_000, help="first noise seed")
    ap.add_argument("--no-build", action="store_true", help="skip make (out/fit_test.exe must exist)")
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    t_start = time.time()

    en, sigma_white = noise_density()
    case_list = cases()

    # ---- 1. records ---------------------------------------------------------------------------------
    recs = []
    with open(os.path.join(OUT, "records.bin"), "wb") as fb:
        n_total = len(case_list) * args.per_case
        fb.write(b"MAGFIT01" + struct.pack("<II4d", n_total, N, FS, TICK_HZ, F_LO, F_HI))
        for ci, (label, snr, t2, f) in enumerate(case_list):
            for ri in range(args.per_case):
                seed = args.seed + 1000 * ci + ri
                codes, edges, truth = make_case_record(snr * NOISE_CODES, t2, f, seed, en)
                fb.write(struct.pack("<iiII", ci, ri, T_REC0, len(edges)))
                fb.write(codes.astype("<u2").tobytes())
                fb.write(edges.astype("<u4").tobytes())
                truth.update(case=ci, rec=ri)
                recs.append((truth, codes))
    with open(os.path.join(OUT, "records_truth.csv"), "w") as fh:
        keys = ["case", "rec", "seed", "f", "A", "T2", "phi", "noise_rms_codes", "n_comparator_edges",
                "n_dropped", "n_extra", "interp_err_codes"]
        fh.write(",".join(keys) + "\n")
        for truth, _ in recs:
            fh.write(",".join(repr(truth[k]) for k in keys) + "\n")
    print(f"{len(recs)} records written ({time.time() - t_start:.0f} s); en_adc {en * 1e6:.3f} uV/rtHz, "
          f"white-equivalent sigma {sigma_white:.2f} codes")

    # ---- 2. build and run the C test ---------------------------------------------------------------
    if not args.no_build:
        b = subprocess.run(["make"], cwd=HERE, capture_output=True, text=True)
        print(b.stdout + b.stderr)
        if b.returncode != 0:
            sys.exit("make failed (a compile error or warning): see above")
    exe = os.path.join(OUT, "fit_test.exe")
    c_run = subprocess.run([exe, os.path.join(OUT, "records.bin"), os.path.join(OUT, "results_c.csv")],
                           cwd=HERE, capture_output=True, text=True)
    print(c_run.stdout + c_run.stderr)
    c_rows = read_c_results(os.path.join(OUT, "results_c.csv"))
    if len(c_rows) != len(recs):
        sys.exit(f"fit_test wrote {len(c_rows)} results for {len(recs)} records")

    # ---- 3. the reference on the same codes --------------------------------------------------------
    t = np.arange(N) / FS
    py_rows = []
    t_py = time.time()
    for (truth, codes) in recs:
        v = codes.astype(float)
        row = {"f_fft": float("nan"), "f_fit": float("nan"), "converged": False, "iters": 0, "same": True}
        try:
            row["f_fft"] = float(FE.est_fft(t, v))
            f_fit, p_fit = FE.est_fit(t, v)
            f_tr, p_tr, conv, n_it = est_fit_traced(t, v)
            row.update(f_fit=float(f_fit), converged=conv, iters=n_it, same=bool(np.array_equal(p_fit, p_tr)),
                       A=float(p_fit[0]), R=float(p_fit[3]))
        except np.linalg.LinAlgError as ex:
            row["error"] = str(ex)
        py_rows.append(row)
    print(f"reference fits: {time.time() - t_py:.0f} s")
    with open(os.path.join(OUT, "results_py.csv"), "w") as fh:
        fh.write("rec,f_fft,f_fit,converged,iterations,replica_identical\n")
        for i, r in enumerate(py_rows):
            fh.write(f"{i},{r['f_fft']!r},{r['f_fit']!r},{int(r['converged'])},{r['iters']},{int(r['same'])}\n")
    if not all(r["same"] for r in py_rows):
        sys.exit("est_fit_traced differs from est_fit: the convergence flags cannot be trusted")

    # ---- 4. comparison -----------------------------------------------------------------------------
    lines, summary = [], {"cases": [], "en_adc_V_rtHz": en, "sigma_white_codes": sigma_white,
                          "noise_codes": NOISE_CODES, "per_case": args.per_case,
                          "c_self_tests_passed": c_run.returncode == 0, "c_stdout": c_run.stdout}
    ok1 = ok2 = True
    hdr = (f"{'case':<34} {'n':>3} {'Py':>3} {'C':>3} {'CRLB':>8} {'CRLBref':>8} {'max|dC-Py|':>10} "
           f"{'rmsC':>8} {'rmsPy':>8} {'C/Py':>6} {'itC':>5} {'itPy':>5} {'seed':>8} {'sigC':>8}")
    lines += ["Fit: |f_C - f_Py| / CRLB on records where est_fit converged; rms errors against the true f.",
              "CRLB = crlb_f(A, T2, sigma = 23 codes, the per-sample rms); CRLBref = the same with the",
              "reference's white-equivalent sigma (en x sqrt(fs/2)). mHz unless marked; seed = max |f_seed_C -",
              "est_fft| in uHz; itC / itPy = mean iterations; sigC = mean f_sigma_hz of the C fit.", "", hdr,
              "-" * len(hdr)]
    pa_lines = ["", "Path A (patha_estimate with the C fit, noise_rms = fit.resid_rms), mHz:", "",
                f"{'case':<34} {'ok':>5} {'bias':>9} {'rms':>9} {'bias-fit':>9} {'rms-fit':>9} {'rise':>6} "
                f"{'fall':>6} {'rej':>5} {'status (count)':<20}"]
    pa_lines.append("-" * len(pa_lines[-1]))
    for ci, (label, snr, t2, f) in enumerate(case_list):
        idx = [i for i, (tr, _) in enumerate(recs) if tr["case"] == ci]
        cs = [c_rows[i] for i in idx]
        ps = [py_rows[i] for i in idx]
        f_true = f
        st = {}
        for c in cs:
            st[FIT_STATUS.get(c["fit_status"], str(c["fit_status"]))] = st.get(
                FIT_STATUS.get(c["fit_status"], str(c["fit_status"])), 0) + 1
        entry = {"label": label, "snr": snr, "T2": t2, "f": f, "n": len(idx), "c_status": st}
        pa_st = {}
        for c in cs:
            k = PA_STATUS.get(c["pa_status"], str(c["pa_status"]))
            pa_st[k] = pa_st.get(k, 0) + 1
        entry["pa_status"] = pa_st
        if snr == 0.0:
            entry["nopeak_ok"] = st.get("E_NOPEAK", 0) == len(idx)
            lines.append(f"{label:<34} {len(idx):>3}   C statuses: {st}  ->  "
                         f"{'as expected' if entry['nopeak_ok'] else 'NOT as expected'}")
            ok1 = ok1 and entry["nopeak_ok"]
            pa_lines.append(f"{label:<34} {'-':>5} {'':>9} {'':>9} {'':>9} {'':>9} {'':>6} {'':>6} {'':>5} {pa_st}")
            summary["cases"].append(entry)
            continue
        a_codes = snr * NOISE_CODES
        crlb = FE.crlb_f(a_codes, t2, NOISE_CODES, N)
        crlb_ref = FE.crlb_f(a_codes, t2, sigma_white, N)
        conv = [k for k in range(len(idx)) if ps[k]["converged"]]
        ratios = [abs(cs[k]["f_hz"] - ps[k]["f_fit"]) / crlb if cs[k]["fit_status"] == 0 else math.inf
                  for k in conv]
        ratios_all = [abs(cs[k]["f_hz"] - ps[k]["f_fit"]) / crlb for k in range(len(idx)) if cs[k]["fit_status"] == 0]
        both = [k for k in conv if cs[k]["fit_status"] == 0]
        e_c = [cs[k]["f_hz"] - f_true for k in both]
        e_p = [ps[k]["f_fit"] - f_true for k in both]
        rms_c, rms_p = rms(e_c), rms(e_p)
        max_ratio = max(ratios) if ratios else float("nan")
        seed_dev = max(abs(cs[k]["f_seed_hz"] - ps[k]["f_fft"]) for k in range(len(idx)) if cs[k]["fit_status"] == 0) \
            if any(c["fit_status"] == 0 for c in cs) else float("nan")
        pass1 = bool(ratios) and max_ratio < 0.1 and len(conv) > 0
        pass2 = bool(both) and abs(rms_c / rms_p - 1.0) <= 0.10
        ok1, ok2 = ok1 and pass1, ok2 and pass2
        it_c = np.mean([c["iterations"] for c in cs])
        it_p = np.mean([p["iters"] for p in ps])
        sig_c = np.mean([c["f_sigma_hz"] for c in cs if c["fit_status"] == 0]) if both else float("nan")
        entry.update(crlb_hz=crlb, crlb_ref_hz=crlb_ref, py_converged=len(conv), c_ok=st.get("OK", 0),
                     max_ratio=max_ratio, max_ratio_all=max(ratios_all) if ratios_all else None, rms_c_hz=rms_c, rms_py_hz=rms_p, bias_c_hz=float(np.mean(e_c)) if e_c else None,
                     bias_py_hz=float(np.mean(e_p)) if e_p else None, rms_ratio=rms_c / rms_p if both else None,
                     iters_c_mean=float(it_c), iters_c_max=int(max(c["iterations"] for c in cs)),
                     iters_py_mean=float(it_p), iters_py_max=int(max(p["iters"] for p in ps)),
                     seed_dev_max_hz=seed_dev, f_sigma_c_mean_hz=float(sig_c), pass_crlb=pass1, pass_rms=pass2)
        lines.append(f"{label:<34} {len(idx):>3} {len(conv):>3} {st.get('OK', 0):>3} {crlb * 1e3:>8.4f} "
                     f"{crlb_ref * 1e3:>8.4f} {max_ratio:>10.2e} {rms_c * 1e3:>8.4f} {rms_p * 1e3:>8.4f} "
                     f"{rms_c / rms_p if both else float('nan'):>6.4f} {it_c:>5.1f} {it_p:>5.1f} "
                     f"{seed_dev * 1e6:>8.3f} {sig_c * 1e3:>8.4f}"
                     + ("" if pass1 and pass2 else "   <-- FAIL"))

        okpa = [k for k in range(len(idx)) if cs[k]["pa_status"] == 0]
        ea = [cs[k]["pa_f_hz"] - f_true for k in okpa]
        ef = [cs[k]["pa_f_hz"] - cs[k]["f_hz"] for k in okpa]
        entry.update(pa_ok=len(okpa), pa_bias_hz=float(np.mean(ea)) if ea else None, pa_rms_hz=rms(ea) if ea else None,
                     pa_bias_vs_fit_hz=float(np.mean(ef)) if ef else None, pa_rms_vs_fit_hz=rms(ef) if ef else None,
                     pa_n_rise=float(np.mean([cs[k]["pa_n_rise"] for k in okpa])) if okpa else None,
                     pa_n_fall=float(np.mean([cs[k]["pa_n_fall"] for k in okpa])) if okpa else None,
                     pa_n_rejected=float(np.mean([cs[k]["pa_n_rejected"] for k in okpa])) if okpa else None)
        if okpa:
            pa_lines.append(f"{label:<34} {len(okpa):>2}/{len(idx):<2} {np.mean(ea) * 1e3:>9.3f} {rms(ea) * 1e3:>9.3f} "
                            f"{np.mean(ef) * 1e3:>9.3f} {rms(ef) * 1e3:>9.3f} {entry['pa_n_rise']:>6.0f} "
                            f"{entry['pa_n_fall']:>6.0f} {entry['pa_n_rejected']:>5.1f} {pa_st}")
        else:
            pa_lines.append(f"{label:<34} {0:>2}/{len(idx):<2} {'':>9} {'':>9} {'':>9} {'':>9} {'':>6} {'':>6} {'':>5} "
                            f"{pa_st}")
        summary["cases"].append(entry)

    worst = max((c["max_ratio"] for c in summary["cases"] if "max_ratio" in c), default=float("nan"))
    worst_all = max((c["max_ratio_all"] for c in summary["cases"] if c.get("max_ratio_all") is not None),
                    default=float("nan"))
    n_sig = sum(c["n"] for c in summary["cases"] if c["snr"] > 0)
    n_unconv = sum(c["n"] - c["py_converged"] for c in summary["cases"] if c["snr"] > 0)
    rr = [c["rms_ratio"] for c in summary["cases"] if c.get("rms_ratio") is not None]
    lines += ["", f"Acceptance 1, |f_C - f_Py| < 0.1 CRLB on every converged record: "
                  f"{'PASS' if ok1 else 'FAIL'} (largest ratio {worst:.3e})",
              f"  (information: over all {n_sig} signal records, including the {n_unconv} where est_fit used all 40 "
              f"iterations, the largest ratio is {worst_all:.3e})",
              f"Acceptance 2, rms error of C within 10 % of the reference per case: {'PASS' if ok2 else 'FAIL'} "
              f"(C/Py from {min(rr):.4f} to {max(rr):.4f})",
              f"C self-tests: {'PASS' if c_run.returncode == 0 else 'FAIL'}"]
    summary.update(pass_crlb=ok1, pass_rms=ok2, worst_ratio=worst, worst_ratio_all=worst_all,
                   py_unconverged_signal_records=n_unconv, rms_ratio_min=min(rr), rms_ratio_max=max(rr),
                   noise_rms_codes_mean=float(np.mean([tr["noise_rms_codes"] for tr, _ in recs])),
                   interp_err_codes_max=float(max(tr["interp_err_codes"] for tr, _ in recs)),
                   runtime_s=time.time() - t_start)
    lines.append(f"Generated noise: mean per-record rms {summary['noise_rms_codes_mean']:.2f} codes; edge "
                 f"interpolation check: max |noise(upsampled) - noise| at the samples "
                 f"{summary['interp_err_codes_max']:.2e} codes")
    text = "\n".join(lines + pa_lines) + "\n"
    print(text)
    with open(os.path.join(OUT, "summary.txt"), "w") as fh:
        fh.write(c_run.stdout + "\n" + text)
    with open(os.path.join(OUT, "summary.json"), "w") as fh:
        json.dump(summary, fh, indent=1)
    print(f"total {time.time() - t_start:.0f} s; out/summary.txt, out/summary.json")
    sys.exit(0 if (ok1 and ok2 and c_run.returncode == 0) else 1)


if __name__ == "__main__":
    main()
