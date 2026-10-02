#pragma once

// The network time: a Time client cluster (0x000A) on its own endpoint, from
// which the bridge reads the coordinator's Time attributes. ZHA (zigpy) and
// Zigbee2MQTT (zigbee-herdsman) answer that read from the host's clock.
// Without WiFi there is no NTP, and the bridge's own clock can lag by hours
// after a power cut at night.
//
// Not esp-zigbee-lib's time-server synchronisation: it looks for time servers
// with a broadcast Match Descriptor request, which a ConBee's firmware neither
// answers for the host nor passes on to it, so it only ever times out.
//
// Built with the esp-zigbee-lib API for the same reason as the power limit
// slider (see hiflow_power_limit.h): ESPHome's zigbee time platform is Zephyr
// only.

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_ZIGBEE)

#include "esphome/components/zigbee/zigbee_esp32.h"

#include <cstdint>

namespace esphome {
namespace hiflow_ble {

enum NetworkTimeStatus {
  NETWORK_TIME_NONE = -1,  // no request yet, or waiting for the answer
  NETWORK_TIME_OK = 0,
  NETWORK_TIME_NO_ANSWER = 1,
  NETWORK_TIME_NOT_SET = 2,
  NETWORK_TIME_SEND_FAILED = 3,
};

/// Adds the endpoint's Time client cluster (same rules as
/// add_power_limit_cluster).
void add_time_cluster(zigbee::ZigbeeComponent *zb, uint8_t endpoint);

enum TimeRequest {
  TIME_REQUEST_SENT = 0,
  TIME_REQUEST_BUSY = 1,    ///< the Zigbee stack is busy: try again soon
  TIME_REQUEST_FAILED = 2,  ///< the stack refused the request
};

/// Asks the coordinator for the time. The answer arrives later; take it with
/// take_network_time(). Never waits for the stack.
TimeRequest request_network_time(uint8_t endpoint);

/// The time the coordinator handed out (unix seconds), once; 0 when none came
/// since the last call. `status` receives the result of the last request (a
/// NetworkTimeStatus).
int64_t take_network_time(int *status);

const char *network_time_status_str(int status);

}  // namespace hiflow_ble
}  // namespace esphome

#endif  // USE_ESP32 && USE_ZIGBEE
