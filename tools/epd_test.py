#!/usr/bin/env python3
"""EPD tactical suite runner for the MaxDogOne engine.

Scores a UCI engine against a Win At Chess-style EPD suite: for every
position the engine is given a fixed amount of think time and the resulting
bestmove is compared against the suite's "bm" (best moves) operation.

Output (stdout):
    <solved>/<total> (<percentage>) - <engine>

Plus a per-position CSV (default: stderr? no - an explicit --out file)
with: index, fen, expected, played, hit, time_ms.

Usage:
    tools/epd_test.py --engine <binary> --suite <file.epd> [--time 1000]
                      [--out result.csv] [--quiet]

Exit code 0 always (the score is the output); a missing engine/suite is
exit 2. This is a triage tool, not a replacement for SPRT.
"""

import argparse
import os
import re
import select
import subprocess
import sys
import time

try:
    import chess
except ImportError:
    chess = None


BESTMOVE_RE = re.compile(r"bestmove\s+([a-h][1-8][a-h][1-8][qrbn]?|0000)")
SCORE_RE = re.compile(r"score\s+(cp|mate)\s+(-?\d+)")

# EPD op keys (everything else is an op value).
EPD_OPS = {"bm", "am", "id", "ce", "dm", "hm", "pv", "acn", "acs", "acd",
           "c0", "c1", "c2", "c3", "c4", "c5", "c6", "c7", "c8", "c9",
           "rc", "sm", "tc", "fmvn", "hmvc"}


def parse_epd(path):
    """Yield (lineno, fen, bm, am) for each position with a bm/am op.

    WAC files put the bm op before the first ';' (e.g.
    "... b - - bm Rxb2; id "WAC.002";"), so ops are parsed token-wise
    from the tail of the line rather than only after a semicolon.
    """
    positions = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            tokens = line.split()
            if len(tokens) < 5:
                continue  # not a valid 4-field EPD
            fen = " ".join(tokens[:4]) + " 0 1"
            bm = set()
            am = set()
            rest = tokens[4:]
            i = 0
            while i < len(rest):
                key = rest[i].lower()
                i += 1
                if key == "bm":
                    while i < len(rest) and rest[i].lower() not in EPD_OPS:
                        bm.add(rest[i].strip(";"))
                        i += 1
                elif key == "am":
                    while i < len(rest) and rest[i].lower() not in EPD_OPS:
                        am.add(rest[i].strip(";"))
                        i += 1
            if not bm and not am:
                continue
            positions.append((lineno, fen, bm, am))
    return positions


def resolve_moves(fen, san_moves):
    """SAN -> UCI via python-chess; falls back to a case-insensitive
    prefix match (stripped of +/#/=) when python-chess is unavailable."""
    if chess is None or not san_moves:
        return {m.lower().rstrip("+#=qrbn")[:4] for m in san_moves}, False
    board = chess.Board(fen)
    uci = set()
    ok = True
    for san in san_moves:
        try:
            uci.add(board.parse_san(san).uci())
        except ValueError:
            ok = False
    return uci, ok


class Engine:
    def __init__(self, binary, hash_mb=64, threads=1):
        self.proc = subprocess.Popen(
            [binary],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            universal_newlines=True,
            bufsize=1,
        )
        self.send("uci")
        self.read_until_idle()
        self.send(f"setoption name Hash value {hash_mb}")
        self.send(f"setoption name Threads value {threads}")
        self.send("ucinewgame")

    def send(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()

    def read_until_idle(self, timeout=30):
        """Consume output until the engine goes idle (bestmove / uciok /
        readyok seen on the wire)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                continue
            if line.startswith(("bestmove", "uciok", "readyok")):
                return

    def bestmove(self, fen, think_ms):
        self.send(f"position fen {fen}")
        self.send(f"go movetime {think_ms}")
        bestmove = None
        score = None
        deadline = time.time() + think_ms / 1000.0 + 30.0
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                continue
            m = BESTMOVE_RE.search(line)
            if m:
                bestmove = m.group(1)
                break
            s = SCORE_RE.search(line)
            if s:
                score = (s.group(1), int(s.group(2)))
        drain_deadline = time.time() + 0.5
        while time.time() < drain_deadline:
            r, _, _ = select.select([self.proc.stdout], [], [], 0.05)
            if not r:
                break
            line = self.proc.stdout.readline()
            if not line:
                break
        return bestmove, score

    def close(self):
        try:
            self.send("quit")
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--engine", required=True, help="UCI engine binary")
    ap.add_argument("--suite", required=True, help="EPD suite file")
    ap.add_argument("--time", type=int, default=1000, help="think time per position (ms)")
    ap.add_argument("--out", default=None, help="optional per-position CSV output")
    ap.add_argument("--quiet", action="store_true", help="suppress per-position lines")
    args = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)

    if not os.path.isfile(args.engine):
        print(f"engine not found: {args.engine}", file=sys.stderr)
        return 2
    if not os.path.isfile(args.suite):
        print(f"suite not found: {args.suite}", file=sys.stderr)
        return 2

    positions = parse_epd(args.suite)
    if not positions:
        print(f"no playable positions in {args.suite}", file=sys.stderr)
        return 2

    engine = Engine(args.engine)
    solved = 0
    rows = []
    start = time.time()
    try:
        for index, (lineno, fen, bm, am) in enumerate(positions, 1):
            bm_uci, bm_ok = resolve_moves(fen, bm)
            am_uci, am_ok = resolve_moves(fen, am)
            played, score = engine.bestmove(fen, args.time)
            if played is None:
                played = "0000"
            hit = False
            if bm:
                hit = played in bm_uci
            elif am:
                hit = played not in am_uci
            if hit:
                solved += 1
            rows.append((lineno, fen, "/".join(sorted(bm)) or "/".join(sorted(am)),
                         played, "hit" if hit else "miss", score, args.time))
            if not args.quiet:
                mark = "OK " if hit else "XX "
                print(f"{mark}[{index}/{len(positions)}] {lineno} expected={rows[-1][2]} "
                      f"played={played} {score or ''}")
    finally:
        engine.close()

    elapsed = time.time() - start
    pct = 100.0 * solved / len(positions)
    print(f"\n{solved}/{len(positions)} ({pct:.1f}%) - {args.engine} @ {args.time}ms "
          f"in {elapsed:.0f}s")

    if args.out:
        with open(args.out, "w") as fh:
            fh.write("line,fen,expected,played,result,score,time_ms\n")
            for r in rows:
                fh.write(",".join(str(x) if x is not None else "" for x in r) + "\n")
        print(f"per-position results: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
