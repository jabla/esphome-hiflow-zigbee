#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"

#include "../hiflow_ble.h"

#ifdef USE_ESP32

namespace esphome {
namespace hiflow_ble {

/// A single decoded HiFlow measurement. Passive: it only registers itself with
/// the parent (which owns the BLE session) and then receives pushed updates.
class HiflowSensor : public sensor::Sensor, public Component {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_parent(HiflowBle *parent) { this->parent_ = parent; }
  void set_type(uint8_t type) { this->type_ = type; }

 protected:
  HiflowBle *parent_{nullptr};
  uint8_t type_{0};
};

}  // namespace hiflow_ble
}  // namespace esphome

#endif  // USE_ESP32
