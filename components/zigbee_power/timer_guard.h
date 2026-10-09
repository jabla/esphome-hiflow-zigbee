#pragma once

// Works around a hardware fault of the ESP32-C6 (seen on rev v0.2, ESP-IDF
// issue 19036): the SYSTIMER counter behind esp_timer_get_time() now and then
// loses bits, so esp_timer jumps back by 4 s up to hours while the FreeRTOS
// tick (the other SYSTIMER counter, and millis()) runs on. Everything timed by
// esp_timer stops until the clock caught up again: the Zigbee stack's poll
// froze in the middle of a firmware download. Worse, when the PHY switches
// off it reads the temperature sensor, which waits out a settling time
// measured with esp_timer inside a critical section: a jump while the radio
// is on makes that wait as long as the jump, and the interrupt watchdog
// (300 ms) resets the chip.
//
// The guard sits on every reading of the clock: the linker routes
// esp_timer_get_time() and esp_timer_impl_get_time() through timer_guard.cpp
// (-Wl,--wrap, see __init__.py). Each reading compares esp_timer with the tick;
// when esp_timer dropped against the tick since the last reading by 2.1 s or
// more (half of one lost bit: 2^26 ticks of 16 MHz, 4.19 s), it moves esp_timer
// forward by the loss, rounded to whole 2^26 ticks, with
// esp_timer_private_advance() (the sibling of the esp_timer_private_set() that
// light sleep uses), and returns the repaired time. So no reader sees the
// clock behind: not the Zigbee stack's timers, not the PHY's temperature wait.
// No timer of its own: ESPHome's main loop reads the clock at least every
// loop_interval (the task watchdog resets the chip if it stalls for 5 s), and
// ESP-IDF reads it around every light sleep, the only time the two counters
// drift apart.

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_ZIGBEE) && defined(USE_ZIGBEE_POWER_TIMER_GUARD)

#include <cstdint>

namespace esphome {
namespace zigbee_power {

struct TimerGuardStats {
  uint32_t repairs;   // jumps repaired
  uint32_t last_ms;   // the last repair, ms
  uint64_t total_ms;  // all repairs, ms
};

/// Logs the repairs since the last call. Main loop only (zigbee_power's loop()).
void timer_guard_log();
void timer_guard_dump_config();
TimerGuardStats timer_guard_stats();

}  // namespace zigbee_power
}  // namespace esphome

#endif
