# MaxDogOne

A fork of [Dog](https://github.com/folkertvanheusden/Dog) (written by Folkert van
Heusden, MIT licensed) for the XIAO ESP32-S3 Plus, with a gate-gated experiment
pipeline for tuning the engine. Every search/eval/net change is measured before
it is kept: 3-game board gates + 200-game desktop gates at 2+0.02 for strength
items, a 40-game clean board match per milestone.

## Highlights

- **NNUE big net** (HIDDEN_SIZE=256, 395 KB blob) — SPRT-confirmed **+25 elo**
  vs the shipped 128-wide net (`ab-bignet-20260805-000637` ACCEPT,
  `ab-bignet2-20260805-012247` ACCEPT). Enabled in `app/src/weights.cpp`
  (`#if 0` selects the 197440 B small blob, `#else` the active 394816 B big net).
- **Board speed (Tier-1)** — the board bench went **5,079 → 6,857 nps**:
  C2 WDT gate period 250 ms → 1.5 s (+3.3%), C5 per-node heap allocations
  killed via depth-indexed per-thread scratch (+3.1%); the rest (~+30%) was a
  clean-rebuild artifact of the harness era. C4 (QIO flash) was tried and
  **reverted**: the runtime-quad bootloader is unstable under sustained
  2-thread load (12-min hang) and the +32.7% it showed was the rebuild artifact.
  A5 (split-brain TT) was dropped: the 2-thread nps gate passed (15.4k ≥ 9,800)
  but the Elo gate failed (0-40 board match). The final 40-game board match at
  the Tier-1 state completed **0-40-0 with zero stalls/forfeits**.
- **Search strength (Phase B, desktop)** — three accepted items, **+116.5 Elo**
  cumulative at 2+0.02 (200-game gates): razoring (+12.2, LOS 78.5%), check
  extension — all in-check moves keep depth, preserved through LMR (~+75,
  LOS 100%), qs SEE pruning (+29.6, LOS 98.0%). Nine probed variants were
  rejected with measured evidence (killers, LMR PV `*3/4`, aspiration ±50/±100,
  TT 4-way, blind singular extension, null-move R=5, razor depth-2, razor
  300+120d — verdicts in `tools/results.log`). All accepted items are ported
  to the board: **bench 7,128 nps** (+3.9%), 3-game gate clean.
- **libchess vendored** — this is a monorepo; `app/include/libchess` is regular
  source (with Dog-specific fixes: FEN en-passant validation, `go st` UCI
  support). A canonical copy lives at `github.com/Timmy6942025/libchess`.
- **Match harness** — `tools/wrapper.py` (line-based pump + bestmove repair for
  the board's USB-JTAG console) makes the XIAO a first-class UCI engine for
  cutechess-cli; `tools/board_session.py` runs the on-board test suite + bench
  + UCI smoke. Every verdict is appended to `tools/results.log`. Earlier
  SPRT-era results (LMR recalibration ACCEPT +34.6 `ab-lmr065`, -56.1 anchor vs
  Stockfish 17) are superseded; the first clean board-vs-native anchor
  (40 games, 2+0.02) was **0-40-0** and the -523.4 anchor is INVALID (clock
  forfeits, see `90ef99e`). Current reference binary: `app/src/linux-windows/build/Dog-native`
  at HEAD (md5 `1773e3c807d0752a40da2ae78ea82924`).
- **Hardware targets** — XIAO ESP32-S3 Plus (WS2812 LED on GPIO44, PSRAM TT up
  to 6 MB, configurable via `app/src/Kconfig`).

## Repo layout

```
app/src/            engine source (search, eval, NNUE, TT, UCI) - single source tree
app/include/libchess  vendored libchess library (board model, movegen, FEN)
app/main/           symlink -> src/ (ESP32-IDF project, esp32s3 target)
tools/              wrapper.py, board_session.py, fast_sprt.sh, native_check.sh, results.log
docs/               upstream Dog reference docs, training notes, experiment queue
README.md           this file
```

## Build & test (Linux/native)

Requires gcc/g++ 14+ (or clang 14+, gcc produces faster binaries):

```sh
tools/native_check.sh     # builds + runs the 18/18 unit suite + bench, exit 0 = green
```

Or manually:

```sh
cd app/src/linux-windows
mkdir build && cd build
cmake ..
make
./Dog-native   # 'Dog-native' is fastest; fall back to Dog-avx512, Dog-avx2, Dog
```

Windows (mingw-w64): `cmake -DCMAKE_TOOLCHAIN_FILE=../mingw64.cmake ..` in
`app/src/linux-windows`.

## Build & flash (ESP32-S3)

```sh
cd app
idf.py build && idf.py flash   # set IDF_PATH, e.g. source ~/esp/esp-idf/export.sh
```

- Board: XIAO ESP32-S3 Plus. LED feature is a Kconfig option
  (`DOG_LED_WS2812`; disable for boards without it or for QEMU).
- Serial tooling: `tools/board_session.py` (on-board test suite + bench + UCI
  smoke), `tools/board_check.sh`, `tools/wrapper.py` (cutechess engine adapter;
  requires a `uci` first when used from a plain terminal).
- Flash with `esptool write_flash "@flash_args"` from `app/build`; boot mode
  (dio/qio) can be verified via a serial DTR toggle.

## Experiment pipeline

- **Desktop strength gates** (search items): 200-game matches at 2+0.02 vs the
  previous accepted state via cutechess-cli; keep on positive Elo + LOS, revert
  otherwise. Deterministic node-count comparisons at fixed depth supplement the
  Elo signal.
- **Board integrity gates**: 3-game matches vs the desktop native (no stalls,
  forfeits, or illegal moves), plus a bench on every ported state.
- **Milestone matches**: full 40-game board-vs-native runs (2+0.02) per phase.

Verdicts: ACCEPT (keep, exit 0) / REJECT (revert, exit 1) — all land in
`tools/results.log`. Never run a match while building or running
`native_check.sh` (the 7.6 GB dev box OOM-kills cutechess under a parallel
build).

Submodules: `app/Dog-book` (opening book) and `app/src/fathom` (Syzygy
tablebase probing) — clone with `--recursive`.

## Notes

- Rejected experiments are documented in `tools/results.log`; the current
  rejected list: killers (redundant with the history mechanism), LMR PV `*2/3 →
  *3/4`, aspiration windows ±50/±100 (75 is the optimum), TT 4-way buckets +
  deeper-keep, blind singular extension (broke the mate-in-N sweep), null-move
  R=5, razor depth ≤ 2 and razor 300+120d — the engine's search parameters sit
  at a measured local optimum for all probed directions.
- The big net is markedly slower at endgame mate detection (two-rook ladder
  mate found at depth 19 vs 5); accounted for in `app/src/test.cpp`.
- Native bench with the big net: ~200 kNPS on the dev box (vs ~360 kNPS for
  the small net); the board benches ~7,100 nps (bench is position-dependent —
  game-path positions run ~2x faster than the start position).
- Board Elo vs the native at 2+0.02 remains far behind despite the +116.5
  Elo: the ~20-40x speed gap (~90-110 Elo of the deficit) plus the residual
  eval/search gap dominates; the gain shows as longer, more competitive games.

Upstream: https://github.com/folkertvanheusden/Dog
License: MIT
