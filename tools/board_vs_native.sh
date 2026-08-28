#!/bin/bash
# board_vs_native.sh - strength probe: XIAO ESP32S3 board (via serial wrapper)
# vs its desktop Dog-native twin at IDENTICAL TC.
#
# Result converts through the SF17 anchor (results.log 03:21:00: Dog-native
# = -56.1 +/- 43.4 vs Stockfish 17 at 2+0.02, 200 games):
#   board_abs_elo  =  -56.1 - (board deficit vs native)
#
# Usage: tools/board_vs_native.sh [port] [games] [tc]
#   env: TC=2+0.02 GAMES=40 ADJ=yes  (same conventions as fast_sprt.sh)

set -euo pipefail

ENGINE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CUTECHESS="${CUTECHESS:-/home/timmy/bin/cutechess-cli}"
RESULTS_LOG="$ENGINE_DIR/tools/results.log"
RUN_DIR="$ENGINE_DIR/tools/runs"
PORT="${1:-/dev/ttyACM0}"
GAMES="${2:-${GAMES:-40}}"
TC="${3:-${TC:-2+0.02}}"
ADJ="${ADJ:-yes}"
WRAPPER="$ENGINE_DIR/tools/wrapper.py"
DOG="$ENGINE_DIR/app/src/linux-windows/build/Dog-native"

stamp() { date +%Y%m%d-%H%M%S; }
log()   { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" >> "$RESULTS_LOG"; }

ADJOPT=""
if [ "$ADJ" = "yes" ]; then
	ADJOPT="-draw movenumber=40 movecount=1 score=100 -resign movecount=3 score=800 -maxmoves 200"
fi

tag="board-vs-native-$(stamp)"
out="$RUN_DIR/$tag.txt"

log "FINGERPRINT A=board(wrapper,port=$PORT)  B=$DOG $(md5sum "$DOG" | cut -d' ' -f1)"
echo "== Board($PORT) vs Dog-native: $tag  (tc=$TC, games=$GAMES) =="

"$CUTECHESS"              -engine name=BoardXS3 proto=uci cmd="$WRAPPER" arg="$PORT" \
             -engine name=DogNative proto=uci cmd="$DOG" option.Threads=1 option.Hash=8 \
             -each tc="$TC" \
             -openings file="$ENGINE_DIR/tools/openings.epd" order=sequential start=1 \
             -rounds "$GAMES" -concurrency 1 \
             -pgnout "$RUN_DIR/$tag.pgn" \
             $ADJOPT 2>&1 | tee "$out"

echo "== done: $tag (log: $out) =="
log "MATCH $tag  A=board(port=$PORT, wrapper)  B=$DOG  tc=$TC games=$GAMES adj=$ADJ"
grep -E "Elo difference|Score of" "$out" >> "$RESULTS_LOG" || true
