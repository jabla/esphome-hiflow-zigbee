#pragma once

// Works around a hardware fault of the ESP32-C6 (reported on revisions v0.1
// and v0.2, ESP-IDF issue 19036): now and then one of the two SYSTIMER
// counters loses bits while it runs.
//
// - Counter 0 holds esp_timer. Most losses clear bits from bit 26 up (4.19 s
//   up to hours), some only lower ones (bit 24 alone: 1.05 s; bits 19 to 22:
//   0.49 s). esp_timer jumps back by that much and everything timed by it
//   stops until the clock caught up again: the Zigbee stack's poll froze in
//   the middle of a firmware download. Worse, when the PHY switches off it
//   reads the temperature sensor, which waits out a settling time measured
//   with esp_timer inside a critical section: a jump while the radio is on
//   makes that wait as long as the jump, and the interrupt watchdog (300 ms)
//   resets the chip.
// - Counter 1 drives the FreeRTOS tick (and ESPHome's millis()). Its losses
//   so far were tens of milliseconds: the tick waits until the counter is
//   back at its next alarm, so FreeRTOS time falls behind by the loss.
//
// Both counters run on one clock, so their difference stays the same. Only
// ESP-IDF changes it, through two calls: esp_timer_private_set() after every
// light sleep (the counters stand still while the chip sleeps, then esp_timer
// is set from the RTC timer) and esp_timer_private_advance(). The linker
// routes both through timer_guard.cpp, along with every reading of the clock,
// esp_timer_get_time() and esp_timer_impl_get_time() (-Wl,--wrap, see
// __init__.py). Each reading compares the difference with the one before:
// - lower by 64 us or more: counter 0 lost that much. The guard moves
//   esp_timer forward by exactly the loss with esp_timer_private_advance() and
//   returns the repaired time, so no reader sees the clock behind: not the
//   Zigbee stack's timers, not the PHY's temperature wait.
// - higher by 64 us or more: counter 1 lost that much. Counted and logged, not
//   repaired (moving that counter would mean moving the tick's alarm too).
// Below 64 us it is the skew between the two reads. After the two calls the
// new difference is the reference. No timer of its own: ESPHome's main loop
// reads the clock at least every loop_interval (the task watchdog resets the
// chip if it stalls for 5 s), and ESP-IDF reads it around every light sleep.

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_ZIGBEE) && defined(USE_ZIGBEE_POWER_TIMER_GUARD)

#include <cstdint>

namespace esphome {
namespace zigbee_power {

struct TimerGuardStats {
  uint32_t repairs;        // losses of esp_timer (counter 0), each repaired
  uint32_t last_ms;        // the last one, ms
  uint64_t total_ms;       // all of them, ms
  uint32_t tick_losses;    // losses of the FreeRTOS tick's counter (counter 1), not repaired
  uint32_t tick_total_ms;  // all of them, ms
};

/// Logs the losses since the last call. Main loop only (zigbee_power's loop()).
void timer_guard_log();
void timer_guard_dump_config();
TimerGuardStats timer_guard_stats();

}  // namespace zigbee_power
}  // namespace esphome

#endif
