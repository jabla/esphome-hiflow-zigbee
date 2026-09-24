#include "hiflow_session.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hiflow_appinfo.h"

/* What the single deadline is about. */
enum {
    HT_NONE = 0,
    HT_REPLY,       /* waiting for the reply to the last request */
    HT_SEND_POLL,   /* the gap between two status polls          */
    HT_BACKOFF_END, /* waiting after a failure                   */
    HT_DATA_POLL    /* next data request                         */
};

/* Same counts as the reference implementation. */
#define MAX_LOGIN_POLLS 5
#define MAX_PIN_POLLS   8
/* Consecutive undecodable data replies before the connection is given up. */
#define MAX_DECODE_FAILURES 3
/* After this long without a connection the status reports "no link". */
#define NO_LINK_AFTER_MS 300000

static void enter_backoff(hiflow_session_t *s, int64_t now_ms, uint32_t delay_ms);
static void enter_wait_link(hiflow_session_t *s, int64_t now_ms);
static void request_data(hiflow_session_t *s, int64_t now_ms);
static void send_login(hiflow_session_t *s, int64_t now_ms);
static void send_status_poll(hiflow_session_t *s, int64_t now_ms, int32_t action);
static void update_status(hiflow_session_t *s, int64_t now_ms);

/* ---------- small helpers ---------- */

static void slog(hiflow_session_t *s, int level, const char *fmt, ...)
{
    char buf[128];
    va_list ap;

    if (s->ops.log == NULL)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s->ops.log(s->ops.ctx, level, buf);
}

static int64_t unix_now(const hiflow_session_t *s, int64_t now_ms)
{
    return hiflow_clock_now(&s->clock, now_ms);
}

static int32_t utc_offset(const hiflow_session_t *s, int64_t unix_time)
{
    return s->cfg.eu_dst ? hiflow_tz_offset_eu(unix_time, s->cfg.std_offset) : s->cfg.std_offset;
}

static void set_timer(hiflow_session_t *s, uint8_t kind, int64_t due_ms)
{
    s->timer = kind;
    s->deadline_ms = due_ms;
}

static void clear_timer(hiflow_session_t *s) { s->timer = HT_NONE; }

static void set_state(hiflow_session_t *s, hiflow_session_state_t state)
{
    if (s->state == (uint8_t) state)
        return;
    slog(s, 3, "state %s -> %s", hiflow_session_state_name(s->state),
         hiflow_session_state_name((uint8_t) state));
    s->state = (uint8_t) state;
}

static int in_handshake(uint8_t state)
{
    switch (state) {
    case HIFLOW_STATE_PAIRING:
    case HIFLOW_STATE_LOGIN:
    case HIFLOW_STATE_LOGIN_POLL:
    case HIFLOW_STATE_PIN:
    case HIFLOW_STATE_PIN_POLL:
    case HIFLOW_STATE_TIMESYNC:
    case HIFLOW_STATE_TIMESYNC_POLL:
        return 1;
    default:
        return 0;
    }
}

/* After a V0 pairing that handed back the key we already had, another failed
   login is a key problem, not a refused identity. */
static hiflow_fail_reason_t handshake_fail_reason(const hiflow_session_t *s)
{
    return s->key_unchanged ? HIFLOW_FAIL_STALE_KEY : HIFLOW_FAIL_LOGIN_REJECTED;
}

static void allow_link(hiflow_session_t *s, int allowed)
{
    if (s->link_allowed != allowed) {
        s->link_allowed = allowed;
        if (s->ops.set_link_allowed != NULL)
            s->ops.set_link_allowed(s->ops.ctx, allowed);
    }
    if (!allowed && s->link_up) {
        s->link_up = 0;
        if (s->ops.disconnect != NULL)
            s->ops.disconnect(s->ops.ctx);
    }
}

/* ---------- status ---------- */

static uint8_t status_code(const hiflow_session_t *s, int64_t now_ms)
{
    switch (s->state) {
    case HIFLOW_STATE_BACKOFF:
        if (s->fail_reason == HIFLOW_FAIL_NONE)
            return HIFLOW_STATUS_WAIT_LINK;
        return (uint8_t) (HIFLOW_STATUS_BACKOFF_BASE + s->fail_reason);
    case HIFLOW_STATE_WAIT_LINK:
        if (now_ms - s->link_allowed_since_ms > NO_LINK_AFTER_MS)
            return HIFLOW_STATUS_BACKOFF_BASE + HIFLOW_FAIL_NO_CONNECTION;
        return HIFLOW_STATUS_WAIT_LINK;
    case HIFLOW_STATE_PAIRING:
        return HIFLOW_STATUS_PAIRING;
    case HIFLOW_STATE_LOGIN:
    case HIFLOW_STATE_LOGIN_POLL:
        return HIFLOW_STATUS_LOGIN;
    case HIFLOW_STATE_PIN:
    case HIFLOW_STATE_PIN_POLL:
        return HIFLOW_STATUS_PIN;
    case HIFLOW_STATE_TIMESYNC:
    case HIFLOW_STATE_TIMESYNC_POLL:
        return HIFLOW_STATUS_TIME_SYNC;
    case HIFLOW_STATE_READY:
        return HIFLOW_STATUS_READY;
    case HIFLOW_STATE_WAIT_DATA:
        return HIFLOW_STATUS_READING;
    default:
        return HIFLOW_STATUS_WAIT_LINK;
    }
}

static void update_status(hiflow_session_t *s, int64_t now_ms)
{
    uint8_t code = status_code(s, now_ms);

    if (code == s->status)
        return;
    s->status = code;
    if (s->ops.on_status != NULL)
        s->ops.on_status(s->ops.ctx, code);
}

/* ---------- failure handling ---------- */

static void fail(hiflow_session_t *s, int64_t now_ms, hiflow_fail_reason_t reason, const char *why)
{
    uint32_t backoff;
    uint8_t i;

    if (reason == HIFLOW_FAIL_LOGIN_REJECTED || reason == HIFLOW_FAIL_STALE_KEY ||
        reason == HIFLOW_FAIL_PIN)
        s->failures++;
    if (s->fail_streak < 255)
        s->fail_streak++;

    if (reason == HIFLOW_FAIL_PIN) {
        backoff = s->cfg.pin_backoff_ms;
    } else {
        backoff = s->cfg.backoff_min_ms;
        for (i = 1; i < s->fail_streak; i++) {
            if (backoff >= s->cfg.backoff_max_ms)
                break;
            backoff *= 2;
        }
        if (backoff > s->cfg.backoff_max_ms)
            backoff = s->cfg.backoff_max_ms;
    }

    s->fail_reason = (uint8_t) reason;
    slog(s, 1, "%s (reason %u, failure %u in a row) - waiting %u s", why, (unsigned) reason,
         (unsigned) s->fail_streak, (unsigned) (backoff / 1000));
    enter_backoff(s, now_ms, backoff);
}

static void enter_backoff(hiflow_session_t *s, int64_t now_ms, uint32_t delay_ms)
{
    set_state(s, HIFLOW_STATE_BACKOFF);
    s->expected_cmd = 0;
    s->handshake_started = 0;
    hiflow_rx_reset(&s->rx);
    set_timer(s, HT_BACKOFF_END, now_ms + (int64_t) delay_ms);
    /* No connection is held while waiting: the inverter has a single BLE slot,
       and an idle link there is what the app runs into. */
    allow_link(s, 0);
    update_status(s, now_ms);
}

static void enter_wait_link(hiflow_session_t *s, int64_t now_ms)
{
    set_state(s, HIFLOW_STATE_WAIT_LINK);
    s->handshake_started = 0;
    s->logins_sent = 0;
    s->v0_done_on_link = 0;
    s->data_seen = 0;
    s->expected_cmd = 0;
    s->link_allowed_since_ms = now_ms;
    clear_timer(s);
    hiflow_rx_reset(&s->rx);
    allow_link(s, 1);
    update_status(s, now_ms);
}

/* ---------- sending ---------- */

static int send_payload(hiflow_session_t *s, int64_t now_ms, uint16_t cmd,
                        const uint8_t *pt, size_t pt_len)
{
    size_t frame_len = 0;
    int rc;

    if (hiflow_is_v0_cmd(cmd))
        rc = hiflow_build_frame_v0(s->cfg.sn, cmd, s->tid, pt, pt_len, s->tx_frame,
                                   sizeof(s->tx_frame), &frame_len);
    else
        rc = hiflow_build_frame_v1(s->cfg.enc_rand, cmd, s->tid, pt, pt_len, s->tx_frame,
                                   sizeof(s->tx_frame), &frame_len);
    /* The reference keeps the transaction id in 15 bits. */
    s->tid = (uint16_t) ((s->tid + 1) & 0x7FFF);

    if (rc != HIFLOW_OK) {
        fail(s, now_ms, HIFLOW_FAIL_NO_REPLY, "could not build the frame");
        return 0;
    }

    /* A reply that arrived late or half must never be glued in front of the
       next one. */
    hiflow_rx_reset(&s->rx);
    s->expected_cmd = HIFLOW_REPLY_CMD(cmd);

    if (s->ops.send == NULL || !s->ops.send(s->ops.ctx, s->tx_frame, frame_len)) {
        fail(s, now_ms, HIFLOW_FAIL_NO_REPLY, "could not write the frame");
        return 0;
    }
    slog(s, 3, "TX cmd=0x%04X len=%u", (unsigned) cmd, (unsigned) frame_len);
    set_timer(s, HT_REPLY, now_ms + (int64_t) s->cfg.request_timeout_ms);
    return 1;
}

static void send_v0_pairing(hiflow_session_t *s, int64_t now_ms)
{
    int64_t unix_time = unix_now(s, now_ms);
    size_t len = 0;

    if (hiflow_encode_app_info_v0(s->tx_pt, sizeof(s->tx_pt), &len, unix_time,
                                  utc_offset(s, unix_time)) != HIFLOW_OK) {
        fail(s, now_ms, HIFLOW_FAIL_NO_REPLY, "could not encode the V0 pairing request");
        return;
    }
    slog(s, 2, "V0 pairing: asking for the current encRand");
    set_state(s, HIFLOW_STATE_PAIRING);
    send_payload(s, now_ms, HIFLOW_CMD_APP_INFO_V0, s->tx_pt, len);
}

static void send_login(hiflow_session_t *s, int64_t now_ms)
{
    int64_t unix_time = unix_now(s, now_ms);
    size_t len = 0;

    if (s->logins_sent > 0) {
        /* Never a second login on the same connection: that is what the
           inverter answers with a silent link kill. */
        slog(s, 1, "a login already went out on this link - reconnecting instead");
        fail(s, now_ms, HIFLOW_FAIL_LOGIN_REJECTED, "second login on the same link avoided");
        return;
    }
    if (hiflow_encode_comm_cmd(s->tx_pt, sizeof(s->tx_pt), &len, unix_time, HIFLOW_ACTION_LOGIN,
                               s->cfg.ble_id) != HIFLOW_OK) {
        fail(s, now_ms, HIFLOW_FAIL_NO_REPLY, "could not encode the login");
        return;
    }
    s->logins_sent++;
    slog(s, 2, "handshake: login (action 64)");
    set_state(s, HIFLOW_STATE_LOGIN);
    send_payload(s, now_ms, HIFLOW_CMD_COMM_CMD, s->tx_pt, len);
}

static void send_pin(hiflow_session_t *s, int64_t now_ms)
{
    int64_t unix_time = unix_now(s, now_ms);
    size_t len = 0;

    if (hiflow_encode_comm_cmd(s->tx_pt, sizeof(s->tx_pt), &len, unix_time, HIFLOW_ACTION_PIN,
                               s->cfg.pin) != HIFLOW_OK) {
        fail(s, now_ms, HIFLOW_FAIL_PIN, "could not encode the PIN");
        return;
    }
    slog(s, 2, "handshake: PIN (action 82)");
    set_state(s, HIFLOW_STATE_PIN);
    send_payload(s, now_ms, HIFLOW_CMD_COMM_CMD, s->tx_pt, len);
}

static void send_time_sync(hiflow_session_t *s, int64_t now_ms)
{
    int64_t unix_time = unix_now(s, now_ms);
    size_t len = 0;

    if (hiflow_encode_time_sync(s->tx_pt, sizeof(s->tx_pt), &len, unix_time,
                                utc_offset(s, unix_time)) != HIFLOW_OK) {
        fail(s, now_ms, HIFLOW_FAIL_NO_REPLY, "could not encode the time-sync");
        return;
    }
    slog(s, 2, "handshake: time-sync (action 104)");
    set_state(s, HIFLOW_STATE_TIMESYNC);
    send_payload(s, now_ms, HIFLOW_CMD_COMM_CMD, s->tx_pt, len);
}

static void send_status_poll(hiflow_session_t *s, int64_t now_ms, int32_t action)
{
    int64_t unix_time = unix_now(s, now_ms);
    size_t len = 0;

    if (hiflow_encode_status_poll(s->tx_pt, sizeof(s->tx_pt), &len, unix_time, action) !=
        HIFLOW_OK) {
        fail(s, now_ms, HIFLOW_FAIL_NO_REPLY, "could not encode the status poll");
        return;
    }
    send_payload(s, now_ms, HIFLOW_CMD_COMM_STATUS, s->tx_pt, len);
}

static void schedule_status_poll(hiflow_session_t *s, int64_t now_ms, int32_t action)
{
    s->pending_poll_action = action;
    s->expected_cmd = 0;
    set_timer(s, HT_SEND_POLL, now_ms + (int64_t) s->cfg.status_poll_gap_ms);
}

static void request_data(hiflow_session_t *s, int64_t now_ms)
{
    int64_t unix_time = unix_now(s, now_ms);
    size_t len = 0;

    if (s->current_page == 0)
        hiflow_measurements_reset(&s->data);
    if (hiflow_encode_real_data_request(s->tx_pt, sizeof(s->tx_pt), &len, unix_time,
                                        utc_offset(s, unix_time), s->current_page) != HIFLOW_OK) {
        fail(s, now_ms, HIFLOW_FAIL_NO_REPLY, "could not encode the data request");
        return;
    }
    slog(s, 3, "requesting data page %d", (int) s->current_page);
    set_state(s, HIFLOW_STATE_WAIT_DATA);
    send_payload(s, now_ms, HIFLOW_CMD_REAL_DATA, s->tx_pt, len);
}

/* ---------- handshake steps ---------- */

static void go_time_sync(hiflow_session_t *s, int64_t now_ms) { send_time_sync(s, now_ms); }

static void handshake_complete(hiflow_session_t *s, int64_t now_ms)
{
    s->sessions++;
    s->fail_streak = 0;
    s->fail_reason = HIFLOW_FAIL_NONE;
    s->decode_failures = 0;
    s->key_unchanged = 0;
    slog(s, 2, "handshake complete (session %u), reading data", (unsigned) s->sessions);
    set_state(s, HIFLOW_STATE_READY);
    s->current_page = 0;
    request_data(s, now_ms);
}

static void handle_login_ack(hiflow_session_t *s, int64_t now_ms, const uint8_t *pt, size_t len)
{
    int64_t device_time = hiflow_decode_login_ack_time(pt, len);

    if (device_time > 0 && hiflow_clock_observe_device_time(&s->clock, now_ms, device_time))
        slog(s, 2, "clock pulled forward to the device time");

    set_state(s, HIFLOW_STATE_LOGIN_POLL);
    s->login_polls = 0;
    send_status_poll(s, now_ms, HIFLOW_ACTION_LOGIN);
}

static void handle_login_poll(hiflow_session_t *s, int64_t now_ms, const uint8_t *pt, size_t len)
{
    int32_t action = 0, sts = 0;

    hiflow_decode_status_reply(pt, len, &action, &sts);
    slog(s, 3, "login poll: sts=%d", (int) sts);

    if (sts == HIFLOW_STS_LOGIN_OK) {
        go_time_sync(s, now_ms);
        return;
    }
    if (sts == HIFLOW_STS_PIN_REQUIRED) {
        if (s->cfg.pin[0] == '\0') {
            fail(s, now_ms, HIFLOW_FAIL_PIN, "the device asks for the BLE PIN but none is set");
            return;
        }
        slog(s, 2, "the device asks for the BLE PIN");
        send_pin(s, now_ms);
        return;
    }
    if (sts == HIFLOW_STS_IN_PROGRESS && ++s->login_polls < MAX_LOGIN_POLLS) {
        schedule_status_poll(s, now_ms, HIFLOW_ACTION_LOGIN);
        return;
    }
    /* Like the reference: carry on with the time-sync even when the login was
       never confirmed; the data requests then show whether it took. */
    slog(s, 1, "login not confirmed (sts=%d) - continuing with the time-sync", (int) sts);
    go_time_sync(s, now_ms);
}

static void handle_pin_poll(hiflow_session_t *s, int64_t now_ms, const uint8_t *pt, size_t len)
{
    int32_t action = 0, sts = 0;

    hiflow_decode_status_reply(pt, len, &action, &sts);
    slog(s, 3, "PIN poll: sts=%d", (int) sts);

    if (sts == HIFLOW_STS_PIN_OK) {
        slog(s, 2, "PIN accepted, the identity is whitelisted");
        go_time_sync(s, now_ms);
        return;
    }
    if (sts == HIFLOW_STS_PIN_WRONG) {
        /* Never retry a refused PIN in a loop. */
        fail(s, now_ms, HIFLOW_FAIL_PIN, "the device refused the BLE PIN");
        return;
    }
    if (++s->pin_polls < MAX_PIN_POLLS) {
        schedule_status_poll(s, now_ms, HIFLOW_ACTION_PIN);
        return;
    }
    go_time_sync(s, now_ms);
}

static void handle_pairing_reply(hiflow_session_t *s, int64_t now_ms, const uint8_t *pt, size_t len)
{
    uint8_t fresh[HIFLOW_ENC_RAND_LEN];
    int64_t device_time = 0;
    int changed;

    if (hiflow_appinfo_extract_enc_rand(pt, len, fresh, &device_time) != HIFLOW_OK) {
        fail(s, now_ms, HIFLOW_FAIL_STALE_KEY, "the V0 pairing reply carries no encRand");
        return;
    }

    /* The login carries our clock, and after a reboot that clock restarts from
       the last value saved to flash, i.e. behind the inverter's. The reply's
       timestamp lets the login go out with the inverter's own time. */
    if (device_time > 0) {
        slog(s, 2, "V0 reply: device time is %ld s ahead of our clock",
             (long) (device_time - unix_now(s, now_ms)));
        if (hiflow_clock_observe_device_time(&s->clock, now_ms, device_time))
            slog(s, 2, "clock pulled forward to the device time");
    } else {
        slog(s, 1, "V0 reply carries no device time");
    }

    changed = !s->cfg.have_enc_rand ||
              memcmp(s->cfg.enc_rand, fresh, HIFLOW_ENC_RAND_LEN) != 0;
    memcpy(s->cfg.enc_rand, fresh, HIFLOW_ENC_RAND_LEN);
    s->cfg.have_enc_rand = 1;
    s->need_v0 = 0;
    s->v0_done_on_link = 1;
    s->key_unchanged = !changed;

    if (changed) {
        /* The value itself never reaches the log: it is the session key. */
        slog(s, 2, "V0 pairing returned a fresh encRand");
        if (s->ops.on_enc_rand != NULL)
            s->ops.on_enc_rand(s->ops.ctx, fresh);
    } else {
        slog(s, 1, "V0 pairing returned the encRand we already had");
    }
    /* Straight on with the login, on this same connection. */
    send_login(s, now_ms);
}

static void handle_data(hiflow_session_t *s, int64_t now_ms, const uint8_t *pt, size_t len)
{
    hiflow_data_t out;
    int32_t ap = 0, cp = 0;

    if (hiflow_merge_real_data(&s->data, pt, len, &ap, &cp) != HIFLOW_OK) {
        s->decode_failures++;
        slog(s, 1, "data reply did not decode (%u in a row)", (unsigned) s->decode_failures);
        if (s->decode_failures >= MAX_DECODE_FAILURES) {
            fail(s, now_ms, HIFLOW_FAIL_NO_REPLY, "data replies keep failing to decode");
            return;
        }
        s->current_page = 0;
        set_state(s, HIFLOW_STATE_READY);
        set_timer(s, HT_DATA_POLL, now_ms + (int64_t) s->cfg.poll_interval_ms);
        return;
    }
    s->decode_failures = 0;
    slog(s, 3, "data page cp=%d ap=%d", (int) cp, (int) ap);

    if (!hiflow_measurements_complete(&s->data)) {
        s->current_page = cp + 1;
        request_data(s, now_ms);
        return;
    }

    hiflow_measurements_to_data(&s->data, &out);
    s->data_seen = 1;
    s->fail_streak = 0;
    s->fail_reason = HIFLOW_FAIL_NONE;
    s->current_page = 0;
    /* The first reply of a session sometimes carries the grid and port blocks
       with every field empty (seen in the field). The lifetime energy is
       never 0 on a real reading, so such a page is dropped instead of zeroing
       every measurement in Home Assistant. */
    if (out.energy_total_wh <= 0.0f)
        slog(s, 1, "dropping an empty data reply (lifetime energy 0)");
    else if (s->ops.on_data != NULL)
        s->ops.on_data(s->ops.ctx, &out);
    set_state(s, HIFLOW_STATE_READY);
    set_timer(s, HT_DATA_POLL, now_ms + (int64_t) s->cfg.poll_interval_ms);
}

/* ---------- frame dispatch ---------- */

static void handle_frame(hiflow_session_t *s, int64_t now_ms, const uint8_t *frame, size_t len)
{
    uint16_t cmd = 0, tid = 0;
    size_t pt_len = 0;
    int rc;

    if (len < HIFLOW_HEADER_LEN)
        return;
    cmd = (uint16_t) (((uint16_t) frame[2] << 8) | frame[3]);

    if (hiflow_is_v0_cmd(cmd))
        rc = hiflow_parse_frame_v0(s->cfg.sn, frame, len, &cmd, &tid, s->rx_pt, sizeof(s->rx_pt),
                                   &pt_len);
    else
        rc = hiflow_parse_frame_v1(s->cfg.enc_rand, frame, len, &cmd, &tid, s->rx_pt,
                                   sizeof(s->rx_pt), &pt_len);

    if (rc == HIFLOW_ERR_GCM_TAG) {
        /* The key no longer matches: pair again on the next connection. */
        s->need_v0 = 1;
        fail(s, now_ms, HIFLOW_FAIL_STALE_KEY, "a V1 frame did not authenticate");
        return;
    }
    if (rc != HIFLOW_OK) {
        fail(s, now_ms, HIFLOW_FAIL_NO_REPLY, "unreadable frame");
        return;
    }
    if (s->expected_cmd != 0 && cmd != s->expected_cmd) {
        slog(s, 3, "reply 0x%04X while waiting for 0x%04X - ignored", (unsigned) cmd,
             (unsigned) s->expected_cmd);
        return;
    }

    /* The reply disarms the request timeout. */
    s->expected_cmd = 0;
    clear_timer(s);

    switch (s->state) {
    case HIFLOW_STATE_PAIRING:
        handle_pairing_reply(s, now_ms, s->rx_pt, pt_len);
        break;
    case HIFLOW_STATE_LOGIN:
        handle_login_ack(s, now_ms, s->rx_pt, pt_len);
        break;
    case HIFLOW_STATE_LOGIN_POLL:
        handle_login_poll(s, now_ms, s->rx_pt, pt_len);
        break;
    case HIFLOW_STATE_PIN:
        set_state(s, HIFLOW_STATE_PIN_POLL);
        s->pin_polls = 0;
        send_status_poll(s, now_ms, HIFLOW_ACTION_PIN);
        break;
    case HIFLOW_STATE_PIN_POLL:
        handle_pin_poll(s, now_ms, s->rx_pt, pt_len);
        break;
    case HIFLOW_STATE_TIMESYNC:
        set_state(s, HIFLOW_STATE_TIMESYNC_POLL);
        send_status_poll(s, now_ms, HIFLOW_ACTION_TIME_SYNC);
        break;
    case HIFLOW_STATE_TIMESYNC_POLL:
        handshake_complete(s, now_ms);
        break;
    case HIFLOW_STATE_WAIT_DATA:
        handle_data(s, now_ms, s->rx_pt, pt_len);
        break;
    default:
        slog(s, 3, "unsolicited frame 0x%04X ignored", (unsigned) cmd);
        break;
    }
}

/* ---------- public API ---------- */

void hiflow_session_config_defaults(hiflow_session_config_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->std_offset = 3600;
    cfg->eu_dst = 1;
    cfg->poll_interval_ms = 30000;
    /* The reference uses 15 s in Home Assistant; 8 s was too tight next to the
       802.15.4 radio. */
    cfg->request_timeout_ms = 15000;
    cfg->status_poll_gap_ms = 1000;
    cfg->backoff_min_ms = 30000;
    cfg->backoff_max_ms = 300000;
    cfg->pin_backoff_ms = 1800000;
}

void hiflow_session_init(hiflow_session_t *s, const hiflow_session_config_t *cfg,
                         const hiflow_session_ops_t *ops, int64_t now_ms,
                         int64_t build_time, int64_t persisted_time)
{
    if (s == NULL || cfg == NULL || ops == NULL)
        return;

    /* Copy first: a caller may hand us pointers into this very struct. */
    {
        hiflow_session_config_t cfg_copy = *cfg;
        hiflow_session_ops_t ops_copy = *ops;

        memset(s, 0, sizeof(*s));
        s->cfg = cfg_copy;
        s->ops = ops_copy;
    }
    hiflow_clock_init(&s->clock, now_ms, build_time, persisted_time);
    s->tid = 1;
    s->status = 0xFF; /* forces the first status report */
    s->state = HIFLOW_STATE_BACKOFF;
    s->link_allowed = -1; /* forces the first set_link_allowed callback */
    if (!s->cfg.have_enc_rand)
        s->need_v0 = 1;
    enter_wait_link(s, now_ms);
}

void hiflow_session_link_up(hiflow_session_t *s, int64_t now_ms)
{
    if (s == NULL)
        return;

    if (s->state != HIFLOW_STATE_WAIT_LINK || !s->link_allowed) {
        slog(s, 1, "link came up while %s - closing it again",
             hiflow_session_state_name(s->state));
        s->link_up = 1;
        if (s->ops.disconnect != NULL)
            s->ops.disconnect(s->ops.ctx);
        s->link_up = 0;
        return;
    }
    if (s->handshake_started) {
        slog(s, 3, "link_up repeated - the handshake is already running");
        return;
    }

    s->link_up = 1;
    s->handshake_started = 1;
    s->logins_sent = 0;
    s->v0_done_on_link = 0;
    s->data_seen = 0;
    s->current_page = 0;
    hiflow_rx_reset(&s->rx);
    hiflow_measurements_reset(&s->data);

    if (s->need_v0 || !s->cfg.have_enc_rand) {
        if (s->cfg.sn[0] == '\0') {
            fail(s, now_ms, HIFLOW_FAIL_STALE_KEY,
                 "a V0 pairing is due but no serial number is configured");
            return;
        }
        send_v0_pairing(s, now_ms);
        return;
    }
    send_login(s, now_ms);
    update_status(s, now_ms);
}

void hiflow_session_link_down(hiflow_session_t *s, int64_t now_ms, int reason)
{
    if (s == NULL)
        return;

    s->link_up = 0;
    s->expected_cmd = 0;
    hiflow_rx_reset(&s->rx);

    if (s->state == HIFLOW_STATE_BACKOFF) {
        slog(s, 3, "link down while %s - expected", hiflow_session_state_name(s->state));
        return;
    }

    slog(s, 1, "link down (reason 0x%02X) while %s", (unsigned) reason,
         hiflow_session_state_name(s->state));

    if (in_handshake(s->state)) {
        if (reason == HIFLOW_LINK_RADIO_TIMEOUT) {
            fail(s, now_ms, HIFLOW_FAIL_RADIO, "the radio link timed out during the handshake");
        } else {
            /* The classic symptom of a rotated encRand or a refused identity:
               the inverter kills the link instead of answering. Pair again on
               the next connection, like the reference does. */
            s->need_v0 = 1;
            fail(s, now_ms, handshake_fail_reason(s), "the link died during the handshake");
        }
        return;
    }

    if (s->state == HIFLOW_STATE_READY || s->state == HIFLOW_STATE_WAIT_DATA) {
        if (s->data_seen) {
            /* A session that delivered data and then ended - the inverter goes
               to sleep at dusk. Try again shortly, without counting a failure. */
            slog(s, 2, "session ended after delivering data");
            s->fail_reason = HIFLOW_FAIL_NONE;
            s->fail_streak = 0;
            enter_backoff(s, now_ms, s->cfg.backoff_min_ms);
            return;
        }
        fail(s, now_ms,
             reason == HIFLOW_LINK_RADIO_TIMEOUT ? HIFLOW_FAIL_RADIO : HIFLOW_FAIL_NO_REPLY,
             "the link died before any data arrived");
        return;
    }

    /* WAIT_LINK: the connection attempt itself failed. */
    fail(s, now_ms,
         reason == HIFLOW_LINK_RADIO_TIMEOUT ? HIFLOW_FAIL_RADIO : HIFLOW_FAIL_NO_CONNECTION,
         "the connection did not come up");
}

void hiflow_session_rx(hiflow_session_t *s, int64_t now_ms, const uint8_t *chunk, size_t len)
{
    int result;

    if (s == NULL)
        return;
    result = hiflow_rx_push(&s->rx, chunk, len);
    if (result == HIFLOW_RX_DROPPED) {
        slog(s, 3, "notification dropped (%u bytes, no frame in progress)", (unsigned) len);
        return;
    }
    if (result != HIFLOW_RX_COMPLETE)
        return;

    handle_frame(s, now_ms, s->rx.buf, s->rx.len);
    hiflow_rx_reset(&s->rx);
    update_status(s, now_ms);
}

void hiflow_session_tx_failed(hiflow_session_t *s, int64_t now_ms)
{
    if (s == NULL)
        return;
    fail(s, now_ms, HIFLOW_FAIL_NO_REPLY, "the transport could not write the frame");
}

void hiflow_session_tick(hiflow_session_t *s, int64_t now_ms)
{
    if (s == NULL)
        return;

    if (s->timer != HT_NONE && now_ms >= s->deadline_ms) {
        uint8_t kind = s->timer;

        s->timer = HT_NONE;
        switch (kind) {
        case HT_REPLY:
            if (in_handshake(s->state)) {
                s->need_v0 = 1;
                fail(s, now_ms, handshake_fail_reason(s), "no reply during the handshake");
            } else if (s->state == HIFLOW_STATE_WAIT_DATA) {
                fail(s, now_ms, HIFLOW_FAIL_NO_REPLY, "no reply to the data request");
            }
            break;
        case HT_SEND_POLL:
            send_status_poll(s, now_ms, s->pending_poll_action);
            break;
        case HT_BACKOFF_END:
            slog(s, 2, "waiting period over, allowing a connection again");
            enter_wait_link(s, now_ms);
            break;
        case HT_DATA_POLL:
            if (s->state == HIFLOW_STATE_READY)
                request_data(s, now_ms);
            break;
        default:
            break;
        }
    }
    update_status(s, now_ms);
}

uint8_t hiflow_session_status(const hiflow_session_t *s) { return s != NULL ? s->status : 0; }
uint8_t hiflow_session_state(const hiflow_session_t *s) { return s != NULL ? s->state : 0; }
uint32_t hiflow_session_failures(const hiflow_session_t *s) { return s != NULL ? s->failures : 0; }
uint32_t hiflow_session_sessions(const hiflow_session_t *s) { return s != NULL ? s->sessions : 0; }

int64_t hiflow_session_unix_time(const hiflow_session_t *s, int64_t now_ms)
{
    return s != NULL ? unix_now(s, now_ms) : 0;
}

const char *hiflow_session_state_name(uint8_t state)
{
    switch (state) {
    case HIFLOW_STATE_BACKOFF:
        return "BACKOFF";
    case HIFLOW_STATE_WAIT_LINK:
        return "WAIT_LINK";
    case HIFLOW_STATE_PAIRING:
        return "PAIRING";
    case HIFLOW_STATE_LOGIN:
        return "LOGIN";
    case HIFLOW_STATE_LOGIN_POLL:
        return "LOGIN_POLL";
    case HIFLOW_STATE_PIN:
        return "PIN";
    case HIFLOW_STATE_PIN_POLL:
        return "PIN_POLL";
    case HIFLOW_STATE_TIMESYNC:
        return "TIMESYNC";
    case HIFLOW_STATE_TIMESYNC_POLL:
        return "TIMESYNC_POLL";
    case HIFLOW_STATE_READY:
        return "READY";
    case HIFLOW_STATE_WAIT_DATA:
        return "WAIT_DATA";
    default:
        return "?";
    }
}
