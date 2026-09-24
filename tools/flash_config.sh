#!/usr/bin/env bash
# Builds an ESPHome config and flashes its factory.bin with esptool.
# Usage: tools/flash_config.sh esp32c6.yaml [esp32c6-bleonly.yaml]
set -euo pipefail
cd "$(dirname "$0")/.."
ESP="${ESP:-esphome}"
ESPTOOL="${ESPTOOL:-esptool}"
DEV="${DEV:-/dev/ttyACM0}"
CONF="${1:?name the config file}"
# ESPHome builds into .esphome/build/<esphome: name:>, so take the name from the config.
NAME="$(sed -n 's/^  name: *\([A-Za-z0-9_-]*\).*/\1/p' "$CONF" | head -1)"
[ -n "$NAME" ] || { echo "no 'esphome: name:' in $CONF" >&2; exit 1; }
BUILD=".esphome/build/$NAME/build/firmware.factory.bin"

"$ESP" compile "$CONF" >/dev/null
[ -f "$BUILD" ] || { echo "no $BUILD was built" >&2; exit 1; }
cp "$BUILD" "/tmp/$(basename "${CONF%.yaml}").factory.bin"
"$ESPTOOL" --chip esp32c6 --port "$DEV" --baud 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x0 "/tmp/$(basename "${CONF%.yaml}").factory.bin" \
  2>&1 | tail -3
