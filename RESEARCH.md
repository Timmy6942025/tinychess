# MaxDogOne — Research Companion (desktop / ESP32-S3)

Companion to `BUILD_PLAN.md` (section numbers below reference that doc).
Written from scratch per the board-free improvement plan, item 3 (plan 0).
Engine: Dog fork, NNUE `HIDDEN_SIZE=256` desktop net, native Linux build used for all
validation. All measured figures are from the native build produced by
`cmake --build app/src/linux-windows/build --target Dog-native`.

---

## 1. NNUE speed profiling (S3 target implications)

### Current engine shape (native, as of this research)
| Component | Detail |
|---|---|
| Feature set | plain 768 (6 piece x 2 colour x 64 square), no king buckets |
| Hidden size | 256 (`HIDDEN_SIZE`), int16 accumulator (big-net baseline) |
| Output | 2 output heads (stm / not-stm), `output_bias`, `SCALE=400` |
| Quantisation | QA=255, QB=64 (see `app/src/nnue.h`) |
| Net blob | `weights_size = 394816` B, embedded in `weights.cpp` |
| Update | incremental `add_feature` / `remove_feature` (2 x 256-row ops per add/remove) |
| Full eval | `Eval::set()` walks both bitboards and adds all 2x32 features |

### Measured bench (host, 2026-08-06)
Time-limited ~2.5 s window, startpos. Recorded in `tools/bench.csv`:
~902k nodes, ~360 kNPS (host load spread 355k-376k).

### Where NNUE time goes (code audit)
- `search()` calls `nnue_evaluate()` at: qsearch leaves, static-null /
  reverse-futility probe, and the single-move case in `search_it()`.
- The incremental accumulator path (`add_feature`/`remove_feature`) is scalar:
  256 int16 adds per feature, ~2-4 features per move -> 512-1024 int16 adds
  per make_move.
- The output pass (`Network::evaluate`) is 2 x 256 MACs of int16xint16 with two
  clamps, recomputed at every qsearch leaf and every static-eval probe. This
  dominates eval cost at leaf nodes.

### Bottleneck ranking (by expected gain)
1. Output pass recomputed at every leaf - a leaf-cached eval would cut a large
   fraction of output passes (biggest single eval-side win). *Attempted as
   `evalcache` experiment: SPRT REJECT (llr -3.02). Reverted.*
2. Int16 accumulator rows are 256 entries = 512 B - ideal width for the S3
   PIE SIMD (16 x 128-bit lanes), but the scalar loop is 256 sequential
   add/sub. Correct PIE kernels were dropped (commit `84e9a0e`); re-port is a
   Phase-1.2 item (ESP32 only, off-limits on desktop validation).
3. No king-relative features (plain 768) - moving to halfKP needs retraining.
4. Feature update not fused (`make_move` does remove+add separately).

### ESP32-S3 implications
- 394 KB net fits 16 MB flash easily; PSRAM (8 MB) holds TT + net copy.
- NPS scaling: host ~360 k at ~3 GHz -> S3 at 240 MHz dual-core lands near the
  "2300-2600 CCRL-class" band from BUILD_PLAN section 0.

---

## 2. Net candidates table (BUILD_PLAN section 2)

Published nets considered (no training on this machine):

| Net candidate | Size | Arch | Self-claim | Use case |
|---|---|---|---|---|
| Dog `quantised-ESP32.bin` | 197 KB | 768 -> 128 -> 1 (2-layer) | shipped, 2866 desktop | default (128-wide) |
| Dog `quantised-big.bin` | 395 KB | 768 -> 256 -> 1 | ACCEPT +25 elo vs 128-wide | **current desktop net** |
| RukChess `net-*.nnue` | 1.5 MB | 768 -> 512 -> 1 | CCRL 3342 (with engine) | max-Elo experiment, **needs arch match** |
| Official SF `nn-37f18f62d772` | 3.5 MB | HalfKAv2 128 | - | not for esp (v2 features) |
| minifish | <64 KB | 768-narrow | tournament fun | speed experiment |

**Decision (validated):** the 256-wide Dog big net is the active net
(commit `23a02e1`, SPRT `ab-bignet-20260805-000637` ACCEPT +25 elo,
`ab-bignet2-20260805-012247` ACCEPT again). RukChess is NOT ported (see section 3).

---

## 3. Net pipeline plan - RukChess converter path (plan 2, M4) - IMPLEMENTED

RukChess ships 768x256/768x512 nets in NNUE `.nnue` format: 4-byte magic "BRKR"
+ 8-byte arch hash + float32 feature weights [768*H], feature bias [H],
output weights [2*H], output bias [1] (file size 1,579,024 for H=512).
RukChess 4.2.0 `Def.h` DEFAULT_NNUE_FILE_NAME = `net-7342fb032855.nnue` — the
`768->512->1` net ported here (downloaded into `tools/nets/`).

Dog's loader hard-coded `HIDDEN_SIZE` and its own 256-net weights; to use the
RukChess 512 net we added:
1. `tools/net_convert.py` - reads the NNUE2
   float arrays, re-quantises them into Dog's embedded binary layout
   (`app/src/weights-ruk.cpp`): int16 feature weights (QA-64), int16 feature
   bias, int16 output weights (QB-512), int32 output bias (QA*QB=32768), plus
   a 40-byte header (magic `MDRK`, source magic `BRKR` + arch hash, dims,
   qa/qb) that the loader validates (incl. blob length). Blob = 789,548 B.
2. `app/src/nnue-ruk.cpp` - a HIDDEN_SIZE=512 implementation of RukChess's
   exact eval (white/black accumulators, same piece/side indexing incl. the
   ^56 mirror for the black perspective, ReLU, /32768 output), selected at compile
   time by `-DUSE_RUK_NET` in the new `Dog-ruk` target; default targets keep
   the 256-net. Unit tests + bench pass with the 512 loader; eval verified
   against NNUE2.cpp semantics on 11 positions (int16 vs float32 within +-2 cp).

SPRT `ab-ruk512` (2+0.02, 256-net baseline) ran 2026-08-07: see
`tools/results.log` for the verdict. Independently of the verdict the converter
pipeline is now real - future nets (any H) just need a regenerate + rebuild.

---
## 4. Memory map (BUILD_PLAN section 1.5)

Final intended shape for the S3:

| Region | Contents |
|---|---|
| SRAM | search stack, eval accumulators (thread-local), TT probe code, hash, counters |
| PSRAM 8 MB | TT buffers (up to 6 MB), network weights copy, large move-score arrays |
| Flash 16 MB | app (code + const), base net + alternate nets, opening book (small), log |

On the desktop validation host the net is embedded in the binary
(`weights.cpp`); PSRAM/SRAM split is S3-only and out of scope for desktop
board-free work.

---

## Open questions / next steps

1. Leaf eval caching (free Elo at fixed nodes) - attempted (`evalcache`),
   REJECTED; needs a correct caching scheme before retry.
2. Re-derive correct PIE SIMD accumulator kernels (bit-exact) - S3 only,
   needs hardware.
3. RukChess 512-net port (item 8) - DONE as a gated `Dog-ruk` target
   (`tools/net_convert.py` + `app/src/nnue-ruk.cpp`); desktop SPRT
   `ab-ruk512-20260807-024334` REJECT (llr -2.22, 0-31-0 @31g, elo -inf) so the
   default stays Dog's own 256-net.
4. Calibration verdicts, 8-item board-free pass (2026-08-06/07): LMR table
   ACCEPT (+34.6 +/- 29.4 elo, llr 2.2, 262g); static-null 121->100 REJECT
   (llr -2.23); futility 180+150d -> 150+110d REJECT (llr -2.23);
   futility 220+180d failed the unit-test gate. Margins unchanged.
   Final-binary anchor vs Stockfish 17 @ 2+0.02, 200 fixed games: -56.1 +/- 43.4
   (63-95-42). Every verdict + match fingerprint in `tools/results.log`.
   Post-plan: LMR deeper x1.15 and LMP both FAILED the unit-test gate
   (mate-in-N two-rook sweep ~3150 vs 32000 needed @ d19) and were reverted -
   the accepted LMR table sits at the aggressive limit; tt.cpp got defensive
   null-ptr/per-mille guards (331e438).
5. Plan phases 0.4/1.x (serial smoke, PSRAM TT, SIMD NNUE, flash net swap,
   threading) are hardware-only - not part of the board-free scope.
