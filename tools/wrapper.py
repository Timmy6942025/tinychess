#!/usr/bin/env python3
"""Serial bridge: forwards cutechess-cli's stdin/stdout UCI session to the
XIAO ESP32S3 Plus over USB-serial.

Usage:  cutechess-cli -engine cmd=wrapper.py arg1=/dev/ttyACM0 ...

The engine boots into its console and enters UCI mode on "uci"; the wrapper
swallows the pre-UCI boot banner and the console prompt so only UCI output
reaches cutechess. Never buffers past a newline; flushes after every write.

Why the raw port: the board reboots itself when it receives "quit"
(main_task -> esp_restart). Relying on pyserial's constructor to pulse DTR/RTS
can wedge this USB-JTAG device's input endpoint. Opening the tty without
touching the control lines lets the wrapper read the boot stream or prompt
that is already present.

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
import termios
import time

import re

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
            # Open without touching DTR/RTS. Pyserial's constructor reset can
            # wedge the USB-JTAG input endpoint on this board.
            fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            attrs = termios.tcgetattr(fd)
            attrs[0] = 0
            attrs[1] = 0
            attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
            attrs[3] = 0
            attrs[4] = termios.B115200
            attrs[5] = termios.B115200
            attrs[6][termios.VMIN] = 0
            attrs[6][termios.VTIME] = 1
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            termios.tcflush(fd, termios.TCIOFLUSH)
            return fd
        except OSError:
            if attempt == 49:
                raise
            time.sleep(0.2)
    raise RuntimeError("cannot open serial port")


serial_fd = open_port(args.port)


def serial_write(data):
    while data:
        _, writable, _ = select.select([], [serial_fd], [], 5)
        if not writable:
            raise OSError("serial write timed out")
        count = os.write(serial_fd, data)
        data = data[count:]


def serial_read(size):
    try:
        return os.read(serial_fd, size)
    except BlockingIOError:
        return b""


# Drain the boot banner + console prompt. The ESP32S3 boot can take a while
# (flash verify + SPIFFS + USB re-enumeration + wifi/SoftAP init), so wait for
# the prompt rather than a fixed startup delay. If the board was already idle
# when the port opened, its prompt may have been printed before we arrived.
# After a quiet boot window, a newline asks the console to print that prompt
# again. Never forward anything before the prompt: boot logs and console
# chatter would make cutechess reject the UCI engine.
buf = b""
last_data = time.time()
deadline = time.time() + 120
prompt_probe_sent = False
while time.time() < deadline and BANNER_END not in buf.decode("utf-8", "replace"):
    chunk = serial_read(4096)
    if chunk:
        buf += chunk
        last_data = time.time()
    elif time.time() - last_data > 5.0 and not prompt_probe_sent:
        try:
            serial_write(b"\n")
            prompt_probe_sent = True
            last_data = time.time()
        except OSError:
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
    r, _, _ = select.select([sys.stdin, serial_fd], [], [], 0.005)
    if sys.stdin in r:
        line = os.read(sys.stdin.fileno(), 65536)
        if not line:
            break
        serial_write(line)
    if serial_fd in r:
        data = serial_read(65536)
        if not data:
            continue
        line_buf += data
        while b"\n" in line_buf:
            raw, _, line_buf = line_buf.partition(b"\n")
            line = raw + b"\n"
            if not ponder_sent and PONDER and b"uciok" in line:
                serial_write(b"setoption name Ponder value true\n")
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

os.close(serial_fd)
