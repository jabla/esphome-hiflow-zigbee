/*
 * hiflow_session — the BLE session with a HiFlow Pro inverter.
 *
 * The state machine that used to live in the ESPHome component, now free of
 * ESPHome, FreeRTOS and BLE APIs so that it can be driven by a test harness.
 * It owns no timers and no threads: the caller feeds it events and a monotonic
 * millisecond clock, and it answers through the callbacks in hiflow_session_ops_t.
 *
 * The rules it follows are the ones the working reference implementation
 * (TheTiEr/ha-hiflow-ble) follows:
 *
 *   * One connection carries exactly one login. Anything that goes wrong ends
 *     the connection instead of logging in again on the same link.
 *   * While waiting after a failure, no connection is held at all
 *     (set_link_allowed(0)), so the inverter's single BLE slot stays free.
 *   * The handshake sequence is action 64 (login) -> optional 82 (PIN) ->
 *     104 (time-sync), with the same poll counts and fallbacks as the reference.
 *   * A failed handshake makes the next connection start with the V0 pairing,
 *     which is the only way to pick up a rotated encRand. Without a key at all
 *     (have_enc_rand = 0) the very first connection starts with it.
 *
 * Timekeeping: every entry point takes `now_ms`, a monotonic uptime in
 * milliseconds as 64-bit (esp_timer_get_time() / 1000 on target). Nothing here
 * wraps after 49.7 days.
 */
#ifndef HIFLOW_SESSION_H
#define HIFLOW_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "hiflow_clock.h"
#include "hiflow_frame.h"
#include "hiflow_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- status codes (kept compatible with the ZHA status sensor) ------- */

#define HIFLOW_STATUS_WAIT_LINK 0 /* no connection (also normal at night) */
#define HIFLOW_STATUS_PAIRING   2
#define HIFLOW_STATUS_LOGIN     3
#define HIFLOW_STATUS_PIN       4
#define HIFLOW_STATUS_TIME_SYNC 5
#define HIFLOW_STATUS_READY     6
#define HIFLOW_STATUS_READING   7
/* While waiting after a failure the status is 8 + reason, i.e. 9..15. */
#define HIFLOW_STATUS_BACKOFF_BASE 8

typedef enum {
    HIFLOW_FAIL_NONE           = 0,
    HIFLOW_FAIL_LOGIN_REJECTED = 1, /* status  9: link died in the handshake   */
    HIFLOW_FAIL_NO_CONNECTION  = 2, /* status 10: no link came up              */
    HIFLOW_FAIL_NO_REPLY       = 3, /* status 11: a request went unanswered    */
    /* 4 (status 12) is unused. */
    HIFLOW_FAIL_PIN            = 5, /* status 13: PIN missing or refused       */
    HIFLOW_FAIL_STALE_KEY      = 6, /* status 14: encRand no longer accepted   */
    HIFLOW_FAIL_RADIO          = 7  /* status 15: supervision timeout          */
} hiflow_fail_reason_t;

/* Link-down reasons as the BLE stack reports them. */
#define HIFLOW_LINK_RADIO_TIMEOUT 0x08 /* supervision timeout                */
#define HIFLOW_LINK_PEER_CLOSED   0x13 /* the inverter terminated the link   */
#define HIFLOW_LINK_LOCAL_CLOSED  0x16 /* we terminated it                   */
#define HIFLOW_LINK_NOT_ESTABLISHED 0x3E

/* ---------- states ---------- */

typedef enum {
    HIFLOW_STATE_BACKOFF = 0, /* waiting, no connection allowed        */
    HIFLOW_STATE_WAIT_LINK,   /* connection allowed, none up yet       */
    HIFLOW_STATE_PAIRING,     /* V0 request sent                       */
    HIFLOW_STATE_LOGIN,       /* action 64 sent                        */
    HIFLOW_STATE_LOGIN_POLL,  /* polling the login status              */
    HIFLOW_STATE_PIN,         /* action 82 sent                        */
    HIFLOW_STATE_PIN_POLL,    /* polling the PIN status                */
    HIFLOW_STATE_TIMESYNC,    /* action 104 sent                       */
    HIFLOW_STATE_TIMESYNC_POLL,
    HIFLOW_STATE_READY,       /* handshake done, waiting for the poll  */
    HIFLOW_STATE_WAIT_DATA    /* data request sent                     */
} hiflow_session_state_t;

/* ---------- configuration ---------- */

#define HIFLOW_BLE_ID_LEN 24
#define HIFLOW_PIN_LEN    16

typedef struct {
    char    sn[HIFLOW_SN_LEN + 1];      /* 12-char serial tail, for V0 */
    char    ble_id[HIFLOW_BLE_ID_LEN];  /* stable identity, never rotated here */
    char    pin[HIFLOW_PIN_LEN];        /* BLE PIN, only sent when asked for   */
    uint8_t enc_rand[HIFLOW_ENC_RAND_LEN];
    int     have_enc_rand;
    int32_t std_offset;                 /* standard UTC offset in seconds      */
    int     eu_dst;                     /* 1 = follow European summer time     */
    uint32_t poll_interval_ms;          /* data cadence, < the ~90 s idle timeout */
    uint32_t request_timeout_ms;        /* no reply -> end the connection      */
    uint32_t status_poll_gap_ms;        /* wait between two status polls       */
    uint32_t backoff_min_ms;
    uint32_t backoff_max_ms;
    uint32_t pin_backoff_ms;            /* after a refused/missing PIN         */
} hiflow_session_config_t;

/* Fills in the defaults that mirror the reference implementation. */
void hiflow_session_config_defaults(hiflow_session_config_t *cfg);

/* ---------- callbacks ---------- */

typedef struct {
    void *ctx;
    /* Writes one frame to the TX characteristic. Returns 1 on success. */
    int (*send)(void *ctx, const uint8_t *frame, size_t len);
    /* Closes the connection cleanly. */
    void (*disconnect)(void *ctx);
    /* Allows or forbids the transport to (re-)connect. */
    void (*set_link_allowed)(void *ctx, int allowed);
    /* One complete set of measurements. */
    void (*on_data)(void *ctx, const hiflow_data_t *data);
    /* A V0 pairing handed out a new encRand (optional; the session keeps it). */
    void (*on_enc_rand)(void *ctx, const uint8_t enc_rand[HIFLOW_ENC_RAND_LEN]);
    /* Status code changed (see HIFLOW_STATUS_*). */
    void (*on_status)(void *ctx, uint8_t status);
    /* Already formatted message; level: 0 error, 1 warn, 2 info, 3 debug. */
    void (*log)(void *ctx, int level, const char *msg);
} hiflow_session_ops_t;

/* ---------- session ---------- */

typedef struct {
    hiflow_session_config_t cfg;
    hiflow_session_ops_t    ops;
    hiflow_clock_t          clock;

    uint8_t  state;
    uint8_t  status;
    uint8_t  fail_reason;
    uint8_t  timer;                /* what deadline_ms is about */
    int64_t  deadline_ms;
    int64_t  link_allowed_since_ms;

    int      link_up;
    int      link_allowed;
    int      handshake_started;    /* at most one handshake per link */
    int      logins_sent;          /* on the current link */
    int      need_v0;              /* next link starts with the V0 pairing */
    int      v0_done_on_link;
    int      key_unchanged;        /* last V0 returned the key we already had */
    int      data_seen;            /* data arrived on the current link */
    uint8_t  decode_failures;      /* consecutive undecodable data replies */
    uint8_t  fail_streak;          /* drives the backoff */
    uint8_t  login_polls;
    uint8_t  pin_polls;
    int32_t  pending_poll_action;
    uint16_t tid;
    uint16_t expected_cmd;         /* reply we are waiting for (0 = none) */
    int32_t  current_page;

    uint32_t failures;             /* failed handshakes since boot */
    uint32_t sessions;             /* successful handshakes since boot */

    hiflow_measurements_t data;
    hiflow_rx_t           rx;
    uint8_t tx_pt[HIFLOW_MAX_REQUEST];
    uint8_t tx_frame[HIFLOW_MAX_FRAME_LEN];
    uint8_t rx_pt[HIFLOW_MAX_PLAINTEXT];
} hiflow_session_t;

/* `build_time` and `persisted_time` seed the clock (see hiflow_clock_init). */
void hiflow_session_init(hiflow_session_t *s, const hiflow_session_config_t *cfg,
                         const hiflow_session_ops_t *ops, int64_t now_ms,
                         int64_t build_time, int64_t persisted_time);

/* The transport reports that notifications are subscribed and the link is
   usable. Starts the handshake. */
void hiflow_session_link_up(hiflow_session_t *s, int64_t now_ms);

/* The connection is gone; `reason` is the BLE reason code (0 if unknown).
   Call this once per connection. */
void hiflow_session_link_down(hiflow_session_t *s, int64_t now_ms, int reason);

/* One BLE notification. */
void hiflow_session_rx(hiflow_session_t *s, int64_t now_ms, const uint8_t *chunk, size_t len);

/* The transport could not write the last frame. */
void hiflow_session_tx_failed(hiflow_session_t *s, int64_t now_ms);

/* Drives timeouts, polls and the backoff. Call it regularly (every loop). */
void hiflow_session_tick(hiflow_session_t *s, int64_t now_ms);

uint8_t  hiflow_session_status(const hiflow_session_t *s);
uint8_t  hiflow_session_state(const hiflow_session_t *s);
uint32_t hiflow_session_failures(const hiflow_session_t *s);
uint32_t hiflow_session_sessions(const hiflow_session_t *s);
int64_t  hiflow_session_unix_time(const hiflow_session_t *s, int64_t now_ms);
const char *hiflow_session_state_name(uint8_t state);

#ifdef __cplusplus
}
#endif

#endif /* HIFLOW_SESSION_H */
