#!/bin/sh
# One-shot native verification: clean configure+build, run the full unit
# test suite, then a quick bench. Exit 0 = all tests pass.
#
# Usage:  tools/native_check.sh
#
# Note: the SIMD kernels are ESP32-only (see nnue.cpp); this verifies the
# scalar engine. The raw SIMD kernel test runs on the board via
# tools/board_check.sh.

set -e

ENGINE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
NATIVE_DIR="$ENGINE_DIR/app/src/linux-windows"
BUILD_DIR="$NATIVE_DIR/build"

echo "== configuring =="
cmake -B "$BUILD_DIR" -S "$NATIVE_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null

echo "== building =="
cmake --build "$BUILD_DIR" --target Dog-native -j"$(nproc)"

echo "== unit tests =="
printf 'test\nquit\n' | timeout 900 "$BUILD_DIR/Dog-native"
echo "(test suite done)"

echo "== bench =="
printf 'bench\nquit\n' | timeout 600 "$BUILD_DIR/Dog-native" | grep -E "Nodes/second"
