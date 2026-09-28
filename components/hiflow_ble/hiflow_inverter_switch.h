#pragma once

// The inverter's on/off switch on the Zigbee side: an On/Off server cluster
// (0x0006) whose on_off attribute is the state the bridge last switched the
// inverter to. ZHA shows it as a switch entity.
//
// Built with the esp-zigbee-lib API for the same reason as the power limit
// slider (see hiflow_power_limit.h): ESPHome's Zigbee component on ESP32 has
// no switch platform. The stack handles the On, Off and Toggle commands by
// itself and only updates the attribute, so the owner polls it.

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_ZIGBEE)

#include "esphome/components/zigbee/zigbee_esp32.h"

namespace esphome {
namespace hiflow_ble {

/// Adds the endpoint's On/Off cluster. Same rules as add_power_limit_cluster:
/// after ESPHome's Zigbee codegen created the endpoint, before the Zigbee task
/// registers the device.
void add_inverter_switch_cluster(zigbee::ZigbeeComponent *zb, uint8_t endpoint);

/// Reads on_off. False when the stack is busy or the attribute is missing.
bool read_inverter_switch_attr(uint8_t endpoint, bool *on);

/// Sets on_off and reports it to the coordinator once joined.
bool write_inverter_switch_attr(zigbee::ZigbeeComponent *zb, uint8_t endpoint, bool on);

}  // namespace hiflow_ble
}  // namespace esphome

#endif  // USE_ESP32 && USE_ZIGBEE
