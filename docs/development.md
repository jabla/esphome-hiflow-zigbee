# Development

## Repository layout

| Path | What |
|---|---|
| `hiflow-zb.yaml` | the device config: Zigbee + `ble_client` + the component, 20 sensors (board independent) |
| `boards/` | board specific hardware and the chip variant, pulled in by the `board` substitution: `boards/xiao_esp32c6.yaml` (RF switch), `boards/waveshare_c6_lcd147.yaml` (on-board LCD) and `boards/waveshare_h2_zero.yaml` (ESP32-H2) |
| `hiflow_secrets.example.yaml` | template for your `hiflow_secrets.yaml` (also holds the `board` selection) |
| `components/hiflow_ble/` | the ESPHome external component (with flat copies of the C core, see its README) |
| `src/hiflow_core/` | C99 core: frames/crypto, payloads and paging, session state machine, clock |
| `src/hiflow_pb/` | vendored nanopb 0.4.9.2 and the generated protobuf code (`regen.sh` reproduces it) |
| `test/` | host test suites and the reference vectors |
| `tools/` | bleId generator, HA entity naming, flashing, log capture, BLE-only variant generator, Zigbee OTA files, a fake inverter over BLE for the bench (`tools/fake_inverter_ble/`) |
| `hiflow-zb-bleonly.yaml` | generated: the bridge without Zigbee, for diagnosing radio coexistence |
| `docs/` | this file, troubleshooting, energy-dashboard template |

## Boards

The config is board independent; `boards/<board>.yaml` carries the hardware and the chip
(`esp32: variant:`), and `board` in `hiflow_secrets.yaml` selects it (dynamic `!include`, so
nothing is duplicated):

| `board` | Hardware |
|---|---|
| `xiao_esp32c6` | Seeed Studio XIAO ESP32-C6. Drives the FM8625H RF switch (GPIO3/GPIO14); `external_antenna` picks the U.FL socket instead of the ceramic antenna. No display. |
| `waveshare_c6_lcd147` | Waveshare ESP32-C6-LCD-1.47 (non-touch). Shows the values on the on-board 172x320 panel, with pages on the BOOT button (see *Display* in the README). No RF switch, no U.FL socket; `external_antenna` is unused. |
| `waveshare_h2_zero` | Waveshare ESP32-H2-Zero (ESP32-H2FH4S, 4 MB flash). Tested on the bench only, not yet with an inverter. No RF switch, no U.FL socket, no display. |

A second board only needs a new file under `boards/` plus the substitution in the secrets file. The
display lambda reads the sensors by `id` (the `id:` lines in `hiflow-zb.yaml`) and the component's
state through `id(hiflow)`. The ids do not affect the Zigbee endpoint numbering, so they are
harmless on a board without a panel.

Build another board variant without touching the secrets file:

```bash
esphome -s board waveshare_c6_lcd147 config  hiflow-zb.yaml   # validate
esphome -s board waveshare_c6_lcd147 compile hiflow-zb.yaml   # build
```

What the panel costs, measured with ESPHome 2026.8.2 / ESP-IDF 5.5.5 (`esp_idf_size`):

| Variant | static DRAM | Flash code | OTA image |
|---|---|---|---|
| `xiao_esp32c6` | 130,502 B (28.9 %) | 1,027,676 B | 1,118,704 B |
| `waveshare_c6_lcd147` | 133,154 B (29.5 %) | 1,066,434 B | 1,159,776 B |

So the display costs about 39 KB of flash and 2.7 KB of static DRAM, plus its frame buffer, which
the driver allocates at runtime: without PSRAM `buffer_size` defaults to 1/6 of the screen
(`172 * 320 * 2 / 6` = 18,346 B) and the driver draws the frame in six passes instead of holding a
full 110 KB buffer. The OTA slot is 0x1B0000 = 1.77 MB, so the display image still leaves ~595 KB
free.

A temporary free-heap readout on the panel (`esp_get_free_heap_size()`, removed again) showed
**247 kB** after boot, before the first BLE connection, and **240 kB** with the HiFlow session up
and reading data, so the panel leaves ample runtime headroom.

The ESP32-H2 has far less RAM. Built from the same code, `waveshare_h2_zero` needs 129,890 B of
static DRAM, 50.3 % of its 258,000 B, and 137,120 B (53.1 %) with `sleepy: "true"` (the XIAO C6
at the same state: 139,362 B, 30.8 % of 452,112 B). Its image is 1,206,792 B (C6: 1,174,778 B).
On the bench (`tools/quicktest.sh`, which logs the free heap, its low mark and the largest free
block once a minute, against the fake inverter in `tools/fake_inverter_ble/`), an H2 without
`sleepy` had **30.5 kB** free with the session up and Zigbee joined, 26.1 kB at the lowest and
27.6 kB as the largest block, steady over 12 minutes. That is an eighth of the C6's and enough
for the session. A sleepy build had 19-20 kB free. A delta update over Zigbee took the low mark
down to 23.2 kB (15.0 kB sleepy); a zlib full image cannot run there (32 KB window), so the
H2 gets full images as heatshrink, see `components/zigbee_ota/README.md`.

The Waveshare C6 board carries **8 MB** of flash, the XIAO and the H2-Zero 4 MB. The config keeps
`flash_size: 4MB` for all of them, so the partition table is the same and the upper 4 MB of the
Waveshare stay unused. Switching between the boards therefore needs no special flash layout;
`tools/flash_config.sh` still erases the flash when the board comes from other firmware, such as
a vendor demo image.

## Build, test, flash

```bash
make -C test/host test                  # host tests: frame/crypto, protobuf, clock, payloads, session
components/hiflow_ble/sync_core.sh      # after changing anything under src/
python3 tools/make_bleonly_board.py     # after changing hiflow-zb.yaml
tools/flash_config.sh hiflow-zb.yaml      # build + flash with esptool, erases on another partition table (ESP=/ESPTOOL=/DEV=)
bash tools/quicktest.sh 300             # DEBUG image, 5 min of log, summary of the session
```

The host tests need a C compiler, the OpenSSL headers and Python 3. The ESPHome build does not
use OpenSSL: the component uses mbedTLS from ESP-IDF.

## How the session works

1. `ble_client` connects and subscribes to `ffe2`, and the handshake starts right away.
2. The first connection after a boot starts with the **V0 pairing**. It hands out the
   inverter's current session key (`encRand`, which the inverter rotates) and its clock. The key
   is kept in RAM only.
3. Handshake: login (action 64), then status polls. If the inverter asks for it, the BLE PIN follows
   (action 82). Then the time-sync (action 104).
4. Data: `0xA311` every `update_interval` (30 s), pages merged, published once per round.
5. Anything that goes wrong ends the connection: a reply missing for 15 s, a link that dies, or a
   frame that does not authenticate. The bridge then waits with backoff and holds **no**
   connection while it waits, so the single BLE slot stays free. A failed handshake makes the next
   connection pair again.

The state machine is `src/hiflow_core/hiflow_session.c`. It is plain C99 and tested on the host
against a simulated inverter.

## Status codes (`sensor.inverter_zb_status`)

| Code | Meaning |
|---|---|
| 0 | waiting for a connection (also the normal state at night) |
| 2 / 3 / 4 / 5 | V0 pairing · login · PIN · time-sync |
| 6 | ready, the session reads data every 30 s |
| 9 | login refused: the link died during the handshake |
| 10 | no connection came up (radio, placement, power) |
| 11 | a request went unanswered |
| 13 | the BLE PIN is missing or was refused |
| 14 | the session key does not fit: a frame did not authenticate, or the pairing handed back the key that just failed |
| 15 | the radio link timed out (supervision timeout) |

Codes 9–11 now and then are normal: the bridge waits (30 s, then doubling up to 5 min) and comes
back by itself. During daylight it should stay at 6.

The session also has a state 7 (a data request in flight). The bridge reports it as 6: it lasts
for one request and would cost two Zigbee reports per poll. A request that hangs shows as 11.
