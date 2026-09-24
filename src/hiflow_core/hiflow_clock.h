/*
 * hiflow_clock — the bridge's notion of wall-clock time.
 *
 * The board has no RTC and no network time: it keeps a base unix timestamp plus
 * the monotonic uptime, and pulls the base forward whenever the inverter reports
 * a plausible time in its login acknowledgement.
 *
 * Two properties matter for the protocol:
 *
 *   * The timestamp in a login frame must move forward. The uptime is therefore
 *     taken as 64-bit milliseconds (esp_timer_get_time() / 1000 on target), not
 *     as 32-bit millis(), which wraps after 49.7 days and made the clock jump
 *     back by the same amount.
 *   * The handshake's time-sync (action 104) sets the inverter's own clock, so
 *     the UTC offset has to follow European summer time instead of a fixed 3600.
 *
 * No libc date functions are used: the civil-date conversion below is exact for
 * the whole int64 range and behaves the same on host and target.
 */
#ifndef HIFLOW_CLOCK_H
#define HIFLOW_CLOCK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Plausibility window for timestamps coming from the device (2024-01-01 ..
   2040-01-01). The device answered with values like -1607349313 when a reply was
   decoded with the wrong layout; such values must never reach the clock. */
#define HIFLOW_TIME_MIN ((int64_t) 1704067200)
#define HIFLOW_TIME_MAX ((int64_t) 2208988800)

/* Length of the "YYYY-MM-DD HH:MM:SS" string including the NUL. */
#define HIFLOW_TIME_STR_LEN 20

typedef struct {
    int64_t base_unix;      /* unix time belonging to base_uptime_ms */
    int64_t base_uptime_ms; /* monotonic uptime, 64 bit */
} hiflow_clock_t;

/* Start the clock. `build_time` is the firmware build timestamp, `persisted` the
   last value written to flash (0 if none). The later of both wins, so the clock
   never restarts behind a timestamp the inverter has already seen. */
void hiflow_clock_init(hiflow_clock_t *clock, int64_t uptime_ms,
                       int64_t build_time, int64_t persisted);

/* Current unix time. */
int64_t hiflow_clock_now(const hiflow_clock_t *clock, int64_t uptime_ms);

/* Adopt the device's time (login acknowledgement, field 2). It is taken only
   when it is plausible and ahead of ours, so the clock keeps moving forward.
   Returns 1 when the base was moved, 0 otherwise. */
int hiflow_clock_observe_device_time(hiflow_clock_t *clock, int64_t uptime_ms,
                                     int64_t device_time);

/* 1 while European summer time is in effect: from the last Sunday in March
   01:00 UTC until the last Sunday in October 01:00 UTC. */
int hiflow_is_eu_dst(int64_t unix_time);

/* Standard offset plus one hour while EU summer time is in effect. */
int32_t hiflow_tz_offset_eu(int64_t unix_time, int32_t std_offset);

/* Writes "YYYY-MM-DD HH:MM:SS" for (unix_time + offset) and returns its length
   (0 if the buffer is too small). This is the `time_ymd_hms` field the reference
   library fills with datetime.now(). */
size_t hiflow_format_local_time(int64_t unix_time, int32_t offset, char *out,
                                size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HIFLOW_CLOCK_H */
