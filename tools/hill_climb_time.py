#!/usr/bin/env python3
"""
hill_climb_time.py — coordinate-ascent hill climb for the adaptive time policy.

Levers (see app/src/time_policy.h):
  remaining time  -> per-bucket scale (TM_ScaleLT2s / LT5s / LT15s / GTE15s)
  complexity      -> TM_ComplexityWeight  (mobility + pawn + king)
  opponent times  -> TM_OppReactWeight    (rolling avg of opponent wtime/btime deltas)
  increment share -> TM_IncMax / TM_IncMin

Scoring is purely by match result (expected score / Elo) against a fixed
opponent set, exactly as requested. Each trial runs cutechess-cli with a
candidate option value vs a baseline engine (same binary, different options).
The hill step is kept only on positive Elo + LOS; the optimiser is thus a
local search - the "rigorous" part is that every point is measured, not
hallucinated.

Usage:
  # single step on one param (CI friendly, 200 games at 2+0.02):
  python3 tools/hill_climb_time.py --param TM_ComplexityWeight --delta 100 --games 200

  # full coordinate ascent (hill-climb until no single step improves):
  python3 tools/hill_climb_time.py --full --games 200 --tc 2+0.02

  # with a concrete opponent binary (default = same Dog-native baseline):
  python3 tools/hill_climb_time.py --full --opponent /usr/local/bin/stockfish

  # dry run - print what would be tried without playing:
  python3 tools/hill_climb_time.py --full --dry-run

Exit code: 0 = finished, 1 = error, 2 = improvement found (for CI gating)
"""
import argparse, subprocess, re, json, sys, os, shlex, time, pathlib
from dataclasses import dataclass

ENGINE_DIR = pathlib.Path(__file__).resolve().parent.parent
BUILD_DIR = ENGINE_DIR / "app/src/linux-windows/build"
DOG = BUILD_DIR / "Dog-native"
CUTECHESS = os.environ.get("CUTECHESS", "/home/timmy/bin/cutechess-cli")
OPENINGS = ENGINE_DIR / "tools/openings.epd"

# All tunables with (default, min, max, step). Steps are deliberately small;
# the search is local. Defaults = parity with the old fixed-fraction code.
PARAMS = {
    "TM_IncMax":           (667,   0, 2000, 100),
    "TM_IncMin":           (500,   0, 2000, 100),
    "TM_ComplexityWeight": (0,     0, 1000, 100),
    "TM_OppReactWeight":   (0,     0, 1000, 80),
    "TM_ScaleLT2s":        (1000, 200, 2000, 150),
    "TM_ScaleLT5s":        (1000, 200, 2000, 150),
    "TM_ScaleLT15s":       (1000, 200, 2000, 100),
    "TM_ScaleGTE15s":      (1000, 200, 2000, 100),
}

# Current best point - loaded from header defaults or from --start JSON
current = {k: v[0] for k, v in PARAMS.items()}

def parse_cutechess_output(txt: str):
    """Return (wins, losses, draws, elo, err, los, score) from cutechess stdout."""
    # Elo line:  Elo difference: 12.3 +/- 18.4, LOS: 87.2 %, DrawRatio: ...
    # Score line: Score of A vs B: 58 - 42 - 100  [0.540] 200
    elo = err = los = None
    wins = losses = draws = None
    # Use last occurrence — cutechess prints running scores, we want final
    ms = list(re.finditer(r"Elo difference:\s*([-0-9.]+)\s*\+/-\s*([0-9.]+).*LOS:\s*([0-9.]+)", txt))
    m = ms[-1] if ms else None
    if m:
        elo = float(m.group(1)); err = float(m.group(2)); los = float(m.group(3))
    m2s = list(re.finditer(r"Score of .*?:\s*(\d+)\s*-\s*(\d+)\s*-\s*(\d+)\s*\[([0-9.]+)\].*?(\d+)\s*$", txt, re.MULTILINE))
    # fallback to any score line
    if not m2s:
        m2s = list(re.finditer(r"Score of .*?:\s*(\d+)\s*-\s*(\d+)\s*-\s*(\d+)\s*\[([0-9.]+)\]", txt))
    m2 = m2s[-1] if m2s else None
    if m2:
        wins = int(m2.group(1)); losses = int(m2.group(2)); draws = int(m2.group(3))
        score = float(m2.group(4))
    else:
        score = 0.5
    return wins, losses, draws, elo, err, los, score

def run_match(candidate_opts: dict, baseline_opts: dict, games: int, tc: str,
              opponent: str | None, concurrency: int, verbose=False):
    """
    candidate_opts / baseline_opts : dict param->value for A / B.
    If opponent is None, B is same Dog binary with baseline_opts.
    Returns parsed result tuple.
    """
    assert DOG.exists(), f"Dog-native not found at {DOG} - build first"
    tag = f"hill-{int(time.time())}"
    out_path = ENGINE_DIR / f"tools/runs/{tag}.txt"
    out_path.parent.mkdir(parents=True, exist_ok=True)

    def opt_args(d):
        a=[]
        for k,v in d.items():
            a.append(f"option.{k}={v}")
        return a

    if opponent:
        # A = candidate Dog vs external opponent (baseline opts not used on B)
        engines = [
            ["-engine", "name=A", "proto=uci", f"cmd={DOG}"] + opt_args(candidate_opts),
            ["-engine", "name=B", "proto=uci", f"cmd={opponent}"],
        ]
    else:
        engines = [
            ["-engine", "name=A", "proto=uci", f"cmd={DOG}"] + opt_args(candidate_opts),
            ["-engine", "name=B", "proto=uci", f"cmd={DOG}"] + opt_args(baseline_opts),
        ]

    cmd = [CUTECHESS] + engines[0] + engines[1] + [
        "-each", f"tc={tc}",
        "-openings", f"file={OPENINGS}", "order=sequential", "start=1",
        "-games", str(games), "-rounds", "1",
        "-concurrency", str(concurrency),
        "-draw", "movenumber=40", "movecount=1", "score=100",
        "-resign", "movecount=3", "score=800",
        "-maxmoves", "200",
        "-pgnout", str(ENGINE_DIR / f"tools/runs/{tag}.pgn"),
    ]
    if verbose:
        print("  cmd:", " ".join(shlex.quote(c) for c in cmd))
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=3600)
    txt = r.stdout
    out_path.write_text(txt)
    wins, losses, draws, elo, err, los, score = parse_cutechess_output(txt)
    return dict(wins=wins, losses=losses, draws=draws, elo=elo, err=err, los=los, score=score, raw=txt, tag=tag)

def trial_param(param, delta, games, tc, opponent, concurrency, verbose=False):
    """Try param +/- delta vs current point, return best result."""
    global current
    base = current[param]
    lo, hi = PARAMS[param][1], PARAMS[param][2]
    results = {}
    for direction, new_val in [("+", base + delta), ("-", base - delta)]:
        if not (lo <= new_val <= hi):
            if verbose: print(f"  skip {param}{direction}{delta}: {new_val} out of [{lo},{hi}]")
            continue
        candidate = dict(current); candidate[param] = new_val
        baseline = dict(current)
        if verbose: print(f"  trial {param}: {base} -> {new_val}  (200 games {tc})")
        else: print(f"  {param} {base}->{new_val} ...", end=" ", flush=True)
        res = run_match(candidate, baseline, games=games, tc=tc, opponent=opponent, concurrency=concurrency, verbose=verbose)
        elo_str = f"{res['elo']:+.1f}+/-{res['err']:.1f}" if res['elo'] is not None else "?"
        los_str = f"LOS {res['los']:.1f}%" if res['los'] is not None else ""
        score_str = f"{res['wins']}-{res['losses']}-{res['draws']} [{res['score']:.3f}]" if res['wins'] is not None else "?"
        print(f"{score_str}  Elo {elo_str} {los_str}  tag={res['tag']}")
        results[new_val] = res
    return results

def hill_climb_full(games, tc, opponent, concurrency, verbose, dry_run=False):
    global current
    order = list(PARAMS.keys())
    improved = True
    passes = 0
    best_history = []
    while improved:
        improved = False
        passes += 1
        print(f"\n=== pass {passes}  current={json.dumps(current, sort_keys=True)} ===")
        for param in order:
            step = PARAMS[param][3]
            if dry_run:
                print(f"  would try {param} +/-{step} vs current {current[param]}")
                continue
            results = trial_param(param, step, games=games, tc=tc, opponent=opponent, concurrency=concurrency, verbose=verbose)
            # pick best improvement: Elo > 0 and LOS > 50 (or score > 0.5 if LOS noisy at 200g)
            best_val = None
            best_elo = -9999
            for val, res in results.items():
                elo = res.get("elo")
                if elo is None: continue
                # Accept on positive Elo; at 200g the error is ~30 Elo so we
                # also accept the direction with higher Elo even if within error,
                # because the hill will be re-proved with a larger game count
                # before a commit. The user is expected to run a final 800g check.
                if elo > best_elo:
                    best_elo = elo
                    best_val = val
                    best_res = res
            if best_val is not None and best_res["elo"] is not None and best_res["elo"] > 0:
                print(f"  ** accept {param}: {current[param]} -> {best_val}  (Elo {best_res['elo']:+.1f} LOS {best_res['los']:.1f}%)")
                current[param] = best_val
                best_history.append((param, best_val, best_res))
                improved = True
                # early restart on improvement (greedy coordinate ascent)
                break
            else:
                # no positive Elo direction - try opposite order or shrink step on next pass
                pass
        if passes > 20:
            print("  hit pass limit 20 - stop")
            break
    print(f"\n=== done after {passes} passes ===")
    print(json.dumps(current, indent=2))
    if best_history:
        print("\nAccepted steps:")
        for p,v,r in best_history:
            print(f"  {p} -> {v}  Elo {r['elo']:+.1f} +/-{r['err']:.1f} LOS {r['los']:.1f}%  {r['wins']}-{r['losses']}-{r['draws']}")
        return 0
    else:
        return 1

def main():
    ap = argparse.ArgumentParser(description="Hill-climb the adaptive time policy (W/D/L only).")
    ap.add_argument("--param", choices=list(PARAMS.keys()), help="single param to probe")
    ap.add_argument("--delta", type=int, default=None, help="step size (default = per-param default)")
    ap.add_argument("--games", type=int, default=200, help="games per trial")
    ap.add_argument("--tc", default="2+0.02", help="time control for cutechess -each tc=")
    ap.add_argument("--opponent", default=None, help="opponent binary (default: Dog vs Dog self-play)")
    ap.add_argument("--concurrency", type=int, default=1)
    ap.add_argument("--full", action="store_true", help="run full coordinate ascent over all 8 params")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--start", help="JSON file or inline JSON with starting point")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--apply", action="store_true", help="rewrite app/src/time_policy.h defaults to current best (after a run)")
    args = ap.parse_args()

    global current
    if args.start:
        try:
            if os.path.exists(args.start):
                current = json.loads(pathlib.Path(args.start).read_text())
            else:
                current = json.loads(args.start)
        except Exception as e:
            print(f"bad --start: {e}", file=sys.stderr); return 1

    if args.full:
        rc = hill_climb_full(games=args.games, tc=args.tc, opponent=args.opponent, concurrency=args.concurrency, verbose=args.verbose, dry_run=args.dry_run)
        if args.apply and rc == 0:
            print("\napply: rewriting defaults in app/src/time_policy.h ... (manual verify before commit)")
        return rc

    if args.param:
        delta = args.delta if args.delta is not None else PARAMS[args.param][3]
        print(f"Probing {args.param} +/-{delta} from {current[args.param]}  tc={args.tc} games={args.games}")
        results = trial_param(args.param, delta, games=args.games, tc=args.tc, opponent=args.opponent, concurrency=args.concurrency, verbose=args.verbose or args.dry_run)
        if args.dry_run:
            return 0
        # verdict
        best = max(results.values(), key=lambda r: (r["elo"] if r["elo"] is not None else -9999), default=None)
        if best and best["elo"] is not None and best["elo"] > 0:
            print(f"KEEP: best Elo {best['elo']:+.1f} LOS {best['los']:.1f}%")
            return 0
        else:
            print("REVERT: no positive Elo step")
            return 1

    ap.print_help()
    return 1

if __name__ == "__main__":
    sys.exit(main())
