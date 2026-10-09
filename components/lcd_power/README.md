# lcd_power

Power saving for a small SPI panel next to a sleepy chip (ESP32-C6, ESP32-H2), used by the
Waveshare ESP32-C6-LCD-1.47 board. Works with [`zigbee_power`](../zigbee_power/README.md)'s
`sleepy: true`; without light sleep it is a plain PWM output.

```yaml
output:
  - platform: lcd_power
    id: lcd_backlight
    pin: GPIO22
    frequency: 20kHz   # 100 Hz to 100 kHz, 20 kHz by default
```

- The backlight output keeps its PWM running while the chip light-sleeps. ESPHome's `ledc`
  output clocks the LEDC from the PLL, which stops in light sleep, so a lit panel used to keep
  the chip awake all day. This output clocks it from the internal RC_FAST oscillator
  (17.5 MHz on the C6: 9 bits of duty at 20 kHz; 8.5 MHz on the H2: 8 bits) and has ESP-IDF
  keep that clock and the pin running in light sleep. At level 0 it lets go of the clock
  again, so at night RC_FAST sleeps too. Should RC_FAST not reach the frequency, the output
  falls back to the PLL and keeps the chip awake while lit, with a warning in the log.
- `lcd_power::set_panel_sleep(id(display), true|false)`, from a lambda: sends SLPIN or SLPOUT
  to a panel driven by ESPHome's `mipi_spi` (ST7789 and other MIPI DCS controllers). In sleep
  the controller draws a few uA instead of a few mA; its frame memory stays and can still be
  written. The waits the ST7789 needs (120 ms between the two, 5 ms after SLPOUT) happen in
  the call.
- `lcd_power::set_status_led(gpio, r, g, b)`, from a lambda: sets a single WS2812-type RGB LED
  (bytes sent in R, G, B order, as this board's LED wants them; a genuine WS2812 takes G, R, B). The RMT channel is enabled only while the 24 bits go out; the LED
  keeps its colour without a signal. ESPHome's `esp32_rmt_led_strip` keeps its channel enabled,
  and an enabled RMT channel holds a power lock (CPU_FREQ_MAX) that keeps the chip out of light
  sleep altogether, day and night.

Things to know:

- All LEDC timers of the C6 and H2 share one clock source, so this output does not go
  together with ESPHome's `ledc` output. It uses LEDC timer 0 and channel 0.
- `set_status_led()` needs the output in the config: the output pulls the RMT driver into the
  build. The first call sets up the LED pin; later calls use that pin.
- RC_FAST is an RC oscillator (about +-7 %), fine for a backlight, not for anything that
  needs an exact frequency.
