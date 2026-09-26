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
#include "hiflow_session.h"

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

  // --- session callbacks, reached through the trampolines in the .cpp ---
  int session_send(const uint8_t *frame, size_t len);
  void session_disconnect();
  void session_set_link_allowed(bool allowed);
  void session_on_data(const hiflow_data_t *data);
  void session_on_status(uint8_t status);
  void session_log(int level, const char *msg);

 protected:
  /// Last timestamp the bridge used. The inverter refuses logins whose time
  /// goes backwards, so the clock must not restart behind it after a reboot.
  struct TimePref {
    uint16_t magic;
    uint16_t version;
    int64_t unix_time;
  };

  int64_t now_ms_() const;
  void start_session_();
  void report_link_up_();
  void report_link_down_(int reason);
  void publish_(uint8_t type, float value);
  // Publishes a lifetime counter unless it is 0 (missing) or below the last one.
  void publish_total_(uint8_t type, float value, float &last);
  void publish_status_(uint8_t status, bool refresh);
  void flush_prefs_();

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
};

}  // namespace hiflow_ble
}  // namespace esphome

#endif  // USE_ESP32
