#pragma once

// Works around an ESP-IDF bug of a sleepy 802.15.4 node with light sleep: the
// 802.15.4 interrupt switches the PHY off (ieee802154_sleep() ->
// esp_phy_disable()), and esp_phy_disable() takes a mutex (phy_init.c on the
// ESP32-C6, phy_init_esp32hxx.c on the ESP32-H2). When a task holds that mutex
// just then (the PLL tracking timer, the BLE controller), taking it in the
// interrupt calls abort(). Seen once on an ESP32-H2, about 25 minutes into a
// Zigbee OTA download with the 50 ms poll and BLE connected; with a test task
// holding the mutex, the ESP32-H2 and the ESP32-C6 both aborted within seconds
// to minutes. Without BLE only such a test task provoked it: BLE is what holds
// the mutex at the wrong moment in practice.
//
// The linker sends every esp_phy_enable()/esp_phy_disable() call from outside
// ESP-IDF's PHY code through the wrappers in phy_guard.cpp (-Wl,--wrap, see
// __init__.py). In the interrupt (or wherever it cannot block), with the mutex
// taken, the 802.15.4 switch-off waits for a task (FreeRTOS's timer task)
// instead, and a switch-on before it ran cancels it. Without a holder the
// switch-off runs at once, as before.
//
// The BLE controller's own PHY calls are passed through. Where they cannot
// block, bt_nonblocking counts them: nothing could wait there, and the same
// abort would follow. On the bench (scan and connection starts from controller
// sleep, with a task holding the mutex) every one of them came from a task.

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_ZIGBEE) && defined(USE_ZIGBEE_POWER_SLEEPY)

#include <cstdint>

namespace esphome {

class Component;

namespace zigbee_power {

// Each field has one writing context. "Cannot block" means an interrupt or a
// critical section. deferred = done + cancelled + nb_cancelled, plus one while
// a switch-off waits for the timer task.
struct PhyGuardStats {
  uint32_t direct;          // 802.15.4 switch-offs that cannot block, mutex free: ran at once
  uint32_t deferred;        // ... mutex taken: handed to the timer task
  uint32_t done;            // deferred switch-offs a task ran
  uint32_t cancelled;       // deferred switch-offs a task's switch-on made moot
  uint32_t pend_failed;     // the timer task's queue was full; the main loop was woken to run it
  uint32_t nb_enable;       // 802.15.4 switch-ons that cannot block (none expected)
  uint32_t nb_cancelled;    // ... of them, those that made a deferred switch-off moot
  uint32_t bt_nonblocking;  // other modems' (BLE's) calls that cannot block, both ways
};

/// Before the Zigbee stack starts. `wake` is the component whose loop() calls
/// phy_guard_loop(); it is woken when the timer task's queue is full (a FAILED
/// component is not: then the next 802.15.4 switch-on cancels the switch-off).
void phy_guard_init(Component *wake);
/// Runs a deferred switch-off the timer task could not take (its queue was
/// full); the interrupt wakes the main loop for it. From the main loop.
void phy_guard_loop();
PhyGuardStats phy_guard_stats();

}  // namespace zigbee_power
}  // namespace esphome

#endif
