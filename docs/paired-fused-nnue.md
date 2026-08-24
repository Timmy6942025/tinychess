# Paired-Fused NNUE rebuild

Status: shipped. This doc records why, and what the measurements said along the way.

## What I measured

Callgrind on the old binary, 3M nodes, midgame FEN, one thread:

| where | share |
|---|---|
| Eval::remove_piece | 21.5% |
| Eval::add_piece | 21.5% |
| nnue_evaluate (+ its stl_algobase clamps) | 17.3% |
| attackers_to (all clones) | 10.2% |
| Position::make/unmake_move | ~5% |

Six of every ten instructions went into the evaluation pipeline. Search itself was under 5%. So when the question came up "which component deserves a full rebuild", the answer was sitting right there in the profile.

Two structural problems, both visible in nnue.cpp:

1. Every piece event runs two separate 256-lane scalar loops (one per perspective), each streaming from its own 512B weight row. The two rows live 196KB apart in the blob, so every capture pays four random cache-line walks. A castling move: eight loops.
2. The inference loop leans on the compiler. GCC vectorized the plain add loop but gave up on the clamp-and-square output loop (the `stl_algobase` line above is `std::clamp` refusing to fold). On ESP32 the story is worse: only the output dot product got hand-written PIE assembly; the accumulator updates, most of the work, run as plain C loops on a 240MHz core.

## What's new here

The net is a plain 780-input flip-based net: both perspectives read the *same* weight table, own view at row `f`, other view at row `f+384`, always together. That pairing is a property nobody else seems to exploit. Surveyed prior art (Stockfish, Berserk, Halogen, Alexandria, Viridithas, Seer): everyone stores one flat table and processes perspectives separately; Berserk/Alexandria fuse multiple deltas per perspective, but no public engine updates both accumulators in a single pass, and none co-locates the two rows a piece event touches.

One subtlety cost me a debugging cycle: the partner of own-row `64p+s` is not `384+64p+s` but `384+64p+(s^56)`, because the square flips between perspectives. The permutation pairs each row with its true partner; `nnue.cpp` documents the index math.

Three ideas, combined:

**Paired rows.** At startup the 768-row blob permutes into 384 pairs of `[own|other]` rows, so one piece event reads one contiguous 1KB region instead of two distant ones. One page touch instead of two, one prefetch stream. Desktop builds the permuted copy once from .rodata; the board permutes its PSRAM copy in place via cycle walk so PSRAM never holds two tables.

**Fused dual-perspective passes.** All deltas of a position change go into one sweep over both accumulators. Unmake rebuilds the inverse batch from the existing undo journal. `Eval::set` batches all 32 pieces into a single pass.

**Explicit SIMD everywhere the profile pointed.** NEON on the aarch64 host, SSE2 as the x86 floor, scalar fallback for sanitizer builds. ESP32-S3 gets PIE assembly for the update loops too: the S3 has no wrapping int16 vector add (VADDS saturates by spec), so chunks are zero-widened to s32 with VZIP.16 against fresh zero registers, combined exactly in s32 (a handful of widened rows cannot reach 2^31), then narrowed back with VUNZIP.16, whose even elements are precisely the low 16 bits of each widened lane. Same wrap-multiply trick stays for the output dot.

Why this is safe to the bit: everything is integer addition mod 2^16, which commutes and associates. Reordering deltas, fusing them, changing lane order, none of it can move a value.

## What the numbers said

The desktop result surprised me, and it's the most useful finding in here.

- Instructions: -12% total Ir for the same 1M-node search; the eval pipeline itself dropped ~40%.
- L1 read misses: -17% (the paired layout doing its job).
- Wall clock: dead even. 3.30s vs 3.30s median over alternating runs, 2M nodes each.

The old evaluator was never the *time* bottleneck on a big out-of-order desktop core - it was instruction-heavy but memory-latency-bound, and the OOO engine had been hiding those loads all along. Callgrind counts instructions, not stalls; the profile pointed where the bytes were cheap, not where the nanoseconds were.

The board is a different machine with a different answer. Its update loops were genuinely serial scalar C, and PSRAM latency makes every saved pass count:

- Old firmware, console bench (startpos, 2.5s): 20,554 nodes, cumulative nps ~8,200.
- New firmware, same bench: completes depth 8 with the same node count as the desktop binary (27,677) and reaches 36,600+ nodes within budget, cumulative nps ~14,600. About **1.8x node throughput**, and fixed-depth searches now match the desktop digit-for-digit through depth 8.

## The bug the hardware caught

The first board build produced sane static evals but insane search scores (-32 pawns from the startpos). Depth ladders against known-good binaries localized it to depth 5, en-passant territory: my PIE dispatch assumed deltas arrived removals-first, but ENPASSANT pushes sub,add,sub and CASTLING pushes sub,add,sub,add. The drain treated an addition as a removal, so any en-passant or castle line corrupted both accumulators by ±2x a weight row.

Two things made this painless instead of painful: the boot selftest (kernels are validated against the scalar reference on-device, and the engine falls back silently if they ever disagree), and the decomposition selftest added after this incident (apply() itself is checked against scrambled op orders). Three flash cycles later the ladder matched the desktop exactly. The NEON path never had the bug because it honors each delta's flag individually - one more argument for keeping the scalar reference honest.

## Interface

The outside world sees no change. `make_move(Eval*, Position&, Move&)` still journals and advances the board; `unmake_move` still inverts; `undo_t` keeps its fields because the test suite prints them.

## Validation record

1. Differential evals: 4,000 positions from quiet-labeled.epd, old vs new binary - identical output, byte for byte.
2. Fixed-depth searches: 6 positions x depths 7 and 9 - identical node counts, scores, PVs, best moves, hashfull.
3. Unit gate: 18/18 tests OK, including the pinned eval values {-985, 436, 1293, 357, 80} and incremental-equals-fresh fuzzing.
4. Desktop speed: parity (measured, not assumed).
5. Board speed: ~1.8x node throughput; fixed-depth node equality with the desktop through depth 8.
6. Strength gate, desktop vs baseline: 200 games at 2+0.02 -> 39-42-119, Elo -5.2 +/- 30.7, LOS 36.9%. A coin flip, which is what bit-exactness predicts.
7. Board-vs-native gate: 3 clean games, no stalls, forfeits or illegal moves.
