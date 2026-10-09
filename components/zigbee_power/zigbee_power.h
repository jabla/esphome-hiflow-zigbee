#pragma once

// Power saving for ESPHome's zigbee component on the ESP32-C6; see __init__.py
// for the whole picture.

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_ZIGBEE)

#include <vector>

#include "driver/temperature_sensor.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/hal.h"
#include "ezbee/app_signals.h"

namespace esphome {
namespace zigbee_power {

class ZigbeePower : public Component {
 public:
  void set_poll_interval(uint32_t ms) { this->poll_interval_ms_ = ms; }
  void set_awake_after_boot(uint32_t ms) { this->awake_after_boot_ms_ = ms; }
  void set_loop_interval(uint32_t ms) { this->loop_interval_ms_ = ms; }
  void set_night_poll_interval(uint32_t ms) { this->night_poll_interval_ms_ = ms; }
  void set_night_loop_interval(uint32_t ms) { this->night_loop_interval_ms_ = ms; }
  void set_request_poll_window(uint32_t ms) { this->request_poll_window_ms_ = ms; }
  void set_wake_button(InternalGPIOPin *pin) { this->button_pin_ = pin; }
  void add_keep_pin(uint8_t pin) { this->keep_pins_.push_back(pin); }
  void add_on_press_callback(std::function<void()> &&callback) { this->press_callback_.add(std::move(callback)); }

  void setup() override;
  void loop() override;
  void dump_config() override;
  // Before ZigbeeComponent::setup() (DATA), which starts the Zigbee task: the
  // receiver mode and our signal handler must be in place before the stack runs.
  float get_setup_priority() const override { return setup_priority::BUS; }

  /// Milliseconds spent in light sleep since boot (0 without `sleepy`).
  uint32_t slept_ms() const;
  /// Light sleeps since boot, and those of them that powered the digital
  /// peripherals down (both 0 without `sleep_debug`).
  uint32_t sleeps() const;
  uint32_t sleeps_pd_top() const;
  /// Why the peripherals stay powered (`sleep_debug` only): bit 0 cpu, 1 clock,
  /// 2 peripherals, 3 modem domain allowed; and the retention modules that
  /// registered but never set their retention up (bit n = module n).
  uint32_t pd_checks() const;
  /// Die temperature in degrees Celsius (1 K steps, NAN on failure). Unlike
  /// ESPHome's internal_temperature, the sensor is set up with retention, so it
  /// does not keep the peripherals powered in light sleep. Not together with
  /// internal_temperature: the chip has one sensor.
  float chip_temperature();
  uint32_t pd_blockers() const;
  uint32_t poll_interval_ms() const { return this->poll_interval_ms_; }
  uint32_t night_poll_interval_ms() const { return this->night_poll_interval_ms_; }
  uint32_t loop_interval_ms() const { return this->loop_interval_ms_; }
  uint32_t night_loop_interval_ms() const { return this->night_loop_interval_ms_; }
  uint32_t request_poll_window_ms() const { return this->request_poll_window_ms_; }

 protected:
  static bool signal_handler_(const ezb_app_signal_t *app_signal);
  static void button_isr_(ZigbeePower *self);
  void setup_button_();
  void rejoin_if_pending_();
  void apply_keep_pins_();

  uint32_t poll_interval_ms_{0};
  temperature_sensor_handle_t tsens_{nullptr};
  bool tsens_failed_{false};
  uint32_t awake_after_boot_ms_{0};
  uint32_t loop_interval_ms_{0};  // 0 = ESPHome's own
  uint32_t night_poll_interval_ms_{0};
  uint32_t night_loop_interval_ms_{0};
  uint32_t request_poll_window_ms_{0};  // 0 = no fast poll after a request

  // The wake button: a level interrupt that waits for the other level each
  // time, so it fires once per change, also right after a wakeup; the presses
  // are counted there and handed to on_press from the main loop.
  InternalGPIOPin *button_pin_{nullptr};
  uint8_t button_gpio_{0};
  bool button_inverted_{false};
  volatile bool button_down_{false};
  volatile int64_t button_change_us_{0};
  volatile uint32_t presses_{0};
  uint32_t presses_seen_{0};
  uint32_t rejoin_checked_ms_{0};
  bool rejoin_leaving_{false};  // the local reset is under way
  uint32_t rejoin_leave_ms_{0};
  CallbackManager<void()> press_callback_;

  std::vector<uint8_t> keep_pins_;
};

class PressTrigger : public Trigger<> {
 public:
  explicit PressTrigger(ZigbeePower *parent) {
    parent->add_on_press_callback([this]() { this->trigger(); });
  }
};

/// While `on` (a firmware update, from the download to the restart): a
/// pending join with the other receiver mode waits, and the sleepy build polls
/// every FAST_POLL_MS; `false` goes back to the day or night interval (or
/// stays fast while requests come in, see request_poll_window).
/// Called on the Zigbee task or under its lock.
void set_updating(bool on);

#ifdef USE_ZIGBEE_POWER_SLEEPY

/// Night mode (the inverter is dark): poll and main loop at their night
/// intervals. Called from the main loop by whoever knows it is night.
void set_night(bool night);
#endif

}  // namespace zigbee_power
}  // namespace esphome

#endif
