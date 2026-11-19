#pragma once

// The settings field on the Zigbee side: the Basic cluster's LocationDescription
// (0x0010) on endpoint 1, a character string of up to 15 characters that the
// coordinator can read and write. ZHA offers it in "Manage Zigbee device"
// (cluster Basic, attribute location_desc), Zigbee2MQTT in the dev console
// (genBasic, locationDesc). The owner writes a command into it (see
// hiflow_setup.h), the bridge answers in the same attribute.
//
// A standard attribute on purpose: ZHA and Zigbee2MQTT handle it without a
// quirk or a converter. As with the power limit slider nothing hooks the
// stack's write callback: the owner polls the value and compares it with the
// last text it set itself.

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_ZIGBEE)

#include <string>

#include "esphome/components/zigbee/zigbee_esp32.h"

namespace esphome {
namespace hiflow_ble {

/// Where the field lives: the first endpoint, the one ZHA and Zigbee2MQTT read
/// the Basic cluster from.
static const uint8_t SETUP_FIELD_ENDPOINT = 1;

/// Takes over the LocationDescription that ESPHome put into the endpoint's
/// Basic cluster: writable, empty at first. Runs from the generated setup code,
/// after ESPHome created the endpoint and before the Zigbee task registers the
/// device.
void add_setup_field(zigbee::ZigbeeComponent *zb, uint8_t endpoint);

/// One look at the field in one hold of the stack's lock (tried for 10 ms).
/// Its text, without the NUL padding of its first value, goes to *text. When
/// that text is `ours` or `*also_ours` and `next` is given, the field takes
/// `*next` (at most 15 characters) in the same hold. So a command the
/// coordinator writes meanwhile is never overwritten unread. False when the
/// stack is busy or the attribute is missing: nothing read, nothing written,
/// the caller tries again later. *wrote tells whether `*next` went in.
bool exchange_setup_field(uint8_t endpoint, std::string *text, const std::string &ours,
                          const std::string *also_ours, const std::string *next, bool *wrote);

}  // namespace hiflow_ble
}  // namespace esphome

#endif  // USE_ESP32 && USE_ZIGBEE
