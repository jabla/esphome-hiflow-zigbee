#include "phy_guard.h"

#if defined(USE_ESP32) && defined(USE_ZIGBEE) && defined(USE_ZIGBEE_POWER_SLEEPY)

#include <cstdlib>
#include <type_traits>

#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_phy_init.h"
#include "esp_private/phy.h"
#include "esphome/core/component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

// The free-mutex check in the 802.15.4 interrupt holds only while nothing else
// runs between it and esp_phy_disable() taking the mutex.
#if configNUMBER_OF_CORES > 1
#error "phy_guard: the mutex check in the 802.15.4 interrupt needs a single core"
#endif

// ESP-IDF internals this file relies on, checked in the 5.5, 6.0 and 6.1 sources: the 802.15.4
// driver switches the PHY off with esp_phy_disable() from its interrupt, and phy_get_lock()
// returns the _lock_t holding the FreeRTOS mutex that esp_phy_enable()/esp_phy_disable() take.
// esp-zigbee-lib 2.0.4 and ESP-IDF 5.5.5 never switch the 802.15.4 radio on where it cannot
// block; nb_enable shows it if a newer version does.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 2, 0)
#warning "phy_guard: not checked against this ESP-IDF version, see phy_guard.cpp"
#endif
#ifdef CONFIG_ESP_PHY_HW_SWITCH_RF
#error "phy_guard: not checked with CONFIG_ESP_PHY_HW_SWITCH_RF"
#endif
static_assert(std::is_same<decltype(&esp_phy_enable), void (*)(esp_phy_modem_t)>::value &&
                  std::is_same<decltype(&esp_phy_disable), void (*)(esp_phy_modem_t)>::value,
              "phy_guard: the wrappers no longer match esp_phy_enable()/esp_phy_disable()");
static_assert(std::is_same<decltype(phy_get_lock()), _lock_t>::value &&
                  std::is_pointer<_lock_t>::value && sizeof(_lock_t) == sizeof(SemaphoreHandle_t),
              "phy_guard: phy_get_lock() no longer returns a FreeRTOS mutex handle");

extern "C" {
void __real_esp_phy_enable(esp_phy_modem_t modem);
void __real_esp_phy_disable(esp_phy_modem_t modem);
void __wrap_esp_phy_enable(esp_phy_modem_t modem);
void __wrap_esp_phy_disable(esp_phy_modem_t modem);
}

namespace esphome {
namespace zigbee_power {

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
// A switch-off that could not block, was handed on and has not run yet. The
// 802.15.4 driver already counts the PHY as off, so its next call is a switch-on.
static volatile bool s_pending = false;
// finish_disable() is between taking s_pending and the end of the real switch-off.
static volatile bool s_finishing = false;
// Keeps a deferred switch-off and a switch-on from overtaking each other:
// both hold it from the look at s_pending to the end of the real call. Tasks
// only; where it cannot block, s_pending is set and cleared without it.
static SemaphoreHandle_t s_order = nullptr;
static StaticSemaphore_t s_order_buf;
static Component *s_wake = nullptr;
// Each field has one writing context, see phy_guard.h; read under s_mux.
static PhyGuardStats s_stats = {};

static bool take_pending() {
  portENTER_CRITICAL_SAFE(&s_mux);
  const bool pending = s_pending;
  s_pending = false;
  portEXIT_CRITICAL_SAFE(&s_mux);
  return pending;
}

static void finish_disable(void *, uint32_t) {
  xSemaphoreTake(s_order, portMAX_DELAY);
  portENTER_CRITICAL(&s_mux);
  const bool run = s_pending;
  s_pending = false;
  s_finishing = run;
  portEXIT_CRITICAL(&s_mux);
  if (run) {
    __real_esp_phy_disable(PHY_MODEM_IEEE802154);
    s_stats.done++;
    s_finishing = false;
  }
  xSemaphoreGive(s_order);
}

void phy_guard_init(Component *wake) {
  // A deferral needs s_order, so the interrupt always finds s_wake set.
  s_wake = wake;
  if (s_order == nullptr)
    s_order = xSemaphoreCreateMutexStatic(&s_order_buf);
}

void phy_guard_loop() {
  if (s_pending && s_order != nullptr)
    finish_disable(nullptr, 0);
}

PhyGuardStats phy_guard_stats() {
  portENTER_CRITICAL(&s_mux);
  const PhyGuardStats stats = s_stats;
  portEXIT_CRITICAL(&s_mux);
  return stats;
}

}  // namespace zigbee_power
}  // namespace esphome

using namespace esphome::zigbee_power;

void __wrap_esp_phy_disable(esp_phy_modem_t modem) {
  // newlib's lock aborts on a held mutex wherever the caller cannot yield: in
  // an interrupt and in a critical section. Everywhere else it just waits.
  if (xPortCanYield()) {
    __real_esp_phy_disable(modem);
    return;
  }
  if (modem != PHY_MODEM_IEEE802154) {
    s_stats.bt_nonblocking++;
    __real_esp_phy_disable(modem);
    return;
  }
  // One core, and with interrupts masked (this one runs at level 1 inside the
  // 802.15.4 driver's critical section) nothing that uses the PHY mutex runs
  // in between: a mutex free now is still free when esp_phy_disable() takes
  // it. The count sees a mutex taken from an interrupt too, the holder not.
  auto lock = reinterpret_cast<SemaphoreHandle_t>(phy_get_lock());
  if (s_order == nullptr || lock == nullptr || uxSemaphoreGetCountFromISR(lock) != 0) {
    s_stats.direct++;
    __real_esp_phy_disable(modem);
    return;
  }
  portENTER_CRITICAL_SAFE(&s_mux);
  s_pending = true;
  portEXIT_CRITICAL_SAFE(&s_mux);
  s_stats.deferred++;
  BaseType_t woken = pdFALSE;
  if (xTimerPendFunctionCallFromISR(finish_disable, nullptr, 0, &woken) != pdPASS) {
    s_stats.pend_failed++;
    if (s_wake != nullptr)
      s_wake->enable_loop_soon_any_context();
  }
  if (xPortInIsrContext())
    portYIELD_FROM_ISR(woken);
}

void __wrap_esp_phy_enable(esp_phy_modem_t modem) {
  if (!xPortCanYield()) {
    if (modem != PHY_MODEM_IEEE802154) {
      s_stats.bt_nonblocking++;
      __real_esp_phy_enable(modem);
      return;
    }
    s_stats.nb_enable++;
    portENTER_CRITICAL_SAFE(&s_mux);
    const bool pending = s_pending;
    s_pending = false;
    const bool finishing = s_finishing;
    portEXIT_CRITICAL_SAFE(&s_mux);
    if (pending) {
      // Still on: the switch-off never ran.
      s_stats.nb_cancelled++;
      return;
    }
    if (finishing) {
      // The timer task is switching the PHY off right now and cannot be
      // waited for here.
      ESP_DRAM_LOGE("zigbee_power.phy_guard", "802.15.4 switch-on during a deferred switch-off");
      abort();
    }
    __real_esp_phy_enable(modem);
    return;
  }
  if (modem != PHY_MODEM_IEEE802154 || s_order == nullptr) {
    __real_esp_phy_enable(modem);
    return;
  }
  xSemaphoreTake(s_order, portMAX_DELAY);
  if (take_pending()) {
    // Still on: the switch-off never ran.
    s_stats.cancelled++;
  } else {
    __real_esp_phy_enable(modem);
  }
  xSemaphoreGive(s_order);
}

#endif
