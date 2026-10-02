#include "hiflow_zigbee_time.h"

#if defined(USE_ESP32) && defined(USE_ZIGBEE)

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <atomic>

namespace esphome {
namespace hiflow_ble {

static const char *const TAG = "hiflow_ble.time";

// Zigbee counts from 2000-01-01.
static const uint32_t UNIX_2000 = 946684800u;
// The coordinator's application endpoint: 1 in zigpy (ZHA) and zigbee-herdsman
// (Zigbee2MQTT), both of which answer a Time read there from the host clock.
static const uint8_t COORDINATOR_EP = 1;
static const uint32_t ANSWER_TIMEOUT_MS = 10000;

static const uint16_t ATTR_TIME = 0x0000;
static const uint16_t ATTR_TIME_STATUS = 0x0001;
static const uint8_t TIME_STATUS_MASTER = 0x01;
static const uint8_t TIME_STATUS_SYNCHRONIZED = 0x02;

// Written from the Zigbee task, read from the main loop.
static std::atomic<uint32_t> g_network_time{0};
static std::atomic<int> g_status{NETWORK_TIME_NONE};
static std::atomic<uint32_t> g_request_ms{0};
static std::atomic<bool> g_pending{false};
static uint8_t g_endpoint = 0;
static bool g_handler_registered = false;

/// Tag type for the add_attr_ specialization below (see hiflow_power_limit.cpp).
struct TimeCluster {};

// Parses the coordinator's Read Attributes Response. Records: attribute id
// (2 bytes), status (1), and on success the type (1) and the value.
static void parse_time_response(const uint8_t *p, uint16_t len) {
  uint32_t time = 0xFFFFFFFFu;
  int time_status = -1;
  uint16_t i = 0;
  while (i + 3 <= len) {
    uint16_t id = p[i] | (p[i + 1] << 8);
    uint8_t status = p[i + 2];
    i += 3;
    if (status != 0)
      continue;
    if (i >= len)
      break;
    uint8_t type = p[i++];
    if (type == EZB_ZCL_ATTR_TYPE_UTC || type == EZB_ZCL_ATTR_TYPE_UINT32) {
      if (i + 4 > len)
        break;
      uint32_t v = p[i] | (p[i + 1] << 8) | (p[i + 2] << 16) | (static_cast<uint32_t>(p[i + 3]) << 24);
      if (id == ATTR_TIME)
        time = v;
      i += 4;
    } else if (type == EZB_ZCL_ATTR_TYPE_MAP8 || type == EZB_ZCL_ATTR_TYPE_UINT8) {
      if (i + 1 > len)
        break;
      if (id == ATTR_TIME_STATUS)
        time_status = p[i];
      i += 1;
    } else {
      break;  // a type we did not ask for; its length is unknown
    }
  }
  // A server that says it is neither master nor synchronised has no time to give.
  if (time == 0xFFFFFFFFu || time == 0 ||
      (time_status >= 0 && (time_status & (TIME_STATUS_MASTER | TIME_STATUS_SYNCHRONIZED)) == 0)) {
    g_status.store(NETWORK_TIME_NOT_SET);
  } else {
    g_network_time.store(time + UNIX_2000);
    g_status.store(NETWORK_TIME_OK);
  }
  g_pending.store(false);
}

static bool on_raw_frame(const ezb_zcl_raw_frame_t *frame) {
  const ezb_zcl_cmd_hdr_t *hdr = frame->header;
  if (hdr == nullptr || hdr->cluster_id != EZB_ZCL_CLUSTER_ID_TIME || hdr->dst_ep != g_endpoint ||
      hdr->cmd_id != EZB_ZCL_CMD_READ_ATTRIBUTES_RESPONSE ||
      (hdr->fc & 0x01) != 0)  // cluster-specific, not a general command
    return false;
  if (g_pending.load())
    parse_time_response(frame->payload, frame->payload_length);
  return true;  // ours; nothing else in the stack waits for it
}

}  // namespace hiflow_ble

namespace zigbee {

template<>
void ZigbeeComponent::add_attr_<hiflow_ble::TimeCluster>(ZigbeeAttribute * /*attr*/, uint8_t endpoint_id,
                                                          uint16_t /*cluster_id*/, uint8_t /*role*/,
                                                          uint16_t /*attr_id*/, hiflow_ble::TimeCluster * /*value_p*/) {
  ezb_af_ep_desc_t ep_desc = ezb_af_device_get_endpoint_desc(this->dev_desc_, endpoint_id);
  if (ep_desc == NULL) {
    ESP_LOGE(hiflow_ble::TAG, "endpoint %u does not exist", endpoint_id);
    return;
  }
  // A Time client: the bridge reads the time, it never serves one.
  ezb_zcl_cluster_desc_t cluster = ezb_zcl_time_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT);
  if (cluster == NULL) {
    ESP_LOGE(hiflow_ble::TAG, "could not create the Time cluster");
    return;
  }
  if (ezb_af_endpoint_add_cluster_desc(ep_desc, cluster) != EZB_ERR_NONE)
    ESP_LOGE(hiflow_ble::TAG, "could not add the Time cluster to endpoint %u", endpoint_id);
}

}  // namespace zigbee

namespace hiflow_ble {

void add_time_cluster(zigbee::ZigbeeComponent *zb, uint8_t endpoint) {
  g_endpoint = endpoint;
  zb->add_attr<TimeCluster>(endpoint, EZB_ZCL_CLUSTER_ID_TIME, EZB_ZCL_CLUSTER_CLIENT, EZB_ZCL_ATTR_TIME_TIME_ID, 0,
                            TimeCluster{});
}

TimeRequest request_network_time(uint8_t endpoint) {
  if (!esp_zigbee_lock_acquire(0))
    return TIME_REQUEST_BUSY;
  if (!g_handler_registered) {
    ezb_zcl_raw_command_handler_register(on_raw_frame);
    g_handler_registered = true;
  }
  g_endpoint = endpoint;
  uint16_t attrs[] = {ATTR_TIME, ATTR_TIME_STATUS};
  ezb_zcl_read_attr_cmd_t req = {};
  req.cmd_ctrl.dst_addr.addr_mode = EZB_ADDR_MODE_SHORT;
  req.cmd_ctrl.dst_addr.u.short_addr = 0x0000;  // the coordinator
  req.cmd_ctrl.dst_ep = COORDINATOR_EP;
  req.cmd_ctrl.src_ep = endpoint;
  req.cmd_ctrl.cluster_id = EZB_ZCL_CLUSTER_ID_TIME;
  req.cmd_ctrl.manuf_code = EZB_ZCL_STD_MANUF_CODE;
  req.payload.attr_number = 2;
  req.payload.attr_field = attrs;
  g_status.store(NETWORK_TIME_NONE);
  g_request_ms.store(millis());
  g_pending.store(true);
  ezb_err_t err = ezb_zcl_read_attr_cmd_req(&req);
  esp_zigbee_lock_release();
  if (err != EZB_ERR_NONE) {
    ESP_LOGW(TAG, "sending the time request failed: %d", static_cast<int>(err));
    g_pending.store(false);
    g_status.store(NETWORK_TIME_SEND_FAILED);
    return TIME_REQUEST_FAILED;
  }
  return TIME_REQUEST_SENT;
}

int64_t take_network_time(int *status) {
  if (g_pending.load() && millis() - g_request_ms.load() > ANSWER_TIMEOUT_MS) {
    g_pending.store(false);
    g_status.store(NETWORK_TIME_NO_ANSWER);
  }
  if (status != nullptr)
    *status = g_status.load();
  return static_cast<int64_t>(g_network_time.exchange(0));
}

const char *network_time_status_str(int status) {
  switch (status) {
    case NETWORK_TIME_OK:
      return "ok";
    case NETWORK_TIME_NO_ANSWER:
      return "the coordinator did not answer";
    case NETWORK_TIME_NOT_SET:
      return "the coordinator has no valid time";
    case NETWORK_TIME_SEND_FAILED:
      return "the request could not be sent";
    default:
      return "pending";
  }
}

}  // namespace hiflow_ble
}  // namespace esphome

#endif  // USE_ESP32 && USE_ZIGBEE
