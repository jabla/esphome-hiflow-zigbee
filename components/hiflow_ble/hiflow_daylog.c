#include "hiflow_daylog.h"

#include <string.h>

/* The record is compared byte for byte with what flash holds: no padding. */
_Static_assert(sizeof(hiflow_dayrec_t) == 40 + 2 * HIFLOW_DAYLOG_SLOTS, "hiflow_dayrec_t has padding");

/* How a pair of counters (lifetime, day) moved since the last reading. */
enum { COUNTERS_UNKNOWN, COUNTERS_SAME_DAY, COUNTERS_RESTARTED };

int32_t hiflow_local_day(int64_t local_time)
{
    return (int32_t) (local_time >= 0 ? local_time / 86400 : (local_time - 86399) / 86400);
}

int hiflow_local_minute(int64_t local_time)
{
    return (int) ((local_time - (int64_t) hiflow_local_day(local_time) * 86400) / 60);
}

/* The day's values go; the lifetime counter, the last reading and the standby
   state belong to the inverter, not to the day, and stay. */
static void clear_day(hiflow_daylog_t *d)
{
    memset(d->rec.curve, 0, sizeof(d->rec.curve));
    memset(d->rec.port_daily_wh, 0, sizeof(d->rec.port_daily_wh));
    d->rec.energy_daily_wh = 0.0f;
    d->rec.peak_w = 0.0f;
    d->rec.peak_min = -1;
    d->rec.day = -1;
}

void hiflow_daylog_init(hiflow_daylog_t *d)
{
    if (d == NULL)
        return;
    memset(d, 0, sizeof(*d));
    d->rec.magic = HIFLOW_DAYREC_MAGIC;
    d->rec.version = HIFLOW_DAYREC_VERSION;
    d->rec.day = -1;
    d->rec.peak_min = -1;
    d->rec.last_feed = -1;
    d->saved = d->rec;
    d->saved_ms = -1;
    d->run_since_ms = -1;
    d->run_last_ms = -1;
    d->refused_ms = -1;
}

/* Within a day the day counter grows exactly as much as the lifetime counter;
   one that grew less has restarted. A day counter that was within the slack
   before says nothing: it fits a new day as well as a day without sun so far
   (and a new day sets it to 0 while the lifetime counter stays), and a
   restart from so little cannot be told from the drift of the two counters. */
static int counters(float total_before, float daily_before, float total, float daily)
{
    float grew_total, grew_daily;

    if (!(total_before > 0.0f) || !(daily_before > HIFLOW_DAY_SLACK_WH) || !(total > 0.0f) || !(daily >= 0.0f))
        return COUNTERS_UNKNOWN;
    grew_total = total - total_before;
    grew_daily = daily - daily_before;
    if (grew_total < -HIFLOW_DAY_SLACK_WH)
        return COUNTERS_UNKNOWN; /* a lifetime counter never goes back: a bad value */
    return grew_daily < grew_total - HIFLOW_DAY_SLACK_WH ? COUNTERS_RESTARTED : COUNTERS_SAME_DAY;
}

static int new_day(const hiflow_daylog_t *d, const hiflow_reading_t *r)
{
    int i, v, restarted = 0, same = 0;

    for (i = 0; i < HIFLOW_DAYLOG_PORTS; i++) {
        v = counters(d->port_total_wh[i], d->rec.port_daily_wh[i], r->port_total_wh[i], r->port_daily_wh[i]);
        restarted += v == COUNTERS_RESTARTED;
        same += v == COUNTERS_SAME_DAY;
    }
    if (same > 0)
        return 0;
    if (restarted > 0)
        return 1;
    /* No input to go by (after a boot their lifetime counters are not known
       yet): the sums over all inputs. */
    return counters(d->rec.energy_total_wh, d->rec.energy_daily_wh, r->energy_total_wh, r->energy_daily_wh) ==
           COUNTERS_RESTARTED;
}

static float at_least(float kept, float v) { return v > kept ? v : kept; }

/* A lifetime counter never goes back: a lower value is a bad one, unless the
   next reading has it too (another inverter, or its counters were reset). */
static float take_total(float kept, float v, uint8_t *back)
{
    if (kept > 0.0f && v < kept - HIFLOW_DAY_SLACK_WH && ++*back < 2)
        return kept;
    *back = 0;
    return v;
}

/* The runs standby is judged by: readings with power, or without. A run
   ends with a reading of the other kind, or after as long a silence as
   standby takes (the poll interval is a minute at most): a reading at dusk
   and one at dawn are not a minute of power. */
static void note_run(hiflow_daylog_t *d, const hiflow_reading_t *r)
{
    const int feed = r->ac_w >= HIFLOW_FEED_MIN_W;

    if (d->run_last_ms < 0 || feed != d->rec.last_feed || r->now_ms - d->run_last_ms >= HIFLOW_STANDBY_ENTER_MS)
        d->run_since_ms = r->now_ms;
    d->run_last_ms = r->now_ms;
    d->rec.last_feed = (int8_t) feed;
    if (!feed) {
        d->feed_streak = 0;
    } else if (d->feed_streak < 2 && ++d->feed_streak >= 2) {
        /* The inverter takes seconds to stop after the off command, and a
           reading in flight can still show power: two in a row it is on. */
        d->fed_since_off = 1;
    }
}

static void record(hiflow_daylog_t *d, const hiflow_reading_t *r)
{
    int i, minute = -1;
    uint16_t w;

    /* The counters as the inverter reports them; within a day a day counter
       never goes back, so a lower value is a bad one. */
    if (r->energy_total_wh > 0.0f) {
        d->rec.energy_total_wh = take_total(d->rec.energy_total_wh, r->energy_total_wh, &d->total_back);
        if (r->energy_daily_wh >= 0.0f)
            d->rec.energy_daily_wh = at_least(d->rec.energy_daily_wh, r->energy_daily_wh);
    }
    for (i = 0; i < HIFLOW_DAYLOG_PORTS; i++) {
        if (!(r->port_total_wh[i] > 0.0f) || !(r->port_daily_wh[i] >= 0.0f))
            continue;
        d->port_total_wh[i] = take_total(d->port_total_wh[i], r->port_total_wh[i], &d->port_back[i]);
        d->rec.port_daily_wh[i] = at_least(d->rec.port_daily_wh[i], r->port_daily_wh[i]);
    }

    /* The curve and the time of the peak only with a clock to trust: better
       none than one at the wrong hours. The first such reading dates the day;
       a day without power so far takes the date of now (after a day without
       any energy the counters cannot tell the next one, and it has nothing to
       lose). */
    if (r->clock_ok && r->local_time > 0) {
        if (d->rec.day < 0 || d->rec.peak_w < HIFLOW_FEED_MIN_W)
            d->rec.day = hiflow_local_day(r->local_time);
        minute = hiflow_local_minute(r->local_time);
        w = (uint16_t) (r->ac_w > 65000.0f ? 65000.0f : r->ac_w + 0.5f);
        if (w > d->rec.curve[minute / 5])
            d->rec.curve[minute / 5] = w;
    }
    if (r->ac_w > d->rec.peak_w) {
        d->rec.peak_w = r->ac_w;
        d->rec.peak_min = (int16_t) minute;
    }
}

void hiflow_daylog_sample(hiflow_daylog_t *d, const hiflow_reading_t *r)
{
    if (d == NULL || r == NULL || !(r->ac_w >= 0.0f))
        return;
    note_run(d, r);
    if (new_day(d, r)) {
        /* A restart shows in two readings in a row; one alone is a bad
           reading, and it stays out of the day. */
        if (!d->restart_pending) {
            d->restart_pending = 1;
            return;
        }
        clear_day(d);
        d->days_begun++;
    }
    d->restart_pending = 0;
    record(d, r);
}

void hiflow_daylog_update(hiflow_daylog_t *d, int64_t now_ms, int facts)
{
    int idle, hold, answers;

    if (d == NULL)
        return;
    if ((facts & HIFLOW_FACT_SWITCH_OFF) && !(d->facts & HIFLOW_FACT_SWITCH_OFF))
        d->fed_since_off = d->feed_streak = 0;
    d->facts = facts;

    if (facts & HIFLOW_FACT_REFUSED) {
        /* Awake, and it turns the bridge away: shown until it has been quiet
           for as long as standby takes. */
        d->refused_ms = now_ms;
        d->rec.standby = 0;
        return;
    }
    if (!d->rec.standby) {
        /* Nothing fed in: the last reading had no power, or it came before
           the boot (what happened since is not known). Never before the first
           reading ever. */
        idle = d->rec.last_feed == 0 || (d->rec.last_feed == 1 && d->run_last_ms < 0);
        /* Held at 0 W by the bridge: no standby while the inverter answers;
           once it is silent too, it went dark all the same. */
        hold = hiflow_daylog_inverter_off(d) || (facts & HIFLOW_FACT_LIMIT_ZERO);
        answers = d->run_last_ms >= 0 && now_ms - d->run_last_ms < HIFLOW_STANDBY_ENTER_MS;
        if (idle && d->run_since_ms >= 0 && now_ms - d->run_since_ms >= HIFLOW_STANDBY_ENTER_MS &&
            !(hold && answers) && (d->refused_ms < 0 || now_ms - d->refused_ms >= HIFLOW_STANDBY_ENTER_MS))
            d->rec.standby = 1;
    } else if (d->rec.last_feed == 1 && d->run_last_ms >= 0 &&
               d->run_last_ms - d->run_since_ms >= HIFLOW_STANDBY_LEAVE_MS) {
        d->rec.standby = 0;
    }
}

int hiflow_daylog_inverter_off(const hiflow_daylog_t *d)
{
    return d != NULL && (d->facts & HIFLOW_FACT_SWITCH_OFF) && !d->fed_since_off;
}

int hiflow_daylog_has_curve(const hiflow_daylog_t *d)
{
    int i;

    for (i = 0; d != NULL && i < HIFLOW_DAYLOG_SLOTS; i++) {
        if (d->rec.curve[i] > 0)
            return 1;
    }
    return 0;
}

int hiflow_daylog_save_due(const hiflow_daylog_t *d, int64_t now_ms, int link_up)
{
    if (d == NULL || memcmp(&d->rec, &d->saved, sizeof(d->rec)) == 0)
        return 0;
    if (d->saved_ms < 0)
        return 1;
    return now_ms - d->saved_ms >= (link_up ? HIFLOW_DAYLOG_SAVE_MS : HIFLOW_DAYLOG_SAVE_IDLE_MS);
}

void hiflow_daylog_mark_saved(hiflow_daylog_t *d, int64_t now_ms, int ok)
{
    if (d == NULL)
        return;
    if (ok)
        d->saved = d->rec;
    d->saved_ms = now_ms;
}

int hiflow_daylog_restore(hiflow_daylog_t *d, const hiflow_dayrec_t *saved, int64_t now_ms)
{
    hiflow_dayrec_t rec;
    int16_t last_w;

    if (d == NULL || saved == NULL || saved->magic != HIFLOW_DAYREC_MAGIC)
        return 0;
    rec = *saved;
    if (rec.version == 3) {
        /* Version 3 kept the last reading in whole W where last_feed and
           standby are now; at 0 W it had gone dark. */
        memcpy(&last_w, &rec.last_feed, sizeof(last_w));
        rec.last_feed = (int8_t) (last_w < 0 ? -1 : last_w >= HIFLOW_FEED_MIN_W ? 1 : 0);
        rec.standby = last_w == 0;
        rec.version = HIFLOW_DAYREC_VERSION;
    }
    if (rec.version != HIFLOW_DAYREC_VERSION || rec.day < -1 || rec.peak_min < -1 ||
        rec.peak_min >= 24 * 60 || rec.last_feed < -1 || rec.last_feed > 1 || rec.standby > 1)
        return 0;
    d->rec = rec;
    d->saved = *saved; /* flash still holds the old form: saved again soon */
    /* The standby runs start over at the boot: without a reading the
       inverter is standby five minutes later, and waking needs readings of
       this boot. */
    d->run_since_ms = now_ms;
    d->run_last_ms = -1;
    return 1;
}
