# REQUIREMENTS — MaxtheDog-S3 (Option A)

Everything needed to build, flash, and test the Dog fork for the XIAO ESP32S3 Plus.
Installed on the dev Pi (Debian 13 trixie, arm64) on 2026-08-03 and **re-verified
in full on 2026-08-07**. Use this file to reproduce the setup on a fresh machine.

---

## Hardware

| Item | Spec | Notes |
|---|---|---|
| Seeed Studio XIAO ESP32S3 Plus | ESP32-S3R8, dual LX7 @ 240 MHz, 512 KB SRAM, 8 MB octal PSRAM, 16 MB flash | build target already in Dog: `-DESP32_S3_XIAO` |
| USB cable | data-capable USB-C | for flash + UCI serial (CDC, /dev/ttyACM0) |

## Host system (verified 2026-08-07)

- Debian 13 (trixie) **arm64** — Raspberry Pi (this is the pie-cert dev machine too)
- 4 cores, 7.6 GiB RAM (the SPRT runs saturate the machine — one match at a time)
- **Disk caveat: 89% full / ~13 GB free** (was 41 GB free at install time) — watch
  `tools/runs/` PGN growth and clean old runs before big match batches
- User in `dialout` group (serial access) — already true for `timmy`

## Toolchain — installed & verified (all re-checked 2026-08-07)

### 1. ESP-IDF v5.3 + esp32s3 toolchain (REQUIRED, ~1 GB)

```bash
mkdir -p ~/esp && cd ~/esp
git clone --recursive -b v5.3 https://github.com/espressif/esp-idf.git
cd esp-idf
git submodule update --init --recursive   # 3rd pass if clone timed out
./install.sh esp32s3                      # downloads xtensa-esp-elf 13.2.0 + python env

# activate per-shell (alias already added to ~/.bashrc, line ~151):
alias get_idf=". $HOME/esp/esp-idf/export.sh"
```

Verified: `idf.py --version` → ESP-IDF v5.3; `xtensa-esp32s3-elf-gcc` → crosstool-NG esp-13.2.0.

### 2. System packages (apt)

```bash
sudo apt-get install ninja-build flex bison gperf ccache libudev-dev \
     libssl-dev dfu-util socat python3-venv python3-pip git wget
# Qt6 (needed only to build cutechess-cli from source):
sudo apt-get install qt6-base-dev qt6-5compat-dev qt6-svg-dev
```

Verified versions: ninja 1.12.1, socat 1.8.0.3.

### 3. Python (pip — note PEP 668, use --break-system-packages)

```bash
python3 -m pip install --user --break-system-packages serial psutil python-chess
```

Verified: pyserial 3.5 (serial IO + list_ports OK), psutil 7.0.0,
python-chess 1.11.2 (**needed by tools/epd_test.py**, the WAC suite runner).
numpy 2.2.4 also present (not required by any current tool).

### 4. cutechess-cli 1.5.1 (NOT in Debian repos — built from source)

```bash
mkdir -p ~/cutechess && cd ~/cutechess
git clone --depth 1 --branch v1.5.1 https://github.com/cutechess/cutechess.git src
cd src
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWITH_TESTS=OFF
cmake --build build -j2
install -m 755 build/cutechess-cli ~/bin/cutechess-cli
```

Verified: `cutechess-cli --version` → 1.5.1, Qt 6.8.2.

### 5. Stockfish 17 (reference opponent, added 2026-08-04)

```bash
# Not in Debian repos; binary installed at:
/usr/local/bin/stockfish        # "Stockfish 17 by the Stockfish developers"
```

The ELO anchor baseline (`tools/fast_sprt.sh baseline`) plays Dog vs this binary
(200 games @ 2+0.02; last anchor: **-56.1 +/- 43.4** on 2026-08-07, vs -301.3 before LMR).

## Engine repo & daily workflow

| Item | Value |
|---|---|
| Repo dir | `/home/timmy/chess2/engine` (run every command from there) |
| origin | `https://github.com/Timmy6942025/tinychess.git` — **push here** (branch `main`) |
| upstream | `https://github.com/folkertvanheusden/Dog.git` — tracking only, never push |
| Current HEAD | up to date with `origin/main` as of 2026-08-07 (commit `331e438`) |

Per-change gate (search/eval changes only get SPRTed after passing):

```bash
cmake --build app/src/linux-windows/build --target Dog-native -j4 2>&1 | grep -cE " error"   # must be 0
printf 'test\nquit\n' | timeout 1800 stdbuf -o0 app/src/linux-windows/build/Dog-native | grep -cE "assert fail|MISMATCH"  # must be 0
python3 tools/epd_test.py --engine app/src/linux-windows/build/Dog-native --suite tools/suites/wacnew.epd --time 1000  # ref 260/299
nohup env TC=2+0.02 ALPHA=0.1 BETA=0.1 CONC=1 tools/fast_sprt.sh ab <tag> <A> <B> > /tmp/opencode/<tag>.log 2>&1 &   # SPRT
```

Tooling map (all in `engine/tools/`):
- `fast_sprt.sh` — SPRT (`ab`) / fixed-match (`baseline`) harness; verdicts + fingerprints appended to `tools/results.log`
- `native_check.sh` — one-shot clean build + full unit test suite (17/17)
- `epd_test.py` + `suites/wacnew.epd` — WAC tactical scoring (300 positions)
- `board_check.sh` / `board_session.py` / `test_board_session.py` — ESP32 flash + serial validation
- `net_convert.py` + `gen_lmr_table.py` — RukChess net converter (item 8, REJECTed) / LMR table generator
- `bench.csv` — NPS benchmark record; `openings.epd` — SPRT book
- `PIPELINE.md` — auto-worker handoff protocol (NOTE: its "Current state" section is stale as of 08-07; results.log is authoritative)
- Docs: `RESEARCH.md` (root) = design + verdicts of the 8-item plan; `docs/RESEARCH.md` = earlier companion (superseded)

## Phase 0 bring-up notes (verified 2026-08-08)

- **Build**: `idf.py build` MUST be run from `engine/app/`. Building from `build/`
  silently produces stale binaries (prints success without recompiling).
- **Flash**: `idf.py flash` hangs on this setup; use esptool directly from
  `engine/app/build/`:
  `python -m esptool --chip esp32s3 -p /dev/ttyACM1 -b 460800 --before default_reset --after hard_reset write_flash "@flash_args"`
  (`tools/board_check.sh` now does build + esptool flash + validation).
- **Console**: USB-Serial/JTAG on /dev/ttyACM0 or /dev/ttyACM1 (node changes on
  re-enumeration). Board boots into a banner + prompt; `uci` → UCI mode,
  `test` → unit tests, `bench [long]` → bench, `bps` → baudrate.
- **Opening book**: `go` with a book position returns a book move instantly
  (polyglot `/spiffs/dog-book.bin`, e.g. `# book suggestion: h2h4`) and the search
  never runs; then auto-ponder (`allow_ponder`, default on) starts an **unbounded**
  search. To exercise the real search use a non-book line:
  `position startpos moves a2a3 d7d6 b2b4`, `go depth 12`.
- **Task watchdog**: two searcher threads + auto-ponder saturated both cores,
  starving IDLE → wdt panic every 5 s. Fixed with a synchronized yield gate in
  `search.cpp` (ESP32 only): both searchers `vTaskDelay(1)` together every 250 ms
  via a task-notification handshake (peer handle from `searcher()`, main.cpp:294).
  Verified 0 wdt hits through go + ponder. A node-counter trigger does NOT work —
  the compiler folds the counter update into unreachable code.
- **Performance baseline (scalar NNUE, 240 MHz)**: ~4.3k nps; `go depth 12` ≈ 40 s;
  full `test` suite (NNUE perft up to depth 5) ≈ 1–2 h; bench is minutes-per-position
  at depth 10. Host-native build does the same suite in seconds (~450k nps on the Pi).
- `board_session.py` timeouts already tuned to these numbers; it also resets the
  board (DTR/RTS) into a clean state at open (no-op on ptys).
- The wdt also fires during the `test` suite (test pthread spins without yielding)
  — cosmetic, dev-tool only, does not affect the results.

## Shell setup (~/.bashrc additions already applied)

```bash
alias get_idf=". $HOME/esp/esp-idf/export.sh"   # activate IDF in interactive shells
export PATH="$HOME/bin:$PATH"                    # cutechess-cli
```

## Not installed (intentionally)

| Item | Why |
|---|---|
| cutechess GUI | CLI suffices for SPRT runs; GUI needs full Qt widgets |
| syzygy tablebases | optional, later phase (storage-based) |

## Quick sanity check (run all, expect no errors)

```bash
. ~/esp/esp-idf/export.sh
idf.py --version                      # ESP-IDF v5.3
xtensa-esp32s3-elf-gcc --version      # 13.2.0
cutechess-cli --version               # 1.5.1 / Qt 6.8.2
/usr/local/bin/stockfish version      # Stockfish 17
python3 -c "import serial, psutil, chess; print('py deps OK')"
ninja --version && socat -V | head -1
```
