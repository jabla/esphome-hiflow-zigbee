/* Flat C interface to fake_inverter.c for the BLE peripheral in fake_ble.py. */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "fake_inverter.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "RealDataNew.pb.h"

/* The fake plus the recorded pages, so a power override can be undone. The
   fake comes first: every other call takes the pointer as the fake. */
typedef struct {
    fake_inverter_t fi;
    uint8_t recorded[FAKE_MAX_PAGES][FAKE_MAX_PAYLOAD];
    size_t recorded_len[FAKE_MAX_PAGES];
    int rotate; /* a new key with every V0 pairing */
} shim_t;

void *shim_new(const uint8_t *key, const char *sn, const char *pin, const char *page0, const char *page1) {
    shim_t *s = calloc(1, sizeof(*s));
    fake_init(&s->fi, key, sn, pin);
    fake_set_pages(&s->fi, page0, page1 && page1[0] ? page1 : NULL);
    memcpy(s->recorded, s->fi.page, sizeof(s->recorded));
    memcpy(s->recorded_len, s->fi.page_len, sizeof(s->recorded_len));
    /* Different keys on every start: a restart (the bench night) must not
       hand out the key the bridge already holds. */
    srand((unsigned) time(NULL) ^ (unsigned) getpid());
    return s;
}
void shim_link_up(void *p) { fake_link_up((fake_inverter_t *) p); }
void shim_set_time(void *p, int64_t t) { ((fake_inverter_t *) p)->device_time = t; }
/* Scenario switches (bench): kill every login, a new key with every V0,
   refuse requests (not the V0) this far ahead of the device clock or behind
   it (0 = off). */
void shim_set_mode(void *p, int refuse, int rotate, int max_lead, int max_lag) {
    shim_t *s = p;
    s->fi.kill_link_on_login = refuse;
    s->fi.max_lead = max_lead;
    s->fi.max_lag = max_lag;
    s->rotate = rotate;
}
/* The last login's timestamp minus the device time back then. */
int64_t shim_last_login_lead(void *p) { return ((fake_inverter_t *) p)->last_login_lead; }
/* Returns the reply length (0 = silent); *kill is set when the device drops the link. */
int shim_frame(void *p, const uint8_t *in, size_t len, uint8_t *out, int *kill) {
    shim_t *s = p;
    fake_inverter_t *fi = &s->fi;
    uint8_t old_key[HIFLOW_ENC_RAND_LEN], old_v0_key[HIFLOW_ENC_RAND_LEN];
    int rotated = 0;
    fi->kill_link = 0;
    if (s->rotate && len >= HIFLOW_HEADER_LEN && hiflow_is_v0_cmd((uint16_t) ((in[2] << 8) | in[3]))) {
        uint8_t key[HIFLOW_ENC_RAND_LEN];
        memcpy(old_key, fi->enc_rand, sizeof(old_key));
        memcpy(old_v0_key, fi->v0_key, sizeof(old_v0_key));
        for (size_t i = 0; i < sizeof(key); i++)
            key[i] = (uint8_t) rand();
        fake_rotate_key(fi, key, 1);
        rotated = 1;
    }
    int n = 0;
    if (fake_handle_frame(fi, in, len)) {
        memcpy(out, fi->reply, fi->reply_len);
        n = (int) fi->reply_len;
    } else if (rotated) {
        /* A V0 frame the fake did not take: no new key either. */
        memcpy(fi->enc_rand, old_key, sizeof(old_key));
        memcpy(fi->v0_key, old_v0_key, sizeof(old_v0_key));
    }
    *kill = fi->kill_link;
    return n;
}
void shim_counts(void *p, int *c) {
    fake_inverter_t *fi = p;
    c[0] = fi->connections; c[1] = fi->logins_seen; c[2] = fi->v0_requests; c[3] = fi->data_requests;
    c[4] = fi->bad_key_frames; c[5] = fi->time_syncs;
}

/* The recorded pages with only the power changed: the inverter's AC power
   and each PV port's share of it. Serial, energies and everything else stay
   as recorded, so the bridge sees the same inverter and its totals go on.
   Returns 0 on success, -1 if a page does not decode or encode. */
int shim_set_power(void *p, int32_t ac_tenths_w) {
    shim_t *s = p;
    static RealDataNewReqDTO m[FAKE_MAX_PAGES];
    int ports = 0;
    for (int k = 0; k < s->fi.pages; k++) {
        memset(&m[k], 0, sizeof(m[k]));
        pb_istream_t is = pb_istream_from_buffer(s->recorded[k], s->recorded_len[k]);
        if (!pb_decode(&is, RealDataNewReqDTO_fields, &m[k]))
            return -1;
        ports += m[k].pv_data_count;
    }
    for (int k = 0; k < s->fi.pages; k++) {
        for (pb_size_t i = 0; i < m[k].sgs_data_count; i++)
            m[k].sgs_data[i].active_power = ac_tenths_w;
        for (pb_size_t i = 0; i < m[k].pv_data_count; i++)
            m[k].pv_data[i].power = ports ? ac_tenths_w / ports : 0;
        pb_ostream_t os = pb_ostream_from_buffer(s->fi.page[k], sizeof(s->fi.page[k]));
        if (!pb_encode(&os, RealDataNewReqDTO_fields, &m[k]))
            return -1;
        s->fi.page_len[k] = os.bytes_written;
    }
    return 0;
}

/* Back to the recorded pages. */
void shim_reset_pages(void *p) {
    shim_t *s = p;
    memcpy(s->fi.page, s->recorded, sizeof(s->recorded));
    memcpy(s->fi.page_len, s->recorded_len, sizeof(s->recorded_len));
}
