/*
 * fake_inverter — a HiFlow Pro as far as the session core can tell.
 *
 * It speaks the real protocol: frames are decrypted with the real crypto core,
 * requests are decoded with nanopb, and the replies are built the way the device
 * builds them (including the login acknowledgement's own field layout). Every
 * behaviour the field notes describe can be switched on:
 *
 *   - ask for the BLE PIN, accept or refuse it
 *   - answer the login poll with "in progress" a few times
 *   - kill the link instead of answering, which is what a rotated encRand and a
 *     refused identity both look like
 *   - refuse a login whose timestamp lags the device's clock, which is what the
 *     a power cut in the field looked like
 *   - rotate the session key, and hand out either the new or the stale one on a
 *     V0 pairing
 *   - split replies across notifications of a given size
 *
 * It also counts what it received, so a test can assert that exactly one login
 * went out per connection.
 */
#ifndef FAKE_INVERTER_H
#define FAKE_INVERTER_H

#include <stddef.h>
#include <stdint.h>

#include "hiflow_frame.h"
#include "hiflow_proto.h"

#define FAKE_MAX_REPLY   HIFLOW_MAX_FRAME_LEN
#define FAKE_MAX_PAGES   2
#define FAKE_MAX_PAYLOAD 512

typedef struct {
    /* identity and keys */
    uint8_t enc_rand[HIFLOW_ENC_RAND_LEN];  /* key the device accepts now */
    uint8_t v0_key[HIFLOW_ENC_RAND_LEN];    /* key a V0 pairing hands out */
    char    sn[HIFLOW_SN_LEN + 1];
    char    pin[16];
    int64_t device_time;

    /* behaviour */
    int require_pin;        /* login poll answers sts=3 until the PIN arrives */
    int refuse_pin;         /* the PIN poll answers sts=1 (wrong PIN)         */
    int login_sts;          /* what the login poll reports once settled (1)   */
    int login_in_progress;  /* this many polls answer sts=0 first             */
    int kill_link_on_login; /* drop the link instead of acknowledging a login */
    int kill_link_on_data;  /* drop the link on the next data request         */
    int silent_on_login;    /* swallow the login, keep the link up            */
    int max_login_lag;      /* >0: kill the link on a login this many seconds
                               or more behind device_time                    */
    int v0_without_time;    /* the V0 reply leaves out its timestamp          */
    int silent_on_data;     /* swallow the data request                       */
    int pages;              /* 1 or 2 data pages                              */

    /* data pages the device replies with (raw RealDataNewReqDTO payloads) */
    uint8_t page[FAKE_MAX_PAGES][FAKE_MAX_PAYLOAD];
    size_t  page_len[FAKE_MAX_PAGES];

    /* observations */
    int connections;      /* link_up events                                   */
    int logins_seen;      /* action 64 frames, all connections                */
    int logins_on_link;   /* action 64 frames on the current connection       */
    int max_logins_on_link;
    int login_polls;
    int pin_frames;
    int pin_polls;
    int time_syncs;
    int data_requests;
    int v0_requests;
    int bad_key_frames;   /* frames that did not authenticate                 */
    int pin_ok;
    char last_ble_id[32];
    char last_pin[32];
    char last_time_sync[48];
    int64_t last_login_time;
    uint16_t last_tid;
    int      tid_max;

    /* output of the last request */
    uint8_t reply[FAKE_MAX_REPLY];
    size_t  reply_len;
    int     kill_link;    /* set when the device drops the connection         */
} fake_inverter_t;

void fake_init(fake_inverter_t *fi, const uint8_t enc_rand[HIFLOW_ENC_RAND_LEN],
               const char *sn, const char *pin);

/* A new BLE connection; resets the per-connection counters. */
void fake_link_up(fake_inverter_t *fi);

/* Loads the two pages of a data reply (hex strings from the vectors). */
void fake_set_pages(fake_inverter_t *fi, const char *page0_hex, const char *page1_hex);

/* Feeds one request frame. Returns 1 when fi->reply/reply_len hold an answer,
   0 when the device stays silent. `fi->kill_link` may be set either way. */
int fake_handle_frame(fake_inverter_t *fi, const uint8_t *frame, size_t len);

/* Replaces the accepted key; `v0_hands_out_new` decides whether a following V0
   pairing reports the new key or keeps handing out the old one. */
void fake_rotate_key(fake_inverter_t *fi, const uint8_t new_key[HIFLOW_ENC_RAND_LEN],
                     int v0_hands_out_new);

#endif /* FAKE_INVERTER_H */
