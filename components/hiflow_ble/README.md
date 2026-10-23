# hiflow_ble — Hoymiles HiFlow Pro over BLE

ESPHome external component that reads a Hoymiles HiFlow Pro (BKW / balcony
power station) over Bluetooth Low Energy and exposes the decoded values as
ESPHome sensors.

The built-in `ble_client` component owns the BLE link (scan, connect, MTU
negotiation, notify plumbing). This component implements the HiFlow
application protocol on top of it.

## Layout

```
components/hiflow_ble/
├── __init__.py            parent component schema + codegen
├── hiflow_ble.h/.cpp      HiflowBle: BLEClientNode, the adapter around the session core
├── sensor/                the sensor platform (`platform: hiflow_ble`)
├── sync_core.sh           flattens src/ into this directory (see below)
├── README.md
└── <copied C core>        hiflow_frame/proto/session/clock .[ch], hiflow_crypto*.c,
                           pb*.c/h, RealDataNew/CommCmdPB/APPInfomationData .pb.[ch]
```

### Why the C core is copied, not referenced

`sync_core.sh` copies `src/hiflow_core/*` and the vendored nanopb +
generated protobuf sources flat into this directory. Two ESPHome loader
facts force this:

* only files **directly** in `components/<name>/` are compiled — arbitrary
  subdirectories are invisible (`recursive_sources=False`);
* `cg.add_build_flag("-I…")` is filtered out, so include paths cannot be
  added. Defines do survive (`-DHIFLOW_CRYPTO_BACKEND_MBEDTLS`).

The copies are therefore the build input; the originals under `src/` stay
untouched and keep feeding the host test suite. Re-run the script after
changing anything under `src/`:

```sh
components/hiflow_ble/sync_core.sh
```

Note that `sensor/` works as a normal ESPHome platform directory even though
arbitrary subdirectories do not, because Python subpackages load as their own
components.

## Protocol notes (verified against the HiFlow reference implementation)

* GATT service `0000e0ff-3c17-d293-8e48-14fe2e4da212`, write `ffe1`, notify
  `ffe2`, MTU 512.
* V1 frames: `"HM" | cmd(BE) | tid(BE) | CRC16-Modbus(ct) | len=len(ct)+10 |
  ct | 16-byte GCM tag`. Key/nonce/AAD derive from `enc_rand`
  (AES-128-GCM). Replies may arrive split across several notifications; the
  expected frame size is `len + 16` (V0 pairing cmds `0xA201/0xA301/0x8901/
  0x7901` have no tag).
* Handshake after every connect — and exactly once per connection, see
  `hiflow_session.c` — on cmd `0xA318` (CommCmdResDTO) with status polls on
  `0xA319`:
  1. action `64`, data = BLE id; poll until `sts == 1` (max 5 polls, 1 s
     apart);
  2. `sts == 3`: action `82`, data = BLE PIN; poll until `sts == 0`
     (max 8 polls);
  3. action `104`, data = `"<unix>,<tz offset>\r"`; one poll.
* Measurements are fetched with cmd **`0xA311`** (not `0xA303`). The request
  carries a `RealDataNewResDTO` message (`time_ymd_hms`, `cp`, `offset=3600`,
  `time`); the reply is a `RealDataNewReqDTO` with `ap` = number of pages.
  While `cp < ap - 1` the component re-requests with `cp+1`.
* Scaling: power ×0.1 gives W, voltage ×0.1 gives V, current ×0.01 gives A,
  frequency ×0.01 gives Hz, temperature ×0.1 gives °C; energies are already Wh.
* Power limit (only with `power_limit: true`): read with cmd **`0xA309`**
  (`GetConfigResDTO{offset, time}`), reply field 5
  `limit_power_mypower` in tenths of a percent (left off the wire at 0).
  Written with cmd **`0xA305`**, `CommandResDTO{time, action = 8,
  package_nub = 1, tid = time, data = "A:<tenths>,B:0,C:0\r"}`; the device
  acknowledges with `err_code` and the bridge reads the value back. Both run
  between two data polls on the logged-in connection. The reference library
  sends the config read with the time 60 s in the past; the inverter drops the
  link on that (like on a login whose time lags its clock) and answers the
  current time.
* Inverter on/off (only with `inverter_control: true`): cmd **`0xA305`**,
  `CommandResDTO{time, action = 6 (on) / 7 (off), dev_kind = 1,
  package_nub = 1, tid = time, mi_to_sn = [serial]}`, where the serial is
  `serial_number` of the grid block in the data. The device acknowledges with
  the action and `err_code`. The reference library sends no `time`; the
  inverter ignores that request (no reply, no effect) and switches when the
  current time is set.

`RealDataNewReqDTO`/`ResDTO` are named the other way round on the wire
compared to the vendor `.proto`: the *request* is built from `…ResDTO`, the
*reply* is decoded from `…ReqDTO`. Same inversion on the CommCmd pair.

## Configuration

The component can be pulled straight from GitHub into your own ESPHome config (the commented
`source:` below); `hiflow-zb.yaml` in the repository root is a complete example.

```yaml
substitutions: !include hiflow_secrets.yaml   # gitignored

external_components:
  - source:
      type: local
      path: components
    components: [hiflow_ble]
  # or, without a local checkout:
  # - source: github://jabla/esphome-hiflow-zigbee@main
  #   components: [hiflow_ble]

ble_client:
  - id: hiflow_client
    mac_address: ${hiflow_mac}

hiflow_ble:
  id: hiflow
  ble_client_id: hiflow_client
  sn: ${hiflow_sn}
  ble_id: ${hiflow_ble_id}
  pin: ${hiflow_ble_pin}
  offset: 3600
  update_interval: 30s

sensor:
  - platform: hiflow_ble
    hiflow_ble_id: hiflow
    type: ac_power
    name: "HiFlow AC Power"
```

`type:` is one of `ac_power`, `ac_voltage`, `ac_current`, `ac_frequency`,
`reactive_power`, `power_factor`, `temperature`, `energy_total`,
`energy_daily`, `warnings` (the inverter's warning count since dawn),
`port{1..4}_{power,voltage,current,energy_total,energy_daily}` and `status`
(the session state as a number). Unit / device class / state class / accuracy
default to the physically correct values and can be overridden.

Every sensor publishes on every poll. To send fewer Zigbee reports, give it
ESPHome's standard filters, as `hiflow-zb.yaml` does:

```yaml
    filters:
      - or:
          - throttle: 300s   # at the latest every 5 minutes
          - delta: 5         # or as soon as it moved by more than 5 W
```

The status is the exception: the component publishes it on change and every
5 minutes, and reports the short state 7 (a data request in flight) as 6.

At night the inverter is dark and sends nothing, so each sensor would keep its
last reading until the morning, and the last one at dusk can be a few tenths
of a watt. Once the inverter is dark (in standby: it fed in nothing for 5
minutes, and the latest reading since the boot was below 1 W) and the link is
gone, the power and current sensors (AC, reactive, per port) are set to 0;
voltages, counters, power factor, frequency and temperature keep their last
values, and a sensor without a reading stays without one. The 0 goes out three
times, 5.5 minutes apart, so a throttle lets one through and a lost report is
sent again; a filter chain that never passes a 0 (a lone `delta`, an `offset`)
keeps the old value. A link lost while the inverter still feeds in, or after a
boot before the first reading, changes nothing; so a dusk whose last reading
was 1 W or more keeps that value until the morning.

`power_limit`, `inverter_control` and `network_time` add Zigbee endpoints.
All three default to `true` wherever the `zigbee` component is loaded
(`zigbee_id:` picks one if there are several), so the BLE-only build leaves
them out. Set one to `false` to leave its endpoint out, e.g. when your own
config already uses the number.

`power_limit` is the power limit slider: a Zigbee Analog Output on endpoint 32
(0-100 %, resolution 10). ZHA shows it as a number entity, Zigbee2MQTT as
`hiflow_power_limit_32`.

* The bridge writes only when the slider moves: never on a boot or a
  reconnect, never the value that is already set, and at most once a minute.
  A slider drag becomes one write, 5 s after the last move.
* The slider shows what the inverter reports. The bridge reads the limit after
  every login and every hour (so it follows changes made in the S-Miles app)
  and reads it back after each write. A value off the 10 % grid, e.g. 87 from
  an automation, ends up as 90.
* A change that cannot reach the inverter within two minutes is dropped and
  the slider returns to the inverter's value, with or without a link (at
  night too). Before the first read since the boot that is the last value
  read, which the inverter keeps over the night, or empty if there is none
  (Zigbee2MQTT then keeps showing the value it had).

`inverter_control` is the on/off switch: a Zigbee On/Off cluster on
endpoint 34.

* The inverter reports no on/off state. The switch shows the state the
  inverter last acknowledged, and the bridge keeps it in flash across reboots.
* A command goes out only when the switch is flipped, never on a boot or a
  reconnect, between two data polls. Without an acknowledgement, or when it
  cannot reach the inverter within two minutes, the switch goes back.
* Off stops the feed-in within seconds and counts in the inverter's daily
  warning counter.

`network_time` reads the time from the coordinator through a Time client
cluster on endpoint 35, so the clock is right even after a power cut at night.

### Power saving

Four options cut the time the radio and the chip are awake. The component
leaves them off, the bridge's `hiflow-zb.yaml` turns them on. Together with
the [`zigbee_power`](../zigbee_power/README.md) component they let the bridge
sleep most of the time.

```yaml
hiflow_ble:
  # ...
  connection_interval: 1s     # once the session is ready
  night_ble_off: 2min         # or night_scan_interval: 10240ms
```

* `connection_interval` (50 ms to 2 s): the BLE connection interval once the
  session is ready. Every connection event wakes the chip; ESP-IDF's default
  is 30-50 ms. The handshake runs at the default, and the bridge reads back
  what the inverter accepted (asking again up to three times).
* Night mode comes in two kinds; set one of them:
  * `night_scan_interval` (100 ms to 10.24 s): the scanner looks only this
    often. The scan window stays the one of `esp32_ble_tracker`.
  * `night_ble_off` (30 s to 30 min): BLE is off altogether and comes on for
    a 15 s scan every `night_ble_off`. Without BLE the 40 MHz crystal and the
    peripherals can sleep too. BLE must not be shared with another
    `ble_client` or a `bluetooth_proxy`. The first link after dusk can take
    up to this long. Needs ESPHome 2026.9.0 or newer, which settles the
    scanner and the client when BLE goes off.

  Night mode starts when the link drops after the inverter fed in nothing
  (standby), or when the bridge has not seen the inverter (no advert, no
  connection attempt) for 10 minutes after standby or since boot, or for an
  hour without either. The first link ends it. No clock is involved.
* `night_test`: bench only. Every lost link starts night mode at once, so a
  fake inverter switched off counts as a night.

### Memory

The ESP32-H2 is short of RAM. `hiflow-zb.yaml` trims the BLE stack on every
board, so all boards run the same stack:

* The day's curve, which only a display draws, is left out when the config
  has no `display:` (1.1 kB).
* `btu_task_stack_size` (3072 to 8192 bytes): the stack of Bluedroid's BTU
  task. `esp32_ble_tracker` sets 8192, which `sdkconfig_options` cannot
  override; `hiflow-zb.yaml` sets the ESP-IDF default of 4096.
* A config may build Bluedroid without its security manager
  (`CONFIG_BT_BLE_SMP_ENABLE: n`): nothing pairs with the inverter. ESPHome
  still calls two of its functions; `ble_smp_off.cpp` stands in for them.

There is no `enc_rand:` option: the inverter rotates its session key, so the
session fetches it with a V0 pairing after every boot and after every failed
handshake, and keeps it in RAM only.

## Notes

* **Time**: the session runs a synthetic clock (`hiflow_clock`: the build time
  or the last time saved in flash, plus the uptime). In light sleep the uptime
  gains tens of seconds a day. With `network_time`, the coordinator's time is
  offered once after the join and then twice a day (every 10 minutes while the
  request fails). It sets the clock in either direction; a step of more than
  five minutes needs the inverter's time to agree with it. Before there is
  one, a step forward is taken too: after a power cut the clock restarts behind.
  Without a network time in the last day, the inverter's time sets the clock:
  forward by any amount, back by at most five minutes. The clock counts as set
  (the display shows it, the day curve uses it) once a time arrived that set
  it or agrees with it within five minutes.
* **Request timestamps**: the inverter refuses a login about a minute or more
  ahead of its own clock or behind it. A time-sync 600 s ahead is refused too.
  So every request carries the inverter's own time, taken from its last V0
  pairing reply or login acknowledgement, instead of our clock; the time-sync
  adds our UTC offset. The V0 pairing carries our clock: it is the request
  that reads the inverter's time. Once that time is 6 hours old (the uptime
  gains up to about 10 s in that span), the next link starts with a V0 pairing.
* `energy_total` and `energy_daily` are sums over the PV ports.
* Single-phase installs report through `SGSMO`, three-phase through `TGSMO`;
  the component handles both and picks port 0 for the AC values.
