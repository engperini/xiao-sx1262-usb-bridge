#!/usr/bin/env bash
# Builds the firmware and writes the .uf2 to build/.
#
# Needs arduino-cli and python on PATH, plus adafruit-nrfutil (pip install
# adafruit-nrfutil), which the Seeed nRF52 core calls during the build.
#
# Usage: scripts/build.sh [board]
#   board: xiao_nrf52840 (default)
set -euo pipefail

BOARD="${1:-xiao_nrf52840}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SKETCH="$ROOT/firmware/xiao-sx1262-usb-bridge"
OUT="$ROOT/build"

SEEED_INDEX="https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json"
SEEED_CORE="Seeeduino:nrf52@1.1.13"

case "$BOARD" in
  xiao_nrf52840)
    FQBN="Seeeduino:nrf52:xiaonRF52840"
    CORE="$SEEED_CORE"
    UF2_FAMILY="0xADA52840"
    ;;
  *)
    echo "unknown board: $BOARD" >&2
    exit 1
    ;;
esac

arduino-cli core update-index --additional-urls "$SEEED_INDEX"
arduino-cli core install "$CORE" --additional-urls "$SEEED_INDEX"

BUILD_DIR="$OUT/$BOARD"
mkdir -p "$BUILD_DIR"
arduino-cli compile \
  --fqbn "$FQBN" \
  --warnings all \
  --output-dir "$BUILD_DIR" \
  "$SKETCH"

DATA_DIR="$(arduino-cli config get directories.data)"
UF2CONV="$(find "$DATA_DIR/packages/Seeeduino/hardware/nrf52" -name uf2conv.py | head -n 1)"
python "$UF2CONV" -f "$UF2_FAMILY" -c \
  -o "$OUT/xiao-sx1262-usb-bridge-$BOARD.uf2" \
  "$BUILD_DIR/xiao-sx1262-usb-bridge.ino.hex"

echo "Built $OUT/xiao-sx1262-usb-bridge-$BOARD.uf2"
