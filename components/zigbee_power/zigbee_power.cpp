#include "zigbee_power.h"

#if defined(USE_ESP32) && defined(USE_ZIGBEE)

#include <atomic>

#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_zigbee.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "esp_ota_ops.h"
#include "sdkconfig.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "ezbee/aps.h"
#include "ezbee/bdb.h"
#include "ezbee/nwk.h"

#ifdef USE_ZIGBEE_POWER_SLEEPY
#include "driver/usb_serial_jtag.h"
#include "esp_pm.h"
#include "phy_guard.h"
#endif
#ifdef USE_ZIGBEE_POWER_TIMER_GUARD
#include "timer_guard.h"
#endif
#ifdef USE_ZIGBEE_POWER_SLEEP_DEBUG
#include "esp_private/esp_pmu.h"
#include "esp_private/esp_sleep_internal.h"
#include "esp_private/sleep_clock.h"
#include "esp_private/sleep_cpu.h"
#include "esp_private/sleep_modem.h"
#include "esp_private/sleep_retention.h"
#include "esp_private/sleep_sys_periph.h"
#endif

namespace esphome {
namespace zigbee_power {

static const char *const TAG = "zigbee_power";

static ZigbeePower *s_power = nullptr;
// Written only by the light-sleep exit callback (idle task, interrupts off);
// an aligned 32-bit load is atomic on the C6, so the main loop reads it as is.
static volatile uint32_t s_slept_ms = 0;
static uint32_t s_slept_rest_us = 0;

// The receiver mode this image wants: off for a sleepy end device.
#ifdef USE_ZIGBEE_POWER_SLEEPY
static const bool WANT_RX_ON = false;
#else
static const bool WANT_RX_ON = true;
#endif
// The node is in a network it joined with the other receiver mode: the parent
// (and the coordinator's interview) still treat it that way, so it keeps that
// mode until it leaves and joins again (see loop()). Until then it polls
// every REJOIN_POLL_MS, in either mode.
static std::atomic<bool> s_rejoin_pending{false};
static const uint32_t REJOIN_POLL_MS = 1000;
// Two ways the ESP32-H2 got stuck on the way, after a USB flash: the stack's
// Device Reboot failed with "no network" for good, so the node never was in
// the network to leave it; or the local reset (the leave) never finished.
// Either way it then clears the stack's data itself and starts over as a new
// node: after failed reboots for a while, or this long after the leave.
// The reboot itself needs no network (with the coordinator unplugged it
// succeeded and the node left at once), so failed reboots are rare; it still
// takes both, this many failures in a row and the first of them this long
// ago, before it gives up the old network.
static const uint8_t REJOIN_REBOOT_FAILURES = 3;
static const uint32_t REJOIN_STUCK_MS = 300000;
static const uint32_t REJOIN_LEAVE_TIMEOUT_MS = 10000;
static std::atomic<uint8_t> s_reboot_failures{0};
static std::atomic<uint32_t> s_reboot_failing_since_ms{0};
#ifdef CONFIG_IDF_TARGET_ESP32H2
// In about one boot in five the ESP32-H2 does not find the network for the
// whole boot: every join try ends after a full scan (about 17 s) with "no
// network", while a C6 next to it joins at once; a restart clears it more
// often than not. A node that is not in a network restarts after this many
// failed tries in a row, at the earliest this long after the boot. A
// coordinator that does not permit joins fails them the same way, so an H2
// waiting to be paired restarts every two minutes.
static const uint8_t JOIN_RESTART_FAILURES = 3;
static const uint32_t JOIN_RESTART_MIN_UPTIME_MS = 120000;
static std::atomic<uint8_t> s_join_failures{0};
static std::atomic<bool> s_join_restart{false};
#endif
// A firmware update runs (set_updating): a pending join waits for its end, as
// the leave would restart the chip in the middle of it.
static std::atomic<bool> s_updating{false};
// The stack does not keep the receiver mode of the join (it starts with the
// receiver on), so this component does: 1 on, 0 off. A node without the
// record joined with an image from before it, receiver on.
static ESPPreferenceObject s_join_pref;
static bool s_joined_rx_on = true;
static std::atomic<bool> s_joined_now{false};

static volatile uint32_t s_sleeps = 0;
static volatile uint32_t s_sleeps_pd_top = 0;
#ifdef USE_ZIGBEE_POWER_SLEEP_DEBUG
static esp_sleep_context_t s_sleep_ctx;
#endif

#ifdef USE_ZIGBEE_POWER_SLEEPY
// Poll interval during a firmware download and after a request from the
// network (request_poll_window): every block or request waits at the parent
// until the next poll, so the poll sets the speed.
// The chip still light-sleeps between polls: on the bench that cost no
// measurable speed (393 against 383 B/s) and saved a third of the awake time.
// 50 ms: about 380 B/s with zigpy's 50 byte blocks on the bench, 120 B/s at
// the stack's own fast poll of 200 ms. A parent that wants End Device Timeout
// Requests as keepalive gets one per poll on top, while it lasts: a few
// minutes per download, request_poll_window (10 s) per burst of requests.
static const uint32_t FAST_POLL_MS = 50;

// The poll interval in force: a firmware update or a request first, then a
// pending join with the other receiver mode (1 s, so the parent reaches us
// whichever mode it assumes), then night, then day.
static std::atomic<bool> s_night{false};
// After a request from the network (the coordinator's interview, a
// reconfigure, a read or a write) the next one usually follows as soon as the
// answer is out, and waits at the parent until our next poll: a few hundred
// of them made an interview take 7 to 14 minutes at a 3 s poll. So poll fast
// until no request came for request_poll_window.
static std::atomic<bool> s_request_fast{false};
static std::atomic<uint32_t> s_request_until_ms{0};
static uint32_t current_poll_ms() {
  if (s_updating.load() || s_request_fast.load())
    return FAST_POLL_MS;
  if (s_rejoin_pending.load())
    return REJOIN_POLL_MS;
  return s_night.load() ? s_power->night_poll_interval_ms() : s_power->poll_interval_ms();
}

// A request to us, not an answer or a report: a ZDP request (cluster below
// 0x8000), or a ZCL command sent to a server other than a Default Response
// (the coordinator's answer to our reports, which must not keep us polling).
// Broadcasts and group frames do not count: address lookups and the like go
// to the whole network, and a busy one would keep us polling fast.
static bool is_request(const ezb_apsde_data_ind_t *ind) {
  if (ind->dst_address.addr_mode == EZB_ADDR_MODE_GROUP ||
      (ind->dst_address.addr_mode == EZB_ADDR_MODE_SHORT && ind->dst_address.u.short_addr >= 0xFFF8))
    return false;
  if (ind->profile_id == 0x0000)
    return ind->cluster_id < 0x8000;
  if (ind->asdu == nullptr || ind->asdu_length < 3)
    return false;
  const uint8_t fc = ind->asdu[0];
  if (fc & 0x08)  // server to client
    return false;
  const bool global = (fc & 0x03) == 0x00;
  const size_t cmd_at = (fc & 0x04) ? 4 : 2;  // after a manufacturer code
  if (ind->asdu_length <= cmd_at)
    return false;
  return !(global && ind->asdu[cmd_at] == 0x0B);
}

// On the Zigbee task, for every incoming frame; never consumes one.
static bool on_aps_indication(const ezb_apsde_data_ind_t *ind) {
  if (s_power == nullptr || !is_request(ind))
    return false;
  s_request_until_ms.store(millis() + s_power->request_poll_window_ms());
  if (!s_request_fast.exchange(true)) {
    ezb_nwk_set_keepalive_interval(current_poll_ms());
    s_power->enable_loop_soon_any_context();
  }
  return false;
}

void set_night(bool night) {
  if (s_power == nullptr || s_night.load() == night)
    return;
  s_night.store(night);
  // Called from the main loop: the stack wants its lock.
  esp_zigbee_lock_acquire(portMAX_DELAY);
  ezb_nwk_set_keepalive_interval(current_poll_ms());
  esp_zigbee_lock_release();
  App.set_loop_interval(night ? s_power->night_loop_interval_ms() : s_power->loop_interval_ms());
  ESP_LOGI(TAG, "%s: poll every %u ms, main loop every %u ms", night ? "Night" : "Day",
           (unsigned) current_poll_ms(), (unsigned) App.get_loop_interval());
}

// ESP-IDF calls this after every attempt, also one the hardware rejected (a
// pending BLE wakeup); without sleep_debug those count as slept, a bias of
// well under a percentage point.
static esp_err_t IRAM_ATTR on_light_sleep_exit(int64_t slept_us, void *arg) {
  if (slept_us <= 0)
    return ESP_OK;
#ifdef USE_ZIGBEE_POWER_SLEEP_DEBUG
  if (s_sleep_ctx.sleep_request_result != ESP_OK)
    return ESP_OK;
  s_sleeps = s_sleeps + 1;
  if (s_sleep_ctx.sleep_flags & PMU_SLEEP_PD_TOP)
    s_sleeps_pd_top = s_sleeps_pd_top + 1;
#endif
  const uint64_t us = (uint64_t) slept_us + s_slept_rest_us;
  s_slept_ms += (uint32_t) (us / 1000);
  s_slept_rest_us = (uint32_t) (us % 1000);
  return ESP_OK;
}
#endif

void set_updating(bool on) {
  if (s_power == nullptr || s_updating.exchange(on) == on)
    return;
#ifdef USE_ZIGBEE_POWER_SLEEPY
  ezb_nwk_set_keepalive_interval(current_poll_ms());
#endif
  // A pending join goes on once the update is over.
  if (!on)
    s_power->enable_loop_soon_any_context();
}

// A change of the button's level within this time of the last one is bounce:
// a press counts only after the line was quiet that long.
static const int64_t BUTTON_QUIET_US = 30000;

// Runs for every change of the button's level (see button_isr_ in the header).
void IRAM_ATTR ZigbeePower::button_isr_(ZigbeePower *self) {
  const int level = gpio_ll_get_level(&GPIO, self->button_gpio_);
  gpio_ll_set_intr_type(&GPIO, self->button_gpio_, level ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
  const bool down = (level != 0) != self->button_inverted_;
  if (down == self->button_down_)
    return;
  self->button_down_ = down;
  const int64_t now = esp_timer_get_time();
  const bool quiet = now - self->button_change_us_ >= BUTTON_QUIET_US;
  self->button_change_us_ = now;
  if (down && quiet) {
    self->presses_ = self->presses_ + 1;
    self->enable_loop_soon_any_context();
  }
}

void ZigbeePower::setup_button_() {
  InternalGPIOPin *pin = this->button_pin_;
  pin->setup();
  this->button_gpio_ = pin->get_pin();
  this->button_inverted_ = pin->is_inverted();
  const int level = gpio_get_level((gpio_num_t) this->button_gpio_);
  this->button_down_ = (level != 0) != this->button_inverted_;
  // ESPHome enables the interrupt before it adds the handler: attached with a
  // level type, a level already present fires without a handler, which
  // cannot clear it (an interrupt watchdog reset). An edge first, then the
  // level that waits for the change.
  pin->attach_interrupt(&ZigbeePower::button_isr_, this, gpio::INTERRUPT_ANY_EDGE);
  gpio_set_intr_type((gpio_num_t) this->button_gpio_, level ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
#ifdef USE_ZIGBEE_POWER_SLEEPY
  // The same level wakes the chip from light sleep. This keeps the pin's
  // pull-up in sleep too, so the idle line does not float into a wakeup.
  gpio_wakeup_enable((gpio_num_t) this->button_gpio_, level ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();
#endif
}

// A node that joined with the other receiver mode leaves the network and
// joins again, so the parent learns the new mode from the join. Only once the
// running image is confirmed: the leave restarts the chip, and a restart
// before that would roll a new image back. The reset clears only the Zigbee
// datasets; joining again needs the coordinator to permit joins.
void ZigbeePower::rejoin_if_pending_() {
  if (!s_rejoin_pending.load())
    return;
  // A new image that is not yet valid stays in its network: it needs the OTA
  // server to become valid, or rolls back after verify_timeout.
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY)
    return;
  esp_zigbee_lock_acquire(portMAX_DELAY);
  // A download that started with the offer that made the image valid (see
  // zigbee_ota) goes first: the leave would restart the chip in the middle.
  if (s_updating.load()) {
    esp_zigbee_lock_release();
    return;
  }
  // The local reset ends in a restart (ESPHome's leave handler), so the
  // pending join only ends with it. The fallback below clears the stack's
  // data and restarts the chip, which then joins as a new node; unlike the
  // local reset it also works off the network, it only tells the parent
  // nothing.
  if (this->rejoin_leaving_) {
    if (millis() - this->rejoin_leave_ms_ >= REJOIN_LEAVE_TIMEOUT_MS) {
      ESP_LOGW(TAG, "The leave did not finish, starting over");
      esp_zigbee_factory_reset();
    }
  } else if (ezb_bdb_dev_joined()) {
    ESP_LOGW(TAG, "Leaving the network to join again with the receiver %s while idle", WANT_RX_ON ? "on" : "off");
    this->rejoin_leaving_ = true;
    this->rejoin_leave_ms_ = millis();
    ezb_bdb_reset_via_local_action();
  } else if (s_reboot_failures.load() >= REJOIN_REBOOT_FAILURES &&
             millis() - s_reboot_failing_since_ms.load() >= REJOIN_STUCK_MS) {
    ESP_LOGW(TAG, "Cannot get back into the network, starting over");
    esp_zigbee_factory_reset();
  }
  esp_zigbee_lock_release();
}

void ZigbeePower::loop() {
#ifdef USE_ZIGBEE_POWER_TIMER_GUARD
  timer_guard_log();
#endif
#ifdef USE_ZIGBEE_POWER_SLEEPY
  phy_guard_loop();
  // Back to the normal poll once the requests stopped. Checked again under
  // the stack's lock, where a new request cannot come in between.
  if (s_request_fast.load() && static_cast<int32_t>(millis() - s_request_until_ms.load()) >= 0 &&
      esp_zigbee_lock_acquire(10 / portTICK_PERIOD_MS)) {
    if (static_cast<int32_t>(millis() - s_request_until_ms.load()) >= 0) {
      s_request_fast.store(false);
      ezb_nwk_set_keepalive_interval(current_poll_ms());
    }
    esp_zigbee_lock_release();
  }
#endif
#ifdef CONFIG_IDF_TARGET_ESP32H2
  if (s_join_restart.exchange(false)) {
    const uint32_t now = millis();
    const uint32_t wait = now < JOIN_RESTART_MIN_UPTIME_MS ? JOIN_RESTART_MIN_UPTIME_MS - now : 0;
    ESP_LOGW(TAG, "No network after %u join tries, restarting in %u s", (unsigned) JOIN_RESTART_FAILURES,
             (unsigned) (wait / 1000));
    this->set_timeout("join_restart", wait, []() { App.safe_reboot(); });
  }
#endif
  if (s_joined_now.exchange(false)) {
#ifdef CONFIG_IDF_TARGET_ESP32H2
    // Joined while it waited for the restart.
    this->cancel_timeout("join_restart");
#endif
    const uint8_t rx_on = WANT_RX_ON ? 1 : 0;
    s_join_pref.save(&rx_on);
    global_preferences->sync();
    ESP_LOGI(TAG, "Joined with the receiver %s while idle", WANT_RX_ON ? "on" : "off");
  }
  const bool rejoin_pending = s_rejoin_pending.load();
  if (rejoin_pending) {
    // Checked once a second until it is done; the loop stays on for it.
    const uint32_t now = millis();
    if (now - this->rejoin_checked_ms_ >= 1000) {
      this->rejoin_checked_ms_ = now;
      this->rejoin_if_pending_();
    }
  }
  // Every press counted since the last round: several come in while the
  // loop waits its interval.
  const uint32_t presses = this->presses_;
  while (this->presses_seen_ != presses) {
    this->presses_seen_++;
    this->press_callback_.call();
  }
#ifdef USE_ZIGBEE_POWER_SLEEPY
  if (s_request_fast.load())
    return;
#endif
#ifndef USE_ZIGBEE_POWER_TIMER_GUARD
  // With timer_guard the loop stays on for its log: the main loop runs every
  // loop_interval anyway, so that costs no wakeup.
  if (!rejoin_pending)
    this->disable_loop();
#endif
}

// In light sleep ESP-IDF isolates every pad (no output, no pull): these keep
// their normal setting instead, a display's control lines and a backlight
// that must hold their level while the chip sleeps.
void ZigbeePower::apply_keep_pins_() {
  for (uint8_t pin : this->keep_pins_)
    gpio_sleep_sel_dis((gpio_num_t) pin);
}

uint32_t ZigbeePower::slept_ms() const { return s_slept_ms; }
uint32_t ZigbeePower::sleeps() const { return s_sleeps; }
uint32_t ZigbeePower::sleeps_pd_top() const { return s_sleeps_pd_top; }

float ZigbeePower::chip_temperature() {
  temperature_sensor_handle_t handle = this->tsens_;
  if (handle == nullptr) {
    if (this->tsens_failed_)
      return NAN;
    temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    config.flags.allow_pd = 1;
    if (temperature_sensor_install(&config, &handle) != ESP_OK) {
      // The driver allows one instance: ESPHome's internal_temperature
      // takes the sensor if it is configured too.
      ESP_LOGE(TAG, "No chip temperature: the sensor is in use (internal_temperature?)");
      this->tsens_failed_ = true;
      return NAN;
    }
    if (temperature_sensor_enable(handle) != ESP_OK) {
      ESP_LOGE(TAG, "No chip temperature: the sensor does not start");
      temperature_sensor_uninstall(handle);
      this->tsens_failed_ = true;
      return NAN;
    }
    this->tsens_ = handle;
  }
  float celsius = NAN;
  if (temperature_sensor_get_celsius(handle, &celsius) != ESP_OK)
    return NAN;
  return celsius;
}

uint32_t ZigbeePower::pd_checks() const {
#ifdef USE_ZIGBEE_POWER_SLEEP_DEBUG
  return (cpu_domain_pd_allowed() ? 1 : 0) | (clock_domain_pd_allowed() ? 2 : 0) |
         (peripheral_domain_pd_allowed() ? 4 : 0) | (modem_domain_pd_allowed() ? 8 : 0);
#else
  return 0;
#endif
}

uint32_t ZigbeePower::pd_blockers() const {
#ifdef USE_ZIGBEE_POWER_SLEEP_DEBUG
  return sleep_retention_get_inited_modules().bitmap[0] & ~sleep_retention_get_created_modules().bitmap[0];
#else
  return 0;
#endif
}

void ZigbeePower::setup() {
  s_power = this;
#ifdef USE_ZIGBEE_POWER_SLEEPY
  phy_guard_init(this);
#endif
  s_join_pref = global_preferences->make_preference<uint8_t>(fnv1_hash("zigbee_power_join_rx"));
  uint8_t joined_rx_on;
  if (s_join_pref.load(&joined_rx_on))
    s_joined_rx_on = joined_rx_on != 0;

  esp_zigbee_lock_acquire(portMAX_DELAY);
  // esp-zigbee-lib 2.0.4 keeps 3 signal handlers: its own, this one and
  // ESPHome's (which comes later and fails without a free slot).
  if (ezb_app_signal_add_handler(ZigbeePower::signal_handler_) != EZB_ERR_NONE) {
    esp_zigbee_lock_release();
    ESP_LOGE(TAG, "Could not add the signal handler");
    this->mark_failed();
    return;
  }
#ifdef USE_ZIGBEE_POWER_SLEEPY
  ezb_nwk_set_keepalive_interval(this->poll_interval_ms_);
  // The stack has one slot for this hook: a later registration elsewhere
  // would replace ours, and the fast poll after requests would stop.
  if (this->request_poll_window_ms_ > 0)
    ezb_apsde_data_indication_handler_register(on_aps_indication);
#endif
  esp_zigbee_lock_release();

#ifdef USE_ZIGBEE_POWER_SLEEPY
  esp_pm_config_t pm = {
      .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
      .min_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
      .light_sleep_enable = true,
  };
  esp_err_t err = esp_pm_configure(&pm);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Power management: %s", esp_err_to_name(err));
  }
#ifdef USE_ZIGBEE_POWER_SLEEP_DEBUG
  esp_sleep_set_sleep_context(&s_sleep_ctx);
  // What keeps the peripherals powered in light sleep: the four domain checks
  // of ESP-IDF, and the retention modules that registered (inited) but never
  // got their retention set up (created). Logged while USB is still up.
  this->set_interval("pd_diag", 10000, [this]() {
    ESP_LOGW(TAG, "pd: domains allowed %" PRIx32 ", blocking modules %08" PRIx32 ", last sleep flags %08" PRIx32,
             this->pd_checks(), this->pd_blockers(), s_sleep_ctx.sleep_flags);
  });
#endif
  static esp_pm_sleep_cbs_register_config_t cbs = {};
  cbs.exit_cb = on_light_sleep_exit;
  err = esp_pm_light_sleep_register_cbs(&cbs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Light-sleep callback: %s", esp_err_to_name(err));
  }
  // Awake for a while after a start on USB: it stays up for logs and a
  // flash. On a battery (no USB host) the chip sleeps at once.
  if (this->awake_after_boot_ms_ > 0 && usb_serial_jtag_is_connected()) {
    static esp_pm_lock_handle_t awake = nullptr;
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "awake_after_boot", &awake) == ESP_OK) {
      esp_pm_lock_acquire(awake);
      this->set_timeout("awake", this->awake_after_boot_ms_, []() {
        ESP_LOGI(TAG, "Light sleep from now on");
        esp_pm_lock_release(awake);
      });
    }
  }
#endif
  // ESPHome's main loop runs the components every 16 ms by default and wakes
  // the chip for it; scheduled work still runs on time.
  if (this->loop_interval_ms_ > 0)
    App.set_loop_interval(this->loop_interval_ms_);

  if (this->button_pin_ != nullptr)
    this->setup_button_();
  // Again once every component is set up, in case one set its pins up anew.
  this->apply_keep_pins_();
  this->defer([this]() { this->apply_keep_pins_(); });
}

bool ZigbeePower::signal_handler_(const ezb_app_signal_t *app_signal) {
  if (s_power == nullptr)
    return false;
  const ezb_app_signal_type_t type = ezb_app_signal_get_type(app_signal);
  switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
      // The stack restored the network state from flash by now (not the
      // receiver mode of the join, see s_joined_rx_on). A node joined in the
      // other mode joins again (rejoin_if_pending_); until then its receiver is
      // on and it polls every second, so the parent reaches it whichever
      // mode it assumes, directly or from its buffer. A new node joins with
      // ours.
      if (!ezb_bdb_is_factory_new() && s_joined_rx_on != WANT_RX_ON) {
        ESP_LOGW(TAG, "Joined with the receiver %s while idle, this image wants it %s: joining again soon",
                 s_joined_rx_on ? "on" : "off", WANT_RX_ON ? "on" : "off");
        s_rejoin_pending.store(true);
        ezb_nwk_set_rx_on_when_idle(true);
        ezb_nwk_set_keepalive_interval(REJOIN_POLL_MS);
        s_power->enable_loop_soon_any_context();
      } else {
        ezb_nwk_set_rx_on_when_idle(WANT_RX_ON);
      }
      break;
    case EZB_BDB_SIGNAL_DEVICE_REBOOT:
      // ESPHome retries a failed one every second, forever.
      if (*((ezb_bdb_comm_status_t *) ezb_app_signal_get_params(app_signal)) == EZB_BDB_STATUS_SUCCESS) {
        s_reboot_failures.store(0);
      } else if (s_rejoin_pending.load()) {
        if (s_reboot_failures.load() == 0)
          s_reboot_failing_since_ms.store(millis());
        if (s_reboot_failures.load() < REJOIN_REBOOT_FAILURES)
          s_reboot_failures.fetch_add(1);
        s_power->enable_loop_soon_any_context();
      }
      break;
    case EZB_BDB_SIGNAL_STEERING:
      // A new join: it used the receiver mode set at the start.
      if (*((ezb_bdb_comm_status_t *) ezb_app_signal_get_params(app_signal)) == EZB_BDB_STATUS_SUCCESS) {
        s_joined_now.store(true);
        s_power->enable_loop_soon_any_context();
      }
#ifdef CONFIG_IDF_TARGET_ESP32H2
      else if (s_join_failures.fetch_add(1) + 1 == JOIN_RESTART_FAILURES) {
        s_join_restart.store(true);
        s_power->enable_loop_soon_any_context();
      }
#endif
      break;
    default:
      break;
  }
  // Never consume a signal: ESPHome's handler needs every one of them.
  return false;
}

void ZigbeePower::dump_config() {
  ESP_LOGCONFIG(TAG, "Zigbee power:");
#ifdef USE_ZIGBEE_POWER_SLEEPY
  ESP_LOGCONFIG(TAG,
                "  Sleepy end device, poll every %u ms\n"
                "  Light sleep at %d MHz, awake for %u s after boot\n"
                "  Main loop every %u ms",
                (unsigned) this->poll_interval_ms_, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
                (unsigned) (this->awake_after_boot_ms_ / 1000), (unsigned) App.get_loop_interval());
  if (this->request_poll_window_ms_ > 0) {
    ESP_LOGCONFIG(TAG, "  Poll every %u ms for %u ms after a request", (unsigned) FAST_POLL_MS,
                  (unsigned) this->request_poll_window_ms_);
  }
#endif
#ifdef USE_ZIGBEE_POWER_TIMER_GUARD
  timer_guard_dump_config();
#endif
}

}  // namespace zigbee_power
}  // namespace esphome

#endif
