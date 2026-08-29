# Agent instructions

> Always use the `unslop` skill when writing or editing any text, docs, comments, or user-facing messages. Scan for the patterns listed in the skill, rewrite to remove them, add soul, and self-audit before committing.
> When the user says `bro` on its own (case-insensitive, trimmed, no other content), invoke the `bro` skill to restate the last message in plain human language.

Repository: `Timmy6942025/tinychess` (branch `main`), origin on GitHub.
This file tells coding agents how to work in this repo.

## Rules

- **Commit and push**: commit every change and `git push origin main`
  immediately after it passes its gates. Do not leave work uncommitted.
- Before committing, check `git submodule status`: if `app/src/fathom` drifts
  off `2251e9974d5e1c77f09e35015fc325098e586e2c`, restore it with
  `git checkout 2251e9974d5e1c77f09e35015fc325098e586e2c` (inside
  `app/src/fathom`) before `git add`.
- Leave untracked: `app/src/linux-windows/Dog-native`, `Dog-ruk`,
  `Dog-stats-prober` (prebuilt binaries, rebuilt by cmake; the latest published
  release is on GitHub Releases as `v0.5-prebuilt`, built from `1efb667`).
- Never commit the generated `app/build/` directory (gitignored).

## Repo facts

- One source tree: `app/main` is a **symlink to `app/src`** — edits apply once
  to both the desktop and the ESP32 build.
- `app/include/libchess` is vendored (git-tracked) with local patches
  (`pseudo_legal_move_list_into`, FEN en-passant validation, `go st` UCI).
- NNUE weights are committed as source: `app/src/weights.cpp` (+
  `quantised-big.bin`). `#if 0` selects the small net, `#else` the active big
  net.
- Flash mode is **DIO only** (`app/sdkconfig.defaults`). QIO was tried and
  reverted: unstable under sustained 2-thread load. Do not re-enable QIO.
- Search parameters are at a measured local optimum: probed-and-rejected list
  is in `tools/results.log` (killers, LMR PV *3/4, aspiration != 75, TT 4-way,
  blind singular extension, null-move R=5, razor depth<=2, razor 300+120d).
  Phase 2 (Aug 25-26) added four more, all gated against the orderA state:
  staged move picker (+9.6 then -2.8 on replication), time-management tweaks
  at bullet TC (-8.6 package, -5.0 easy-move alone), proper singular
  extensions (-6.5; the R3R1K1 depth-19 sweep is 5-0 lifetime - any variant
  must preflight against it, and SE needs a LOWERBOUND TT entry to avoid
  promiscuous extension in blown positions), TT key widening to 32 bit with
  12-byte entries (-9.7 at Hash=8: capacity loss beats integrity gain at
  blitz node counts).
- Board console requires `uci` before UCI commands; the cutechess adapter is
  `tools/wrapper.py` (needs a serial DTR toggle / fresh port open).

## Workflow (always gate changes)

1. **Desktop build**: `cd app/src/linux-windows/build && cmake --build . --target
   Dog-native -j$(nproc)` (default cmake config = RelWithDebInfo + LTO).
2. **Unit gate**: `printf "test\n" | ./Dog-native` — all 18 tests must pass
   (`OK`, no `assert fail`). The suite takes >10 min; exit 124 (timeout) with
   0 failures and tests still printing `OK` counts as passing.
3. **Strength gate** (search items): 200-game match at 2+0.02 vs the previous
   accepted state:
   `/home/timmy/bin/cutechess-cli -engine name=A proto=uci cmd=<binary> -engine
   name=B proto=uci cmd=<baseline> option.Threads=1 option.Hash=8 -draw
   movenumber=40 movecount=1 score=100 -resign movecount=3 score=800 -maxmoves
   200 -games 200 -rounds 1 -each tc=2+0.02`
   Keep on positive Elo + LOS, revert otherwise. Record the verdict in
   `tools/results.log` with numbers.
4. **Board port**: rebuild with the IDF (`export IDF_PATH=~/esp/esp-idf &&
   source ~/esp/esp-idf/export.sh && idf.py build`), flash
   (`python -m esptool --port /dev/ttyACM0 write_flash "@flash_args"` from
   `app/build`), then a 3-game board-vs-native gate (no stalls/forfeits/illegal
   moves) and a bench.
5. Push after each committed gate result.

## State (what the numbers should look like)

- Board bench: compare same-session pairs only - absolute nps swings ~15% with
  board temperature. Cool-board anchors: ~18,800 staged vs ~17,700 control
  (output-layer staging era, Aug 25); hot-board anchors: ~15,500 vs ~14,600
  same night. History: ~8,300 pre-paired-fused rebuild, 6,857 at Tier-1.
  Keep this dev Pi off DOG-CHESS while benching (its httpd client load costs
  real nps).
- Desktop native bench: roughly 350k-590k nps via the bench protocol depending on
  machine load, LTO build, big net. The evaluator is
  memory-latency-bound on desktop; instruction-count profiles mislead.
- Accepted search items (Phase B): razoring, check extension, qs SEE pruning, recapture extension, qsearch TT-probe removal (+209.5 Elo cumulative vs Tier-1), then the move-ordering rebuild: capture history `[side][piece][to][victim]`, butterfly from-to history, and a one-ply continuation table (+24.6 accepted at SPRT, +16.5 +/- 16.3 on 800-game replication; docs/move-ordering-rebuild.md). Cumulative post-Tier-1 is ~+230 Elo. REJECTED in the same campaign, do not retry: history-gated futility/LMR hooks (-28.8 packaged with the tables, roughly -50 isolated - third confirmation the margins have no slack), SEE scoring of main-search captures (~20% bench tax), two-sided LMR modulation (unit gate, R3R1K1 d19). The board's ordering tables live in one PSRAM block per searcher with an internal fallback; an ESP32-only 25 ms budget trim under a 3 s clock keeps bullet TCs clean because the reads cost ~3% node throughput. Aug 26 re-profile re-confirmed 5.7% desktop / 3.3% board (tools/results.log); bit-exact victim reuse and bounds cleanup saved 0.40% Ir and ~0% board, the naive 2 MB merged butterfly/continuation layout does not fit the 33 KB PSRAM budget, no ship.
- Accepted eval-kernel item: paired-fused rebuild - bit-exact on
  desktop (strength gate 39-42-119, -5.2 +/- 30.7 = parity), ~1.8x board node
  throughput worth +173.9 +/- 34.3 measured Elo on-device (240-game
  asymmetric-clock selfplay). Accepted speed item: output-layer SRAM staging -
  bit-exact everywhere, +5.9-6.2% board node throughput same-day A/B
  (docs/output-layer-staging.md). REJECTED after six gated variants (~1,900
  games): correction history in every consumption form - within-game learning
  volume is too small and the pruning margins are tuned tighter than any useful
  deflection (docs/correction-history.md, do not re-try). A follow-up with
  shared persistent correction and experience tables also lost 19.1 +/- 32.5
  Elo over 200 games and was reverted; do not retry without a new design.
  Reference desktop
  binary at the move-ordering rebuild has md5
  `fd8cef65eda53294c03a705bd40e32ff`; hashes change per build, fingerprints
  land in results.log per match.
- Accepted speed item (Aug 29): C8 IRAM of the remaining per-node flash
  callees (generate_non_pawn_captures/quiets, Position::is_legal_move,
  nnue_k::apply) plus a grow-only scores resize that kills the per-node
  libstdc++ `_M_default_append` flash call. Search tree bit-identical. Board
  startpos bench 14,198 vs 14,027 (+1.2%); strength gate +15.6 +/- 34.6,
  LOS 81.2% vs pre-C8 HEAD: KEEP; 18/18 unit; board-vs-native 3-game clean.
- Board pthread stacks default to 16 KB (`sdkconfig`, was 32 KB): internal SRAM
  got too tight for the old 96 KB peak demand during `test`. `allocate_threads`
  degrades to fewer searchers instead of aborting, and the stack protector
  (`check_min_stack_size`) bails depth gracefully if a searcher ever runs tight.
- The long-bench INT_MIN bug (`1 << 31` as infinite time) is fixed; if the
  long bench ever reports a few hundred nodes total again, suspect a time-budget regression first.
- 2-thread board stability: board must survive a 15-min 2-thread session
  (a 2-thread hang is what killed QIO).
- `app/sdkconfig` is tracked and takes precedence over `sdkconfig.defaults`:
  editing only `.defaults` silently keeps the old value baked into the build
  (caught Aug 29 when a 16B cache-line flip never applied). To change a
  config, edit `app/sdkconfig` directly (or delete it to regenerate from
  `.defaults`), then verify in `app/build/config/sdkconfig.h` before flashing.
