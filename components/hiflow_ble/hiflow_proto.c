#include "hiflow_proto.h"

#include <stdio.h>
#include <string.h>

#include "pb_decode.h"
#include "pb_encode.h"

#include "APPInfomationData.pb.h"
#include "CommCmdPB.pb.h"

#include "hiflow_clock.h"

int hiflow_is_v0_cmd(uint16_t cmd)
{
    return cmd == 0xA201u || cmd == 0xA301u || cmd == 0x8901u || cmd == 0x7901u;
}

/* ---------- request encoders ---------- */

static int encode_msg(uint8_t *buf, size_t cap, size_t *out_len,
                      const pb_msgdesc_t *fields, const void *msg)
{
    pb_ostream_t os;

    if (buf == NULL || out_len == NULL)
        return HIFLOW_ERR_ARG;
    os = pb_ostream_from_buffer(buf, cap);
    if (!pb_encode(&os, fields, msg))
        return HIFLOW_ERR_BUFFER;
    *out_len = os.bytes_written;
    return HIFLOW_OK;
}

int hiflow_encode_comm_cmd(uint8_t *buf, size_t cap, size_t *out_len,
                           int64_t now, int32_t action, const char *data)
{
    CommCmdResDTO req = CommCmdResDTO_init_zero;

    if (data == NULL)
        return HIFLOW_ERR_ARG;
    if (strlen(data) >= sizeof(req.data))
        return HIFLOW_ERR_BUFFER;

    req.time = now;
    req.action = action;
    req.tid = now; /* the reference echoes the timestamp as the transaction id */
    strncpy(req.data, data, sizeof(req.data) - 1);
    return encode_msg(buf, cap, out_len, CommCmdResDTO_fields, &req);
}

int hiflow_encode_time_sync(uint8_t *buf, size_t cap, size_t *out_len,
                            int64_t now, int32_t offset)
{
    char data[32];

    snprintf(data, sizeof(data), "%lld,%d\r", (long long) now, (int) offset);
    return hiflow_encode_comm_cmd(buf, cap, out_len, now, HIFLOW_ACTION_TIME_SYNC, data);
}

int hiflow_encode_status_poll(uint8_t *buf, size_t cap, size_t *out_len,
                              int64_t now, int32_t action)
{
    CommCmdStatusResDTO req = CommCmdStatusResDTO_init_zero;

    req.time = now;
    req.action = action;
    req.tid = now;
    return encode_msg(buf, cap, out_len, CommCmdStatusResDTO_fields, &req);
}

/* nanopb callback for the one bytes field that has no static storage. */
typedef struct {
    const uint8_t *data;
    size_t len;
} bytes_arg_t;

static bool encode_bytes_cb(pb_ostream_t *stream, const pb_field_t *field, void *const *arg)
{
    const bytes_arg_t *a = (const bytes_arg_t *) *arg;

    if (a == NULL || a->data == NULL || a->len == 0)
        return true; /* leave the optional field out */
    return pb_encode_tag_for_field(stream, field) && pb_encode_string(stream, a->data, a->len);
}

int hiflow_encode_real_data_request(uint8_t *buf, size_t cap, size_t *out_len,
                                    int64_t now, int32_t offset, int32_t cp)
{
    RealDataNewResDTO req = RealDataNewResDTO_init_zero;
    char ymd[HIFLOW_TIME_STR_LEN];
    bytes_arg_t arg;

    if (hiflow_format_local_time(now, offset, ymd, sizeof(ymd)) == 0)
        return HIFLOW_ERR_BUFFER;

    arg.data = (const uint8_t *) ymd;
    arg.len = strlen(ymd);
    req.time_ymd_hms.funcs.encode = &encode_bytes_cb;
    req.time_ymd_hms.arg = &arg;
    req.cp = cp;
    req.offset = offset;
    req.time = (int32_t) now;
    req.error_code = 0;
    return encode_msg(buf, cap, out_len, RealDataNewResDTO_fields, &req);
}

int hiflow_encode_app_info_v0(uint8_t *buf, size_t cap, size_t *out_len,
                              int64_t now, int32_t offset)
{
    APPInfoDataResDTO req = APPInfoDataResDTO_init_zero;
    char ymd[HIFLOW_TIME_STR_LEN];
    size_t ymd_len;

    ymd_len = hiflow_format_local_time(now, offset, ymd, sizeof(ymd));
    if (ymd_len == 0)
        return HIFLOW_ERR_BUFFER;
    if (ymd_len > sizeof(req.time_ymd_hms.bytes))
        ymd_len = sizeof(req.time_ymd_hms.bytes);

    req.time_ymd_hms.size = (pb_size_t) ymd_len;
    memcpy(req.time_ymd_hms.bytes, ymd, ymd_len);
    req.offset = offset;
    req.time = (uint32_t) now;
    return encode_msg(buf, cap, out_len, APPInfoDataResDTO_fields, &req);
}

/* ---------- reply decoders ---------- */

int hiflow_decode_status_reply(const uint8_t *pt, size_t len,
                               int32_t *action, int32_t *sts)
{
    CommCmdStatusReqDTO msg = CommCmdStatusReqDTO_init_zero;
    pb_istream_t is;

    if (action != NULL)
        *action = 0;
    if (sts != NULL)
        *sts = HIFLOW_STS_IN_PROGRESS;
    if (pt == NULL)
        return HIFLOW_ERR_ARG;

    is = pb_istream_from_buffer(pt, len);
    if (!pb_decode(&is, CommCmdStatusReqDTO_fields, &msg))
        return HIFLOW_ERR_FIELD;
    if (action != NULL)
        *action = msg.action;
    if (sts != NULL)
        *sts = msg.sts;
    return HIFLOW_OK;
}

/* Reads one varint; returns 0 when the buffer ends inside it. */
static int read_varint(const uint8_t **p, const uint8_t *end, uint64_t *out)
{
    uint64_t value = 0;
    unsigned shift = 0;

    while (*p < end && shift < 64) {
        uint8_t byte = *(*p)++;
        value |= (uint64_t) (byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) {
            *out = value;
            return 1;
        }
        shift += 7;
    }
    return 0;
}

int64_t hiflow_decode_login_ack_time(const uint8_t *pt, size_t len)
{
    const uint8_t *p = pt;
    const uint8_t *end = pt + len;

    if (pt == NULL)
        return 0;

    /* Measured layout: field 1 is a large counter, field 2 the device time in
       unix seconds, field 3 echoes the action. */
    while (p < end) {
        uint64_t tag = 0, value = 0;
        uint32_t field, wire;

        if (!read_varint(&p, end, &tag))
            break;
        field = (uint32_t) (tag >> 3);
        wire = (uint32_t) (tag & 0x07u);
        switch (wire) {
        case 0: /* varint */
            if (!read_varint(&p, end, &value))
                return 0;
            if (field == 2 && (int64_t) value > HIFLOW_TIME_MIN && (int64_t) value < HIFLOW_TIME_MAX)
                return (int64_t) value;
            break;
        case 2: /* length-delimited */
            if (!read_varint(&p, end, &value) || (uint64_t) (end - p) < value)
                return 0;
            p += value;
            break;
        case 5: /* 32 bit */
            if (end - p < 4)
                return 0;
            p += 4;
            break;
        case 1: /* 64 bit */
            if (end - p < 8)
                return 0;
            p += 8;
            break;
        default:
            return 0;
        }
    }
    return 0;
}

/* ---------- measurements ---------- */

void hiflow_measurements_reset(hiflow_measurements_t *acc)
{
    if (acc == NULL)
        return;
    memset(acc, 0, sizeof(*acc));
}

static hiflow_port_t *port_slot(hiflow_measurements_t *acc, int32_t port_number, size_t index)
{
    size_t i;

    /* Ports are addressed by their port_number; a page that carries only ports
       3 and 4 must not overwrite the slots of ports 1 and 2. */
    if (port_number >= 1 && port_number <= HIFLOW_MAX_PORTS)
        return &acc->ports[port_number - 1];

    /* Devices that leave port_number at 0: fall back to the first free slot,
       keeping the order in which the ports arrived. */
    for (i = index; i < HIFLOW_MAX_PORTS; i++) {
        if (!acc->ports[i].present)
            return &acc->ports[i];
    }
    return NULL;
}

int hiflow_merge_real_data(hiflow_measurements_t *acc, const uint8_t *pt, size_t len,
                           int32_t *ap, int32_t *cp)
{
    pb_istream_t is;
    pb_size_t i;

    if (acc == NULL || pt == NULL)
        return HIFLOW_ERR_ARG;

    memset(&acc->page, 0, sizeof(acc->page));
    is = pb_istream_from_buffer(pt, len);
    if (!pb_decode(&is, RealDataNewReqDTO_fields, &acc->page))
        return HIFLOW_ERR_FIELD;

    if (ap != NULL)
        *ap = acc->page.ap;
    if (cp != NULL)
        *cp = acc->page.cp;

    acc->page_count = acc->page.ap > 0 ? acc->page.ap : 1;
    acc->last_page = acc->page.cp;
    acc->pages_merged++;

    /* Grid side: single-phase installs report via SGSMO, three-phase via TGSMO.
       Only the first block that carries values is taken. */
    if (!acc->have_ac && acc->page.sgs_data_count > 0) {
        const SGSMO *sgs = &acc->page.sgs_data[0];
        acc->have_ac = 1;
        acc->ac_power = sgs->active_power;
        acc->ac_voltage = sgs->voltage;
        acc->ac_current = sgs->current;
        acc->ac_frequency = sgs->frequency;
        acc->ac_temperature = sgs->temperature;
    } else if (!acc->have_ac && acc->page.tgs_data_count > 0) {
        const TGSMO *tgs = &acc->page.tgs_data[0];
        acc->have_ac = 1;
        acc->ac_power = tgs->active_power;
        acc->ac_voltage = tgs->voltage_phase_A;
        acc->ac_current = tgs->current_phase_A;
        acc->ac_frequency = tgs->frequency;
        acc->ac_temperature = tgs->temperature;
    }

    for (i = 0; i < acc->page.pv_data_count; i++) {
        const PvMO *pv = &acc->page.pv_data[i];
        hiflow_port_t *slot = port_slot(acc, pv->port_number, i);

        if (slot == NULL)
            continue; /* more ports than this board has */
        slot->present = 1;
        slot->port_number = pv->port_number;
        slot->voltage = pv->voltage;
        slot->current = pv->current;
        slot->power = pv->power;
        slot->energy_total = pv->energy_total;
        slot->energy_daily = pv->energy_daily;
        slot->error_code = pv->error_code;
    }
    return HIFLOW_OK;
}

int hiflow_measurements_complete(const hiflow_measurements_t *acc)
{
    if (acc == NULL || acc->pages_merged == 0)
        return 0;
    return acc->last_page >= acc->page_count - 1;
}

void hiflow_measurements_to_data(const hiflow_measurements_t *acc, hiflow_data_t *out)
{
    int64_t energy_total = 0, energy_daily = 0;
    size_t i;

    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    if (acc == NULL)
        return;

    out->have_ac = acc->have_ac;
    out->ac_power_w = (float) acc->ac_power * 0.1f;
    out->ac_voltage_v = (float) acc->ac_voltage * 0.1f;
    out->ac_current_a = (float) acc->ac_current * 0.01f;
    out->ac_frequency_hz = (float) acc->ac_frequency * 0.01f;
    out->temperature_c = (float) acc->ac_temperature * 0.1f;

    for (i = 0; i < HIFLOW_MAX_PORTS; i++) {
        const hiflow_port_t *port = &acc->ports[i];

        out->ports[i].present = port->present;
        if (!port->present)
            continue;
        out->port_count++;
        out->ports[i].port_number = port->port_number;
        out->ports[i].power_w = (float) port->power * 0.1f;
        out->ports[i].voltage_v = (float) port->voltage * 0.1f;
        out->ports[i].current_a = (float) port->current * 0.01f;
        energy_total += port->energy_total;
        energy_daily += port->energy_daily;
    }
    out->energy_total_wh = (float) energy_total;
    out->energy_daily_wh = (float) energy_daily;
}

/* ---------- notification reassembly ---------- */

void hiflow_rx_reset(hiflow_rx_t *rx)
{
    if (rx == NULL)
        return;
    rx->len = 0;
    rx->expected = 0;
}

uint16_t hiflow_rx_cmd(const hiflow_rx_t *rx)
{
    if (rx == NULL || rx->len < HIFLOW_HEADER_LEN)
        return 0;
    return (uint16_t) (((uint16_t) rx->buf[2] << 8) | rx->buf[3]);
}

int hiflow_rx_push(hiflow_rx_t *rx, const uint8_t *chunk, size_t len)
{
    if (rx == NULL || chunk == NULL)
        return HIFLOW_RX_DROPPED;
    if (len == 0)
        return HIFLOW_RX_NEED_MORE;

    /* A frame always starts with "HM"; anything else is a stray notification. */
    if (rx->len == 0 && (len < 2 || chunk[0] != HIFLOW_MAGIC0 || chunk[1] != HIFLOW_MAGIC1)) {
        hiflow_rx_reset(rx);
        return HIFLOW_RX_DROPPED;
    }
    if (rx->len + len > sizeof(rx->buf)) {
        hiflow_rx_reset(rx);
        return HIFLOW_RX_DROPPED;
    }

    memcpy(rx->buf + rx->len, chunk, len);
    rx->len += len;

    if (rx->expected == 0) {
        uint16_t length, cmd;

        if (rx->len < HIFLOW_HEADER_LEN)
            return HIFLOW_RX_NEED_MORE; /* length field not in yet */
        length = (uint16_t) (((uint16_t) rx->buf[8] << 8) | rx->buf[9]);
        cmd = hiflow_rx_cmd(rx);
        if (length < HIFLOW_HEADER_LEN) {
            hiflow_rx_reset(rx);
            return HIFLOW_RX_DROPPED;
        }
        /* `length` covers header + ciphertext; V1 frames carry the tag on top. */
        rx->expected = hiflow_is_v0_cmd(cmd) ? (size_t) length : (size_t) length + HIFLOW_TAG_LEN;
        if (rx->expected > sizeof(rx->buf)) {
            hiflow_rx_reset(rx);
            return HIFLOW_RX_DROPPED;
        }
    }

    if (rx->len < rx->expected)
        return HIFLOW_RX_NEED_MORE;

    /* Trailing bytes would belong to a frame we never asked for. */
    rx->len = rx->expected;
    return HIFLOW_RX_COMPLETE;
}
