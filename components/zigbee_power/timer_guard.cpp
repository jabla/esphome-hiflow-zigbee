#include "timer_guard.h"

#if defined(USE_ESP32) && defined(USE_ZIGBEE) && defined(USE_ZIGBEE_POWER_TIMER_GUARD)

#include "esp_attr.h"
#include "esp_idf_version.h"
#include "esp_private/esp_timer_private.h"
#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/efuse_hal.h"
#include "sdkconfig.h"

#ifndef CONFIG_IDF_TARGET_ESP32C6
#error "timer_guard: only for the ESP32-C6"
#endif
// The offset check below needs one core: nothing runs between reading the
// tick and the counter.
#if configNUMBER_OF_CORES > 1
#error "timer_guard: needs a single core"
#endif
static_assert(1000000 % configTICK_RATE_HZ == 0, "timer_guard: the tick must be a whole number of microseconds");

// ESP-IDF internals this file relies on, checked in the 5.5, 6.0 and 6.1 sources: on the
// ESP32-C6 esp_timer_get_time() is an alias of esp_timer_impl_get_time() in
// esp_timer_impl_systimer.c, which reads the SYSTIMER counter; every other reader of the
// clock (esp_timer.c, the Zigbee stack's alarm, sleep and PHY code, ESPHome) calls one of
// the two from another file, so the linker can wrap both. esp_timer_private_advance() must
// sit in IRAM (CONFIG_PM_SLP_IRAM_OPT, set in __init__.py): a repair can run in an
// interrupt while the flash cache is off.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 2, 0)
#warning "timer_guard: not checked against this ESP-IDF version, see timer_guard.cpp"
#endif

extern "C" {
int64_t __real_esp_timer_get_time(void);
int64_t __wrap_esp_timer_get_time(void);
int64_t __wrap_esp_timer_impl_get_time(void);
}

namespace esphome {
namespace zigbee_power {

static const char *const TAG = "zigbee_power.timer_guard";

// The fault clears counter bits from bit 26 up: every loss is a whole multiple
// of 2^26 ticks at 16 MHz, 2^22 us (4.19 s).
static constexpr int LOSS_SHIFT = 22;
static constexpr int64_t LOSS_UNIT_US = int64_t{1} << LOSS_SHIFT;
static constexpr int64_t TICK_US = 1000000 / configTICK_RATE_HZ;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
// esp_timer minus the tick at the last reading. It drifts slowly (in light
// sleep the tick ran 0.3 % fast on the bench, 1.2 % in a report on the issue)
// and is followed. A jump is a drop by half a loss unit or more from one
// reading to the next. The tick is only current while the scheduler runs:
// while it is suspended (flash operations; the idle task around light sleep,
// where ESP-IDF sets esp_timer forward before it steps the tick) ticks are
// held back and esp_timer seems to run ahead, so a rise is taken over only
// with the scheduler running, a drop always. Before the scheduler starts the
// tick stands at 0 and restarts with it: nothing to compare.
static bool s_have_offset = false;
static int64_t s_offset_us = 0;
static uint32_t s_last_tick = 0;
static uint32_t s_tick_wraps = 0;
static TimerGuardStats s_stats = {};
static uint32_t s_logged = 0;

// Returns the time, repaired if the counter lost bits since the last reading.
static int64_t IRAM_ATTR read_checked() {
  portENTER_CRITICAL_SAFE(&s_mux);
  int64_t now = __real_esp_timer_get_time();
  // Both kept in IRAM by freertos/linker.lf and plain reads on one core, also
  // in an interrupt (xTaskGetTickCountFromISR() may sit in flash).
  const BaseType_t state = xTaskGetSchedulerState();
  if (state != taskSCHEDULER_NOT_STARTED) {
    const uint32_t tick = xTaskGetTickCount();
    if (tick < s_last_tick)
      s_tick_wraps++;
    s_last_tick = tick;
    const int64_t tick_us = static_cast<int64_t>((static_cast<uint64_t>(s_tick_wraps) << 32) | tick) * TICK_US;
    const int64_t offset = now - tick_us;
    const bool tick_current = state == taskSCHEDULER_RUNNING;
    if (!s_have_offset) {
      if (tick_current) {
        s_offset_us = offset;
        s_have_offset = true;
      }
    } else {
      int64_t change = offset - s_offset_us;
      if (change <= -LOSS_UNIT_US / 2) {
        const int64_t loss = ((-change + LOSS_UNIT_US / 2) >> LOSS_SHIFT) << LOSS_SHIFT;
        esp_timer_private_advance(loss);
        now += loss;
        change += loss;
        s_stats.repairs++;
        s_stats.last_ms = static_cast<uint32_t>(loss / 1000);
        s_stats.total_ms += static_cast<uint64_t>(loss / 1000);
      }
      // What is left is drift, or a rise that only the held-back tick causes.
      if (change < 0 || tick_current)
        s_offset_us += change;
    }
  }
  portEXIT_CRITICAL_SAFE(&s_mux);
  return now;
}

void timer_guard_dump_config() {
  const uint32_t rev = efuse_hal_chip_revision();
  ESP_LOGCONFIG(TAG, "  Guards esp_timer against ESP-IDF issue 19036 (chip revision v%" PRIu32 ".%" PRIu32 ")",
                rev / 100, rev % 100);
}

void timer_guard_log() {
  const TimerGuardStats stats = timer_guard_stats();
  if (stats.repairs == s_logged)
    return;
  const uint32_t added = stats.repairs - s_logged;
  s_logged = stats.repairs;
  // A hardware fault: always worth a line, also on a deployed board.
  ESP_LOGW(TAG,
           "esp_timer jumped back %" PRIu32 " ms (ESP32-C6 SYSTIMER fault, ESP-IDF issue 19036), moved forward "
           "again; repairs: %" PRIu32 " new, %" PRIu32 " since boot",
           stats.last_ms, added, stats.repairs);
}

TimerGuardStats timer_guard_stats() {
  portENTER_CRITICAL(&s_mux);
  const TimerGuardStats stats = s_stats;
  portEXIT_CRITICAL(&s_mux);
  return stats;
}

}  // namespace zigbee_power
}  // namespace esphome

int64_t IRAM_ATTR __wrap_esp_timer_get_time(void) { return esphome::zigbee_power::read_checked(); }
int64_t IRAM_ATTR __wrap_esp_timer_impl_get_time(void) { return esphome::zigbee_power::read_checked(); }

#endif
