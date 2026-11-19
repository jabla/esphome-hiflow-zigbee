#include "hiflow_setup_field.h"

#if defined(USE_ESP32) && defined(USE_ZIGBEE)

#include "esphome/core/log.h"

#include <cstring>

#include "hiflow_setup.h"

namespace esphome {
namespace hiflow_ble {

static const char *const TAG = "hiflow_ble.setup";

/// Tag type that routes ZigbeeComponent::add_attr() to the specialization
/// below. add_attr() is the public way to reach the device descriptor before
/// the Zigbee task registers it; the specialization runs there with the
/// endpoint list (dev_desc_).
struct SetupFieldAttr {};

}  // namespace hiflow_ble

namespace zigbee {

template<>
void ZigbeeComponent::add_attr_<hiflow_ble::SetupFieldAttr>(ZigbeeAttribute * /*attr*/, uint8_t endpoint_id,
                                                             uint16_t /*cluster_id*/, uint8_t /*role*/,
                                                             uint16_t /*attr_id*/,
                                                             hiflow_ble::SetupFieldAttr * /*value_p*/) {
  ezb_af_ep_desc_t ep_desc = ezb_af_device_get_endpoint_desc(this->dev_desc_, endpoint_id);
  if (ep_desc == NULL) {
    ESP_LOGE(hiflow_ble::TAG, "endpoint %u does not exist", endpoint_id);
    return;
  }
  ezb_zcl_cluster_desc_t basic = ezb_af_endpoint_get_cluster_desc(ep_desc, EZB_ZCL_CLUSTER_ID_BASIC,
                                                                   EZB_ZCL_CLUSTER_SERVER);
  if (basic == NULL) {
    ESP_LOGE(hiflow_ble::TAG, "endpoint %u has no Basic cluster", endpoint_id);
    return;
  }
  // ESPHome creates LocationDescription from the area text, and the stack
  // sized the attribute by that first value: hiflow-zb.yaml's area has the 15
  // characters the field needs (the build checks it).
  ezb_zcl_attr_desc_t attr =
      ezb_zcl_cluster_get_attr_desc(basic, EZB_ZCL_ATTR_BASIC_LOCATION_DESCRIPTION_ID, EZB_ZCL_STD_MANUF_CODE);
  if (attr == NULL || ezb_zcl_attr_desc_get_type(attr) != EZB_ZCL_ATTR_TYPE_STRING) {
    ESP_LOGE(hiflow_ble::TAG, "endpoint %u has no LocationDescription to take over", endpoint_id);
    return;
  }
  const uint16_t size = ezb_zcl_attr_desc_get_value_size(attr);
  if (size < 1 + HIFLOW_SETUP_TEXT_MAX) {
    ESP_LOGE(hiflow_ble::TAG, "LocationDescription holds %u bytes, the settings field needs %u", size,
             1 + HIFLOW_SETUP_TEXT_MAX);
    return;
  }
  // Writable whatever the library's own table says, and empty: all NUL reads
  // as an empty text, so the area never shows.
  uint8_t empty[1 + HIFLOW_SETUP_TEXT_MAX] = {};
  empty[0] = HIFLOW_SETUP_TEXT_MAX;
  if (ezb_zcl_attr_desc_set_access(attr, EZB_ZCL_ATTR_ACCESS_READ_WRITE) != EZB_ERR_NONE ||
      ezb_zcl_attr_desc_set_value(attr, empty) != EZB_ERR_NONE)
    ESP_LOGE(hiflow_ble::TAG, "could not take over LocationDescription on endpoint %u", endpoint_id);
}

}  // namespace zigbee

namespace hiflow_ble {

void add_setup_field(zigbee::ZigbeeComponent *zb, uint8_t endpoint) {
  zb->add_attr<SetupFieldAttr>(endpoint, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER,
                               EZB_ZCL_ATTR_BASIC_LOCATION_DESCRIPTION_ID, 0, SetupFieldAttr{});
}

bool exchange_setup_field(uint8_t endpoint, std::string *text, const std::string &ours,
                          const std::string *also_ours, const std::string *next, bool *wrote) {
  uint8_t buf[1 + 255];
  uint8_t out[1 + HIFLOW_SETUP_TEXT_MAX];
  *wrote = false;
  if (next != nullptr) {
    const size_t len = next->size() > HIFLOW_SETUP_TEXT_MAX ? HIFLOW_SETUP_TEXT_MAX : next->size();
    out[0] = static_cast<uint8_t>(len);
    std::memcpy(out + 1, next->data(), len);
  }

  if (!esp_zigbee_lock_acquire(10 / portTICK_PERIOD_MS))
    return false;
  bool ok = false;
  bool write = false;
  ezb_zcl_status_t status = EZB_ZCL_STATUS_SUCCESS;
  ezb_zcl_attr_desc_t attr = ezb_zcl_get_attr_desc(endpoint, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER,
                                                   EZB_ZCL_ATTR_BASIC_LOCATION_DESCRIPTION_ID, EZB_ZCL_STD_MANUF_CODE);
  if (attr != NULL && ezb_zcl_attr_desc_get_value_size(attr) <= sizeof(buf))
    ok = ezb_zcl_attr_desc_get_value(attr, buf) == EZB_ERR_NONE;
  if (ok) {
    // A ZCL string: the length, then the characters. Cut at the first NUL (the
    // padding of the first value), never past what the field can hold.
    size_t len = buf[0] == 0xFF ? 0 : buf[0];
    if (len > HIFLOW_SETUP_TEXT_MAX)
      len = HIFLOW_SETUP_TEXT_MAX;
    const void *nul = std::memchr(buf + 1, '\0', len);
    if (nul != nullptr)
      len = static_cast<const uint8_t *>(nul) - (buf + 1);
    text->assign(reinterpret_cast<const char *>(buf + 1), len);  // 15 characters at most: no heap
    write = next != nullptr && (*text == ours || (also_ours != nullptr && *text == *also_ours));
    if (write)
      status = ezb_zcl_set_attr_value(endpoint, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER,
                                      EZB_ZCL_ATTR_BASIC_LOCATION_DESCRIPTION_ID, EZB_ZCL_STD_MANUF_CODE, out, false);
  }
  esp_zigbee_lock_release();

  if (write) {
    if (status == EZB_ZCL_STATUS_SUCCESS) {
      *wrote = true;
    } else {
      ESP_LOGW(TAG, "setting the settings field failed, ZCL status %u", static_cast<unsigned>(status));
    }
  }
  return ok;
}

}  // namespace hiflow_ble
}  // namespace esphome

#endif  // USE_ESP32 && USE_ZIGBEE
