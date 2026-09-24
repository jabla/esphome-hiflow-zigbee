#!/usr/bin/env bash
# HiFlow bridge quick test: build a DEBUG image, flash it, capture the log and
# summarise what the session did.
#
# Prerequisites:
#   - the board is on USB (default /dev/ttyACM0), udev rule as in docs/troubleshooting.md
#   - nothing else talks to the inverter (S-Miles app, the hiflow_ble HA integration):
#     it serves a single BLE central
#   - daylight, otherwise the inverter's radio is off and the test says nothing
#
# Usage:  bash tools/quicktest.sh [seconds]        (default 180)
#         CONF=esp32c6-bleonly.yaml bash tools/quicktest.sh   (isolation run)
#         NOFLASH=1 bash tools/quicktest.sh 300   (capture only: no build, no flash)
set -euo pipefail
cd "$(dirname "$0")/.."

PY="${PY:-python3}"   # needs pyserial (ESPHome ships it)
DEV="${DEV:-/dev/ttyACM0}"
LOG="${LOG:-/tmp/hiflow-quicktest.log}"
SECS="${1:-${SECS:-180}}"
CONF="${CONF:-esp32c6.yaml}"
DEBUG_CONF="${CONF%.yaml}-debug.yaml"

if [ -n "${NOFLASH:-}" ]; then
  echo "== 1/3 NOFLASH set: capturing whatever the board runs now"
else
  echo "== 1/3 build and flash a DEBUG copy of $CONF"
  # The deployed image logs at WARN (a charger is not a USB host, and blocking
  # CDC writes end in a watchdog reset). For an attended run on laptop USB the
  # handshake steps are worth seeing.
  sed 's/^  level: WARN/  level: DEBUG/' "$CONF" > "$DEBUG_CONF"
  tools/flash_config.sh "$DEBUG_CONF"
fi

echo "== 2/3 capture $SECS s of log -> $LOG"
# `esphome logs` is unreliable on the USB-Serial/JTAG port, and opening it with
# the modem lines asserted holds the board in reset (docs/troubleshooting.md):
# open with dtr/rts low.
"$PY" - "$DEV" "$LOG" "$SECS" <<'PY'
import sys, time, serial

dev, log, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
s = serial.Serial()
s.port = dev
s.baudrate = 115200
s.timeout = 1
s.dtr = False
s.rts = False
s.open()
t0 = time.time()
with open(log, "w") as fh:
    while time.time() - t0 < secs:
        line = s.readline()
        if not line:
            continue
        txt = line.decode(errors="replace").rstrip()
        fh.write(txt + "\n")
        fh.flush()
        if any(k in txt for k in ("hiflow", "HiFlow", "WARN", "ERROR", "Connect", "connect")):
            print(txt, flush=True)
s.close()
PY

echo "== 3/3 summary"
logins=$(grep -c "handshake: login" "$LOG" || true)
connects=$(grep -c "subscribed to ffe2" "$LOG" || true)
complete=$(grep -c "handshake complete" "$LOG" || true)
data=$(grep -c "data page" "$LOG" || true)
drops=$(grep -c "link down (reason" "$LOG" || true)
echo "  connections: $connects   logins: $logins   handshakes completed: $complete"
echo "  data pages:  $data       link drops: $drops"
echo
echo "--- handshake ---"
grep -E "subscribed to ffe2|handshake|V0 pairing|PIN|time-sync|status" "$LOG" | tail -20 || true
echo "--- link drops (reason 0x08 radio, 0x13 inverter, 0x16 us) ---"
grep -E "link down \(reason|waiting [0-9]+ s" "$LOG" | tail -10 || true
echo "--- warnings and errors ---"
grep -E "\[W\]|\[E\]" "$LOG" | tail -15 || true
echo
echo "What to look for:"
echo "  * logins == connections            => one login per connection (the rule)"
echo "  * 'handshake complete' seconds after 'subscribed to ffe2'"
echo "  * a data page every ~30 s, status 6/7 in Home Assistant"
echo "  * no 'link down' during an established session"
echo
echo "Back to the deployed image (WARN):  tools/flash_config.sh $CONF"
