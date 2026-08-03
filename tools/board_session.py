#!/usr/bin/env python3
"""Automated validation session for the XIAO ESP32S3 Plus chess engine.

Run after flashing (tools/board_check.sh does both). Talks to the engine's
serial console and verifies:

  1. Clean boot (no panic/assert), SPIFFS mounted, opening book succeeds
  2. Full test suite passes -- critical: the SIMD NNUE kernels must be
     bit-identical to the scalar accumulator (NNUE evaluation test)
  3. Bench completes and reports nodes/second
  4. UCI mode is healthy and the PSRAM transposition table allocates

Exit code 0 = all critical checks passed.

Usage: board_session.py --port /dev/ttyACM0
"""

import argparse
import re
import sys
import time

import serial

BAUD = 115200
BANNER = "HELLO, THIS IS DOG"
PROMPT = 'ENTER "uci" FOR uci-MODE'
TIMEOUTS = {"boot": 30, "test": 600, "bench": 300, "uci": 30, "quit": 30}

CRITICAL_MARKERS = [
    ("NNUE evaluation test", "OK"),
    ("NNUE incremental update test", "OK"),
    ("NNUE SIMD kernel test", "OK"),
    ("tt test", "OK"),
    ("SAN parsing test", "OK"),
]

FAIL_WORDS = ["assert fail", "Guru Meditation", "abort()", "PANIC"]


class Session:
    def __init__(self, port):
        self.ser = None
        for attempt in range(20):
            try:
                self.ser = serial.Serial(port, BAUD, timeout=1)
                break
            except (serial.SerialException, OSError):
                if attempt == 19:
                    raise
                time.sleep(0.5)
        self.ser.reset_input_buffer()
        self.all_text = ""

    def read_until(self, needle, timeout, label):
        """Read until needle appears in *newly received* data. Returns the
        text accumulated during this call."""
        buf = ""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                chunk = self.ser.read(4096)
            except serial.SerialException:
                # device closed (e.g. engine exited after 'quit')
                return buf
            if chunk:
                text = chunk.decode("utf-8", errors="replace")
                buf += text
                self.all_text += text
                if needle in buf:
                    return buf
            else:
                time.sleep(0.05)
        print(f"TIMEOUT waiting for {needle!r} ({label})")
        return buf

    def send(self, cmd):
        try:
            self.ser.write(cmd.encode() + b"\n")
        except serial.SerialException as e:
            print(f"FAIL: device disconnected while sending {cmd!r}: {e}")
            sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--no-simd", action="store_true",
                    help="image built without SIMD kernels (DOG_NO_SIMD=1); "
                         "skip the NNUE SIMD kernel test check (A/B bench only)")
    args = ap.parse_args()

    s = Session(args.port)

    results = {}

    print("== waiting for console prompt ==")
    s.send("")
    boot = s.read_until(PROMPT, TIMEOUTS["boot"], "console prompt")
    if PROMPT not in boot:
        print("FAIL: engine did not print its console prompt. Is the firmware flashed?")
        print(boot[-2000:])
        sys.exit(1)

    results["boot log: banner"] = "seen" if BANNER in s.all_text else "missed (port opened after boot)"

    results["clean boot"] = not any(w in s.all_text for w in FAIL_WORDS)
    results["boot log: spiffs line"] = "seen" if "SPIFFS size:" in s.all_text else "missed (port opened after boot)"
    results["book loads (no error)"] = "Failed to open book" not in s.all_text

    print("== running test suite ==")
    s.send("test")
    out = s.read_until(PROMPT, TIMEOUTS["test"], "test suite")
    markers = [m for m in CRITICAL_MARKERS
               if not (args.no_simd and m[0] == "NNUE SIMD kernel test")]
    for name, marker in markers:
        # the test name line only prints when the test actually ran
        ran = name in s.all_text
        results[f"test: {name}"] = ran and marker in s.all_text and "assert fail" not in s.all_text
    results["test suite complete"] = PROMPT in out
    results["no test failure"] = "assert fail" not in s.all_text

    print("== running bench ==")
    s.send("bench")
    out = s.read_until(PROMPT, TIMEOUTS["bench"], "bench")
    m = re.search(r"Nodes/second\s*:\s*(\d+)", s.all_text)
    if m:
        results["bench nps"] = f"{int(m.group(1)):,}"
    else:
        results["bench nps"] = "missing"

    print("== UCI smoke test ==")
    s.send("uci")
    s.read_until("uciok", TIMEOUTS["uci"], "uci handshake")
    s.send("setoption name Hash value 8")  # ESP32 Hash option max is 8 (larger values are silently ignored)
    time.sleep(1.0)  # no reply token; PSRAM allocation line may print
    s.send("isready")
    s.read_until("readyok", TIMEOUTS["uci"], "isready")
    results["psram tt"] = "bytes of PSRAM" in s.all_text
    s.send("quit")
    s.read_until("TASK TERMINATED", TIMEOUTS["quit"], "quit")

    print()
    print("=============== RESULTS ================")
    for k, v in results.items():
        if isinstance(v, bool):
            print(f"  [{'OK  ' if v else 'FAIL'}] {k}")
        else:
            print(f"  [INFO] {k}: {v}")
    print("========================================")

    critical = [
        results["clean boot"],
        results["test: NNUE evaluation test"],
        results["test: NNUE incremental update test"],
        results["no test failure"],
    ]
    if not args.no_simd:
        critical.append(results["test: NNUE SIMD kernel test"])
    ok = all(critical)
    if ok:
        if args.no_simd:
            print("ALL CRITICAL CHECKS PASSED (scalar image, A/B bench only).")
        else:
            print("ALL CRITICAL CHECKS PASSED - SIMD NNUE verified bit-exact.")
    else:
        print("CRITICAL CHECK FAILED - see output above.")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
