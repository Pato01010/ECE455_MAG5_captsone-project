"""
mag.py - talk to the magnetometer firmware over the Nucleo's ST-LINK virtual COM port.

  py -3.14 tools/mag.py ports                         list serial ports
  py -3.14 tools/mag.py term                          interactive terminal (Ctrl+C to quit)
  py -3.14 tools/mag.py cmd "shot 3 tuned dump"       one command; prints the report; with "dump",
                                                      saves the record and refits it in Python
  py -3.14 tools/mag.py cmd "run 100 3 tuned gap=0" --csv survey.csv
                                                      a survey: one CSV row per shot (UTC, position,
                                                      field, clock correction), appended as each ends
  options: --port COM5 (default: the first ST-LINK port), --baud 115200, --out shots, --timeout 30,
           --csv FILE

A saved shot is shots/shot_<id>_<time>.npz: rec (ADC codes), edges (TIM2 counts), pre / post
(XFER_SENSE codes) and the header and report lines. The refit uses est_fit from
simulations/frequency-estimation/frequency_estimation.py on the same codes, so the firmware's
FIT line and the Python answer can be compared shot by shot. Both work on the nominal clock: the
comparison uses FIT's f_raw_Hz, and FIT's f_Hz is that times the GPS-measured clock (clk_ppm in SHOT).
"""
import csv
import argparse
import os
import sys
import time
import zlib

import numpy as np

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover
    sys.exit("pyserial is needed: it is installed for py -3.14 on this laptop")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
REPORT_KEYS = ("SHOT", "CHECK", "TIME", "FIT", "PATHA", "SELFTEST", "STATUS", "CONFIG", "ERR", "ABORT", "RUN",
               "CLOCK", "MAGNETOMETER", "GPS", "NMEA")

# One survey row per shot: (CSV column, report line, key). "na" where the firmware had no value.
CSV_COLUMNS = (
    ("id", "SHOT", "id"), ("utc", "SHOT", "utc"), ("utc_at", "SHOT", "utc_at"),
    ("lat", "SHOT", "lat"), ("lon", "SHOT", "lon"), ("alt_m", "SHOT", "alt_m"), ("fix", "SHOT", "fix"),
    ("sats", "SHOT", "sats"), ("hdop", "SHOT", "hdop"), ("spd_mps", "SHOT", "spd_mps"),
    ("cog_deg", "SHOT", "cog_deg"), ("pos_age_s", "SHOT", "pos_age_s"),
    ("mode", "SHOT", "mode"), ("result", "SHOT", "result"), ("reason", "SHOT", "reason"), ("tuned", "SHOT", "tuned"),
    ("tpol_s", "SHOT", "tpol_s"), ("clock", "SHOT", "clock"), ("clk_ppm", "SHOT", "clk_ppm"),
    ("B_nT", "FIT", "B_nT"), ("Bsig_nT", "FIT", "Bsig_nT"), ("f_Hz", "FIT", "f_Hz"), ("fsig_Hz", "FIT", "fsig_Hz"),
    ("f_raw_Hz", "FIT", "f_raw_Hz"), ("corr", "FIT", "corr"), ("snr", "FIT", "snr"), ("A_mV", "FIT", "A_mV"),
    ("T2_s", "FIT", "T2_s"), ("patha_f_Hz", "PATHA", "f_Hz"), ("patha_minus_fit_nT", "PATHA", "minus_fit_nT"),
)


def csv_row(lines):
    """lines: {"SHOT": kv, "FIT": kv, "PATHA": kv} of one shot -> the CSV row (missing = "na")."""
    return [lines.get(tag, {}).get(key, "na") for _, tag, key in CSV_COLUMNS]


def csv_append(path, row):
    new = not os.path.exists(path) or os.path.getsize(path) == 0
    with open(path, "a", newline="", encoding="ascii") as fh:
        w = csv.writer(fh)
        if new:
            w.writerow([c for c, _, _ in CSV_COLUMNS])
        w.writerow(row)


def find_port():
    for p in list_ports.comports():
        text = f"{p.description} {p.manufacturer or ''}".lower()
        if "stlink" in text or "st-link" in text or (p.vid == 0x0483):
            return p.device
    return None


def parse_kv(line):
    out = {}
    for tok in line.split()[1:]:
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def read_line(ser, deadline):
    buf = bytearray()
    while time.time() < deadline:
        c = ser.read(1)
        if not c:
            continue
        if c == b"\n":
            return buf.decode("ascii", "replace").rstrip("\r")
        buf += c
    raise TimeoutError("no complete line before the timeout")


def read_dump(ser, header, deadline):
    kv = parse_kv(header)
    n = int(kv["bytes"])
    data = bytearray()
    while len(data) < n and time.time() < deadline:
        data += ser.read(n - len(data))
    if len(data) != n:
        raise TimeoutError(f"dump: {len(data)} of {n} bytes")
    crc = zlib.crc32(data) & 0xFFFFFFFF
    if crc != int(kv["crc32"], 16):
        raise ValueError(f"dump CRC {crc:08x}, header says {kv['crc32']}")
    n_rec, n_edges, n_pre, n_post = (int(kv[k]) for k in ("rec", "edges", "pre", "post"))
    o = 0
    rec = np.frombuffer(data, "<u2", n_rec, o); o += 2 * n_rec
    edges = np.frombuffer(data, "<u4", n_edges, o); o += 4 * n_edges
    pre = np.frombuffer(data, "<u2", n_pre, o); o += 2 * n_pre
    post = np.frombuffer(data, "<u2", n_post, o)
    return kv, rec, edges, pre, post


def python_refit(rec, fs):
    """est_fit from the project's reference, on the same codes."""
    sys.path.insert(0, os.path.join(ROOT, "simulations", "frequency-estimation"))
    sys.path.insert(0, os.path.join(ROOT, "simulations", "tools"))
    import frequency_estimation as FE  # noqa: E402
    t = np.arange(len(rec)) / fs
    v = rec.astype(float)
    f, p = FE.est_fit(t, v, f0=FE.est_fft(t, v))
    return f, p


def cmd(ser, line, out_dir, timeout, csv_path=None):
    """Send one command. A shot's report ends with DONE; anything else ends after 0.5 s of quiet.
    With csv_path, each finished shot appends one row (CSV_COLUMNS)."""
    ser.reset_input_buffer()
    ser.write((line.strip() + "\r\n").encode("ascii"))
    words = line.split()
    expect_shot = bool(words) and words[0] in ("shot", "dry", "run")
    shots_left = int(words[1]) if words and words[0] == "run" and len(words) > 1 else 1
    deadline = time.time() + (timeout if expect_shot else 2.0)
    report, saved, fit_line = [], None, None
    shot_lines = {}                     # SHOT / FIT / PATHA of the shot being reported
    while time.time() < deadline:
        try:
            ln = read_line(ser, deadline if expect_shot else min(deadline, time.time() + 0.5))
        except TimeoutError:
            break
        if not ln:
            continue
        if ln.startswith("DUMP"):
            kv, rec, edges, pre, post = read_dump(ser, ln, deadline)
            stamp = time.strftime("%Y%m%d-%H%M%S")
            os.makedirs(out_dir, exist_ok=True)
            saved = os.path.join(out_dir, f"shot_{kv['id']}_{stamp}.npz")
            np.savez(saved, rec=rec, edges=edges, pre=pre, post=post, header=ln,
                     report="\n".join(report))
            print(f"saved {saved}  ({len(rec)} samples, {len(edges)} edges)")
            if fit_line is not None and len(rec) > 0:
                fw = parse_kv(fit_line)
                f_py, _ = python_refit(rec, float(kv["fs_Hz"]))
                try:
                    f_fw = fw.get("f_raw_Hz", fw["f_Hz"])     # the record is on the nominal clock
                    d = (float(f_fw) - f_py) * 1e3
                    print(f"python est_fit {f_py:.6f} Hz; firmware (nominal clock) {f_fw} Hz; "
                          f"difference {d:+.4f} mHz")
                except (KeyError, ValueError):
                    print(f"python est_fit {f_py:.6f} Hz")
            continue
        print(ln)
        if ln.split(" ", 1)[0] in REPORT_KEYS:
            report.append(ln)
        tag = ln.split(" ", 1)[0]
        if tag == "SHOT":                   # a new shot: nothing of the last one may be used for it
            shot_lines = {}
            fit_line = None
        if tag == "FIT":
            fit_line = ln
        if tag in ("SHOT", "FIT", "PATHA"):
            shot_lines[tag] = parse_kv(ln)
        if ln.startswith("DONE") and csv_path and "SHOT" in shot_lines:
            csv_append(csv_path, csv_row(shot_lines))
            shot_lines = {}
        if expect_shot and ln.startswith("DONE"):
            shots_left -= 1
            deadline = time.time() + timeout
        if expect_shot and (shots_left <= 0 or ln.startswith(("ERR", "RUN stopped"))):
            break
    return saved


def term(ser):
    import threading

    def reader():
        while True:
            data = ser.read(256)
            if data:
                sys.stdout.write(data.decode("ascii", "replace"))
                sys.stdout.flush()

    threading.Thread(target=reader, daemon=True).start()
    try:
        for line in sys.stdin:
            ser.write((line.rstrip("\n") + "\r\n").encode("ascii"))
    except KeyboardInterrupt:
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=("ports", "term", "cmd"))
    ap.add_argument("command", nargs="?", default="")
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", default="shots")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--csv", help="append one row per finished shot to this CSV file")
    a = ap.parse_args()

    if a.what == "ports":
        for p in list_ports.comports():
            print(f"{p.device:8} {p.description}")
        return
    port = a.port or find_port()
    if not port:
        sys.exit("no ST-LINK port found: plug the Nucleo in, or give --port")
    with serial.Serial(port, a.baud, timeout=0.05) as ser:
        if a.what == "term":
            term(ser)
        else:
            if not a.command:
                sys.exit('give the command, e.g. cmd "shot 3 dump"')
            cmd(ser, a.command, a.out, a.timeout, csv_path=a.csv)


if __name__ == "__main__":
    main()
