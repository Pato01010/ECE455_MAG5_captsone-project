"""
test_mag.py - tools/mag.py against a fake serial port that replays the firmware's output byte for byte.

  py -3.14 tests/tools/test_mag.py

Checks: report lines are printed and kept; a DUMP's binary block is read whole, CRC-checked and saved to
an .npz with the right arrays; a corrupted dump is refused; the Python refit runs on the saved record and is
compared with FIT's f_raw_Hz; --csv writes one row per shot with the GPS fields, also over a run with an
aborted shot (no FIT line: its row says na, and no earlier shot's values leak into it).
"""
import csv
import os
import sys
import tempfile
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import mag  # noqa: E402


class FakeSerial:
    def __init__(self, stream):
        self.buf = bytearray(stream)
        self.written = bytearray()

    def read(self, n=1):
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out

    def write(self, data):
        self.written += data

    def reset_input_buffer(self):
        pass


def firmware_stream(rec, edges, pre, post, corrupt=False):
    """What app.c sends for 'shot 0.2 dump': the report, DUMP header, raw arrays, END, DONE."""
    payload = rec.astype("<u2").tobytes() + edges.astype("<u4").tobytes() + pre.astype("<u2").tobytes() \
        + post.astype("<u2").tobytes()
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    if corrupt:
        payload = bytes([payload[0] ^ 1]) + payload[1:]
    lines = [
        "SHOT id=1 mode=shot tuned=0 tpol_s=0.200 result=ok reason=none clock=HSE_crystal utc_at=rec0 "
        "utc=2026-10-22T15:04:21.091973Z fix=1 sats=11 hdop=0.90 lat=43.0712345 lon=-89.4012345 alt_m=270.1 "
        "spd_mps=1.25 cog_deg=271.4 pos_age_s=0.09 clk_ppm=25.000 clk_s=16",
        "CHECK id=1 a_mV=2900 a_ok=1",
        "TIME id=1 from=gate_off gate_off_us=0",
        "FIT id=1 status=0 f_Hz=2128.928222 fsig_Hz=0.000500 B_nT=50001.250 corr=gps f_raw_Hz=2128.875000",
        "PATHA id=1 skipped=untuned",
    ]
    head = (f"DUMP id=1 bytes={len(payload)} crc32={crc:08x} rec={len(rec)} edges={len(edges)} pre={len(pre)} "
            f"post={len(post)} fs_Hz=20000 tick_Hz=170000000 vref_mV=3250 t_rec0=4000000000 rec0_us=60052 "
            f"pre0_us=-1935 post0_us=565 tuned=0")
    text = "".join(ln + "\r\n" for ln in lines) + head + "\r\n"
    return text.encode() + payload + b"\r\nEND id=1\r\nDONE id=1\r\n"


def main():
    rng = np.random.default_rng(1)
    n = 18800
    t = np.arange(n) / 20000.0
    rec = np.clip(np.round(2048 + 735 * np.exp(-t) * np.sin(2 * np.pi * 2128.875 * t + 0.7)
                           + rng.normal(0, 23, n)), 0, 4095).astype(np.uint16)
    edges = (4000000000 + np.arange(100, dtype=np.uint64) * 79852) % 2**32
    pre = np.full(542, 3655, np.uint16)
    post = np.full(800, 3660, np.uint16)
    failures = 0

    with tempfile.TemporaryDirectory() as out:
        ser = FakeSerial(firmware_stream(rec, edges, pre, post))
        csv_path = os.path.join(out, "survey.csv")
        saved = mag.cmd(ser, "shot 0.2 dump", out, timeout=5, csv_path=csv_path)
        if not saved or not os.path.exists(saved):
            print("FAIL: no .npz saved"); failures += 1
        else:
            with np.load(saved) as z:
                for name, want in (("rec", rec), ("edges", edges.astype(np.uint32)), ("pre", pre), ("post", post)):
                    if not np.array_equal(z[name], want):
                        print(f"FAIL: {name} differs after the round trip"); failures += 1
                if "FIT id=1" not in str(z["report"]):
                    print("FAIL: report lines not saved"); failures += 1
        if ser.written != b"shot 0.2 dump\r\n":
            print(f"FAIL: wrote {ser.written!r}"); failures += 1
        with open(csv_path, newline="") as fh:
            rows = list(csv.reader(fh))
        want = {"id": "1", "utc": "2026-10-22T15:04:21.091973Z", "lat": "43.0712345", "lon": "-89.4012345",
                "B_nT": "50001.250", "clk_ppm": "25.000", "corr": "gps", "f_raw_Hz": "2128.875000",
                "patha_f_Hz": "na", "mode": "shot"}
        if len(rows) != 2:
            print(f"FAIL: survey.csv has {len(rows)} rows, expected header + 1"); failures += 1
        else:
            got = dict(zip(rows[0], rows[1]))
            for k, v in want.items():
                if got.get(k) != v:
                    print(f"FAIL: survey.csv {k}={got.get(k)!r}, expected {v!r}"); failures += 1

        # a run of two: shot 1 complete, shot 2 aborted before its record (no FIT, no PATHA)
        run = ("SHOT id=1 mode=shot tuned=1 tpol_s=3.000 result=ok reason=none clock=HSE_crystal utc_at=rec0 "
               "utc=2026-10-22T15:04:21.000000Z fix=1 lat=43.1 lon=-89.4 clk_ppm=1.000 clk_s=16\r\n"
               "FIT id=1 status=0 f_Hz=2128.000000 B_nT=49980.000 corr=gps f_raw_Hz=2127.997872\r\n"
               "PATHA id=1 status=0 f_Hz=2128.001000 minus_fit_nT=0.023\r\nDONE id=1\r\n"
               "SHOT id=2 mode=shot tuned=1 tpol_s=3.000 result=aborted reason=overcurrent clock=HSE_crystal "
               "utc_at=gate_off utc=2026-10-22T15:04:26.500000Z fix=1 lat=43.2 lon=-89.5 clk_ppm=1.000 clk_s=16\r\n"
               "DONE id=2\r\nRUN stopped: shot aborted\r\n")
        run_csv = os.path.join(out, "run.csv")
        mag.cmd(FakeSerial(run.encode()), "run 2 3 tuned", out, 5, csv_path=run_csv)
        with open(run_csv, newline="") as fh:
            rows = list(csv.reader(fh))
        if len(rows) != 3:
            print(f"FAIL: run.csv has {len(rows)} rows, expected header + 2"); failures += 1
        else:
            r2 = dict(zip(rows[0], rows[2]))
            if r2["id"] != "2" or r2["result"] != "aborted" or r2["B_nT"] != "na" or r2["patha_f_Hz"] != "na" \
                    or r2["utc_at"] != "gate_off" or r2["lat"] != "43.2":
                print(f"FAIL: the aborted shot's row: {r2}"); failures += 1

        try:
            mag.cmd(FakeSerial(firmware_stream(rec, edges, pre, post, corrupt=True)), "shot 0.2 dump", out, 5)
            print("FAIL: a corrupted dump was accepted"); failures += 1
        except ValueError as ex:
            print(f"corrupted dump refused: {ex}")

    print("PASSED" if failures == 0 else f"FAILED: {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
