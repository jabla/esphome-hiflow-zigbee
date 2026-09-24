#include "hiflow_clock.h"

#include <stdio.h>

/* ---------- civil date conversion (Howard Hinnant's algorithms) ---------- */

/* Days since 1970-01-01 for a proleptic Gregorian date. */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;                                   /* [0, 399] */
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  /* [0, 365] */
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           /* [0, 146096] */
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y_out, int *m_out, int *d_out)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;                                          /* [0, 146096] */
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;     /* [0, 399] */
    const int64_t y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                   /* [0, 365] */
    const int64_t mp = (5 * doy + 2) / 153;                                        /* [0, 11] */
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;                                /* [1, 31] */
    const int64_t m = mp + (mp < 10 ? 3 : -9);                                     /* [1, 12] */
    *y_out = y + (m <= 2);
    *m_out = (int) m;
    *d_out = (int) d;
}

/* Floor division, so that times before 1970 still map to the right day. */
static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

/* 0 = Sunday. */
static int weekday_from_days(int64_t days)
{
    int wd = (int) ((days + 4) % 7);
    return wd < 0 ? wd + 7 : wd;
}

/* UTC instant of the last Sunday of `month` in `year` at `hour_utc`. */
static int64_t last_sunday_utc(int64_t year, int month, int hour_utc)
{
    static const int last_day[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int64_t days = days_from_civil(year, month, last_day[month]);
    days -= weekday_from_days(days); /* step back to the Sunday */
    return days * 86400 + (int64_t) hour_utc * 3600;
}

/* ---------- clock ---------- */

void hiflow_clock_init(hiflow_clock_t *clock, int64_t uptime_ms,
                       int64_t build_time, int64_t persisted)
{
    int64_t base;

    if (clock == NULL)
        return;

    base = build_time > HIFLOW_TIME_MIN ? build_time : HIFLOW_TIME_MIN;
    /* A value saved before the reboot wins: the inverter has already seen it,
       and a login timestamp that goes backwards looks like a replay. */
    if (persisted > base)
        base = persisted + 1;
    clock->base_unix = base;
    clock->base_uptime_ms = uptime_ms;
}

int64_t hiflow_clock_now(const hiflow_clock_t *clock, int64_t uptime_ms)
{
    if (clock == NULL)
        return 0;
    return clock->base_unix + floor_div(uptime_ms - clock->base_uptime_ms, 1000);
}

int hiflow_clock_observe_device_time(hiflow_clock_t *clock, int64_t uptime_ms,
                                     int64_t device_time)
{
    if (clock == NULL)
        return 0;
    if (device_time <= HIFLOW_TIME_MIN || device_time >= HIFLOW_TIME_MAX)
        return 0;
    if (device_time <= hiflow_clock_now(clock, uptime_ms))
        return 0;
    clock->base_unix = device_time;
    clock->base_uptime_ms = uptime_ms;
    return 1;
}

int hiflow_is_eu_dst(int64_t unix_time)
{
    int64_t year;
    int month, day;

    civil_from_days(floor_div(unix_time, 86400), &year, &month, &day);
    return unix_time >= last_sunday_utc(year, 3, 1) &&
           unix_time < last_sunday_utc(year, 10, 1);
}

int32_t hiflow_tz_offset_eu(int64_t unix_time, int32_t std_offset)
{
    return hiflow_is_eu_dst(unix_time) ? std_offset + 3600 : std_offset;
}

size_t hiflow_format_local_time(int64_t unix_time, int32_t offset, char *out,
                                size_t cap)
{
    int64_t local, days, secs, year;
    int month, day;
    int written;

    if (out == NULL || cap < HIFLOW_TIME_STR_LEN)
        return 0;

    local = unix_time + offset;
    days = floor_div(local, 86400);
    secs = local - days * 86400;
    civil_from_days(days, &year, &month, &day);
    written = snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d", (int) year, month, day,
                       (int) (secs / 3600), (int) ((secs / 60) % 60), (int) (secs % 60));
    return written > 0 ? (size_t) written : 0;
}
