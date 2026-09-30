#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"

#include <string>

#ifdef USE_ESP32

#include <esp_gattc_api.h>

// Flat, ESPHome-loadable copies of the pre-verified C core. Regenerate with
// ./sync_core.sh; the originals live under src/hiflow_core and src/hiflow_pb.
#include "hiflow_daylog.h"
#include "hiflow_session.h"

#ifdef USE_ZIGBEE
#include "esphome/components/zigbee/zigbee_esp32.h"
#endif

namespace esphome {
namespace hiflow_ble {

namespace espbt = esphome::esp32_ble_tracker;

/// Which measurement a configured sensor entity maps to.
/// Keep in sync with the SENSOR_TYPES table in sensor/__init__.py.
enum HiflowSensorType : uint8_t {
  HIFLOW_AC_POWER = 0,
  HIFLOW_AC_VOLTAGE,
  HIFLOW_AC_CURRENT,
  HIFLOW_AC_FREQUENCY,
  HIFLOW_TEMPERATURE,
  HIFLOW_ENERGY_TOTAL,
  HIFLOW_ENERGY_DAILY,
  HIFLOW_PORT1_POWER,
  HIFLOW_PORT1_VOLTAGE,
  HIFLOW_PORT1_CURRENT,
  HIFLOW_PORT2_POWER,
  HIFLOW_PORT2_VOLTAGE,
  HIFLOW_PORT2_CURRENT,
  HIFLOW_PORT3_POWER,
  HIFLOW_PORT3_VOLTAGE,
  HIFLOW_PORT3_CURRENT,
  HIFLOW_PORT4_POWER,
  HIFLOW_PORT4_VOLTAGE,
  HIFLOW_PORT4_CURRENT,
  // Diagnostics: the session state as a number (see hiflow_session.h). Without
  // a serial console at the installation site this is the only way to see from
  // Home Assistant how far a session gets.
  HIFLOW_STATUS,
  // Added after the status so that older configurations keep their endpoints.
  HIFLOW_REACTIVE_POWER,
  HIFLOW_POWER_FACTOR,
  HIFLOW_WARNINGS,
  HIFLOW_PORT1_ENERGY_TOTAL,
  HIFLOW_PORT1_ENERGY_DAILY,
  HIFLOW_PORT2_ENERGY_TOTAL,
  HIFLOW_PORT2_ENERGY_DAILY,
  HIFLOW_PORT3_ENERGY_TOTAL,
  HIFLOW_PORT3_ENERGY_DAILY,
  HIFLOW_PORT4_ENERGY_TOTAL,
  HIFLOW_PORT4_ENERGY_DAILY,
  HIFLOW_SENSOR_TYPE_COUNT,
};

/// Reads a Hoymiles HiFlow Pro over BLE and publishes the values as sensors.
///
/// This class is only the adapter: the BLE transport below it is ESPHome's
/// ble_client, the protocol above it is the session core in hiflow_session.h,
/// which the host tests in test/host cover. What is left here is plumbing —
/// GATT events in, frames out, values published, the clock kept in flash. The
/// session key is never configured or stored: the session fetches it with a V0
/// pairing on the first connection after boot and whenever a handshake fails.
class HiflowBle : public Component, public ble_client::BLEClientNode {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  // --- codegen setters ---
  void set_sn(const std::string &v) { this->sn_ = v; }
  void set_ble_id(const std::string &v) { this->ble_id_ = v; }
  void set_pin(const std::string &v) { this->pin_ = v; }
  void set_offset(int32_t v) { this->std_offset_ = v; }
  void set_eu_dst(bool v) { this->eu_dst_ = v; }
  void set_poll_interval(uint32_t ms) { this->poll_interval_ms_ = ms; }
  void set_service_uuid128(const uint8_t *uuid) { this->service_uuid_ = espbt::ESPBTUUID::from_raw(uuid); }
  void set_tx_uuid16(uint16_t uuid) { this->tx_uuid_ = espbt::ESPBTUUID::from_uint16(uuid); }
  void set_rx_uuid16(uint16_t uuid) { this->rx_uuid_ = espbt::ESPBTUUID::from_uint16(uuid); }
  void register_sensor(sensor::Sensor *sensor, uint8_t type);
#ifdef USE_ZIGBEE
  /// The power limit slider on `endpoint` (see hiflow_power_limit.h).
  void set_power_limit_slider(zigbee::ZigbeeComponent *zb, uint8_t endpoint) {
    this->slider_zb_ = zb;
    this->slider_endpoint_ = endpoint;
  }
  /// The network time from the Time cluster on `endpoint` (see hiflow_zigbee_time.h).
  void set_network_time(zigbee::ZigbeeComponent *zb, uint8_t endpoint) {
    this->time_zb_ = zb;
    this->time_endpoint_ = endpoint;
  }
  /// The inverter's on/off switch on `endpoint` (see hiflow_inverter_switch.h).
  void set_inverter_switch(zigbee::ZigbeeComponent *zb, uint8_t endpoint) {
    this->switch_zb_ = zb;
    this->switch_endpoint_ = endpoint;
  }
#endif

  // --- for an on-board display (see boards/waveshare_c6_lcd147.yaml) ---
  /// Milliseconds since the last complete set of measurements, -1 before the first.
  int64_t ms_since_data() const;
  /// The inverter has fed in nothing for a while: at night (see hiflow_daylog.h).
  bool is_standby() const { return this->daylog_.rec.standby != 0; }
  /// Switched off by the bridge and not fed in since; it switches itself on
  /// again when it wakes in the morning.
  bool inverter_off() const { return hiflow_daylog_inverter_off(&this->daylog_) != 0; }
  /// The day's curve, peak and energy, kept in flash (see hiflow_daylog.h).
  const hiflow_daylog_t &daylog() const { return this->daylog_; }
  /// Seconds until the next connection attempt after a failure, -1 otherwise.
  int retry_in_s() const;
  /// The on/off switch: 1 on, 0 off, -1 when there is no switch.
  int inverter_switch_state() const;
  /// The inverter's power limit in percent as read since the boot, NaN when
  /// unknown or disabled.
  float power_limit_percent() const;
  /// The same, or else the last one read before the boot (the inverter keeps
  /// its limit over the night); NaN when none was ever read.
  float last_power_limit_percent() const;
  /// Local time (unix seconds plus the UTC offset), 0 before the session starts.
  int64_t local_time() const;
  /// 1 once a trusted time arrived since boot (the inverter or the network);
  /// before that the clock runs on the value saved in flash.
  bool clock_synced() const { return hiflow_session_clock_synced(&this->session_) != 0; }
  /// The session status as published (7 folded into 6), -1 before the first.
  int status() const { return this->last_status_; }
  uint32_t sessions() const { return hiflow_session_sessions(&this->session_); }
  uint32_t failures() const { return hiflow_session_failures(&this->session_); }

  // --- session callbacks, reached through the trampolines in the .cpp ---
  int session_send(const uint8_t *frame, size_t len);
  void session_disconnect();
  void session_set_link_allowed(bool allowed);
  void session_on_data(const hiflow_data_t *data);
  void session_on_status(uint8_t status);
  void session_on_power_limit(int32_t tenths);
  void session_on_inverter_power(bool on, bool confirmed);
  void session_log(int level, const char *msg);

 protected:
  /// Last timestamp the bridge used. The inverter refuses logins whose time
  /// goes backwards, so the clock must not restart behind it after a reboot.
  struct TimePref {
    uint16_t magic;
    uint16_t version;
    int64_t unix_time;
  };
  /// The last power limit read from the inverter, for the display.
  struct LimitPref {
    uint16_t magic;
    int16_t tenths;
  };
  ESPPreferenceObject limit_pref_{};
  int32_t last_limit_tenths_{-1};  // -1: never read
  bool limit_save_due_{false};

  int64_t now_ms_() const;
  void start_session_();
  void report_link_up_();
  void report_link_down_(int reason);
  void publish_(uint8_t type, float value);
  // Publishes a lifetime counter unless it is 0 (missing) or below the last one.
  void publish_total_(uint8_t type, float value, float &last);
  void publish_status_(uint8_t status, bool refresh);
  void flush_prefs_();
  void poll_slider_(int64_t now);
  void poll_switch_(int64_t now);
  void poll_network_time_(int64_t now);

  // --- transport ---
  espbt::ESPBTUUID service_uuid_{};
  espbt::ESPBTUUID tx_uuid_{};  // ffe1
  espbt::ESPBTUUID rx_uuid_{};  // ffe2
  uint16_t tx_handle_{0};
  uint16_t rx_handle_{0};
  uint16_t mtu_{23};
  bool link_ready_{false};     // notifications subscribed and reported
  bool link_reported_{false};  // link-down already reported for this connection
  int64_t notify_registered_ms_{0};
  int disconnect_reason_{0};

  // --- configuration (credentials are never logged in full) ---
  std::string sn_;
  std::string ble_id_;
  std::string pin_;
  int32_t std_offset_{3600};
  bool eu_dst_{true};
  uint32_t poll_interval_ms_{30000};

  // --- session core ---
  hiflow_session_t session_{};
  bool session_started_{false};

  // --- persistence ---
  ESPPreferenceObject time_pref_{};
  int64_t next_time_save_ms_{0};

  // --- publishing ---
  sensor::Sensor *sensors_[HIFLOW_SENSOR_TYPE_COUNT]{};
  float last_energy_total_{0.0f};
  float last_port_energy_total_[HIFLOW_MAX_PORTS]{};
  int64_t next_status_refresh_ms_{0};
  int last_status_{-1};
  int64_t last_data_ms_{-1};

  // --- day log (for a display) ---
  hiflow_daylog_t daylog_{};
  ESPPreferenceObject daylog_pref_{};
  int64_t next_daylog_check_ms_{0};

#ifdef USE_ZIGBEE
  // --- power limit slider ---
  zigbee::ZigbeeComponent *slider_zb_{nullptr};
  uint8_t slider_endpoint_{0};
  float slider_set_{NAN};        // the value we put there ourselves
  float slider_seen_{NAN};       // the value read last, for the quiet period
  int64_t slider_seen_ms_{0};
  bool slider_publish_{false};   // slider_set_ still has to reach the attribute
  int64_t next_slider_poll_ms_{0};

  // --- network time ---
  zigbee::ZigbeeComponent *time_zb_{nullptr};
  uint8_t time_endpoint_{0};
  int64_t next_time_request_ms_{0};
  int64_t time_joined_ms_{0};  // when is_joined() last became true, 0 = not joined
  int last_time_status_{-1};

  // --- inverter on/off switch ---
  /// The state the inverter was last switched to (the inverter reports none).
  struct SwitchPref {
    uint16_t magic;
    uint8_t on;
  };
  zigbee::ZigbeeComponent *switch_zb_{nullptr};
  uint8_t switch_endpoint_{0};
  ESPPreferenceObject switch_pref_{};
  bool switch_state_{true};      // confirmed, saved in flash
  bool switch_pending_{false};   // a request is with the session
  bool switch_publish_{false};   // switch_state_ still has to reach the attribute
  int switch_seen_{-1};          // the value read last, for the quiet period
  int64_t switch_seen_ms_{0};
  int64_t next_switch_poll_ms_{0};
#endif
};

}  // namespace hiflow_ble
}  // namespace esphome

#endif  // USE_ESP32
