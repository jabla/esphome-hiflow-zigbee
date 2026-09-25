# Development

## Repository layout

| Path | What |
|---|---|
| `esp32c6.yaml` | the device config: Zigbee + `ble_client` + the component, 20 sensors (board independent) |
| `boards/` | board specific hardware, pulled in by the `board` substitution: `boards/xiao_esp32c6.yaml` (RF switch) and `boards/waveshare_c6_lcd147.yaml` (on-board LCD) |
| `hiflow_secrets.example.yaml` | template for your `hiflow_secrets.yaml` (also holds the `board` selection) |
| `components/hiflow_ble/` | the ESPHome external component (with flat copies of the C core, see its README) |
| `src/hiflow_core/` | C99 core: frames/crypto, payloads and paging, session state machine, clock |
| `src/hiflow_pb/` | vendored nanopb 0.4.9.2 and the generated protobuf code (`regen.sh` reproduces it) |
| `test/` | host test suites and the reference vectors |
| `tools/` | bleId generator, HA entity naming, flashing, log capture, BLE-only variant generator |
| `esp32c6-bleonly.yaml` | generated: the bridge without Zigbee, for diagnosing radio coexistence |
| `docs/` | this file, troubleshooting, energy-dashboard template |

## Boards

The config is board independent; `boards/<board>.yaml` carries the hardware, and `board` in
`hiflow_secrets.yaml` selects it (dynamic `!include`, so nothing is duplicated):

| `board` | Hardware |
|---|---|
| `xiao_esp32c6` | Seeed Studio XIAO ESP32-C6. Drives the FM8625H RF switch (GPIO3/GPIO14); `external_antenna` picks the U.FL socket instead of the ceramic antenna. No display. |
| `waveshare_c6_lcd147` | Waveshare ESP32-C6-LCD-1.47 (non-touch). Shows status, AC power, voltage, temperature, the energy of the day and the free heap on the on-board 172x320 panel. No RF switch, no U.FL socket; `external_antenna` is unused. |

A second board only needs a new file under `boards/` plus the substitution in the secrets file. The
display lambda reads the sensors by `id` (`ac_power`, `ac_voltage`, `temperature`, `energy_daily`,
`hiflow_status`); those ids are declared in `esp32c6.yaml` and do not affect the Zigbee endpoint
numbering, so they are harmless on a board without a panel.

Build another board variant without touching the secrets file:

```bash
esphome -s board waveshare_c6_lcd147 config  esp32c6.yaml   # validate
esphome -s board waveshare_c6_lcd147 compile esp32c6.yaml   # build
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

A hardware run of the Waveshare variant reported **247 kB free heap** after boot while the bridge
was waiting for a BLE connection. This leaves ample runtime headroom; an active HiFlow session
was not connected during this measurement.

## Build, test, flash

```bash
make -C test/host test                  # host tests: frame/crypto, protobuf, clock, payloads, session
components/hiflow_ble/sync_core.sh      # after changing anything under src/
python3 tools/make_bleonly_board.py     # after changing esp32c6.yaml
tools/flash_config.sh esp32c6.yaml      # build + flash with esptool (ESP=/ESPTOOL=/DEV= override)
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
| 6 / 7 | ready · reading data |
| 9 | login refused: the link died during the handshake |
| 10 | no connection came up (radio, placement, power) |
| 11 | a request went unanswered |
| 13 | the BLE PIN is missing or was refused |
| 14 | the session key does not fit: a frame did not authenticate, or the pairing handed back the key that just failed |
| 15 | the radio link timed out (supervision timeout) |

Codes 9–11 now and then are normal: the bridge waits (30 s, then doubling up to 5 min) and comes
back by itself. During daylight it should alternate between 6 and 7.
