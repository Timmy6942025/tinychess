# MaxDogOne — Hardware Readiness

Target board: **Seeed Studio XIAO ESP32-S3 Plus** (`-DESP32_S3_XIAO`).
This document records the memory/stack/clock budgets we actually have, what
the firmware currently does with them, and what still needs to change before
the board is "hardware-ready".

> Companion to `BUILD_PLAN.md` (Phase 1 = hardware maximization) and
> `RESEARCH.md` (eval side). Validation gate remains `tools/native_check.sh`;
> every hardware change that touches search/eval must keep it green.

---

## 1. The budgets (measured from sdkconfig + datasheet)

| Resource | Size | What the plan assumes | Current sdkconfig |
|---|---|---|---|
| SRAM | 512 KB (datasheet), ~300 KB usable | search stack, eval accumulators, TT probe code | `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` |
| PSRAM | 8 MB octal (in-package) | TT 4–8 MB, net copy, large arrays | `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_SPEED_80M` → **matches** |
| Flash | **16 MB on the Plus** | app + nets + book | **`CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y` — MISMATCH ⚠️** |
| CPU | 2× LX7 @ 240 MHz | — | `CONFIG_IDF_TARGET_ESP32S3=y` |
| SIMD | 128-bit PIE (16×int8) | hand-written kernels | not used (scalar NNUE since `84e9a0e`) |

### The flash-size mismatch (must fix before shipping)

`sdkconfig` declares **8 MB** flash but the XIAO ESP32-S3 **Plus** ships
**16 MB**. Consequences today:

- `partitions.csv` reserves only `0x300000` (3 MB) for `app0` and leaves a
  5 MB `spiffs` region that nothing needs — we are wasting ~8 MB.
- The 197 440 B net + book + future alternate nets fit either way, so nothing
  is *broken*, but we are not using the board's real budget.

**Fix (Phase 1.1, trivial):**
```
CONFIG_ESPTOOLPY_FLASHSIZE=16MB
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
```
and optionally shrink `spiffs` (5 MB → 1 MB) and give `app0` the rest.

---

## 2. Memory map today

| Region | Contents | Size | Location |
|---|---|---|---|
| Flash (const) | `weights.cpp` net blob (197 440 B), opening book `dog-book.bin`, code | ~? | flash (read via cache) |
| SRAM | per-thread `search_pars_t` (history 1536 B + killers 1024 B + Move arrays + nnue eval accumulators), TT probe code, input buffers | small (KBs) | internal |
| PSRAM | TT entries (up to **6 MB** when present) | `tt::allocate()` | `heap_caps_malloc(MALLOC_CAP_SPIRAM)` |
| SRAM fallback | TT entries when PSRAM missing/broken | `ESP32_TT_RAM_SIZE = 98304` (96 KB) | `tt.h:28` |

`tt.cpp:47–77` already does the right thing: probe PSRAM first (capped at
`max_sp_size = 6 MB`), fall back to 96 KB SRAM. The PSRAM path is confirmed
working (commit `c85d7ee` + `e80d99e`).

### SRAM budget check (per search thread, 2 threads by default on S3)

| Allocation | Bytes |
|---|---|
| `history[2*6*64]` (int16) | 1 536 |
| `killers[256]` (uint32 Move) | 1 024 |
| `best_moves[128]` (uint32 Move) | 512 |
| `nnue_eval` accumulators (2 × 128 int16) | 512 |
| `chess_stats`, `Position`, misc | small |
| **≈ total per thread** | **≈ 4–5 KB** |

Well within the ~300 KB usable SRAM even with both cores searching. The
killer-move array added in the search work (2× per ply × 256) is 1 KB/thread —
negligible.

---

## 3. Stack budgets

| Task | Size | Source | Risk |
|---|---|---|---|
| main/UCI task | 6 144 B | `CONFIG_ESP_MAIN_TASK_STACK_SIZE=6144` | OK for parsing/IO |
| **search thread** | **24 576 B** | `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=24576` | **⚠️ see below** |
| timer/IPC/event tasks | 1 280–5 120 B | sdkconfig | OK |

The search thread runs `search()` → `qs()` recursively with
`std::array<undo_t,4>` make/unmake, `libchess::MoveList` temporaries, and up
to `max_depth` recursion. The plan's risk table (§0) recommended **60–80 KB
for the search task**.

Current protection: `check_min_stack_size()` (`main.cpp:605`) samples the
FreeRTOS high-water mark each iteration and, at `<1280 B`, forces the search
to stop QS; at `<768 B` it hard-stops. That prevents a crash but **cripples
strength** (search ends early on deep lines).

**Recommendation:** raise the pthread default stack for the search task to
`32768`–`49152` B before the first over-the-air game, or create search threads
with an explicit `xTaskCreatePinnedToCore(..., 60*1024)` stack. Verify with a
long `bench` and watch `# dts: ... level N` high-water trace for the real
minimum.

---

## 4. TT sizing & latency

- PSRAM random-access ~120 ns/transaction; TT uses **1 probe** (`fastrange32`,
  no bucketing, no two-probe scheme). At 50–150 kNPS the single probe is fine;
  revisit only if measured `hashfull` shows hot-index thrash.
- `Hash` UCI option: not yet exposed as a `setoption`. `tt::set_size()`
  exists and is called from the config handler, but the `Hash` UCI option is
  **not registered** — add it in `main.cpp` `uci_handler` so the GUI can pick
  4/6 MB.
- ESP32 uses `ESP32_TT_RAM_SIZE` (96 KB SRAM) as the hard floor — keep it.

---

## 5. NNUE on hardware

- `evaluate()` is `IRAM_ATTR` (good: TT probe + output pass in SRAM).
- Accumulator add/sub is **scalar** (128-entry int16 loop). The earlier PIE
  SIMD `.S` kernels were removed (`84e9a0e`) because they didn't bit-match the
  scalar wrapping. Re-adding correct SIMD kernels is the Phase-1.2 Elo item;
  until then budget eval cost as 128 scalar adds/feature.
- Net blob is `constexpr` in flash, no PSRAM copy needed (cache-mapped).

---

## 6. UCI timing / clock forfeits (Phase 3.3)

- `MOVE_OVERHEAD_MS = 100` reserved in both `movetime` and clock (`wtime`)
  paths — the clock path now subtracts overhead from our remaining time
  (`main.cpp`, see the timing work) so `bestmove` never arrives late over
  serial.
- `movestogo` is now honored when a GUI sends it; otherwise a 40-move horizon
  estimate is used. `wtime/btime/winc/binc` map correctly via
  `go_parameters.*()`.
- Serial latency is the main forfeit risk; keep `MoveOverhead` ≥ 100 on the
  board. No forfeit-prone code path found in review.

---

## 7. Action list (hardware)

| # | Item | Effort | Blocks |
|---|---|---|---|
| 1 | Set flash to 16 MB; slim `spiffs`; regrow `app0` | minutes | shipping build |
| 2 | Raise search-thread stack to ≥ 32 KB (ideally 60 KB via pinned task) | small | deep-search stability |
| 3 | Register `Hash` UCI option (wires `tt::set_size`) | small | GUI TT control |
| 4 | Re-add bit-exact PIE SIMD accumulator kernels | days | eval NPS ≥2× |
| 5 | On-board `bench` + high-water trace to confirm real stack floor | minutes | calibrate #2 |
| 6 | Verify `# PSRAM malloc failed, falling back` does **not** print on the Plus | boot log | confirm TT in PSRAM |

Every item keeps `tools/native_check.sh` green as the acceptance gate
(no physical board available to iterate on).
