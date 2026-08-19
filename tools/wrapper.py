#!/usr/bin/env python3
"""Serial bridge: forwards cutechess-cli's stdin/stdout UCI session to the
XIAO ESP32S3 Plus over USB-serial.

Usage:  cutechess-cli -engine cmd=wrapper.py arg1=/dev/ttyACM0 ...

The engine boots into its console and enters UCI mode on "uci"; the wrapper
swallows the pre-UCI boot banner and the console prompt so only UCI output
reaches cutechess. Never buffers past a newline; flushes after every write.

Why no reset: the board reboots itself when it receives "quit" (main_task ->
esp_restart). The old reset dance (RTS pulse + close + reopen at 1.2 s, i.e.
mid boot) wedged the chip's USB-JTAG input endpoint: every write to the tty
blocked forever and matches hung. Opening the port fresh (rts/dtr deasserted)
and waiting for the boot banner - or 2 s of silence if the board is already
idle at its console prompt - is all that is needed.

Why select(0.005): the select timeout is the dominant per-move latency in
board-vs-native matches at ultra-fast TC. At 0.5 s the round trip
(cutechess -> wrapper -> board -> wrapper -> cutechess) added up to ~1 s per
move, so the board's 2+0.02 clock died after ~9 moves and every game was lost
on time (the -523.4 Elo anchor match was 40/40 clock forfeits - see
results.log correction). 0.005 s keeps the round trip at ~30-50 ms.
"""

import argparse
import os
import select
import sys
import time

import re
import serial

BAUD = 115200
BANNER_END = "ENTER \"uci\" FOR uci-MODE"  # console prompt line

parser = argparse.ArgumentParser()
parser.add_argument("port", nargs="?", default=os.environ.get("PORT", "/dev/ttyACM1"))
args = parser.parse_args()

# Enable the board's internal pondering after the uci handshake when set.
# The board ponders the predicted line on the opponent's clock (warms the TT),
# invisible to cutechess either way. PONDER=1 is used for the A/B gate.
PONDER = os.environ.get("PONDER", "0") == "1"


def open_port(port):
    for attempt in range(50):
        try:
            # The constructor asserts DTR/RTS, which pulses the board's
            # USB-JTAG reset line (clean boot per game). Deassert right away
            # so the board is not held in reset.
            ser = serial.Serial(port, BAUD, timeout=0.05)
            try:
                ser.setDTR(False)
                ser.setRTS(False)
            except (OSError, ValueError, NotImplementedError):
                pass
            return ser
        except (serial.SerialException, OSError):
            if attempt == 49:
                raise
            time.sleep(0.2)
    raise RuntimeError("cannot open serial port")


ser = open_port(args.port)

# Drain the boot banner + console prompt. A fresh open always pulses the
# board's reset line, so a boot banner always follows - but the ESP32S3 boot
# can take a while (flash verify + SPIFFS + USB re-enumeration + wifi/SoftAP
# init, up to ~90 s on a cold boot), so the drain must wait for the banner,
# not a fixed budget. The silence break only covers the rare case where the
# reset pulse did not take (board already idle at its prompt). Never forward
# anything before the prompt: writes sent mid-boot wedge the chip's USB-JTAG
# input endpoint (every subsequent write blocks forever), and dumping the
# raw banner to cutechess reads as a protocol violation ("Could not
# initialize player Board"). If the prompt never arrives, exit non-zero so
# cutechess reports a clean diagnostic instead of a wedged pipe.
buf = b""
last_data = time.time()
deadline = time.time() + 120
while time.time() < deadline and BANNER_END not in buf.decode("utf-8", "replace"):
    chunk = ser.read(4096)
    if chunk:
        buf += chunk
        last_data = time.time()
    elif time.time() - last_data > 30.0:
        break
if BANNER_END not in buf.decode("utf-8", "replace"):
    sys.stderr.write("wrapper: board did not reach the console prompt\n")
    sys.exit(1)
buf = buf.decode("utf-8", "replace")
# Drop the post-prompt remnant instead of forwarding it. Pre-wifi this was
# always empty, but the web companion's wifi/SoftAP/DHCP log lines ("I (4926)
# esp_netif_lwip: DHCP server ...") print after the console prompt and, if
# forwarded during cutechess's uci handshake, read as a protocol violation
# ("Could not initialize player Board").

# bidirectional pump: stdin -> serial (line-buffered), serial -> stdout.
# Only UCI-wire responses are forwarded: boot logs and console chatter that
# leak past the prompt (wifi events, "info" console command output) would
# break the cutechess protocol. Lines are matched on their first word.
UCI_RESP_RE = re.compile(rb"^(id|option|uciok|readyok|info|bestmove|copyprotection|registration)\b")
MOVE_RE = re.compile(rb"^[a-h][1-8][a-h][1-8](?:[qrbn])?$")
last_pv = None
line_buf = b""
ponder_sent = False
while True:
    r, _, _ = select.select([sys.stdin, ser], [], [], 0.005)
    if sys.stdin in r:
        line = os.read(sys.stdin.fileno(), 65536)
        if not line:
            break
        ser.write(line)
    if ser in r:
        data = ser.read(65536)
        if not data:
            continue
        line_buf += data
        while b"\n" in line_buf:
            raw, _, line_buf = line_buf.partition(b"\n")
            line = raw + b"\n"
            if not ponder_sent and PONDER and b"uciok" in line:
                ser.write(b"setoption name Ponder value true\n")
                ponder_sent = True
            if line.startswith(b"info") and b" pv " in line:
                pv_tokens = line.split(b" pv ", 1)[1].split()
                if pv_tokens and MOVE_RE.match(pv_tokens[0]):
                    last_pv = pv_tokens[0]
            elif line.startswith(b"bestmove"):
                move = line.split(None, 2)
                if len(move) >= 2 and not MOVE_RE.match(move[1]):
                    if last_pv is not None:
                        line = b"bestmove " + last_pv + b"\n"
            if UCI_RESP_RE.match(line):
                os.write(sys.stdout.fileno(), line)
        sys.stdout.flush()

ser.close()