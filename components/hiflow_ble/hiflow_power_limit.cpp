#include "hiflow_power_limit.h"

#if defined(USE_ESP32) && defined(USE_ZIGBEE)

#include "esphome/core/log.h"

#include <cmath>
#include <cstring>

namespace esphome {
namespace hiflow_ble {

static const char *const TAG = "hiflow_ble.power_limit";

// BACnet engineering unit "percent", the unit table the Analog clusters use.
static const uint16_t BACNET_UNIT_PERCENT = 98;
static const char *const DESCRIPTION = "HiFlow Power Limit";

/// Tag type that routes ZigbeeComponent::add_attr() to the specialization
/// below, which is the one place with access to the device descriptor.
struct PowerLimitCluster {};

}  // namespace hiflow_ble

namespace zigbee {

// ZigbeeComponent keeps its device descriptor protected and is final, and its
// public add_attr() only knows the clusters ESPHome builds itself. The public
// add_attr<T>() forwards to the member template add_attr_<T>(); an explicit
// specialization of that member for our own tag type is a member function and
// may use dev_desc_. If ESPHome changes the signature, this stops compiling
// instead of misbehaving.
template<>
void ZigbeeComponent::add_attr_<hiflow_ble::PowerLimitCluster>(ZigbeeAttribute * /*attr*/, uint8_t endpoint_id,
                                                                uint16_t /*cluster_id*/, uint8_t /*role*/,
                                                                uint16_t /*attr_id*/,
                                                                hiflow_ble::PowerLimitCluster * /*value_p*/) {
  ezb_af_ep_desc_t ep_desc = ezb_af_device_get_endpoint_desc(this->dev_desc_, endpoint_id);
  if (ep_desc == NULL) {
    ESP_LOGE(hiflow_ble::TAG, "endpoint %u does not exist", endpoint_id);
    return;
  }

  // The present value stays unknown (NaN) until the session has read the
  // limit from the inverter: the slider never shows a guess.
  ezb_zcl_analog_output_cluster_server_config_t cfg = {};
  cfg.out_of_service = false;
  cfg.present_value = NAN;
  cfg.status_flags = 0;
  ezb_zcl_cluster_desc_t cluster = ezb_zcl_analog_output_create_cluster_desc(&cfg, EZB_ZCL_CLUSTER_SERVER);
  if (cluster == NULL) {
    ESP_LOGE(hiflow_ble::TAG, "could not create the Analog Output cluster");
    return;
  }

  // The inverter takes 10 % steps only; ZHA uses the resolution as the step.
  float min_value = 0.0f, max_value = 100.0f, resolution = 10.0f;
  uint16_t units = hiflow_ble::BACNET_UNIT_PERCENT;
  uint8_t *description = get_zcl_string(hiflow_ble::DESCRIPTION, 31);

  ezb_zcl_analog_output_cluster_desc_add_attr(cluster, EZB_ZCL_ATTR_ANALOG_OUTPUT_MIN_PRESENT_VALUE_ID, &min_value);
  ezb_zcl_analog_output_cluster_desc_add_attr(cluster, EZB_ZCL_ATTR_ANALOG_OUTPUT_MAX_PRESENT_VALUE_ID, &max_value);
  ezb_zcl_analog_output_cluster_desc_add_attr(cluster, EZB_ZCL_ATTR_ANALOG_OUTPUT_RESOLUTION_ID, &resolution);
  ezb_zcl_analog_output_cluster_desc_add_attr(cluster, EZB_ZCL_ATTR_ANALOG_OUTPUT_ENGINEERING_UNITS_ID, &units);
  ezb_zcl_analog_output_cluster_desc_add_attr(cluster, EZB_ZCL_ATTR_ANALOG_OUTPUT_DESCRIPTION_ID, description);
  delete[] description;

  if (ezb_af_endpoint_add_cluster_desc(ep_desc, cluster) != EZB_ERR_NONE)
    ESP_LOGE(hiflow_ble::TAG, "could not add the Analog Output cluster to endpoint %u", endpoint_id);
}

}  // namespace zigbee

namespace hiflow_ble {

void add_power_limit_cluster(zigbee::ZigbeeComponent *zb, uint8_t endpoint) {
  zb->add_attr<PowerLimitCluster>(endpoint, EZB_ZCL_CLUSTER_ID_ANALOG_OUTPUT, EZB_ZCL_CLUSTER_SERVER,
                                  EZB_ZCL_ATTR_ANALOG_OUTPUT_PRESENT_VALUE_ID, 0, PowerLimitCluster{});
}

bool read_power_limit_attr(uint8_t endpoint, float *value) {
  bool ok = false;

  if (!esp_zigbee_lock_acquire(10 / portTICK_PERIOD_MS))
    return false;
  ezb_zcl_attr_desc_t attr =
      ezb_zcl_get_attr_desc(endpoint, EZB_ZCL_CLUSTER_ID_ANALOG_OUTPUT, EZB_ZCL_CLUSTER_SERVER,
                            EZB_ZCL_ATTR_ANALOG_OUTPUT_PRESENT_VALUE_ID, EZB_ZCL_STD_MANUF_CODE);
  if (attr != NULL)
    ok = ezb_zcl_attr_desc_get_value(attr, value) == EZB_ERR_NONE;
  esp_zigbee_lock_release();
  return ok;
}

bool write_power_limit_attr(zigbee::ZigbeeComponent *zb, uint8_t endpoint, float value) {
  if (!esp_zigbee_lock_acquire(10 / portTICK_PERIOD_MS))
    return false;
  ezb_zcl_status_t status =
      ezb_zcl_set_attr_value(endpoint, EZB_ZCL_CLUSTER_ID_ANALOG_OUTPUT, EZB_ZCL_CLUSTER_SERVER,
                             EZB_ZCL_ATTR_ANALOG_OUTPUT_PRESENT_VALUE_ID, EZB_ZCL_STD_MANUF_CODE, &value, false);
  if (status != EZB_ZCL_STATUS_SUCCESS) {
    esp_zigbee_lock_release();
    ESP_LOGE(TAG, "setting the slider failed, ZCL status %u", static_cast<unsigned>(status));
    return false;
  }
  // Same report as ESPHome's own attributes with `report: force`: straight to
  // the coordinator, independent of any reporting configuration.
  if (zb->is_joined()) {
    ezb_zcl_report_attr_cmd_t cmd = {};
    cmd.cmd_ctrl.fc.direction = EZB_ZCL_CMD_DIRECTION_TO_CLI;
    cmd.cmd_ctrl.fc.dis_default_rsp = 1;
    cmd.cmd_ctrl.dst_addr.addr_mode = EZB_ADDR_MODE_SHORT;
    cmd.cmd_ctrl.dst_addr.u.short_addr = 0x0000;
    cmd.cmd_ctrl.dst_ep = 1;
    cmd.cmd_ctrl.src_ep = endpoint;
    cmd.cmd_ctrl.cluster_id = EZB_ZCL_CLUSTER_ID_ANALOG_OUTPUT;
    cmd.cmd_ctrl.fc.manuf_specific = 0;
    cmd.payload.attr_id = EZB_ZCL_ATTR_ANALOG_OUTPUT_PRESENT_VALUE_ID;
    ezb_zcl_report_attr_cmd_req(&cmd);
  }
  esp_zigbee_lock_release();
  return true;
}

}  // namespace hiflow_ble
}  // namespace esphome

#endif  // USE_ESP32 && USE_ZIGBEE
