# Move-ordering rebuild: capture history + butterfly + continuation

Status: gated (this file records the design; the verdict lives in
tools/results.log).

## What was here before

The ordering layer was untouched since the Dog fork point:

- Captures: static MVV-LVA (`victim << 19` + `(QUEEN - attacker) << 8`),
  stateless, blind to how a capture class actually performs in the search.
- Quiets: one int16 table, 768 entries (`side * piece-type * to`), updated
  only on quiet beta cutoffs with `depth*30 - 25` gravity bonuses.
- qsearch: pure SEE ordering + SEE<0 prune (accepted item, kept intact).
- No capture history, no continuation history, no from-square information,
  and no connection between ordering scores and pruning decisions.

## What changed

Four new/expanded signals, all int16 with the same 1023-gravity rule the old
history used (`update_*` helpers in search.cpp):

1. **Capture history** - `capture_history[side][piece][to][captured]`
   (12 x 64 x 6 = 4608 entries, 9 KB). Scored as
   `MVV-LVA + ch * 16`, so it reorders inside a victim class and across the
   attacker sub-ranks, but an unlearned table degrades exactly to plain
   MVV-LVA (victim class stays dominant). Updated on capture cutoffs:
   bonus to the cutting capture, malus to the captures searched before it -
   the mirror of what the quiet path always did.
2. **Butterfly history** - `[side][from][to]` (2 x 64 x 64, 16 KB), added
   to quiet scoring next to the existing piece-to table. This restores the
   from-square dimension hist3d wanted without diluting the existing table
   (hist3d REPLACED piece-to with piece-from-to and lost the denser signal;
   here both run in parallel).
3. **Continuation history** - `[prev_to][to]` butterfly continuation keyed
   by the opponent's previous to-square (64 x 64, 8 KB). One ply only.
   Stockfish keeps four-plus plies, Ethereal two; at this engine's depth and
   learning budget one ply captures most of the countermove-shaped signal
   without the sparsity that killed bigger tables.
4. **Pruning hooks off the same scores** (the part that makes this a layer,
   not just sorting):
   - Futility: a shallow-depth quiet now escapes futility pruning when its
     combined history score is strongly positive (>500). One-sided by
     construction: baseline prunes everything under the margin, this can
     only search MORE.
   - LMR: good history reduces the reduction by up to 2 plies
     (`max(0, histScore/1500)`), PV branches get up to +2 depth. Strictly
     one-sided - see the graveyard lesson below.

## The two failures caught before they shipped

- **SEE in main-search ordering**: the first cut scored captures with full
  SEE plus a below-quiets demotion for losing captures. Bench tax: ~20% nps
  on desktop. qs already SEEs its own moves; paying again per main node did
  not survive contact with the bench protocol. Dropped; capture history
  alone orders captures at zero marginal cost.
- **Two-sided LMR modulation**: letting bad history deepen reductions
  failed the unit gate the same way every deeper-LMR probe in results.log
  died - the R+R vs K mate-in-19 sweep (tables fill during the search;
  extra reduction breaks the ladder horizon). Fixed by making modulation
  strictly one-sided: history may only buy depth, never spend it. Unit gate
  passed 18/18 after the fix.
- Related hygiene bug: blending histories into the qsearch score array
  would have tainted the array that drives the accepted SEE<0 prune.
  Ordering and prune criteria stay decoupled; qs remains bit-exact with the
  accepted semantics.

## Memory

Desktop: four calloc'd tables per searcher (~35 KB/thread total).
ESP32: the three new tables come out of ONE PSRAM block per thread
(33 KB) with fallback to internal allocations if PSRAM is exhausted -
internal SRAM cannot fund another ~34 KB/thread on top of the stacks and
web buffers. Reset points mirror history[] exactly: ucinewgame, bench
(both paths), TUI reset, unit tests.

`ORDER_TABLE_READS` in search.cpp is a compile-time switch that keeps the
table updates while compiling out the scoring reads - it exists because
the board-side ablation needed to split read cost from update cost, and it
stays as an ablation hook.

## Board port

Same-session alternating-flash bench pairs (warm-stable protocol, doctrine:
same-session pairs only):

- updates-only vs full reads: 18,916 -> 18,283 nps average = **-3.3% node
  throughput from the scoring reads** (three extra PSRM-resident lookups
  per scored move; desktop caches absorb this, the in-order S3 does not).
  The update path itself measured ~free.
- Against the desktop-measured +16..+25 Elo, the throughput curve puts the
  on-device net clearly positive.

One real regression surfaced at the harshest TC and was fixed: with 2+0.02
the slower nodes skip the between-iteration budget window more often, so
searches rode the hard cap every move and cap + emit + wrapper round trip
(~35 ms) overshot the ~80 ms/move income - clock death around move 50
(2 forfeits in the first 3-game gate; baseline control flashed the same
hour passed 3/3). Fix: ESP32-only 25 ms budget trim when the remaining
clock is under 3 s (`go_handler`, same pattern as the floor-30 horizon) -
long TCs untouched. Re-gate after the fix: **6/6 games clean**, no
forfeits/stalls/illegal moves.

## Gates

Recorded in tools/results.log under the orderA tags:
- unit gate 18/18 (desktop), including the mate-in-N sweep.
- strength: SPRT 2+0.02 Threads=1 Hash=8 vs pre-change baseline -
  ACCEPT +24.6 +/- 22.6 (481 g, llr crossed H1), replication KEEP
  +16.5 +/- 16.3 (800 g, LOS 97.6%). Combined 346-274-661.
- board: bench pair above + solo stability gates clean (the one forfeit
  in a 6-game batch was self-inflicted concurrent load, reproduced clean
  solo).

## Final-testing lessons (2026-08-25)

The ablation switch bit back once: `ORDER_TABLE_READS` was left FALSE by
the board A/B prep, and one desktop build plus one "clean" board gate ran
on reads-disabled (= baseline-strength) binaries before the final
bit-exactness ladder caught it - the tell was a depth-9 startpos search
reproducing BASELINE node counts instead of vA's. Final round re-verified
everything on the true config: unit 18/18, ladder exact-match vs the SPRT
binary, solo 4/4 board gate, shipped-fw bench 18,327 nps warm.

Known trade-off, logged not hidden: WAC at fixed 1000 ms drops ~10
positions vs baseline (236 vs 246; the old 260 ref predates the whole
Phase-B stack - baseline itself scores 246), roughly half of it explained
by the speed tax. Two full SPRTs on real play outrank the snapshot suite;
the deep mate-in-N floor is unaffected.

