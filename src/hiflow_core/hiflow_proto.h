/*
 * hiflow_proto — payloads and notification reassembly of the HiFlow BLE protocol.
 *
 * This is the layer between the frame/crypto core (hiflow_frame.h) and the
 * session state machine (hiflow_session.h):
 *
 *   * encoders for every request the bridge sends (CommCmd login / PIN /
 *     time-sync, the status polls, the RealDataNew data request and the V0
 *     APPInfo pairing request), byte-compatible with the reference library;
 *   * decoders for the replies, including the hand-decoded device time in the
 *     login acknowledgement, whose field layout differs from the request;
 *   * an accumulator that merges the pages of a RealDataNew reply and maps the
 *     ports by their port_number instead of their position;
 *   * reassembly of one frame from BLE notifications.
 *
 * All functions return a hiflow_status_t code; nothing here allocates.
 */
#ifndef HIFLOW_PROTO_H
#define HIFLOW_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "hiflow_frame.h"
#include "RealDataNew.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- command codes (see the component README) ---------- */

#define HIFLOW_CMD_COMM_CMD     0xA318u /* CommCmdResDTO: login, PIN, time-sync */
#define HIFLOW_CMD_COMM_STATUS  0xA319u /* CommCmdStatusResDTO: poll an action  */
#define HIFLOW_CMD_REAL_DATA    0xA311u /* RealDataNew request (paged)          */
#define HIFLOW_CMD_HEARTBEAT    0xA302u /* keeps an idle link warm              */
#define HIFLOW_CMD_APP_INFO_V0  0xA301u /* V0 pairing, answered on 0xA201       */

/* The device answers on the request command minus 0x0100. */
#define HIFLOW_REPLY_CMD(cmd) ((uint16_t) ((cmd) -0x0100u))

/* ---------- handshake actions and their status codes ---------- */

#define HIFLOW_ACTION_LOGIN      64
#define HIFLOW_ACTION_PIN        82
#define HIFLOW_ACTION_TIME_SYNC 104

/* The status is per action: action 64 reports 1 for "logged in", action 82
   reports 0 for "PIN accepted". That asymmetry is in the protocol. */
#define HIFLOW_STS_IN_PROGRESS   0
#define HIFLOW_STS_LOGIN_OK      1
#define HIFLOW_STS_PIN_REQUIRED  3
#define HIFLOW_STS_PIN_OK        0
#define HIFLOW_STS_PIN_WRONG     1

/* Largest request payload the encoders produce. */
#define HIFLOW_MAX_REQUEST 128

/* Ports of a HiFlow Pro 4WB. */
#define HIFLOW_MAX_PORTS 4

/* A whole frame: header + ciphertext + GCM tag. */
#define HIFLOW_MAX_FRAME_LEN (HIFLOW_HEADER_LEN + HIFLOW_MAX_PLAINTEXT + HIFLOW_TAG_LEN)

/* 1 for the commands that travel on the V0 (SN-keyed CBC, no tag) path. */
int hiflow_is_v0_cmd(uint16_t cmd);

/* ---------- request encoders ---------- */

/* CommCmdResDTO{time, action, tid=time, data} — login (data = bleId), PIN
   (data = PIN) and time-sync (data = "<unix>,<offset>\r"). */
int hiflow_encode_comm_cmd(uint8_t *buf, size_t cap, size_t *out_len,
                           int64_t now, int32_t action, const char *data);

/* Same, but builds the time-sync payload string itself. */
int hiflow_encode_time_sync(uint8_t *buf, size_t cap, size_t *out_len,
                            int64_t now, int32_t offset);

/* CommCmdStatusResDTO{time, action, tid=time} — polls the state of an action. */
int hiflow_encode_status_poll(uint8_t *buf, size_t cap, size_t *out_len,
                              int64_t now, int32_t action);

/* RealDataNewResDTO{time_ymd_hms, cp, offset, time} — one data page. */
int hiflow_encode_real_data_request(uint8_t *buf, size_t cap, size_t *out_len,
                                    int64_t now, int32_t offset, int32_t cp);

/* APPInfoDataResDTO{time_ymd_hms, offset, time} — the V0 pairing request whose
   reply carries a fresh encRand. */
int hiflow_encode_app_info_v0(uint8_t *buf, size_t cap, size_t *out_len,
                              int64_t now, int32_t offset);

/* ---------- reply decoders ---------- */

/* CommCmdStatusReqDTO{3: action, 11: sts}. A payload that does not decode is
   reported as action 0 / sts 0, i.e. "still in progress". */
int hiflow_decode_status_reply(const uint8_t *pt, size_t len,
                               int32_t *action, int32_t *sts);

/* Device time out of the login acknowledgement (field 2), 0 when the payload
   carries none. The acknowledgement has its own layout — decoding it with the
   request layout yields nonsense such as time = -1607349313, which is why the
   varints are walked by hand and only plausible values are returned. */
int64_t hiflow_decode_login_ack_time(const uint8_t *pt, size_t len);

/* ---------- measurements ---------- */

typedef struct {
    int      present;
    int32_t  port_number;
    int32_t  voltage;      /* x0.1 V  */
    int32_t  current;      /* x0.01 A */
    int32_t  power;        /* x0.1 W  */
    int32_t  energy_total; /* Wh      */
    int32_t  energy_daily; /* Wh      */
    int32_t  error_code;
} hiflow_port_t;

/* Accumulator for the pages of one RealDataNew round. The nanopb message is
   part of it so that decoding never needs a ~4 kB stack frame. */
typedef struct {
    int             have_ac;
    int32_t         ac_power;       /* x0.1 W  */
    int32_t         ac_voltage;     /* x0.1 V  */
    int32_t         ac_current;     /* x0.01 A */
    int32_t         ac_frequency;   /* x0.01 Hz */
    int32_t         ac_temperature; /* x0.1 C  */
    hiflow_port_t   ports[HIFLOW_MAX_PORTS];
    int32_t         page_count;     /* ap of the last page  */
    int32_t         last_page;      /* cp of the last page  */
    int             pages_merged;
    RealDataNewReqDTO page;         /* scratch for the nanopb decode */
} hiflow_measurements_t;

/* Physical values, ready to publish. */
typedef struct {
    int   have_ac;
    float ac_power_w;
    float ac_voltage_v;
    float ac_current_a;
    float ac_frequency_hz;
    float temperature_c;
    struct {
        int   present;
        int32_t port_number;
        float power_w;
        float voltage_v;
        float current_a;
    } ports[HIFLOW_MAX_PORTS];
    float energy_total_wh;
    float energy_daily_wh;
    int   port_count;
} hiflow_data_t;

void hiflow_measurements_reset(hiflow_measurements_t *acc);

/* Decodes one page and merges it into `acc`. `ap` and `cp` receive the page
   counters of that page (both may be NULL). */
int hiflow_merge_real_data(hiflow_measurements_t *acc, const uint8_t *pt,
                           size_t len, int32_t *ap, int32_t *cp);

/* 1 when every page of the round has been merged. */
int hiflow_measurements_complete(const hiflow_measurements_t *acc);

/* Converts the accumulator into physical values. */
void hiflow_measurements_to_data(const hiflow_measurements_t *acc, hiflow_data_t *out);

/* ---------- notification reassembly ---------- */

typedef enum {
    HIFLOW_RX_NEED_MORE = 0, /* frame not complete yet          */
    HIFLOW_RX_COMPLETE  = 1, /* rx->buf / rx->len hold a frame  */
    HIFLOW_RX_DROPPED   = -1 /* garbage; the buffer was cleared */
} hiflow_rx_result_t;

typedef struct {
    uint8_t buf[HIFLOW_MAX_FRAME_LEN];
    size_t  len;
    size_t  expected; /* 0 until the header has arrived */
} hiflow_rx_t;

/* Clears the buffer. The session calls this before every request, so a reply
   that arrived late or half can never be glued in front of the next one. */
void hiflow_rx_reset(hiflow_rx_t *rx);

/* Feeds one notification. */
int hiflow_rx_push(hiflow_rx_t *rx, const uint8_t *chunk, size_t len);

/* Command of the frame in the buffer (only valid after HIFLOW_RX_COMPLETE). */
uint16_t hiflow_rx_cmd(const hiflow_rx_t *rx);

#ifdef __cplusplus
}
#endif

#endif /* HIFLOW_PROTO_H */
