/*
 * test_session.c — host test for the session state machine.
 *
 * A small world model plays the transport: it connects when the session allows
 * it, hands frames to fake_inverter.c, delivers the replies as notifications of
 * 20 bytes, and reports link-downs with a BLE reason code. Time is virtual, so
 * a day of operation runs in milliseconds.
 *
 * The rules under test are the ones the reference implementation follows and
 * the old component broke (see docs/session-core-plan.md):
 *   - one connection carries exactly one login
 *   - a failure ends the connection instead of logging in again on the link
 *   - while waiting after a failure no connection is held at all
 *   - the handshake keeps the reference's poll counts and fallbacks
 *   - the power limit is only ever written on request, never twice for the
 *     same value, at most once a minute, and a failure keeps the connection
 *   - an on/off command goes out once per request, with the inverter's serial
 *     number and the current time, and its outcome is always reported
 *   - the readings reach the day log with the clock and the counters (the
 *     day log itself has its own suite, test_daylog.c)
 *
 * Build/run:  make -C test/host test-session
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "RealDataNew.pb.h"
#include "fake_inverter.h"
#include "hiflow_daylog.h"
#include "hiflow_session.h"
#include "pb_encode.h"
#include "vectors_proto.h"

static int g_pass;
static int g_fail;
static int g_verbose;

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

/* ---------- the world around the session ---------- */

#define WORLD_STEP_MS        10
#define WORLD_CONNECT_MS    500 /* how long the transport needs to connect   */
#define WORLD_REPLY_MS       50 /* how long the inverter needs to answer     */
#define WORLD_LINKDOWN_MS    20 /* delay of a disconnect notification        */
#define WORLD_CHUNK         20  /* notification size                         */
#define WORLD_MAX_STATUS    64

typedef struct {
    hiflow_session_t s;
    fake_inverter_t  fi;
    int64_t now;

    int     link_allowed;
    int     link_up;
    int64_t connect_at;    /* -1 = no connect pending */
    int64_t reply_at;      /* -1 = no reply pending   */
    int64_t link_down_at;  /* -1 = none pending       */
    int     link_down_reason;
    uint8_t reply[FAKE_MAX_REPLY];
    size_t  reply_len;
    int     chunk;
    int     reply_ms;      /* how long the inverter needs to answer */
    int     tick_ms;       /* the session is ticked this often (0 = every step) */
    int64_t stall_from;    /* no ticks from here ...                    */
    int64_t stall_until;   /* ... to here (-1 = no stall)               */

    int     connects;
    int64_t connect_at_ms[32];
    int     disconnects;
    int     data_count;
    int64_t data_at_ms[64];
    hiflow_data_t last_data;
    int     enc_rand_saves;
    uint8_t last_saved_key[HIFLOW_ENC_RAND_LEN];
    uint8_t status_seen[WORLD_MAX_STATUS];
    int     status_count;
    int     limit_reports;
    int32_t last_limit;    /* tenths, -1 = never reported */
    int     power_reports;
    int     last_power_on;
    int     last_power_confirmed;
    int     dark;          /* the inverter's radio is off: no connection comes up */
    int     use_daylog;
    hiflow_daylog_t daylog;
} world_t;

static int world_send(void *ctx, const uint8_t *frame, size_t len)
{
    world_t *w = (world_t *) ctx;

    if (!w->link_up)
        return 0;
    if (fake_handle_frame(&w->fi, frame, len)) {
        memcpy(w->reply, w->fi.reply, w->fi.reply_len);
        w->reply_len = w->fi.reply_len;
        w->reply_at = w->now + w->reply_ms;
    }
    if (w->fi.kill_link) {
        w->fi.kill_link = 0;
        w->link_down_at = w->now + WORLD_LINKDOWN_MS;
        w->link_down_reason = HIFLOW_LINK_PEER_CLOSED;
        w->reply_at = -1;
    }
    return 1;
}

static void world_disconnect(void *ctx)
{
    world_t *w = (world_t *) ctx;

    w->disconnects++;
    if (!w->link_up)
        return;
    w->link_up = 0;
    w->reply_at = -1;
    w->link_down_at = w->now + WORLD_LINKDOWN_MS;
    w->link_down_reason = HIFLOW_LINK_LOCAL_CLOSED;
}

static void world_set_link_allowed(void *ctx, int allowed)
{
    world_t *w = (world_t *) ctx;

    w->link_allowed = allowed;
    if (!allowed) {
        w->connect_at = -1;
        return;
    }
    if (!w->link_up)
        w->connect_at = w->now + WORLD_CONNECT_MS;
}

static void world_on_data(void *ctx, const hiflow_data_t *data)
{
    world_t *w = (world_t *) ctx;

    if (w->data_count < (int) (sizeof(w->data_at_ms) / sizeof(w->data_at_ms[0])))
        w->data_at_ms[w->data_count] = w->now;
    w->data_count++;
    w->last_data = *data;
    if (w->use_daylog && data->have_ac) {
        hiflow_reading_t r;
        int i;
        memset(&r, 0, sizeof(r));
        r.now_ms = w->now;
        r.local_time = hiflow_session_local_time(&w->s, w->now);
        r.clock_ok = hiflow_session_clock_synced(&w->s);
        r.ac_w = data->ac_power_w;
        r.energy_total_wh = data->energy_total_wh;
        r.energy_daily_wh = data->energy_daily_wh;
        for (i = 0; i < HIFLOW_DAYLOG_PORTS && i < HIFLOW_MAX_PORTS; i++) {
            r.port_total_wh[i] = data->ports[i].present ? data->ports[i].energy_total_wh : 0.0f;
            r.port_daily_wh[i] = data->ports[i].present ? data->ports[i].energy_daily_wh : -1.0f;
        }
        hiflow_daylog_sample(&w->daylog, &r);
    }
}

static void world_on_enc_rand(void *ctx, const uint8_t key[HIFLOW_ENC_RAND_LEN])
{
    world_t *w = (world_t *) ctx;

    w->enc_rand_saves++;
    memcpy(w->last_saved_key, key, HIFLOW_ENC_RAND_LEN);
}

static void world_on_status(void *ctx, uint8_t status)
{
    world_t *w = (world_t *) ctx;

    if (w->status_count < WORLD_MAX_STATUS)
        w->status_seen[w->status_count++] = status;
}

static void world_on_power_limit(void *ctx, int32_t tenths)
{
    world_t *w = (world_t *) ctx;

    w->limit_reports++;
    w->last_limit = tenths;
}

static void world_on_inverter_power(void *ctx, int on, int confirmed)
{
    world_t *w = (world_t *) ctx;

    w->power_reports++;
    w->last_power_on = on;
    w->last_power_confirmed = confirmed;
}

static void world_log(void *ctx, int level, const char *msg)
{
    world_t *w = (world_t *) ctx;

    if (g_verbose)
        printf("      [%7lld ms] %d %s\n", (long long) w->now, level, msg);
}

static void world_deliver_reply(world_t *w)
{
    uint8_t frame[FAKE_MAX_REPLY];
    size_t len = w->reply_len;
    size_t off;

    /* Deliver from a copy: handing the notification to the session makes it send
       the next request, whose reply lands in w->reply while we are still here. */
    memcpy(frame, w->reply, len);
    w->reply_len = 0;

    for (off = 0; off < len; off += (size_t) w->chunk) {
        size_t n = len - off < (size_t) w->chunk ? len - off : (size_t) w->chunk;
        hiflow_session_rx(&w->s, w->now, frame + off, n);
    }
}

static void world_run(world_t *w, int64_t duration_ms)
{
    int64_t end = w->now + duration_ms;

    while (w->now < end) {
        w->now += WORLD_STEP_MS;

        if (w->connect_at >= 0 && w->now >= w->connect_at && !w->dark) {
            w->connect_at = -1;
            w->link_up = 1;
            if (w->connects < (int) (sizeof(w->connect_at_ms) / sizeof(w->connect_at_ms[0])))
                w->connect_at_ms[w->connects] = w->now;
            w->connects++;
            fake_link_up(&w->fi);
            hiflow_session_link_up(&w->s, w->now);
        }
        if (w->link_down_at >= 0 && w->now >= w->link_down_at) {
            w->link_down_at = -1;
            w->link_up = 0;
            hiflow_session_link_down(&w->s, w->now, w->link_down_reason);
            if (w->link_allowed && w->connect_at < 0)
                w->connect_at = w->now + WORLD_CONNECT_MS;
        }
        if (w->reply_at >= 0 && w->now >= w->reply_at) {
            w->reply_at = -1;
            if (w->link_up)
                world_deliver_reply(w);
        }
        if (w->stall_until >= 0 && w->now >= w->stall_from && w->now < w->stall_until)
            continue;
        if (w->tick_ms == 0 || w->now % w->tick_ms == 370 % w->tick_ms)
            hiflow_session_tick(&w->s, w->now);
    }
}

/* Drops the link from the outside, e.g. a supervision timeout. */
static void world_drop_link(world_t *w, int reason)
{
    if (!w->link_up)
        return;
    w->reply_at = -1;
    w->link_down_at = w->now + WORLD_LINKDOWN_MS;
    w->link_down_reason = reason;
}

static const uint8_t TEST_KEY[HIFLOW_ENC_RAND_LEN] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
};
static const uint8_t ROTATED_KEY[HIFLOW_ENC_RAND_LEN] = {
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf
};
#define TEST_SN     "0000000000AA"
#define TEST_BLE_ID "176354289012345678"
#define TEST_PIN    "4711"

static void world_init_ex(world_t *w, int have_key, const char *pin, int power_limit)
{
    hiflow_session_config_t cfg;
    hiflow_session_ops_t ops;

    memset(w, 0, sizeof(*w));
    w->connect_at = -1;
    w->reply_at = -1;
    w->link_down_at = -1;
    w->chunk = WORLD_CHUNK;
    w->reply_ms = WORLD_REPLY_MS;
    w->stall_from = -1;
    w->stall_until = -1;
    w->now = 1000;
    w->last_limit = -1;

    fake_init(&w->fi, TEST_KEY, TEST_SN, TEST_PIN);
    fake_set_pages(&w->fi, VEC_HEX_PAGE_SINGLE, NULL);

    hiflow_session_config_defaults(&cfg);
    snprintf(cfg.sn, sizeof(cfg.sn), "%s", TEST_SN);
    snprintf(cfg.ble_id, sizeof(cfg.ble_id), "%s", TEST_BLE_ID);
    snprintf(cfg.pin, sizeof(cfg.pin), "%s", pin != NULL ? pin : "");
    if (have_key) {
        memcpy(cfg.enc_rand, TEST_KEY, HIFLOW_ENC_RAND_LEN);
        cfg.have_enc_rand = 1;
    }
    cfg.power_limit = power_limit;

    memset(&ops, 0, sizeof(ops));
    ops.ctx = w;
    ops.send = world_send;
    ops.disconnect = world_disconnect;
    ops.set_link_allowed = world_set_link_allowed;
    ops.on_data = world_on_data;
    ops.on_enc_rand = world_on_enc_rand;
    ops.on_status = world_on_status;
    ops.on_power_limit = world_on_power_limit;
    ops.on_inverter_power = world_on_inverter_power;
    ops.log = world_log;

    hiflow_session_init(&w->s, &cfg, &ops, w->now, 1774000000, 0);
}

static void world_init(world_t *w, int have_key, const char *pin)
{
    world_init_ex(w, have_key, pin, 0);
}

/* With inverter control on (and the power limit off). */
static void world_init_control(world_t *w)
{
    world_init_ex(w, 1, TEST_PIN, 0);
    w->s.cfg.inverter_control = 1;
}

/* A one-page reply (ap = 1) with the AC block and ports 1 and 2. */
static void world_single_page(world_t *w)
{
    fake_set_pages(&w->fi, VEC_HEX_PAGE_SINGLE, NULL);
}

static int status_seen(const world_t *w, uint8_t status)
{
    int i;

    for (i = 0; i < w->status_count; i++) {
        if (w->status_seen[i] == status)
            return 1;
    }
    return 0;
}

/* ---------- scenarios ---------- */

static void test_happy_path(void)
{
    world_t w;

    printf("\n[1] normal run: connect, handshake, data\n");
    world_init(&w, 1, TEST_PIN);
    world_single_page(&w);

    world_run(&w, 5000);

    expect_int(w.connects, 1, "exactly one connection");
    expect_int(w.fi.logins_seen, 1, "exactly one login");
    expect_int(w.fi.max_logins_on_link, 1, "never two logins on one link");
    expect_int(w.fi.time_syncs, 1, "one time-sync");
    expect_int(w.fi.pin_frames, 0, "no PIN needed");
    expect_int(w.fi.v0_requests, 0, "no V0 pairing needed");
    expect_int(w.data_count, 1, "one set of measurements");
    expect_int(hiflow_session_state(&w.s), HIFLOW_STATE_READY, "session is ready");
    expect_int(hiflow_session_status(&w.s), HIFLOW_STATUS_READY, "status 6");
    expect_int((long) hiflow_session_sessions(&w.s), 1, "session counter");
    expect_int((long) hiflow_session_failures(&w.s), 0, "no failures");
    expect(!status_seen(&w, 9) && !status_seen(&w, 10) && !status_seen(&w, 11),
           "no failure status reported");
    expect(status_seen(&w, HIFLOW_STATUS_LOGIN), "status showed the login");
    expect(status_seen(&w, HIFLOW_STATUS_TIME_SYNC), "status showed the time-sync");

    expect_float(w.last_data.ac_power_w, VEC_MERGED_AC_POWER_W, "ac power");
    expect_float(w.last_data.ac_voltage_v, VEC_MERGED_AC_VOLTAGE_V, "ac voltage");
    expect_int(w.last_data.port_count, 2, "two ports on the single page");

    /* The login carries the configured identity and a plausible timestamp. */
    expect(strcmp(w.fi.last_ble_id, TEST_BLE_ID) == 0, "login carries the configured bleId");
    expect(w.fi.last_login_time > 1704067200, "login timestamp is plausible");
    expect(strlen(w.fi.last_time_sync) > 10, "time-sync carries a timestamp and offset");
}

static void test_poll_cadence(void)
{
    world_t w;

    printf("[2] data cadence on one connection\n");
    world_init(&w, 1, TEST_PIN);
    world_single_page(&w);

    world_run(&w, 95000); /* first round plus three polls */

    expect_int(w.data_count, 4, "four rounds in 95 s");
    expect_int(w.fi.logins_seen, 1, "still one login");
    expect_int(w.connects, 1, "still one connection");
    expect_int(w.disconnects, 0, "no disconnect");
    expect_int(hiflow_session_state(&w.s), HIFLOW_STATE_READY, "still ready");
    expect_int((long) hiflow_session_failures(&w.s), 0, "no failures");
}

static void test_poll_cadence_slow_link(void)
{
    world_t w;
    int i;

    printf("[2b] data cadence on a slow link\n");
    world_init(&w, 1, TEST_PIN);
    fake_set_pages(&w.fi, VEC_HEX_PAGE0, VEC_HEX_PAGE1);
    /* A 1 s connection interval: every reply takes a second, so a two-page
       round takes about 2 s. The rounds still start 30 s apart. */
    w.reply_ms = 1000;

    world_run(&w, 10 * 60000);

    expect(w.data_count >= 20, "twenty rounds in ten minutes");
    for (i = 1; i < w.data_count && i < 20; i++)
        expect(w.data_at_ms[i] - w.data_at_ms[i - 1] == 30000, "rounds 30 s apart");
    expect_int(w.connects, 1, "one connection");
    expect_int((long) hiflow_session_failures(&w.s), 0, "no failures");
}

static void test_poll_cadence_coarse_tick(void)
{
    world_t w;
    int n;

    printf("[2c] data cadence with a coarse tick\n");
    world_init(&w, 1, TEST_PIN);
    fake_set_pages(&w.fi, VEC_HEX_PAGE0, VEC_HEX_PAGE1);
    /* A sleepy bridge runs its main loop, and so the session's tick, about
       once a second, not in step with the 30 s cadence (1010 ms here): a due
       round starts up to a second late. The cadence must not drift by that. */
    w.reply_ms = 1000;
    w.tick_ms = 1010;

    world_run(&w, 30 * 60000);

    n = w.data_count < 64 ? w.data_count : 64;
    expect(w.data_count >= 59 && w.data_count <= 61, "one round per 30 s for thirty minutes");
    if (n >= 3)
        expect(w.data_at_ms[n - 1] - w.data_at_ms[1] <= (int64_t) (n - 2) * 30000 + 1000,
               "no drift over thirty minutes");
    expect_int((long) hiflow_session_failures(&w.s), 0, "no failures");
}

static void test_poll_cadence_stall(void)
{
    world_t w;
    int64_t stall, max_gap = 0;
    int i, short_gaps = 0;

    printf("[2d] data cadence after a stalled tick\n");
    /* The main loop can stall (a flash write, a long BLE call). Whatever the
       stall, the round after it must not follow the late one at once. Stalls
       from 20 s to 75 s after a round, so the due round starts up to 47 s
       late. A round up to 15 s late keeps the 30 s grid, so the next one
       may come 15 s after it; a gap of a few seconds is a repeated round. */
    for (stall = 20000; stall <= 75000; stall += 1000) {
        world_init(&w, 1, TEST_PIN);
        fake_set_pages(&w.fi, VEC_HEX_PAGE0, VEC_HEX_PAGE1);
        w.reply_ms = 1000;
        w.tick_ms = 1010;
        while (w.data_count < 3 && w.now < 5 * 60000)
            world_run(&w, WORLD_STEP_MS);
        w.stall_from = w.now + 1000;
        w.stall_until = w.now + stall;
        world_run(&w, 5 * 60000);
        for (i = 3; i < w.data_count && i < 64; i++) {
            const int64_t gap = w.data_at_ms[i] - w.data_at_ms[i - 1];
            if (gap < 12000)
                short_gaps++;
            if (gap > max_gap)
                max_gap = gap;
        }
    }
    expect_int(short_gaps, 0, "no round right behind another");
    expect(max_gap <= 75000 + 31000, "the cadence comes back after the stall");
}

static void test_paging(void)
{
    world_t w;

    printf("[3] two-page reply\n");
    world_init(&w, 1, TEST_PIN);
    fake_set_pages(&w.fi, VEC_HEX_PAGE0, VEC_HEX_PAGE1);

    world_run(&w, 5000);

    expect_int(w.data_count, 1, "one set of measurements for both pages");
    expect_int(w.fi.data_requests, 2, "both pages requested");
    expect_int(w.last_data.port_count, 4, "all four ports");
    expect_float(w.last_data.energy_total_wh, VEC_MERGED_ENERGY_TOTAL_WH, "energy total");
    expect_float(w.last_data.energy_daily_wh, VEC_MERGED_ENERGY_DAILY_WH, "energy daily");
    expect_float(w.last_data.ports[2].power_w, VEC_MERGED_PORTS[2].power_w, "port 3 power");
    expect_float(w.last_data.ports[3].power_w, VEC_MERGED_PORTS[3].power_w, "port 4 power");
    expect_int(w.last_data.ports[0].port_number, 1, "port 1 in slot 1");
    expect_int(w.last_data.ports[3].port_number, 4, "port 4 in slot 4");
}

static void test_pin_path(void)
{
    world_t w;

    printf("[4] the device asks for the PIN\n");
    world_init(&w, 1, TEST_PIN);
    world_single_page(&w);
    w.fi.require_pin = 1;

    world_run(&w, 8000);

    expect_int(w.fi.pin_frames, 1, "PIN sent once");
    expect(strcmp(w.fi.last_pin, TEST_PIN) == 0, "the configured PIN was sent");
    expect_int(w.fi.pin_ok, 1, "device whitelisted the identity");
    expect_int(w.fi.time_syncs, 1, "handshake continued to the time-sync");
    expect_int(w.data_count, 1, "data arrived");
    expect(status_seen(&w, HIFLOW_STATUS_PIN), "status showed the PIN step");
    expect_int(w.fi.logins_seen, 1, "still exactly one login");
}

static void test_login_in_progress(void)
{
    world_t w;

    printf("[5] login poll answers 'in progress' first\n");
    world_init(&w, 1, TEST_PIN);
    world_single_page(&w);
    w.fi.login_in_progress = 2;

    world_run(&w, 10000);

    expect(w.fi.login_polls >= 3, "polled until the login settled");
    expect_int(w.data_count, 1, "data arrived");
    expect_int(w.fi.logins_seen, 1, "one login");
    expect_int((long) hiflow_session_failures(&w.s), 0, "no failure booked");
}

static void test_login_never_confirmed(void)
{
    world_t w;

    printf("[6] login never confirmed: carry on like the reference\n");
    world_init(&w, 1, TEST_PIN);
    world_single_page(&w);
    w.fi.login_sts = 2; /* neither 1 (ok) nor 3 (PIN) */

    world_run(&w, 10000);

    expect_int(w.fi.time_syncs, 1, "time-sync went out anyway");
    expect_int(w.data_count, 1, "data still arrived");
    expect_int(w.fi.logins_seen, 1, "no second login attempt");
}

static void test_tid_range(void)
{
    world_t w;

    printf("[7] transaction id stays in 15 bits\n");
    world_init(&w, 1, TEST_PIN);
    world_single_page(&w);

    world_run(&w, 3000);
    w.s.tid = 0x7FFE; /* just below the wrap the reference uses */
    world_run(&w, 95000);

    expect(w.fi.tid_max <= 0x7FFF, "no transaction id above 0x7FFF");
    expect_int(w.data_count, 4, "data kept flowing across the wrap");
}

/* ---------- failure paths ---------- */

static void test_link_killed_at_login(void)
{
    world_t w;

    printf("[8] the inverter kills the link on the login frame\n");
    world_init(&w, 1, TEST_PIN);
    world_single_page(&w);
    w.fi.kill_link_on_login = 1;

    world_run(&w, 20000); /* first refusal plus part of the waiting period */

    expect_int(w.fi.max_logins_on_link, 1, "still only one login per connection");
    expect_int(hiflow_session_state(&w.s), HIFLOW_STATE_BACKOFF, "waiting after the failure");
    expect_int(hiflow_session_status(&w.s), 9, "status 9: login rejected");
    expect_int(w.link_allowed, 0, "no connection is held while waiting");
    expect_int((long) hiflow_session_failures(&w.s), 1, "one failure booked");
    expect_int(w.data_count, 0, "no data");

    /* The next connection starts with the V0 pairing, like the reference does
       after a failed handshake. */
    world_run(&w, 60000);
    expect(w.fi.v0_requests >= 1, "V0 pairing on the next connection");
    expect(w.connects >= 2, "reconnected after the waiting period");
    expect_int(w.fi.max_logins_on_link, 1, "never two logins on one link");
    /* The V0 pairing handed back the key we already had, so the next refusal is
       reported as a key problem. */
    expect_int(hiflow_session_status(&w.s), 14, "status 14: the key is stale");
}

static void test_rotated_key_recovery(void)
{
    world_t w;

    printf("[9] the inverter rotated its key\n");
    world_init(&w, 1, TEST_PIN);
    world_single_page(&w);
    fake_rotate_key(&w.fi, ROTATED_KEY, 1);

    /* First connection: the login cannot be decrypted, the device kills the link. */
    world_run(&w, 20000);
    expect(w.fi.bad_key_frames >= 1, "the device could not read our frame");
    expect_int(w.data_count, 0, "no data with the stale key");

    /* Next connection: V0 pairing hands out the new key, the session stores it
       and logs in with it. */
    world_run(&w, 60000);
    expect_int(w.enc_rand_saves, 1, "the fresh key was handed to the caller once");
    expect(memcmp(w.last_saved_key, ROTATED_KEY, HIFLOW_ENC_RAND_LEN) == 0,
           "the stored key is the rotated one");
    expect(w.data_count >= 1, "data flows again after the re-pairing");
    expect_int(w.fi.max_logins_on_link, 1, "one login per connection throughout");
    expect_int(hiflow_session_status(&w.s), HIFLOW_STATUS_READY, "back to ready");
}

static void test_reply_timeout(void)
{
    world_t w;
    int connects_before;

    printf("[10] a data request goes unanswered\n");
    world_init(&w, 1, TEST_PIN);
    world_single_page(&w);

    world_run(&w, 5000);
    expect_int(w.data_count, 1, "first round arrived");
    connects_before = w.connects;

    w.fi.silent_on_data = 1;
    world_run(&w, 50000); /* poll + 15 s timeout + backoff */

    expect_int(hiflow_session_status(&w.s), 11, "status 11: no reply");
    expect(w.disconnects >= 1, "the connection was closed");
    expect_int(w.fi.max_logins_on_link, 1, "no second login on the same link");

    /* Once the device answers again the bridge comes back by itself. */
    w.fi.silent_on_data = 0;
    world_run(&w, 60000);
    expect(w.connects > connects_before, "reconnected");
    expect(w.data_count >= 2, "data flows again");
    expect_int(hiflow_session_status(&w.s), HIFLOW_STATUS_READY, "ready again");
}

static void test_wrong_pin(void)
{
    world_t w;

    printf("[11] the device refuses the PIN\n");
    world_init(&w, 1, TEST_PIN);
    world_single_page(&w);
    w.fi.require_pin = 1;
    w.fi.refuse_pin = 1;

    world_run(&w, 60000);

    expect_int(hiflow_session_status(&w.s), 13, "status 13: PIN problem");
    expect_int(w.fi.pin_frames, 1, "the PIN was sent once, not in a loop");
    expect_int(w.link_allowed, 0, "no connection held");

    /* The PIN backoff is half an hour, so nothing happens for a long while. */
    world_run(&w, 600000);
    expect_int(w.fi.pin_frames, 1, "still only one PIN attempt after 10 minutes");
    expect_int(hiflow_session_status(&w.s), 13, "still reporting the PIN problem");
}

static void test_missing_pin(void)
{
    world_t w;

    printf("[12] the device asks for a PIN we do not have\n");
    world_init(&w, 1, ""); /* no PIN configured */
    world_single_page(&w);
    w.fi.require_pin = 1;

    world_run(&w, 30000);

    expect_int(hiflow_session_status(&w.s), 13, "status 13: PIN missing");
    expect_int(w.fi.pin_frames, 0, "nothing was sent as a PIN");
    expect_int(w.data_count, 0, "no data");
}

static void test_radio_loss(void)
{
    world_t w;

    printf("[13] the radio link times out during the handshake\n");
    world_init(&w, 1, TEST_PIN);
    world_single_page(&w);
    w.fi.silent_on_login = 1; /* keeps the session in the handshake */

    world_run(&w, 2000);
    world_drop_link(&w, HIFLOW_LINK_RADIO_TIMEOUT);
    world_run(&w, 1000);

    expect_int(hiflow_session_status(&w.s), 15, "status 15: radio loss");
    expect_int((long) hiflow_session_failures(&w.s), 0,
               "a radio drop is not booked as a login failure");
}

static void test_session_ends_after_data(void)
{
    world_t w;

    printf("[14] the inverter goes to sleep after delivering data\n");
    world_init(&w, 1, TEST_PIN);
    world_single_page(&w);

    world_run(&w, 5000);
    expect_int(w.data_count, 1, "data arrived");

    world_drop_link(&w, HIFLOW_LINK_PEER_CLOSED);
    world_run(&w, 1000);
    expect_int((long) hiflow_session_failures(&w.s), 0, "no failure booked");
    expect_int(hiflow_session_status(&w.s), HIFLOW_STATUS_WAIT_LINK, "status back to waiting");

    /* It comes back on its own. */
    world_run(&w, 60000);
    expect(w.connects >= 2, "reconnected");
    expect(w.data_count >= 2, "data flows again");
}

static void test_backoff_growth(void)
{
    world_t w;
    int i;
    int64_t gap[6];

    printf("[15] the waiting period grows and is capped\n");
    world_init(&w, 1, TEST_PIN);
    world_single_page(&w);
    w.fi.kill_link_on_login = 1;

    world_run(&w, 1500000); /* 25 minutes */

    expect(w.connects >= 6, "kept retrying");
    expect(w.connects <= 9, "but did not hammer the inverter");
    for (i = 0; i < 6 && i + 1 < w.connects; i++)
        gap[i] = w.connect_at_ms[i + 1] - w.connect_at_ms[i];
    expect(gap[0] >= 30000 && gap[0] < 40000, "first wait about 30 s");
    expect(gap[1] >= 60000 && gap[1] < 70000, "then about 60 s");
    expect(gap[2] >= 120000 && gap[2] < 130000, "then about 120 s");
    expect(gap[4] >= 300000 && gap[4] < 320000, "capped at 5 minutes");
}

static void test_stray_frames(void)
{
    world_t w;
    uint8_t frame[HIFLOW_MAX_FRAME_LEN];
    uint8_t payload[8] = {0x18, 0x40, 0x58, 0x01};
    size_t frame_len = 0;
    uint8_t other_key[HIFLOW_ENC_RAND_LEN];

    printf("[16] stray notifications and frames\n");
    world_init(&w, 1, TEST_PIN);
    world_single_page(&w);
    world_run(&w, 5000);
    expect_int(w.data_count, 1, "running");

    /* A notification that is not the start of a frame. */
    hiflow_session_rx(&w.s, w.now, (const uint8_t *) "noise", 5);
    world_run(&w, 100);
    expect_int(hiflow_session_state(&w.s), HIFLOW_STATE_READY, "stray notification ignored");

    /* A well-formed frame the session did not ask for. */
    hiflow_build_frame_v1(TEST_KEY, 0xA219, 7, payload, 4, frame, sizeof(frame), &frame_len);
    hiflow_session_rx(&w.s, w.now, frame, frame_len);
    world_run(&w, 100);
    expect_int(hiflow_session_state(&w.s), HIFLOW_STATE_READY, "unexpected reply ignored");
    expect_int((long) hiflow_session_failures(&w.s), 0, "and not booked as a failure");

    /* A frame encrypted with a different key: that is what a rotated encRand
       looks like, so the session gives up the connection and re-pairs. */
    memcpy(other_key, TEST_KEY, sizeof(other_key));
    other_key[0] ^= 0xFF;
    hiflow_build_frame_v1(other_key, 0xA211, 8, payload, 4, frame, sizeof(frame), &frame_len);
    hiflow_session_rx(&w.s, w.now, frame, frame_len);
    world_run(&w, 100);
    expect_int(hiflow_session_status(&w.s), 14, "status 14 after a frame that does not authenticate");
}

static void test_long_uptime(void)
{
    world_t w;

    printf("[17] uptime beyond the 32-bit millisecond range\n");
    world_init(&w, 1, TEST_PIN);
    world_single_page(&w);
    /* Around 60 days of uptime, where millis() would have wrapped twice. */
    {
        hiflow_session_config_t cfg = w.s.cfg;
        hiflow_session_ops_t ops = w.s.ops;

        w.now = 5200000000LL;
        hiflow_session_init(&w.s, &cfg, &ops, w.now, 1774000000, 0);
    }

    world_run(&w, 95000);

    expect(w.data_count >= 4, "data keeps flowing at a huge uptime");
    expect_int(w.fi.max_logins_on_link, 1, "still one login per connection");
    expect(w.fi.last_login_time > 1704067200, "login timestamp still plausible");
}

static void test_start_without_key(void)
{
    world_t w;

    printf("[18] no key configured: V0 first, then the login on the same link\n");
    /* The bridge keeps no key across reboots: every boot starts like this. */
    world_init(&w, 0, TEST_PIN);
    world_single_page(&w);

    world_run(&w, 10000);
    expect_int(w.connects, 1, "a single connection");
    expect_int(w.fi.v0_requests, 1, "one V0 pairing");
    expect_int(w.fi.max_logins_on_link, 1, "followed by exactly one login");
    expect_int(w.fi.bad_key_frames, 0, "no frame with a missing key");
    expect_int(w.enc_rand_saves, 1, "the key was handed over once");
    expect(w.data_count >= 1, "data flows");
    expect_int(hiflow_session_status(&w.s), HIFLOW_STATUS_READY, "ready");
}

static void test_clock_behind_after_reboot(void)
{
    world_t w;

    printf("[19] after a reboot the clock lags the inverter: V0 carries its time\n");
    /* After a power cut in the field the clock restarted from the value saved to flash,
       ~90 s behind the inverter, and every login was refused. */
    world_init(&w, 0, TEST_PIN);
    world_single_page(&w);
    w.fi.device_time += 300;
    w.fi.max_login_lag = 60;

    world_run(&w, 10000);
    expect_int(w.connects, 1, "a single connection");
    expect_int(w.fi.max_logins_on_link, 1, "one login");
    expect(w.fi.last_login_time >= w.fi.device_time, "the login carries the device's time");
    expect(w.data_count >= 1, "data flows");
    expect_int(hiflow_session_status(&w.s), HIFLOW_STATUS_READY, "ready");

    printf("[20] the same without a timestamp in the V0 reply stays refused\n");
    world_init(&w, 0, TEST_PIN);
    world_single_page(&w);
    w.fi.device_time += 300;
    w.fi.max_login_lag = 60;
    w.fi.v0_without_time = 1;

    world_run(&w, 60000);
    expect(w.fi.logins_seen >= 2, "retried");
    expect_int(w.data_count, 0, "no data with a lagging clock");
    expect_int(w.fi.max_logins_on_link, 1, "still one login per connection");
}

/* Seen in the field: the grid block and both port blocks present, every value 0. */
static const char HEX_PAGE_EMPTY[] = "0a0c5445535444545530303030311801"
                                     "4a00"
                                     "5a021001"
                                     "5a021002";

static void test_empty_data_reply(void)
{
    world_t w;

    printf("[21] an all-zero data reply is dropped, the session carries on\n");
    world_init(&w, 1, TEST_PIN);
    fake_set_pages(&w.fi, HEX_PAGE_EMPTY, NULL);

    world_run(&w, 5000);
    expect_int(w.fi.data_requests, 1, "the data was requested");
    expect_int(w.data_count, 0, "the empty reply is not passed on");
    expect_int(hiflow_session_status(&w.s), HIFLOW_STATUS_READY, "still ready");
    expect_int((long) hiflow_session_failures(&w.s), 0, "not counted as a failure");

    world_single_page(&w);
    world_run(&w, 30000);
    expect_int(w.data_count, 1, "the next poll delivers real data");
    expect_int(w.connects, 1, "on the same connection");
    expect_int(w.fi.logins_seen, 1, "without a new login");
}

/* ---------- power limit ---------- */

static void test_limit_read(void)
{
    world_t w;

    printf("[22] power limit: read once per connection and hourly, never written unasked\n");
    world_init(&w, 1, TEST_PIN);
    world_run(&w, 70000);
    expect_int(w.fi.config_reads, 0, "disabled: no config read");
    expect_int(w.limit_reports, 0, "disabled: nothing reported");
    expect_int(hiflow_session_request_power_limit(&w.s, w.now, 90.0f), -1,
               "disabled: a request is refused");
    world_run(&w, 70000);
    expect_int(w.fi.limit_writes, 0, "disabled: no write");

    world_init_ex(&w, 1, TEST_PIN, 1);
    world_run(&w, 5000);
    expect_int(w.data_count, 1, "data first");
    expect_int(w.fi.config_reads, 1, "then one config read");
    expect_int(w.last_limit, 1000, "the device's 100 % is reported");
    expect_int(hiflow_session_status(&w.s), HIFLOW_STATUS_READY, "back to ready");

    world_run(&w, 3600000);
    expect_int(w.fi.config_reads, 2, "read again after an hour");
    expect(w.fi.data_requests >= 119 && w.fi.data_requests <= 121,
           "the 30 s data cadence is unchanged");
    expect_int(w.connects, 1, "one connection");

    /* The app changes the limit; the next hourly read picks it up. */
    w.fi.limit_tenths = 700;
    world_run(&w, 3600000);
    expect_int(w.last_limit, 700, "a change made elsewhere is picked up");

    world_drop_link(&w, HIFLOW_LINK_RADIO_TIMEOUT);
    world_run(&w, 60000);
    expect_int(w.connects, 2, "reconnected");
    expect_int(w.fi.config_reads, 4, "read again on the new connection");
    expect_int(w.fi.limit_writes, 0, "never written without a request");
}

static void test_limit_write(void)
{
    world_t w;

    printf("[23] power limit: one write, read back, no write on reconnect\n");
    world_init_ex(&w, 1, TEST_PIN, 1);
    world_run(&w, 5000);

    expect_int(hiflow_session_request_power_limit(&w.s, w.now, 90.0f), 90, "90 % accepted");
    world_run(&w, 5000);
    expect_int(w.fi.limit_writes, 1, "one write");
    expect_int(w.fi.limit_tenths, 900, "the device took 90 %");
    expect_int(w.fi.config_reads, 2, "read back after the write");
    expect_int(w.last_limit, 900, "the read-back value is reported");
    expect_int(w.connects, 1, "on the same connection");

    world_run(&w, 60000);
    expect_int(w.data_count >= 3, 1, "data keeps flowing");

    world_drop_link(&w, HIFLOW_LINK_PEER_CLOSED);
    world_run(&w, 60000);
    expect_int(w.connects, 2, "reconnected");
    expect_int(w.fi.limit_writes, 1, "no write on the reconnect");
    expect_int(w.last_limit, 900, "still 90 %");
}

static void test_limit_dedup_and_rounding(void)
{
    world_t w;
    int reports;

    printf("[24] power limit: the value already set is not written, off-grid values round\n");
    world_init_ex(&w, 1, TEST_PIN, 1);
    world_run(&w, 5000);

    reports = w.limit_reports;
    expect_int(hiflow_session_request_power_limit(&w.s, w.now, 100.0f), 100, "100 % accepted");
    world_run(&w, 2000);
    expect_int(w.fi.limit_writes, 0, "100 % is already set: no write");
    expect_int(w.limit_reports, reports + 1, "but the value is reported again");
    expect_int(w.last_limit, 1000, "as 100 %");

    expect_int(hiflow_session_request_power_limit(&w.s, w.now, 96.0f), 100, "96 rounds to 100");
    world_run(&w, 2000);
    expect_int(w.fi.limit_writes, 0, "and needs no write either");

    expect_int(hiflow_session_request_power_limit(&w.s, w.now, 87.0f), 90, "87 rounds to 90");
    world_run(&w, 5000);
    expect_int(w.fi.limit_writes, 1, "one write");
    expect_int(w.fi.limit_tenths, 900, "of 90 %");
    expect_int(w.last_limit, 900, "reported as 90 %");
}

static void test_limit_rate(void)
{
    world_t w;

    printf("[25] power limit: at most one write a minute, the newest request wins\n");
    world_init_ex(&w, 1, TEST_PIN, 1);
    world_run(&w, 5000);

    hiflow_session_request_power_limit(&w.s, w.now, 90.0f);
    world_run(&w, 5000);
    expect_int(w.fi.limit_writes, 1, "first write");

    hiflow_session_request_power_limit(&w.s, w.now, 70.0f);
    world_run(&w, 1000);
    hiflow_session_request_power_limit(&w.s, w.now, 60.0f);
    world_run(&w, 1000);
    hiflow_session_request_power_limit(&w.s, w.now, 50.0f);
    world_run(&w, 40000);
    expect_int(w.fi.limit_writes, 1, "nothing within the minute");
    expect_int(w.last_limit, 900, "the slider still shows the confirmed 90 %");

    world_run(&w, 20000);
    expect_int(w.fi.limit_writes, 2, "one write after the minute");
    expect_int(w.fi.limit_tenths, 500, "with the newest value");
    expect_int(w.last_limit, 500, "read back as 50 %");

    world_run(&w, 300000);
    expect_int(w.fi.limit_writes, 2, "and nothing after that");
}

static void test_limit_failures(void)
{
    world_t w;

    printf("[26] power limit: failures keep the connection and the confirmed value\n");
    world_init_ex(&w, 1, TEST_PIN, 1);
    world_run(&w, 5000);

    /* The command goes unanswered. */
    w.fi.silent_on_limit = 1;
    hiflow_session_request_power_limit(&w.s, w.now, 80.0f);
    world_run(&w, 25000);
    expect_int(w.fi.limit_writes, 1, "one write, no retry");
    expect_int(w.fi.config_reads, 2, "read back anyway");
    expect_int(w.last_limit, 1000, "the unchanged 100 % is reported");
    expect_int(w.connects, 1, "the connection stays");
    expect_int((long) hiflow_session_failures(&w.s), 0, "not a session failure");
    world_run(&w, 60000);
    expect_int(w.fi.limit_writes, 1, "still no retry");

    /* The device refuses the value. */
    w.fi.silent_on_limit = 0;
    w.fi.limit_err_code = 3;
    hiflow_session_request_power_limit(&w.s, w.now, 80.0f);
    world_run(&w, 5000);
    expect_int(w.fi.limit_writes, 2, "second write");
    expect_int(w.last_limit, 1000, "refused: 100 % is reported");

    /* It acknowledges but keeps the old value: the read-back tells. */
    w.fi.limit_err_code = 0;
    w.fi.limit_ignored = 1;
    world_run(&w, 60000);
    hiflow_session_request_power_limit(&w.s, w.now, 80.0f);
    world_run(&w, 5000);
    expect_int(w.fi.limit_writes, 3, "third write");
    expect_int(w.last_limit, 1000, "ignored: 100 % is reported");

    /* The config read goes unanswered: the link stays, data keeps flowing. */
    world_init_ex(&w, 1, TEST_PIN, 1);
    w.fi.silent_on_config = 1;
    world_run(&w, 60000);
    expect_int(w.fi.config_reads, 1, "one config read");
    expect_int(w.limit_reports, 0, "nothing to report");
    expect_int(w.connects, 1, "the connection stays");
    expect(w.data_count >= 2, "data keeps flowing");
    hiflow_session_request_power_limit(&w.s, w.now, 50.0f);
    world_run(&w, 300000);
    expect_int(w.fi.limit_writes, 0, "no write while the current value is unknown");
    expect_int(w.fi.config_reads, 2, "the read is retried after five minutes");
    w.fi.silent_on_config = 0;
    world_run(&w, 300000);
    expect_int(w.last_limit, 1000, "known again");
    expect_int(w.fi.limit_writes, 0, "and the stale request is not written");
}

static void test_limit_offline(void)
{
    world_t w;
    int reports;

    printf("[27] power limit: a request while offline is dropped after two minutes\n");
    world_init_ex(&w, 1, TEST_PIN, 1);
    world_run(&w, 5000);

    /* Dusk: the link goes and the inverter does not answer any more. The
       request is dropped after two minutes without any link, and the slider
       goes back to the limit the device holds. */
    w.fi.kill_link_on_login = 1;
    world_drop_link(&w, HIFLOW_LINK_PEER_CLOSED);
    world_run(&w, 1000);
    hiflow_session_request_power_limit(&w.s, w.now, 60.0f);
    reports = w.limit_reports;
    world_run(&w, 119000);
    expect_int(w.limit_reports, reports, "offline, 1:59 later: still waiting");
    world_run(&w, 2000);
    expect_int(w.limit_reports, reports + 1, "2 minutes: dropped, without a link");
    expect_int(w.last_limit, 1000, "the slider goes back to 100 %");
    world_run(&w, 600000 - 121000);
    w.fi.kill_link_on_login = 0;
    world_run(&w, 600000);
    expect(w.fi.logins_seen >= 3, "logged in again later");
    expect_int(w.fi.limit_writes, 0, "the request from the evening is not written");
    expect_int(w.last_limit, 1000, "the slider shows 100 %");

    /* A request that arrives during a data read waits for it. */
    hiflow_session_request_power_limit(&w.s, w.now, 0.0f);
    world_run(&w, 5000);
    expect_int(w.fi.limit_writes, 1, "0 % is written");
    expect_int(w.fi.limit_tenths, 0, "the output is off");
    expect_int(w.last_limit, 0, "0 % is read back although it is not on the wire");
}

static void test_limit_link_killed(void)
{
    world_t w;
    int reports;

    printf("[28] power limit: the inverter drops the link on the config read\n");
    world_init_ex(&w, 1, TEST_PIN, 1);
    w.fi.kill_link_on_config = 1;
    world_run(&w, 600000);
    expect_int(w.fi.config_reads, 1, "one config read");
    expect_int(w.connects, 2, "one reconnect, no loop");
    expect_int(w.fi.logins_seen, 2, "two logins in ten minutes");
    expect(w.data_count >= 18, "data keeps flowing");
    expect_int((long) hiflow_session_failures(&w.s), 0, "not a handshake failure");
    reports = w.limit_reports;
    hiflow_session_request_power_limit(&w.s, w.now, 40.0f);
    world_run(&w, 600000);
    expect_int(w.fi.limit_writes, 0, "no write while the limit is unknown");
    expect_int(w.limit_reports, reports + 1, "the request was dropped");
    expect_int(w.last_limit, -1, "before any read: the slider goes back to unknown");

    w.fi.kill_link_on_config = 0;
    world_run(&w, 3000000);
    expect_int(w.fi.config_reads, 2, "read again after the hour");
    expect_int(w.last_limit, 1000, "and known");
    expect_int(w.connects, 2, "on the same connection");
}

/* ---------- inverter on/off ---------- */

static void test_power_switch(void)
{
    world_t w;

    printf("[29] inverter on/off: one command per request, serial number and time on it\n");
    world_init(&w, 1, TEST_PIN);
    expect_int(hiflow_session_request_inverter_power(&w.s, w.now, 0), -1, "disabled: refused");
    world_run(&w, 60000);
    expect_int(w.fi.power_commands, 0, "disabled: nothing sent");

    world_init_control(&w);
    /* Asked before the first data round: the serial number is not known yet. */
    expect_int(hiflow_session_request_inverter_power(&w.s, w.now, 0), 0, "off accepted");
    world_run(&w, 5000);
    expect_int(w.fi.power_commands, 1, "one command once the data named the inverter");
    expect(w.fi.last_power_sn == VEC_INVERTER_SN, "addressed to the inverter's serial number");
    expect_int(w.fi.power_untimed, 0, "the command carries the time");
    expect_int(w.fi.output_on, 0, "the output is off");
    expect_int(w.power_reports, 1, "one report");
    expect_int(w.last_power_on, 0, "reported: off");
    expect_int(w.last_power_confirmed, 1, "reported: confirmed");
    world_run(&w, 60000);
    expect_int(w.fi.power_commands, 1, "no repeat");
    expect_int(w.connects, 1, "the connection stays");
    expect(w.data_count >= 3, "data keeps flowing");

    hiflow_session_request_inverter_power(&w.s, w.now, 1);
    world_run(&w, 5000);
    expect_int(w.fi.power_commands, 2, "second command");
    expect_int(w.fi.output_on, 1, "the output is on again");
    expect_int(w.last_power_on, 1, "reported: on");
    expect_int(w.last_power_confirmed, 1, "reported: confirmed");

    /* A newer request replaces one that has not gone out. */
    world_init_control(&w);
    hiflow_session_request_inverter_power(&w.s, w.now, 0);
    hiflow_session_request_inverter_power(&w.s, w.now, 1);
    world_run(&w, 5000);
    expect_int(w.fi.power_commands, 1, "replaced: one command");
    expect_int(w.fi.output_on, 1, "replaced: the newest (on) went out");
    expect_int(w.power_reports, 1, "replaced: one report");
}

static void test_power_failures(void)
{
    world_t w;

    printf("[30] inverter on/off: failures are reported, never retried\n");
    world_init_control(&w);
    world_run(&w, 5000);

    w.fi.silent_on_power = 1;
    hiflow_session_request_inverter_power(&w.s, w.now, 0);
    world_run(&w, 25000);
    expect_int(w.fi.power_commands, 1, "unanswered: one command");
    expect_int(w.power_reports, 1, "unanswered: reported");
    expect_int(w.last_power_confirmed, 0, "unanswered: not confirmed");
    expect_int(w.connects, 1, "unanswered: the connection stays");
    expect_int((long) hiflow_session_failures(&w.s), 0, "unanswered: not a session failure");
    world_run(&w, 120000);
    expect_int(w.fi.power_commands, 1, "unanswered: no retry");

    w.fi.silent_on_power = 0;
    w.fi.power_err_code = 3;
    hiflow_session_request_inverter_power(&w.s, w.now, 0);
    world_run(&w, 5000);
    expect_int(w.fi.power_commands, 2, "refused: one command");
    expect_int(w.last_power_confirmed, 0, "refused: not confirmed");
    expect_int(w.fi.output_on, 1, "refused: the output stays on");

    /* The link dies on the command: reported, not retried after the reconnect. */
    w.fi.power_err_code = 0;
    w.fi.kill_link_on_power = 1;
    hiflow_session_request_inverter_power(&w.s, w.now, 0);
    world_run(&w, 5000);
    expect_int(w.fi.power_commands, 3, "killed: one command");
    expect_int(w.power_reports, 3, "killed: reported");
    expect_int(w.last_power_confirmed, 0, "killed: not confirmed");
    w.fi.kill_link_on_power = 0;
    world_run(&w, 120000);
    expect_int(w.connects, 2, "killed: reconnected");
    expect_int(w.fi.power_commands, 3, "killed: no retry after the reconnect");
    expect(w.data_count >= 4, "killed: data again");

    /* Asked while no link can come up (at night): dropped after two minutes. */
    world_init_control(&w);
    w.fi.kill_link_on_login = 1;
    hiflow_session_request_inverter_power(&w.s, w.now, 0);
    world_run(&w, 180000);
    expect_int(w.fi.power_commands, 0, "offline: nothing sent");
    expect_int(w.power_reports, 1, "offline: the drop is reported");
    expect_int(w.last_power_confirmed, 0, "offline: not confirmed");
    w.fi.kill_link_on_login = 0;
    world_run(&w, 600000);
    expect_int(w.fi.power_commands, 0, "offline: the stale request never goes out");
}

/* ---------- accessors for the display ---------- */

static void test_display_accessors(void)
{
    world_t w;

    printf("[31] accessors: retry countdown, power limit, local time\n");
    world_init_ex(&w, 1, TEST_PIN, 1);
    expect_int(hiflow_session_power_limit_tenths(&w.s), -1, "limit unknown before the read");
    world_run(&w, 5000);
    expect_int(hiflow_session_power_limit_tenths(&w.s), 1000, "limit after the read");
    expect_int(hiflow_session_retry_in_s(&w.s, w.now), -1, "no countdown on a working link");
    /* the clock starts at 1774000000 = 2026-03-20, still winter time */
    expect((hiflow_session_local_time(&w.s, w.now) - hiflow_session_unix_time(&w.s, w.now)) == 3600,
           "local time = unix time + offset");

    world_init(&w, 1, TEST_PIN);
    w.fi.kill_link_on_login = 1;
    world_run(&w, 3000);
    expect(hiflow_session_retry_in_s(&w.s, w.now) > 0, "countdown while waiting after a failure");
    expect(hiflow_session_retry_in_s(&w.s, w.now) <= 300, "countdown within the backoff");
    expect_int(hiflow_session_power_limit_tenths(&w.s), -1, "limit disabled: -1");
}

static void test_retry_now(void)
{
    world_t w;

    printf("[32] retry_now ends only the wait after a link failure\n");
    world_init(&w, 1, TEST_PIN);
    world_single_page(&w);
    world_run(&w, 5000);
    hiflow_session_retry_now(&w.s, w.now);
    expect_int(hiflow_session_retry_in_s(&w.s, w.now), -1, "no-op on a working link");

    world_init(&w, 1, TEST_PIN);
    world_single_page(&w);
    w.fi.silent_on_login = 1;
    world_run(&w, 2000);
    world_drop_link(&w, HIFLOW_LINK_RADIO_TIMEOUT);
    world_run(&w, 1000);
    expect(hiflow_session_retry_in_s(&w.s, w.now) > 0, "waiting after the radio loss");
    hiflow_session_retry_now(&w.s, w.now);
    expect_int(hiflow_session_retry_in_s(&w.s, w.now), 0, "radio loss: the wait is over");

    world_init(&w, 1, TEST_PIN);
    world_single_page(&w);
    w.fi.require_pin = 1;
    w.fi.refuse_pin = 1;
    world_run(&w, 60000);
    expect_int(hiflow_session_status(&w.s), 13, "PIN refused");
    hiflow_session_retry_now(&w.s, w.now);
    expect(hiflow_session_retry_in_s(&w.s, w.now) > 600, "PIN refused: the half-hour wait stays");

    world_init(&w, 1, TEST_PIN);
    w.fi.kill_link_on_login = 1;
    world_run(&w, 3000);
    expect_int(hiflow_session_status(&w.s), 9, "login rejected");
    hiflow_session_retry_now(&w.s, w.now);
    expect(hiflow_session_retry_in_s(&w.s, w.now) > 0, "login rejected: the wait stays");
}

/* ---------- day log (for the display) ---------- */

/* A one-page reply with the given AC power (x0.1 W) and lifetime energy. */
static void set_power_page(world_t *w, int32_t ac_tenths_w, int32_t energy_wh, int32_t daily_wh)
{
    static RealDataNewReqDTO m;
    static uint8_t buf[256];
    static char hex[513];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    size_t i;

    memset(&m, 0, sizeof(m));
    snprintf(m.device_serial_number, sizeof(m.device_serial_number), "TESTDTU00001");
    m.ap = 1;
    m.sgs_data_count = 1;
    m.sgs_data[0].serial_number = VEC_INVERTER_SN;
    m.sgs_data[0].active_power = ac_tenths_w;
    m.sgs_data[0].voltage = 2305;
    m.pv_data_count = 1;
    m.pv_data[0].port_number = 1;
    m.pv_data[0].power = ac_tenths_w;
    m.pv_data[0].energy_total = energy_wh;
    m.pv_data[0].energy_daily = daily_wh;
    pb_encode(&os, RealDataNewReqDTO_fields, &m);
    for (i = 0; i < os.bytes_written; i++)
        snprintf(hex + 2 * i, 3, "%02x", buf[i]);
    fake_set_pages(&w->fi, hex, NULL);
}

static int64_t local_now(world_t *w) { return hiflow_session_local_time(&w->s, w->now); }

static void test_daylog(void)
{
    static world_t w;
    int i;

    printf("[33] day log: the session's readings reach it, with the clock and the counters\n");

    world_init(&w, 1, TEST_PIN);
    w.use_daylog = 1;
    hiflow_daylog_init(&w.daylog);
    set_power_page(&w, 8123, 400000, 1234);
    world_run(&w, 90000);
    expect(w.data_count >= 3, "a day with readings");
    expect_float(w.daylog.rec.peak_w, 812.3f, "the peak");
    expect_int(w.daylog.rec.curve[hiflow_local_minute(local_now(&w)) / 5], 812,
               "the curve slot of now: the V0 reply synced the clock");
    expect_float(w.daylog.rec.energy_total_wh, 400000.0f, "the lifetime energy");
    expect_float(w.daylog.rec.energy_daily_wh, 1234.0f, "the day's energy");
    expect_float(w.daylog.rec.port_daily_wh[0], 1234.0f, "the day's energy of input 1");
    expect_int(w.daylog.rec.last_feed, 1, "the last reading had power");

    /* Dusk: 0 W with the link up, then the radio goes dark. Standby five
       minutes after the first reading at 0 W, whatever the session does. */
    set_power_page(&w, 0, 400000, 1234);
    world_run(&w, 60000);
    expect_float(w.last_data.ac_power_w, 0.0f, "dusk: 0 W");
    w.dark = 1;
    w.fi.kill_link_on_data = 1;
    for (i = 0; i < 400; i++) {
        world_run(&w, 1000);
        hiflow_daylog_update(&w.daylog, w.now, 0);
    }
    expect_int(w.daylog.rec.standby, 1, "standby after dusk");
    expect_int(w.daylog.days_begun, 0, "the same day");

    /* Dawn: the day counter starts again; the second reading begins the day. */
    w.dark = 0;
    w.fi.kill_link_on_data = 0;
    set_power_page(&w, 0, 400000, 0);
    world_run(&w, 120000);
    expect_int(w.daylog.days_begun, 1, "dawn: a new day");
    expect_float(w.daylog.rec.peak_w, 0.0f, "no peak yet");
    expect_float(w.daylog.rec.energy_total_wh, 400000.0f, "the lifetime energy stays");

    /* The clock counts as synced once the device time or a network time came. */
    expect_int(hiflow_session_clock_synced(&w.s), 1, "synced by the device time of the V0 reply");
    world_init(&w, 1, TEST_PIN);
    expect_int(hiflow_session_clock_synced(&w.s), 0, "not synced at boot");
    expect_int(hiflow_session_observe_time(&w.s, w.now, 1000), 0, "an implausible network time is refused");
    expect_int(hiflow_session_clock_synced(&w.s), 0, "still not synced");
    expect_int(hiflow_session_observe_time(&w.s, w.now, 1774000000 + 7200), 1, "a network time ahead moves the clock");
    expect_int(hiflow_session_clock_synced(&w.s), 1, "synced by the network time");

    /* The inverter's time counts only when it moved our clock or agrees with
       it: one hours behind says the inverter's clock is off, and ours may be. */
    world_init(&w, 1, TEST_PIN);
    w.fi.device_time = 1774000000 - 7200;
    world_single_page(&w);
    world_run(&w, 5000);
    expect_int(hiflow_session_state(&w.s), HIFLOW_STATE_READY, "a device time far behind: still a session");
    expect_int(hiflow_session_clock_synced(&w.s), 0, "a device time far behind ours: not synced");
    world_init(&w, 1, TEST_PIN);
    w.fi.device_time = 1774000000 - 60;
    world_single_page(&w);
    world_run(&w, 5000);
    expect_int(hiflow_session_clock_synced(&w.s), 1, "a device time within a few minutes of ours: synced");
    world_init(&w, 1, TEST_PIN);
    w.fi.device_time = 1774000000 + 7200;
    world_single_page(&w);
    world_run(&w, 5000);
    expect_int(hiflow_session_clock_synced(&w.s), 1, "a device time ahead moves the clock: synced");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-v") == 0)
        g_verbose = 1;

    printf("=== hiflow_session ===\n");
    test_happy_path();
    test_poll_cadence();
    test_poll_cadence_slow_link();
    test_poll_cadence_coarse_tick();
    test_poll_cadence_stall();
    test_paging();
    test_pin_path();
    test_login_in_progress();
    test_login_never_confirmed();
    test_tid_range();
    test_link_killed_at_login();
    test_rotated_key_recovery();
    test_reply_timeout();
    test_wrong_pin();
    test_missing_pin();
    test_radio_loss();
    test_session_ends_after_data();
    test_backoff_growth();
    test_stray_frames();
    test_long_uptime();
    test_start_without_key();
    test_clock_behind_after_reboot();
    test_empty_data_reply();
    test_limit_read();
    test_limit_write();
    test_limit_dedup_and_rounding();
    test_limit_rate();
    test_limit_failures();
    test_limit_offline();
    test_limit_link_killed();
    test_power_switch();
    test_power_failures();
    test_display_accessors();
    test_retry_now();
    test_daylog();

    printf("\n=== summary ===\n");
    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0)
        printf("session: %d/%d ok — ALL PASS\n", g_pass, g_pass);
    return g_fail == 0 ? 0 : 1;
}
