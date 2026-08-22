# Agent instructions

> Always use the `unslop` skill when writing or editing any text, docs, comments, or user-facing messages. Scan for the patterns listed in the skill, rewrite to remove them, add soul, and self-audit before committing.

Repository: `Timmy6942025/tinychess` (branch `main`), origin on GitHub.
This file tells coding agents how to work in this repo.

## Rules

- **Commit and push**: commit every change and `git push origin main`
  immediately after it passes its gates. Do not leave work uncommitted.
- Before committing, check `git submodule status`: if `app/src/fathom` drifts
  off `2251e9974d5e1c77f09e35015fc325098e586e2c`, restore it with
  `git checkout 2251e9974d5e1c77f09e35015fc325098e586e2c` (inside
  `app/src/fathom`) before `git add`.
- Leave untracked: `app/src/linux-windows/Dog-native`, `Dog-ruk`,
  `Dog-stats-prober` (prebuilt binaries, rebuilt by cmake; the canonical
  release lives on GitHub Releases `v0.1-prebuilt`).
- Never commit the generated `app/build/` directory (gitignored).

## Repo facts

- One source tree: `app/main` is a **symlink to `app/src`** — edits apply once
  to both the desktop and the ESP32 build.
- `app/include/libchess` is vendored (git-tracked) with local patches
  (`pseudo_legal_move_list_into`, FEN en-passant validation, `go st` UCI).
- NNUE weights are committed as source: `app/src/weights.cpp` (+
  `quantised-big.bin`). `#if 0` selects the small net, `#else` the active big
  net.
- Flash mode is **DIO only** (`app/sdkconfig.defaults`). QIO was tried and
  reverted: unstable under sustained 2-thread load. Do not re-enable QIO.
- Search parameters are at a measured local optimum: probed-and-rejected list
  is in `tools/results.log` (killers, LMR PV *3/4, aspiration != 75, TT 4-way,
  blind singular extension, null-move R=5, razor depth<=2, razor 300+120d).
- Board console requires `uci` before UCI commands; the cutechess adapter is
  `tools/wrapper.py` (needs a serial DTR toggle / fresh port open).

## Workflow (always gate changes)

1. **Desktop build**: `cd app/src/linux-windows/build && cmake --build . --target
   Dog-native -j$(nproc)` (default cmake config = RelWithDebInfo + LTO).
2. **Unit gate**: `printf "test\n" | ./Dog-native` — all 18 tests must pass
   (`OK`, no `assert fail`). The suite takes >10 min; exit 124 (timeout) with
   0 failures and tests still printing `OK` counts as passing.
3. **Strength gate** (search items): 200-game match at 2+0.02 vs the previous
   accepted state:
   `/home/timmy/bin/cutechess-cli -engine name=A proto=uci cmd=<binary> -engine
   name=B proto=uci cmd=<baseline> option.Threads=1 option.Hash=8 -draw
   movenumber=40 movecount=1 score=100 -resign movecount=3 score=800 -maxmoves
   200 -games 200 -rounds 1 -each tc=2+0.02`
   Keep on positive Elo + LOS, revert otherwise. Record the verdict in
   `tools/results.log` with numbers.
4. **Board port**: rebuild with the IDF (`export IDF_PATH=~/esp/esp-idf &&
   source ~/esp/esp-idf/export.sh && idf.py build`), flash
   (`python -m esptool --port /dev/ttyACM0 write_flash "@flash_args"` from
   `app/build`), then a 3-game board-vs-native gate (no stalls/forfeits/illegal
   moves) and a bench.
5. Push after each committed gate result.

## State (what the numbers should look like)

- Board bench: ~7,100 nps (startpos bench), 6,857 nps at Tier-1 state.
- Desktop native bench: ~340k nps (LTO build, big net, ~150-290k under load).
- Accepted search items (Phase B): razoring, check extension, qs SEE pruning
  (+116.5 Elo cumulative vs Tier-1). Reference binary md5
  `1773e3c807d0752a40da2ae78ea82924` (commit `75e6d10` era).
- 2-thread board stability: board must survive a 15-min 2-thread session
  (a 2-thread hang is what killed QIO).