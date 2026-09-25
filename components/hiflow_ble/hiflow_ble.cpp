#include "hiflow_ble.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef USE_ESP32

#include <esp_gattc_api.h>
#include <esp_timer.h>

namespace esphome {
namespace hiflow_ble {

static const char *const TAG = "hiflow_ble";

// Flash records.
static const uint16_t TIME_PREF_MAGIC = 0x4846;  // 'HF'
static const uint16_t TIME_PREF_VERSION = 2;
static const char *const TIME_PREF_NAME = "hiflow_time_v2";
static const int64_t TIME_SAVE_INTERVAL_MS = 300000;  // every 5 minutes
static const int64_t STATUS_REFRESH_MS = 300000;      // republish the status
static const int64_t LINK_UP_FALLBACK_MS = 3000;      // if no CCCD write is confirmed

// ---------------------------------------------------------------------------
// trampolines: the session core is C and calls back through a context pointer
// ---------------------------------------------------------------------------

static int cb_send(void *ctx, const uint8_t *frame, size_t len) {
  return static_cast<HiflowBle *>(ctx)->session_send(frame, len);
}
static void cb_disconnect(void *ctx) { static_cast<HiflowBle *>(ctx)->session_disconnect(); }
static void cb_set_link_allowed(void *ctx, int allowed) {
  static_cast<HiflowBle *>(ctx)->session_set_link_allowed(allowed != 0);
}
static void cb_on_data(void *ctx, const hiflow_data_t *data) {
  static_cast<HiflowBle *>(ctx)->session_on_data(data);
}
static void cb_on_status(void *ctx, uint8_t status) {
  static_cast<HiflowBle *>(ctx)->session_on_status(status);
}
static void cb_log(void *ctx, int level, const char *msg) {
  static_cast<HiflowBle *>(ctx)->session_log(level, msg);
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

int64_t HiflowBle::now_ms_() const { return esp_timer_get_time() / 1000; }

void HiflowBle::setup() {
  this->time_pref_ = global_preferences->make_preference<TimePref>(fnv1_hash(TIME_PREF_NAME));
}

void HiflowBle::start_session_() {
  hiflow_session_config_t cfg;
  hiflow_session_ops_t ops;
  TimePref time_rec{};
  int64_t persisted_time = 0;

  hiflow_session_config_defaults(&cfg);
  std::snprintf(cfg.sn, sizeof(cfg.sn), "%s", this->sn_.c_str());
  std::snprintf(cfg.ble_id, sizeof(cfg.ble_id), "%s", this->ble_id_.c_str());
  std::snprintf(cfg.pin, sizeof(cfg.pin), "%s", this->pin_.c_str());
  cfg.std_offset = this->std_offset_;
  cfg.eu_dst = this->eu_dst_ ? 1 : 0;
  cfg.poll_interval_ms = this->poll_interval_ms_;

  // No session key here (cfg.have_enc_rand stays 0): the inverter rotates it,
  // so the first connection starts with a V0 pairing, which hands out the
  // current one. The session keeps it in RAM and pairs again whenever a
  // handshake fails, the same way the reference implementation recovers.

  if (this->time_pref_.load(&time_rec) && time_rec.magic == TIME_PREF_MAGIC &&
      time_rec.version == TIME_PREF_VERSION)
    persisted_time = time_rec.unix_time;

  std::memset(&ops, 0, sizeof(ops));
  ops.ctx = this;
  ops.send = cb_send;
  ops.disconnect = cb_disconnect;
  ops.set_link_allowed = cb_set_link_allowed;
  ops.on_data = cb_on_data;
  ops.on_status = cb_on_status;
  ops.log = cb_log;

  hiflow_session_init(&this->session_, &cfg, &ops, this->now_ms_(),
                      static_cast<int64_t>(App.get_build_time()), persisted_time);
  this->session_started_ = true;
  this->next_time_save_ms_ = this->now_ms_() + TIME_SAVE_INTERVAL_MS;
}

void HiflowBle::loop() {
  int64_t now = this->now_ms_();

  // Started from loop() and not from setup(): the session enables the BLE
  // client right away, and ble_client only knows its own enabled flag once its
  // setup() has run.
  if (!this->session_started_) {
    this->start_session_();
    return;
  }

  // The CCCD write is what actually turns notifications on. If its confirmation
  // never arrives, go ahead anyway rather than sitting on an idle connection.
  if (!this->link_ready_ && this->node_state == espbt::ClientState::ESTABLISHED &&
      this->notify_registered_ms_ != 0 && now - this->notify_registered_ms_ > LINK_UP_FALLBACK_MS) {
    ESP_LOGW(TAG, "no confirmation for the notify subscription - starting the handshake anyway");
    this->report_link_up_();
  }

  hiflow_session_tick(&this->session_, now);

  if (now >= this->next_status_refresh_ms_)
    this->publish_status_(hiflow_session_status(&this->session_), true);
  this->flush_prefs_();
}

void HiflowBle::flush_prefs_() {
  // Flash writes block the BLE stack for a moment, so they only happen while
  // the session is idle between two polls.
  if (hiflow_session_state(&this->session_) != HIFLOW_STATE_READY)
    return;

  int64_t now = this->now_ms_();
  if (now >= this->next_time_save_ms_) {
    TimePref rec{};
    rec.magic = TIME_PREF_MAGIC;
    rec.version = TIME_PREF_VERSION;
    rec.unix_time = hiflow_session_unix_time(&this->session_, now);
    this->next_time_save_ms_ = now + TIME_SAVE_INTERVAL_MS;
    this->time_pref_.save(&rec);
    global_preferences->sync();
  }
}

void HiflowBle::dump_config() {
  ESP_LOGCONFIG(TAG, "HiFlow BLE:");
  if (this->parent() != nullptr) {
    ESP_LOGCONFIG(TAG, "  MAC address: %s", this->parent()->address_str());
  }
  ESP_LOGCONFIG(TAG, "  GATT service: 0000e0ff-3c17-d293-8e48-14fe2e4da212 (ffe1 write / ffe2 notify)");
  ESP_LOGCONFIG(TAG, "  Device serial: configured (%zu chars)", this->sn_.size());
  ESP_LOGCONFIG(TAG, "  BLE id: configured (%zu chars)", this->ble_id_.size());
  ESP_LOGCONFIG(TAG, "  BLE PIN: %s", this->pin_.empty() ? "not set" : "configured");
  ESP_LOGCONFIG(TAG, "  Time offset: %d s%s", static_cast<int>(this->std_offset_),
                this->eu_dst_ ? " + European summer time" : "");
  ESP_LOGCONFIG(TAG, "  Poll interval: %u ms", static_cast<unsigned>(this->poll_interval_ms_));
}

void HiflowBle::register_sensor(sensor::Sensor *sensor, uint8_t type) {
  if (type >= HIFLOW_SENSOR_TYPE_COUNT) {
    ESP_LOGE(TAG, "sensor registered with unknown type id %u", type);
    return;
  }
  this->sensors_[type] = sensor;
}

// ---------------------------------------------------------------------------
// GATT plumbing
// ---------------------------------------------------------------------------

void HiflowBle::report_link_up_() {
  if (this->link_ready_)
    return;
  this->link_ready_ = true;
  ESP_LOGI(TAG, "subscribed to ffe2 (MTU %u), starting the HiFlow handshake", this->mtu_);
  hiflow_session_link_up(&this->session_, this->now_ms_());
}

void HiflowBle::report_link_down_(int reason) {
  if (this->link_reported_)
    return;
  this->link_reported_ = true;
  this->link_ready_ = false;
  this->tx_handle_ = 0;
  this->rx_handle_ = 0;
  this->notify_registered_ms_ = 0;
  if (this->session_started_)
    hiflow_session_link_down(&this->session_, this->now_ms_(), reason);
}

void HiflowBle::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                    esp_ble_gattc_cb_param_t *param) {
  if (!this->session_started_)
    return;

  switch (event) {
    case ESP_GATTC_CONNECT_EVT:
      // A new connection: its link-down is reported exactly once again.
      this->link_reported_ = false;
      this->link_ready_ = false;
      this->disconnect_reason_ = 0;
      this->notify_registered_ms_ = 0;
      break;

    case ESP_GATTC_OPEN_EVT:
      if (param->open.status != ESP_GATT_OK && param->open.status != ESP_GATT_ALREADY_OPEN) {
        ESP_LOGW(TAG, "connection could not be opened (status %d)", param->open.status);
        this->report_link_down_(HIFLOW_LINK_NOT_ESTABLISHED);
      }
      break;

    case ESP_GATTC_CFG_MTU_EVT:
      if (param->cfg_mtu.status == ESP_GATT_OK)
        this->mtu_ = param->cfg_mtu.mtu;
      break;

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      this->tx_handle_ = 0;
      this->rx_handle_ = 0;
      if (this->parent() == nullptr) {
        ESP_LOGE(TAG, "no ble_client parent");
        return;
      }
      auto *tx = this->parent()->get_characteristic(this->service_uuid_, this->tx_uuid_);
      auto *rx = this->parent()->get_characteristic(this->service_uuid_, this->rx_uuid_);
      if (tx == nullptr || rx == nullptr) {
        ESP_LOGW(TAG, "HiFlow GATT service/characteristics not found on this device");
        return;
      }
      this->tx_handle_ = tx->handle;
      this->rx_handle_ = rx->handle;
      // Subscribe first; ESPHome's ble_client keeps the GATT cache alive until
      // the registration completes, after which only the handles are usable.
      auto status = this->parent()->register_for_notify(this->rx_handle_);
      if (status != ESP_OK)
        ESP_LOGW(TAG, "esp_ble_gattc_register_for_notify failed: %d", status);
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.handle != this->rx_handle_)
        break;
      if (param->reg_for_notify.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "notify registration rejected: %d", param->reg_for_notify.status);
        break;
      }
      this->node_state = espbt::ClientState::ESTABLISHED;
      this->notify_registered_ms_ = this->now_ms_();
      break;
    }

    case ESP_GATTC_WRITE_DESCR_EVT:
      // The CCCD write confirms that notifications are really on. Only now can
      // a reply reach us, so only now does the handshake start.
      if (!this->link_ready_ && this->node_state == espbt::ClientState::ESTABLISHED) {
        if (param->write.status == ESP_GATT_OK) {
          this->report_link_up_();
        } else {
          ESP_LOGW(TAG, "notifications could not be enabled (status %d)", param->write.status);
          this->report_link_down_(HIFLOW_LINK_NOT_ESTABLISHED);
        }
      }
      break;

    case ESP_GATTC_WRITE_CHAR_EVT:
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "the device rejected our write (status %d)", param->write.status);
        hiflow_session_tx_failed(&this->session_, this->now_ms_());
      }
      break;

    case ESP_GATTC_NOTIFY_EVT:
      if (param->notify.handle != this->rx_handle_)
        break;
      hiflow_session_rx(&this->session_, this->now_ms_(), param->notify.value,
                        param->notify.value_len);
      break;

    case ESP_GATTC_DISCONNECT_EVT:
      // DISCONNECT_EVT and CLOSE_EVT both arrive; the reason is only in the
      // first one, and only the first one counts.
      this->disconnect_reason_ = param->disconnect.reason;
      this->report_link_down_(param->disconnect.reason);
      break;

    case ESP_GATTC_CLOSE_EVT:
      this->report_link_down_(this->disconnect_reason_ != 0 ? this->disconnect_reason_
                                                            : param->close.reason);
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// session callbacks
// ---------------------------------------------------------------------------

int HiflowBle::session_send(const uint8_t *frame, size_t len) {
  if (this->node_state != espbt::ClientState::ESTABLISHED || this->tx_handle_ == 0) {
    ESP_LOGW(TAG, "cannot write, the link is not established");
    return 0;
  }
  if (len + 3 > this->mtu_)
    ESP_LOGW(TAG, "frame of %u bytes exceeds the negotiated MTU of %u", static_cast<unsigned>(len),
             this->mtu_);

  // Acknowledged writes: the device drops unacknowledged ones.
  auto status = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                         this->tx_handle_, static_cast<uint16_t>(len),
                                         const_cast<uint8_t *>(frame), ESP_GATT_WRITE_TYPE_RSP,
                                         ESP_GATT_AUTH_REQ_NONE);
  if (status != ESP_OK) {
    ESP_LOGW(TAG, "esp_ble_gattc_write_char failed: %d", status);
    return 0;
  }
  return 1;
}

void HiflowBle::session_disconnect() {
  if (this->parent() != nullptr)
    this->parent()->disconnect();
}

void HiflowBle::session_set_link_allowed(bool allowed) {
  if (this->parent() == nullptr)
    return;
  // While the session waits after a failure, the BLE client must not hold a
  // connection: the inverter serves a single central, and an idle link there is
  // what the phone app runs into.
  this->parent()->set_enabled(allowed);
}

void HiflowBle::session_on_data(const hiflow_data_t *data) {
  static const uint8_t PORT_BASE[HIFLOW_MAX_PORTS] = {HIFLOW_PORT1_POWER, HIFLOW_PORT2_POWER,
                                                      HIFLOW_PORT3_POWER, HIFLOW_PORT4_POWER};
  static const uint8_t PORT_ENERGY_BASE[HIFLOW_MAX_PORTS] = {HIFLOW_PORT1_ENERGY_TOTAL, HIFLOW_PORT2_ENERGY_TOTAL,
                                                             HIFLOW_PORT3_ENERGY_TOTAL, HIFLOW_PORT4_ENERGY_TOTAL};

  if (data->have_ac) {
    this->publish_(HIFLOW_AC_POWER, data->ac_power_w);
    this->publish_(HIFLOW_AC_VOLTAGE, data->ac_voltage_v);
    this->publish_(HIFLOW_AC_CURRENT, data->ac_current_a);
    this->publish_(HIFLOW_AC_FREQUENCY, data->ac_frequency_hz);
    this->publish_(HIFLOW_TEMPERATURE, data->temperature_c);
    this->publish_(HIFLOW_REACTIVE_POWER, data->reactive_power_var);
    this->publish_(HIFLOW_POWER_FACTOR, data->power_factor_pct);
    this->publish_(HIFLOW_WARNINGS, data->warning_count);
  }

  for (size_t i = 0; i < HIFLOW_MAX_PORTS; i++) {
    if (!data->ports[i].present)
      continue;
    uint8_t base = PORT_BASE[i];  // enum order per port: POWER, VOLTAGE, CURRENT
    this->publish_(base + 0, data->ports[i].power_w);
    this->publish_(base + 1, data->ports[i].voltage_v);
    this->publish_(base + 2, data->ports[i].current_a);
    base = PORT_ENERGY_BASE[i];  // enum order per port: ENERGY_TOTAL, ENERGY_DAILY
    // Same rule as the sum below: a lifetime counter of 0 is a missing value.
    if (data->ports[i].energy_total_wh > 0.0f)
      this->publish_(base + 0, data->ports[i].energy_total_wh);
    this->publish_(base + 1, data->ports[i].energy_daily_wh);
  }

  // The lifetime counter feeds a total_increasing sensor: a value that drops
  // would look like a meter reset in Home Assistant's statistics.
  if (data->energy_total_wh > 0.0f) {
    if (data->energy_total_wh + 0.5f >= this->last_energy_total_) {
      this->last_energy_total_ = data->energy_total_wh;
      this->publish_(HIFLOW_ENERGY_TOTAL, data->energy_total_wh);
    } else {
      ESP_LOGW(TAG, "ignoring a total energy of %.0f Wh below the last %.0f Wh",
               data->energy_total_wh, this->last_energy_total_);
    }
  }
  this->publish_(HIFLOW_ENERGY_DAILY, data->energy_daily_wh);
}

void HiflowBle::session_on_status(uint8_t status) { this->publish_status_(status, false); }

void HiflowBle::session_log(int level, const char *msg) {
  switch (level) {
    case 0:
      ESP_LOGE(TAG, "%s", msg);
      break;
    case 1:
      ESP_LOGW(TAG, "%s", msg);
      break;
    case 2:
      ESP_LOGI(TAG, "%s", msg);
      break;
    default:
      ESP_LOGD(TAG, "%s", msg);
      break;
  }
}

// ---------------------------------------------------------------------------
// publishing
// ---------------------------------------------------------------------------

void HiflowBle::publish_(uint8_t type, float value) {
  if (type >= HIFLOW_SENSOR_TYPE_COUNT)
    return;
  auto *sensor = this->sensors_[type];
  if (sensor == nullptr || std::isnan(value))
    return;
  sensor->publish_state(value);
}

// Every published value is a Zigbee report, so the status goes out when it
// changes and as a keep-alive every STATUS_REFRESH_MS (at night it is the only
// traffic). READING is folded into READY: it lasts for one request, and sending
// both would cost two reports per data cycle without telling anything; a
// request that hangs ends in status 11.
void HiflowBle::publish_status_(uint8_t status, bool refresh) {
  if (status == HIFLOW_STATUS_READING)
    status = HIFLOW_STATUS_READY;
  if (!refresh && status == this->last_status_)
    return;
  this->last_status_ = status;
  this->next_status_refresh_ms_ = this->now_ms_() + STATUS_REFRESH_MS;
  this->publish_(HIFLOW_STATUS, static_cast<float>(status));
}

}  // namespace hiflow_ble
}  // namespace esphome

#endif  // USE_ESP32
