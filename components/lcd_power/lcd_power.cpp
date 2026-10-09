#include "lcd_power.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cinttypes>
#include <cmath>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_clk_tree.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esphome/core/log.h"

namespace esphome {
namespace lcd_power {

static const char *const TAG = "lcd_power";

namespace detail {
uint32_t &panel_last_change() {
  static uint32_t value = 0;
  return value;
}
bool &panel_asleep() {
  static bool value = false;
  return value;
}
}  // namespace detail

void set_status_led(uint8_t gpio, uint8_t r, uint8_t g, uint8_t b) {
  static rmt_channel_handle_t channel = nullptr;
  static rmt_encoder_handle_t encoder = nullptr;
  if (channel == nullptr) {
    rmt_tx_channel_config_t config = {};
    config.gpio_num = (gpio_num_t) gpio;
    config.clk_src = RMT_CLK_SRC_DEFAULT;
    config.resolution_hz = 10000000;  // 0.1 us ticks
    config.mem_block_symbols = 48;
    config.trans_queue_depth = 1;
    if (rmt_new_tx_channel(&config, &channel) != ESP_OK) {
      ESP_LOGE(TAG, "No RMT channel for the LED");
      channel = nullptr;
      return;
    }
    // WS2812: a 0 is 0.4 us high + 0.9 us low, a 1 is 0.8 us high + 0.5 us low.
    rmt_bytes_encoder_config_t bits = {};
    bits.bit0.duration0 = 4;
    bits.bit0.level0 = 1;
    bits.bit0.duration1 = 9;
    bits.bit0.level1 = 0;
    bits.bit1.duration0 = 8;
    bits.bit1.level0 = 1;
    bits.bit1.duration1 = 5;
    bits.bit1.level1 = 0;
    bits.flags.msb_first = 1;
    if (rmt_new_bytes_encoder(&bits, &encoder) != ESP_OK) {
      ESP_LOGE(TAG, "No RMT encoder for the LED");
      rmt_del_channel(channel);
      channel = nullptr;
      return;
    }
  }
  const uint8_t frame[3] = {r, g, b};
  rmt_transmit_config_t transmit = {};
  if (rmt_enable(channel) != ESP_OK)
    return;
  if (rmt_transmit(channel, encoder, frame, sizeof(frame), &transmit) == ESP_OK)
    rmt_tx_wait_all_done(channel, 100);
  rmt_disable(channel);
}

void BacklightOutput::setup() {
  // Dark until the first level arrives.
  this->pin_->setup();
  this->pin_->digital_write(false);
#ifdef CONFIG_PM_ENABLE
  esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "lcd_power", &this->awake_lock_);
#endif
}

void BacklightOutput::dump_config() {
  ESP_LOGCONFIG(TAG, "Backlight output:");
  LOG_PIN("  Pin: ", this->pin_);
  ESP_LOGCONFIG(TAG, "  Frequency: %" PRIu32 " Hz", this->frequency_);
  if (this->resolution_ != 0) {
    ESP_LOGCONFIG(TAG, "  Duty resolution: %" PRIu32 " bits, clock %s", this->resolution_,
                  this->fallback_ ? "PLL (no light sleep while lit)" : "RC_FAST (runs in light sleep)");
  }
}

bool BacklightOutput::start_() {
  uint32_t rc_fast_hz = 0;
  esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_RC_FAST, ESP_CLK_TREE_SRC_FREQ_PRECISION_APPROX, &rc_fast_hz);
  ledc_timer_config_t timer = {};
  timer.speed_mode = SPEED_MODE;
  timer.timer_num = TIMER;
  timer.freq_hz = this->frequency_;
  timer.clk_cfg = LEDC_USE_RC_FAST_CLK;
  timer.duty_resolution = (ledc_timer_bit_t) ledc_find_suitable_duty_resolution(rc_fast_hz, this->frequency_);
  this->fallback_ = timer.duty_resolution == 0 || ledc_timer_config(&timer) != ESP_OK;
  if (this->fallback_) {
    timer.clk_cfg = LEDC_AUTO_CLK;
    timer.duty_resolution = (ledc_timer_bit_t) ledc_find_suitable_duty_resolution(80000000, this->frequency_);
    if (ledc_timer_config(&timer) != ESP_OK) {
      ESP_LOGE(TAG, "No LEDC timer for %" PRIu32 " Hz", this->frequency_);
      return false;
    }
    ESP_LOGW(TAG, "RC_FAST cannot drive %" PRIu32 " Hz; the chip stays awake while lit", this->frequency_);
  }
  this->resolution_ = timer.duty_resolution;

  ledc_channel_config_t channel = {};
  channel.gpio_num = this->pin_->get_pin();
  channel.speed_mode = SPEED_MODE;
  channel.channel = CHANNEL;
  channel.timer_sel = TIMER;
  channel.duty = 0;
  channel.sleep_mode = this->fallback_ ? LEDC_SLEEP_MODE_NO_ALIVE_NO_PD : LEDC_SLEEP_MODE_KEEP_ALIVE;
  channel.flags.output_invert = this->pin_->is_inverted();
  if (ledc_channel_config(&channel) != ESP_OK) {
    ESP_LOGE(TAG, "LEDC channel setup failed");
    return false;
  }
  this->running_ = true;
  this->keep_awake_(this->fallback_);
  return true;
}

void BacklightOutput::stop_() {
  // Output at its idle level (dark), then release the timer, and with it the
  // request to keep RC_FAST running in light sleep. The stop only takes at
  // the next PWM period: a timer paused before that leaves the pin where it
  // was, lit in as many stops as the duty share. The idle level is the
  // signal's, the pin's inversion applies on top of it.
  ledc_stop(SPEED_MODE, CHANNEL, 0);
  delayMicroseconds(2000000 / this->frequency_ + 1);
  ledc_timer_pause(SPEED_MODE, TIMER);
  ledc_timer_config_t timer = {};
  timer.speed_mode = SPEED_MODE;
  timer.timer_num = TIMER;
  timer.deconfigure = true;
  ledc_timer_config(&timer);
  // The next start configures the channel again, which reserves the pin
  // again and would warn of a conflict with itself.
  esp_gpio_revoke(BIT64(this->pin_->get_pin()));
  this->running_ = false;
  this->keep_awake_(false);
}

void BacklightOutput::write_state(float state) {
  if (state <= 0.0f) {
    if (this->running_)
      this->stop_();
    return;
  }
  if (!this->running_ && !this->start_())
    return;
  const uint32_t max_duty = (1u << this->resolution_) - 1;
  const uint32_t duty = std::min<uint32_t>(max_duty, (uint32_t) std::lround(state * max_duty));
  ledc_set_duty(SPEED_MODE, CHANNEL, duty);
  ledc_update_duty(SPEED_MODE, CHANNEL);
}

void BacklightOutput::keep_awake_(bool on) {
#ifdef CONFIG_PM_ENABLE
  if (this->awake_lock_ == nullptr || this->awake_held_ == on)
    return;
  if (on)
    esp_pm_lock_acquire(this->awake_lock_);
  else
    esp_pm_lock_release(this->awake_lock_);
  this->awake_held_ = on;
#endif
}

}  // namespace lcd_power
}  // namespace esphome

#endif  // USE_ESP32
