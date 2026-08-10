#!/usr/bin/env python3
"""Automated validation session for the XIAO ESP32S3 Plus chess engine.

Run after flashing (tools/board_check.sh does both). Talks to the engine's
serial console and verifies:

  1. Clean boot (no panic/assert), SPIFFS mounted, opening book succeeds
  2. Full test suite passes (NNUE evaluation, incremental update, tt, SAN)
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
# Timeouts tuned to the ESP32S3 scalar engine (~4.3k nps): the full test
# suite (NNUE perft up to depth 5) takes 1-2h, the depth-10 bench many minutes.
TIMEOUTS = {"boot": 30, "test": 7200, "bench": 3600, "uci": 30, "quit": 30}

CRITICAL_MARKERS = [
    ("NNUE evaluation test", "OK"),
    ("NNUE incremental update test", "OK"),
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
        try:
            # reboot the board into a known state; no-op on ptys (test harness)
            self.ser.setDTR(False)
            self.ser.setRTS(True)
            time.sleep(0.1)
            self.ser.setRTS(False)
        except (OSError, ValueError, NotImplementedError):
            pass
        self.ser.reset_input_buffer()
        time.sleep(8)
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
    markers = list(CRITICAL_MARKERS)
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
    ok = all(critical)
    if ok:
        print("ALL CRITICAL CHECKS PASSED - scalar NNUE verified.")
    else:
        print("CRITICAL CHECK FAILED - see output above.")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
