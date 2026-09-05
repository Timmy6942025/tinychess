# Adaptive Time Management — Hill-Climbed Policy

> The highest-leverage engine work nobody does rigorously.

## What it is
A small, hill-climbable policy that replaces the fixed
`ms / moves_to_go + inc * 2/3` fraction. Three levers:

1. **Remaining time -> fraction to spend.** Per-bucket multipliers
   `TM_ScaleLT2s / LT5s / LT15s / GTE15s` scale the base budget. When the
   clock is low we spend proportionally less; when it's ample we spend
   more. The four buckets are tuned, not guessed.

2. **Position complexity -> extra time.** `compute_complexity(pos)` blends
   mobility (pseudo-legal move count), pawn structure (islands / doubled /
   isolated) and king safety (attackers to king + pawn shield + in-check)
   into a single `[0,1]` score. The budget is multiplied by
   `1 + cpx * TM_ComplexityWeight/1000` (min gets half the boost). The
   hill climber decides how much a sharp position is worth.

3. **Opponent recent move times -> reaction.** `g_opp_hist` infers the
   opponent's last move time from the `wtime/btime` stream
   (`used = prev_opp - cur_opp + inc`). `opp_factor` is the deviation of
   the last move from the rolling average, plus an absolute-speed bias.
   The budget is multiplied by `1 + opp_f * TM_OppReactWeight/1000`.

Remaining-time and increment share (`TM_IncMax / TM_IncMin`) are the other
two hill dimensions. All eight are exposed as UCI Spin options so cutechess
can set them per engine without recompiling:

```
option.TM_IncMax=667  option.TM_IncMin=500
option.TM_ComplexityWeight=0  option.TM_OppReactWeight=0
option.TM_ScaleLT2s=1000 option.TM_ScaleLT5s=1000
option.TM_ScaleLT15s=1000 option.TM_ScaleGTE15s=1000
```

Defaults are parity: `ComplexityWeight = OppReactWeight = 0` and all scales
`1000` reproduce the old arithmetic bit-identically (verified by building
and playing 10 games with defaults vs old logic: no signal expected).

## Hill climbing
Score purely by match result (expected score / Elo) against a fixed
opponent set - same as every other search gate in this repo (see
`tools/results.log` for the exchange rate).

```
# one probe (200 games at bullet-like 2+0.02, the strength gate TC):
python3 tools/hill_climb_time.py --param TM_ComplexityWeight --delta 100 --games 200

# full coordinate ascent (greedy, restarts on improvement):
python3 tools/hill_climb_time.py --full --games 200 --tc 2+0.02

# vs Stockfish as fixed opponent set:
python3 tools/hill_climb_time.py --full --opponent /usr/local/bin/stockfish

# dry run:
python3 tools/hill_climb_time.py --full --dry-run
```

The climber does coordinate ascent: try `+step` and `-step` for each param,
keep the direction with positive Elo (highest LOS), restart from the
improved point. This is the correct local search for a noisy, expensive
objective; random search wastes the 200-game budget. The inner loop is
`run_match(candidate_opts, baseline_opts)` via cutechess-cli with the same
openings, adjudication and concurrency the rest of the repo uses.

A 200-game bullet match has ~34 Elo error, so the accept bar is LOS >=
75% (about +23 Elo at 50% draws), not bare Elo > 0. The lax bar ratchets
on noise: opposite directions of one param once both read +5.2. Every
accept is reverse-confirmed (the losing direction must actually lose).

Two hard lessons from the Sep 5 climb. First, only tune what the gate TC
exercises. At 2+0.02 the clock never reaches the 5s/15s+ buckets, so
`TM_ScaleLT15s`/`TM_ScaleGTE15s` readings there are pure noise by
construction; both "accepts" were reverted to parity. An untested bucket
value that is a no-op at bullet would still change behavior at real TCs,
so parity is the only safe default for unreachable buckets. Second,
2-second bullet is forfeit-noisy (up to ~9% of games end on time in a
200-game match), which inflates variance past the Elo error bars. Check
the forfeit balance (`Player: A/B ... loses on time`) in every match file
before trusting a verdict; the Sep 5 accepts that survived all had net
forfeit margins near zero. The tight overhead handling
(`MOVE_OVERHEAD_MS = 100` plus the ESP32 25/10 ms bullet trim) is
preserved; the policy multiplies the already trimmed budget.

Bookkeeping: every trial writes `tools/runs/hill-<ts>.txt` and `.pgn` plus
md5 fingerprints, exactly like `tools/fast_sprt.sh`. The final accepted
point is copied into `app/src/time_policy.h` defaults and committed.

## Sep 5 2026 result (committed, then REVERTED same day)
Hill climb found `TM_ComplexityWeight=300`, `TM_ScaleLT2s=1300`
(+40.6 +/- 15.9 over 800 games at 2+0.02, LOS 100%). The transfer audit
killed it: -145.1 @1+0.01 (56 forfeits to 3), -49.0 @5+0.05 (31 to 3).
Root cause is structural, not noise: the budget fractions *remaining*
time with no income anchor, and the `40-fullmoves` horizon collapses to
whole-clock budgets late in the game. Move-level accounting: at 5s the
candidate spends ~169ms/move vs ~163 sustainable; at 2s 57 vs 65, just
sustainable, which is the entire +40. Non-forfeit games at 5s still
favored the candidate (~0.53), so the complexity signal is real and the
pacing is what is broken. Defaults are back at parity until an
income-anchored redesign (horizon floor + capped complexity extension)
passes the `--confirm` gate at two TCs. Full trail in
`tools/results.log`; matches under `tools/runs/hill-*`,
`tools/runs/proof-tm-20260905-105651.*`, `tools/runs/audit*`.
Open follow-ups: `TM_IncMin=600` (+10.4/74.1%, just under the bar),
re-tune of the 5s/15s+ buckets at a longer TC where they are reachable.

## Implementation notes
* `app/src/time_policy.h` is header-only; no new translation unit, no
  extra per-node cost (budget is computed once in `go_handler`). Complexity
  helpers use only cheap bitboard ops. Parity defaults guarantee no
  regression before tuning.
* `g_opp_hist` is reset on `ucinewgame`; otherwise the inference is
  stateless across positions. The factor is clamped to `[-1,1]`.
* ESP32 and desktop share the same policy; the ESP32 bullet trim still
  applies after the policy. The `remaining_scale_for_ms` curve is the right
  place to tune bullet-vs-classical behaviour without touching the trim.
* Do not hand-tune: run the climber. The 8-D space is non-convex in
  practice (time-pressure and complexity interact); local search from
  parity is the honest procedure.

## Workflow
1. Build: `cmake --build app/src/linux-windows/build --target Dog-native -j$(nproc)`
2. Probe one param or ` --full`.
3. When the climber reports an `accept`, re-run at 800 games to shrink
   the error bar, then patch `Params` defaults in `time_policy.h` and
   commit (record Elo/LOS/fingerprint in `tools/results.log`).
