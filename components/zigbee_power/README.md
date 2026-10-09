# zigbee_power

Power saving for ESPHome's `zigbee` component on the ESP32-C6 and ESP32-H2: a sleepy end device
with light sleep, for a bridge on a battery or one that should run cooler on mains. The bridge
(`hiflow-zb.yaml`) runs with `sleepy: true` unless `hiflow_secrets.yaml` says `"false"`.

```yaml
zigbee_power:
  sleepy: true
  # all optional
  poll_interval: 3s          # 1 s to 7 s
  request_poll_window: 10s   # 0 s (off) to 60 s
  loop_interval: 1s          # 16 ms to 3 s
  night_poll_interval: 7s    # with hiflow_ble's night mode
  night_loop_interval: 1s
  awake_after_boot: 3min
  power_down_peripherals: false
  ble_sleep_clock: xtal      # or rc
  sleep_debug: false
  wake_button:
    pin: GPIO9
    on_press:
      - logger.log: "pressed"
  keep_pins: [6, 7, 22]
  timer_guard: true          # ESP32-C6 only
```

- `sleepy`: the node joins as a sleepy end device (receiver off while idle) and the chip goes
  to light sleep whenever every task is idle, with the 802.15.4 radio and the BLE controller
  sleeping as well. Not with `zigbee: router: true`.
- `poll_interval`: how often the node asks its parent for messages. The parent keeps a message
  for a sleepy child about 7.5 s, so it stays below that; below 1 s the stack would never
  notice a lost parent.
- `request_poll_window`: after a request from the network (ZHA's interview after an update, a
  reconfigure, a read or a write) the node polls every 50 ms until no request came for this
  long. Each request waits at the parent until the next poll, and the next one usually follows
  as soon as the answer is out: at a 3 s poll an interview took 7 to 14 minutes, with the window
  about 25 s. Only requests sent to this node count, not broadcasts, not answers and not the
  coordinator's acknowledgements of our reports. `0s` turns it off. Only with `sleepy: true`.
- `loop_interval`: ESPHome's main loop interval (16 ms by default), 1 s with `sleepy: true`.
  Every round wakes the chip. Also works without `sleepy`. At most 3 s, because the loop
  waits that long without feeding the 5 s task watchdog.
- `night_poll_interval`, `night_loop_interval`: in force while
  [`hiflow_ble`](../hiflow_ble/README.md#power-saving)'s night mode is on; they default to the
  day values.
- `awake_after_boot`: after a start with a USB host connected the chip stays awake this long,
  for logs and a USB flash after a press on RESET. In light sleep the USB port goes down. On a
  battery the chip sleeps at once. Updates without touching the board go over
  [Zigbee OTA](../zigbee_ota/README.md).
- `power_down_peripherals`: power the digital peripherals down in light sleep (about 180 down
  to 35 uA for the chip, datasheet). ESP-IDF only does it while every peripheral in use can
  restore its state, and BLE with `ble_sleep_clock: xtal` keeps them on.
- `ble_sleep_clock`: the BLE controller's clock in light sleep. `xtal` keeps the 40 MHz crystal
  running; `rc` lets it sleep, but a BLE connection with a 1 s interval drops every few
  minutes with it. `hiflow_ble`'s `night_ble_off` gets the same saving at night without that.
- `sleep_debug`: counts the light sleeps and those that powered the peripherals down, and logs
  what keeps them powered.
- `wake_button`: a button that wakes the chip from light sleep. Every press is counted in an
  interrupt and runs `on_press` once from the main loop, so a short press is not lost while
  the loop waits its `loop_interval`, nor merged with the next; bounces within 30 ms count once. The pin keeps its pull-up
  in sleep. Also works without `sleepy`.
- `keep_pins`: GPIO numbers that keep their normal setting in light sleep instead of floating:
  a display's control and reset lines, a backlight, an LED's data line.
- `timer_guard`: the workaround for the ESP32-C6's clock fault below. On by default on the
  ESP32-C6, whatever its revision (which revisions have the fault is not known); not available on
  the ESP32-H2.

Things to know:

- Switching `sleepy` either way, by USB or over Zigbee, makes the node join again: the parent
  learns the receiver mode at the join only. The component keeps the mode of the join in flash
  (a node without that record joined with the receiver on). After a start with the other mode
  the receiver stays on and the node polls every second, so the parent still reaches it; once
  the running image is confirmed (after an update over Zigbee) it leaves the network, clears
  its Zigbee data (nothing else) and joins again; a firmware download under way goes first.
  **The coordinator must permit joins then**:
  ESPHome tries ten times a second apart, then every ten minutes. ZHA keeps the device and its
  entities (same IEEE address).
  If it cannot get back into the old network first (every start failed for 5 minutes, at least
  three in a row), or the leave has not finished after 10 s, it clears its Zigbee data itself
  and joins as a new node. The start itself needs no network: with the coordinator unplugged
  the node started, left at once and joined again once the coordinator was back and permitted
  joins.
  Both happened on the ESP32-H2 after a USB flash; without this the node stayed out of the
  network for good and needed its Zigbee data erased by hand.
- On the ESP32-H2 a node that is not in a network restarts after three failed join tries in a
  row. In about one boot in five the H2 does not find the network for the whole boot (every try
  ends after about 17 s with "no network", while a C6 next to it joins at once), and a restart
  clears it more often than not (at the earliest 2 minutes after the boot). Waiting to be paired,
  an H2 therefore restarts every two minutes.
- With `sleepy: true` the component works around an ESP-IDF bug (seen with 5.5.5): when a
  sleepy node's 802.15.4 interrupt switches the radio off, ESP-IDF takes the PHY's mutex in the
  interrupt and aborts if a task holds it just then (the PLL tracking timer, the BLE
  controller). An ESP32-H2 on the bench restarted once from it, about 25 minutes into a Zigbee
  OTA download with the 50 ms poll; with a test task holding the mutex most of the time, both
  the ESP32-H2 and the ESP32-C6 crashed within seconds to minutes. In practice it takes BLE next
  to Zigbee: without BLE only such a test task provoked it. The linker routes
  `esp_phy_enable()`/`esp_phy_disable()` through `phy_guard.cpp`: with the mutex taken, the
  switch-off waits for FreeRTOS's timer task, and a switch-on before then cancels it. Without a
  holder the switch-off runs in the interrupt as before. The BLE controller's own PHY calls
  pass through unchanged; on the bench all of them came from a task, where waiting is fine.
- On the ESP32-C6, with or without `sleepy`, the component works around a hardware fault
  (ESP-IDF issue 19036, seen on a XIAO ESP32C6 with chip revision v0.2; `dump_config` shows the
  revision): the counter behind `esp_timer` loses bits and the clock jumps back by 4 s up to
  hours. Everything timed by `esp_timer` stops until it catches up (ESPHome's scheduler, the
  Zigbee poll), and a jump while the radio is on makes the PHY's switch-off wait with interrupts
  off until the interrupt watchdog resets the chip. On the bench one XIAO hit it three times in
  about 2 hours of Zigbee OTA downloads (two resets, one stalled download). `timer_guard.cpp`
  checks every reading of the clock against the FreeRTOS tick (the fault leaves the tick alone)
  and moves `esp_timer` forward before anyone sees it behind. It lives here because the Zigbee
  poll and the PHY switch-off are where the fault bites; a C6 without this component gets no
  guard. A repair logs a warning ("esp_timer jumped back ..."); nothing to do.
- A board that ran a sleepy image and then gets an image without this component, a rollback
  included, keeps its receiver off until it is paired again.
- GPIO outputs float while the chip sleeps: ESP-IDF isolates every pad in light sleep. A pin
  that must hold its level (a backlight, a display's reset line) goes into `keep_pins`.
- `slept_ms()`, `sleeps()`, `sleeps_pd_top()` and `chip_temperature()` are there for template
  sensors: the share of time asleep without any measuring equipment, and the die temperature
  without ESPHome's `internal_temperature`, which keeps the peripherals powered in light sleep
  even with `power_down_peripherals` (`hiflow-zb.yaml` reads it this way). The chip has
  one temperature sensor, so not both. With `sleepy: true`, `zigbee_power::phy_guard_stats()`
  counts what the PHY workaround above did, e.g. `deferred` (switch-offs that would have
  aborted) and `bt_nonblocking` (BLE controller calls where it could not wait; none seen). With
  `timer_guard`, `zigbee_power::timer_guard_stats()` gives `repairs` and `total_ms` (C6 builds
  only: wrap a lambda shared with the H2 in `#ifdef USE_ZIGBEE_POWER_TIMER_GUARD`).

For debugging, the share of the last 5 minutes spent in light sleep as a Zigbee sensor (around
98 % for a sleepy bridge by day, 0 % without `sleepy`; no value for the first 5 minutes after a
start). It is not part of `hiflow-zb.yaml`:

```yaml
sensor:
  - platform: template
    name: "Bridge Asleep"
    endpoint: 37
    report: force
    unit_of_measurement: "%"
    accuracy_decimals: 1
    update_interval: 300s
    lambda: |-
      static uint32_t last_slept = 0, last_now = 0;
      const uint32_t slept = id(zb_power).slept_ms(), now = millis();
      const float share = last_now == 0 ? NAN : 100.0f * (slept - last_slept) / (float) (now - last_now);
      last_slept = slept;
      last_now = now;
      return share;
```
