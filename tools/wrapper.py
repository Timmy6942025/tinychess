#!/usr/bin/env python3
"""Serial bridge: forwards cutechess-cli's stdin/stdout UCI session to the
XIAO ESP32S3 Plus over USB-serial.

Usage:  cutechess-cli -engine cmd=wrapper.py arg1=/dev/ttyACM0 ...

The engine boots into its console and enters UCI mode on "uci"; the wrapper
swallows the pre-UCI boot banner and the console prompt so only UCI output
reaches cutechess. Never buffers past a newline; flushes after every write.
"""

import argparse
import errno
import os
import select
import sys
import time

import serial

BAUD = 115200
BANNER_END = "ENTER \"uci\" FOR uci-MODE"  # console prompt line

parser = argparse.ArgumentParser()
parser.add_argument("port", nargs="?", default=os.environ.get("PORT", "/dev/ttyACM1"))
args = parser.parse_args()


def open_port(port):
    for attempt in range(50):
        try:
            ser = serial.Serial(port, BAUD, timeout=0.05)
            return ser
        except (serial.SerialException, OSError):
            if attempt == 49:
                raise
            time.sleep(0.2)
    raise RuntimeError("cannot open serial port")


ser = open_port(args.port)
try:
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
except (OSError, ValueError, NotImplementedError):
    pass
ser.reset_input_buffer()

# drain the boot banner + console prompt (skip them, keep anything after)
buf = b""
deadline = time.time() + 15
while time.time() < deadline and BANNER_END not in buf.decode("utf-8", "replace"):
    chunk = ser.read(4096)
    if chunk:
        buf += chunk
buf = buf.decode("utf-8", "replace")
tail = buf.split(BANNER_END, 1)[-1]
if tail:
    sys.stdout.write(tail)
    sys.stdout.flush()

# bidirectional pump: stdin -> serial (line-buffered), serial -> stdout
while True:
    r, _, _ = select.select([sys.stdin, ser], [], [], 0.5)
    if sys.stdin in r:
        line = os.read(sys.stdin.fileno(), 65536)
        if not line:
            break
        ser.write(line)
    if ser in r:
        data = ser.read(65536)
        if not data:
            continue
        os.write(sys.stdout.fileno(), data)
        sys.stdout.flush()

ser.close()
