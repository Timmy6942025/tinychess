# MaxDogOne

A fork of [Dog](https://github.com/folkertvanheusden/Dog) (written by Folkert van
Heusden, MIT licensed) for the XIAO ESP32-S3 Plus, with a SPRT-gated experiment
pipeline for tuning the engine. Every search/eval/net change is measured before
it is kept.

## Highlights

- **NNUE big net** (HIDDEN_SIZE=256, 395 KB blob) — SPRT-confirmed **+25 elo**
  vs the shipped 128-wide net (`ab-bignet-20260805-000637` ACCEPT,
  `ab-bignet2-20260805-012247` ACCEPT). Enabled in `app/src/weights.cpp`
  (`#if 0` selects the 197440 B small blob, `#else` the active 394816 B big net).
- **libchess vendored** — this is a monorepo; `app/include/libchess` is regular
  source (with Dog-specific fixes: FEN en-passant validation, `go st` UCI
  support). A canonical copy lives at `github.com/Timmy6942025/libchess`.
- **SPRT harness** — `tools/fast_sprt.sh` (cutechess-cli, elo0/elo1 = 0/20,
  bounds ±2.944, cap 1500 games, 5+0.05). Every verdict is appended to
  `tools/results.log` with a run tag; experiments are reproducible via
  `tools/patches/`. Current reference binary: `tools/runs/bin/Dog-v2-bignet`
  (md5 `194ee46e69dd3a8f14478de206a78503`).
- **Hardware targets** — XIAO ESP32-S3 Plus (WS2812 LED on GPIO44, PSRAM TT up
  to 6 MB, configurable via `app/src/Kconfig`).

## Repo layout

```
app/src/            engine source (search, eval, NNUE, TT, UCI)
app/include/libchess  vendored libchess library (board model, movegen, FEN)
app/main/           ESP32-IDF project (esp32s3 target)
tools/              fast_sprt.sh, native_check.sh, board_session.py, patches/, runs/ (results)
README.md           this file
RESEARCH.md         experiment queue and net-training pipeline plan
HARDWARE_READINESS.md
```

## Build & test (Linux/native)

Requires gcc/g++ 14+ (or clang 14+, gcc produces faster binaries):

```sh
tools/native_check.sh     # builds + runs the 17/17 unit suite + bench, exit 0 = green
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
- Serial tooling: `tools/board_session.py` + `tools/test_board_session.py`
  (e2e harness running a QEMU image), `tools/board_check.sh`.
- The ESP32 version also works with xboard: adapt `app/wrapper.sh` port and
  run `xboard -fUCI -fcp app/wrapper.sh`.

## Experiment pipeline

```sh
tools/fast_sprt.sh ab <tag> <A-binary> <B-binary>   # SPRT: A vs B
tools/fast_sprt.sh baseline                          # fixed-games anchor vs stockfish
```

Verdicts: ACCEPT (keep, exit 0) / REJECT (revert, exit 1) / KEEP-REVERT (cap
reached, decided by llr sign). All verdicts land in `tools/results.log`; match
logs live in `tools/runs/`. Never run a match while building or running
`native_check.sh` (the 7.6 GB dev box OOM-kills cutechess under a parallel
build).

Submodules: `app/Dog-book` (opening book) and `app/src/fathom` (Syzygy
tablebase probing) — clone with `--recursive`.

## Notes

- Rejected experiments are documented in `tools/results.log` (hist3d, checkext,
  TT eval cache, LMR retune) — all REJECT under the corrected SPRT gate
  (elo1=20); see the "Power notes" in `tools/fast_sprt.sh`.
- The big net is markedly slower at endgame mate detection (two-rook ladder
  mate found at depth 19 vs 5); accounted for in `app/src/test.cpp`.
- Native bench with the big net: ~200 kNPS on the dev box (vs ~360 kNPS for
  the small net).

Upstream: https://github.com/folkertvanheusden/Dog
License: MIT
