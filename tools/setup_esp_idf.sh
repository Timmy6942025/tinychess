#!/usr/bin/env bash
# Install the ESP-IDF toolchain pinned to the version this project was built
# with (v5.3). Usage: ./tools/setup_esp_idf.sh [target-dir]
# Target dir defaults to ~/esp/esp-idf. Re-run the export step in every new
# shell before building: source "$IDF_PATH/export.sh"
set -euo pipefail

IDF_VERSION="v5.3"
TARGET="${1:-$HOME/esp/esp-idf}"

if [ -f "$TARGET/export.sh" ]; then
	echo "ESP-IDF already present at $TARGET - skipping clone."
	echo "Verify the version with: git -C $TARGET describe --tags"
	exit 0
fi

echo "== cloning ESP-IDF $IDF_VERSION =="
mkdir -p "$(dirname "$TARGET")"
git clone --recursive -b "$IDF_VERSION" --shallow-submodules \
	https://github.com/espressif/esp-idf.git "$TARGET"

echo "== running the installer (downloads the Xtensa/RISC-V toolchains) =="
"$TARGET/install.sh"

cat <<EOF

Done. To use the toolchain in a shell:

  export IDF_PATH="$TARGET"
  source "\$IDF_PATH/export.sh"

Then build the board firmware from the repo root:

  cd app && idf.py build && idf.py -p /dev/ttyACM0 flash
EOF