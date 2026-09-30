# HiFlow Pro to Zigbee bridge (ESP32-C6)

An ESPHome firmware that reads a **Hoymiles HiFlow Pro** inverter locally over **Bluetooth LE**
and reports its measurements as a **Zigbee end device** to Home Assistant (ZHA or Zigbee2MQTT).
No WiFi, no cloud: only BLE (to the inverter) and 802.15.4 (to your Zigbee coordinator).

```
inverter ──BLE──> ESP32-C6 ──Zigbee──> coordinator ──ZHA / Zigbee2MQTT──> Home Assistant
```

It keeps one **persistent** BLE session (log in once, poll every 30 s over the same link) and
exposes 32 values: AC power/voltage/current/frequency, reactive power, power factor,
temperature, energy total/today, the inverter's daily warning count, power/voltage/current and
energy total/today for each of the four PV ports, plus the session status and the board's
uptime. A value is reported when it changes noticeably or after a few minutes at the latest,
not on every 30 s poll.
It also adds a slider for the inverter's power limit and a switch that turns the inverter on
and off.

The protocol implementation is a C port of [TheTiEr/hiflow-ble](https://github.com/TheTiEr/hiflow-ble),
the library behind the [ha-hiflow-ble](https://github.com/TheTiEr/ha-hiflow-ble) integration. It
is checked against reference vectors generated with that library.

## Status

Works with **ZHA** and **Zigbee2MQTT**. Tested on one **HMS-2000-4WB** with a Seeed **XIAO
ESP32-C6** and a **Waveshare ESP32-C6-LCD-1.47**. Other HiFlow Pro / HMS-WB models that work
with ha-hiflow-ble should work too. Reports are welcome.

## What you need

<img src="docs/img/waveshare-panel.png" align="right" width="188" alt="Waveshare ESP32-C6-LCD-1.47 overview page: session status, clock, AC power, energy today and in total, power limit">

- A **Seeed Studio XIAO ESP32-C6** ([Amazon.de](https://www.amazon.de/dp/B0D2NKVB34)) or a
  **Waveshare ESP32-C6-LCD-1.47** ([Amazon.de](https://www.amazon.de/dp/B0DHTMYTCY)). The board
  is picked with the `board` setting in `hiflow_secrets.yaml` (see *Boards* in
  `docs/development.md`): the XIAO variant drives its RF switch and can use an external U.FL
  antenna, the Waveshare variant shows the values on its on-board 172x320 display (rendering on
  the right, example values; see *Display* below).
- A Zigbee coordinator in ZHA or Zigbee2MQTT (2.8.0 or newer), on current firmware. Tested with
  a ConBee III ([Amazon.de](https://www.amazon.de/dp/B0C8HV79N7)) on deCONZ firmware
  **0x26550900**.
- ESPHome with native Zigbee on the ESP32-C6 (tested with **2026.8.2**), plus Python 3 for the
  helper scripts.
- The inverter within BLE range of the board. An external U.FL antenna is optional.
- **Nothing else connected to the inverter over BLE.** It serves exactly one BLE central. Close
  the S-Miles app, and disable the ha-hiflow-ble integration and any BLE proxy that talks to it.

## Quick start

1. **Credentials.** Copy `hiflow_secrets.example.yaml` to `hiflow_secrets.yaml` and fill it in.
   The comments in the file say where each value comes from:
   - set `board` to `xiao_esp32c6` or `waveshare_c6_lcd147`;
   - the serial tail comes from the inverter's BLE name `RMI-XXXXXXXXXXXX`, the MAC address from
     a BLE scanner app or Home Assistant's Bluetooth panel (the two differ);
   - get a `ble_id` from `python3 tools/gen_ble_id.py`, or reuse the one from ha-hiflow-ble;
   - the PIN is the Bluetooth PIN from the S-Miles app;
   - set whether the external antenna is used.

   The build stops with an error while the MAC, the serial or the `ble_id` still hold the
   example values: a bridge built with them would never find the inverter.
2. **Time zone.** Check `offset` and `eu_dst` under `hiflow_ble:` in `esp32c6.yaml` (default:
   CET with European summer time). The inverter's own clock is set from them, and that clock
   drives its daily energy reset.
3. **Build first:**
   ```bash
   esphome compile esp32c6.yaml
   ```
   The first build compiles ESP-IDF and takes several minutes, longer than the coordinator's
   pairing window.
4. **Open pairing, then flash** over USB. In ZHA click *Add device*, in Zigbee2MQTT *Permit join*,
   and leave it open, then run:
   ```bash
   DEV=/dev/ttyACM0 tools/flash_config.sh esp32c6.yaml
   ```
   `DEV` is the board's port (`ls /dev/serial/by-id/`, it shows up as *Espressif USB JTAG*); with
   a Zigbee stick on the same computer, make sure it is not the stick. The board looks for a
   network only during its first seconds after a boot, then only every 10 minutes.

   The script erases the whole flash first if the board ran any other firmware before: a BLE
   proxy, another ESPHome config, another Zigbee firmware. It compares the partition table on
   the board with the new one. The old firmware's settings would otherwise end up inside the
   bridge's, and neither BLE nor Zigbee comes up. Reflashing the bridge keeps its settings and
   its Zigbee pairing. Flash with `esphome run` only if you erase first
   (`esptool --chip esp32c6 --port /dev/ttyACM0 erase_flash`).

   The board joins as `HMS-2000-4WB Bridge`. If it missed the pairing window, open
   pairing again and reset the board (RESET button or re-plug). Zigbee2MQTT builds a generated
   definition for it, with the sensor names from `esp32c6.yaml`.
5. **Optional, ZHA: name the entities.** ZHA names analog inputs generically. This command gives
   them the ids `sensor.inverter_zb_*` and English names by endpoint:
   ```bash
   python3 -m venv .venv && .venv/bin/pip install websockets
   HASS_URL=http://homeassistant.local:8123 HASS_TOKEN=<long-lived token> \
     .venv/bin/python tools/ha_fix_bridge_entities.py
   ```
   `--hide-extras` leaves only the AC power visible, `--prefix` changes the ids, and `--dry-run`
   only shows what would happen.
6. **Optional: add it to the energy dashboard.** ZHA analog inputs can only be `measurement`,
   so the energy dashboard needs a template sensor on top. It is in
   `docs/ha-energy-template.yaml`.

Put the board at the inverter and power it from a USB charger. The deployed image logs at
`WARN`, which matters on a charger (see `docs/troubleshooting.md`).

## Controls

Next to the measurements, the bridge adds two controls.

- **Power limit** (endpoint 32): a slider in percent of the rated power, in 10 % steps. The
  inverter keeps the limit in its own flash. It is meant for a limit you set now and then, not
  for zero-export regulation. 0 % switches the output off.
- **On/off** (endpoint 34): a switch for the inverter's output. The inverter does not report
  whether it is on, so the switch shows what the bridge last switched it to. A change made in
  the S-Miles app does not show.

A change that cannot reach the inverter within two minutes (at night, for example) is dropped
and the control goes back. The details are in the configuration reference in
[`components/hiflow_ble/README.md`](components/hiflow_ble/README.md).

## Network time

Without WiFi there is no NTP. The bridge asks the Zigbee coordinator for the time, through a
Time client cluster on endpoint 35: once after joining, then twice a day. ZHA and Zigbee2MQTT
answer from the host's clock. The bridge sets the inverter's
clock at every login, and that clock drives the inverter's daily energy reset, so the time stays
right even after a power cut at night.

When an update adds endpoints (the controls on 32 and 34, the network time on 35), an existing
bridge has to re-join after the flash: follow *After changing the sensor list* in
`docs/troubleshooting.md`. In ZHA, reload the integration once after the re-join (see there).

## Display (Waveshare board)

<p align="center"><img src="docs/img/waveshare-pages.png" alt="The five pages of the Waveshare panel: overview, the day's power curve, energy per PV input, grid values, bridge state"></p>

Rendered from the display code, with example values: an HMS-2000 at a 40 % power limit on a
sunny early afternoon.

By day the panel shows the overview at 25 % brightness: the session status, the AC power, the
energy of the day and in total, and the day's peak (or the power limit while it is below
100 %). The BOOT button turns the brightness up and steps through the other pages, 15 s each,
then goes back to the overview; another press moves on to the next page at once:

- **Today**: the AC power curve from 05:00 to 22:00, with the peak and when it was.
- **Ports**: the energy of the day per PV input, the best one in green, with the live power and
  voltage.
- **Grid**: voltage, frequency, current, reactive power, power factor and temperature.
- **Bridge**: uptime, BLE sessions and failures, power limit, output on or off, and the
  inverter's warning count.

A page or a row without data is left out. The status line reads `LIVE` with the age of the last
data, `STANDBY`, `TURNED OFF`, or the step the session is at (the status codes are in
`docs/development.md`). The clock shows once the time was set since the boot. After a reboot the
power limit shows in grey until it is read again: it is the last one read, which the inverter
keeps over the night.

At night the panel goes dark and the RGB LED glows dim red, so the button can be found; a press
shows the pages with the values of the day. Night means the inverter has fed in nothing for five
minutes (`STANDBY`), and the panel lights up again once it has fed in for a minute. That comes
from the inverter's readings alone, not from the clock, so it holds at any latitude and in any
time zone, and after a reboot at night the panel stays dark. A link lost while the inverter feeds
in is a fault and stays on the panel, and so does a refused PIN.

The day's values are kept in flash, so a reboot or a power cut does not lose them. A new day
begins when the inverter starts its day counter again in the morning, however long the bridge
was off; until then the pages show the last day (titled "Last day" unless the clock says it is
today). The curve needs the time: until the network time or the inverter's time has arrived
after a boot, the day's energy and peak are kept, but no curve is drawn.

The brightness, the page time and the night mode (`glow`, `off`, or `screensaver`: the panel
stays dim with "HiFlow" bouncing off its edges) are substitutions at the top of
`boards/waveshare_c6_lcd147.yaml`.

## More documentation

- [`docs/troubleshooting.md`](docs/troubleshooting.md): radio, inverter, Zigbee and USB details
- [`components/hiflow_ble/README.md`](components/hiflow_ble/README.md): the component on its own
  (use it from GitHub in your own ESPHome config), configuration reference, protocol notes
- [`docs/development.md`](docs/development.md): repository layout, host tests, build and flash
  tools, how the session works, and the status codes of `sensor.inverter_zb_status`

## Limitations

- Right after a boot, ZHA shows 0 for the measurements until the first data page arrives (about
  30 s later). The energy total is not affected, so the energy dashboard stays correct.
- The power limit is percent only. The inverter's rated power is not read, so there is no watt
  slider.
- All measurements that ha-hiflow-ble shows are exposed, except the per-port error code (in
  practice the same `0x03000000` on every port while the inverter feeds in). The warning count
  says how many warnings there were, not which ones.

## Credits and license

MIT, see `LICENSE`. The protocol knowledge, the `.proto` files and the Python reference code in
`test/ref/` come from [TheTiEr/hiflow-ble](https://github.com/TheTiEr/hiflow-ble) (MIT).
[nanopb](https://github.com/nanopb/nanopb) is vendored under its zlib license. See
`THIRD_PARTY.md`.