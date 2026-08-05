# MaxDogOne — NNUE / Eval Research

Companion to `BUILD_PLAN.md`. This document records what was investigated and
what is currently known about three research areas:

1. NNUE speed profiling on the native build (the host we test on) and what it
   implies for the ESP32-S3.
2. Feasibility of converting the RukChess 768→512→1 (or 768→256→1) net to
   Dog's loader.
3. The net training pipeline already present in the repo, and a concrete plan
   to produce a stronger same-architecture net.

All figures here were measured against the native build produced by
`tools/native_check.sh` (scalar NNUE accumulation, `-O3 -march=native`).

---

## 1. NNUE speed profiling

### Current engine shape (native, as of this research)

| Component | Detail |
|---|---|
| Feature set | plain 768 (6 pieces × 2 colours × 64 squares), no king buckets, no halfKP |
| Hidden size | 128 (`HIDDEN_SIZE`), int16 accumulator |
| Output | 2 output heads (stm / not-stm), `output_bias`, SCALE=400 |
| Quantisation | QA=255, QB=64 (see `app/src/nnue.h`) |
| Net blob | `weights_size = 197440` bytes, embedded in `weights.cpp` as `alignas(64) constexpr` |
| Update strategy | incremental `add_feature` / `remove_feature` in `nnue.cpp` (2×128 row ops per added/removed feature) |
| Full eval | `Eval::set()` walks both bitboards and adds all 2×32 features |

Memory layout of the 197 440-byte blob (all int16):

```
feature_weights [2*6*64][128]   = 196 608 B
feature_bias            [128]   =     256 B
output_weights     [2][128]     =     512 B
output_bias             [1]     =       2 B
                                  -------
                                   197 378 B  (+ 62 B trailer/padding)
```

### Measured NPS (host, 2026-08-04)

Bench via `printf 'bench\nquit\n' | ./Dog-native`:

| Run | Nodes/second |
|---|---|
| 1 | 480 145 |
| 2 | 576 605 |
| 3 | 492 815 |
| 4 | 781 411 (quieter host) |
| previous recorded baseline | 678 563 |

The bench is a fixed position (startpos, capped depth), so the spread is host
load. **Use ~500–600 kNPS as the reproducible host figure** and treat anything
above as a quiet-machine measurement.

### Where NNUE time goes (code audit, not yet perf-counted)

- `search()` calls `nnue_evaluate()` at:
  - qsearch leaves (`qs()`, every search leaf) — `search.cpp:184/189/193`
  - the static-null / reverse-futility probe — `search.cpp:457`
  - `search_it()` single-move case — `search.cpp:905`
- The **incremental accumulator path** (`add_feature`/`remove_feature`) is
  scalar today: 128 int16 adds per feature, ~2–4 features per move →
  **256–512 int16 adds per make_move**.
- The **full output pass** (`Network::evaluate`) is 2 × 128 macs of
  `int16*int16` with two clamps. This runs at every qsearch leaf and every
  static-eval probe, so it dominates eval cost.

### Bottleneck ranking (by inspection, order of expected gain)

1. **Output pass is recomputed at every leaf.** No lazy-eval: Dog computes the
   256-element dot product even when the cached score would do. A leaf-cached
   eval (`if (hash matches) return cached`) or a "recently-evaluated" stamp
   would cut a large fraction of output passes. *(Biggest single win.)*
2. **Int16 accumulator rows are 128 entries = 256 bytes.** On the S3's 128-bit
   PIE SIMD that is exactly 16×128-bit lanes per row — ideal for `EE.VMULAS`
   kernels, but the current scalar loop is 128 sequential add/sub. The earlier
   `nnue_simd.S` kernels were dropped (commit `84e9a0e`) because they did not
   bit-match the scalar wrapping; a correct port is a Phase-1.2 item.
3. **No king-relative features.** Plain 768 forces the net to learn piece
   placement *globally*. Moving to halfKP-style would change the loader and
   is not feasible without retraining (see §2/§3).
4. **Feature update is not fused.** `make_move` calls remove+add separately
   (two 128-loop passes). A fused `add_sub` pass halves the per-move cost.

### ESP32-S3 implications

- 197 KB net in flash / SRAM is tiny (fits 16 MB flash easily; even the 8 MB
  flash config with a 5 MB spiffs partition has 3 MB app room).
- TT in PSRAM (`app/src/tt.cpp`, up to 6 MB when PSRAM is present) already
  works; the eval hot loop should stay in SRAM (`IRAM_ATTR` on `evaluate`
  exists).
- NPS scaling: host ~500–700 k at ~3 GHz → the S3 at 240 MHz (2 cores) should
  land around 50–150 kNPS with the current scalar eval. That is the
  "2300–2600 CCRL-class" band from `BUILD_PLAN.md` §0.

---

## 2. RukChess 768→64→32 / 768→512→1 conversion feasibility

### What RukChess actually ships (verified from upstream docs, 2026)

- Master branch (`Ilya-Ruk/RukChess`): **768→512→1**, 2 perspectives, output 1.
  Net file 1 579 024 B, floats, `4 B magic + 8 B hash` header.
- GCC port (`Ilya-Ruk/RukChessGCC`): **768→256→1**, net 789 520 B, floats.
- Quantisation: input QA=64, output QB=512.
- Self-claim: CCRL blitz **3248–3257** (engine + its net).

Note the "768→64→32" shape in the BUILD_PLAN text is **not** what RukChess
uses; treat it as a loose reference to *small second layers*. There is no
official 64/32-wide RukChess net.

### Why you cannot drop the net in as-is

Dog's loader (`nnue.cpp` `Network` struct) hard-codes:

- `HIDDEN_SIZE = 128`,
- plain 768 feature indexing (piece-type-major),
- a **single 256-wide output pass with two 128-weight heads**,
- int16 quantisation (QA=255, QB=64).

RukChess uses a *different feature layout, different hidden width, a third
bias layer, floats, and a different quantisation*. The `.nnue` blob embeds an
architecture/hash checksum that Dog's loader ignores. Any conversion therefore
requires:

1. A converter that reads RukChess's 4 B magic + 8 B hash + float arrays and
   emits Dog's exact `weights.cpp` binary layout (768×128 int16 feature rows,
   128 int16 bias, 2×128 int16 output, int16 output bias) with matching
   QA/QB scaling;
2. **Arbitrary-width support** in `nnue.cpp` (make `HIDDEN_SIZE` a runtime
   value) — the only hard part, ~a day of work;
3. **Retraining or rank-projection.** You cannot simply truncate RukChess's
   512-wide hidden layer to 128 without destroying the eval. Options:
   - keep 512 wide (net becomes ~1.5 MB → still fits 16 MB flash, but
     PSRAM-cached; eval cost ×4);
   - train a fresh 128-wide net in Dog's own arch (see §3).

### Recommendation

**Do not port RukChess.** The value is a marginally stronger net that we
cannot evaluate without a physical board, at the cost of a loader rewrite and
a ×4 eval cost. The realistic Elo-per-effort winner is **train a 128-wide net
in the current architecture** (§3), which requires zero loader changes and
keeps eval cost identical.

---

## 3. Net training pipeline plan

### What already exists in the repo

| Artifact | Path | Role |
|---|---|---|
| Data collector (server) | `app/src/collect-fens-Dog-nnue.py` | receives FEN+result+score over TCP:31250, stores in SQLite |
| Data generator (client) | `app/src/gen-train-data.py` | plays self-games with a UCI engine, emits balanced positions to the collector |
| Weights embedder | `app/src/weights.cpp` | `xxd -i quantised.bin` output; `#if 1` selects the 197 440 B blob |
| Tuning macro gen | `app/tune-to-h.py` | converts a `tune.dat` text table into `src/tune.h` (HCE/PSQT-style params) |
| Existing nets | `app/src/quantised-ESP32.bin` (197 KB), `app/src/quantised-big.bin` (395 KB) | shipped nets |

The collection pipeline is a legacy distributed rig: clients (`gen-train-data.py`)
push FEN batches (score = engine's white-centric centipawn eval, nodes, result)
to a central SQLite sink. It is **not** an NNUE trainer — it produces
**training data**, nothing trains on it.

### The gap: no trainer

There is no `train.py`/PyTorch training loop, no validation set, no
quantisation step that writes `quantised.bin`. Producing a net today means:
1. play data → 2. train → 3. quantise → 4. `xxd` into `weights.cpp`.

### Plan (increasing effort)

**Step 1 — write a small PyTorch trainer matching Dog's arch (target: same or
better than shipped 197 KB net).**

- Model: `Linear(768,128) → ClippedReLU(QA=255) → Linear(256,1)` with two
  output heads folded into one 256→1 layer (stm head on first 128 inputs,
  not-stm on second 128). This mirrors `Network::evaluate` exactly.
- Loss: MSE on `(score / SCALE)` from the collector data.
- Quantise: weights → int16 with scale `QA`/`QB` per the existing format;
  write the exact byte layout and regenerate `weights.cpp` via
  `xxd -c 26 -i`.
- Validate: load the blob in `Dog-native`, run `test` (NNUE eval test +
  incremental fuzz) and a quick SPRT vs the shipped net
  (`tools/fast_sprt.sh ab`).
- Effort: a focused ~1–2 week personal project.

**Step 2 — bulk data generation without the network rig.**

`gen-train-data.py` expects the remote collector. For a local run: point
`host` at `127.0.0.1`, run `collect-fens-Dog-nnue.py` locally, generate a few
hundred thousand balanced positions from self-play at fixed nodes. The
`-d <nodes>` cap (default 10 000) and `is_balanced()` filter are already in
place.

**Step 3 — larger net only if Step 1 shows promise.**

Only after a same-arch net beats the shipped one at equal TC should we
consider the RukChess 512-wide experiment (§2).

### Validation gate

Everything is gated by `tools/native_check.sh` (build + 12-group unit suite +
bench) and SPRT via `tools/fast_sprt.sh ab`. A net change is only "shipped"
when:
- `native_check.sh` passes with the new `weights.cpp`;
- `nnue_verify_perft`, the NNUE incremental fuzz, and the eval test all pass;
- SPRT vs the shipped net at TC=5+0.05 is non-negative (keep if ≥0, prefer
  `elo0=0 elo1=10` accept).

---

## Open questions / next actions

1. Add leaf eval caching in `qs()`/`search()` and measure NPS before touching
   the net — free Elo at fixed nodes, likely the single best eval-side win.
2. Re-derive correct PIE SIMD accumulator kernels (bit-exact vs scalar) and
   enable on `ESP32_S3_XIAO`.
3. Confirm whether the S3 build (with 8 MB PSRAM) reports the TT in PSRAM at
   boot (`# PSRAM malloc failed, falling back` would be the red flag).
4. Decide whether to build Step-1 trainer locally or keep the shipped net and
   focus on search (§B of BUILD_PLAN, killer/LMR/aspiration work).
