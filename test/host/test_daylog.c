/*
 * test_daylog.c — host test for the day log (hiflow_daylog.c) on its own.
 *
 * What it has to hold:
 *   - standby follows the readings: five minutes without power, a minute
 *     with it to wake; never on a link lost with power, before the first
 *     reading, while the bridge holds the inverter at 0 W and it answers, or
 *     while it refuses the session
 *   - a new day comes from the inverter's counters, over any gap; a bad value
 *     makes none
 *   - the clock only places the curve: the same days with any clock, or
 *     none, give the same standby and the same days
 *   - a reboot changes nothing, and a record of version 3 is taken over
 *   - recorded days replay with one standby per night (only when a file of
 *     them is given: real readings are not part of the repo, see test_replay)
 *
 * Build/run:  make -C test/host test-daylog [REPLAY=<recorded days>]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hiflow_daylog.h"

static int g_pass;
static int g_fail;

static void expect(int ok, const char *what)
{
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL %s\n", what);
    }
}

static void expect_int(long got, long want, const char *what)
{
    if (got == want) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL %s: got %ld want %ld\n", what, got, want);
    }
}

static void expect_float(float got, float want, const char *what)
{
    if (fabsf(got - want) < 0.01f) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL %s: got %.3f want %.3f\n", what, (double) got, (double) want);
    }
}

/* Local midnight of the first day. */
#define DAY0     20000
#define MIDNIGHT ((int64_t) DAY0 * 86400)
#define HOUR     3600

/* ---------- a board around the day log ---------- */

/* The inverter behind a day log: its counters grow with the power it feeds
   in, each input by its share, reported in whole Wh, the sums over the inputs
   as the session makes them. The board reads it on a grid of `every` seconds
   of uptime and updates the day log every second, as the component does. */
typedef struct {
    hiflow_daylog_t d;
    int64_t up_ms;
    int64_t real0;                         /* the real local time at uptime 0 */
    int64_t lag_s;                         /* the board's clock behind it */
    int     clock_ok;
    int     facts;
    int     ports;                         /* the readings carry the inputs */
    double  share[HIFLOW_DAYLOG_PORTS];
    double  wh[HIFLOW_DAYLOG_PORTS];       /* lifetime energy per input */
    double  day_wh[HIFLOW_DAYLOG_PORTS];   /* day energy per input */
    int     enters, leaves;
    int64_t enter_ms, leave_ms;            /* uptime of the last change */
} rig_t;

static void rig_init(rig_t *g, int64_t real0, int clock_ok)
{
    static const double SHARE[HIFLOW_DAYLOG_PORTS] = {0.2, 0.3, 0.3, 0.2};
    int i;

    memset(g, 0, sizeof(*g));
    hiflow_daylog_init(&g->d);
    g->real0 = real0;
    g->clock_ok = clock_ok;
    g->ports = 1;
    for (i = 0; i < HIFLOW_DAYLOG_PORTS; i++) {
        g->share[i] = SHARE[i];
        g->wh[i] = 12000.0 + 1000.0 * i;
    }
}

static int64_t rig_real(const rig_t *g) { return g->real0 + g->up_ms / 1000; }

static void rig_reading(const rig_t *g, float ac_w, hiflow_reading_t *r)
{
    double total = 0.0, daily = 0.0;
    int i;

    memset(r, 0, sizeof(*r));
    r->now_ms = g->up_ms;
    r->local_time = rig_real(g) - g->lag_s;
    r->clock_ok = g->clock_ok;
    r->ac_w = ac_w;
    for (i = 0; i < HIFLOW_DAYLOG_PORTS; i++) {
        total += floor(g->wh[i]);
        daily += floor(g->day_wh[i]);
        r->port_total_wh[i] = g->ports ? (float) floor(g->wh[i]) : 0.0f;
        r->port_daily_wh[i] = g->ports ? (float) floor(g->day_wh[i]) : -1.0f;
    }
    r->energy_total_wh = (float) total;
    r->energy_daily_wh = (float) daily;
}

static void rig_read(rig_t *g, float ac_w)
{
    hiflow_reading_t r;

    rig_reading(g, ac_w, &r);
    hiflow_daylog_sample(&g->d, &r);
}

/* The inverter feeds in `ac_w` for `secs` seconds (counters only). */
static void inverter_feeds(rig_t *g, int64_t secs, float ac_w)
{
    int i;

    for (i = 0; i < HIFLOW_DAYLOG_PORTS; i++) {
        g->wh[i] += ac_w * g->share[i] * (double) secs / 3600.0;
        g->day_wh[i] += ac_w * g->share[i] * (double) secs / 3600.0;
    }
}

/* The inverter wakes in the morning: its day counters start again. */
static void inverter_wakes(rig_t *g)
{
    int i;

    for (i = 0; i < HIFLOW_DAYLOG_PORTS; i++)
        g->day_wh[i] = 0.0;
}

static void rig_step(rig_t *g, float ac_w, int every)
{
    int standby;

    g->up_ms += 1000;
    inverter_feeds(g, 1, ac_w);
    if (every > 0 && (g->up_ms / 1000) % every == 0)
        rig_read(g, ac_w);
    standby = g->d.rec.standby;
    hiflow_daylog_update(&g->d, g->up_ms, g->facts);
    if (g->d.rec.standby && !standby) {
        g->enters++;
        g->enter_ms = g->up_ms;
    }
    if (!g->d.rec.standby && standby) {
        g->leaves++;
        g->leave_ms = g->up_ms;
    }
}

/* Runs until uptime `to_s`: the inverter feeds in `ac_w`, read every `every`
   s of uptime (0: no link). */
static void rig_to(rig_t *g, int64_t to_s, float ac_w, int every)
{
    while (g->up_ms < to_s * 1000)
        rig_step(g, ac_w, every);
}

/* The board is off for `off_s`, then boots from `flash` (NULL: nothing) with
   its clock `lag_s` behind the real time. */
static void rig_boot(rig_t *g, const hiflow_dayrec_t *flash, int64_t off_s, int64_t lag_s, int clock_ok)
{
    g->real0 = rig_real(g) + off_s;
    g->up_ms = 0;
    g->lag_s = lag_s;
    g->clock_ok = clock_ok;
    hiflow_daylog_init(&g->d);
    if (flash != NULL)
        expect_int(hiflow_daylog_restore(&g->d, flash, g->up_ms), 1, "boot: flash restored");
}

/* ---------- standby ---------- */

static void test_standby(void)
{
    static rig_t g;
    const int64_t noon = MIDNIGHT + 12 * HOUR;

    printf("[1] standby: five minutes without power, a minute with it to wake\n");

    rig_init(&g, noon, 1);
    expect_int(g.d.rec.standby, 0, "no reading yet: not standby");
    rig_to(&g, 600, 500.0f, 30); /* readings 30..600 s */
    expect_int(g.d.rec.standby, 0, "feeding in: awake");

    /* Dusk: 0 W from the reading at 630 s on, the link up. */
    rig_to(&g, 929, 0.0f, 30);
    expect_int(g.d.rec.standby, 0, "0 W for 4:59: not yet");
    rig_to(&g, 930, 0.0f, 30);
    expect_int(g.d.rec.standby, 1, "0 W for 5:00: standby");
    expect_int(g.enters, 1, "entered once");

    /* A lone reading with power at dusk, silence, a lone one at dawn: that
       is no minute of power. */
    rig_to(&g, 960, 3.0f, 30);
    rig_to(&g, 3600, 0.0f, 0);
    rig_to(&g, 3630, 2.0f, 30);
    rig_to(&g, 3660, 0.0f, 30);
    expect_int(g.d.rec.standby, 1, "lone readings with power: still standby");

    /* Readings with power a minute apart wake it, half a minute does not. */
    rig_to(&g, 3749, 20.0f, 30); /* readings at 3690 and 3720 */
    expect_int(g.d.rec.standby, 1, "power for 30 s: still standby");
    rig_to(&g, 3750, 20.0f, 30); /* and 3750 */
    expect_int(g.d.rec.standby, 0, "power for 60 s: awake");
    expect_int(g.leaves, 1, "woke once");

    /* A link lost with power is a fault: never standby, however long. */
    rig_to(&g, 4020, 400.0f, 30);
    rig_to(&g, 4020 + 8 * HOUR, 400.0f, 0);
    expect_int(g.d.rec.standby, 0, "a link lost at 400 W, for 8 hours: no standby");
    /* It comes back at 0 W: five minutes from that reading. */
    rig_to(&g, 4020 + 8 * HOUR + 30, 0.0f, 30);
    rig_to(&g, 4020 + 8 * HOUR + 329, 0.0f, 0);
    expect_int(g.d.rec.standby, 0, "then 0 W for 4:59: not yet");
    rig_to(&g, 4020 + 8 * HOUR + 330, 0.0f, 0);
    expect_int(g.d.rec.standby, 1, "0 W, then silent: standby");

    /* Below 1 W is nothing, 1 W is power. */
    rig_init(&g, noon, 1);
    rig_to(&g, 30, 0.9f, 30);
    rig_to(&g, 330, 0.9f, 0);
    expect_int(g.d.rec.standby, 1, "0.9 W is nothing");
    rig_init(&g, noon, 1);
    rig_to(&g, 30, 1.0f, 30);
    rig_to(&g, 3 * HOUR, 1.0f, 0);
    expect_int(g.d.rec.standby, 0, "1 W is power");

    /* Being set up: never a reading, never standby. */
    rig_init(&g, noon, 1);
    rig_to(&g, 48 * HOUR, 0.0f, 0);
    expect_int(g.d.rec.standby, 0, "no reading ever: not standby");
}

static void test_held_and_refused(void)
{
    static rig_t g;
    const int64_t noon = MIDNIGHT + 12 * HOUR;

    printf("[2] standby: held at 0 W by the bridge, refused by the inverter\n");

    /* Switched off by the bridge: no standby while the inverter answers. */
    rig_init(&g, noon, 1);
    rig_to(&g, 600, 500.0f, 30);
    g.facts = HIFLOW_FACT_SWITCH_OFF;
    rig_to(&g, 2 * HOUR, 0.0f, 30);
    expect_int(g.d.rec.standby, 0, "switched off, answering: no standby");
    expect_int(hiflow_daylog_inverter_off(&g.d), 1, "the inverter is off");
    /* Its radio goes dark: standby five minutes after the last reading. */
    rig_to(&g, 2 * HOUR + 299, 0.0f, 0);
    expect_int(g.d.rec.standby, 0, "silent for 4:59: not yet");
    rig_to(&g, 2 * HOUR + 300, 0.0f, 0);
    expect_int(g.d.rec.standby, 1, "silent for 5:00: standby all the same");
    /* The link comes and goes at dusk: it stays standby. */
    rig_to(&g, 2 * HOUR + 600, 0.0f, 30);
    expect_int(g.d.rec.standby, 1, "held off and answering again: standby stays");
    /* Dawn: the inverter switches itself on and feeds in. */
    inverter_wakes(&g);
    rig_to(&g, 12 * HOUR, 0.0f, 30);
    expect_int(hiflow_daylog_inverter_off(&g.d), 1, "at 0 W the bridge still has it off");
    rig_to(&g, 12 * HOUR + 30, 30.0f, 30);
    expect_int(hiflow_daylog_inverter_off(&g.d), 1, "one reading with power: could be one in flight");
    rig_to(&g, 12 * HOUR + 60, 30.0f, 30);
    expect_int(hiflow_daylog_inverter_off(&g.d), 0, "it feeds in: on, whatever the switch says");
    rig_to(&g, 12 * HOUR + 90, 30.0f, 30);
    expect_int(g.d.rec.standby, 0, "and awake");
    /* The next off counts again. */
    g.facts = 0;
    rig_to(&g, 12 * HOUR + 120, 30.0f, 30);
    g.facts = HIFLOW_FACT_SWITCH_OFF;
    rig_to(&g, 12 * HOUR + 150, 0.0f, 30);
    expect_int(hiflow_daylog_inverter_off(&g.d), 1, "switched off again: off");

    /* The inverter needs seconds to stop: a reading in flight still shows
       power after the off command, and the switch stays off. */
    rig_init(&g, noon, 1);
    rig_to(&g, 600, 500.0f, 30);
    g.facts = HIFLOW_FACT_SWITCH_OFF;
    rig_to(&g, 630, 500.0f, 30);
    expect_int(hiflow_daylog_inverter_off(&g.d), 1, "a reading with power right after the off command");
    rig_to(&g, 2 * HOUR, 0.0f, 30);
    expect_int(hiflow_daylog_inverter_off(&g.d), 1, "the next ones at 0 W: still off");
    expect_int(g.d.rec.standby, 0, "and no standby while it answers");

    /* A power limit of 0 %: the same while it answers. */
    rig_init(&g, noon, 1);
    rig_to(&g, 600, 500.0f, 30);
    g.facts = HIFLOW_FACT_LIMIT_ZERO;
    rig_to(&g, 2 * HOUR, 0.0f, 30);
    expect_int(g.d.rec.standby, 0, "a 0 % limit, answering: no standby");
    expect_int(hiflow_daylog_inverter_off(&g.d), 0, "not switched off");

    /* The inverter refuses the PIN: awake while it does, standby once it has
       been quiet for five minutes. */
    rig_init(&g, noon, 1);
    rig_to(&g, 30, 0.0f, 30);
    rig_to(&g, 400, 0.0f, 0);
    expect_int(g.d.rec.standby, 1, "standby");
    g.facts = HIFLOW_FACT_REFUSED;
    rig_to(&g, 401, 0.0f, 0);
    expect_int(g.d.rec.standby, 0, "refused: awake, that needs fixing");
    g.facts = 0; /* the next attempt */
    rig_to(&g, 401 + 299, 0.0f, 0);
    expect_int(g.d.rec.standby, 0, "4:59 after the refusal: still shown");
    rig_to(&g, 401 + 300, 0.0f, 0);
    expect_int(g.d.rec.standby, 1, "quiet for 5 minutes: standby");
}

/* ---------- the day ---------- */

static void test_new_day(void)
{
    static rig_t g;
    hiflow_dayrec_t flash;
    hiflow_reading_t r;
    float total, daily, kept;
    uint32_t days;
    int i;

    printf("[3] a new day: the inverter's day counters restart; bad values make none\n");

    rig_init(&g, MIDNIGHT + 7 * HOUR, 1); /* the board starts at 07:00 */
    rig_to(&g, 30, 0.0f, 30);
    expect_int(g.d.days_begun, 0, "the first reading ever: nothing to compare");
    expect_int(g.d.rec.day, DAY0, "the day, dated by the trusted clock");
    rig_to(&g, 11 * HOUR, 600.0f, 30); /* until 18:00 */
    expect_int(g.d.days_begun, 0, "a day in lockstep: no new day");
    daily = g.d.rec.energy_daily_wh;
    expect(daily > 6590.0f && daily <= 6600.0f, "the day's energy from the counters");
    rig_to(&g, 11 * HOUR + 1800, 0.0f, 30); /* dusk */
    rig_to(&g, 24 * HOUR, 0.0f, 0);         /* the night */
    total = g.d.rec.energy_total_wh;

    /* 07:00: the radio is back while the day counters still read yesterday's
       (on one of the recorded mornings they restarted minutes later). */
    rig_to(&g, 24 * HOUR + 300, 0.0f, 30);
    expect_int(g.d.days_begun, 0, "yesterday's day counters at dawn: the same day");
    expect_float(g.d.rec.energy_daily_wh, daily, "the day's energy stays");
    inverter_wakes(&g);
    rig_to(&g, 24 * HOUR + 330, 0.0f, 30);
    expect_int(g.d.days_begun, 0, "the day counters restarted, one reading: not yet");
    rig_to(&g, 24 * HOUR + 360, 0.0f, 30);
    expect_int(g.d.days_begun, 1, "and the next reading: a new day");
    expect_float(g.d.rec.energy_daily_wh, 0.0f, "its energy");
    expect_float(g.d.rec.peak_w, 0.0f, "its peak");
    expect_int(hiflow_daylog_has_curve(&g.d), 0, "its curve");
    expect_float(g.d.rec.port_daily_wh[2], 0.0f, "its input energy");
    expect_int(g.d.rec.day, DAY0 + 1, "dated by the trusted clock");
    expect_float(g.d.rec.energy_total_wh, total, "the lifetime energy stays");
    expect_int(g.d.rec.standby, 1, "standby until it feeds in");

    /* Day 2 is dull: 300 W for two hours. The board is off from its evening
       to the noon of day 3, which is sunny: at the first reading the day
       counters are above day 2's, but the lifetime counters grew by more, so
       it is a new day. The board's clock lags and is not trusted. */
    rig_to(&g, 26 * HOUR + 330, 300.0f, 30);
    rig_to(&g, 26 * HOUR + 1800, 0.0f, 30);
    flash = g.d.rec;
    expect(flash.energy_daily_wh > 590.0f && flash.energy_daily_wh <= 600.0f, "day 2: 600 Wh");
    inverter_wakes(&g);
    inverter_feeds(&g, 5 * HOUR, 800.0f);
    rig_boot(&g, &flash, 26 * HOUR, 20 * HOUR, 0);
    rig_to(&g, 60, 800.0f, 30);
    expect_int(g.d.days_begun, 1, "a sunnier day after a day off: a new day");
    expect(g.d.rec.energy_daily_wh > 3990.0f, "its energy from the counters");
    expect_int(g.d.rec.day, -1, "undated: the clock is not trusted");
    expect_int(hiflow_daylog_has_curve(&g.d), 0, "no curve without a trusted clock");
    expect_float(g.d.rec.peak_w, 800.0f, "the peak");
    expect_int(g.d.rec.peak_min, -1, "without its time");

    /* One input's day counter reads 0 at noon: the other inputs keep pace,
       no new day, and nothing drops. */
    rig_to(&g, 600, 800.0f, 30);
    days = g.d.days_begun;
    kept = g.d.rec.port_daily_wh[1];
    daily = g.d.rec.energy_daily_wh;
    rig_reading(&g, 800.0f, &r);
    r.energy_daily_wh -= r.port_daily_wh[1];
    r.port_daily_wh[1] = 0.0f;
    hiflow_daylog_sample(&g.d, &r);
    expect_int(g.d.days_begun, days, "one input reads 0: outvoted, no new day");
    expect(g.d.rec.port_daily_wh[1] >= kept, "its day energy does not drop");
    expect(g.d.rec.energy_daily_wh >= daily, "nor the day's");
    /* One input's day counter is broken and reads 0 from now on: the other
       inputs keep pace and outvote it, reading after reading. */
    for (i = 0; i < 4; i++) {
        rig_reading(&g, 800.0f, &r);
        r.energy_daily_wh -= r.port_daily_wh[1];
        r.port_daily_wh[1] = 0.0f;
        r.now_ms += 30000 * (i + 1);
        hiflow_daylog_sample(&g.d, &r);
    }
    expect_int(g.d.days_begun, days, "an input stuck at 0: outvoted, no new day");
    /* Lifetime counters that went back in one reading: a bad reading, and the
       next good one goes on with the day. */
    rig_reading(&g, 800.0f, &r);
    for (i = 0; i < HIFLOW_DAYLOG_PORTS; i++)
        r.port_total_wh[i] -= 500.0f;
    r.energy_total_wh -= 2000.0f;
    hiflow_daylog_sample(&g.d, &r);
    expect_int(g.d.days_begun, days, "lifetime counters gone back: no new day");
    rig_to(&g, 690, 800.0f, 30);
    expect_int(g.d.days_begun, days, "the next good reading: the same day");
    /* Another inverter (its counters stay lower): taken on the second reading,
       and it starts a day of its own. */
    for (i = 0; i < HIFLOW_DAYLOG_PORTS; i++) {
        g.wh[i] = 500.0 + 100.0 * i;
        g.day_wh[i] = 50.0;
    }
    rig_to(&g, 720, 800.0f, 30);
    expect_int(g.d.days_begun, days, "another inverter, first reading: not yet");
    rig_to(&g, 810, 800.0f, 30);
    expect_int(g.d.days_begun, days + 1, "its counters stay: its own day");
    expect(g.d.rec.energy_total_wh < 3000.0f, "with its lifetime energy");
    expect(g.d.rec.energy_daily_wh < 300.0f, "and its day energy");

    /* An input without sun (a dead string, its day counter always 0) does not
       hold back the new day. */
    rig_init(&g, MIDNIGHT + 7 * HOUR, 1);
    g.share[3] = 0.0;
    rig_to(&g, 11 * HOUR, 600.0f, 30);
    rig_to(&g, 24 * HOUR, 0.0f, 0);
    inverter_wakes(&g);
    rig_to(&g, 24 * HOUR + 60, 0.0f, 30);
    expect_int(g.d.days_begun, 1, "a dead input: a new day all the same");

    /* An input that made hardly anything the day before (shaded, or a
       faulty panel) has a day counter within the slack of 0: its restart
       shows nothing, and it must not outvote the inputs that do show it. */
    rig_init(&g, MIDNIGHT + 7 * HOUR, 1);
    g.share[3] = 0.0;
    rig_to(&g, 11 * HOUR, 600.0f, 30);
    g.day_wh[3] = 3.0;
    rig_to(&g, 11 * HOUR + 60, 600.0f, 30);
    rig_to(&g, 24 * HOUR, 0.0f, 0);
    inverter_wakes(&g);
    rig_to(&g, 24 * HOUR + 60, 0.0f, 30);
    expect_int(g.d.days_begun, 1, "an input with 3 Wh yesterday: a new day all the same");
    expect_float(g.d.rec.port_daily_wh[2], 0.0f, "the day's input energy starts again");

    /* An inverter without the inputs: the sums decide. */
    rig_init(&g, MIDNIGHT + 7 * HOUR, 1);
    g.ports = 0;
    rig_to(&g, 11 * HOUR, 600.0f, 30);
    rig_to(&g, 24 * HOUR, 0.0f, 0);
    inverter_wakes(&g);
    rig_to(&g, 24 * HOUR + 60, 0.0f, 30);
    expect_int(g.d.days_begun, 1, "sums only: a new day");

    /* The empty reply of the recorded days: in one reading the day counter
       is 0 and the lifetime counter did not move, with only the sums to go
       by. One reading is no restart, and nothing of it is kept. */
    rig_to(&g, 26 * HOUR, 600.0f, 30);
    days = g.d.days_begun;
    daily = g.d.rec.energy_daily_wh;
    rig_reading(&g, 0.0f, &r);
    r.energy_daily_wh = 0.0f;
    hiflow_daylog_sample(&g.d, &r);
    expect_int(g.d.days_begun, days, "a day counter at 0 in one reading: no new day");
    expect_float(g.d.rec.energy_daily_wh, daily, "and the day's energy stays");
    rig_to(&g, 26 * HOUR + 60, 600.0f, 30);
    expect_int(g.d.days_begun, days, "the next readings go on with the day");

    /* A day without any energy (snow): the counters cannot tell the next
       one, and the empty record just goes on; a trusted clock dates it anew. */
    rig_init(&g, MIDNIGHT + 7 * HOUR, 1);
    rig_to(&g, 11 * HOUR, 0.0f, 30);
    rig_to(&g, 24 * HOUR, 0.0f, 0);
    inverter_wakes(&g);
    rig_to(&g, 24 * HOUR + 30, 0.0f, 30);
    expect_int(g.d.days_begun, 0, "after a day without energy: the record goes on");
    expect_int(g.d.rec.day, DAY0 + 1, "dated anew by the trusted clock");
    rig_to(&g, 25 * HOUR, 300.0f, 30);
    expect_int(g.d.rec.day, DAY0 + 1, "a day with power keeps its date");

    /* The midnight sun: the inverter feeds in around the clock and restarts
       its day counters at its midnight. A new day there, and never standby. */
    rig_init(&g, MIDNIGHT + 12 * HOUR, 1);
    rig_to(&g, 12 * HOUR, 150.0f, 30);
    inverter_wakes(&g);
    rig_to(&g, 36 * HOUR, 150.0f, 30);
    expect_int(g.d.days_begun, 1, "midnight sun: a new day at the counters' midnight");
    expect_int(g.enters, 0, "and never standby");
}

static void test_clock(void)
{
    static rig_t g, runs[4];
    static const int64_t LAG[4] = {0, 7 * HOUR, -10 * HOUR, 0};
    static const int TRUSTED[4] = {1, 1, 1, 0};
    int k, day;

    printf("[4] the clock only places the curve: any clock, or none, the same days\n");

    /* No trusted clock: the peak without its time, no curve, no date. */
    rig_init(&g, MIDNIGHT + 12 * HOUR, 0);
    rig_to(&g, 60, 500.0f, 30);
    expect_float(g.d.rec.peak_w, 500.0f, "the peak");
    expect_int(g.d.rec.peak_min, -1, "without its time");
    expect_int(hiflow_daylog_has_curve(&g.d), 0, "no curve");
    expect_int(g.d.rec.day, -1, "no date");
    /* Once it is trusted, the next reading dates the day and fills the curve. */
    g.clock_ok = 1;
    rig_to(&g, 90, 600.0f, 30);
    expect_int(g.d.rec.day, DAY0, "dated");
    expect_int(g.d.rec.curve[(12 * 60 + 1) / 5], 600, "the curve slot of 12:01");
    expect_int(g.d.rec.peak_min, 12 * 60 + 1, "the peak at 12:01");

    /* Three days (sunrise 06:40, sunset 19:10, flapping dawn and dusk links)
       with the board's clock right, 7 hours behind, 10 hours ahead, and not
       trusted: standby and the days come out the same. */
    for (k = 0; k < 4; k++) {
        rig_t *b = &runs[k];
        rig_init(b, MIDNIGHT, TRUSTED[k]);
        b->lag_s = LAG[k];
        for (day = 0; day < 3; day++) {
            const int64_t m = (int64_t) day * 24 * HOUR;
            int s;
            rig_to(b, m + 6 * HOUR + 30 * 60, 0.0f, 0);
            inverter_wakes(b);
            for (s = 0; s < 10 * 60; s++) /* the link comes and goes, 0 W */
                rig_step(b, 0.0f, (s % 70) < 10 ? 30 : 0);
            rig_to(b, m + 6 * HOUR + 40 * 60, 0.0f, 30);
            rig_to(b, m + 19 * HOUR + 10 * 60, 700.0f, 30);
            rig_to(b, m + 19 * HOUR + 30 * 60, 0.0f, 30);
            for (s = 0; s < 20 * 60; s++)
                rig_step(b, 0.0f, (s % 90) < 15 ? 30 : 0);
            rig_to(b, m + 24 * HOUR, 0.0f, 0);
        }
    }
    expect_int(runs[0].enters, 4, "right clock: standby at the first dawn (the log starts there) and every evening");
    expect_int(runs[0].leaves, 3, "awake every morning");
    expect_int((long) runs[0].d.days_begun, 2, "two new days after the first");
    for (k = 1; k < 4; k++) {
        expect_int(runs[k].enters, runs[0].enters, "another clock: the same standby");
        expect_int(runs[k].leaves, runs[0].leaves, "the same waking");
        expect_int((long) runs[k].enter_ms, (long) runs[0].enter_ms, "at the same time");
        expect_int((long) runs[k].leave_ms, (long) runs[0].leave_ms, "woken at the same time");
        expect_int((long) runs[k].d.days_begun, (long) runs[0].d.days_begun, "the same days");
        expect_float(runs[k].d.rec.energy_daily_wh, runs[0].d.rec.energy_daily_wh, "the same energy");
    }
    expect_int(hiflow_daylog_has_curve(&runs[3].d), 0, "no clock: no curve");
}

/* ---------- reboots and flash ---------- */

static void test_restore(void)
{
    static rig_t g;
    static hiflow_daylog_t d;
    hiflow_dayrec_t flash, v3;
    int16_t last_w;

    printf("[5] reboots: standby and the day come back, version 3 is taken over\n");

    /* Standby comes back at once; waking takes readings of this boot. */
    rig_init(&g, MIDNIGHT + 12 * HOUR, 1);
    rig_to(&g, 3 * HOUR, 500.0f, 30);
    rig_to(&g, 4 * HOUR, 0.0f, 30);
    expect_int(g.d.rec.standby, 1, "standby before the reboot");
    flash = g.d.rec;
    rig_boot(&g, &flash, 60, 0, 1);
    expect(memcmp(&g.d.rec, &flash, sizeof(flash)) == 0, "the record as it was");
    expect_int(hiflow_daylog_save_due(&g.d, 5000, 0), 0, "nothing to save after the restore");
    rig_to(&g, 1, 0.0f, 0);
    expect_int(g.d.rec.standby, 1, "standby at once");
    rig_to(&g, 30, 50.0f, 30);
    expect_int(g.d.rec.standby, 1, "one reading with power: still standby");
    rig_to(&g, 90, 50.0f, 30);
    expect_int(g.d.rec.standby, 0, "a minute of power: awake");

    /* A reboot at dusk, just after the first reading at 0 W... */
    rig_to(&g, 120, 0.0f, 30);
    flash = g.d.rec;
    rig_boot(&g, &flash, 60, 0, 1);
    rig_to(&g, 299, 0.0f, 0);
    expect_int(g.d.rec.standby, 0, "0 W before the reboot, silent 4:59 after: not yet");
    rig_to(&g, 300, 0.0f, 0);
    expect_int(g.d.rec.standby, 1, "5 minutes after the boot: standby");

    /* ...and a power cut while it fed in, back at night: what came before
       the boot says nothing about now, so the silence is standby, not a
       fault. */
    rig_to(&g, 600, 300.0f, 30);
    expect_int(g.d.rec.standby, 0, "feeding in again");
    flash = g.d.rec;
    expect_int(flash.last_feed, 1, "the last reading before the power cut had power");
    rig_boot(&g, &flash, 10 * HOUR, 0, 1);
    rig_to(&g, 299, 0.0f, 0);
    expect_int(g.d.rec.standby, 0, "back at night: not in the first 5 minutes");
    rig_to(&g, 300, 0.0f, 0);
    expect_int(g.d.rec.standby, 1, "silent for 5 minutes after the boot: standby");
    /* Being set up is another matter: no record, no standby. */
    rig_boot(&g, NULL, 60, 0, 1);
    rig_to(&g, 3 * HOUR, 0.0f, 0);
    expect_int(g.d.rec.standby, 0, "no record and no reading: being set up");

    /* The user's case: switched off at night, on again exactly 24 hours
       later. Standby at once, the day stays until the inverter's counters
       say the next one began. */
    rig_init(&g, MIDNIGHT + 7 * HOUR, 1);
    rig_to(&g, 11 * HOUR, 600.0f, 30);
    rig_to(&g, 16 * HOUR, 0.0f, 30); /* 23:00 */
    flash = g.d.rec;
    inverter_wakes(&g);
    inverter_feeds(&g, 11 * HOUR, 500.0f); /* the next day, without the board */
    rig_boot(&g, &flash, 24 * HOUR, 24 * HOUR, 0);
    rig_to(&g, 1, 0.0f, 0);
    expect_int(g.d.rec.standby, 1, "24 hours off: standby at once");
    expect_float(g.d.rec.energy_daily_wh, flash.energy_daily_wh, "the last day's energy on the pages");
    expect_int(g.d.rec.day, DAY0, "and its date");
    inverter_wakes(&g);
    rig_to(&g, 8 * HOUR, 0.0f, 0);
    rig_to(&g, 8 * HOUR + 60, 0.0f, 30); /* the next dawn */
    expect_int(g.d.days_begun, 1, "the next dawn: a new day");

    /* Version 3 kept the last reading in whole W where last_feed and standby
       are now. */
    v3 = flash;
    v3.version = 3;
    last_w = 0;
    memcpy(&v3.last_feed, &last_w, sizeof(last_w));
    hiflow_daylog_init(&d);
    expect_int(hiflow_daylog_restore(&d, &v3, 0), 1, "v3 at 0 W: taken");
    expect_int(d.rec.version, HIFLOW_DAYREC_VERSION, "now version 4");
    expect_int(d.rec.standby, 1, "v3 at 0 W: standby");
    expect_int(d.rec.last_feed, 0, "v3 at 0 W: no power");
    expect_float(d.rec.energy_daily_wh, v3.energy_daily_wh, "v3: the day's energy");
    expect_int(hiflow_daylog_save_due(&d, 1000, 0), 1, "v3: saved again as version 4");
    last_w = 812;
    memcpy(&v3.last_feed, &last_w, sizeof(last_w));
    hiflow_daylog_init(&d);
    hiflow_daylog_restore(&d, &v3, 0);
    expect_int(d.rec.standby, 0, "v3 at 812 W: not standby");
    expect_int(d.rec.last_feed, 1, "v3 at 812 W: power");
    last_w = -1;
    memcpy(&v3.last_feed, &last_w, sizeof(last_w));
    hiflow_daylog_init(&d);
    hiflow_daylog_restore(&d, &v3, 0);
    expect_int(d.rec.last_feed, -1, "v3 without a reading: none");

    /* Invalid records are refused and leave the log alone. */
    hiflow_daylog_init(&d);
    v3 = flash;
    v3.magic = 0;
    expect_int(hiflow_daylog_restore(&d, &v3, 0), 0, "a record without magic");
    expect_int(d.rec.last_feed, -1, "nothing taken");
    v3 = flash;
    v3.version = HIFLOW_DAYREC_VERSION + 1;
    expect_int(hiflow_daylog_restore(&d, &v3, 0), 0, "a record of a later version");
    v3.version = 2;
    expect_int(hiflow_daylog_restore(&d, &v3, 0), 0, "a record of version 2");
    v3 = flash;
    v3.peak_min = 24 * 60;
    expect_int(hiflow_daylog_restore(&d, &v3, 0), 0, "a peak after midnight");
    v3 = flash;
    v3.last_feed = 2;
    expect_int(hiflow_daylog_restore(&d, &v3, 0), 0, "a last reading of 2");
    v3 = flash;
    v3.standby = 2;
    expect_int(hiflow_daylog_restore(&d, &v3, 0), 0, "a standby of 2");
    v3 = flash;
    v3.day = -2;
    expect_int(hiflow_daylog_restore(&d, &v3, 0), 0, "a day of -2");
    v3 = flash;
    v3.day = -1;
    expect_int(hiflow_daylog_restore(&d, &v3, 0), 1, "an undated day is fine");
}

static void test_saves(void)
{
    static hiflow_daylog_t d;
    static rig_t g;
    int saves = 0, early = 0;
    int64_t last = -1;

    printf("[6] flash: saved as it changes, at most every 5 minutes (1 without a link)\n");

    hiflow_daylog_init(&d);
    expect_int(hiflow_daylog_save_due(&d, 1000, 1), 0, "nothing to save at boot");
    d.rec.peak_w = 500.0f;
    expect_int(hiflow_daylog_save_due(&d, 1000, 1), 1, "a change: due at once after boot");
    hiflow_daylog_mark_saved(&d, 1000, 0);
    expect_int(hiflow_daylog_save_due(&d, 2000, 1), 0, "failed: not again at once");
    expect_int(hiflow_daylog_save_due(&d, 1000 + HIFLOW_DAYLOG_SAVE_MS, 1), 1, "failed: again after the interval");
    expect_int(hiflow_daylog_save_due(&d, 1000 + HIFLOW_DAYLOG_SAVE_IDLE_MS, 0), 1, "without a link after a minute");
    hiflow_daylog_mark_saved(&d, 1000 + HIFLOW_DAYLOG_SAVE_MS, 1);
    expect_int(hiflow_daylog_save_due(&d, 1000 + 3 * HIFLOW_DAYLOG_SAVE_MS, 1), 0, "saved: nothing due");

    /* A day: about one save per 5 minutes of daylight, standby saved within
       a minute of dusk. */
    rig_init(&g, MIDNIGHT + 7 * HOUR, 1);
    while (g.up_ms < 12 * HOUR * 1000) {
        const float w = g.up_ms < 11 * HOUR * 1000 ? 600.0f : 0.0f;
        const int link = g.up_ms < 11 * HOUR * 1000 + 30 * 60 * 1000;
        rig_step(&g, w, link ? 30 : 0);
        if (hiflow_daylog_save_due(&g.d, g.up_ms, link)) {
            if (last >= 0 && g.up_ms - last < HIFLOW_DAYLOG_SAVE_IDLE_MS)
                early++;
            hiflow_daylog_mark_saved(&g.d, g.up_ms, 1);
            last = g.up_ms;
            saves++;
        }
    }
    expect(saves >= 11 * 12 && saves <= 11 * 12 + 12, "about one save per 5 minutes");
    expect_int(early, 0, "never two within a minute");
    expect(memcmp(&g.d.saved, &g.d.rec, sizeof(g.d.rec)) == 0, "flash holds the whole day");
    expect_int(g.d.saved.standby, 1, "and the standby");
}

/* ---------- recorded days ---------- */

#define REPLAY_MAX 4096

typedef struct {
    int     kind; /* 'R' a reading, 'H' facts from then on */
    int64_t t;
    float   ac, total, daily;
    float   port_total[HIFLOW_DAYLOG_PORTS], port_daily[HIFLOW_DAYLOG_PORTS];
    int     facts;
} replay_line_t;

static replay_line_t g_lines[REPLAY_MAX];
static int g_nlines;

static int load_replay(const char *path)
{
    FILE *f = fopen(path, "r");
    char buf[256];
    int i, n;

    if (f == NULL)
        return 0;
    g_nlines = 0;
    while (fgets(buf, sizeof(buf), f) != NULL && g_nlines < REPLAY_MAX) {
        replay_line_t *l = &g_lines[g_nlines];
        long long t;
        memset(l, 0, sizeof(*l));
        if (buf[0] == 'R') {
            float p[2 * HIFLOW_DAYLOG_PORTS];
            n = sscanf(buf, "R,%lld,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", &t, &l->ac, &l->total, &l->daily,
                       &p[0], &p[1], &p[2], &p[3], &p[4], &p[5], &p[6], &p[7]);
            if (n != 4 && n != 12)
                continue;
            l->kind = 'R';
            for (i = 0; i < HIFLOW_DAYLOG_PORTS; i++) {
                l->port_total[i] = n == 12 && p[2 * i] >= 0.0f ? p[2 * i] : 0.0f;
                l->port_daily[i] = n == 12 && p[2 * i] >= 0.0f ? p[2 * i + 1] : -1.0f;
            }
        } else if (buf[0] == 'H') {
            if (sscanf(buf, "H,%lld,%d", &t, &l->facts) != 2)
                continue;
            l->kind = 'H';
        } else {
            continue;
        }
        l->t = t;
        g_nlines++;
    }
    fclose(f);
    return g_nlines;
}

/* The time of the next reading from line `i` on, far ahead when none. */
static int64_t next_reading(int i)
{
    for (; i < g_nlines; i++) {
        if (g_lines[i].kind == 'R')
            return g_lines[i].t;
    }
    return (int64_t) 1 << 40;
}

typedef struct {
    int     enters, leaves, days;
    int64_t enter_t[16], leave_t[16], day_t[16];
    int     standby_by_day;   /* readings with power a minute into a run, in standby */
    int     woke_on_zero;     /* left standby at a reading without power */
    float   dusk_daily[16];   /* the day's energy on the evening's last reading */
    int     dusks;
} replay_result_t;

/* Runs the recorded days through a day log, one second at a time. The clock
   is `lag_s` behind (or not trusted); nothing but the curve may change. */
static void replay(int64_t lag_s, int clock_ok, replay_result_t *res)
{
    static hiflow_daylog_t d;
    int i = 0, standby, facts = 0;
    int64_t t, feed_since = -1;
    const int64_t t0 = g_lines[0].t, t1 = g_lines[g_nlines - 1].t + 2 * HOUR;

    memset(res, 0, sizeof(*res));
    hiflow_daylog_init(&d);
    for (t = t0; t <= t1; t++) {
        while (i < g_nlines && g_lines[i].t == t) {
            const replay_line_t *l = &g_lines[i++];
            if (l->kind == 'H') {
                facts = l->facts;
            } else {
                hiflow_reading_t r;
                const uint32_t days = d.days_begun;
                const int was = d.rec.standby;
                int k;
                memset(&r, 0, sizeof(r));
                r.now_ms = (t - t0) * 1000;
                r.local_time = MIDNIGHT + t - lag_s;
                r.clock_ok = clock_ok;
                r.ac_w = l->ac;
                r.energy_total_wh = l->total;
                r.energy_daily_wh = l->daily;
                for (k = 0; k < HIFLOW_DAYLOG_PORTS; k++) {
                    r.port_total_wh[k] = l->port_total[k];
                    r.port_daily_wh[k] = l->port_daily[k];
                }
                hiflow_daylog_sample(&d, &r);
                if (d.days_begun != days && res->days < 16)
                    res->day_t[res->days++] = t;
                if (l->ac >= 1.0f) {
                    if (feed_since < 0)
                        feed_since = t;
                    if (t - feed_since >= 90 && was)
                        res->standby_by_day++;
                } else {
                    feed_since = -1;
                    /* The last reading of an evening: no reading follows
                       within the next four hours. */
                    if (next_reading(i) - t > 4 * HOUR && res->dusks < 16)
                        res->dusk_daily[res->dusks++] = d.rec.energy_daily_wh;
                }
            }
        }
        standby = d.rec.standby;
        hiflow_daylog_update(&d, (t - t0) * 1000, facts);
        if (d.rec.standby && !standby && res->enters < 16)
            res->enter_t[res->enters++] = t;
        if (!d.rec.standby && standby && res->leaves < 16) {
            res->leave_t[res->leaves++] = t;
            if (d.rec.last_feed != 1)
                res->woke_on_zero++;
        }
    }
}

/* The file: lines "R,<s>,<AC W>,<lifetime Wh>,<day Wh>[,<PVn lifetime Wh>,
   <PVn day Wh> for n = 1..4]" (a reading; -1,-1 for an input without fresh
   counters) and "H,<s>,<HIFLOW_HELD_* flags>" (held from then on); <s> are
   seconds from local midnight of the first day, other lines are skipped. The
   checks expect five and a half days of dusk and dawn stretches. */
static void test_replay(const char *path)
{
    static replay_result_t base, other;
    static const int64_t LAG[3] = {7 * HOUR, -11 * HOUR, 0};
    static const int TRUSTED[3] = {1, 1, 0};
    int i, k, j, n;

    if (path == NULL) {
        printf("[7] recorded days: skipped (no file given)\n");
        return;
    }
    printf("[7] recorded days: one standby per night, one new day per morning\n");

    n = load_replay(path);
    expect(n > 1000, "the recorded days are there");
    if (n <= 1000)
        return;
    replay(0, 1, &base);

    /* Five and a half recorded days: five evenings end in standby, five
       mornings leave it, each morning begins one day. */
    expect_int(base.enters, 6, "six evenings: standby six times");
    expect_int(base.leaves, 5, "five mornings: awake five times");
    expect_int(base.days, 5, "five new days");
    expect_int(base.standby_by_day, 0, "never standby with power flowing");
    expect_int(base.woke_on_zero, 0, "never woke without power");
    for (i = 0; i < base.enters && i < base.leaves; i++) {
        const int64_t evening = base.enter_t[i] % 86400, morning = base.leave_t[i] % 86400;
        expect(evening > 16 * HOUR && evening < 20 * HOUR, "standby in the evening");
        expect(morning > 6 * HOUR && morning < 8 * HOUR, "awake in the morning");
        expect(base.leave_t[i] > base.enter_t[i], "after the night");
    }
    for (i = 0; i < base.days; i++) {
        const int64_t at = base.day_t[i] % 86400;
        expect(at > 6 * HOUR && at < 7 * HOUR, "each new day at dawn");
        /* Before its leaving standby. */
        for (j = 0; j < base.leaves; j++) {
            if (base.leave_t[j] / 86400 == base.day_t[i] / 86400)
                expect(base.day_t[i] < base.leave_t[j], "the day begins before the power");
        }
    }
    expect(base.dusks >= 5, "the evenings are there");
    for (i = 0; i < base.dusks; i++)
        expect(base.dusk_daily[i] > 800.0f, "each evening keeps its day's energy");

    /* Another clock, or none: the same standby and the same days. */
    for (k = 0; k < 3; k++) {
        replay(LAG[k], TRUSTED[k], &other);
        expect_int(other.enters, base.enters, "another clock: the same standby");
        expect_int(other.leaves, base.leaves, "the same waking");
        expect_int(other.days, base.days, "the same days");
        for (i = 0; i < base.enters && i < other.enters; i++)
            expect_int((long) other.enter_t[i], (long) base.enter_t[i], "standby at the same second");
        for (i = 0; i < base.days && i < other.days; i++)
            expect_int((long) other.day_t[i], (long) base.day_t[i], "the day at the same reading");
    }
}

int main(int argc, char **argv)
{
    const char *replay_path = argc > 1 ? argv[1] : NULL;

    printf("=== hiflow_daylog ===\n");
    test_standby();
    test_held_and_refused();
    test_new_day();
    test_clock();
    test_restore();
    test_saves();
    test_replay(replay_path);

    printf("\n=== summary ===\n");
    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0)
        printf("daylog: %d/%d ok — ALL PASS\n", g_pass, g_pass);
    return g_fail == 0 ? 0 : 1;
}
