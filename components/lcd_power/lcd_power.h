#pragma once

// Power saving for a small SPI panel next to a sleepy chip; see __init__.py
// for the whole picture.

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "driver/ledc.h"
#include "esp_pm.h"
#include "esphome/components/output/float_output.h"
#include "esphome/core/application.h"
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
// The ST7789 wants this long between SLPIN and SLPOUT.
static constexpr uint32_t PANEL_SLEEP_GAP_MS = 120;

namespace detail {
// mipi_spi keeps write_command_() protected; a pointer to it, named through a
// derived class, is the standard way to reach it without a patched driver.
template<typename Display> struct PanelAccess : Display {
  static void command(Display *display, uint8_t cmd) {
    void (Display::*write)(uint8_t) = &PanelAccess::write_command_;
    (display->*write)(cmd);
  }
};
// What set_panel_sleep() knows about the panel. Its address keys the timer.
struct PanelState {
  // millis_64(), the scheduler's clock, once the last SLPIN or SLPOUT had
  // settled. On millis() (the tick count, which can fall behind in light
  // sleep) the timer could find the gap a little short.
  uint64_t last_change{0};
  bool asleep{false};  // the mode the panel was put in then
  bool wanted{false};  // the mode the last call asked for
};
PanelState &panel_state();
}  // namespace detail

/// Sets a single WS2812-type RGB LED (bytes sent in R, G, B order). The LED
/// keeps its colour without a signal, so the RMT channel is enabled only for
/// the frame: ESPHome's esp32_rmt_led_strip keeps it enabled, and an enabled
/// RMT channel holds a CPU_FREQ_MAX lock that keeps the chip out of light
/// sleep for good. The first call sets the pin up; later calls keep it.
void set_status_led(uint8_t gpio, uint8_t r, uint8_t g, uint8_t b);

/// Puts the panel into its sleep mode (a few uA, the frame memory kept and
/// still writable) or wakes it. The ST7789 wants 120 ms between the two
/// commands. A change that comes sooner goes out from a timer once they have
/// passed, so the main loop never waits for it; a waking panel stays dark
/// until then, even with the backlight on. The 5 ms it wants after either
/// command before the next one are waited here. The state is kept once for
/// the program, so this is for a single panel.
template<typename Display> void set_panel_sleep(Display *display, bool sleep) {
  detail::PanelState &panel = detail::panel_state();
  panel.wanted = sleep;
  if (panel.asleep == sleep)
    return;
  const uint64_t since = millis_64() - panel.last_change;
  if (since < PANEL_SLEEP_GAP_MS) {
    // One timer: a newer change within the 120 ms replaces it. When it runs it
    // sends what was asked for last, or nothing if the panel is in that mode.
    App.scheduler.set_timeout(&panel, static_cast<uint32_t>(PANEL_SLEEP_GAP_MS - since),
                              [display]() { set_panel_sleep(display, detail::panel_state().wanted); });
    // ESPHome 2026.10 sees a timer set from a component's loop(), such as a
    // button's on_press, only at its next scheduler run: up to a loop interval
    // later. The wake makes that the next pass. 2026.11 no longer needs it.
    App.wake_loop_threadsafe();
    return;
  }
  detail::PanelAccess<Display>::command(display, sleep ? PANEL_SLPIN : PANEL_SLPOUT);
  delay(6);  // at least 5 ms: delay(5) ends on the 5th tick, 4 to 5 ms on
  panel.last_change = millis_64();
  panel.asleep = sleep;
}

}  // namespace lcd_power
}  // namespace esphome

#endif  // USE_ESP32
