# Correction history: built, gated, rejected

This document records a complete R&D cycle: the search-side eval interface
was rebuilt around self-correcting static evaluation ("correction history",
the technique Stockfish adopted in 2024 and small engines have measured at
+35 Elo cumulative), driven through six gated configurations and roughly
1,900 games of controlled play. The verdict is that it does not pay for
itself in this engine. The full diff survives in the session archive; the
numbers below should stop anyone from re-trying it without new information.

## The idea

The net's static eval carries systematic biases per position class. The
search was to measure its own disagreement with the eval at finished nodes
and feed residuals back into three int16 tables keyed by:

- pawn structure (XOR of piece-square keys over pawns)
- white non-pawn placement (that side's pieces minus pawns)
- black non-pawn placement

Keys were XOR-maintained inside the per-thread Eval object by the same
make/unmake wrappers that maintain the NNUE accumulator (castling, en
passant and promotions all route through one lambda pair). Application,
update and reset plumbing mirrored the repo's history[] conventions:
ucinewgame, both bench paths, TUI new game and the four test sites clear
the tables; benches stay deterministic because tables start empty per FEN.

Update rule followed Stockfish's shape with local fixes: only bounded
results teach (quiet cutoff above the corrected eval, or all-moves-failed-low
below it), depth-weighted bonus `clamp(error * depth / 8)`, gravity limit
1024. Excluded: exact PV residuals, capture/promotion cutoffs, depth-1
nodes, mate-band scores, stopped searches.

## What was measured

All matches vs the accepted HEAD build (31be6d3), 2+0.02, Threads=1,
Hash=8, openings sequential from tools/openings.epd, adjudication per repo
convention, concurrency 3 on the dev Pi.

| variant | consumption | games | result |
|---|---|---|---|
| v1 | shift all evals; updates incl. exact PV | 200 | -19.1 +/- 29.9 |
| v2 | shift main-search only | 200 | -31.4 +/- 30.8 |
| v3 | shift all evals; bounded-only updates | 200 then 600 | +12.2 +/- 29.5, then **-33.1 +/- 17.6** |
| v4 | no eval shifts; table magnitude drives futility/LMR adaptivity | 200 | -3.5 +/- 29.7 (+15% nodes) |
| v5 | shift qs stand-pat only, full weights | 200 | +1.7 +/- 29.5 |
| v6 | shift qs stand-pat only, half weights | 200 then 600 | +5.2 +/- 30.3, then **-0.6 +/- 16.6** |

The v1/v2 pair isolated the update policy: feeding uncorrelated PV
residuals into saturating tables costs Elo outright. The v3 pair exposed
the trap: a 200-game sample scored +12.2 (LOS 79%), the 600-game
replication of the same binary scored -33. Screening positives at 200
games without replication would have shipped a regression.

## Why it fails here

Three reasons, in decreasing order of confidence:

1. Learning budget. Tables reset every game (ucinewgame), so all learning
   happens within one game: thousands of nodes per move, ~40 moves. That is
   orders of magnitude less observation than Stockfish's millions of nodes
   per move with persistent-per-game tables. Buckets are still near noise
   when the game ends.
2. Margin coupling. Razor (350+150d), reverse futility (121d/move) and
   futility (180+150d) were each tuned to tighter tolerances than the
   correction deflections they would consume (results.log shows ±30 cp
   margin changes losing 20-80 Elo). Shifting the eval scale under them
   randomizes all three simultaneously; v4 confirmed the margins themselves
   have no adaptive headroom being left on the table (+15% nodes bought
   zero Elo).
3. Depth profile. At 2+0.02 searches run 7-11 ply. Correction history's
   documented gains grow with time control; at these depths the signal has
   not separated from the noise even where tables do learn.

## What was kept

Nothing in code. The tree is back at HEAD and reproduces baseline
bit-exactly (bench 7,582,143 nodes at fixed depth). The unit gate passed
18/18 with the component installed, so the integration pattern itself
(incremental keys inside Eval, bound-gated updates at node completion,
reset-point parity with history[]) is proven safe to resurrect if a future
net or longer TC changes the economics.

Archive: tools/patches/corrhist.patch (the final v6 variant against HEAD).
