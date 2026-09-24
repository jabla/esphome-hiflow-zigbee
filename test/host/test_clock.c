/*
 * test_clock.c — host test for the bridge's wall-clock handling.
 *
 * Covers what the old synthetic clock got wrong (see docs/session-core-plan.md):
 *   - the clock must not jump backwards when the 32-bit uptime would wrap
 *   - a reboot must not restart behind a timestamp the inverter has seen
 *   - only plausible device times, and only ones that are ahead, are adopted
 *   - the UTC offset follows European summer time instead of a fixed 3600
 *   - "YYYY-MM-DD HH:MM:SS" matches what the reference library sends
 *
 * Build/run:  make -C test/host test-clock
 */
#include <stdio.h>
#include <string.h>

#include "hiflow_clock.h"

static int g_pass;
static int g_fail;

static void check(int ok, const char *what)
{
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL %s\n", what);
    }
}

static void check_i64(int64_t got, int64_t want, const char *what)
{
    if (got == want) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL %s: got %lld want %lld\n", what, (long long) got, (long long) want);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    if (strcmp(got, want) == 0) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL %s: got \"%s\" want \"%s\"\n", what, got, want);
    }
}

/* 2026-03-20 09:46:40 UTC — the timestamp behind the request vectors. */
#define VECTOR_TIME ((int64_t) 1774000000)

static void test_base(void)
{
    hiflow_clock_t c;

    printf("\n[1] base, uptime and persistence\n");

    /* Fresh board: the build time wins over the (missing) flash value. */
    hiflow_clock_init(&c, 0, VECTOR_TIME, 0);
    check_i64(hiflow_clock_now(&c, 0), VECTOR_TIME, "now at uptime 0");
    check_i64(hiflow_clock_now(&c, 1500), VECTOR_TIME + 1, "now after 1.5 s");
    check_i64(hiflow_clock_now(&c, 3600 * 1000), VECTOR_TIME + 3600, "now after an hour");

    /* A rubbish build time cannot drag the clock into the past. */
    hiflow_clock_init(&c, 0, 0, 0);
    check(hiflow_clock_now(&c, 0) >= HIFLOW_TIME_MIN, "implausible build time clamped");

    /* Reboot: the value saved before the reboot is ahead of the build time. */
    hiflow_clock_init(&c, 0, VECTOR_TIME, VECTOR_TIME + 86400);
    check_i64(hiflow_clock_now(&c, 0), VECTOR_TIME + 86400 + 1, "persisted value wins");

    /* Past the 32-bit millis() wrap the old clock jumped back 49.7 days. */
    hiflow_clock_init(&c, 0, VECTOR_TIME, 0);
    check_i64(hiflow_clock_now(&c, (int64_t) 4294967296LL + 5000), VECTOR_TIME + 4294967 + 5,
              "no jump at the 32-bit uptime wrap");
    check_i64(hiflow_clock_now(&c, (int64_t) 100 * 86400 * 1000), VECTOR_TIME + 100 * 86400,
              "100 days of uptime");
}

static void test_device_time(void)
{
    hiflow_clock_t c;

    printf("[2] device time from the login acknowledgement\n");

    hiflow_clock_init(&c, 0, VECTOR_TIME, 0);
    check(hiflow_clock_observe_device_time(&c, 1000, VECTOR_TIME + 3600) == 1, "ahead is adopted");
    check_i64(hiflow_clock_now(&c, 1000), VECTOR_TIME + 3600, "clock moved to the device time");
    check_i64(hiflow_clock_now(&c, 61000), VECTOR_TIME + 3660, "and keeps running from there");

    check(hiflow_clock_observe_device_time(&c, 61000, VECTOR_TIME) == 0, "behind is ignored");
    check(hiflow_clock_observe_device_time(&c, 61000, -1607349313) == 0, "garbage is ignored");
    check(hiflow_clock_observe_device_time(&c, 61000, 0) == 0, "zero is ignored");
    check(hiflow_clock_observe_device_time(&c, 61000, HIFLOW_TIME_MAX + 1) == 0, "far future ignored");
    check_i64(hiflow_clock_now(&c, 61000), VECTOR_TIME + 3660, "clock untouched by the rejects");
}

static void test_dst(void)
{
    /* Last Sundays: 2025-03-30 / 2025-10-26, 2026-03-29 / 2026-10-25,
       2027-03-28 / 2027-10-31. Switch is at 01:00 UTC. */
    const struct {
        int64_t t;
        int dst;
        const char *what;
    } cases[] = {
        {1743296399, 0, "2025-03-30 00:59:59 UTC, still CET"},
        {1743296400, 1, "2025-03-30 01:00:00 UTC, CEST starts"},
        {1761440399, 1, "2025-10-26 00:59:59 UTC, still CEST"},
        {1761440400, 0, "2025-10-26 01:00:00 UTC, back to CET"},
        {1774746000, 1, "2026-03-29 01:00:00 UTC, CEST starts"},
        {1774745999, 0, "2026-03-29 00:59:59 UTC, still CET"},
        {1792889999, 1, "2026-10-25 00:59:59 UTC, still CEST"},
        {1792890000, 0, "2026-10-25 01:00:00 UTC, back to CET"},
        {1806195600, 1, "2027-03-28 01:00:00 UTC, CEST starts"},
        {1824944400, 0, "2027-10-31 01:00:00 UTC, back to CET"},
        {VECTOR_TIME, 0, "2026-03-20, winter"},
        {1782000000, 1, "2026-06-21, summer"},
    };
    size_t i;

    printf("[3] European summer time\n");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        check(hiflow_is_eu_dst(cases[i].t) == cases[i].dst, cases[i].what);
        check_i64(hiflow_tz_offset_eu(cases[i].t, 3600), cases[i].dst ? 7200 : 3600,
                  "offset for the same instant");
    }
}

static void test_format(void)
{
    char buf[HIFLOW_TIME_STR_LEN];

    printf("[4] local time string\n");

    /* Same value as request_payloads.time_ymd_hms in test/vectors.json. */
    check_i64((int64_t) hiflow_format_local_time(VECTOR_TIME, 7200, buf, sizeof(buf)), 19,
              "string length");
    check_str(buf, "2026-03-20 11:46:40", "vector timestamp");

    hiflow_format_local_time(VECTOR_TIME, 3600, buf, sizeof(buf));
    check_str(buf, "2026-03-20 10:46:40", "same instant in CET");

    hiflow_format_local_time(0, 0, buf, sizeof(buf));
    check_str(buf, "1970-01-01 00:00:00", "unix epoch");

    /* Leap day and end of year. */
    hiflow_format_local_time(1709164800, 0, buf, sizeof(buf));
    check_str(buf, "2024-02-29 00:00:00", "leap day");
    hiflow_format_local_time(1767225599, 0, buf, sizeof(buf));
    check_str(buf, "2025-12-31 23:59:59", "end of year");

    check_i64((int64_t) hiflow_format_local_time(VECTOR_TIME, 0, buf, 5), 0, "short buffer refused");
}

int main(void)
{
    printf("=== hiflow_clock ===\n");
    test_base();
    test_device_time();
    test_dst();
    test_format();

    printf("\n=== summary ===\n");
    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0)
        printf("clock: %d/%d ok — ALL PASS\n", g_pass, g_pass);
    return g_fail == 0 ? 0 : 1;
}
