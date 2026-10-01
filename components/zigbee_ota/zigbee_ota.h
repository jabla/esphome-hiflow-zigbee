#pragma once

// ZCL OTA Upgrade client for ESPHome's zigbee component on the ESP32-C6; see
// __init__.py for the whole picture.
//
// Threads: the stack calls the progress handler on the Zigbee task. Flash
// writes happen there (OTA_WITH_SEQUENTIAL_WRITES erases sector by sector, so
// no single call blocks the stack for long); everything the main loop needs
// goes through atomics.

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_ZIGBEE)

#include <atomic>

#include "esp_ota_ops.h"
#include "esphome/components/zigbee/zigbee_esp32.h"
#include "esphome/core/component.h"

namespace esphome {
namespace zigbee_ota {

class ZigbeeOta : public Component {
 public:
  void set_zigbee(zigbee::ZigbeeComponent *zb) { this->zb_ = zb; }
  void set_endpoint(uint8_t endpoint) { this->endpoint_ = endpoint; }
  void set_manufacturer_code(uint16_t code) { this->manufacturer_code_ = code; }
  void set_image_type(uint16_t type) { this->image_type_ = type; }
  void set_block_size(uint8_t size) { this->block_size_ = size; }
  void set_verify_timeout(uint32_t ms) { this->verify_timeout_ms_ = ms; }
  void set_file_version(uint32_t version) { this->file_version_ = version; }
  void set_tx_power(int8_t dbm) { this->tx_power_ = dbm; }

  /// Adds the OTA Upgrade client cluster to the endpoint. Runs from the
  /// generated setup code, after the zigbee codegen created the endpoint and
  /// before the Zigbee task registers the device.
  void add_cluster();

  void setup() override;
  void loop() override;
  void dump_config() override;
  // After ZigbeeComponent::setup(), which registers ESPHome's action handler:
  // ours replaces it (the stack keeps one) and forwards nothing it needs.
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  static void action_handler_(uint32_t callback_id, void *message);
  void on_progress_(void *message);
  void on_query_response_(void *message);
  void send_query_();
  // The image inside the OTA file comes zlib-compressed, or as an
  // esp_delta_ota patch against the running image.
  enum class ImageFormat : uint8_t { NONE, ZLIB, DELTA };
  bool begin_image_(uint16_t tag, uint32_t length);
  esp_err_t write_image_(const uint8_t *data, uint32_t len, bool last);
  esp_err_t finish_image_();
  void release_decoders_();
  esp_err_t inflate_(const uint8_t *data, uint32_t len, bool last);
  esp_err_t write_output_(const uint8_t *data, size_t len);

  zigbee::ZigbeeComponent *zb_{nullptr};
  uint8_t endpoint_{1};
  uint16_t manufacturer_code_{0x131B};
  uint16_t image_type_{0x4846};
  uint8_t block_size_{223};
  uint32_t verify_timeout_ms_{600000};
  uint32_t file_version_{0};
  int8_t tx_power_{127};  // 127 = leave the stack default

  // Download state, owned by the Zigbee task.
  const esp_partition_t *partition_{nullptr};
  esp_ota_handle_t handle_{0};
  uint32_t file_size_{0};
  uint32_t header_length_{0};      // OTA file header, from its own length field
  uint32_t element_left_{0};       // bytes left in the current sub-element
  ImageFormat format_{ImageFormat::NONE};  // of the current sub-element
  uint8_t pending_[8];             // a sub-element header split across blocks
  uint8_t pending_len_{0};
  uint32_t image_written_{0};      // bytes of app image written to flash
  // zlib: miniz's inflater (in ROM) and its 32 KB output window.
  void *inflator_{nullptr};
  uint8_t *window_{nullptr};
  size_t window_pos_{0};
  bool inflate_done_{false};
  // delta: esp_delta_ota patch header (magic + SHA-256 of the base image),
  // then the detools patch.
  const esp_partition_t *running_{nullptr};
  void *delta_{nullptr};
  uint8_t delta_header_[64];
  uint8_t delta_header_len_{0};
  bool failed_{false};

  // Shared with the main loop.
  std::atomic<uint32_t> offset_{0};
  std::atomic<bool> downloading_{false};
  std::atomic<bool> server_answered_{false};
  std::atomic<bool> restart_{false};

  // Main loop only.
  bool pending_verify_{false};
  uint32_t boot_ms_{0};
  uint32_t last_query_ms_{0};
  uint32_t last_log_ms_{0};
  uint32_t download_start_ms_{0};
  uint32_t restart_at_ms_{0};
};

}  // namespace zigbee_ota
}  // namespace esphome

#endif  // USE_ESP32 && USE_ZIGBEE
