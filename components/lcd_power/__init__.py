"""Power saving for a small SPI panel next to a sleepy chip (ESP32-C6, ESP32-H2).

Three parts:

- An output platform for the backlight PWM that keeps running in light sleep.
  ESPHome's `ledc` output clocks the LEDC from the PLL, which stops in light
  sleep, so the backlight went dark and the chip had to stay awake while it
  was lit. This one clocks the LEDC from the internal RC_FAST oscillator
  (17.5 MHz on the C6, 8.5 MHz on the H2) and asks ESP-IDF to keep it and the
  pad running in light sleep. At level 0 the timer is released, so RC_FAST
  can sleep again at night.
- `set_panel_sleep()` in lcd_power.h: puts a MIPI DCS panel (ST7789 and the
  like) driven by ESPHome's `mipi_spi` into its sleep mode (SLPIN) and back
  (SLPOUT). With the backlight off the controller keeps drawing a few mA
  otherwise.
- `set_status_led()` in lcd_power.h: sets a single WS2812-type RGB LED with the
  RMT channel enabled only for the frame. ESPHome's esp32_rmt_led_strip keeps
  its channel enabled, and the power lock that comes with it keeps the chip
  out of light sleep altogether.

All LEDC timers of the C6 and H2 share one clock source, so this output does
not mix with ESPHome's `ledc` output.
"""

CODEOWNERS = []
