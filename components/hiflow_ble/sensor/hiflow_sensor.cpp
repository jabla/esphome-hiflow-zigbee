#include "hiflow_sensor.h"

#include "esphome/core/log.h"

#ifdef USE_ESP32

namespace esphome {
namespace hiflow_ble {

static const char *const TAG = "hiflow_ble.sensor";

void HiflowSensor::setup() {
  if (this->parent_ == nullptr) {
    ESP_LOGE(TAG, "no hiflow_ble parent, sensor will never update");
    return;
  }
  this->parent_->register_sensor(this, this->type_);
}

}  // namespace hiflow_ble
}  // namespace esphome

#endif  // USE_ESP32
