# Option A Build Plan — "MaxtheDog-S3" (Dog → XIAO ESP32S3 Plus)

> Research companion: see `engine/RESEARCH.md` for NNUE speed profiling,
> RukChess net-conversion feasibility, and the net training pipeline plan.

Fork and heavily modify **Dog** (folkertvanheusden/Dog, MIT, v4.10.2)
to run as a near-3000-rated UCI chess engine on the Seeed Studio **XIAO ESP32S3 Plus**.

> Project name in progress. Working title: **MaxDogOne** (Mahog). Any name works.
> Scope: Option A only. Existing HCE engine in `~/chess/pie-cert` stays independent.

---

## 0. Reality anchor (re-read before every phase)

- A true **3000 CCRL** engine cannot run on any MCU (needs GB RAM + 100 MB nets + multi-m NPS/S).
- Dog-proven ESP32 ceiling ≈ **2400–2700 CCRL-class** with full hardware maximization.
- XIAO ESP32S3 Plus resources we actually have:
  - Dual-core **Xtensa LX7 @ 240 MHz**, 512 KB SRAM (~300 KB usable by app)
  - **8 MB octal PSRAM** (in-package), sequential read ~125 MB/s, random ~8 MB/s
  - **16 MB flash** (16 MB on the Plus), QIO 80 MHz
  - **128-bit PIE SIMD** (~16×int8 lanes, MAC+load ops) — no auto-vectorization, needs esp-nn or hand-written kernels
  - Single-precision FPU only → **integer math mandatory** in eval/search
- Expected strength ladder by engine class (CCR 40/15, NNUE + strong search):
  - 50 kNPS → ~1950–2400 | 200 k → ~2300–2600 | 800 k → ~2650–2900 | 2 M → ~2950–3300

### The anchors that make Option A the right call
| Fact | Detail | Where |
|---|---|---|
| Dog is MIT | fork + vendor freely, commercially usable | repo README |
| XIAO target already built in | `-DESP32_S3_XIAO` in CMake | `app/CMakeLists.txt` |
| Real NNUE on ESP32, proven | Dog v3.0 hit **2866 CCRL** (desktop build), ESP32 bot ~2100–2400 | vanheusden.com/chess/Dog |
| Net budget | 197,440 B quantised net fits the 16 MB Plus | `app/src/quantised-ESP32.bin` |
| TT today is tiny | ESP32 build uses 48 KB TT → PSRAM gives us 4–8 MB | `app/src/tt.*` |

---

## Phase 0 — Toolchain & baseline bring-up (NOT optional)

Goal in one sentence: build Dog stock for the XIAO, flash it, talk UCI to it, and capture a
reproducible NPS/bench baseline. **Gate: do not start Phases 1+ until this passes.**

### 0.1 Install the SDK (nothing is missing on this mythical)

```bash
# A. ESP-IDF v5.x (idf.py) — REQUIRED. Current IDF_PATH must exist before cmake.
mkdir -p ~/esp && cd ~/esp
git clone --recursive -b v5.3 https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3        # downloads toolchain (~1 GB, minutes)

# optional all-targets: ./install.sh esp32s3 esp32 esp32c3
source ~/esp/esp-idf/export.sh            # adds idf.py + xtensa toolchain to PATH (persistent: add to ~/.bashrc)

# B. native niceties for testing (Linux host)
sudo apt-get install ninja-build libudev-dev python3-venv socat
python3 -m pip install --user serial psutil   # wrapper.py deps
```

### 0.2 Fork (this is YOUR git repo for Option A)
```bash
cd ~/chess2
git clone --recursive https://github.com/folkertvanheusden/Dog.git .      # or: git init + subtree; keep upstream remote
git remote rename origin upstream
# NOTE: --recursive pulls the 3 submodules: libchess, Dog-book, fathom
git submodule update --init --recursive
git remote add origin git@github.com:YOU/MAXOne.git
```

### 0.3 First build for XIAO, stock config
```bash
cd app
idf.py set-target esp32s3
idf.py menuconfig   #  →  Component config → (S3): enable octal PSRAM (default is 8 MB),
                    #  → set flash size 16 MB (vendor: qspi 80 MHz)
idf.py build
idf.py -p /dev/ttyACM0 flash
idf.py monitor       # confirm Device "hello" banner + boot count
```

### 0.4 UCI over serial smoke test
```bash
# The ESP32-S3's native USB-to-serial (CDC) appears as /dev/ttyACM0 on the host.
# A minimal smoke test — talk UCI directly:
python3 -m serial.tools.miniterm /dev/ttyACM0 115200
#   then type: uci  →  isready  →  position startpos  →  go depth 8  →  quit

# GUI path (Arena/Cutechess connect through the serial wrapper):
cutechess-cli -engine cmd=app/wrapper.py arg1=/dev/ttyACM0 \
  -engine cmd=/usr/games/stockfish -each proto=uci tc=40/15 \
  -draw 0 1 100 1 60 0 -resign 3 600 0 \
  -openings file=/usr/share/cutechess/Scrambled.epd order=random \
  -games 20 -rounds 2 -repeat -concurrency 1 -sprt elo0=0 elo1=35 \
  -debug -pgnout /tmp/dog-serial.pgn
```

**Expected** (from Dog/esp32 hardware):
- `id name Dog 4.10.0`, `uciok`, `readyok` all appear.
- `info depth 2 … pv` within ms on the start position.
- A stock 40/10 against Stockfish 16 is expected to lose heavily (~+2000 elo gap). **This is normal.**

### 0.5 Baseline benchmark (your project starts here)
- Add `bench` command support (Dog has `go` + terminal; we add a custom UCI `bench`).
- Record `info depth 12 nodes N nps M` at startpos, both cores, default TT → write into
  `output/bench.csv`.
- Record total flash/RAM (ESP32-S3 builds: `idf.py size`).

**Gate to close:** you can run one full `cutechess-cli` 2-game match to completion and see valid
`bestmove` + exact scores, no `hang`. This is the skeleton everything else hangs from.

---

## Phase 1 — Hardware maximization (the big Elo wins live here)

**Ordered by expected ELO-per-effort.**

### 1.1 PSRAM-backed Transposition Table (48 KB → 4–8 MB) ★★★
- Dog TT lives in SRAM today. Move arrays to PSRAM (`malloc`/`heap_caps_MALLOC_SIZE` heap PSRAM).
- Add UCI `Hash` option (default 4 MB, min 32 KB, max 8 MB, step 256 KB).
- Keep TT-fast-path index code in SRAM: only the table buffers in PSRAM.
- Watch: PSRAM random access ~120 ns/transaction — halve TT lookup (use 2 probes) if needed.
- Monitor `info hashfull` (Dog prints it).

*Expected:* +100–200 Elo from deeper/cleaner tree (biggest single jump on this board).

### 1.2 SIMD NNUE (PIE intrinsics / esp-nn) ★★
- Current `evaluate()` loops scalar over HIDDEN_SIZE=128 with 2 output gates. Replace with int8 MMUL:
  - Use `esp-dsp` `dsps_dotprod_s16/s8` or **esp-nn** dense kernels, or hand-written
    `EE.VMULAS.S16.ACCX.LD.IP` kernels in `.S`.
  - 16-byte alignment for the feature-weight rows (they're `alignas` today).
  - Keep the incremental accumulator (`add_piece`/`remove_piece` ~2 rows/move) — only the output
    FC compact (hidden→output) benefits from SIMD; feature-transformer stays incremental.
- 2 cores: run lazy-SMP already present. Eval uses only ints.

*Expected: eval ~2–4× faster → +50–150 cumulative.*

### 1.3 Net storage & hot-swap on flash ▸ PSRAM ▸ eval
- Ship `quantised-ESP32.bin` + an alternative small net in a flash partition (we have 16 MB).
- `setoption name EvalFile <path>` support; mmap/net in flash→ memcpy to PSRAM at boot.
- Follow-model: **RukChess** 1.5 MB `768→512→1` and **minifish** <64 KB remain as swapped-in experiments (Phase 3.3).

### 1.4 The 2nd core / threading (lazy SMP already in Dog)
- Pin whole engine (already) both cores; consider pondering on idle core at low priority — gives UCI terminal win.

### 1.5 Memory map (final shape)
| Region | Contents |
|---|---|
| SRAM | search stack, eval accumulators(thread-local), TT probe code, hash, counters |
| PSRAM 8 MB | TT buffers (up to 6 MB), network weights copy, large stack for move-score arrays |
| Flash 16 MB | app (code + const), base net + alternate nets, opening book (small), log |

---

## Phase 2 — Existing-net strategy (no long training, ~30 min of setup)

The whole point: **don't train on your machine.** Use published nets:

| Net candidate | Size | Arch | Self-claim | Use case |
|---|---|---|---|---|
| Dog `quantised-ESP32.bin` | 197 KB | 768 → 128 → 1 (2-layer) | shipped, 2866 desktop | default |
| RukChess `net-*.nnue` | 1.5 MB | 768 → 512 → 1 | CCRL 3342 (with engine) | max-Elo experiment, **needs arch match** |
| Official SF small `nn-37f18f62d772` | 3.5 MB | HalfKAv2 128 | — | not for esp (v2 features) |
| minifish | <64 KB | 768-narrow | tournament fun | speed experiment |

⚠️ **Critical**: `.nnue` files embed an architecture+featureset checksum. You cannot drop a
RukChess net into Dog's loader — the feature set differs. Two honest paths:
1. **(default)** keep Dog's own arch (768-in/128-hidden) and, if we want a stronger same-size net,
   fine-tune/train OUR net in this SAME arch — but training is explicitly low-priority; skip for now.
2. **(experiment)** write a small net-converter that expands RukChess's 768→64→32 scheme into the
   loader (better search value vs smaller net).

Get **base strength first (Phase 1 + 1.2)**, then decide if you need path 2.

---

## Phase 3 — Search & eval hardening (tune to the S3's budgets)

None of this matters before it's fast; come back after NPS climbs.

- 3.1 **cutechess SPRT harness**: `cutechess-cli -each tc=40/15 st=... ` script in `tools/sprt.sh` + TuningDb staging for manual net line-tuning `bin*(1-x)`.
- 3.2 Search params provenance: Dog's lmr-red tables + eval cut elli further: fit internal units to the
  int8 eval scale (SCALE=400). (Do NOT keep float pruning — breaks reproducibility.)
- 3.3 Time management: map `wtime/btime/winc/binc/movestoo` → Dog's timing; deduct +50−200 ms serial
  overshoot; default `MoveOverhead 100`, ensure no forfeits vs Cutechess with `timemargin`.
- 3.4 **Static null, funnel, and razor bound calibration** to the NPS range — shorthand: budget nodes, not seconds, in the fast regime.

---

## Phase 4 — Measure & iterate (your ELO law)

**You are your own query rig.** Use `cutechess-cli`:
```
tools/match.sh  <- build(s) A vs B, fixed TC, SPRT(-freq) with elo0/elo1
out: ELO delta, LOS for Cores identical build on desktop vs PSRAM build → isolates hardware gains
```

Rating ladder anchors to compare vs:
- `stockfish` (native) → see how big the gap is per NPS (should match ~table in §0)
- Existing **PIE-CERT** engine (your own) → if you ever want a 2nd engine train

**Ship criteria:** NPS 150–400k on two cores @ 240 MHz, TT 4 MB+, net load from flash,
stable UCI over serial, `bestmove` never-times-out, batched PGN in `output/`.

---

## Phase 5 — Polish & innovation (pick * off-the-shelf* free quality)

- `ponder`/lazy SMP on both cores (client option `Ponder`).
- Syzygy probe (already in Dog) — enable when storage is available (SD/USB).
- Book: Dog ships small `Dog-book`; skip or expand if flash allows.
- Net hot-swap from serial (debug: dump net over XMODEM to flash at runtime).
- Multi-threaded play via BLE in later XIAO boards, optional.

---

## Known risks & mitigation
| Risk | Mitigation |
|---|---|
| PSRAM latency kills TT | keep probe code in SRAM, revert to 48 KB/slower if measure Δ
| No vectorization of NNUE on LX7 | use esp-nn / .S kernels; quant target int16→int8 |
| Cutechess forfeits from USB latency | MoveOverhead, socat/wrapper, `st+timemargin` |
| Net arch mismatch | keep Dog 128-arch; convert only with checksum validation |
| 3000 expectations | insist on CCRL 40/15 anchors; honest floor ~2400; ceiling ~2700 |
| Watchdogs (core0) | `vTaskDelay`/idle hooks; stack 60–80 KB for search task |

---

## Milestone summary & Elo map
| Milestone | What you get | Expected CCRL |
|---|---|---|
| M0 baseline (stock Dog on XIAO) | UCI works, bench, hashfull, net, RAM | ~2100–2400 |
| M1 PSRAM TT + real net in flash | 4 MB TT, Hash option | ~2300–2550 |
| M2 SIMD eval on both cores | NNUE int8, ≥2× nps | ~2450–2650 |
| M3 search tuning + timing | stable blitz, SPRT tested | ~2500–2750 |
| M4 RukChess-class net (if converted) | 512-node net | ~2550–2900 tail |
| M5 tuning + refine | iterative SPRT rollups | as far up as your patience |

---

## Immediate next actions (for an agent or for you)
1. Install & export the IDF toolchain, then clone Dog `--recursive` into `chess2`.
2. Set up `app/` first build against the XIAO board (Phase 0.3).
3. Verify the serial wrapper + cutechess works.
4. Then return for Phase 1.1 (PSRAM TT) — that's the first code change you'll make.