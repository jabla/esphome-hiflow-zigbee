/*
 * hiflow_daylog — the day's values for an on-board display, whether the
 * inverter is in standby, and when to keep both in flash.
 *
 * Nothing here depends on the clock, the time zone or the place. A PV
 * inverter runs on its panels, so its readings tell when it is dark, and its
 * energy counters tell when a new day begins. The clock only places readings
 * on the day's curve and dates the day, and only once it can be trusted.
 *
 * Standby: the inverter fed in nothing (below HIFLOW_FEED_MIN_W) for
 * HIFLOW_STANDBY_ENTER_MS, with a link or without. It ends once the inverter
 * feeds in again for HIFLOW_STANDBY_LEAVE_MS: readings with power at least
 * that far apart, none without and no silence of HIFLOW_STANDBY_ENTER_MS
 * between them, so a single reading at dawn does not count. No standby, at
 * any hour:
 *   - while the last reading of this boot had power: a link lost then is a
 *     fault, and the display shows it as one (at dusk the 0 W reading comes
 *     first: feeding in takes far more power than the radio);
 *   - before the first reading ever: the bridge is being set up;
 *   - while the bridge holds the inverter at 0 W (switched off, or a 0 %
 *     limit) and it still answers;
 *   - while the inverter answers but refuses the session (PIN or key): it is
 *     awake, and that needs fixing.
 * The state is part of the record, so a reboot keeps it. A reboot forgets
 * whether the inverter was feeding in: without a reading for
 * HIFLOW_STANDBY_ENTER_MS after the boot, it is standby.
 *
 * A new day: the inverter restarts its day counters when it wakes in the
 * morning and keeps its lifetime counters going. Within a day both grow by
 * the same energy (whole Wh, in lockstep), so a day counter that grew by less
 * than its lifetime counter since the last reading has restarted, once two
 * readings in a row show it. That holds over any gap: a night, a reboot, a
 * board that was off for days. It is
 * judged per PV input, so one input keeping pace outvotes a bad value on
 * another; after a reboot, before the inputs' lifetime counters are known,
 * by the sums. After a day without any energy the counters cannot tell the
 * next one; nothing is lost then, and a trusted clock dates the record anew.
 *
 * Flash holds the current record, a few minutes old at most: the caller saves
 * it whenever it changed (hiflow_daylog_save_due) and takes it back after a
 * boot (hiflow_daylog_restore), so a reboot changes nothing the display shows.
 *
 * Like the session, it owns no clock: every call takes the uptime in ms, and
 * a reading also its local time (unix seconds plus the UTC offset) and
 * whether that time can be trusted.
 */
#ifndef HIFLOW_DAYLOG_H
#define HIFLOW_DAYLOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HIFLOW_DAYLOG_SLOTS    288   /* 5-minute slots of a day */
#define HIFLOW_DAYREC_MAGIC    0x4844u /* 'HD' */
#define HIFLOW_DAYREC_VERSION  4u
#define HIFLOW_DAYLOG_PORTS    4     /* PV inputs kept per day */
#define HIFLOW_DAYLOG_SAVE_MS       300000 /* at most one save per 5 minutes with a link, */
#define HIFLOW_DAYLOG_SAVE_IDLE_MS   60000 /* per minute without one (dusk) */
#define HIFLOW_FEED_MIN_W          1.0f    /* a reading below this fed in nothing */
#define HIFLOW_STANDBY_ENTER_MS  300000    /* nothing fed in for 5 minutes: standby */
#define HIFLOW_STANDBY_LEAVE_MS   60000    /* fed in for a minute: awake again */
#define HIFLOW_DAY_SLACK_WH        10.0f   /* how far the two counters may drift apart */

/* What the bridge knows besides the readings, for hiflow_daylog_update. */
#define HIFLOW_FACT_SWITCH_OFF 1 /* the on/off switch was last set to off */
#define HIFLOW_FACT_LIMIT_ZERO 2 /* the inverter reports a power limit of 0 % */
#define HIFLOW_FACT_REFUSED    4 /* the inverter refuses the session (PIN or key) */

/* What is kept in flash. */
typedef struct {
    uint16_t magic;
    uint16_t version;
    int32_t  day;              /* local day number (local time / 86400), -1 unknown */
    float    energy_total_wh;  /* the lifetime counter at the last reading, 0 = none yet */
    float    energy_daily_wh;  /* the day counter at the last reading */
    float    peak_w;           /* the day's highest reading, 0 = none */
    int16_t  peak_min;         /* its minute of the day, -1 unknown */
    int8_t   last_feed;        /* the last reading: 1 with power, 0 without, -1 none yet */
    uint8_t  standby;          /* 1 while the inverter is in standby */
    float    port_daily_wh[HIFLOW_DAYLOG_PORTS]; /* the day counter per PV input */
    uint16_t curve[HIFLOW_DAYLOG_SLOTS]; /* W, the highest reading per slot */
} hiflow_dayrec_t;

/* One complete set of measurements. */
typedef struct {
    int64_t now_ms;            /* uptime */
    int64_t local_time;        /* unix seconds plus the UTC offset */
    int     clock_ok;          /* local_time can be trusted */
    float   ac_w;
    float   energy_total_wh;   /* the lifetime counter; 0 or less: not in this reading */
    float   energy_daily_wh;   /* the day counter; below 0: not in this reading */
    float   port_total_wh[HIFLOW_DAYLOG_PORTS]; /* per PV input; 0 or less: no such input */
    float   port_daily_wh[HIFLOW_DAYLOG_PORTS]; /* per PV input; below 0: no such input */
} hiflow_reading_t;

typedef struct {
    hiflow_dayrec_t rec;
    hiflow_dayrec_t saved;     /* what flash holds: the last record saved or restored */
    int64_t saved_ms;          /* uptime of the last save (or attempt), -1 none */
    int64_t run_since_ms;      /* uptime the current run of rec.last_feed began, -1 none */
    int64_t run_last_ms;       /* uptime of the latest reading, -1 none since boot */
    int64_t refused_ms;        /* uptime the inverter last refused the session, -1 none */
    float   port_total_wh[HIFLOW_DAYLOG_PORTS]; /* per input at the latest reading, 0 unknown */
    uint8_t port_back[HIFLOW_DAYLOG_PORTS];     /* readings in a row with a lifetime counter gone back */
    uint8_t total_back;        /* the same for the sum */
    uint8_t restart_pending;   /* the last reading showed the day counters restarted */
    int     facts;             /* HIFLOW_FACT_* of the last hiflow_daylog_update */
    int     fed_since_off;     /* two readings with power in a row came after the switch went off */
    uint8_t feed_streak;       /* readings with power in a row, 2 at most */
    uint32_t days_begun;       /* new days since boot */
} hiflow_daylog_t;

void hiflow_daylog_init(hiflow_daylog_t *d);

/* One complete set of measurements: starts a new day when the counters say
   so (the first of two readings that do is left out), then records the
   reading. Readings without a valid AC power are ignored. */
void hiflow_daylog_sample(hiflow_daylog_t *d, const hiflow_reading_t *r);

/* Call regularly (every second) with the HIFLOW_FACT_* flags that apply:
   keeps rec.standby. */
void hiflow_daylog_update(hiflow_daylog_t *d, int64_t now_ms, int facts);

/* 1 while the bridge has the inverter switched off and it did not feed in
   since. The inverter cannot be asked, and it switches itself on again when
   it wakes in the morning; its first reading with power ends this. */
int hiflow_daylog_inverter_off(const hiflow_daylog_t *d);

/* 1 when the day's curve has a reading (it needs a trusted clock). */
int hiflow_daylog_has_curve(const hiflow_daylog_t *d);

/* 1 when the record should go to flash now: it differs from what flash holds,
   and the last save is HIFLOW_DAYLOG_SAVE_MS ago (HIFLOW_DAYLOG_SAVE_IDLE_MS
   without a link). The caller writes d->rec and reports the outcome to
   hiflow_daylog_mark_saved; a failed write is tried again after the interval. */
int hiflow_daylog_save_due(const hiflow_daylog_t *d, int64_t now_ms, int link_up);
void hiflow_daylog_mark_saved(hiflow_daylog_t *d, int64_t now_ms, int ok);

/* After a boot: takes the saved record back as it was, whatever its day
   (records of version 3 are converted), so the reboot changes nothing.
   Returns 1 when it was taken, 0 when it is invalid. */
int hiflow_daylog_restore(hiflow_daylog_t *d, const hiflow_dayrec_t *saved, int64_t now_ms);

/* Local day number and minute of the day of a local time. */
int32_t hiflow_local_day(int64_t local_time);
int     hiflow_local_minute(int64_t local_time);

#ifdef __cplusplus
}
#endif

#endif /* HIFLOW_DAYLOG_H */
