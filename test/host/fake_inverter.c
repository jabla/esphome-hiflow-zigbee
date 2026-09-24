#include "fake_inverter.h"

#include <stdio.h>
#include <string.h>

#include "pb_decode.h"

#include "CommCmdPB.pb.h"
#include "RealDataNew.pb.h"

/* ---------- small protobuf writers (the device's own layouts) ---------- */

static size_t put_varint(uint8_t *out, uint64_t value)
{
    size_t n = 0;

    while (value > 0x7F) {
        out[n++] = (uint8_t) ((value & 0x7F) | 0x80);
        value >>= 7;
    }
    out[n++] = (uint8_t) value;
    return n;
}

static size_t put_varint_field(uint8_t *out, int field, uint64_t value)
{
    size_t n = put_varint(out, (uint64_t) field << 3);
    return n + put_varint(out + n, value);
}

static size_t put_bytes_field(uint8_t *out, int field, const uint8_t *data, size_t len)
{
    size_t n = put_varint(out, ((uint64_t) field << 3) | 2);
    n += put_varint(out + n, len);
    memcpy(out + n, data, len);
    return n + len;
}

static size_t unhex_into(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = strlen(hex) / 2, i;

    if (n > cap)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned int byte = 0;
        if (sscanf(hex + 2 * i, "%2x", &byte) != 1)
            return 0;
        out[i] = (uint8_t) byte;
    }
    return n;
}

/* ---------- setup ---------- */

void fake_init(fake_inverter_t *fi, const uint8_t enc_rand[HIFLOW_ENC_RAND_LEN],
               const char *sn, const char *pin)
{
    memset(fi, 0, sizeof(*fi));
    memcpy(fi->enc_rand, enc_rand, HIFLOW_ENC_RAND_LEN);
    memcpy(fi->v0_key, enc_rand, HIFLOW_ENC_RAND_LEN);
    snprintf(fi->sn, sizeof(fi->sn), "%s", sn);
    snprintf(fi->pin, sizeof(fi->pin), "%s", pin != NULL ? pin : "");
    fi->login_sts = 1;
    fi->pages = 1;
    fi->device_time = 1774000000;
}

void fake_link_up(fake_inverter_t *fi)
{
    fi->connections++;
    fi->logins_on_link = 0;
    fi->kill_link = 0;
}

void fake_set_pages(fake_inverter_t *fi, const char *page0_hex, const char *page1_hex)
{
    fi->page_len[0] = unhex_into(page0_hex, fi->page[0], sizeof(fi->page[0]));
    fi->page_len[1] = page1_hex != NULL ? unhex_into(page1_hex, fi->page[1], sizeof(fi->page[1])) : 0;
    fi->pages = page1_hex != NULL ? 2 : 1;
}

void fake_rotate_key(fake_inverter_t *fi, const uint8_t new_key[HIFLOW_ENC_RAND_LEN],
                     int v0_hands_out_new)
{
    memcpy(fi->enc_rand, new_key, HIFLOW_ENC_RAND_LEN);
    if (v0_hands_out_new)
        memcpy(fi->v0_key, new_key, HIFLOW_ENC_RAND_LEN);
    /* else: the V0 pairing keeps handing out the stale record, which is what
       the field notes describe for a peer the inverter already knows. */
}

/* ---------- replies ---------- */

static int reply_v1(fake_inverter_t *fi, uint16_t cmd, uint16_t tid, const uint8_t *pt, size_t len)
{
    size_t frame_len = 0;

    if (hiflow_build_frame_v1(fi->enc_rand, cmd, tid, pt, len, fi->reply, sizeof(fi->reply),
                              &frame_len) != HIFLOW_OK)
        return 0;
    fi->reply_len = frame_len;
    return 1;
}

static int reply_v0(fake_inverter_t *fi, uint16_t cmd, uint16_t tid, const uint8_t *pt, size_t len)
{
    size_t frame_len = 0;

    if (hiflow_build_frame_v0(fi->sn, cmd, tid, pt, len, fi->reply, sizeof(fi->reply),
                              &frame_len) != HIFLOW_OK)
        return 0;
    fi->reply_len = frame_len;
    return 1;
}

/* APPInfoDataReqDTO { field 2: timestamp, field 8: MAPPDtuInfo { field 27: encRand } } */
static int reply_v0_pairing(fake_inverter_t *fi, uint16_t tid)
{
    uint8_t inner[32];
    uint8_t outer[64];
    size_t inner_len, outer_len = 0;

    inner_len = put_bytes_field(inner, 27, fi->v0_key, HIFLOW_ENC_RAND_LEN);
    if (!fi->v0_without_time)
        outer_len += put_varint_field(outer, 2, (uint64_t) fi->device_time);
    outer_len += put_bytes_field(outer + outer_len, 8, inner, inner_len);
    return reply_v0(fi, 0xA201, tid, outer, outer_len);
}

/* The acknowledgement has its own layout: field 1 is a counter, field 2 the
   device time, field 3 echoes the action. */
static int reply_comm_cmd_ack(fake_inverter_t *fi, uint16_t tid, int32_t action)
{
    uint8_t pt[32];
    size_t len = 0;

    len += put_varint_field(pt + len, 1, 2199023255552ull);
    len += put_varint_field(pt + len, 2, (uint64_t) fi->device_time);
    len += put_varint_field(pt + len, 3, (uint64_t) action);
    return reply_v1(fi, 0xA218, tid, pt, len);
}

/* CommCmdStatusReqDTO { 3: action, 11: sts } */
static int reply_status(fake_inverter_t *fi, uint16_t tid, int32_t action, int32_t sts)
{
    uint8_t pt[16];
    size_t len = 0;

    len += put_varint_field(pt + len, 3, (uint64_t) action);
    len += put_varint_field(pt + len, 11, (uint64_t) sts);
    return reply_v1(fi, 0xA219, tid, pt, len);
}

/* ---------- request handling ---------- */

static int handle_comm_cmd(fake_inverter_t *fi, uint16_t tid, const uint8_t *pt, size_t len)
{
    CommCmdResDTO req = CommCmdResDTO_init_zero;
    pb_istream_t is = pb_istream_from_buffer(pt, len);

    if (!pb_decode(&is, CommCmdResDTO_fields, &req))
        return 0;

    switch (req.action) {
    case HIFLOW_ACTION_LOGIN:
        fi->logins_seen++;
        fi->logins_on_link++;
        if (fi->logins_on_link > fi->max_logins_on_link)
            fi->max_logins_on_link = fi->logins_on_link;
        fi->last_login_time = req.time;
        snprintf(fi->last_ble_id, sizeof(fi->last_ble_id), "%s", req.data);
        if (fi->kill_link_on_login ||
            (fi->max_login_lag > 0 && req.time <= fi->device_time - fi->max_login_lag)) {
            fi->kill_link = 1;
            return 0;
        }
        if (fi->silent_on_login)
            return 0;
        return reply_comm_cmd_ack(fi, tid, req.action);
    case HIFLOW_ACTION_PIN:
        fi->pin_frames++;
        snprintf(fi->last_pin, sizeof(fi->last_pin), "%s", req.data);
        if (!fi->refuse_pin && strcmp(req.data, fi->pin) == 0)
            fi->pin_ok = 1;
        return reply_comm_cmd_ack(fi, tid, req.action);
    case HIFLOW_ACTION_TIME_SYNC:
        fi->time_syncs++;
        snprintf(fi->last_time_sync, sizeof(fi->last_time_sync), "%s", req.data);
        return reply_comm_cmd_ack(fi, tid, req.action);
    default:
        return 0;
    }
}

static int handle_status_poll(fake_inverter_t *fi, uint16_t tid, const uint8_t *pt, size_t len)
{
    CommCmdStatusResDTO req = CommCmdStatusResDTO_init_zero;
    pb_istream_t is = pb_istream_from_buffer(pt, len);
    int32_t sts;

    if (!pb_decode(&is, CommCmdStatusResDTO_fields, &req))
        return 0;

    switch (req.action) {
    case HIFLOW_ACTION_LOGIN:
        fi->login_polls++;
        if (fi->require_pin && !fi->pin_ok)
            sts = HIFLOW_STS_PIN_REQUIRED;
        else if (fi->login_in_progress > 0) {
            fi->login_in_progress--;
            sts = HIFLOW_STS_IN_PROGRESS;
        } else {
            sts = fi->login_sts;
        }
        break;
    case HIFLOW_ACTION_PIN:
        fi->pin_polls++;
        sts = fi->pin_ok ? HIFLOW_STS_PIN_OK : HIFLOW_STS_PIN_WRONG;
        break;
    default:
        sts = 0;
        break;
    }
    return reply_status(fi, tid, req.action, sts);
}

static int handle_data_request(fake_inverter_t *fi, uint16_t tid, const uint8_t *pt, size_t len)
{
    RealDataNewResDTO req = RealDataNewResDTO_init_zero;
    pb_istream_t is = pb_istream_from_buffer(pt, len);
    int32_t cp;

    fi->data_requests++;
    if (fi->kill_link_on_data) {
        fi->kill_link = 1;
        return 0;
    }
    if (fi->silent_on_data)
        return 0;
    if (!pb_decode(&is, RealDataNewResDTO_fields, &req))
        return 0;

    cp = req.cp;
    if (cp < 0 || cp >= fi->pages || fi->page_len[cp] == 0)
        return 0;
    return reply_v1(fi, 0xA211, tid, fi->page[cp], fi->page_len[cp]);
}

int fake_handle_frame(fake_inverter_t *fi, const uint8_t *frame, size_t len)
{
    uint8_t pt[FAKE_MAX_PAYLOAD];
    uint16_t cmd = 0, tid = 0;
    size_t pt_len = 0;
    int rc;

    fi->reply_len = 0;
    if (len < HIFLOW_HEADER_LEN)
        return 0;
    cmd = (uint16_t) (((uint16_t) frame[2] << 8) | frame[3]);

    if (hiflow_is_v0_cmd(cmd)) {
        rc = hiflow_parse_frame_v0(fi->sn, frame, len, &cmd, &tid, pt, sizeof(pt), &pt_len);
        if (rc != HIFLOW_OK)
            return 0;
        fi->v0_requests++;
        fi->last_tid = tid;
        if (tid > fi->tid_max)
            fi->tid_max = tid;
        return reply_v0_pairing(fi, tid);
    }

    rc = hiflow_parse_frame_v1(fi->enc_rand, frame, len, &cmd, &tid, pt, sizeof(pt), &pt_len);
    if (rc != HIFLOW_OK) {
        /* A frame the device cannot decrypt is not answered with an error: the
           link is simply killed. That is why a stale key and a blocked identity
           look identical from the outside. */
        fi->bad_key_frames++;
        fi->kill_link = 1;
        return 0;
    }

    fi->last_tid = tid;
    if (tid > fi->tid_max)
        fi->tid_max = tid;

    switch (cmd) {
    case HIFLOW_CMD_COMM_CMD:
        return handle_comm_cmd(fi, tid, pt, pt_len);
    case HIFLOW_CMD_COMM_STATUS:
        return handle_status_poll(fi, tid, pt, pt_len);
    case HIFLOW_CMD_REAL_DATA:
        return handle_data_request(fi, tid, pt, pt_len);
    default:
        return 0;
    }
}
