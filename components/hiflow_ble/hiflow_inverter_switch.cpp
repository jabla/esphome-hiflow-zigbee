#include "hiflow_inverter_switch.h"

#if defined(USE_ESP32) && defined(USE_ZIGBEE)

#include "esphome/core/log.h"

namespace esphome {
namespace hiflow_ble {

static const char *const TAG = "hiflow_ble.switch";

/// Tag type for the add_attr_ specialization below (see hiflow_power_limit.cpp).
struct InverterSwitchCluster {};

}  // namespace hiflow_ble

namespace zigbee {

template<>
void ZigbeeComponent::add_attr_<hiflow_ble::InverterSwitchCluster>(ZigbeeAttribute * /*attr*/, uint8_t endpoint_id,
                                                                    uint16_t /*cluster_id*/, uint8_t /*role*/,
                                                                    uint16_t /*attr_id*/,
                                                                    hiflow_ble::InverterSwitchCluster * /*value_p*/) {
  ezb_af_ep_desc_t ep_desc = ezb_af_device_get_endpoint_desc(this->dev_desc_, endpoint_id);
  if (ep_desc == NULL) {
    ESP_LOGE(hiflow_ble::TAG, "endpoint %u does not exist", endpoint_id);
    return;
  }

  // Starts as on, the inverter's normal state; the owner puts the state it
  // saved in flash there before the device joins.
  ezb_zcl_on_off_cluster_server_config_t cfg = {};
  cfg.on_off = true;
  ezb_zcl_cluster_desc_t cluster = ezb_zcl_on_off_create_cluster_desc(&cfg, EZB_ZCL_CLUSTER_SERVER);
  if (cluster == NULL) {
    ESP_LOGE(hiflow_ble::TAG, "could not create the On/Off cluster");
    return;
  }
  if (ezb_af_endpoint_add_cluster_desc(ep_desc, cluster) != EZB_ERR_NONE)
    ESP_LOGE(hiflow_ble::TAG, "could not add the On/Off cluster to endpoint %u", endpoint_id);
}

}  // namespace zigbee

namespace hiflow_ble {

void add_inverter_switch_cluster(zigbee::ZigbeeComponent *zb, uint8_t endpoint) {
  zb->add_attr<InverterSwitchCluster>(endpoint, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_SERVER,
                                      EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, 0, InverterSwitchCluster{});
}

bool read_inverter_switch_attr(uint8_t endpoint, bool *on) {
  bool ok = false;

  if (!esp_zigbee_lock_acquire(10 / portTICK_PERIOD_MS))
    return false;
  ezb_zcl_attr_desc_t attr = ezb_zcl_get_attr_desc(endpoint, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_SERVER,
                                                   EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, EZB_ZCL_STD_MANUF_CODE);
  if (attr != NULL)
    ok = ezb_zcl_attr_desc_get_value(attr, on) == EZB_ERR_NONE;
  esp_zigbee_lock_release();
  return ok;
}

bool write_inverter_switch_attr(zigbee::ZigbeeComponent *zb, uint8_t endpoint, bool on) {
  if (!esp_zigbee_lock_acquire(10 / portTICK_PERIOD_MS))
    return false;
  ezb_zcl_status_t status = ezb_zcl_set_attr_value(endpoint, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_SERVER,
                                                   EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, EZB_ZCL_STD_MANUF_CODE, &on, false);
  if (status != EZB_ZCL_STATUS_SUCCESS) {
    esp_zigbee_lock_release();
    ESP_LOGE(TAG, "setting the switch failed, ZCL status %u", static_cast<unsigned>(status));
    return false;
  }
  // Reported straight to the coordinator, like the power limit slider.
  if (zb->is_joined()) {
    ezb_zcl_report_attr_cmd_t cmd = {};
    cmd.cmd_ctrl.fc.direction = EZB_ZCL_CMD_DIRECTION_TO_CLI;
    cmd.cmd_ctrl.fc.dis_default_rsp = 1;
    cmd.cmd_ctrl.dst_addr.addr_mode = EZB_ADDR_MODE_SHORT;
    cmd.cmd_ctrl.dst_addr.u.short_addr = 0x0000;
    cmd.cmd_ctrl.dst_ep = 1;
    cmd.cmd_ctrl.src_ep = endpoint;
    cmd.cmd_ctrl.cluster_id = EZB_ZCL_CLUSTER_ID_ON_OFF;
    cmd.cmd_ctrl.fc.manuf_specific = 0;
    cmd.payload.attr_id = EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID;
    ezb_zcl_report_attr_cmd_req(&cmd);
  }
  esp_zigbee_lock_release();
  return true;
}

}  // namespace hiflow_ble
}  // namespace esphome

#endif  // USE_ESP32 && USE_ZIGBEE
