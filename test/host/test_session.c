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
 *
 * Build/run:  make -C test/host test-session
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fake_inverter.h"
#include "hiflow_session.h"
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

    int     connects;
    int64_t connect_at_ms[32];
    int     disconnects;
    int     data_count;
    hiflow_data_t last_data;
    int     enc_rand_saves;
    uint8_t last_saved_key[HIFLOW_ENC_RAND_LEN];
    uint8_t status_seen[WORLD_MAX_STATUS];
    int     status_count;
} world_t;

static int world_send(void *ctx, const uint8_t *frame, size_t len)
{
    world_t *w = (world_t *) ctx;

    if (!w->link_up)
        return 0;
    if (fake_handle_frame(&w->fi, frame, len)) {
        memcpy(w->reply, w->fi.reply, w->fi.reply_len);
        w->reply_len = w->fi.reply_len;
        w->reply_at = w->now + WORLD_REPLY_MS;
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

    w->data_count++;
    w->last_data = *data;
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

        if (w->connect_at >= 0 && w->now >= w->connect_at) {
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

static void world_init(world_t *w, int have_key, const char *pin)
{
    hiflow_session_config_t cfg;
    hiflow_session_ops_t ops;

    memset(w, 0, sizeof(*w));
    w->connect_at = -1;
    w->reply_at = -1;
    w->link_down_at = -1;
    w->chunk = WORLD_CHUNK;
    w->now = 1000;

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

    memset(&ops, 0, sizeof(ops));
    ops.ctx = w;
    ops.send = world_send;
    ops.disconnect = world_disconnect;
    ops.set_link_allowed = world_set_link_allowed;
    ops.on_data = world_on_data;
    ops.on_enc_rand = world_on_enc_rand;
    ops.on_status = world_on_status;
    ops.log = world_log;

    hiflow_session_init(&w->s, &cfg, &ops, w->now, 1774000000, 0);
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

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-v") == 0)
        g_verbose = 1;

    printf("=== hiflow_session ===\n");
    test_happy_path();
    test_poll_cadence();
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

    printf("\n=== summary ===\n");
    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0)
        printf("session: %d/%d ok — ALL PASS\n", g_pass, g_pass);
    return g_fail == 0 ? 0 : 1;
}
