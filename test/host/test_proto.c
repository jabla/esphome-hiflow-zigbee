/*
 * test_proto.c — host test for hiflow_proto: payloads, paging, reassembly.
 *
 * Checks against test/host/vectors_proto.h (generated from test/vectors.json,
 * which the unmodified Python library produced):
 *   - every request payload the bridge sends is byte-identical to the reference
 *   - the device's status replies decode to the right action/sts
 *   - the device time is pulled out of the login acknowledgement, and nonsense
 *     values (the famous -1607349313) are rejected
 *   - a two-page RealDataNew reply merges into one set of values, with the
 *     ports addressed by port_number
 *   - reassembly across 20-byte notifications, and the cases that used to glue
 *     a stale fragment in front of the next reply
 *
 * Build/run:  make -C test/host test-proto
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hiflow_proto.h"
#include "pb_encode.h"
#include "RealDataNew.pb.h"
#include "vectors_proto.h"

static int g_pass;
static int g_fail;

static void check(int ok, const char *what)
{
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL %s\n", what);
    }
}

static void check_int(long got, long want, const char *what)
{
    if (got == want) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL %s: got %ld want %ld\n", what, got, want);
    }
}

static void check_float(float got, float want, const char *what)
{
    if (fabsf(got - want) < 0.005f) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL %s: got %.3f want %.3f\n", what, (double) got, (double) want);
    }
}

static size_t unhex(const char *hex, uint8_t *out, size_t cap)
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

static void check_payload(const uint8_t *got, size_t got_len, const char *want_hex,
                          const char *what)
{
    uint8_t want[HIFLOW_MAX_REQUEST];
    size_t want_len = unhex(want_hex, want, sizeof(want));
    size_t i;

    if (want_len == got_len && memcmp(got, want, want_len) == 0) {
        g_pass++;
        return;
    }
    g_fail++;
    printf("  FAIL %s\n      got =", what);
    for (i = 0; i < got_len; i++)
        printf("%02x", got[i]);
    printf("\n      want=%s\n", want_hex);
}

/* ---------- request payloads ---------- */

static void test_requests(void)
{
    uint8_t buf[HIFLOW_MAX_REQUEST];
    size_t len = 0;

    printf("\n[1] request payloads vs. the Python reference\n");

    check_int(hiflow_encode_comm_cmd(buf, sizeof(buf), &len, VEC_REQ_TIME,
                                     HIFLOW_ACTION_LOGIN, VEC_BLE_ID), HIFLOW_OK, "login encodes");
    check_payload(buf, len, VEC_HEX_LOGIN, "login (action 64)");

    check_int(hiflow_encode_comm_cmd(buf, sizeof(buf), &len, VEC_REQ_TIME,
                                     HIFLOW_ACTION_PIN, VEC_PIN), HIFLOW_OK, "PIN encodes");
    check_payload(buf, len, VEC_HEX_PIN, "PIN (action 82)");

    check_int(hiflow_encode_time_sync(buf, sizeof(buf), &len, VEC_REQ_TIME, VEC_REQ_OFFSET),
              HIFLOW_OK, "time-sync encodes");
    check_payload(buf, len, VEC_HEX_TIME_SYNC, "time-sync (action 104)");

    hiflow_encode_status_poll(buf, sizeof(buf), &len, VEC_REQ_TIME, HIFLOW_ACTION_LOGIN);
    check_payload(buf, len, VEC_HEX_POLL_LOGIN, "status poll 64");
    hiflow_encode_status_poll(buf, sizeof(buf), &len, VEC_REQ_TIME, HIFLOW_ACTION_PIN);
    check_payload(buf, len, VEC_HEX_POLL_PIN, "status poll 82");
    hiflow_encode_status_poll(buf, sizeof(buf), &len, VEC_REQ_TIME, HIFLOW_ACTION_TIME_SYNC);
    check_payload(buf, len, VEC_HEX_POLL_TIME_SYNC, "status poll 104");

    check_int(hiflow_encode_real_data_request(buf, sizeof(buf), &len, VEC_REQ_TIME,
                                              VEC_REQ_OFFSET, 0), HIFLOW_OK, "data request encodes");
    check_payload(buf, len, VEC_HEX_REAL_DATA_CP0, "data request cp=0");
    hiflow_encode_real_data_request(buf, sizeof(buf), &len, VEC_REQ_TIME, VEC_REQ_OFFSET, 1);
    check_payload(buf, len, VEC_HEX_REAL_DATA_CP1, "data request cp=1");

    check_int(hiflow_encode_app_info_v0(buf, sizeof(buf), &len, VEC_REQ_TIME, VEC_REQ_OFFSET),
              HIFLOW_OK, "V0 pairing request encodes");
    check_payload(buf, len, VEC_HEX_APP_INFO_V0, "V0 pairing request");

    /* A bleId longer than the field must be refused, not silently truncated. */
    {
        char too_long[80];
        memset(too_long, '7', sizeof(too_long) - 1);
        too_long[sizeof(too_long) - 1] = '\0';
        check(hiflow_encode_comm_cmd(buf, sizeof(buf), &len, VEC_REQ_TIME,
                                     HIFLOW_ACTION_LOGIN, too_long) != HIFLOW_OK,
              "oversized data refused");
    }
    check(hiflow_encode_comm_cmd(buf, 4, &len, VEC_REQ_TIME, HIFLOW_ACTION_LOGIN, VEC_BLE_ID) !=
              HIFLOW_OK,
          "short output buffer refused");
}

/* ---------- replies ---------- */

static void test_replies(void)
{
    uint8_t pt[64];
    size_t len;
    int i;

    printf("[2] status replies and the login acknowledgement\n");

    for (i = 0; i < VEC_STATUS_REPLY_COUNT; i++) {
        int32_t action = -1, sts = -1;
        char what[64];

        len = unhex(VEC_STATUS_REPLIES[i].hex, pt, sizeof(pt));
        snprintf(what, sizeof(what), "status reply action=%d sts=%d",
                 VEC_STATUS_REPLIES[i].action, VEC_STATUS_REPLIES[i].sts);
        check_int(hiflow_decode_status_reply(pt, len, &action, &sts), HIFLOW_OK, what);
        check_int(action, VEC_STATUS_REPLIES[i].action, "  action");
        check_int(sts, VEC_STATUS_REPLIES[i].sts, "  sts");
    }

    /* Garbage decodes as "in progress" rather than as a wrong status. */
    {
        const uint8_t junk[] = {0xFF, 0xFF, 0xFF};
        int32_t action = 9, sts = 9;
        check(hiflow_decode_status_reply(junk, sizeof(junk), &action, &sts) != HIFLOW_OK,
              "undecodable status reply reported");
        check_int(sts, HIFLOW_STS_IN_PROGRESS, "  falls back to in-progress");
    }

    /* Login acknowledgement: field 1 counter, field 2 device time, field 3 action. */
    {
        const uint8_t ack[] = {0x08, 0x80, 0x80, 0x80, 0x80, 0x08, /* f1 = big counter */
                               0x10, 0x80, 0xAF, 0xF4, 0xCD, 0x06, /* f2 = 1774000000 */
                               0x18, 0x40};                        /* f3 = 64 */
        check_int((long) hiflow_decode_login_ack_time(ack, sizeof(ack)), 1774000000L,
                  "device time from the acknowledgement");
    }
    {
        /* The value that appeared when the acknowledgement was decoded with the
           request layout; must not reach the clock. */
        const uint8_t bad[] = {0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F};
        check_int((long) hiflow_decode_login_ack_time(bad, sizeof(bad)), 0L,
                  "implausible device time rejected");
    }
    {
        const uint8_t truncated[] = {0x10, 0x80, 0xAF};
        check_int((long) hiflow_decode_login_ack_time(truncated, sizeof(truncated)), 0L,
                  "truncated acknowledgement rejected");
    }
}

/* ---------- paging ---------- */

static void test_paging(void)
{
    hiflow_measurements_t acc;
    hiflow_data_t data;
    uint8_t page[512];
    size_t len;
    int32_t ap = 0, cp = 0;
    int i;

    printf("[3] paged RealDataNew reply\n");

    hiflow_measurements_reset(&acc);

    len = unhex(VEC_HEX_PAGE0, page, sizeof(page));
    check_int(hiflow_merge_real_data(&acc, page, len, &ap, &cp), HIFLOW_OK, "page 0 decodes");
    check_int(ap, VEC_PAGE_AP, "  ap");
    check_int(cp, 0, "  cp");
    check_int(hiflow_measurements_complete(&acc), 0, "round not complete after page 0");

    len = unhex(VEC_HEX_PAGE1, page, sizeof(page));
    check_int(hiflow_merge_real_data(&acc, page, len, &ap, &cp), HIFLOW_OK, "page 1 decodes");
    check_int(cp, 1, "  cp");
    check_int(hiflow_measurements_complete(&acc), 1, "round complete after the last page");

    hiflow_measurements_to_data(&acc, &data);
    check_int(data.have_ac, 1, "AC block kept from page 0");
    check_float(data.ac_power_w, VEC_MERGED_AC_POWER_W, "ac power");
    check_float(data.ac_voltage_v, VEC_MERGED_AC_VOLTAGE_V, "ac voltage");
    check_float(data.ac_current_a, VEC_MERGED_AC_CURRENT_A, "ac current");
    check_float(data.ac_frequency_hz, VEC_MERGED_AC_FREQUENCY_HZ, "ac frequency");
    check_float(data.temperature_c, VEC_MERGED_TEMPERATURE_C, "temperature");
    check_float(data.energy_total_wh, VEC_MERGED_ENERGY_TOTAL_WH, "energy total over all ports");
    check_float(data.energy_daily_wh, VEC_MERGED_ENERGY_DAILY_WH, "energy daily over all ports");
    check_int(data.port_count, VEC_MERGED_PORT_COUNT, "all four ports present");

    for (i = 0; i < VEC_MERGED_PORT_COUNT; i++) {
        char what[48];
        int slot = VEC_MERGED_PORTS[i].port - 1;

        snprintf(what, sizeof(what), "port %d power", VEC_MERGED_PORTS[i].port);
        check_float(data.ports[slot].power_w, VEC_MERGED_PORTS[i].power_w, what);
        snprintf(what, sizeof(what), "port %d voltage", VEC_MERGED_PORTS[i].port);
        check_float(data.ports[slot].voltage_v, VEC_MERGED_PORTS[i].voltage_v, what);
        snprintf(what, sizeof(what), "port %d current", VEC_MERGED_PORTS[i].port);
        check_float(data.ports[slot].current_a, VEC_MERGED_PORTS[i].current_a, what);
        snprintf(what, sizeof(what), "port %d number", VEC_MERGED_PORTS[i].port);
        check_int(data.ports[slot].port_number, VEC_MERGED_PORTS[i].port, what);
    }

    /* Page 1 alone: the ports must land in slots 3 and 4, not 1 and 2. */
    hiflow_measurements_reset(&acc);
    len = unhex(VEC_HEX_PAGE1, page, sizeof(page));
    hiflow_merge_real_data(&acc, page, len, NULL, NULL);
    hiflow_measurements_to_data(&acc, &data);
    check_int(data.ports[0].present, 0, "port 1 stays empty");
    check_int(data.ports[2].port_number, 3, "page 1 lands on port 3");
    check_int(data.have_ac, 0, "no AC block on page 1");

    check(hiflow_merge_real_data(&acc, (const uint8_t *) "\xff\xff\xff", 3, NULL, NULL) != HIFLOW_OK,
          "undecodable page reported");
}

/* ---------- values beyond the reference vectors ---------- */

/* Encodes one single-page reply with nanopb. `sgs` picks the single-phase
   block, otherwise the three-phase one carries the grid values. */
static size_t encode_page(uint8_t *buf, size_t cap, int sgs)
{
    static RealDataNewReqDTO m; /* ~4 kB, keep it off the stack */
    pb_ostream_t os = pb_ostream_from_buffer(buf, cap);

    memset(&m, 0, sizeof(m));
    strcpy(m.device_serial_number, "TESTDTU00001");
    m.ap = 1;
    if (sgs) {
        m.sgs_data_count = 1;
        m.sgs_data[0].active_power = 8123;
        m.sgs_data[0].reactive_power = -769;
        m.sgs_data[0].power_factor = 997;
        m.sgs_data[0].warning_number = 7;
    } else {
        m.tgs_data_count = 1;
        m.tgs_data[0].active_power = 8123;
        m.tgs_data[0].reactive_power = 412;
        m.tgs_data[0].power_factor = 985;
        m.tgs_data[0].warning_number = 3;
    }
    m.pv_data_count = 2;
    m.pv_data[0].port_number = 1;
    m.pv_data[0].energy_total = 11023;
    m.pv_data[0].energy_daily = 1028;
    m.pv_data[1].port_number = 2;
    m.pv_data[1].energy_total = 9876;
    m.pv_data[1].energy_daily = 954;
    if (!pb_encode(&os, RealDataNewReqDTO_fields, &m))
        return 0;
    return os.bytes_written;
}

static void test_extra_values(void)
{
    static hiflow_measurements_t acc;
    hiflow_data_t data;
    uint8_t page[512];
    size_t len;

    printf("[5] reactive power, power factor, warnings, energy per port\n");

    len = encode_page(page, sizeof(page), 1);
    check(len > 0, "single-phase page encodes");
    hiflow_measurements_reset(&acc);
    check_int(hiflow_merge_real_data(&acc, page, len, NULL, NULL), HIFLOW_OK, "single-phase page decodes");
    hiflow_measurements_to_data(&acc, &data);
    check_float(data.reactive_power_var, -76.9f, "reactive power (x0.1 var, signed)");
    check_float(data.power_factor_pct, 99.7f, "power factor (x0.1 %)");
    check_float(data.warning_count, 7.0f, "warning count");
    check_float(data.ports[0].energy_total_wh, 11023.0f, "port 1 energy total");
    check_float(data.ports[0].energy_daily_wh, 1028.0f, "port 1 energy daily");
    check_float(data.ports[1].energy_total_wh, 9876.0f, "port 2 energy total");
    check_float(data.ports[1].energy_daily_wh, 954.0f, "port 2 energy daily");
    check_float(data.energy_total_wh, 11023.0f + 9876.0f, "total is still the sum of the ports");

    len = encode_page(page, sizeof(page), 0);
    check(len > 0, "three-phase page encodes");
    hiflow_measurements_reset(&acc);
    check_int(hiflow_merge_real_data(&acc, page, len, NULL, NULL), HIFLOW_OK, "three-phase page decodes");
    hiflow_measurements_to_data(&acc, &data);
    check_float(data.reactive_power_var, 41.2f, "three-phase reactive power");
    check_float(data.power_factor_pct, 98.5f, "three-phase power factor");
    check_float(data.warning_count, 3.0f, "three-phase warning count");
}

/* ---------- reassembly ---------- */

static void feed_chunks(hiflow_rx_t *rx, const uint8_t *frame, size_t len, size_t chunk,
                        int *last_result)
{
    size_t off;

    for (off = 0; off < len; off += chunk) {
        size_t n = len - off < chunk ? len - off : chunk;
        *last_result = hiflow_rx_push(rx, frame + off, n);
    }
}

static void build_frame(uint8_t *frame, uint16_t cmd, size_t payload_len)
{
    size_t total = HIFLOW_HEADER_LEN + payload_len + HIFLOW_TAG_LEN;
    size_t i;

    frame[0] = HIFLOW_MAGIC0;
    frame[1] = HIFLOW_MAGIC1;
    frame[2] = (uint8_t) (cmd >> 8);
    frame[3] = (uint8_t) (cmd & 0xFF);
    frame[4] = 0;
    frame[5] = 1; /* tid */
    frame[6] = 0;
    frame[7] = 0; /* crc, not checked here */
    frame[8] = (uint8_t) ((payload_len + HIFLOW_HEADER_LEN) >> 8);
    frame[9] = (uint8_t) ((payload_len + HIFLOW_HEADER_LEN) & 0xFF);
    for (i = HIFLOW_HEADER_LEN; i < total; i++)
        frame[i] = (uint8_t) i;
}

static void test_rx(void)
{
    hiflow_rx_t rx;
    uint8_t frame[HIFLOW_MAX_FRAME_LEN];
    int result = HIFLOW_RX_NEED_MORE;
    size_t total;

    printf("[4] notification reassembly\n");

    hiflow_rx_reset(&rx);
    build_frame(frame, 0xA211, 200);
    total = HIFLOW_HEADER_LEN + 200 + HIFLOW_TAG_LEN;
    feed_chunks(&rx, frame, total, 20, &result);
    check_int(result, HIFLOW_RX_COMPLETE, "V1 frame across 20-byte notifications");
    check_int((long) rx.len, (long) total, "  length includes the GCM tag");
    check_int(hiflow_rx_cmd(&rx), 0xA211, "  command readable");

    /* V0 replies have no tag. */
    hiflow_rx_reset(&rx);
    build_frame(frame, 0xA201, 64);
    feed_chunks(&rx, frame, HIFLOW_HEADER_LEN + 64, 20, &result);
    check_int(result, HIFLOW_RX_COMPLETE, "V0 frame without a tag");
    check_int((long) rx.len, (long) (HIFLOW_HEADER_LEN + 64), "  length without the tag");

    /* One long notification carrying everything. */
    hiflow_rx_reset(&rx);
    build_frame(frame, 0xA211, 40);
    check_int(hiflow_rx_push(&rx, frame, HIFLOW_HEADER_LEN + 40 + HIFLOW_TAG_LEN),
              HIFLOW_RX_COMPLETE, "single notification");

    /* A stray notification without the magic is dropped, not appended. */
    hiflow_rx_reset(&rx);
    check_int(hiflow_rx_push(&rx, (const uint8_t *) "junk", 4), HIFLOW_RX_DROPPED,
              "notification without HM dropped");
    check_int((long) rx.len, 0L, "  buffer cleared");

    /* Half a reply is left over; the session resets before the next request, so
       the next reply starts clean instead of being glued behind the fragment. */
    hiflow_rx_reset(&rx);
    build_frame(frame, 0xA211, 200);
    check_int(hiflow_rx_push(&rx, frame, 30), HIFLOW_RX_NEED_MORE, "fragment kept");
    hiflow_rx_reset(&rx);
    build_frame(frame, 0xA211, 40);
    check_int(hiflow_rx_push(&rx, frame, HIFLOW_HEADER_LEN + 40 + HIFLOW_TAG_LEN),
              HIFLOW_RX_COMPLETE, "next reply after a reset");

    /* Implausible and oversized length fields. */
    hiflow_rx_reset(&rx);
    build_frame(frame, 0xA211, 40);
    frame[8] = 0;
    frame[9] = 3;
    check_int(hiflow_rx_push(&rx, frame, HIFLOW_HEADER_LEN + 10), HIFLOW_RX_DROPPED,
              "length below the header dropped");
    hiflow_rx_reset(&rx);
    build_frame(frame, 0xA211, 40);
    frame[8] = 0xFF;
    frame[9] = 0xFF;
    check_int(hiflow_rx_push(&rx, frame, HIFLOW_HEADER_LEN + 10), HIFLOW_RX_DROPPED,
              "oversized frame dropped");
}

int main(void)
{
    printf("=== hiflow_proto ===\n");
    test_requests();
    test_replies();
    test_paging();
    test_rx();
    test_extra_values();

    printf("\n=== summary ===\n");
    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0)
        printf("proto: %d/%d ok — ALL PASS\n", g_pass, g_pass);
    return g_fail == 0 ? 0 : 1;
}
