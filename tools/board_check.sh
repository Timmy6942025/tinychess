#!/bin/sh
# Board validation: flash the firmware to the XIAO ESP32S3 Plus and run the
# automated serial checks (tools/board_session.py).
#
# Usage:  tools/board_check.sh [PORT] [BUILD_DIR]
# Example: tools/board_check.sh /dev/ttyACM0

set -e

ENGINE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APP_DIR="$ENGINE_DIR/app"

PORT="${1:-}"
if [ -z "$PORT" ]; then
	# set -e is on: the ls failing (no ports) must not abort the script
	CANDIDATES="$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || true)"
	if [ -n "$CANDIDATES" ]; then
		N="$(printf '%s\n' "$CANDIDATES" | wc -l)"
		PORT="$(printf '%s\n' "$CANDIDATES" | head -1)"
		if [ "$N" -gt 1 ]; then
			echo "Multiple serial ports found:"
			printf '%s\n' "$CANDIDATES"
			echo "Using $PORT (pass a port to override: $0 $PORT)"
		fi
	fi
fi
if [ -z "$PORT" ]; then
	echo "No serial port found. Plug in the XIAO ESP32S3 Plus and retry,"
	echo "or pass the port explicitly, e.g.:  $0 /dev/ttyACM0"
	exit 1
fi

IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
if [ ! -f "$IDF_PATH/export.sh" ]; then
	echo "ESP-IDF not found at $IDF_PATH (set IDF_PATH or edit this script)."
	exit 1
fi

echo "Using port: $PORT"
echo "Building (must run from the app dir - building from build/ yields stale binaries) ..."

. "$IDF_PATH/export.sh" >/dev/null 2>&1
cd "$APP_DIR"
idf.py build || exit 1

echo "Flashing firmware + book partition via esptool (idf.py flash hangs on this setup) ..."
cd "$APP_DIR/build"
python -m esptool --chip esp32s3 -p "$PORT" -b 460800 \
	--before default_reset --after hard_reset write_flash "@flash_args" || exit 1

echo "Flash done. Running validation session ..."
exec python3 "$ENGINE_DIR/tools/board_session.py" --port "$PORT"
