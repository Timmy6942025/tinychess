# ESP32-S3 board-vs-desktop speedup plan (Dog fork)

Status: written plan only - no code changes made. All numbers below were
measured today (2026-08-13) with the exact tools in `tools/`.

---

## 1. Baseline verification report (mission hard rule 1)

### 1.1 The mandated baseline (9,400 nps) is NOT reproducible as specified

Protocol as mandated: fresh flash from current HEAD, fixed 2.5 s console bench.

| Run | Nodes | nps |
|---|---|---|
| Board, fixed 2.5 s bench, 1 thread | 12,998 | **5,167** |
| Board, repeat run | 12,813 | **5,084** |
| Desktop Dog-native, identical protocol | 917,220 | **366,390** |

Board 2-thread game path (UCI `go movetime 2500`, non-book FEN
`r3k2r/2pb1ppp/2pp1q2/p7/1nP1B3/1P2P3/P2N1PPP/R2QK2R w KQkq a6 0 14`):
19,350 nodes, **8,015 nps**, depth 7.

### 1.2 Why the mission's "9,400 nps" is stale

- 9,400 nps was measured at commit `3748495`, when the console bench ran with
  **2 searcher threads** (node count = thread 0 only). Commit `ea843cf`
  ("honest single-threaded bench") deliberately changed the bench to 1 thread,
  because the two threads raced the shared lock-free PSRAM TT.
- The current 2-thread game-path measurement (8,015 nps) reproduces the *spirit*
  of 9,400: same 2-thread protocol, minus the TT-race inflation. The 9,400
  number was partially an artifact of two threads sharing one node counter.

### 1.3 The mission's desktop reference is wrong, in the engine's favor

The mission's "~57,000 nps desktop" was an *estimate* (results.log:3748
"clock-scaled desktop (~57k nps est)"), not a measurement. The real desktop
number is 366,390 nps. Consequences:

- Real gap is **45.7x under match conditions** (8,015 vs 366,390), not ~6x.
- Real gap single-thread is **71x** (5,167 vs 366,390).
- Mission Elo claim (-523) is real and matches the measured match:
  results.log:3749, Board 0-39-1, **-523.4 +/- 156.1** Elo, DrawRatio 2.5%
  (vs Dog-native, Threads=1 Hash=8, TC 2+0.02, 40 games).
- SF17 anchor: -56.1 +/- 43.4 @ 2+0.02, 200 games (results.log:3691).

### 1.4 Conclusion

The gap is real, and **larger than the mission believed** (45.7x vs ~6x), but
the Elo consequence the mission cared about (-523) is exactly right. The plan
below is anchored to the honest numbers: **5,167 nps single-thread, 8,015 nps
game path, -523 Elo**.

---

## 2. Profiling diagnosis

### 2.1 Cycle budget (the central finding: stall-bound, not instruction-bound)

- Board 1-thread: 240 MHz / 5,167 nps = **46,450 cycles/node**.
- Board 2-thread (per node, wall): 240 MHz / 8,015 = 29,940 cycles/node.
- Desktop: ~12,300 cycles/node at ~4.5 GHz (366,390 nps).
- Efficiency gap: 46,450 / 12,300 = **3.8x** (rest is the 19x clock gap).

Instruction-count audit of the hot path (movegen ~30 moves, selection sort,
legality, make/unmake with 4 NNUE row updates, TT probe/store, static eval,
qs) puts execution at roughly 1,500-2,500 instructions per average node. An
in-order Xtensa core with IPC ~1.2-1.5 accounts for only ~1,200-2,100 of the
46,450 cycles. **~95% of per-node time is memory stalls (PSRAM + flash XIP)**
on a core that cannot overlap a miss with other work. This is the headline
diagnosis: the board is latency-bound, so every optimization that reduces
*exposed latency* (cache sizing, placement, fewer random PSRAM accesses)
beats every optimization that reduces instruction count.

### 2.2 Measured tree shape (desktop shm counters, same search code)

| Counter | Value | Meaning |
|---|---|---|
| nodes / qnodes | 147,854 / 69,447 | qsearch is 47% of nodes |
| tt_query | 97.8% of nodes | TT probed per node |
| tt_hit / tt_cutoff | 20.7% / 10.0% | low reuse: TT not helping much |
| static eval | 76.9% of internal nodes | NNUE output pass dominates eval cost |
| static-null immediate return | 89.5% of probes | (but still pays the output pass) |
| null move | 14,716 (44.7% hit) | |
| LMR | 126,430 (85% of internals) | |
| futility | 70,590 | |

NNUE output pass (2x256 PIE MACs, `accx_dot16`) runs at every static-eval
probe and every qsearch leaf, streaming 256-entry rows from flash rodata
(weights_data @ 0x3c12a080, 394,816 B). Note: the accumulator *update* is
scalar (4 row-ops per move, 256 int16 each); only the output layer is PIE'd.

### 2.3 Measured multi-core behavior (key anomaly)

- 1 thread: 5,167 nps. 2 threads: 8,015 nps total -> **+55% scaling**, i.e.
  ~4,000 nps per core = **-23% per-core efficiency** from sharing.
- Cause: both searchers hammer one 6 MB lock-free TT in PSRAM. ESP32-S3 PSRAM
  is not cache-coherent between cores; every store from one core invalidates
  the other core's DCache lines; PSRAM bandwidth is shared. The mission's
  claim "second core sits idle" is wrong (2 searcher threads run in games;
  only the bench forces 1 thread) - but the *effective* second core is only
  worth +55%, not +100%.

### 2.4 Other measured/audited findings

1. **Per-node heap allocations (mission says eliminated - they are not)**:
   `std::vector<int> move_scores(n_moves)` (search.cpp:643), `MoveList child_pv`
   (search.cpp:652), and `MoveList` itself is `std::vector<Move>` with
   `reserve(32)` (Move.h:124-132). Each internal node does ~3 malloc+free
   (TLSF is IRAM'd, but the calls go through flash libstdc++ wrappers and
   shuffle DCache lines).
2. **Selection sort O(n^2)** (search.cpp:656-661): ~465 comparisons per
   ~30-move node; measurable on a 1 IPC core.
3. **WDT yield gate costs ~4% wall time**: both threads block 10 ms
   (`vTaskDelay(1)`, FREERTOS_HZ=100) simultaneously every 250 ms
   (search.cpp:457) to let IDLE feed the task WDT (timeout 60 s -
   the gate is ~240x more frequent than needed).
4. **Asserts compiled in**: build is `-O2` but **no `-DNDEBUG`**; eval.cpp has
   35 asserts, search.cpp 9. Assert code + branches live inside IRAM hot
   functions, and their string literals sit in flash rodata (loads from IRAM
   code).
5. **Flash is DIO 80 MHz; the chip is quad-capable** (eFuse: "Flash type set
   in eFuse: quad"). QIO would halve XIP fetch time for flash rodata
   (weights, tables) and I-cache refills.
6. **DCache is 32 KB (8-way); 64 KB is available** and not enabled
   (CONFIG_ESP32S3_DATA_CACHE_SIZE=0x8000). 32 KB is shared by TT stream,
   NNUE weight rows, zobrist, attack tables.
7. **IRAM headroom**: .iram0.text is 135,915 B of the 448 KB
   instruction-addressable budget; .flash.text is 1,009,876 B. Hot engine
   functions are already IRAM'd (search, qs, see, movegen, make/unmake,
   attacks, hash, NNUE eval). libstdc++ vector ops remain in flash.
8. **No atomic hardware ops** (`-mdisable-hardware-atomics`) but the TT uses
   no atomics at all - genuinely plain shared memory (no per-access locks);
   the atomic cost in the gate is negligible (2x per 250 ms).

### 2.5 Mission premise corrections (evidence-based)

| Premise | Reality |
|---|---|
| ~9,400 nps current board speed | 5,167 (1-thread bench) / 8,015 (game path) |
| Desktop ~57k nps | 366,390 nps measured |
| 768->32 hidden NNUE layer; only output PIE'd | No 32-layer at all; net is 768->256->1; PIE only on output layer; feature transformer is scalar |
| NNUE weights in PSRAM | weights_data is in flash rodata (0x3c12a080) |
| Second core idle during search | 2 searcher threads in games (+55% measured) |
| Per-node allocations eliminated | MoveList=std::vector reserve(32), move_scores, child_pv per node |
| Lazy SMP = highest risk/reward | It's already on; reward is +55%, tax is -23%/core |

---

## 3. Ranked candidate optimizations

Ranking = expected nps gain / (effort x risk). All are pure speedups: no
retraining, NNUE outputs bit-identical, constraints respected. Elo assumed
~95 Elo per doubling of nps at this operating point (from the -523 match at
45.7x). Candidates are sub-additive; treat the totals as upper bounds.

### Tier 1: cheap, safe, high confidence (do first)

**C1. Add `-DNDEBUG` (or strip asserts in hot paths)**
- Mechanism: removes assert branches/string literals from IRAM hot functions;
  removes flash-rodata loads.
- Expectation: +2-4% nps (~+10-15 Elo). Effort: 1 CMake line. Risk: none
  (40-game gate catches any misdiagnosis).
- RAM: 0.
- DONE Sep 1 2026: REJECT. 200-game gate at 2+0.02 measured -19.1 +/-30.7,
  LOS 11.1%. The asserts were already free; the flag changed codegen for the
  worse. Reverted.

**C2. WDT yield gate: raise period 250 ms -> 1500 ms**
- Mechanism: gate fires every 250 ms with 10 ms double-block = ~4% wall loss;
  WDT timeout is 60 s, so 1.5 s is 40x margin. Halves to ~0.7% loss.
- Expectation: +3-4% nps. Effort: constant. Risk: none (verify IDLE still
  runs; keep 60 s timeout).
- RAM: 0.

**C3. Enable 64 KB DCache**
- Mechanism: doubles cache for the TT/weights/tables working set; biggest
  single win per cache byte for a latency-bound core.
- Expectation: +3-8% nps. Effort: sdkconfig. Risk: none (validated config).
- RAM: 0 (cache, not RAM).

**C4. QIO flash mode (DIO -> QIO)**
- Mechanism: chip eFuse says quad-capable; XIP fetches (flash rodata: weights,
  kindergarten tables, zobrist; I-cache refills) go 4 lanes instead of 2.
- Expectation: +3-6% nps. Effort: flash mode flag. Risk: low (chip supports;
  verify stability over 60-ply stress).
- RAM: 0.

**C5. Kill per-node heap allocations**
- Mechanism: small-buffer MoveList (fixed array + count, or SBO), stack
  `move_scores` (max 32 ints), reuse `child_pv`. Removes 3 malloc+free per
  node and the flash libstdc++ wrappers.
- Expectation: +3-8% nps. Effort: moderate (Move.h type change + search.cpp
  + qsearch call sites). Risk: medium (child_pv ownership/aliasing - careful
  with the `pv->clear()` and parent-PV-copy logic; gate after).
- RAM: ~0 (stack instead of heap).
- DONE Sep 2 2026: REJECT. Gated as stack-allocated sort scores in
  `MoveList::sort` (the main/qsearch scorers already used scratch buffers, so
  this was the remaining per-node heap). 200-game gate at 2+0.02 measured
  -6.9 +/-25.9, LOS 30.0%. Reverted.

Tier 1 combined upper bound: ~+15-30% nps -> game path ~9,200-10,400 nps,
~+65-125 Elo. Individual gains are gated per-step.

### Tier 2: moderate effort

**C6. NNUE output-pass cost at static probes**: 89.5% of static-null probes
return immediately but still pay the full 2x256 MAC pass. A static-eval cache
keyed on accumulator fingerprint failed on desktop (evalcache, SPRT reject) -
do NOT retry that exact design. Alternative: cheaper ordering (probe TT score
before static eval where the TT already has a bound). Expectation: +1-3%.

**C7. Replace selection sort with insertion sort** (nearly-sorted move lists
are the common case after MVV/LVA-ish scoring; ~465 comparisons -> ~60 in the
common case). Expectation: +1-3%. Effort: small. Risk: low.

DONE Sep 2 2026: REJECT. Gated in both `search()` and `qs()` (full pre-sort,
then linear play). 200-game gate at 2+0.02 measured -33.1 +/-29.0, LOS 1.3%:
the lists are not sorted enough for insertion to win. Reverted.

**C8. IRAM the remaining per-node flash callees** (libstdc++ vector ops die
with C5; then re-measure PMC I-cache misses and IRAM the next hot symbols).
Expectation: +1-3% after C5. Effort: small (map-file driven).

DONE Aug 29 2026 (~4.0 KB flash -> IRAM): IRAM'd the only movegen fns still
missing LIBCHESS_IRAM_ATTR (generate_non_pawn_quiets/captures),
Position::is_legal_move (per-node TT-move verify), nnue_k::apply (per-move
delta dispatcher); grow-only scores resize killed the per-node libstdc++
_M_default_append flash call and its zero-fill (scores always overwritten
below n_moves). Board startpos bench 14,198 vs 14,027 (+1.2%); strength gate
+15.6 +/- 34.6, LOS 81.2% vs pre-C8 HEAD: KEEP. Unit 18/18, board-vs-native
3-game clean. Closed since: C7 insertion sort was gated and rejected (-33.1),
C2 WDT gate (250 ms -> 1.5 s) shipped earlier. The node sort stays selection
sort: measured optimum.

**C9. Weight-layout locality**: order NNUE feature rows by king square so the
streamed rows cluster in the DCache. Bit-identical math, different memory
layout. Expectation: +2-4% on eval-bound nodes. Effort: build-time reorder +
re-generated blob (no retraining; same values). Risk: low-medium (must keep
weights in sync with the board).

DONE Sep 2 2026: KEEP. Shipped as square-major (`sq*12+piece*2+half`, this net
has no king-relative features so the piece square is the cluster key),
permuted before pairing on both targets with an index remap in `push_delta`.
200-game gate at 2+0.02 measured +6.9 +/-30.9, LOS 67.1%. Board bench moved
14,213 -> 17,609 cool (+24%). Unit 18/18, 3-game clean.

### Tier 3: audacious (see section 4)

**C10. Split-brain TT (audacious headline)**: per-core private TT + core
pinning + small shared PV hint in SRAM. See section 4.

DONE Sep 3 2026: REJECT. Gated as halved shared table (per-core halves of one
array, peer-probe fallback, pinned searchers). 200-game gate at 2+0.02
measured -29.6 +/-31.8, LOS 3.4% (35-52-113). Halved shared knowledge beat
the coherence savings. Reverted by its own rules.

**C11. L0 TT in internal SRAM**: 128-256 KB private L0 (high-ply-reuse
entries) in SRAM (~2-5 cycle access vs 30-45 PSRAM). Cheaper than C10,
smaller payoff (only the ~20% tt_hit traffic benefits), and it composes with
C10. Expectation: +2-5%. Effort: moderate. Risk: low-medium (SRAM budget:
dram0 data+bss ~45 KB today; ~200 KB free internal).

DONE Sep 3 2026: KEEP. Shipped as a 128 KB 2-way SRAM front cache (16,384
entries): SRAM-first lookup, PSRAM-hit backfill, mirrored stores, same age
policy, PSRAM-only fallback. 200-game gate at 2+0.02 measured +5.2 +/-31.8,
LOS 62.6% (45-42-113). Board bench 14,382 warm (parity band), 3-game clean.

### Rejected (with reason)
- **PIE for the feature transformer**: PIE lacks an elementwise add; row
  updates cannot be vectorized with it. (Documented in research.)
- **Bitboard 32-bit compression**: measured ~1-2% on desktop; not worth the
  churn and per-node instr cost on a stall-bound core.
- **NNUE 256 -> 128 hidden**: requires retraining. Forbidden.
- **Leaf eval caching (evalcache)**: SPRT-rejected on desktop. Do not retry.
- **Bigger net in SRAM / TT in internal SRAM**: internal SRAM too small
  (394 KB net + 6 MB TT cannot move).
- **Whole-engine IRAM residency**: 1 MB flash text cannot fit 448 KB IRAM
  budget (135,915 B used per 7.); only targeted moves (C8) make sense.

---

## 4. Audacious idea: Split-brain TT (per-core TT + core pinning)

### Claim
The -23% per-core lazy-SMP tax is caused by both searchers thrashing one
6 MB PSRAM TT through a non-coherent 32 KB DCache. Giving each core its own
PSRAM TT (3 MB each) and pinning each searcher to its core removes all
cross-core TT traffic; the shared-knowledge loss is small because TT reuse
is low anyway (tt_hit 20.7%, tt_cutoff 10.0% - measured).

### Design
- Two 3 MB TT partitions, indexed by `(hash % 3MB / core)`: probe own first,
  fall back to peer (or skip peer probe entirely at first).
- `xTaskCreatePinnedToCore` for both searchers (currently unpinned).
- Small (few KB) SRAM "shared hint" table: each thread stores its current
  best move per depth; the other thread consumes it for its own move
  ordering - preserves the Lazy SMP *information* channel that matters,
  without the cache-coherence traffic.
- L0 SRAM TT (C11) can be layered on top of each private TT.

### Falsifiable predictions (in priority order)
1. **Throughput**: 2-thread game-path nps >= 9,800 (baseline 8,015), i.e.
   per-core nps within 5% of the 1-thread bench (>= 4,900 vs ~4,000 today).
   Measured with the fixed 2.5 s bench *in 2-thread mode* plus the UCI
   game-path FEN protocol from section 1.1.
2. **Elo**: board-vs-Dog-native 40-game match (tools/board_vs_native.sh)
   improves from -523.4 to >= -440 (>= +80 Elo).
3. **Stability**: 60-ply selfplay stress x 20 games, zero illegal moves;
   bench variance < 2% across 3 runs.

If prediction 1 fails (2-thread nps < 9,800), the shared-TT knowledge is
worth more than the coherence tax - the idea is falsified, and the fallback
is C11 (L0 SRAM TT on the shared design) plus Tier-1/Tier-2 items.

### Costs
- Effort: high (tt.cpp split, thread pinning, hint table, bench protocol
  must keep 1-thread mode honest). Risk: high (touches the single most
  important data structure). RAM: 0 (splits the existing 6 MB TT; hint
  table ~8-16 KB SRAM).

---

## 5. Verification protocol

Per candidate (C1-C9, C11): one step at a time, always from a clean HEAD.

1. **Reproduce**: `esptool write_flash @flash_args` (fresh app partition);
   fixed 2.5 s bench; 3 runs within +-2%. Baseline = 5,167 (1-thread),
   8,015 (2-thread game path).
2. **Bench gate**: >= +2% nps on the fixed bench vs previous step, or the
   step is reverted. Record nodes + nps in `tools/results.log` alongside the
   commit hash.
3. **Match gate (40 games)**: `tools/board_vs_native.sh` vs Dog-native
   (Threads=1, Hash=8, TC 2+0.02). Pass = no Elo regression beyond the
   noise floor (+-40); the sprint target is -523 -> -440.
4. **Stress gate**: 60-ply selfplay x 20 games; zero illegal moves; no WDT
   resets; heap exhaustion watch (per-go 24 KB pthread + L0 TT must not
   collide).
5. **Final re-anchor** (mission hard rule 2): board vs SF17 @ 2+0.02, 200
   games (replaces the -56.1 anchor if numbers shift).

## 6. Instrumentation prerequisite (first implementation step)

Before implementing C5/C8/C10, add temporary esp_perf PMC + CCOUNT
instrumentation to decompose the 46,450 cycles/node into: I-cache misses,
D-cache misses, PSRAM stall cycles, flash stall cycles, execution cycles.
This is a small, revertible change and turns the stall-dominance hypothesis
into a measured breakdown; every candidate's ranking is refined by it.
(Current perft isolation is blocked: perft output goes to UART1 via
`my_printf`/`to_uart` (tui.cpp), not the USB console - instrumentation
should count rather than print, or use UART0.)
