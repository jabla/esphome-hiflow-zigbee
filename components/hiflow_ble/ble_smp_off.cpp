// A board short of RAM (the ESP32-H2) builds Bluedroid without its security
// manager (CONFIG_BT_BLE_SMP_ENABLE off): the inverter's login is part of its
// own protocol, nothing pairs. ESPHome's BLE code still calls two of the
// security manager's functions, which are then missing at link time. Here they
// do nothing: the IO capability goes nowhere. A security request never
// arrives without the security manager.
#include "sdkconfig.h"

#if defined(CONFIG_BT_BLUEDROID_ENABLED) && !defined(CONFIG_BT_BLE_SMP_ENABLE)
#include <esp_gap_ble_api.h>

extern "C" esp_err_t esp_ble_gap_set_security_param(esp_ble_sm_param_t param_type, void *value, uint8_t len) {
  return ESP_OK;
}

extern "C" esp_err_t esp_ble_gap_security_rsp(esp_bd_addr_t bd_addr, bool accept) { return ESP_OK; }

// ESPHome 2026.11 wraps this one for its bond key repair on ESP-IDF 5.5.6
// (esp32_ble/bluedroid_bond_keys.c), also without the security manager. The
// wrapper needs the function to link. Only the security manager calls it, so
// here nothing does (arguments: tSMP_CB *, tSMP_INT_DATA *).
extern "C" void smp_check_auth_req(void *, void *) {}
#endif
