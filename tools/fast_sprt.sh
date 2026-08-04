#!/bin/sh
# fast_sprt.sh - deterministic SPRT / A-B match harness for the MaxDogOne engine.
#
# Every search/eval change is measured through this script before it is kept.
# All results are appended to tools/results.log with a unique run tag so any
# change is attributable and revertible.
#
# Usage:
#   tools/fast_sprt.sh baseline            # fixed-games ELO anchor vs stockfish
#   tools/fast_sprt.sh ab <tag> [A] [B]    # SPRT between two Dog builds (default A=baseline dog, B=diff)
#
# SPRT verdicts (printed + logged, also as exit code):
#   ACCEPT  llr hit the upper bound -> H1: A is stronger (keep the change)   exit 0
#   REJECT  llr hit the lower bound -> H0: no improvement (revert)           exit 1
#   KEEP/REVERT  game cap reached before a bound -> decided by llr sign      exit 0/1
#   ERROR   no usable SPRT output                                            exit 2
#
# Power notes (why these defaults):
#   cutechess-cli's SPRT (fishtest/OpenBench-style) estimates the draw rate
#   in-sample, so its llr drifts negative for a null match - that is by
#   design: a null test is SUPPOSED to accept H0 (elo0). Verified against
#   measured runs 2026-08-05 (all reproduced to 2 decimals):
#     elo1=50:  null  rejects at ~160 games  (hist3d: exactly 160, was neutral)
#               -100 elo regression rejects at ~20-30 games (checkext 29,
#               evalcache 19)                 - REAL but small (+10..+30 elo)
#               changes get rejected too: +30 needs ~7400 games with drift ~0
#               -> elo1=50 only ever ACCEPTs +50 elo effects. Too harsh.
#     elo1=20:  +20 elo accepts ~1600 games, +30 ~700, null rejects ~1000
#     elo1=10:  +10 elo accepts ~1800+ games (hours at CONC=2) - too slow.
#   elo1=20 is the sweet spot for detecting realistic +15..+35 elo search
#   changes; runs take ~2-7h at CONC=2. SPRT_MAX caps the rest; borderline
#   cap verdicts are decided by llr sign.
#
# Overridable via env: TC= ELO0= ELO1= ALPHA= BETA= SPRT_MAX= (cap, rounds)
#   GAMES= (fixed mode) ADJ=yes|no (adjudication) CONC= (cutechess -concurrency)

set -e

ENGINE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
NATIVE_DIR="$ENGINE_DIR/app/src/linux-windows"
BUILD_DIR="$NATIVE_DIR/build"
CUTECHESS="${CUTECHESS:-/home/timmy/bin/cutechess-cli}"
STOCKFISH="${STOCKFISH:-/usr/local/bin/stockfish}"
RESULTS_LOG="$ENGINE_DIR/tools/results.log"
RUN_DIR="$ENGINE_DIR/tools/runs"
mkdir -p "$RUN_DIR"

TC="${TC:-5+0.05}"
ELO0="${ELO0:-0}"
ELO1="${ELO1:-20}"
ALPHA="${ALPHA:-0.05}"
BETA="${BETA:-0.05}"
SPRT_MAX="${SPRT_MAX:-1500}"
GAMES="${GAMES:-200}"
THREADS="${THREADS:-1}"
HASH="${HASH:-64}"
CONC="${CONC:-1}"
ADJ="${ADJ:-yes}"

DOG="$BUILD_DIR/Dog-native"

# llr bounds for the given alpha/beta (ln((1-a)/a))
SPRT_BOUND=$(awk -v a="$ALPHA" 'BEGIN { print log((1-a)/a) }')

ADJOPT=""
if [ "$ADJ" = "yes" ]; then
	# Only adjudicate clearly dead games: draw if both engines within 100cp
	# of zero for 1 move after move 40; resign at -800cp for 3 moves; hard
	# cap at 200 full moves. Symmetric for both engines, so the A/B signal
	# is unaffected - it just saves wall time on drawn-out endgames.
	ADJOPT="-draw movenumber=40 movecount=1 score=100 -resign movecount=3 score=800 -maxmoves 200"
fi

ensure_build() {
	if [ ! -x "$DOG" ]; then
		echo "Building Dog-native (baseline) ..."
		cmake -B "$BUILD_DIR" -S "$NATIVE_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
		cmake --build "$BUILD_DIR" --target Dog-native -j"$(nproc)" >/dev/null
	fi
}

stamp() { date +%Y%m%d-%H%M%S; }
log()  { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" >> "$RESULTS_LOG"; }

fingerprint() {
	# Log md5 of both engine binaries so a mid-match binary swap (e.g. a
	# git checkout/stash of a tracked build artifact) is detectable later.
	# Binaries must NOT change while a match is running.
	log "FINGERPRINT A=$1 $(md5sum "$1" | cut -d' ' -f1)  B=$2 $(md5sum "$2" | cut -d' ' -f1)"
}

run_match() {
	# $1=tag $2=protocfg ("sprt" = SPRT A-B between near-equal builds, else fixed games)
	# $3=engineA $4=engineB
	local tag="$1" protocfg="$2" a="$3" b="$4"
	local out="$RUN_DIR/$tag.txt"
	local rounds="$GAMES" popt=""
	if [ "$protocfg" = "sprt" ]; then
		rounds="$SPRT_MAX"
		popt="-sprt elo0=$ELO0 elo1=$ELO1 alpha=$ALPHA beta=$BETA"
		echo "== SPRT A-B: $tag  (tc=$TC, sprt=[$ELO0,$ELO1] a/b=$ALPHA/$BETA, max $SPRT_MAX games, adj=$ADJ) =="
	else
		echo "== Fixed match: $tag  (tc=$TC, games=$GAMES) =="
	fi
	fingerprint "$a" "$b"
	echo "   $a  vs  $b"
	"$CUTECHESS" -engine name=A proto=uci cmd="$a" option.Threads=$THREADS option.Hash=$HASH \
	             -engine name=B proto=uci cmd="$b" option.Threads=$THREADS option.Hash=$HASH \
	             -each tc="$TC" \
	             -openings file="$ENGINE_DIR/tools/openings.epd" order=sequential start=1 \
	             -rounds "$rounds" -concurrency "$CONC" \
	             -pgnout "$RUN_DIR/$tag.pgn" \
	             $ADJOPT $popt 2>&1 | tee "$out"
	echo "== done: $tag (log: $out) =="
	log "MATCH $tag  A=$a  B=$b  tc=$TC rounds=$rounds $popt adj=$ADJ"
	grep -E "Elo difference|Score of|SPRT:" "$out" >> "$RESULTS_LOG" || true

	if [ "$protocfg" = "sprt" ]; then
		verdict "$tag" "$out"
	fi
}

verdict() {
	# Decide the SPRT outcome and log/echo it; sets the exit code.
	# $1=tag $2=output file
	local tag="$1" out="$2"
	local line llr lbound rc verdict_str

	line=$(grep "SPRT:" "$out" | tail -1 || true)
	llr=$(printf '%s' "$line" | sed -n 's/.*llr \([0-9.eE+-]*\).*/\1/p')
	lbound=$(printf '%s' "$line" | sed -n 's/.*lbound \(-*[0-9.eE+]*\).*/\1/p')
	# normalise: bounds are symmetric around 0, so drop the sign
	lbound=$(awk -v v="$lbound" 'BEGIN { print (v<0 ? -v : v) }')

	if [ -z "$llr" ] || [ -z "$lbound" ]; then
		verdict_str="ERROR: no parseable SPRT line in $out"
		rc=2
	elif awk -v l="$llr" -v b="$lbound" 'BEGIN { exit !(l >= b) }'; then
		verdict_str="ACCEPT: H1 (A stronger than B) - llr $llr >= $lbound"
		rc=0
	elif awk -v l="$llr" -v b="$lbound" 'BEGIN { exit !(l <= -b) }'; then
		verdict_str="REJECT: H0 (no improvement) - llr $llr <= -$lbound"
		rc=1
	elif awk -v l="$llr" 'BEGIN { exit !(l > 0) }'; then
		verdict_str="KEEP (cap reached before bound, llr $llr > 0)"
		rc=0
	else
		verdict_str="REVERT (cap reached before bound, llr $llr <= 0)"
		rc=1
	fi

	echo "VERDICT [$tag]: $verdict_str"
	log "VERDICT $tag: $verdict_str"
	exit "$rc"
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
		echo "  env: TC ELO0 ELO1 ALPHA BETA SPRT_MAX GAMES ADJ CONC"
		echo "  exit: 0=keep 1=revert 2=error"
		exit 2
		;;
esac
