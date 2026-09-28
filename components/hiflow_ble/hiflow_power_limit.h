#pragma once

// The power limit slider on the Zigbee side: an Analog Output server cluster
// (0x000D) whose present_value is the inverter's power limit in percent. ZHA
// shows an Analog Output as a number entity.
//
// ESPHome's Zigbee component on ESP32 only builds Analog Input and Binary
// Input clusters (its number and switch platforms are Zephyr only), so the
// cluster is built here with the esp-zigbee-lib API. Nothing hooks the stack's
// write callback either: the owner polls present_value and compares it with
// the last value it set itself.

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_ZIGBEE)

#include "esphome/components/zigbee/zigbee_esp32.h"

namespace esphome {
namespace hiflow_ble {

/// Adds the endpoint's Analog Output cluster. The endpoint itself (basic and
/// identify cluster) is created by ESPHome's Zigbee codegen; this must run
/// after that and before the Zigbee task registers the device, i.e. from the
/// generated setup code.
void add_power_limit_cluster(zigbee::ZigbeeComponent *zb, uint8_t endpoint);

/// Reads present_value. False when the stack is busy or the attribute is
/// missing; the caller simply tries again later.
bool read_power_limit_attr(uint8_t endpoint, float *value);

/// Sets present_value and reports it to the coordinator once joined.
bool write_power_limit_attr(zigbee::ZigbeeComponent *zb, uint8_t endpoint, float value);

}  // namespace hiflow_ble
}  // namespace esphome

#endif  // USE_ESP32 && USE_ZIGBEE
