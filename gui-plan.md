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
  ~0.3s .. 120s/move), battery indicator.
- Pondering enabled during human think time.
- Playtest: 3 full games on a phone with no crashes, no state desync.

### Phase 3 — Hardening & battery (acceptance: 2h untethered session)
- Power: LiPo/power bank test; ADC battery readout calibrated; watch
  brownout/WDT behavior under sustained search + WiFi TX.
- Stability: 15-min continuous web game (the 2-thread stability rule);
  watchdog feeding while the httpd+search run concurrently.
- Edge cases: mid-game refresh (state survives via `/state`), phone
  disconnect, promotion of repeated positions, book on/off.

### Phase 4 — Playtest with friends + release
- 2-3 friends, real devices (Android + iOS), no setup beyond joining the
  AP and opening the IP.
- Polish: SSID/password, page tweaks, maybe a "battery to go" hint.
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
