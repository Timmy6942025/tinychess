# TinyChess

MaxDogOne. A UCI chess engine that runs on a XIAO ESP32-S3 Plus and on your desktop. It is a fork of [Dog](https://github.com/folkertvanheusden/Dog) by Folkert van Heusden, MIT licensed. The board broadcasts its own WiFi network. You join it from a phone and play in the browser. No app, no internet, no cables.

This repo is `Timmy6942025/tinychess` on GitHub, branch `main`. The engine name inside the code is still Dog.

## What you get

TinyChess is two builds from one source tree. `app/src` compiles as a native x86_64 UCI engine and as ESP32 firmware.

* The native build is a strong UCI engine and the test harness for every change.
* The ESP32 build runs the same search, eval, and NNUE on the board, plus a hotspot and web UI so you can play from a phone.

Every change passes the same gates before it stays. Strength changes need a 200-game match at 2+0.02. Board changes need a 3-game board-vs-native run with no stalls, forfeits, or illegal moves.

## Features

Board and native share the same engine. The differences are only where the hardware forces them.

* NNUE big net, 768x256x1. The active net is 394,816 bytes in `app/src/weights.cpp`. The small net stays behind `#if 0` for reference. The big net tested +25 Elo vs the 128-wide net on the desktop. Around it sits a paired-fused kernel layer: weight rows permuted at load into `[own|other]` pairs so one piece event touches one contiguous region, all of a move's accumulator deltas applied to both perspectives in a single sweep, and explicit SIMD everywhere (NEON on ARM, SSE2 on x86, hand-written PIE assembly on the S3 using a zero-widen/add/narrow trick for exact wraparound int16 math). Desktop stays bit-exact with the old evaluator; the board gains about 1.8x node throughput. Design notes in `docs/paired-fused-nnue.md`.
* Search that survived measurement. The stacked gated total is about +230 Elo vs Tier-1: recalibrated LMR (+34.6), TT aging (+36.7), mate distance pruning (+19.8), razoring (+12.2), check extension (about +75), qsearch SEE pruning (+29.6), recapture extension (+57.8), qsearch TT probe removal (+34.9), and a move-ordering rebuild (+24.6 accepted, then +16.5 +/- 16.3 on an 800-game replication) that added capture history, butterfly from-to history, and a one-ply continuation table next to the old piece-to history. Design notes in `docs/move-ordering-rebuild.md`. The rejects are kept with numbers too: killers, hist3d, late-move pruning, correction history, delta pruning, eval cache, a shelf of margin probes, and a phase-2 sweep that gated a staged move picker, easy-move/stability time management, proper singular extensions, and a 32-bit-signature transposition table against the current state and reverted all four (the search sits at a measured local optimum). Everything lives in `tools/results.log`.
* Speed that we actually measured. Native with LTO and the big net does roughly 350k to 590k nps on the dev box by the bench protocol depending on machine load (instantaneous UCI rates run higher). The ordering-table reads cost about 6% of that; the strength they buy is measured at more than that in Elo. An Aug 26 re-profile (tools/results.log) re-confirmed the tax at 5.7% desktop and 3.3% board (alternating-flash 18,916 -> 18,283) and tried to reclaim it bit-exact: victim reuse and bounds cleanup saved 0.40% of instructions and ~0% on the board, and the naive 2 MB merged butterfly/continuation layout does not fit the 33 KB PSRAM budget, so no layout ship. The board does about 18,300 to 18,800 nps on the startpos bench, up from ~8,300 before the evaluator kernel rebuild (worth a measured +174 +/- 34 Elo on-device) and ~17,700 before the output layer moved into on-board SRAM (+6% node throughput, bit-exact, see docs/output-layer-staging.md). Earlier history: the gain from 5,079 to 6,857 came mostly from a clean rebuild and two small fixes. C2 moved the WDT gate to 1.5 s and C5 fixed per-node allocations. C4 tried QIO flash and reverted. It hung under 2-thread load and the speed claim was a measurement error.
* Web play from a phone. The board is an open access point called `DOG-CHESS` at `http://192.168.4.1`. You tap to move, clocks run, and the page works offline. It handles promotion, captured material, last-move and check highlights, move animation, sounds and haptics, flip board, resign, and a 10 minute clock with an increment per difficulty level. Level 1 is 5 s per move, level 10 is 120 s. One game at a time. Others see the position live and can join a waitlist. The holder can rematch or give up the board. Idle seats free after 3 minutes.
* Captive portal. Phones that probe `captive.apple.com` or `connectivitycheck.gstatic.com` get a 302 to `192.168.4.1`, so the OS shows a sign-in sheet that lands on the game.
* Mobile UI that fits. The board always fills the largest square that fits the screen, with safe-area insets for notches. Landscape phones get a two-pane layout with the board on the left. Inputs use 16 px to avoid iOS zoom, tap targets are 44 px, dragging shows a floating piece, and `touch-action: none` plus a `touchstart` guard stops double-tap zoom from stealing a move.
* Vendored `app/include/libchess` with local fixes for `pseudo_legal_move_list_into`, FEN en-passant validation, and `go st` UCI support. The canonical copy is at `github.com/Timmy6942025/libchess`.
* The adapter that makes the board a UCI engine. `tools/wrapper.py` speaks UCI over USB-JTAG, repairs the line protocol, and plugs into `cutechess-cli`. The board console needs `uci` before UCI commands.

## Hardware

* Seeed Studio XIAO ESP32-S3 Plus. Dual LX7 at 240 MHz, 512 KB SRAM, 8 MB octal PSRAM, 16 MB flash.
* Flash mode is DIO at 80 MHz in `app/sdkconfig.defaults`. QIO was tried and reverted. Do not enable it. It hangs under sustained 2-thread load.
* LED on GPIO44, WS2812, optional via Kconfig.
* Power from USB. The `/battery` endpoint still reports raw ADC, but the UI shows USB when on a power bank. No LiPo calibration is needed for the current setup.

## Quick start

You need Python 3 and a data cable. That is it if you use the prebuilt files.

### Option A. Prebuilt binaries, no toolchain

Get the release `v0.1-prebuilt` at commit `75e6d10` from https://github.com/Timmy6942025/tinychess/releases.

Desktop engine, Linux x86_64:

```sh
chmod +x Dog-native
./Dog-native          # UCI engine
printf "test\n" | ./Dog-native   # 18 unit tests, all must say OK
```

Board flash, Python 3 and esptool only:

```sh
unzip board-flash.zip -d board-flash && cd board-flash
python -m esptool --port /dev/ttyACM0 write_flash "@flash_args"
```

To run matches against the board with cutechess:

```sh
cutechess-cli -engine name=board proto=uci cmd="python3 tools/wrapper.py /dev/ttyACM0" \
  -engine name=native proto=uci cmd=./app/src/linux-windows/build/Dog-native
```

### Option B. Build from source

Native, gcc or g++ 14 or newer:

```sh
tools/native_check.sh   # builds Dog-native, runs the 18 tests, runs a bench
```

Or step by step:

```sh
cd app/src/linux-windows
mkdir -p build && cd build
cmake .. && cmake --build . --target Dog-native -j$(nproc)
./Dog-native   # falls back to Dog-avx512, Dog-avx2, Dog if needed
```

Board, ESP-IDF 5.3:

```sh
./tools/setup_esp_idf.sh   # installs ESP-IDF v5.3 once
source ~/esp/esp-idf/export.sh
cd app
idf.py build
python -m esptool --port /dev/ttyACM0 write_flash "@flash_args"   # from app/build
```

The firmware and the web files flash together. `spiffs_create_partition_image` packs `app/data` including `data/web/index.html`, `portal.html`, and the piece PNGs.

## Play from your phone

This is the whole point. The board is the hotspot.

1. Power the board from USB. Wait for the LED.
2. On your phone, join the WiFi network `DOG-CHESS`. It is open, no password.
3. The phone should pop a sign-in sheet. If it does not, open `http://192.168.4.1` in the browser. `portal.html` explains the same steps.
4. Pick white or black, set difficulty, tap Start game.
5. Tap a piece, then tap a target. Drag also works. The piece you hold follows your finger. Promotions pop a chooser.
6. Use Flip board to view from either side, Resign to end early. After mate, stalemate, draw, or flag fall the result card offers Rematch, Give up the board, or View board.
7. If someone else is playing, you see their name and your place in line. Tap Join waitlist. You take the seat when they finish or go idle.

Tip. Phones remember the network. Use Forget this network when you are done so it does not auto-join later. The network has no internet, that warning is normal.

Multiplayer is one seat only. The engine and TT are shared, so two games would fight over both cores and the same position. The waitlist keeps it orderly.

## Web UI

The page is one file, `app/data/web/index.html`, about 40 KB including the PNG pieces under `pieces/ejgfv`. No CDN, no build step.

* Board is a CSS grid, 8 by 8, with a container-query square that never scrolls off screen in portrait. Landscape hands the left side to the board and the right side to clocks and controls.
* State comes from the board. `GET /state` returns FEN, legal moves, clocks, move list, and game-over flag. `POST /move` runs the search. `GET /battery` returns millivolts. Your browser stores a random pid in localStorage so the board can tell who holds the seat.
* Clocks are server authoritative. The server deducts your think on each move plus its own, adds the level increment, and flags at zero. The page just renders the countdown.
* Assets are served from SPIFFS. `web.cpp` sends `Cache-Control: no-cache` so a reflash is visible after one hard refresh.

Pieces are the ejgfv set. The board colors are `ece6d8` and `b3a98e`, with a dark theme around them.

## Repository layout

```
app/src/                engine source, single tree for desktop and ESP32
app/include/libchess    vendored libchess, local patches kept here
app/main                symlink to app/src, the IDF project entry
app/data/web/           index.html, portal.html, pieces
app/data/               opening book, SPIFFS image root
tools/                  wrapper.py, board_session.py, fast_sprt.sh, results.log
docs/                   upstream docs, training notes, experiment queue
```

`app/src/weights.cpp` and `quantised-big.bin` are the net. `tools/results.log` is the source of truth for every accepted or rejected experiment.

## Build, test, and gates

One source, two targets, same rules.

Native gate:

```sh
cd app/src/linux-windows/build && cmake --build . --target Dog-native -j$(nproc)
printf "test\n" | ./Dog-native   # 18 tests, all OK, no assert fail
```

A timeout with zero failures and tests still printing OK counts as a pass. The suite takes more than 10 minutes.

Strength gate, only for search or eval changes that could affect play:

```sh
cutechess-cli -engine name=A proto=uci cmd=<new> -engine name=B proto=uci cmd=<old> \
  option.Threads=1 option.Hash=8 \
  -draw movenumber=40 movecount=1 score=100 -resign movecount=3 score=800 -maxmoves 200 \
  -games 200 -rounds 1 -each tc=2+0.02
```

Keep it if Elo is positive and LOS is high, otherwise revert. Write the numbers to `tools/results.log`.

Board gate:

```sh
export IDF_PATH=~/esp/esp-idf && source ~/esp/esp-idf/export.sh && idf.py build
python -m esptool --port /dev/ttyACM0 write_flash "@flash_args"   # from app/build
```

Then 3 games board vs native, no stalls, forfeits, or illegal moves, plus a bench. A 15-minute 2-thread session must survive. QIO failed exactly here.

All of this, plus the current bench numbers and the rejected list, are captured at the top of `AGENTS.md`.

## Notes

* Use `app/src/linux-windows/Dog-native` directly. The fallback names `Dog-avx512`, `Dog-avx2`, and `Dog` exist only if your CPU lacks the top feature level.
* The dev Pi in `REQUIREMENTS.md` is 7.6 GB RAM and was 89 percent full at last check. Do not run a match while building, the OOM killer will take cutechess.
* Clone with `--recursive` to get `app/Dog-book` and `app/src/fathom`. The fathom pin must stay at `2251e9974d5e1c77f09e35015fc325098e586e2c`.
* `tools/board_session.py` is the board smoke. `tools/wrapper.py` needs a fresh open or DTR toggle after a host disconnect.
* The reference native binary at the time of the move-ordering rebuild has md5 `fd8cef65eda53294c03a705bd40e32ff`. New builds will differ, hashes will change.
* NNUE weights are source. `#if 0` in `weights.cpp` picks the small net, `#else` picks the big net that is active now.

## License and upstream

MIT, same as Dog. Upstream is https://github.com/folkertvanheusden/Dog.

If you use this fork, keep the MIT notice and link back to Folkert's project. Issues or ideas for this fork go to https://github.com/Timmy6942025/tinychess.
