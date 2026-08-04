#!/bin/sh
# fast_sprt.sh - deterministic SPRT / A-B match harness for the MaxDogOne engine.
#
# Every search/eval change is measured through this script before it is kept.
# All results are appended to tools/results.log with a unique run tag so any
# change is attributable and revertible.
#
# Usage:
#   tools/fast_sprt.sh baseline        # SPRT: current Dog-native vs stockfish (ELO anchor)
#   tools/fast_sprt.sh ab <tag> [A] [B]  # A-B between two Dog builds (default A=baseline dog, B=diff)
#
# The default elo0/elo1 + tc are chosen to be reproducible on a 4-core arm64
# host; override via env: TC=, ELO0=, ELO1=, SPRT=yes|no, GAMES=.

set -e

ENGINE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
NATIVE_DIR="$ENGINE_DIR/app/src/linux-windows"
BUILD_DIR="$NATIVE_DIR/build"
CUTECHESS="${CUTECHESS:-/home/timmy/bin/cutechess-cli}"
STOCKFISH="${STOCKFISH:-/usr/local/bin/stockfish}"
RESULTS_LOG="$ENGINE_DIR/tools/results.log"
RUN_DIR="$ENGINE_DIR/tools/runs"
mkdir -p "$RUN_DIR"

TC="${TC:-40/15+0.5}"
ELO0="${ELO0:-0}"
ELO1="${ELO1:-10}"
SPRT_CFG="${SPRT_CFG:--sprt elo0=$ELO0 elo1=$ELO1 alpha=0.05 beta=0.05}"
GAMES="${GAMES:-200}"
THREADS="${THREADS:-1}"
HASH="${HASH:-64}"

DOG="$BUILD_DIR/Dog-native"

ensure_build() {
	if [ ! -x "$DOG" ]; then
		echo "Building Dog-native (baseline) ..."
		cmake -B "$BUILD_DIR" -S "$NATIVE_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
		cmake --build "$BUILD_DIR" --target Dog-native -j"$(nproc)" >/dev/null
	fi
}

stamp() { date +%Y%m%d-%H%M%S; }
log()  { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" >> "$RESULTS_LOG"; }

run_match() {
	# $1=tag $2=protocfg ("sprt" = ad-hoc sprT between near-equal builds, else fixed games)
	# $3=engineA $4=engineB
	local tag="$1" protocfg="$2" a="$3" b="$4"
	local out="$RUN_DIR/$tag.txt"
	if [ "$protocfg" = "sprt" ]; then
		POPT="-sprt elo0=$ELO0 elo1=$ELO1 alpha=0.05 beta=0.05"
		echo "== SPRT A-B: $tag  (tc=$TC, sprt=[$ELO0,$ELO1]) =="
	else
		POPT=""
		echo "== Fixed match: $tag  (tc=$TC, games=$GAMES) =="
	fi
	echo "   $a  vs  $b"
	"$CUTECHESS" -engine name=A proto=uci cmd="$a" option.Threads=$THREADS option.Hash=$HASH \
	             -engine name=B proto=uci cmd="$b" option.Threads=$THREADS option.Hash=$HASH \
	             -each tc="$TC" \
	             -openings file="$ENGINE_DIR/tools/openings.epd" order=sequential start=1 \
	             -rounds "$GAMES" \
	             -pgnout "$RUN_DIR/$tag.pgn" \
	             $POPT 2>&1 | tee "$out"
	echo "== done: $tag (log: $out) =="
	log "MATCH $tag  A=$a  B=$b  tc=$TC games=$GAMES $POPT"
	grep -E "Elo difference|Score of|SPRT:" "$out" >> "$RESULTS_LOG" || true
}

case "$1" in
	baseline)
		# ELO anchor vs the far-stronger stockfish: fixed games, no SPRT.
		ensure_build
		run_match "baseline-vs-stockfish-$(stamp)" fixed "$DOG" "$STOCKFISH"
		;;
	ab)
		tag="$2"; a="${3:-$DOG}"; b="${4:-$DOG}"
		ensure_build
		run_match "ab-$tag-$(stamp)" sprt "$a" "$b"
		;;
	*)
		echo "usage: $0 baseline | ab <tag> [engineA] [engineB]"
		echo "  baseline = fixed-games ELO anchor vs stockfish"
		echo "  ab       = SPRT between two Dog builds (A=baseline dog, B=diff build in place)"
		exit 1
		;;
esac

