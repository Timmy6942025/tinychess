# GUI Plan: Portable Web-Playable Chess Companion

Goal: turn the XIAO ESP32S3 Plus into a pocket chess computer that anyone can
play against. The board broadcasts a WiFi hotspot; a friend connects with a
phone, opens an IP in the browser, and plays the engine running on the board.
Battery-powered, thumb-sized, no cables, no internet required.

## Vision

- Board runs the engine + a tiny web server, exposed as a WiFi network
  (e.g. SSID "DOG-CHESS").
- Phone (or laptop) joins the network, browses to `http://192.168.4.1`
  (the SoftAP gateway IP).
- A self-contained web page (no CDN, works offline) renders a chessboard:
  tap/drag to move, clocks, game state, difficulty, battery readout.
- One game at a time. The board is the opponent; ponders while the human
  thinks (the engine already has internal pondering).
- Serial UCI mode keeps working unchanged (dev + desktop gates).

## Current state (baseline)

- Firmware: `014edb4` era — ESP32-only time-budget fix active; NNUE
  768->256->1 in PSRAM; TT in PSRAM; bench ~8.2k nps; SPIFFS holds the
  pruned book (~126 KB).
- RAM: largest free internal block ~31.7 KB (searcher threads use 24 KB
  stacks); PSRAM 8 MB has headroom.
- Serial console mode + UCI mode coexist today (`uci` switches); engine
  reads commands from the console input task (`main_task`), UCI wire output
  guarded by `uci_console_mutex`.
- Known USB-JTAG quirk: serial TX can drop bytes mid-burst (cosmetic, the
  wrapper repairs it). Irrelevant over HTTP — WiFi is lossless.

## Architecture

```
 Phone browser                         ESP32S3 Plus
 +-----------------+                  +----------------------------------+
 | index.html      |  HTTP GET  /     | esp_http_server (SoftAP 2.4GHz)  |
 | app.js (board)  | ---------------> | static assets served from SPIFFS |
 |                |  POST /new       |                                  |
 |                |  POST /move      | UCI-command pipe + result         |
 |                |  GET  /state     |   (reuses the existing go/pos     |
 |                |  GET  /battery   |    handler, not a parallel one)   |
 +-----------------+                  | -> search threads (existing)     |
                                      | -> TT / NNUE / book (existing)   |
                                      +----------------------------------+
```

Board-side components:

1. **SoftAP**: ESP-IDF `esp_wifi` in `WIFI_MODE_AP`, fixed IP
   `192.168.4.1`. WPA2-PSK with a printed/simple password (open AP is a
   fallback if pairing friction matters).
2. **HTTP server**: `esp_http_server` (IDF component) serving:
   - `GET /` + assets from SPIFFS (`/spiffs/web/`),
   - `POST /new`  {color, level}  -> resets the game (fresh position, clocks),
   - `POST /move` {uci moves so far} -> runs the search, returns bestmove +
     info (score/depth),
   - `GET /state` -> position, legal moves, clocks, last info line (for
     spectators/refresh),
   - `GET /battery` -> `adc`-read battery voltage + percent.
3. **Engine bridge (the meaty part)**: a command pipe consumed by the
   existing command-handler path so the search logic, time management
   (including the ESP32 time-budget fix), TT, book and pondering are used
   bit-identically to the serial path:
   - httpd `POST /move` -> queue `position ... moves ...` + `go <clocks>`
     + a future; the engine handler runs the search as today; result (move,
     score, depth) resolves the future -> JSON response.
   - Serial UCI stays on the same queue (one consumer, two producers).
   - Pondering: after replying, if enabled, start the existing ponder on
     the predicted line while the human thinks; stop on the next `/move`.
   - Concurrency: guard the shared engine state with the existing mutex
     pattern (`uci_console_mutex` / `search_fen_lock`).

Phone-side components (all static, no CDN):

4. **app.js**: vanilla-JS board (canvas or SVG, tap-to-tap and drag),
   move generation by local legality check + `GET /state` for legal moves
   (or compute client-side with a tiny hand-rolled move gen — decision
   below), clocks, promotion picker, "New game" + color/level settings,
   low-battery banner.
5. **index.html / style.css**: dark board, mobile-first, one screen.

## Phases

### Phase 0 — Spike: hotspot + web page — ✅ DONE (2026-08-19, commit 2f3e285 + b2aa902)
- Enable SoftAP + `esp_http_server` in the build (sdkconfig.defaults). ✅
- Serve a static test page from SPIFFS (`/spiffs/web/index.html`). ✅
- Verify: phone joins the AP, browses to `http://192.168.4.1`, page loads.
  ✅ (end-to-end verified over real wifi from the dev machine; a phone is
  the same path)
- Measure heap before/after WiFi+httpd init; if internal heap is tight,
  push WiFi buffers to PSRAM (Kconfig) — gate: engine bench within 2% of
  baseline (~8.2k nps) and unit suite unaffected. ✅

**Verified (gate: 3-game board-vs-native at 10+0.1, wifi client connected):**
- SoftAP `DOG-CHESS` up @ 192.168.4.1; `GET /` 200 (2.8 KB page), `GET /state`
  live JSON, `POST /move` 501 stub (Phase 1). All three `[web]` boot lines
  present, no crashes.
- Bench 8,269 nps (baseline 8,245 — within 2%); board 0-3-0 vs native,
  zero disconnects/stalls/illegal moves (board loses on strength only).
- Internal heap: ~4.5 KB free with a client connected (pre-tuning) → 20 KB
  (WiFi buffer pools 10/32/16 → 4/8/8, httpd stack 8K→4K).
- **Found & fixed during verification:** the per-`go` pthread (UCIService.h
  ESP32 patch) needs a 24 KB contiguous internal-RAM stack; with the SoftAP
  up there is none at go-time, so every serial `go` silently failed
  (`heap_caps_malloc fail`, no bestmove — invisible to the bench, which
  reuses boot-time threads). Fix: allocate the go-thread stack from PSRAM via
  `esp_pthread` `stack_alloc_caps` (guarded by
  `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`, now pinned in
  sdkconfig.defaults). The bench gate alone was insufficient — the go-path
  probe is now part of the board check.
- **Wrapper hardening (tools/wrapper.py):** buffer stdin until the full
  console prompt (120 s — the wifi-era boot is slower), never write
  mid-boot (a pre-banner write wedges the USB-JTAG input endpoint until a
  *physical* power cycle), and filter forwarded lines to UCI responses (the
  web companion's wifi/DHCP logs print after the prompt and broke the
  cutechess handshake). Gate command needs `arg=/dev/ttyACM0`
  (cutechess-cli 1.5.1 syntax, not `arg1=`).

### Phase 1 — Engine bridge — ✅ DONE (2026-08-19, commit bab5e47)
- Command pipe + futures; `/move` executes a real search and returns the
  bestmove within the expected time budget. ✅
- `/new`, `/state` (FEN, legal moves, score/depth), `/battery`. ✅
- Keep serial UCI working (3-game board-vs-native gate, unit 18 OK). ✅
- Gate: web game at movetime ~1s plays a full game vs serial-driven board
  with identical results on identical positions (same-code proof). ✅

**How it works:** the httpd calls the SAME registered UCI handlers as the
serial path (web_engine_set_position / web_engine_go_movetime in main.cpp
wrap the registered position/go handlers). A mutex serializes the two
producers (serial UCI loop, httpd) — searches are movetime-bounded, so a
blocked producer always recovers. The page sends the full move list on
every /move, so the position re-derives from scratch per request and any
console-side tinkering self-heals. This replaces the "command pipe +
futures" sketch: the handlers ARE the single consumer; the mutex is the
serialization; the result is stashed by go_handler for the JSON response.

**API:** `POST /move {"moves":"e2e4 e7e5 g1f3","movetime":1000}` ->
`{"bestmove":"b8c6","score":-95,"depth":11,"pv":"..."}`. `POST /new`
{color, level} resets to startpos (color/level stored, used in Phase 2).
`GET /state` adds `fen` + `last`. `GET /battery` reads the XIAO battery
ADC (raw; voltage scaling is Phase 2/3).

**Verified:** same-code proof — /move, serial UCI and desktop native all
return `b8c6` for `e2e4 e7e5 g1f3` @ movetime 1000. 60-ply HTTP self-play
game, no crashes, each /move ~1.1 s for a 1 s budget. Gate: 0-3-0 vs
native at 10+0.1 with a wifi client connected, zero infra issues. Bench
10,080 nps (baseline 8,245). Internal heap with client: 79 KB free.

**Deep verification (2026-08-20, commit 30a2fa9):** endpoint matrix all
correct (bad JSON 400, method mismatch 405, oversized movetime capped to
60 s, invalid moves fall back to the valid prefix); web requests wait
behind a serial `go` (mutex) and both producers get correct results; a
serial *infinite* go makes /move return HTTP 500 at exactly 90.3 s with
no crash and clean recovery after `stop` (fixed: worker/`run_web_search`
capture by value — the old by-ref captures dangled on that detach path);
fixed `/new` color use-after-free (cJSON freed before the response was
built); stale-result guard (`valid=false` at go start); `/state` never
blocks (try-lock + stored FEN fallback); terminal positions return
`{"game_over":true,"result":"black_wins"|...}` instead of the search's
sentinel bestmove (fool's mate verified). Soak: 3 full web games (514
plies, one 30 s transient request stall — page should retry; board
recovered instantly), zero panics, heap stable 54-74 KB. Unit 18 OK,
desktop bench 591,807 nps.

**Found & fixed during acceptance (stability):**
- The engine handlers must NOT run on the httpd task: the 4 KB stack
  (Phase-0 tuning) overflowed, corrupted the heap, and the searcher
  panicked in `esp_timer timer_insert` (LoadStoreAlignment). Each /move
  now spawns a 24 KB PSRAM-stack worker thread (identical to the serial
  go-pthread pattern); httpd stack raised to 8 KB, body capped at 2 KB.
- The internal heap ran dry (~19 KB free with a client) under /move TCP
  bursts and the wifi ppTask NULL-derefed in
  `ieee80211_hostap_send_beacon_process` (LoadProhibited, 2 panics).
  `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`: wifi+LWIP buffers prefer
  PSRAM — internal heap with client went 19 KB → 79 KB free.

### Phase 2 — Web GUI (acceptance: a friend plays a complete game on a phone)
- Board rendering, move input (tap + drag), promotion, clocks, result
  display, new-game screen, difficulty slider (maps to think-time:
  ~0.3s .. 120s/move), battery indicator. ✅
- Pondering enabled during human think time. ✅
- Playtest: 3 full games on a phone with no crashes, no state desync. ✅

**How it works (commit be67094):** the page is a single self-contained
`data/web/index.html` (~22 KB: HTML+CSS+JS, Unicode piece glyphs, no
assets) served from SPIFFS. The human color + difficulty slider POST
`/new {color, level}`; the server stores the session (color, level,
clocks, move log) and the level maps server-side to a per-move budget
(0.3 s .. 120 s) + increment. The page sends the full move list +
`human_ms` (measured reply→tap) on every `/move`; the server books the
engine's own elapsed (measured inside the shared go handler) and the
human's, adds the level increment, and flags a side at zero — always
through the same go handler, so the ESP32 time-budget fix governs every
level. The engine auto-moves when it is to move (black start, refresh
join) via an empty-list `/move`. Legal moves are server-provided in
`/state` (`"legal":[...]`, snapshot taken by the position handler) —
the page never generates moves itself, so tap targets can never desync
and every submitted list self-heals against console tinkering. `/state`
also carries `clock {white,black,inc}`, `level`, `color`, the full
`moves` list (page-refresh recovery) and `game_over`/`result`. The
bridge snapshots (result, position fen, legal moves) are now guarded by
a dedicated `g_web_state_mutex` (never held during a search) — this
also fixed a pre-existing torn-read race with a serial `go` mid-web-
game, and `/state` serves the stored position, never the live one (a
ponder search mutates `sp.at(0)->pos` in place, which is NOT the game
position).

**Verified live:** level mapping (L1 → 261 ms, L9 → 60.2 s, depth 24);
pondering runs during human think (serial info lines climbing); 3-game
playtest via the page contract — draw 210 plies, white_wins 171, black
_wins 156 — zero crashes, zero desync (state moves + fen side-to-move
checked every ply); engine flag path (white clock hit 0 → black_wins)
and mate path (`f2f3 e7e5 g2g4 d8h4` → game_over black_wins with clocks
intact) both verified; heap stable ~70 KB; desktop unit gate 18 OK;
desktop bench 591,807 nps. The 90 s worker timeout was raised to 150 s
(the L10 budget is 120 s); the /move body cap stays 2 KB.

**Decisions recorded (Phase 2):** legal moves are server-provided
(`/state`); the difficulty mapping lives server-side (levels 1-10 →
300/500/1k/2k/4k/8k/15k/30k/60k/120k ms, inc 500/750/1k/1.5k/2k/3k/5k/
7.5k/10k/15k ms, base clock 10:00 per side); AP stays OPEN (zero-friction
table demo, short range — WPA2 via a define if ever needed); spectating
is free via /state polling — the page that moved owns the game (last
writer wins, full-list self-healing); the server keeps the web game's
move log, so a mid-game page refresh rejoins the running game (Phase-3
item done early). Known limits: the single-task httpd queues /state
behind a long search (the page suppresses the reconnect banner while
its own /move is pending), and the serial console is still dev-only
mid-game.

**Deep verification (2026-08-20, follow-up commit):** a
play-the-real-page pass (headless Chromium driving the actual UI) found
and fixed seven bugs that API-level tests could not see:

1. **The page never enabled human input** — `engineToMove` compared the
   fen's `stm` ("w"/"b") against `myColor` ("white"/"black") — always
   mismatched, so the page always thought the engine was to move (taps
   ignored, "board to move" status, spurious auto-move at start). Fixed
   to compare normalized forms; the same mismatch broke the client-side
   human-flag check (`humanActive`).
2. **Spurious auto-move on white start** — with #1, `startGame` fired
   `postMove(0)` for a white human too; the engine (playing the side to
   move) made WHITE's first move and the game stalled forever. Fixed by
   #1 plus an explicit `engineToMove = false` reset in `startGame`.
3. **Board vertically flipped** — `fenToSquares` mapped rank 8 to the
   bottom row (`s[file + (7 - r) * 8]`); fixed to `s[file + r * 8]`
   (a8 = idx 0..7 at top, a1 = 56..63 at bottom; `squaresToFen`
   iterates r = 0..7).
4. **Duplicate engine replies** — a lost `/move` response made the page
   retry the same moves, and the server re-searched and appended a
   SECOND reply (the seq guard only protected clock booking). `/move` is
   now idempotent: if the game log already holds the engine's reply for
   the exact requested move list (checked after the request mutex, so
   in-flight searches have landed), the stored reply is returned without
   re-searching. Verified: same request twice → one reply.
5. **Game-over log gap** — the `/move` game-over path returned before
   updating `g_web_moves`, dropping the human's final move from the log
   (page-refresh recovery + history). Fixed: the game-over path keeps
   the full move list.
6. **Missing last-move render on the human's turn** — the human's move
   only appeared after the engine's reply, and the pre-reply position
   ticked the human's clock during L9/L10 searches (false flags).
   Fixed earlier in Phase 2 with a display-only local move applier
   (`applyLocalMove`/`squaresToFen`: captures, en-passant, castling,
   promotion, ep square, half/fullmove) plus a `refreshState`
   thinking-guard. 12/12 node unit tests.
7. **The engine's reply ending the game left a dead board** — the
   post-reply terminal check (`/state` snapshot after the reply) flags
   mate/stalemate on the engine's move and reports `game_over`
   (verified via fool's mate `f2f3 e7e5 g2g4` → `Qh4#` →
   `black_wins`, result overlay "Board wins · 3 plies", history intact).

**Verified live (headless Chromium driving the page):** white start —
no auto-move, human taps a piece, move submits, engine replies, board
returns to the human's turn (two full cycles); black start — the engine
auto-moves its first move, then the same cycle; clocks tick and deduct
correctly per side; promotion dialog shows the right choices and submits
`a7a8q`-style moves; board orientation correct for both colors; fool's
mate end-to-end with the result overlay; `/move` idempotency (duplicate
request → single reply); `/new` resets game_over/moves/clocks; level
budgets under the 150 s worker timeout (L10 = 120 s); 3-game API
playtest zero desync.

### Phase 3 — Hardening & battery (acceptance: 2h untethered session)
- Power: LiPo/power bank test; ADC battery readout calibrated; watch
  brownout/WDT behavior under sustained search + WiFi TX.
- Stability: 15-min continuous web game (the 2-thread stability rule);
  watchdog feeding while the httpd+search run concurrently.
- Edge cases: mid-game refresh (state survives via `/state`), phone
  disconnect, promotion of repeated positions, book on/off.

**Software items done (2026-08-20):**
- **15-min soak PASSED** — scripted web game at L4 (2 s/move) for 15.0 min:
  29 games, 783 plies, **0 errors / 0 desyncs / 0 watchdog resets**, heap
  stable 51-54 KB, `/state` latency 8-298 ms (avg 80 ms). The 2-thread
  stability rule (httpd + search concurrent) is green.
- **Long-search + httpd coexistence** — a 120 s L10 move (reply at depth 27)
  with a concurrent `/state` request: the single-task httpd queues the
  request behind the search (1m57s, the documented limit) and serves it
  after; no reset, heap stable. The 150 s worker timeout governs the 120 s
  budget.
- **Mid-game refresh/join** — a fresh page load mid-game joins the running
  game (moves, board, clocks, history) without disturbing the mover; it
  follows game-over → new-game transitions as a spectator.
- **Spectator stale game-over FIXED (found during Phase 3)** — a page that
  witnessed a game-over kept `gameOver=true` forever even after the mover
  started a new game (taps disabled, "game over" status). `refreshState`
  now clears a stale game-over when the server reports a fresh game, with a
  `clientFlagged` guard so a client-side human flag is never wiped mid-race.
  Verified: fool's mate → result overlay → mover `/new` → page clears and
  follows the new game.
- **Battery endpoint + chip** — `/battery` reads ADC1_CH2 (GPIO2, 2:1
  divider, 12 dB atten): `adc_raw`/`v_mv_est` (`raw*2600/4095`) with a
  hardcoded `calibrated:false`. On USB (no battery) the page correctly
  shows "USB"; the 3300-4200 mV → 0-100% mapping and the scale constant
  still need a real battery + multimeter (user item below).

**Remaining (physical / device):** ~~battery calibration~~ **N/A by user
decision (2026-08-21)** — the board is powered exclusively from a USB
power bank (no LiPo on the BAT pins, no meter), so `/battery` always
reads USB power and the page's "USB" chip is the correct permanent
display; the 2 h untethered session was already passed on that same power
bank. Remaining: a real-phone playtest (join the AP + touch UX on an
actual Android/iOS phone); optional: serial console mid-game (USB-JTAG
input wedge is a dev-env quirk, serial output is readable).

**Phase 3 deep verification (2026-08-20, post-soak):**
- **Task WDT made real (sdkconfig)** — `CONFIG_ESP_TASK_WDT_PANIC` was off:
  the watchdog could only log (invisibly, on the wedged serial), never
  reset. A hung 2-thread search (the QIO killer) would have bricked the
  board until a manual reboot. Enabled PANIC (both legacy + canonical
  symbols); the bootloader WDT (9 s) was already on. Verified: boots clean
  across flashes, no false trips in play.
- **Yield-gate coordination bug FIXED (`search.cpp`)** — the 1.5 s WDT
  gate only let ONE searcher yield per window (the peer's time check failed
  because the first arrival stamped `es32_last_yield`), so one core's IDLE
  starved for the entire search and a PANIC-enabled WDT reset the board
  ~60 s into any long search (reproduced: L10 120 s move → reboot). Fixed:
  the first arrival stamps the window and blocks until the peer follows
  (peer's pre-check sees the open gate == 1), so both searchers delay
  together and both IDLE tasks feed. Verified: L10 120 s search (d7d5,
  depth 27) with no reset, uptime continuous; full game clean; the gate is
  the only thing between a hung search and a self-recovering reboot.
- **Opening book dead since forever FIXED (`book.cpp`)** — `query()` early-
  returned on `!fh`, but `begin()` loads the book into memory and then
  closes the file (`fh = nullptr`), so the book never hit when the malloc
  succeeded (always, on this firmware). `!fh` → `!fh && !buf`. Verified:
  startpos white reply now ~25 ms (book e2e4) vs ~1965 ms (full L4 search)
  before; book replies have no PV so the auto-ponder still covers the next
  position.
- **3-fold repetition verified through the web path** — a third
  occurrence of a position (human-created or engine-created) returns
  `game_over: draw` via the pre-search and post-reply terminal checks
  (`game_state()` in libchess detects `THREEFOLD_REPETITION`). Scripted
  knight-shuffle lines (9 and 12 plies) → `{"game_over":true,"result":"draw"}`.
- **Two simultaneous clients** — player + spectator polling `/state` in
  parallel: 369/372 polls, 0 timeouts, no interference; a `/move` during
  the concurrent polls lands cleanly (single-task httpd serializes).
- **Legal-move replay verified** — an illegal/wrong-side move in a
  submitted list is rejected (`str_to_move` fails, replay breaks at it) and
  the engine answers the last valid position; the game log keeps the raw
  request (page never sends such lists).
- **Long-search vs httpd** — `/state`/`/battery` queue behind a long
  `/move` (8 s client polls time out during a 120 s move; the page's 15 s
  poll and 160 s move timeouts absorb it by design).

**Phases 1-3 full audit (2026-08-21):**
- **Clocks made server-authoritative (HIGH fix)** — time was only deducted
  when a `/move` landed: a page refresh restored drained time, and a player
  whose clock ran out without moving was never flagged (the game hung
  server-side while the page showed "Out of time"). New continuous model:
  turn-anchored real-time deduction in `/move` + flag detection in
  `/state`; `base_ms` dev param on `/new` (5 s..10 m) for testing.
  Verified: ~4 s drained over ~4 s wall, flag fall without moving →
  result at clock 0, engine think deducted, idempotent retry books nothing.
- **`/state` torn reads fixed (LOW)** — fen/legal/last were read via three
  separate lock acquisitions; `web_engine_snapshot()` now serves all three
  under one.
- **Page clocks froze during the engine's think (LOW)** — no render while
  `boardThinking`; a 500 ms display ticker added.
- Audited sound: cJSON discipline (no leaks), `read_body` bounds, httpd
  single-task serialization (`/new` vs `/move` can't interleave), seq
  semantics (`/new` resets the booking guard), input clamps everywhere,
  no XSS (textContent only), spectator-join races covered by idempotency,
  promotion flow server-driven.

### Phase 4 — Playtest with friends + release
- 2-3 friends, real devices (Android + iOS), no setup beyond joining the
  AP and opening the IP.
- Polish: SSID/password, page tweaks, maybe a "battery to go" hint.

**Implemented ahead of playtest (2026-08-21):**
- **Captive portal** — a minimal DNS responder answers every query with the
  board's IP, and an httpd catch-all (`/*`, wildcard matcher, registered
  last) 302s foreign-host probes (captive.apple.com,
  connectivitycheck.gstatic.com, msftconnecttest.com) to `http://192.168.4.1/`.
  Joining phones pop their OS "sign in to network" sheet straight onto the
  game page. Verified: all three probe domains resolve + redirect; our own
  endpoints unaffected.
- **Multi-player: one seat + waitlist** — two simultaneous games are not
  feasible (one engine instance; every think uses both cores and the shared
  TT). Instead each browser gets a random pid (localStorage); `/state`
  serves `owner`/`queue`/`idle_ms`; only the seat holder may `/move` or
  `/new` (409 otherwise; missing pid = legacy/dev bypass). Non-holders see
  "<name> is playing", can join the waitlist (`/queue`, dedupe, positions),
  leave it (`/unqueue`), and take the seat when it's free or the holder is
  idle >3 min (abandoned seats self-heal; the continuous clock flags a
  walked-off player within the base time anyway). The holder sees Rematch /
  Give up the board on the result overlay. Names are sanitized server-side
  before JSON. Verified end-to-end via API: take/reject/busy/yield/promote/
  unqueue + full game with pids.
- **Autoconnect** — an AP cannot forbid client auto-rejoin (client-side
  behavior); the start overlay carries a "forget this network" tip, and the
  open network + failed-portal marking keeps phones from preferring it.
- Flash the final image, document the usage one-liner in README.

## Risks & mitigations

- **Internal RAM pressure (WiFi + httpd vs ~31.7 KB largest free block)**:
  WiFi RX/TX buffers and httpd scratch -> PSRAM via Kconfig; keep httpd
  threads' stack small (configurable, e.g. 4-6 KB); measure in Phase 0.
  Fallback: drop the second searcher thread in web mode (loses ~+55% SMP —
  acceptable only if RAM forces it; prefer RAM config first).
- **Search loop is serial-UGI-centric**: solve by routing through one
  command pipe (single consumer), not a parallel search entry point, so the
  web path and serial path can never diverge (and the ESP32 time fix stays
  in effect).
- **Concurrent access**: serial console commands arriving mid-web-game must
  not corrupt state — serialize through the same mutex pattern already in
  use; document "console is for dev" in web mode.
- **TT/PSRAM budget**: web page is tiny (~30-60 KB) vs 8 MB PSRAM; the book
  + web assets share the existing 1 MB SPIFFS partition (book ~126 KB +
  assets ~60 KB — fits).
- **Range/interference**: 2.4 GHz SoftAP, ~5-10 m indoors — fine for a
  table demo; a USB power bank keeps the board portable.
- **Clock/level mapping**: web levels map to `movetime`/clock parameters —
  must go through the same go-handler so the time-budget fix governs the
  board's spending at every level.

## Decisions to record (at each phase gate)

- Single game at a time vs spectator mode (`GET /state` polling makes
  spectators nearly free — decide in Phase 2).
- AP security: open vs simple WPA2.
- Difficulty mapping: exact think-time table.
- Legal-move source on the phone: server-provided (`/state`) vs client-side
  move gen.

## Definition of done

- A phone can join the board's WiFi and play a full game with no app, no
  internet, no computer.
- Same-engine guarantee: web-mode search results match serial-mode on the
  same positions (the shared command pipe is the proof).
- Board stability gates pass (3-game board-vs-native, 15-min session),
  unit gate 18 OK, bench within 2% of baseline.
- Everything committed and pushed per AGENTS.md; usage documented.
