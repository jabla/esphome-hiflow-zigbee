#pragma once

// Power saving for a small SPI panel next to a sleepy chip; see __init__.py
// for the whole picture.

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "driver/ledc.h"
#include "esp_pm.h"
#include "esphome/components/output/float_output.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace lcd_power {

/// Backlight PWM that keeps running while the chip light-sleeps.
class BacklightOutput : public output::FloatOutput, public Component {
 public:
  void set_pin(InternalGPIOPin *pin) { this->pin_ = pin; }
  void set_frequency(uint32_t hz) { this->frequency_ = hz; }

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  /// True while the PWM runs from RC_FAST, so the chip may sleep while lit.
  bool keeps_running_in_sleep() const { return this->running_ && !this->fallback_; }
  /// The LEDC channel and duty resolution in use (for bench tests).
  ledc_channel_t channel() const { return CHANNEL; }
  uint32_t resolution_bits() const { return this->resolution_; }

  static constexpr ledc_mode_t SPEED_MODE = LEDC_LOW_SPEED_MODE;
  static constexpr ledc_timer_t TIMER = LEDC_TIMER_0;
  static constexpr ledc_channel_t CHANNEL = LEDC_CHANNEL_0;

 protected:
  void write_state(float state) override;
  bool start_();
  void stop_();
  void keep_awake_(bool on);

  InternalGPIOPin *pin_{nullptr};
  uint32_t frequency_{20000};
  uint32_t resolution_{0};
  bool running_{false};
  // RC_FAST could not drive the timer: PLL clock, and no light sleep while lit.
  bool fallback_{false};
#ifdef CONFIG_PM_ENABLE
  esp_pm_lock_handle_t awake_lock_{nullptr};
  bool awake_held_{false};
#endif
};

// MIPI DCS commands, the same on the ST7789 and its relatives.
static constexpr uint8_t PANEL_SLPIN = 0x10;
static constexpr uint8_t PANEL_SLPOUT = 0x11;

namespace detail {
// mipi_spi keeps write_command_() protected; a pointer to it, named through a
// derived class, is the standard way to reach it without a patched driver.
template<typename Display> struct PanelAccess : Display {
  static void command(Display *display, uint8_t cmd) {
    void (Display::*write)(uint8_t) = &PanelAccess::write_command_;
    (display->*write)(cmd);
  }
};
uint32_t &panel_last_change();
bool &panel_asleep();
}  // namespace detail

/// Sets a single WS2812-type RGB LED (bytes sent in R, G, B order). The LED
/// keeps its colour without a signal, so the RMT channel is enabled only for
/// the frame: ESPHome's esp32_rmt_led_strip keeps it enabled, and an enabled
/// RMT channel holds a CPU_FREQ_MAX lock that keeps the chip out of light
/// sleep for good. The first call sets the pin up; later calls keep it.
void set_status_led(uint8_t gpio, uint8_t r, uint8_t g, uint8_t b);

/// Puts the panel into its sleep mode (a few uA, the frame memory kept and
/// still writable) or wakes it. The ST7789 wants 120 ms between the two
/// commands and 5 ms after SLPOUT before the next one; both waits happen here.
/// The state is kept once for the program, so this is for a single panel.
template<typename Display> void set_panel_sleep(Display *display, bool sleep) {
  if (detail::panel_asleep() == sleep)
    return;
  const uint32_t since = millis() - detail::panel_last_change();
  if (since < 120)
    delay(120 - since);
  detail::PanelAccess<Display>::command(display, sleep ? PANEL_SLPIN : PANEL_SLPOUT);
  delay(5);
  detail::panel_last_change() = millis();
  detail::panel_asleep() = sleep;
}

}  // namespace lcd_power
}  // namespace esphome

#endif  // USE_ESP32
