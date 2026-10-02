#include "hiflow_ble.h"
#include "hiflow_inverter_switch.h"
#include "hiflow_power_limit.h"
#include "hiflow_zigbee_time.h"

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
static const int64_t SLIDER_POLL_MS = 500;            // present_value is polled this often
static const int64_t SLIDER_QUIET_MS = 5000;          // a new value must stand this long
static const int64_t IDLE_TIME_SAVE_INTERVAL_MS = 1800000;  // the clock, while no link is up
static const char *const DAYLOG_PREF_NAME = "hiflow_daylog_v2";
static const int64_t TIME_REQUEST_INTERVAL_MS = 43200000;  // the network time, twice a day
static const int64_t TIME_RETRY_MS = 600000;               // after a failed request
static const int64_t TIME_BUSY_RETRY_MS = 5000;            // the Zigbee stack was busy
static const int64_t TIME_JOIN_SETTLE_MS = 30000;           // after a (re)join, before the first request
static const int64_t SWITCH_QUIET_MS = 1000;          // merges a quick off-on into one command
static const uint16_t SWITCH_PREF_MAGIC = 0x4853;     // 'HS'
static const char *const SWITCH_PREF_NAME = "hiflow_switch_v1";
static const uint16_t LIMIT_PREF_MAGIC = 0x484C;      // 'HL'
static const char *const LIMIT_PREF_NAME = "hiflow_limit_v1";

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
static void cb_on_power_limit(void *ctx, int32_t tenths) {
  static_cast<HiflowBle *>(ctx)->session_on_power_limit(tenths);
}
static void cb_on_inverter_power(void *ctx, int on, int confirmed) {
  static_cast<HiflowBle *>(ctx)->session_on_inverter_power(on != 0, confirmed != 0);
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
  this->daylog_pref_ = global_preferences->make_preference<hiflow_dayrec_t>(fnv1_hash(DAYLOG_PREF_NAME));
  hiflow_daylog_init(&this->daylog_);
  this->limit_pref_ = global_preferences->make_preference<LimitPref>(fnv1_hash(LIMIT_PREF_NAME));
  LimitPref limit{};
  if (this->limit_pref_.load(&limit) && limit.magic == LIMIT_PREF_MAGIC && limit.tenths >= 0 && limit.tenths <= 1000)
    this->last_limit_tenths_ = limit.tenths;
#ifdef USE_ZIGBEE
  if (this->switch_zb_ != nullptr) {
    // The switch shows the state the inverter was last switched to; nothing
    // is sent to the inverter for it.
    SwitchPref rec{};
    this->switch_pref_ = global_preferences->make_preference<SwitchPref>(fnv1_hash(SWITCH_PREF_NAME));
    if (this->switch_pref_.load(&rec) && rec.magic == SWITCH_PREF_MAGIC)
      this->switch_state_ = rec.on != 0;
    this->switch_publish_ = true;
  }
#endif
}

#ifdef HIFLOW_DEMO_DAY
// A made-up sunny day with two clouds, sunrise 07:20 to dusk 19:10, up to the
// current time. It never reaches flash (with demo_day the day log is not
// saved); after dusk the last reading is 0 W and the inverter is in standby.
static void fill_demo_day(hiflow_daylog_t *d, int64_t local_time) {
  const int32_t day = hiflow_local_day(local_time);
  const int now_min = hiflow_local_minute(local_time);
  const int rise = 7 * 60 + 20, set = 19 * 60 + 10;
  hiflow_daylog_init(d);
  d->rec.day = day;
  d->rec.energy_total_wh = 1234567.0f;
  float last = 0.0f;
  for (int slot = 0; slot < HIFLOW_DAYLOG_SLOTS; slot++) {
    const int minute = slot * 5;
    if (minute > now_min)
      break;
    float w = 0.0f;
    if (minute > rise && minute < set) {
      const float x = static_cast<float>(minute - rise) / (set - rise);
      w = 1650.0f * powf(sinf(3.14159265f * x), 1.4f);
      if (minute >= 11 * 60 && minute < 11 * 60 + 40)
        w *= 0.45f;  // a cloud
      if (minute >= 14 * 60 + 30 && minute < 14 * 60 + 50)
        w *= 0.6f;
      w *= 1.0f + 0.03f * sinf(slot * 1.7f);  // a little ripple
    }
    d->rec.curve[slot] = static_cast<uint16_t>(w + 0.5f);
    d->rec.energy_daily_wh += w * 5.0f / 60.0f;
    if (w > d->rec.peak_w) {
      d->rec.peak_w = w;
      d->rec.peak_min = static_cast<int16_t>(minute);
    }
    last = w;
  }
  // Four inputs with different panels: east, south, south, west.
  static const float SHARE[HIFLOW_DAYLOG_PORTS] = {0.21f, 0.31f, 0.29f, 0.19f};
  for (int i = 0; i < HIFLOW_DAYLOG_PORTS; i++)
    d->rec.port_daily_wh[i] = d->rec.energy_daily_wh * SHARE[i];
  d->rec.last_feed = last >= HIFLOW_FEED_MIN_W ? 1 : 0;
  d->rec.standby = d->rec.last_feed == 0;
}
#endif

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
#ifdef USE_ZIGBEE
  cfg.power_limit = this->slider_zb_ != nullptr ? 1 : 0;
  cfg.inverter_control = this->switch_zb_ != nullptr ? 1 : 0;
#endif

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
  ops.on_power_limit = cb_on_power_limit;
  ops.on_inverter_power = cb_on_inverter_power;
  ops.log = cb_log;

  hiflow_session_init(&this->session_, &cfg, &ops, this->now_ms_(),
                      static_cast<int64_t>(App.get_build_time()), persisted_time);
  this->session_started_ = true;
  this->next_time_save_ms_ = this->now_ms_() + TIME_SAVE_INTERVAL_MS;

  // The day log as it was before the reboot, whatever its day: the first
  // reading of a new day starts that day, as it would have without the reboot.
  hiflow_dayrec_t saved{};
  if (this->daylog_pref_.load(&saved) && hiflow_daylog_restore(&this->daylog_, &saved, this->now_ms_())) {
    ESP_LOGI(TAG, "day log restored (peak %.0f W%s)", saved.peak_w, this->daylog_.rec.standby ? ", standby" : "");
  }
#ifdef HIFLOW_DEMO_DAY
  fill_demo_day(&this->daylog_, this->local_time());
  ESP_LOGW(TAG, "demo_day: today's day log is made up (peak %.0f W, %.2f kWh)", this->daylog_.rec.peak_w,
           this->daylog_.rec.energy_daily_wh / 1000.0f);
#endif
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

  this->poll_slider_(now);
  this->poll_switch_(now);
  this->poll_network_time_(now);
  if (now >= this->next_daylog_check_ms_) {
    this->next_daylog_check_ms_ = now + 1000;
    int facts = 0;
    if (this->inverter_switch_state() == 0)
      facts |= HIFLOW_FACT_SWITCH_OFF;
    if (hiflow_session_power_limit_tenths(&this->session_) == 0)
      facts |= HIFLOW_FACT_LIMIT_ZERO;
    if (this->last_status_ == HIFLOW_STATUS_BACKOFF_BASE + HIFLOW_FAIL_PIN ||
        this->last_status_ == HIFLOW_STATUS_BACKOFF_BASE + HIFLOW_FAIL_STALE_KEY)
      facts |= HIFLOW_FACT_REFUSED;
    hiflow_daylog_update(&this->daylog_, now, facts);
  }
  hiflow_session_tick(&this->session_, now);

  if (now >= this->next_status_refresh_ms_)
    this->publish_status_(hiflow_session_status(&this->session_), true);
  this->flush_prefs_();
}

void HiflowBle::flush_prefs_() {
  // Flash writes block the BLE stack for a moment, so they only happen while
  // the session is idle between two polls, or while no link is up at all and
  // none is being made (the inverter is not advertising, at night): then every
  // 30 minutes, so a reboot at night does not throw the clock back to dusk.
  // Not while the client connects or discovers: that could stall it.
  uint8_t state = hiflow_session_state(&this->session_);
  bool no_link = state == HIFLOW_STATE_WAIT_LINK && !this->link_ready_ &&
                 this->node_state == espbt::ClientState::IDLE;
  bool idle = state == HIFLOW_STATE_BACKOFF || no_link;
  if (state != HIFLOW_STATE_READY && !idle)
    return;

  int64_t now = this->now_ms_();
  bool time_due = now >= this->next_time_save_ms_;
  if (time_due) {
    TimePref rec{};
    rec.magic = TIME_PREF_MAGIC;
    rec.version = TIME_PREF_VERSION;
    rec.unix_time = hiflow_session_unix_time(&this->session_, now);
    this->next_time_save_ms_ = now + (idle ? IDLE_TIME_SAVE_INTERVAL_MS : TIME_SAVE_INTERVAL_MS);
    this->time_pref_.save(&rec);
  }
  // The day log whenever it changed, at most every 5 minutes (every minute
  // without a link), so a reboot loses no more than the last minutes of the
  // curve; the inverter reports its energy counters again anyway.
  bool daylog_due = false;
  bool daylog_saved = false;
#ifndef HIFLOW_DEMO_DAY
  daylog_due = hiflow_daylog_save_due(&this->daylog_, now, !idle);
  if (daylog_due)
    daylog_saved = this->daylog_pref_.save(&this->daylog_.rec);
#endif
  // The last power limit read, for the display after a reboot.
  const bool limit_due = this->limit_save_due_;
  if (limit_due) {
    LimitPref rec{};
    rec.magic = LIMIT_PREF_MAGIC;
    rec.tenths = static_cast<int16_t>(this->last_limit_tenths_);
    this->limit_pref_.save(&rec);
    this->limit_save_due_ = false;
  }
  if (!time_due && !daylog_due && !limit_due)
    return;
  bool ok = global_preferences->sync();
  if (daylog_due) {
    // Saved only when the record went into the preference and the sync wrote it.
    ok = ok && daylog_saved;
    hiflow_daylog_mark_saved(&this->daylog_, now, ok);
    if (!ok) {
      ESP_LOGW(TAG, "day log not saved, trying again later");
    }
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
#ifdef USE_ZIGBEE
  if (this->slider_zb_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Power limit slider: Zigbee endpoint %u", this->slider_endpoint_);
  }
  if (this->switch_zb_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Inverter switch: Zigbee endpoint %u, last state %s", this->switch_endpoint_,
                  this->switch_state_ ? "on" : "off");
  }
  if (this->time_zb_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Network time: Zigbee endpoint %u", this->time_endpoint_);
  }
#endif
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
      ESP_LOGV(TAG, "notification, %u bytes", param->notify.value_len);
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

  this->last_data_ms_ = this->now_ms_();
  if (data->have_ac) {
    hiflow_reading_t reading{};
    reading.now_ms = this->last_data_ms_;
    reading.local_time = this->local_time();
    reading.clock_ok = this->clock_synced();
    reading.ac_w = data->ac_power_w;
    reading.energy_total_wh = data->energy_total_wh;
    reading.energy_daily_wh = data->energy_daily_wh;
    for (int i = 0; i < HIFLOW_DAYLOG_PORTS && i < HIFLOW_MAX_PORTS; i++) {
      reading.port_total_wh[i] = data->ports[i].present ? data->ports[i].energy_total_wh : 0.0f;
      reading.port_daily_wh[i] = data->ports[i].present ? data->ports[i].energy_daily_wh : -1.0f;
    }
    hiflow_daylog_sample(&this->daylog_, &reading);
  }
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
    this->publish_total_(base + 0, data->ports[i].energy_total_wh, this->last_port_energy_total_[i]);
    this->publish_(base + 1, data->ports[i].energy_daily_wh);
  }

  this->publish_total_(HIFLOW_ENERGY_TOTAL, data->energy_total_wh, this->last_energy_total_);
  this->publish_(HIFLOW_ENERGY_DAILY, data->energy_daily_wh);
}

void HiflowBle::session_on_status(uint8_t status) { this->publish_status_(status, false); }

// The session reports the limit the inverter holds: after every read, and
// unchanged when a request needed no write or was dropped (-1 when it was not
// read since the boot). Only these values ever reach the slider.
void HiflowBle::session_on_power_limit(int32_t tenths) {
  // Kept for the display: the inverter keeps its limit over the night, so the
  // last value read stands until the next read. The slider never gets it.
  if (tenths >= 0 && tenths <= 1000 && tenths != this->last_limit_tenths_) {
    this->last_limit_tenths_ = tenths;
    this->limit_save_due_ = true;
  }
#ifdef USE_ZIGBEE
  if (this->slider_zb_ == nullptr)
    return;
  // -1: a request was dropped before the limit was read since the boot. The
  // slider goes back to the last limit read, which the inverter keeps over the
  // night, or to empty when none was ever read. A value the bridge sets itself
  // is never taken as a request.
  if (tenths < 0)
    tenths = this->last_limit_tenths_;
  this->slider_set_ = tenths < 0 ? NAN : static_cast<float>(tenths) / 10.0f;
  this->slider_publish_ = true;
#endif
}

// ---------------------------------------------------------------------------
// power limit slider
// ---------------------------------------------------------------------------

// A write from the coordinator lands in present_value without telling us, so
// the attribute is polled. A value that differs from the one we set ourselves
// is a user change; it goes to the session once it has stood for
// SLIDER_QUIET_MS, so dragging the slider costs one write at most. Nothing is
// ever requested on boot or on a (re)join: present_value starts as NaN and
// only the session's readback fills it.
void HiflowBle::poll_slider_(int64_t now) {
#ifdef USE_ZIGBEE
  if (this->slider_zb_ == nullptr || !this->slider_zb_->is_started() || now < this->next_slider_poll_ms_)
    return;
  this->next_slider_poll_ms_ = now + SLIDER_POLL_MS;

  if (this->slider_publish_ && this->slider_zb_->is_joined()) {
    if (write_power_limit_attr(this->slider_zb_, this->slider_endpoint_, this->slider_set_)) {
      this->slider_publish_ = false;
      this->slider_seen_ = this->slider_set_;
    }
    return;
  }

  float value = NAN;
  if (!read_power_limit_attr(this->slider_endpoint_, &value) || std::isnan(value))
    return;
  if (value == this->slider_set_ || this->slider_publish_) {
    this->slider_seen_ = value;
    return;
  }
  if (value != this->slider_seen_) {
    ESP_LOGD(TAG, "slider moved to %.1f %%", value);
    this->slider_seen_ = value;
    this->slider_seen_ms_ = now;
    return;
  }
  if (now - this->slider_seen_ms_ < SLIDER_QUIET_MS)
    return;

  int32_t percent = hiflow_session_request_power_limit(&this->session_, now, value);
  ESP_LOGI(TAG, "slider set to %.1f %%, requesting %d %%", value, static_cast<int>(percent));
  // Until the session answers, the request counts as handled; the readback
  // (or the unchanged value, if nothing is written) replaces it.
  this->slider_set_ = value;
#endif
}

// ---------------------------------------------------------------------------
// display accessors
// ---------------------------------------------------------------------------

int64_t HiflowBle::ms_since_data() const {
  return this->last_data_ms_ < 0 ? -1 : this->now_ms_() - this->last_data_ms_;
}

int HiflowBle::retry_in_s() const {
  return this->session_started_ ? hiflow_session_retry_in_s(&this->session_, this->now_ms_()) : -1;
}

int HiflowBle::inverter_switch_state() const {
#ifdef USE_ZIGBEE
  if (this->switch_zb_ != nullptr)
    return this->switch_state_ ? 1 : 0;
#endif
  return -1;
}

float HiflowBle::power_limit_percent() const {
  int32_t tenths = hiflow_session_power_limit_tenths(&this->session_);
  return tenths < 0 ? NAN : static_cast<float>(tenths) / 10.0f;
}

float HiflowBle::last_power_limit_percent() const {
  const float now = this->power_limit_percent();
  if (!std::isnan(now))
    return now;
  return this->last_limit_tenths_ < 0 ? NAN : static_cast<float>(this->last_limit_tenths_) / 10.0f;
}

int64_t HiflowBle::local_time() const {
  return this->session_started_ ? hiflow_session_local_time(&this->session_, this->now_ms_()) : 0;
}

// ---------------------------------------------------------------------------
// inverter on/off switch
// ---------------------------------------------------------------------------

// The session reports every request's outcome. A confirmed state is kept and
// saved; otherwise the switch goes back to the state it had.
void HiflowBle::session_on_inverter_power(bool on, bool confirmed) {
#ifdef USE_ZIGBEE
  if (this->switch_zb_ == nullptr)
    return;
  this->switch_pending_ = false;
  if (confirmed && on != this->switch_state_) {
    SwitchPref rec{};
    rec.magic = SWITCH_PREF_MAGIC;
    rec.on = on ? 1 : 0;
    this->switch_state_ = on;
    this->switch_pref_.save(&rec);
    global_preferences->sync();
  }
  if (!confirmed)
    ESP_LOGW(TAG, "the inverter was not switched %s, the switch goes back to %s", on ? "on" : "off",
             this->switch_state_ ? "on" : "off");
  this->switch_publish_ = true;
#endif
}

// Same scheme as the slider: the attribute is polled, a value that differs
// from the confirmed state is a user change, and it goes to the session once
// it has stood for SWITCH_QUIET_MS. Nothing is ever sent on boot or on a
// (re)join: only a change of the attribute sends a command.
void HiflowBle::poll_switch_(int64_t now) {
#ifdef USE_ZIGBEE
  if (this->switch_zb_ == nullptr || !this->switch_zb_->is_started() || now < this->next_switch_poll_ms_)
    return;
  this->next_switch_poll_ms_ = now + SLIDER_POLL_MS;

  if (this->switch_publish_ && this->switch_zb_->is_joined()) {
    if (write_inverter_switch_attr(this->switch_zb_, this->switch_endpoint_, this->switch_state_)) {
      this->switch_publish_ = false;
      this->switch_seen_ = this->switch_state_;
    }
    return;
  }

  bool on = true;
  if (this->switch_pending_ || !read_inverter_switch_attr(this->switch_endpoint_, &on))
    return;
  if (on == this->switch_state_ || this->switch_publish_) {
    this->switch_seen_ = on;
    return;
  }
  if (static_cast<int>(on) != this->switch_seen_) {
    ESP_LOGD(TAG, "switch set to %s", on ? "on" : "off");
    this->switch_seen_ = on;
    this->switch_seen_ms_ = now;
    return;
  }
  if (now - this->switch_seen_ms_ < SWITCH_QUIET_MS)
    return;

  if (hiflow_session_request_inverter_power(&this->session_, now, on ? 1 : 0) == 0) {
    ESP_LOGI(TAG, "switch set to %s, requesting it from the inverter", on ? "on" : "off");
    this->switch_pending_ = true;
  } else {
    this->switch_publish_ = true;
  }
#endif
}

// ---------------------------------------------------------------------------
// network time
// ---------------------------------------------------------------------------

// Asks the network's time server once joined, then twice a day (every 10
// minutes while it fails), and hands a received time to the session clock.
void HiflowBle::poll_network_time_(int64_t now) {
#ifdef USE_ZIGBEE
  if (this->time_zb_ == nullptr)
    return;
  int status = -1;
  int64_t network_time = take_network_time(&status);
  if (network_time > 0) {
    int64_t before = hiflow_session_unix_time(&this->session_, now);
    hiflow_session_observe_time(&this->session_, now, network_time);
    ESP_LOGI(TAG, "network time received, our clock was %lld s behind", (long long) (network_time - before));
  }
  if (status != this->last_time_status_) {
    this->last_time_status_ = status;
    if (status != NETWORK_TIME_OK && status != NETWORK_TIME_NONE) {
      ESP_LOGW(TAG, "no network time (%s), asking again in 10 minutes", network_time_status_str(status));
      this->next_time_request_ms_ = now + TIME_RETRY_MS;
    }
  }
  // Right after a boot or join the network is not ready yet (the device
  // announce is still pending, a new device is still being interviewed), so
  // a request then only times out.
  if (!this->time_zb_->is_joined()) {
    this->time_joined_ms_ = 0;
    return;
  }
  if (this->time_joined_ms_ == 0)
    this->time_joined_ms_ = now;
  if (now - this->time_joined_ms_ < TIME_JOIN_SETTLE_MS || now < this->next_time_request_ms_)
    return;
  switch (request_network_time(this->time_endpoint_)) {
    case TIME_REQUEST_SENT:
      ESP_LOGD(TAG, "asking the network for the time");
      this->next_time_request_ms_ = now + TIME_REQUEST_INTERVAL_MS;
      this->last_time_status_ = -1;
      break;
    case TIME_REQUEST_BUSY:
      this->next_time_request_ms_ = now + TIME_BUSY_RETRY_MS;
      break;
    case TIME_REQUEST_FAILED:
      this->next_time_request_ms_ = now + TIME_RETRY_MS;
      break;
  }
#endif
}

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

// The lifetime counters feed total_increasing sensors: a value that drops would
// look like a meter reset in Home Assistant's statistics, and 0 is a missing
// value, not a reading.
void HiflowBle::publish_total_(uint8_t type, float value, float &last) {
  if (!(value > 0.0f))
    return;
  if (value + 0.5f < last) {
    ESP_LOGW(TAG, "ignoring a lifetime energy of %.0f Wh below the last %.0f Wh (sensor %u)", value, last, (unsigned) type);
    return;
  }
  last = value;
  this->publish_(type, value);
}

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
