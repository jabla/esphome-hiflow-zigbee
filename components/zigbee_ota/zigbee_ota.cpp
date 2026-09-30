#include "zigbee_ota.h"

#if defined(USE_ESP32) && defined(USE_ZIGBEE)

#include <algorithm>
#include <cstring>

#include "esp_delta_ota.h"
#include "esp_system.h"
#include "miniz.h"
#include "esp_zigbee.h"
#include "ezbee/zcl/cluster/ota_upgrade.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace zigbee_ota {

static const char *const TAG = "zigbee_ota";

// OTA file layout (ZCL spec 11.4): a header whose length sits at offset 6,
// then sub-elements of tag (2 bytes) + length (4 bytes) + data. The ESP-IDF
// app image comes in one of two sub-elements from the manufacturer tag range,
// written by tools/make_zigbee_ota.py; other sub-elements are skipped.
static const uint32_t OTA_FILE_IDENTIFIER = 0x0BEEF11E;
static const uint16_t TAG_IMAGE_ZLIB = 0xF100;   // zlib stream (RFC 1950) of the app image
static const uint16_t TAG_IMAGE_DELTA = 0xF101;  // esp_delta_ota patch against the running image
static const uint32_t DELTA_MAGIC = 0xFCCDDE10;
static const uint8_t DELTA_HEADER_LEN = 64;      // magic, SHA-256 of the base, reserved
static const uint8_t SUB_ELEMENT_HEADER_LEN = 6;
static const uint32_t QUERY_PERIOD_MS = 30000;
static const uint32_t PROGRESS_LOG_MS = 15000;
static const uint32_t RESTART_DELAY_MS = 2000;

static ZigbeeOta *global_ota = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

/// Tag type that routes ZigbeeComponent::add_attr() to the specialization
/// below, the one place with access to the device descriptor. It carries the
/// cluster's settings.
struct OtaClientCluster {
  uint16_t manufacturer_code;
  uint16_t image_type;
  uint32_t file_version;
};

}  // namespace zigbee_ota

namespace zigbee {

// ZigbeeComponent keeps its device descriptor protected and is final. The
// public add_attr<T>() forwards to the member template add_attr_<T>(); an
// explicit specialization of that member for our own tag type is a member
// function and may use dev_desc_ (same trick as hiflow_ble's power limit). If
// ESPHome changes the signature, this stops compiling instead of misbehaving.
template<>
void ZigbeeComponent::add_attr_<zigbee_ota::OtaClientCluster>(ZigbeeAttribute * /*attr*/, uint8_t endpoint_id,
                                                               uint16_t /*cluster_id*/, uint8_t /*role*/,
                                                               uint16_t /*attr_id*/,
                                                               zigbee_ota::OtaClientCluster *cfg) {
  ezb_af_ep_desc_t ep_desc = ezb_af_device_get_endpoint_desc(this->dev_desc_, endpoint_id);
  if (ep_desc == NULL) {
    ESP_LOGE(zigbee_ota::TAG, "endpoint %u does not exist", endpoint_id);
    return;
  }
  ezb_zcl_ota_upgrade_cluster_client_config_t client_cfg = {
      .upgrade_server_id = EZB_ZCL_OTA_UPGRADE_UPGRADE_SERVER_ID_DEFAULT_VALUE,
      .file_offset = 0,
      .image_upgrade_status = EZB_ZCL_OTA_UPGRADE_IMAGE_UPGRADE_STATUS_DEFAULT_VALUE,
      .manufacturer_id = cfg->manufacturer_code,
      .image_type_id = cfg->image_type,
  };
  ezb_zcl_cluster_desc_t cluster = ezb_zcl_ota_upgrade_create_cluster_desc(&client_cfg, EZB_ZCL_CLUSTER_CLIENT);
  if (cluster == NULL) {
    ESP_LOGE(zigbee_ota::TAG, "could not create the OTA Upgrade client cluster");
    return;
  }
  ezb_zcl_ota_upgrade_cluster_desc_add_attr(cluster, EZB_ZCL_ATTR_OTA_UPGRADE_CURRENT_FILE_VERSION_ID,
                                            &cfg->file_version);
  if (ezb_af_endpoint_add_cluster_desc(ep_desc, cluster) != EZB_ERR_NONE)
    ESP_LOGE(zigbee_ota::TAG, "could not add the OTA Upgrade client cluster to endpoint %u", endpoint_id);
}

}  // namespace zigbee

namespace zigbee_ota {

void ZigbeeOta::add_cluster() {
  OtaClientCluster cfg{this->manufacturer_code_, this->image_type_, this->file_version_};
  this->zb_->add_attr<OtaClientCluster>(this->endpoint_, EZB_ZCL_CLUSTER_ID_OTA_UPGRADE, EZB_ZCL_CLUSTER_CLIENT,
                                        EZB_ZCL_ATTR_OTA_UPGRADE_CURRENT_FILE_VERSION_ID, 0, cfg);
}

void ZigbeeOta::setup() {
  global_ota = this;
  this->boot_ms_ = millis();
  ESP_LOGW(TAG, "Reset reason %d (%s)", esp_reset_reason(),
           esp_reset_reason() == ESP_RST_BROWNOUT ? "brownout" : "see esp_reset_reason_t");

  esp_ota_img_states_t state;
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
    this->pending_verify_ = true;
    ESP_LOGW(TAG, "New image on %s, waiting for the OTA server before marking it valid", running->label);
  }

  // The stack keeps a single action handler; ESPHome's only logs.
  ezb_zcl_core_action_handler_register(ZigbeeOta::action_handler_);

  this->zb_->add_on_start_callback([this]() {
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_ota_upgrade_set_download_block_size(this->endpoint_, this->block_size_);
    int8_t before = 0;
    ezb_get_tx_power(&before);
    if (this->tx_power_ != 127)
      ezb_set_tx_power(this->tx_power_);
    int8_t after = 0;
    ezb_get_tx_power(&after);
    esp_zigbee_lock_release();
    ESP_LOGW(TAG, "TX power %d dBm (stack default %d dBm)", after, before);
  });
}

void ZigbeeOta::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Zigbee OTA:\n"
                "  Endpoint: %u\n"
                "  Manufacturer code: 0x%04X\n"
                "  Image type: 0x%04X\n"
                "  File version: 0x%08" PRIX32 "\n"
                "  Block size: %u",
                this->endpoint_, this->manufacturer_code_, this->image_type_, this->file_version_, this->block_size_);
}

void ZigbeeOta::loop() {
  const uint32_t now = millis();

  if (this->restart_.load()) {
    if (this->restart_at_ms_ == 0) {
      this->restart_at_ms_ = now + RESTART_DELAY_MS;
      ESP_LOGW(TAG, "Rebooting into the new image in %" PRIu32 " ms", RESTART_DELAY_MS);
    } else if (static_cast<int32_t>(now - this->restart_at_ms_) >= 0) {
      App.reboot();
    }
  }

  if (this->pending_verify_) {
    if (this->server_answered_.load()) {
      esp_ota_mark_app_valid_cancel_rollback();
      this->pending_verify_ = false;
      ESP_LOGW(TAG, "OTA server answered, image marked valid");
    } else if (now - this->boot_ms_ > this->verify_timeout_ms_) {
      ESP_LOGE(TAG, "No answer from the OTA server within %" PRIu32 " s, rolling back",
               this->verify_timeout_ms_ / 1000);
      esp_ota_mark_app_invalid_rollback_and_reboot();
    } else if (this->zb_->is_joined() && (this->last_query_ms_ == 0 || now - this->last_query_ms_ > QUERY_PERIOD_MS)) {
      this->last_query_ms_ = now;
      this->send_query_();
    }
  }

  if (this->downloading_.load() && now - this->last_log_ms_ > PROGRESS_LOG_MS) {
    this->last_log_ms_ = now;
    const uint32_t offset = this->offset_.load();
    const uint32_t secs = (now - this->download_start_ms_) / 1000;
    ESP_LOGW(TAG, "Download %" PRIu32 " / %" PRIu32 " bytes (%" PRIu32 " %%), %" PRIu32 " s, %" PRIu32 " B/s", offset,
             this->file_size_, this->file_size_ ? offset * 100 / this->file_size_ : 0, secs,
             secs ? offset / secs : 0);
  }
}

void ZigbeeOta::send_query_() {
  ezb_zcl_ota_upgrade_query_next_image_req_cmd_t req = {};
  req.cmd_ctrl.dst_addr.addr_mode = EZB_ADDR_MODE_SHORT;
  req.cmd_ctrl.dst_addr.u.short_addr = 0x0000;  // the coordinator
  req.cmd_ctrl.dst_ep = 1;
  req.cmd_ctrl.src_ep = this->endpoint_;
  req.payload.fc = 0;
  req.payload.manuf_code = this->manufacturer_code_;
  req.payload.image_type = this->image_type_;
  req.payload.file_version = this->file_version_;
  if (!esp_zigbee_lock_acquire(10 / portTICK_PERIOD_MS))
    return;
  ezb_err_t err = ezb_zcl_ota_upgrade_query_next_image_cmd_req(&req);
  esp_zigbee_lock_release();
  ESP_LOGD(TAG, "Query Next Image sent (%d)", err);
}

void ZigbeeOta::action_handler_(ezb_zcl_core_action_callback_id_t callback_id, void *message) {
  switch (callback_id) {
    case EZB_ZCL_CORE_OTA_UPGRADE_CLIENT_PROGRESS_CB_ID:
      global_ota->on_progress_(message);
      break;
    case EZB_ZCL_CORE_OTA_UPGRADE_QUERY_NEXT_IMAGE_RSP_CB_ID:
      global_ota->on_query_response_(message);
      break;
    default:
      break;
  }
}

void ZigbeeOta::on_query_response_(void *message) {
  auto *msg = static_cast<ezb_zcl_ota_upgrade_query_next_image_rsp_message_t *>(message);
  // Any answer, "no image" included, proves the round trip to the server.
  this->server_answered_ = true;
  if (msg->in.image.status == EZB_ZCL_OTA_UPGRADE_STATUS_CODE_SUCCESS) {
    ESP_LOGW(TAG, "Image offered: type 0x%04X version 0x%08" PRIX32 ", %" PRIu32 " bytes", msg->in.image.image_type,
             msg->in.image.file_version, msg->in.image.size);
  } else {
    ESP_LOGD(TAG, "No image available (0x%02X)", msg->in.image.status);
  }
  msg->out.result = EZB_ZCL_STATUS_SUCCESS;
}

void ZigbeeOta::on_progress_(void *message) {
  auto *msg = static_cast<ezb_zcl_ota_upgrade_client_progress_message_t *>(message);
  ezb_zcl_status_t result = EZB_ZCL_STATUS_SUCCESS;

  switch (msg->in.progress) {
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_START: {
      ESP_LOGW(TAG, "Download starts: version 0x%08" PRIX32 ", %" PRIu32 " bytes", msg->in.start.file_version,
               msg->in.start.image_size);
      if (this->handle_ != 0)
        esp_ota_abort(this->handle_);
      this->handle_ = 0;
      this->file_size_ = msg->in.start.image_size;
      this->header_length_ = 0;
      this->element_left_ = 0;
      this->release_decoders_();
      this->pending_len_ = 0;
      this->image_written_ = 0;
      this->failed_ = false;
      this->offset_ = 0;
      this->partition_ = esp_ota_get_next_update_partition(nullptr);
      // Sequential writes erase sector by sector as the data comes, instead of
      // erasing the whole slot up front and stalling the Zigbee task.
      esp_err_t err = this->partition_ == nullptr
                          ? ESP_ERR_NOT_FOUND
                          : esp_ota_begin(this->partition_, OTA_WITH_SEQUENTIAL_WRITES, &this->handle_);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        this->failed_ = true;
        result = EZB_ZCL_STATUS_ABORT;
        break;
      }
      this->download_start_ms_ = millis();
      this->last_log_ms_ = this->download_start_ms_;
      this->downloading_ = true;
      break;
    }

    case EZB_ZCL_OTA_UPGRADE_PROGRESS_RECEIVING: {
      if (this->failed_) {
        result = EZB_ZCL_STATUS_ABORT;
        break;
      }
      const uint32_t block_offset = msg->in.receiving.file_offset;
      const uint8_t *data = msg->in.receiving.block;
      uint32_t len = msg->in.receiving.block_size;
      if (block_offset != this->offset_.load()) {
        ESP_LOGE(TAG, "Block at %" PRIu32 ", expected %" PRIu32, block_offset, this->offset_.load());
        this->failed_ = true;
        result = EZB_ZCL_STATUS_ABORT;
        break;
      }
      uint32_t pos = block_offset;
      this->offset_ = block_offset + len;

      while (len > 0) {
        // File header: only its length field matters here, the stack already
        // matched manufacturer, type and version.
        if (this->header_length_ == 0 || pos < this->header_length_) {
          if (pos < 8) {
            // bytes 0-3 identifier, 4-5 header version, 6-7 header length
            uint8_t take = std::min<uint32_t>(len, 8 - pos);
            memcpy(this->pending_ + pos, data, take);
            pos += take;
            data += take;
            len -= take;
            if (pos == 8) {
              uint32_t ident;
              memcpy(&ident, this->pending_, 4);
              this->header_length_ = this->pending_[6] | (this->pending_[7] << 8);
              if (ident != OTA_FILE_IDENTIFIER || this->header_length_ < 56) {
                ESP_LOGE(TAG, "Not an OTA file (identifier 0x%08" PRIX32 ", header %" PRIu32 ")", ident,
                         this->header_length_);
                this->failed_ = true;
                result = EZB_ZCL_STATUS_INVALID_IMAGE;
                break;
              }
            }
            continue;
          }
          uint32_t skip = std::min<uint32_t>(len, this->header_length_ - pos);
          pos += skip;
          data += skip;
          len -= skip;
          continue;
        }
        if (this->element_left_ == 0) {
          // Sub-element header, possibly split across two blocks.
          uint8_t take = std::min<uint32_t>(len, SUB_ELEMENT_HEADER_LEN - this->pending_len_);
          memcpy(this->pending_ + this->pending_len_, data, take);
          this->pending_len_ += take;
          pos += take;
          data += take;
          len -= take;
          if (this->pending_len_ == SUB_ELEMENT_HEADER_LEN) {
            uint16_t tag = this->pending_[0] | (this->pending_[1] << 8);
            uint32_t length;
            memcpy(&length, this->pending_ + 2, 4);
            this->pending_len_ = 0;
            this->element_left_ = length;
            if (!this->begin_image_(tag, length)) {
              this->failed_ = true;
              result = EZB_ZCL_STATUS_INVALID_IMAGE;
              break;
            }
          }
          continue;
        }
        uint32_t take = std::min(len, this->element_left_);
        if (this->format_ != ImageFormat::NONE) {
          esp_err_t err = this->write_image_(data, take, take == this->element_left_);
          if (err != ESP_OK) {
            ESP_LOGE(TAG, "Writing the image failed at %" PRIu32 ": %s", this->image_written_, esp_err_to_name(err));
            this->failed_ = true;
            result = EZB_ZCL_STATUS_ABORT;
            break;
          }
        }
        this->element_left_ -= take;
        pos += take;
        data += take;
        len -= take;
      }
      break;
    }

    case EZB_ZCL_OTA_UPGRADE_PROGRESS_CHECK: {
      this->downloading_ = false;
      const uint32_t secs = (millis() - this->download_start_ms_) / 1000;
      ESP_LOGW(TAG, "Download complete: %" PRIu32 " bytes in %" PRIu32 " s, image %" PRIu32 " bytes",
               this->offset_.load(), secs, this->image_written_);
      if (this->failed_ || this->offset_.load() != this->file_size_ || this->element_left_ != 0 ||
          this->finish_image_() != ESP_OK || this->image_written_ == 0) {
        ESP_LOGE(TAG, "Incomplete download");
        this->release_decoders_();
        result = EZB_ZCL_STATUS_INVALID_IMAGE;
        break;
      }
      // esp_ota_end() checks the app image (magic, segments, SHA-256), which
      // also catches a delta applied to the wrong base.
      esp_err_t err = esp_ota_end(this->handle_);
      this->handle_ = 0;
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Image check failed: %s", esp_err_to_name(err));
        this->failed_ = true;
        result = EZB_ZCL_STATUS_INVALID_IMAGE;
      }
      break;
    }

    case EZB_ZCL_OTA_UPGRADE_PROGRESS_APPLY: {
      esp_err_t err = this->failed_ ? ESP_FAIL : esp_ota_set_boot_partition(this->partition_);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not select the new image: %s", esp_err_to_name(err));
        result = EZB_ZCL_STATUS_ABORT;
      } else {
        ESP_LOGW(TAG, "New image selected for the next boot (%s)", this->partition_->label);
        // Workaround for esp-zigbee-lib 2.0.4: after a successful APPLY the
        // stack calls FINISH and then sends a response packet it never set
        // up (disassembly of ota_upgrade_cluster_cli_cmd_proc_handler); the
        // download context lookup for that packet's endpoint fails an assert
        // and the chip panics before it acknowledged the server's Upgrade End
        // Response (zigpy then reports the update as failed). Espressif's
        // ota_client example hides this by restarting inside FINISH. Any
        // APPLY result other than 0x96/0x99 takes the stack's default
        // response path instead, which is sound. The image is already
        // selected, so the reboot is ours: from the main loop, with a moment
        // for the acknowledgements to go out.
        this->restart_ = true;
        result = EZB_ZCL_STATUS_ABORT;
      }
      break;
    }

    case EZB_ZCL_OTA_UPGRADE_PROGRESS_FINISH:
      // Not reached while APPLY answers ABORT (see above); kept for a stack
      // that fixes the FINISH path.
      ESP_LOGW(TAG, "Upgrade finished, restart in %" PRIu32 " s", msg->in.finish.count_down_delay);
      this->restart_ = true;
      break;

    case EZB_ZCL_OTA_UPGRADE_PROGRESS_ABORT:
      ESP_LOGW(TAG, "Download aborted at %" PRIu32 " bytes", this->offset_.load());
      this->downloading_ = false;
      this->release_decoders_();
      if (this->handle_ != 0)
        esp_ota_abort(this->handle_);
      this->handle_ = 0;
      break;

    default:
      break;
  }
  msg->out.result = result;
}

bool ZigbeeOta::begin_image_(uint16_t tag, uint32_t length) {
  this->release_decoders_();
  switch (tag) {
    case TAG_IMAGE_ZLIB:
      this->inflator_ = calloc(1, sizeof(tinfl_decompressor));
      this->window_ = static_cast<uint8_t *>(malloc(TINFL_LZ_DICT_SIZE));
      if (this->inflator_ == nullptr || this->window_ == nullptr) {
        ESP_LOGE(TAG, "No memory for the inflater");
        this->release_decoders_();
        return false;
      }
      tinfl_init(static_cast<tinfl_decompressor *>(this->inflator_));
      this->format_ = ImageFormat::ZLIB;
      break;
    case TAG_IMAGE_DELTA:
      this->running_ = esp_ota_get_running_partition();
      this->format_ = ImageFormat::DELTA;
      break;
    default:
      // Some other sub-element (signature, ...): skipped.
      this->format_ = ImageFormat::NONE;
      return true;
  }
  ESP_LOGW(TAG, "Image sub-element 0x%04X, %" PRIu32 " bytes", tag, length);
  return true;
}

esp_err_t ZigbeeOta::write_output_(const uint8_t *data, size_t len) {
  esp_err_t err = esp_ota_write(this->handle_, data, len);
  if (err == ESP_OK)
    this->image_written_ += len;
  return err;
}

esp_err_t ZigbeeOta::write_image_(const uint8_t *data, uint32_t len, bool last) {
  switch (this->format_) {
    case ImageFormat::ZLIB:
      return this->inflate_(data, len, last);
    case ImageFormat::DELTA: {
      if (this->delta_ == nullptr) {
        // The header first: refuse a patch for another base before touching
        // the patch engine.
        uint8_t take = std::min<uint32_t>(len, DELTA_HEADER_LEN - this->delta_header_len_);
        memcpy(this->delta_header_ + this->delta_header_len_, data, take);
        this->delta_header_len_ += take;
        data += take;
        len -= take;
        if (this->delta_header_len_ < DELTA_HEADER_LEN)
          return ESP_OK;
        uint32_t magic;
        memcpy(&magic, this->delta_header_, 4);
        uint8_t running_sha[32];
        esp_partition_get_sha256(this->running_, running_sha);
        if (magic != DELTA_MAGIC || memcmp(running_sha, this->delta_header_ + 4, 32) != 0) {
          ESP_LOGE(TAG, "Delta patch is not for the running image (%s)", magic != DELTA_MAGIC ? "magic" : "SHA-256");
          return ESP_ERR_INVALID_VERSION;
        }
        esp_delta_ota_cfg_t cfg = {};
        cfg.user_data = this;
        cfg.read_cb_with_user_data = [](uint8_t *buf, size_t size, int src_offset, void *user) -> esp_err_t {
          return esp_partition_read(static_cast<ZigbeeOta *>(user)->running_, src_offset, buf, size);
        };
        cfg.write_cb_with_user_data = [](const uint8_t *buf, size_t size, void *user) -> esp_err_t {
          return static_cast<ZigbeeOta *>(user)->write_output_(buf, size);
        };
        this->delta_ = esp_delta_ota_init(&cfg);
        if (this->delta_ == nullptr)
          return ESP_ERR_NO_MEM;
        ESP_LOGW(TAG, "Delta patch matches the running image");
      }
      return len == 0 ? ESP_OK : esp_delta_ota_feed_patch(this->delta_, data, len);
    }
    case ImageFormat::NONE:
      break;
  }
  return ESP_OK;
}

esp_err_t ZigbeeOta::inflate_(const uint8_t *data, uint32_t len, bool last) {
  auto *inflator = static_cast<tinfl_decompressor *>(this->inflator_);
  const int flags = TINFL_FLAG_PARSE_ZLIB_HEADER | (last ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
  while (!this->inflate_done_) {
    size_t in_bytes = len;
    size_t out_bytes = TINFL_LZ_DICT_SIZE - this->window_pos_;
    tinfl_status status = tinfl_decompress(inflator, data, &in_bytes, this->window_, this->window_ + this->window_pos_,
                                           &out_bytes, flags);
    data += in_bytes;
    len -= in_bytes;
    if (out_bytes > 0) {
      esp_err_t err = this->write_output_(this->window_ + this->window_pos_, out_bytes);
      if (err != ESP_OK)
        return err;
      this->window_pos_ = (this->window_pos_ + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
    }
    if (status < TINFL_STATUS_DONE) {
      ESP_LOGE(TAG, "Inflate failed (%d)", status);
      return ESP_FAIL;
    }
    if (status == TINFL_STATUS_DONE)
      this->inflate_done_ = true;
    else if (status == TINFL_STATUS_NEEDS_MORE_INPUT)
      break;
    // TINFL_STATUS_HAS_MORE_OUTPUT: the window is full, go round again.
  }
  return ESP_OK;
}

esp_err_t ZigbeeOta::finish_image_() {
  switch (this->format_) {
    case ImageFormat::ZLIB:
      if (!this->inflate_done_) {
        ESP_LOGE(TAG, "zlib stream ended early");
        return ESP_FAIL;
      }
      break;
    case ImageFormat::DELTA: {
      if (this->delta_ == nullptr)
        return ESP_FAIL;
      esp_err_t err = esp_delta_ota_finalize(this->delta_);
      esp_delta_ota_deinit(this->delta_);
      this->delta_ = nullptr;
      if (err != ESP_OK)
        return err;
      break;
    }
    default:
      break;
  }
  this->release_decoders_();
  return ESP_OK;
}

void ZigbeeOta::release_decoders_() {
  free(this->inflator_);
  this->inflator_ = nullptr;
  free(this->window_);
  this->window_ = nullptr;
  this->window_pos_ = 0;
  this->inflate_done_ = false;
  if (this->delta_ != nullptr)
    esp_delta_ota_deinit(this->delta_);
  this->delta_ = nullptr;
  this->delta_header_len_ = 0;
  this->format_ = ImageFormat::NONE;
}

}  // namespace zigbee_ota
}  // namespace esphome

#endif  // USE_ESP32 && USE_ZIGBEE
