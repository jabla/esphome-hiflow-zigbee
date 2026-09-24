/*
 * test_pb.c -- host test for the vendored nanopb decoders of the HiFlow
 * RealDataNew protobuf message (and the CommCmd handshake messages).
 *
 * Pure host code: no ESP32, no BLE, no hardware. It
 *   1. reads `wire_hex` from test/vectors.json at run time (ground truth,
 *      produced by gen_vectors.py with the real library protobuf modules),
 *   2. decodes it with the nanopb-generated RealDataNewReqDTO decoder and
 *      compares EVERY field from vectors.json `fields`,
 *   3. re-encodes the decoded message and decodes it again (roundtrip),
 *      checking byte-identity and all field values a second time,
 *   4. proves that truncated / corrupted byte arrays make pb_decode FAIL
 *      instead of returning plausible values,
 *   5. exercises encode+decode roundtrips for the CommCmd handshake messages
 *      (CommCmdResDTO / CommCmdStatusResDTO / CommCmdStatusReqDTO).
 *
 * Expected field values come from vectors_pb.h (generated from the same
 * vectors.json by gen_vectors_header.py). Exit code 0 == all checks passed.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "RealDataNew.pb.h"
#include "CommandPB.pb.h"
#include "CommCmdPB.pb.h"
#include "vectors_pb.h"

/* ------------------------------------------------------------------ */
/* check accounting                                                    */
/* ------------------------------------------------------------------ */
static int g_field_checks, g_field_fails; /* RealDataNew field comparisons */
static int g_other_fails;                 /* decode-ok / roundtrip / misc   */
static int g_neg_total, g_neg_rejected;   /* negative (must-fail) cases     */
static int g_hs_checks, g_hs_fails;       /* handshake checks               */

#define CK_FIELD(cond, ...)                                                    \
    do {                                                                       \
        g_field_checks++;                                                      \
        if (!(cond)) {                                                         \
            g_field_fails++;                                                   \
            printf("    FAIL: ");                                              \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

#define CK_OTHER(cond, ...)                                                    \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_other_fails++;                                                   \
            printf("  FAIL: ");                                                \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

#define CK_HS(cond, ...)                                                       \
    do {                                                                       \
        g_hs_checks++;                                                         \
        if (!(cond)) {                                                         \
            g_hs_fails++;                                                      \
            printf("    FAIL: ");                                              \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */
static void print_hex(const char *label, const uint8_t *b, size_t n)
{
    printf("  %s (%zu bytes): ", label, n);
    for (size_t i = 0; i < n; i++) {
        printf("%02x", b[i]);
    }
    printf("\n");
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Extract the hex-string value of the first top-level `"key": "...."` found
 * in a JSON file. Returns the number of decoded bytes (0 on failure). */
static size_t load_hex(const char *path, const char *key, uint8_t *out, size_t cap)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';

    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    char *p = strstr(buf, pat);
    if (!p) {
        return 0;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p = strchr(p, '"');
    if (!p) return 0;
    p++;

    size_t len = 0;
    while (*p && *p != '"' && len < cap) {
        int hi = hexval((unsigned char)p[0]);
        int lo = hexval((unsigned char)p[1]);
        if (hi < 0 || lo < 0) break;
        out[len++] = (uint8_t)((hi << 4) | lo);
        p += 2;
    }
    return len;
}

static bool decodes_ok(const uint8_t *buf, size_t len)
{
    RealDataNewReqDTO m = RealDataNewReqDTO_init_zero;
    pb_istream_t s = pb_istream_from_buffer(buf, len);
    return pb_decode(&s, RealDataNewReqDTO_fields, &m);
}

/* Compare every field of a decoded RealDataNewReqDTO against vectors.json. */
static void check_realdata(const RealDataNewReqDTO *m)
{
    int i;

    CK_FIELD(m->sgs_data_count == EXP_SGS_COUNT,
             "sgs_data_count=%u expected %d", (unsigned)m->sgs_data_count, EXP_SGS_COUNT);
    CK_FIELD(m->pv_data_count == EXP_PV_COUNT,
             "pv_data_count=%u expected %d", (unsigned)m->pv_data_count, EXP_PV_COUNT);

    for (i = 0; i < (int)m->sgs_data_count && i < EXP_SGS_COUNT; i++) {
        const SGSMO *s = &m->sgs_data[i];
        CK_FIELD(s->active_power == EXP_SGS_ACTIVE_POWER[i],
                 "sgs[%d].active_power=%d expected %lld", i, s->active_power,
                 (long long)EXP_SGS_ACTIVE_POWER[i]);
        CK_FIELD(s->voltage == EXP_SGS_VOLTAGE[i],
                 "sgs[%d].voltage=%d expected %lld", i, s->voltage,
                 (long long)EXP_SGS_VOLTAGE[i]);
        CK_FIELD(s->frequency == EXP_SGS_FREQUENCY[i],
                 "sgs[%d].frequency=%d expected %lld", i, s->frequency,
                 (long long)EXP_SGS_FREQUENCY[i]);
        CK_FIELD(s->temperature == EXP_SGS_TEMPERATURE[i],
                 "sgs[%d].temperature=%d expected %lld", i, s->temperature,
                 (long long)EXP_SGS_TEMPERATURE[i]);
    }

    for (i = 0; i < (int)m->pv_data_count && i < EXP_PV_COUNT; i++) {
        const PvMO *p = &m->pv_data[i];
        CK_FIELD(p->voltage == EXP_PV_VOLTAGE[i],
                 "pv[%d].voltage=%d expected %lld", i, p->voltage,
                 (long long)EXP_PV_VOLTAGE[i]);
        CK_FIELD(p->current == EXP_PV_CURRENT[i],
                 "pv[%d].current=%d expected %lld", i, p->current,
                 (long long)EXP_PV_CURRENT[i]);
        CK_FIELD(p->power == EXP_PV_POWER[i],
                 "pv[%d].power=%d expected %lld", i, p->power,
                 (long long)EXP_PV_POWER[i]);
        CK_FIELD(p->energy_total == EXP_PV_ENERGY_TOTAL[i],
                 "pv[%d].energy_total=%d expected %lld", i, p->energy_total,
                 (long long)EXP_PV_ENERGY_TOTAL[i]);
        CK_FIELD(p->energy_daily == EXP_PV_ENERGY_DAILY[i],
                 "pv[%d].energy_daily=%d expected %lld", i, p->energy_daily,
                 (long long)EXP_PV_ENERGY_DAILY[i]);
        CK_FIELD(p->error_code == EXP_PV_ERROR_CODE[i],
                 "pv[%d].error_code=%d expected %lld", i, p->error_code,
                 (long long)EXP_PV_ERROR_CODE[i]);
    }
}

/* Negative case: the buffer MUST be rejected by pb_decode. */
static void neg_case(const char *name, const uint8_t *buf, size_t len)
{
    g_neg_total++;
    if (!decodes_ok(buf, len)) {
        g_neg_rejected++;
        printf("  rejected (as required): %s\n", name);
    } else {
        printf("  FAIL: accepted a bad buffer: %s\n", name);
        g_other_fails++;
    }
}

/* ------------------------------------------------------------------ */
/* handshake messages (CommCmd)                                        */
/* ------------------------------------------------------------------ */
static void test_handshake(void)
{
    printf("\n[4] CommCmd handshake messages (encode -> decode roundtrip)\n");
    printf("    layout: CommCmdResDTO{1:time,2:action,5:tid,6:data}, "
           "CommCmdStatusResDTO{1:time,2:action,4:tid},\n"
           "            CommCmdStatusReqDTO{3:action,11:sts}; "
           "actions 64=login,82=PIN,104=time-sync\n");

    /* action=64 login: data carries the bleId. Placeholder string only --
     * real credentials are supplied by the caller at run time. */
    {
        uint8_t buf[128];
        CommCmdResDTO tx = CommCmdResDTO_init_zero;
        CommCmdResDTO rx = CommCmdResDTO_init_zero;
        pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
        tx.time = 1757558400;
        tx.action = 64;
        tx.tid = 1757558400;
        snprintf(tx.data, sizeof(tx.data), "%s", "TEST-BLEID-000000000");
        CK_HS(pb_encode(&os, CommCmdResDTO_fields, &tx), "encode action=64 failed");
        print_hex("action=64 login", buf, os.bytes_written);
        pb_istream_t is = pb_istream_from_buffer(buf, os.bytes_written);
        CK_HS(pb_decode(&is, CommCmdResDTO_fields, &rx), "decode action=64: %s", PB_GET_ERROR(&is));
        CK_HS(rx.action == 64, "action=%d expected 64", rx.action);
        CK_HS(rx.time == tx.time, "time=%lld", (long long)rx.time);
        CK_HS(rx.tid == tx.tid, "tid=%lld", (long long)rx.tid);
        CK_HS(strcmp(rx.data, tx.data) == 0, "data='%s' expected '%s'", rx.data, tx.data);
    }

    /* action=82 PIN: same message, data carries the PIN. */
    {
        uint8_t buf[128];
        CommCmdResDTO tx = CommCmdResDTO_init_zero;
        CommCmdResDTO rx = CommCmdResDTO_init_zero;
        pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
        tx.time = 1757558401;
        tx.action = 82;
        tx.tid = 1757558401;
        snprintf(tx.data, sizeof(tx.data), "%s", "TEST-PIN");
        CK_HS(pb_encode(&os, CommCmdResDTO_fields, &tx), "encode action=82 failed");
        pb_istream_t is = pb_istream_from_buffer(buf, os.bytes_written);
        CK_HS(pb_decode(&is, CommCmdResDTO_fields, &rx), "decode action=82: %s", PB_GET_ERROR(&is));
        CK_HS(rx.action == 82, "action=%d expected 82", rx.action);
        CK_HS(strcmp(rx.data, "TEST-PIN") == 0, "data='%s'", rx.data);
    }

    /* action=104 time-sync: data = "<unix>,<tz_offset>\r" */
    {
        uint8_t buf[128];
        CommCmdResDTO tx = CommCmdResDTO_init_zero;
        CommCmdResDTO rx = CommCmdResDTO_init_zero;
        pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
        tx.time = 1757558402;
        tx.action = 104;
        tx.tid = 1757558402;
        snprintf(tx.data, sizeof(tx.data), "%d,%d\r", 1757558402, 3600);
        CK_HS(pb_encode(&os, CommCmdResDTO_fields, &tx), "encode action=104 failed");
        print_hex("action=104 time-sync", buf, os.bytes_written);
        pb_istream_t is = pb_istream_from_buffer(buf, os.bytes_written);
        CK_HS(pb_decode(&is, CommCmdResDTO_fields, &rx), "decode action=104: %s", PB_GET_ERROR(&is));
        CK_HS(rx.action == 104, "action=%d expected 104", rx.action);
        CK_HS(strcmp(rx.data, "1757558402,3600\r") == 0, "data='%s'", rx.data);
    }

    /* poll message, action=64 */
    {
        uint8_t buf[64];
        CommCmdStatusResDTO tx = CommCmdStatusResDTO_init_zero;
        CommCmdStatusResDTO rx = CommCmdStatusResDTO_init_zero;
        pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
        tx.time = 1757558403;
        tx.action = 64;
        tx.tid = 7;
        CK_HS(pb_encode(&os, CommCmdStatusResDTO_fields, &tx), "encode status poll failed");
        pb_istream_t is = pb_istream_from_buffer(buf, os.bytes_written);
        CK_HS(pb_decode(&is, CommCmdStatusResDTO_fields, &rx), "decode status poll: %s", PB_GET_ERROR(&is));
        CK_HS(rx.action == 64 && rx.tid == 7 && rx.time == tx.time,
             "poll action=%d tid=%lld time=%lld", rx.action, (long long)rx.tid, (long long)rx.time);
    }

    /* device status response: field 3 = action, field 11 = sts */
    {
        uint8_t buf[64];
        CommCmdStatusReqDTO tx = CommCmdStatusReqDTO_init_zero;
        CommCmdStatusReqDTO rx = CommCmdStatusReqDTO_init_zero;
        pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
        tx.action = 82; /* 82 = PIN action */
        tx.sts = 0;     /* sts=0 for action=82 => SUCCESS */
        CK_HS(pb_encode(&os, CommCmdStatusReqDTO_fields, &tx), "encode device status failed");
        pb_istream_t is = pb_istream_from_buffer(buf, os.bytes_written);
        CK_HS(pb_decode(&is, CommCmdStatusReqDTO_fields, &rx), "decode device status: %s", PB_GET_ERROR(&is));
        CK_HS(rx.action == 82 && rx.sts == 0, "status action=%d sts=%d", rx.action, rx.sts);
    }
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    static uint8_t wire[4096];
    size_t wlen = 0;
    const char *used_path = NULL;

    if (argc > 1) {
        wlen = load_hex(argv[1], "wire_hex", wire, sizeof(wire));
        if (wlen) used_path = argv[1];
    } else {
        static const char *cands[] = {
            "test/vectors.json", "../vectors.json", "../../test/vectors.json",
        };
        for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
            wlen = load_hex(cands[i], "wire_hex", wire, sizeof(wire));
            if (wlen) { used_path = cands[i]; break; }
        }
    }
    if (!wlen) {
        fprintf(stderr, "ERROR: could not read 'wire_hex' from vectors.json "
                        "(pass the path as argv[1])\n");
        return 2;
    }

    printf("vectors.json : %s\n", used_path);
    printf("wire_hex     : %zu bytes\n", wlen);
    print_hex("wire", wire, wlen);

    /* ---- [1] decode ------------------------------------------------ */
    printf("\n[1] decode RealDataNewReqDTO from wire_hex\n");
    static RealDataNewReqDTO msg;
    memset(&msg, 0, sizeof(msg));
    pb_istream_t s1 = pb_istream_from_buffer(wire, wlen);
    bool dec_ok = pb_decode(&s1, RealDataNewReqDTO_fields, &msg);
    CK_OTHER(dec_ok, "pb_decode failed: %s", dec_ok ? "" : PB_GET_ERROR(&s1));
    if (dec_ok) {
        CK_OTHER(s1.bytes_left == 0, "unconsumed bytes: %zu", s1.bytes_left);
        printf("  pb_decode ok (all %zu bytes consumed)\n", wlen);
        printf("  sgs_data[0]: active_power=%d voltage=%d frequency=%d temperature=%d\n",
               msg.sgs_data[0].active_power, msg.sgs_data[0].voltage,
               msg.sgs_data[0].frequency, msg.sgs_data[0].temperature);
        for (unsigned i = 0; i < msg.pv_data_count; i++) {
            printf("  pv_data[%u] : voltage=%d current=%d power=%d energy_total=%d "
                   "energy_daily=%d error_code=%d\n",
                   i, msg.pv_data[i].voltage, msg.pv_data[i].current, msg.pv_data[i].power,
                   msg.pv_data[i].energy_total, msg.pv_data[i].energy_daily,
                   msg.pv_data[i].error_code);
        }
        g_field_checks = g_field_fails = 0;
        check_realdata(&msg);
    }
    int dec_ck = g_field_checks, dec_fl = g_field_fails;

    /* ---- [2] roundtrip --------------------------------------------- */
    printf("\n[2] roundtrip: re-encode -> decode again -> compare\n");
    int rt_fl = 0;
    if (!dec_ok) {
        rt_fl = -1;
        printf("  skipped (initial decode failed)\n");
    } else {
        static uint8_t buf2[4096];
        pb_ostream_t os = pb_ostream_from_buffer(buf2, sizeof(buf2));
        bool enc_ok = pb_encode(&os, RealDataNewReqDTO_fields, &msg);
        CK_OTHER(enc_ok, "pb_encode failed: %s", enc_ok ? "" : PB_GET_ERROR(&os));
        if (enc_ok) {
            bool ident = (os.bytes_written == wlen) && (memcmp(buf2, wire, wlen) == 0);
            CK_OTHER(ident, "re-encoded bytes differ from original wire");
            printf("  re-encoded %zu bytes, byte-identical to wire: %s\n",
                   os.bytes_written, ident ? "yes" : "no");
            static RealDataNewReqDTO msg2;
            memset(&msg2, 0, sizeof(msg2));
            pb_istream_t s2 = pb_istream_from_buffer(buf2, os.bytes_written);
            bool ok2 = pb_decode(&s2, RealDataNewReqDTO_fields, &msg2);
            CK_OTHER(ok2, "re-decode failed: %s", ok2 ? "" : PB_GET_ERROR(&s2));
            if (ok2) {
                g_field_checks = g_field_fails = 0;
                check_realdata(&msg2);
                rt_fl = g_field_fails;
                printf("  second decode: %d/%d fields match\n",
                       g_field_checks - g_field_fails, g_field_checks);
            } else {
                rt_fl = -1;
            }
        } else {
            rt_fl = -1;
        }
    }

    /* ---- [3] negative cases ---------------------------------------- */
    printf("\n[3] negative cases (truncated / corrupted must be rejected)\n");
    {
        static uint8_t tmp[4096];

        /* truncated mid-field: drop the final length byte of the last entry */
        memcpy(tmp, wire, wlen - 1);
        neg_case("truncated to len-1 (bare 0x5a tag, no length)", tmp, wlen - 1);

        /* truncated to 3 bytes: sgs_data claims 12 bytes, only 1 present */
        memcpy(tmp, wire, 3);
        neg_case("truncated to first 3 bytes (length exceeds buffer)", tmp, 3);

        /* corrupted field length: 0x0c -> 0xff for sgs_data */
        memcpy(tmp, wire, wlen);
        tmp[1] = 0xFF;
        neg_case("corrupted sgs_data length byte 0x0c -> 0xff", tmp, wlen);

        /* unterminated varint: error_code last byte 0x18 -> 0x98 */
        memcpy(tmp, wire, wlen);
        tmp[wlen - 3] = 0x98;
        neg_case("unterminated varint for error_code", tmp, wlen);
    }

    /* ---- [4] handshake --------------------------------------------- */
    test_handshake();

    /* ---- summary --------------------------------------------------- */
    int total_fails = dec_fl + g_other_fails + g_hs_fails + rt_fl + g_field_fails;
    bool all_pass = (dec_ok && dec_fl == 0 && rt_fl == 0 && g_other_fails == 0 &&
                     g_neg_rejected == g_neg_total && g_hs_fails == 0);

    printf("\n=== summary ===\n");
    printf("decode: %d/%d fields matched%s\n",
           dec_ck - dec_fl, dec_ck, dec_ok ? "" : " (decode FAILED)");
    printf("roundtrip: %s\n", rt_fl == 0 ? "ok (byte-identical, fields re-verified)" :
           (rt_fl < 0 ? "FAILED" : "field mismatch"));
    printf("negative: %d/%d rejected\n", g_neg_rejected, g_neg_total);
    printf("handshake: %d/%d checks ok\n", g_hs_checks - g_hs_fails, g_hs_checks);

    printf("\npb decode %d/%d ok, fields %d/%d match, roundtrip %s, negative %s — %s\n",
           dec_ok ? 1 : 0, 1,
           dec_ck - dec_fl, dec_ck,
           rt_fl == 0 ? "ok" : "FAIL",
           g_neg_rejected == g_neg_total ? "ok" : "FAIL",
           all_pass ? "ALL PASS" : "FAIL");

    return (all_pass && total_fails == 0) ? 0 : 1;
}
