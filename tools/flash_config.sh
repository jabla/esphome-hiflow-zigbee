#!/usr/bin/env bash
# Builds an ESPHome config and flashes its factory.bin with esptool.
# Usage: DEV=/dev/ttyACM0 tools/flash_config.sh hiflow-zb.yaml [hiflow-zb-bleonly.yaml]
# Works for any ESP32-C6 or ESP32-H2 config, e.g. ~/esphome/ble-proxy.yaml when going back to a
# proxy; the chip comes from the build.
#
# If the partition table on the board differs from the new image's, the board ran other
# firmware before (a BLE proxy, another ESPHome config, another Zigbee firmware), so the
# whole flash is erased first. Otherwise that firmware's settings would sit inside this
# image's nvs and zb_fct partitions. A build with the same table keeps the settings and the
# Zigbee pairing.
set -euo pipefail
ESP="${ESP:-esphome}"
ESPTOOL="${ESPTOOL:-esptool}"
DEV="${DEV:-/dev/ttyACM0}"
CONF="${1:?name the config file}"
# ESPHome builds into .esphome/build/<esphome: name:> next to the config, so take the name
# from the config.
NAME="$(sed -n 's/^  name: *\([A-Za-z0-9_-]*\).*/\1/p' "$CONF" | head -1)"
[ -n "$NAME" ] || { echo "no 'esphome: name:' in $CONF" >&2; exit 1; }
BUILD="$(dirname "$CONF")/.esphome/build/$NAME/build/firmware.factory.bin"
FACTORY="/tmp/$(basename "${CONF%.yaml}").factory.bin"

"$ESP" compile "$CONF" >/dev/null
[ -f "$BUILD" ] || { echo "no $BUILD was built" >&2; exit 1; }
cp "$BUILD" "$FACTORY"
SDKCONFIG="$(dirname "$BUILD")/../sdkconfig.$NAME"
CHIP="$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"/\1/p' "$SDKCONFIG")"
[ -n "$CHIP" ] || { echo "no CONFIG_IDF_TARGET in $SDKCONFIG" >&2; exit 1; }

# The partition table is 0xC00 bytes at 0x8000, in the factory image as on the board.
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
dd if="$FACTORY" of="$TMP/new.bin" bs=1024 skip=32 count=3 status=none
[ "$(od -An -tx1 -N2 "$TMP/new.bin" | tr -d ' ')" = aa50 ] \
  || { echo "no partition table at 0x8000 in $FACTORY" >&2; exit 1; }
"$ESPTOOL" --chip "$CHIP" --port "$DEV" read_flash 0x8000 0xC00 "$TMP/board.bin" >/dev/null
if ! cmp -s "$TMP/new.bin" "$TMP/board.bin"; then
  echo "the board has another partition table (other firmware before): erasing the whole flash"
  "$ESPTOOL" --chip "$CHIP" --port "$DEV" erase_flash 2>&1 | tail -1
fi

# Flash mode, frequency and size stay as the build put them into the bootloader header
# (the C6 runs its flash at 80 MHz, the H2 at 48 MHz at most).
"$ESPTOOL" --chip "$CHIP" --port "$DEV" --baud 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode keep --flash_freq keep --flash_size keep 0x0 "$FACTORY" \
  2>&1 | tail -3

# With zigbee_ota, keep the app image under its version: the next update over
# Zigbee can then be a delta against it (tools/make_zigbee_ota.py --from).
INFO="$(dirname "$BUILD")/../zigbee_ota.json"
if [ -f "$INFO" ]; then
  VERSION="$(sed -n 's/.*"file_version": *\([0-9]*\).*/\1/p' "$INFO")"
  KEEP="$(dirname "$CONF")/.esphome/zigbee_ota"
  mkdir -p "$KEEP"
  cp "$(dirname "$BUILD")/firmware.ota.bin" "$KEEP/$NAME-$(printf '%08X' "$VERSION").bin"
  echo "kept as $KEEP/$NAME-$(printf '%08X' "$VERSION").bin (version 0x$(printf '%08X' "$VERSION"))"
fi
