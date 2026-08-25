# Output-layer SRAM staging

A one-kilobyte change to the eval interface that buys roughly six percent
node throughput on the board without changing a single search result.

## The observation

The profiling pass that fed the correction-history work left behind exact
numbers for how often the engine calls its evaluator: 6.97 million calls in
one long bench, 0.92 per node, 79 percent of them the qsearch standing pat.
The full output pass costs about 450 ns per node on the desktop - a third of
wall time sits behind that one call chain.

On the ESP32 the cost has a specific shape. `NNUE` points at the weights
blob in flash, so every evaluate() re-reads the two 256-lane output weight
rows plus bias (1 KB total) through flash XIP. The feature rows got their
own fast path in the paired-fused rebuild; the output layer kept paying
memory latency on every single call.

## The change

At boot, after the PSRAM copy settles, `nnue_stage_output_layer()` copies
`output_weights[0]`, `[1]` and `output_bias` - 1088 bytes - into an
internal-SRAM block from `heap_caps_aligned_alloc(MALLOC_CAP_INTERNAL)`.
`Network::evaluate()` reads the staged rows when they exist and falls back
to blob pointers when they do not.

Same bytes either way, which is the entire design contract: results are
bit-exact by construction. If internal SRAM is tight the allocation fails,
a line prints, and the board runs exactly like before. Correctness never
depends on getting the fast path.

## Results

| check | result |
|---|---|
| desktop long bench | 7,582,143 nodes, identical across rebuilds |
| desktop unit gate | 18/18 |
| board bench, staged | 18,773 / 18,781 / 18,829 / 18,843 nps |
| board bench, control (staging compiled out) | 17,722 / 17,743 nps |
| delta | +5.9-6.2% node throughput |
| board-vs-native gate | 4 games, no stalls/forfeits/illegal moves |
| 2-thread UCI smoke | completed both searches |

The control comparison matters: the old ~14.7k reference number predates
current board conditions, so same-day back-to-back numbers are the only
honest pair. On-device Elo value follows the throughput curve the
paired-fused item calibrated (1.8x throughput measured +173.9), putting
this in the +10-15 class.

## What else turned up

Two environment facts surfaced while gating this, neither caused by it:

1. The console test suite aborts on every firmware variant right now,
   including unmodified HEAD: a deterministic 32 KB internal pthread-stack
   allocation failure inside `run_tests()`' thread churn at t=9.1s. UCI
   play is unaffected (threads allocate once at boot). Needs its own
   investigation.
2. This dev Pi was auto-joining the board's DOG-CHESS access point, adding
   httpd client load during benches. Disconnected and set autoconnect off;
   keep it that way when benching the board.

## Thermal reality check

A same-night control rerun exposed something the historical numbers had been
hiding: absolute board bench nps swings about 15% with board temperature.
Cool-boot session: staged/control = 18.8k/17.7k. Hot evening session, same
pair of firmwares: 15.5k/14.6k. The staging delta reproduces in both states
(+4.8-6.2%), and the long-standing "~14,700 nps" reference turns out to have
been a warm-board number. Rule of thumb going forward: only same-session
pairs mean anything on a board bench.

Related: the pthread default stack shrank to 16 KB around the same time to
fix the test-suite abort (`docs/` history in tools/results.log), with
searchers pinned at 28 KB via an explicit esp_pthread config - qsearch depth
pays for every kilobyte below that.
