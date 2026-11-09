#include "timer_guard.h"

#if defined(USE_ESP32) && defined(USE_ZIGBEE) && defined(USE_ZIGBEE_POWER_TIMER_GUARD)

#include <cinttypes>
#include <cstdio>

#include "esp_attr.h"
#include "esp_idf_version.h"
#include "esp_private/esp_timer_private.h"
#include "esp_private/systimer.h"
#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/efuse_hal.h"
#include "hal/systimer_ll.h"
#include "sdkconfig.h"
#include "soc/systimer_struct.h"

#ifndef CONFIG_IDF_TARGET_ESP32C6
#error "timer_guard: only for the ESP32-C6"
#endif
// The check below needs one core: nothing runs between reading the two
// counters.
#if configNUMBER_OF_CORES > 1
#error "timer_guard: needs a single core"
#endif

// ESP-IDF internals this file relies on, checked in the 5.5, 6.0 and 6.1 sources:
// - On the ESP32-C6 esp_timer_get_time() is an alias of esp_timer_impl_get_time() in
//   esp_timer_impl_systimer.c, which reads SYSTIMER counter 0 (16 ticks per us). Every
//   other reader (esp_timer.c, the Zigbee stack's alarm, sleep and PHY code, ESPHome)
//   calls one of the two from another file, so the linker can wrap both.
// - Counter 0 is written only by esp_timer_impl_set() and esp_timer_impl_advance() in that
//   file, called from elsewhere as esp_timer_private_set() (light sleep, sleep_modes.c)
//   and esp_timer_private_advance(). Counter 1 is written only when FreeRTOS starts
//   (vSystimerSetup() in port_systick.c sets it to 0). The sleep retention saves and
//   restores both counters together, before the light sleep's esp_timer_private_set().
// - esp_timer_private_set() and esp_timer_private_advance() sit in IRAM
//   (CONFIG_PM_SLP_IRAM_OPT, set in __init__.py): they run in the light sleep path, and a
//   repair can run in an interrupt while the flash cache is off.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 2, 0)
#warning "timer_guard: not checked against this ESP-IDF version, see timer_guard.cpp"
#endif

extern "C" {
int64_t __real_esp_timer_get_time(void);
void __real_esp_timer_private_set(uint64_t new_us);
void __real_esp_timer_private_advance(int64_t time_diff_us);
int64_t __wrap_esp_timer_get_time(void);
int64_t __wrap_esp_timer_impl_get_time(void);
void __wrap_esp_timer_private_set(uint64_t new_us);
void __wrap_esp_timer_private_advance(int64_t time_diff_us);
}

namespace esphome {
namespace zigbee_power {

static const char *const TAG = "zigbee_power.timer_guard";

static constexpr int64_t TICKS_PER_US = 16;  // systimer_ticks_to_us() on the ESP32-C6
static constexpr int64_t TICKS_PER_MS = 1000 * TICKS_PER_US;
// The smallest change of the difference that counts as a loss. The two reads
// are a few dozen ticks apart at most (40 MHz CPU), and the time read from
// counter 0 is cut to whole microseconds.
static constexpr int64_t MIN_LOSS_TICKS = 64 * TICKS_PER_US;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
// Counter 0 minus counter 1 at the last reading, in ticks.
static bool s_have_ref = false;
static int64_t s_ref = 0;

static uint32_t s_repairs = 0;
static uint64_t s_last_ticks = 0;
static uint64_t s_total_ticks = 0;
static uint32_t s_tick_losses = 0;
static uint64_t s_tick_last_ticks = 0;
static uint64_t s_tick_total_ticks = 0;
static uint32_t s_logged = 0;
static uint32_t s_tick_logged = 0;

// Counter 1, read from the SYSTIMER directly: ESP-IDF has no call for it. The
// wait for the snapshot is bounded; without one the reading goes unchecked.
static bool IRAM_ATTR read_tick_counter(uint64_t *ticks) {
  for (int attempt = 0; attempt < 3; attempt++) {
    systimer_ll_counter_snapshot(&SYSTIMER, SYSTIMER_COUNTER_OS_TICK);
    for (int i = 0; i < 1000; i++) {
      if (systimer_ll_is_counter_value_valid(&SYSTIMER, SYSTIMER_COUNTER_OS_TICK)) {
        // Inside the caller's critical section: no other snapshot in between.
        const uint32_t lo = systimer_ll_get_counter_value_low(&SYSTIMER, SYSTIMER_COUNTER_OS_TICK);
        const uint32_t hi = systimer_ll_get_counter_value_high(&SYSTIMER, SYSTIMER_COUNTER_OS_TICK);
        *ticks = (static_cast<uint64_t>(hi) << 32) | lo;
        return true;
      }
    }
  }
  return false;
}

// The counters' difference with esp_timer at `now` (us, read just before).
static bool IRAM_ATTR read_difference(int64_t now, int64_t *diff) {
  uint64_t tick_counter;
  if (!read_tick_counter(&tick_counter))
    return false;
  *diff = now * TICKS_PER_US - static_cast<int64_t>(tick_counter);
  return true;
}

// Compares the difference with the reference: repairs a loss of counter 0,
// counts one of counter 1. Returns `now` (esp_timer, us, read just before),
// repaired. Caller holds s_mux.
static int64_t IRAM_ATTR check_locked(int64_t now) {
  int64_t diff;
  if (!read_difference(now, &diff))
    return now;
  if (!s_have_ref) {
    // Counter 1 counts from FreeRTOS's start on (set to 0 there); a first
    // tick shows that it does.
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED && xTaskGetTickCount() != 0) {
      s_ref = diff;
      s_have_ref = true;
    }
    return now;
  }
  const int64_t change = diff - s_ref;
  if (change <= -MIN_LOSS_TICKS) {
    const int64_t loss_us = (-change + TICKS_PER_US / 2) / TICKS_PER_US;
    __real_esp_timer_private_advance(loss_us);
    now += loss_us;
    s_repairs++;
    s_last_ticks = static_cast<uint64_t>(-change);
    s_total_ticks += static_cast<uint64_t>(-change);
    // The difference is back at the reference.
    return now;
  }
  if (change >= MIN_LOSS_TICKS) {
    s_tick_losses++;
    s_tick_last_ticks = static_cast<uint64_t>(change);
    s_tick_total_ticks += static_cast<uint64_t>(change);
  }
  // A loss of counter 1 stays (FreeRTOS time is behind by it); below the
  // limit the difference follows the read skew.
  s_ref = diff;
  return now;
}

// After ESP-IDF set or moved esp_timer: the new difference is the reference.
static void IRAM_ATTR rebase_locked() {
  if (!s_have_ref)
    return;
  if (!read_difference(__real_esp_timer_get_time(), &s_ref))
    s_have_ref = false;  // a new one at the next reading
}

static int64_t IRAM_ATTR read_checked() {
  portENTER_CRITICAL_SAFE(&s_mux);
  const int64_t now = check_locked(__real_esp_timer_get_time());
  portEXIT_CRITICAL_SAFE(&s_mux);
  return now;
}

// A loss since the last reading would vanish unseen in the set: checked first.
static void IRAM_ATTR set_checked(uint64_t new_us) {
  portENTER_CRITICAL_SAFE(&s_mux);
  check_locked(__real_esp_timer_get_time());
  __real_esp_timer_private_set(new_us);
  rebase_locked();
  portEXIT_CRITICAL_SAFE(&s_mux);
}

static void IRAM_ATTR advance_checked(int64_t time_diff_us) {
  portENTER_CRITICAL_SAFE(&s_mux);
  check_locked(__real_esp_timer_get_time());
  __real_esp_timer_private_advance(time_diff_us);
  rebase_locked();
  portEXIT_CRITICAL_SAFE(&s_mux);
}

static void format_ms(char *buf, size_t size, uint64_t ticks) {
  const uint64_t us = ticks / TICKS_PER_US;
  snprintf(buf, size, "%" PRIu64 ".%03" PRIu32 " ms", us / 1000, static_cast<uint32_t>(us % 1000));
}

void timer_guard_dump_config() {
  const uint32_t rev = efuse_hal_chip_revision();
  ESP_LOGCONFIG(TAG, "  Guards esp_timer against ESP-IDF issue 19036 (chip revision v%" PRIu32 ".%" PRIu32 ")",
                rev / 100, rev % 100);
}

void timer_guard_log() {
  portENTER_CRITICAL(&s_mux);
  const uint32_t repairs = s_repairs;
  const uint64_t last = s_last_ticks;
  const uint32_t tick_losses = s_tick_losses;
  const uint64_t tick_last = s_tick_last_ticks;
  portEXIT_CRITICAL(&s_mux);
  char size[32];
  // A hardware fault: always worth a line, also on a deployed board.
  if (repairs != s_logged) {
    format_ms(size, sizeof(size), last);
    ESP_LOGW(TAG,
             "esp_timer lost %s (SYSTIMER counter 0, %" PRIu64 " ticks; ESP32-C6 fault, ESP-IDF issue 19036), "
             "moved forward again; repairs: %" PRIu32 " new, %" PRIu32 " since boot",
             size, last, repairs - s_logged, repairs);
    s_logged = repairs;
  }
  if (tick_losses != s_tick_logged) {
    format_ms(size, sizeof(size), tick_last);
    ESP_LOGW(TAG,
             "FreeRTOS tick lost %s (SYSTIMER counter 1, %" PRIu64 " ticks; ESP32-C6 fault, ESP-IDF issue 19036), "
             "stays behind; losses: %" PRIu32 " new, %" PRIu32 " since boot",
             size, tick_last, tick_losses - s_tick_logged, tick_losses);
    s_tick_logged = tick_losses;
  }
}

TimerGuardStats timer_guard_stats() {
  portENTER_CRITICAL(&s_mux);
  TimerGuardStats stats;
  stats.repairs = s_repairs;
  stats.last_ms = static_cast<uint32_t>(s_last_ticks / TICKS_PER_MS);
  stats.total_ms = s_total_ticks / TICKS_PER_MS;
  stats.tick_losses = s_tick_losses;
  stats.tick_total_ms = static_cast<uint32_t>(s_tick_total_ticks / TICKS_PER_MS);
  portEXIT_CRITICAL(&s_mux);
  return stats;
}

}  // namespace zigbee_power
}  // namespace esphome

int64_t IRAM_ATTR __wrap_esp_timer_get_time(void) { return esphome::zigbee_power::read_checked(); }
int64_t IRAM_ATTR __wrap_esp_timer_impl_get_time(void) { return esphome::zigbee_power::read_checked(); }
void IRAM_ATTR __wrap_esp_timer_private_set(uint64_t new_us) { esphome::zigbee_power::set_checked(new_us); }
void IRAM_ATTR __wrap_esp_timer_private_advance(int64_t time_diff_us) {
  esphome::zigbee_power::advance_checked(time_diff_us);
}

#endif
