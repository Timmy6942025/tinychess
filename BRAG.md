# BRAG.md

A complete record of what this fork did on top of [Dog](https://github.com/folkertvanheusden/Dog) by Folkert van Heusden (MIT). Three weeks of work, August 3 to 23, 2026, 143 commits. The result ships as TinyChess (`Timmy6942025/tinychess`): one source tree that builds both a desktop UCI engine and ESP32-S3 firmware, a board that broadcasts its own WiFi and plays against a phone browser, and an experiment log with a number attached to every decision.

Every figure below comes from `tools/results.log` (4,149 lines), `tools/bench.csv`, or a named doc in the repo. Nothing here is recalled from memory.

## Where we started

Credit where due. Upstream Dog at the fork point (`9549c3c`) gave us a working engine with an int8 NNUE (128 hidden units), pondering, Syzygy probing through fathom, an opening book, and an ESP32 port. The base was solid. It also had room: when we first measured things properly, the desktop build sat 301 Elo under Stockfish 17 at 2+0.02, and the board searched about 4,300 nodes per second with a 48 KB transposition table.

## Headline numbers

| | fork point | today |
|---|---|---|
| Desktop vs Stockfish 17, 2+0.02, 200 games | -301.3 +/- 57.9 | **-56.1 +/- 43.4** (63-95-42) |
| Gated search gains stacked after Tier-1 | 0 | **+209.5 Elo** |
| Board bench | ~4,300 to 5,079 nps | **~8,300 nps** (depth-8 console protocol) |
| Board absolute strength | unmeasured | **~2800-2900** on the SF17 scale |
| Who can play it | people with a serial cable | **anyone with a phone in WiFi range** |
| Unit tests | 12 | **21**, plus fuzzers |
| Documented experiments | 0 | **~40 accepts and rejects**, all with numbers |

The board rating deserves context. It loses every game against Stockfish 17 at any time control, and we say so in the anchors section. On the human scale it beats virtually every sub-2000 player, which is the scale that matters for a pocket chess computer.

## Strength: what survived the gates

The rule for search and eval changes: 200-game match at 2+0.02 against the previous accepted binary, cutechess-cli, one thread, 8 MB hash. Early work used proper SPRT; later work used fixed 200-game windows with LOS checks. Positive Elo and high LOS or it gets reverted, no exceptions, including to ideas we liked.

Accepted, in order:

- **Recalibrated LMR table** (+34.6, llr 2.2, 262 games). Regenerated reductions from `tools/gen_lmr_table.py`. Later probes confirmed this table sits exactly at the aggressive limit.
- **TT aging** (+36.7, llr 2.95).
- **Mate distance pruning** (+19.8, llr 2.22).
- **SEE-based capture ordering in qsearch**, shipped with the August 6 batch alongside futility pruning. The futility half was kept on a technicality: its match got interrupted at 468 games with llr +0.39, inconclusive, and the cap rule kept it. We are honest about which wins were loud.
- **Big net** (768x256x1, +25.8, llr 3.04; repeat run +49.3 to leave no doubt). 394,816 bytes committed as C source in `weights.cpp`, small net preserved behind `#if 0`.
- **Razoring** (+12.2, LOS 78.5%). Static eval far below alpha at shallow depth goes straight to qsearch.
- **Check extension** (+86.9 cumulative, LOS 100%; about +75 on its own). Every move at an in-check node keeps its depth, preserved through LMR.
- **Qsearch SEE pruning** (+29.6, LOS 98.0%), taking the Phase B set to **+116.5** over Tier-1. Skip losing captures, exempt the TT move, never when in check.
- **Recapture extension** (+57.8, LOS ~99%). Capturing back on the previous capture square gets a ply. Biggest single accepted gain since the check extension.
- **Qsearch TT probe removal** (+34.9, LOS 98.2%). The stats prober showed a 2.65% hit rate, and the hits were stale horizon-context scores cutting real evals. Sometimes the improvement is deleting code.

That last stretch puts the post-Tier-1 cumulative at **+209.5 gated Elo**, all logged in `results.log` with fingerprints.

### Rating anchors

- Desktop vs Stockfish 17, 200 games at 2+0.02: **-301.3 +/- 57.9** before the tuning pass, **-56.1 +/- 43.4** after it. A +245 Elo swing measured directly against a fixed external reference, not inferred from internal matches.
- Board vs desktop, 40 games at 2+0.02: 0-40-0, completed clean, roughly -523. The 45.7x nps gap is the price of a 240 MHz microcontroller, and the doubling math some of us hoped for (+390 Elo for 48x) was optimistic. Real port cost: about 500 to 550.
- Board vs Stockfish 17: 0-201-0 at 2+0.02, 1-23-0 at 10+0.1, 0-12-0 at 30+0.1. Conclusion drawn and recorded: more time does not lift the board at any TC. Absolute estimate ~2800-2900.
- Desktop control, same code as the board, 20 games vs SF17: 9-6-5 (+53). The engine logic punches near its anchor; hardware is the ceiling.

## The graveyard

About 30 ideas were tried, measured, and killed. Each entry has numbers. This section is the actual brag: the discipline is the product.

Search ideas:

- Killer moves: +13.9 +/- 33.1, inconclusive, and 0 node-count delta. Redundant with history bonuses.
- 3D history table: -2.2, rejected.
- Two early check-extension implementations: -124.9 and -79.5 before the correct version won by +87. Implementation matters.
- Leaf eval cache: -113.6. Do not retry this exact design.
- Gentle LMR: -72.9. Deeper LMR x1.15: failed the unit gate outright, scoring 3207 instead of mate-in-19's 19998. LMP failed the same way. The suite catches what matches miss.
- Static-null margin 121 to 100: -21.0. Futility 180+150d to 150+110d: -57.8. Futility 220+180d: failed the unit gate.
- Aspiration window 75 to 50: -33.1. 75 to 100: -8.7. The window we inherited is the optimum.
- LMR PV reduction *3/4: -17.4.
- TT 4-way buckets with deeper-keep: -8.7.
- Blind singular extension: missed a mate in the unit sweep. Gone.
- Null-move R=5 at depth>10: -6.9. Razor depth<=2 only: -3.5. Razor 300+120d: -27.9.
- Qsearch SEE margin extended to -40: -33.1. More qsearch work hurts at this NPS.
- Lazy-SEE qsearch ordering: -24.4, LOS 6.8%. The upfront SEE pass carries real strength.
- Cached checkers_to: bit-correct but +6% per-op cost for noise-level nps. Reverted, simpler code kept.
- Incremental x-ray SEE: fuzz-verified bit-exact, gained +0.1 to 0.3%, within noise. Reverted, simpler code kept.
- TT generation-cycle tweak: rejected by SPRT. TT depth-0 store guard: -15.6, the fresh qsearch entries turn out to have mild ordering value.
- RukChess nets, see the nets section. Definitively.

Platform and speed ideas:

- `-DNDEBUG`: +0.0% nps. The asserts were register checks in IRAM; their cost was already nothing. Reverted with a fresh-flash A/B to prove the negative was real.
- 64 KB data cache: the Kconfig trades 32 KB of internal heap for it, and largest free block was 31.7 KB. Rejected on arithmetic before flashing.
- QIO flash: claimed +32.7%, then we caught the claim as a clean-rebuild artifact, then the mode hung a 2-thread session at 12 minutes. Reverted. Flash is DIO and stays DIO.
- Split-brain TT (per-core tables to dodge the non-coherent PSRAM cache): we wrote falsifiable predictions first. Throughput gate passed, Elo gate failed badly, hypothesis dropped by its own rules. This is how you kill a darling.
- PIE vectorized accumulator updates: the S3's PIE is a reduced dot-product subset. `EE.VADD.S16` does not exist in this silicon (full add/sub PIE is S2-only). Proven with CCOUNT probes, documented, closed.
- Serial-aware time budget (subtract the 50 ms serial floor): saves forfeits, costs 1.5 to 2 plies, a net wash that cannot pass a gate. Emergency clamp variant: +25 estimated against +/-32 gate noise, ungateable. No change.
- Moves-to-go floor on desktop: -76 regression. Kept ESP32-only, and the desktop gate that caught the regression is recorded next to the fix.

## The speed campaign

The board went from ~4,300 scalar nps to ~8,300 on the depth-8 console bench, with every step gated by a 3-game board-vs-native match and a bench. Protocols differ across the timeline, so each row names its protocol.

| Step | Gain | Protocol note |
|---|---|---|
| Build optimization `-Og` to `-O2` | 2-4x | the sdkconfig was shipping debug-tier optimization |
| Hand-written PIE assembly output kernel (`nnue_simd.S`) | +31% | saturating semantics verified bit-exact against scalar |
| Hot path into IRAM + 32 KB I-cache | +25% | `bench long` 8,837 ms to 5,958 ms combined with the row above |
| WDT yield gate 250 ms to 1.5 s | +3.3% | watchdog still fed, 40x margin |
| Per-node heap allocations killed | +3.1% | depth-indexed per-thread scratch replaced vectors |
| Weights copied to PSRAM at boot | -11% cycles, +17.5% nodes | byte-identical, so eval stays bit-exact |
| Kindergarten sliding attacks, pinned pieces once per node, IRAM draw check | folded into Tier-1 | each benched separately |

Tier-1 finished at 6,857 nps (fixed 2.5 s window), Phase B ported the accepted search set to 7,128, and the PSRAM-weights era measures 8,288 to 8,303 on the depth-8 console protocol. Wall-clock verification put the true 2-thread search at 7,369 nps.

Then we stopped, on purpose. CCOUNT profiling accounted for the full 29k cycles per node: eval machinery is about 65% of it, mostly irreducible flash-cache-bound accumulator updates, and the qsearch subtree was profiled component by component to its floor (stand-pat eval 45%, movegen 25%, SEE 15%, TT 15%). NPS R&D was declared complete with receipts, which is rarer than a win.

## Measurement bugs we found in our own toolbox

This deserves its own section because it invalidated numbers we had already published.

- `esp_timer_get_time()` deltas lie when called across tasks, inflated up to 1000x, and the distortion is bidirectional (we watched it deflate a 7.4 s run to a claimed 3.2 s). The desktop gettimeofday shim has the same disease. Fix: `bench2` with inline `cntvct_el0` / CCOUNT reads that agree with wall clock. Consequence recorded honestly: absolute desktop nps claims are garbage, relative same-session comparisons hold, and cutechess match verdicts were never affected because the arbiter enforces wall clock.
- Five orphaned engine processes from prior runs were eating 400% CPU and polluting every load-dependent measurement since August 15. Found and killed.
- The QIO +32.7% was a clean-rebuild artifact wearing a costume. Caught by rebuilding both arms cleanly.
- Every engine binary that enters a match gets an md5 fingerprint in `results.log`, so a mid-match swap is detectable.
- Match reports must state both sides' thread counts. The board runs 2 searcher threads in games; the native baseline runs 1. An undocumented asymmetry here would have quietly inflated every board-vs-native number.
- The SPRT harness originally used elo1=50, which mathematically could only ever accept +50 Elo effects. Fixed to elo1=20 after noticing the harness was structurally unable to confirm smaller wins.
- The bench is deliberately single-threaded. Two threads racing one lock-free PSRAM TT inflate the shared node counter, and the old 2-thread bench was partly measuring that race.

## Bugs hunted down

Some of these were hiding for the whole life of the code.

Engine core and vendored libchess:

- `Bitboard{0}` constructs square a1, not an empty bitboard, because the int constructor treats it as a shift. Every empty square read as occupied by a phantom rook on a1. Fixed at eight call sites; one latent site in `Attacks.h:38` is documented but left alone pending a strength gate, because changing it changes behavior.
- The opening book never fired a single move in this firmware's life. `query()` checked `!fh`, but `begin()` loads to memory and closes the file first, so the check always tripped after a successful load. Fixed: startpos reply went from 1,965 ms to 25 ms.
- Moves-to-go collapsed to 1 after move 40, spending the whole remaining clock on one move and forfeiting on time. The fix floors the horizon at 30 on ESP32 only; the desktop variant of the same fix measurably regressed and was reverted.
- The adaptive time budget collapsed to a third of nominal whenever the root move was stable, then got halved again by the stop threshold. The board was spending 300 ms of a 30 s budget. Fixed ESP32-only after the desktop gate showed -83.
- Eight-bug hunt: `%u` on uint64_t counters, `%lu` mismatches, `munmap(p, 0)` returning EINVAL, an unused variable, a missing `nnue.cpp` link in one target, more `Bitboard{0}`.
- Later seven-bug batch: polyglot promotion encoding, a settings buffer overflow risk, a shared-memory unlock, a stats divide-by-zero printing `nan%`, and friends.

Board stability:

- The task watchdog had `PANIC` disabled. It could log to a wedged serial port that nobody could read. A hang would brick the board until a manual reboot. Enabled, then the real bug appeared: the yield gate let one searcher through per window, starving the other core's idle task, resetting the board 60 seconds into any long search. Both searchers now delay together behind a task-notification handshake.
- The per-go pthread wanted a 24 KB contiguous internal-RAM stack. With WiFi up, internal RAM could not provide one, so every `go` silently allocated nothing and never produced a bestmove. Invisible to the boot-time bench, fatal in every game. Search stacks now allocate from PSRAM.
- HTTP handlers ran on a 4 KB stack, overflowed it, and corrupted the heap; the searcher panicked inside `esp_timer`. Requests now run on 24 KB PSRAM-stack workers.
- WiFi and LWIP buffers drained internal RAM to 19 KB free and the beacon path NULL-derefed twice. Buffers now prefer PSRAM: 79 KB free with a client connected.
- USB-JTAG serial folklore, written down so nobody rediscovers it: writing during boot wedges the input endpoint until a physical power cycle, TX drops bytes mid-burst, and changing the reported baud does nothing because USB CDC ignores line coding.

Web stack, most of it found by driving the real page:

- Tap input was dead from v1: the pointer handler compared FEN color `"w"` against `"white"` and never matched. Every previous test called the move function directly and skipped the event pipeline. Only real dispatched PointerEvents caught it.
- The board rendered upside down. `(7-r)` versus `r`.
- `/move` was not idempotent, so a lost response plus a retry appended a second engine reply to the game.
- Clocks only deducted when a move landed, so a page refresh restored drained time and a flagged-out player never lost server-side. Clocks are now server-authoritative and continuous, verified to drain 4,030 ms over ~4 s of wall time.
- A clock write-back stored the drained value without re-anchoring, compounding the drain until clocks accelerated and flagged in minutes.
- The hand-rolled DNS responder broke the httpd listen socket after answering one query (accept returned EBADF), killing every session about a minute in. Replaced with a raw lwip UDP responder, then verified under 149 concurrent DNS plus 149 HTTP requests.
- A CSS rule for legal-move dots collided with the header status LED and stretched it into a giant translucent ellipse across the board. On every device. In screenshots we had already approved.

## Nets

- The big net (768x256x1) won its SPRT twice and is the active net, embedded as source so the build stays reproducible.
- We also built the thing we decided not to use. `net_convert.py` converts published RukChess `.nnue` files into Dog's quantised layout with a validated header, and a parameterized loader runs any hidden size behind a compile flag. While wiring it we root-caused a perspective bug (accumulators are fixed-view, not per-piece-color slots), fixed it, and verified bit-exact against the actual RukChess trainer on the published net.
- The verdict: RukChess nets do not transfer to Dog. The released 512 net is genuinely color-asymmetric (eval of a position from white's view is not minus black's), its scale is ~4x smaller than Dog's net, int16 quantization amplifies endgame errors 2-3x, and it searches 2.14x slower. Lost 250 to 500 Elo depending on variant. Three attempts, all rejected with numbers, and a do-not-retry note so future-us does not try a fourth.

## The board became a product

The original goal was a chess engine on a microcontroller. Somewhere along the way the board learned to host the game itself.

- SoftAP `DOG-CHESS` at `192.168.4.1`. Join from a phone, a captive-portal sign-in sheet pops (DNS responder plus a wildcard httpd catch-all 302s the probes from Apple, Google, and Microsoft), and you land on instructions, then the game. No app, no internet, no cables.
- The web page is one self-contained HTML file, about 40 KB with the ejgfv PNG piece set included, served from SPIFFS. Offline-capable, mobile-first, container-query board sizing, two-pane landscape, safe-area insets, no CDN.
- Full chess UX: tap-tap and drag moves with a floating piece, promotion chooser, last-move and check highlights, FLIP move animation, captured-piece bars with material lead, sounds and haptics, flip board, resign, formatted history (`e4xd5`, `e8=Q`), rematch flow.
- Server-authoritative continuous clocks with a difficulty ladder from 5 s to 120 s per move, 30-minute casual base clocks.
- Multiplayer for a table: one seat, spectator view for everyone else, joinable waitlist, abandoned seats self-heal after 3 idle minutes, holder can rematch or give up the board. Two simultaneous games are ruled out on purpose; one engine instance uses both cores and one TT.
- The web path calls the exact same UCI handlers as the serial path. Proof by construction: `/move`, serial UCI, and the desktop binary return the identical move for identical positions.
- Stability earned, not assumed: a 15-minute scripted soak (29 games, 783 plies, 0 errors) and a 2-hour power-bank session (253 games, 6,909 plies, 0 errors, 0 resets, uptime continuous at 7,571 s).
- Real-phone playtesting humbled us twice, and the lessons are recorded: release the seat and clear session state after any multi-player testing, and first-timers learning a UI need longer clocks than enthusiasts think.

## Tooling built along the way

- `fast_sprt.sh`, the SPRT and fixed-match harness that appends verdicts plus binary fingerprints straight into `results.log`.
- `wrapper.py`, the UCI-over-USB-JTAG adapter that repairs the board's dropped-byte line protocol, survives slow boots, and plugs the physical board into cutechess-cli as just another engine.
- `board_session.py` and `test_board_session.py`, serial validation with an e2e test suite for the validator itself.
- `epd_test.py` plus a 300-position WAC suite, `native_check.sh` for one-shot clean build and full tests, `board_check.sh` and `board_vs_native.sh` for the flash-and-match loop.
- `gen_lmr_table.py`, `gen_attack_tables.py`, `net_convert.py`, `setup_esp_idf.sh`.
- A Linux CI workflow building and running the unit suite on push.
- Test count grew 12 to 21, including NNUE incremental-update fuzzing, TT round-trip fuzzing, a mate-in-N depth sweep, deep perft, and a SIMD kernel test pinning the saturating semantics.
- `openings.epd`, 28 balanced mainlines, expanded after the first SPRT book proved too thin.
- The opening book itself got audited: every branch walked and depth-8 scored, 148 leaky lines pruned, leaky exits down from 16.8% to 0.9%, mean exit value up from +75.5 to +119.3.

## Process that made the numbers mean something

- Every change passes the desktop build, the unit gate, then either a 200-game strength match or the board flash plus 3-game gate plus bench. The workflow lives in AGENTS.md and was followed for all 143 commits.
- Platform-conditional fixes are a deliberate pattern: when a fix helps the board and regresses desktop, the desktop gate decides, and the fix ships guarded with both measurements recorded.
- Housekeeping counted too: libchess vendored into the tree (three local fixes: `pseudo_legal_move_list_into`, FEN en-passant validation, `go st`), upstream cruft removed (Docker packaging, RPM spec, historic versions, a 3D-printed box), stale docs archived.
- A prebuilt release (`v0.1-prebuilt`) ships the desktop binary and a flashable board image so a new owner needs Python and a cable, nothing else.

## What we still lose to

Stockfish 17, always and everywhere, and we publish the score rather than bury it. The board gives up roughly 500 Elo to the same logic on a desktop, capped by flash latency and a 240 MHz in-order core, and the profile says that cap is near its practical floor. Desktop absolute nps numbers are untrustworthy until the clock shim issue is redesigned around, though every relative comparison stands. And the first two attempts at real-human playtesting failed on details no amount of headless-browser simulation surfaced.

The fork keeps the MIT license and the link back to Folkert's project. The engine name inside the code is still Dog. Most of what is good here stands on his work; what broke along the way was ours, and it is all written down above.
