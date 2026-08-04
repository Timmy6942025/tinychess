#!/usr/bin/env python3
"""End-to-end test of tools/board_session.py against a simulated engine.

Spins up the *real* board_session.py against a pty that speaks the engine's
console protocol, and asserts the session's exit code / verdict:

  * a healthy scalar image   -> board_session exits 0 ("ALL CRITICAL CHECKS PASSED")
  * a broken image (assert)  -> board_session exits 1 ("CRITICAL CHECK FAILED")

Run:  python3 tools/test_board_session.py
"""

import os
import pty
import select
import subprocess
import sys
import threading
import time

BAUD = 115200
PROMPT = ('# ENTER "uci" FOR uci-MODE, "test" TO RUN THE UNIT TESTS,\n'
          '# "quit" TO QUIT, "bench [long]" for the benchmark, "info" for build info\n'
          '# "bps ..." set serial baudrate\n')

TEST_OUT = """Size of int must be 32 bit
OK
tt move conversion
OK
NNUE perft
OK
NNUE incremental update test
OK
move sorting & generation test
OK
tt test
OK
is_insufficient_material_draw test
OK
SAN parsing test
OK
NNUE evaluation test
OK
"""

BOARD_SESSION = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "board_session.py")


def mock_engine(master, fail):
    """Speak the engine console protocol on the pty master. `fail` injects an
    assert during 'test' to exercise the failure-detection path."""
    def say(text):
        os.write(master, text.encode())

    # Wait for the first keystroke from board_session (it sends "" -> "\n");
    # writing the banner before it connects would be lost by reset_input_buffer.
    buf = b""
    while True:
        r, _, _ = select.select([master], [], [], 60)
        if not r:
            return
        data = os.read(master, 4096)
        if not data:
            continue
        buf += data
        if b"\n" in buf:
            break

    say("SPIFFS size: 5177344, free: 5049088\n")
    say("# HELLO, THIS IS DOG\n")
    say(PROMPT)

    pending = buf.split(b"\n")[-1]
    while True:
        if b"\n" in pending:
            line, pending = pending.split(b"\n", 1)
            cmd = line.strip().decode(errors="replace")
            if cmd == "test":
                say(TEST_OUT)
                if fail:
                    say("assert fail at line 123 (tests) in ./main/test.cpp\n")
                say(PROMPT)
            elif cmd == "bench":
                say("===========================\n")
                say("Total time (ms) : 2500\n")
                say("Nodes searched  : 543210\n")
                say("Nodes/second    : 217284\n")
                say(PROMPT)
            elif cmd == "uci":
                say("id name Dog 4.10.2\nid author Folkert van Heusden\nuciok\n")
            elif cmd.startswith("setoption"):
                say("Using 67108864 bytes of PSRAM\n")
            elif cmd == "isready":
                say("readyok\n")
            elif cmd == "quit":
                say("TASK TERMINATED\n")
                return
        r, _, _ = select.select([master], [], [], 60)
        if not r:
            continue
        data = os.read(master, 4096)
        if not data:
            continue
        pending += data


def run_case(fail, expect_pass):
    master, slave = pty.openpty()
    tty = os.ttyname(slave)
    t = threading.Thread(target=mock_engine, args=(master, fail), daemon=True)
    t.start()
    time.sleep(0.3)

    try:
        proc = subprocess.run(
            ["python3", BOARD_SESSION, "--port", tty],
            capture_output=True, text=True, timeout=120,
        )
    finally:
        try:
            os.close(master)
        except OSError:
            pass
        os.close(slave)

    ok = (proc.returncode == 0) == expect_pass
    verdict = "PASS" if ok else "FAIL"
    print(f"[{verdict}] {'passing' if expect_pass else 'failing'} image "
          f"-> exit {proc.returncode} (expected {0 if expect_pass else 1})")
    print(proc.stdout)
    if proc.stderr:
        print("STDERR:", proc.stderr)
    return ok


def main():
    all_ok = True
    all_ok &= run_case(fail=False, expect_pass=True)
    all_ok &= run_case(fail=True, expect_pass=False)
    if all_ok:
        print("ALL BOARD_SESSION E2E CHECKS PASSED")
        sys.exit(0)
    print("BOARD_SESSION E2E CHECKS FAILED")
    sys.exit(1)


if __name__ == "__main__":
    main()
