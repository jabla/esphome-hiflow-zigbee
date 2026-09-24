/*
 * test_hiflow_frame.c — host test: C port vs Python reference vectors.
 *
 * Verifies, against test/host/vectors.h (generated from test/vectors.json,
 * itself produced by the unmodified Python library):
 *   - CRC16-Modbus values
 *   - V1 key / nonce / aad derivation
 *   - V1 frame build == expected bytes, and parse(build) round-trip,
 *     and parse(expected_bytes) == original plaintext
 *   - V0 key / iv derivation
 *   - V0 frame build == expected bytes, and parse round-trip both ways
 *   - negative cases: corrupted CRC -> HIFLOW_ERR_CRC,
 *                     corrupted GCM tag -> HIFLOW_ERR_GCM_TAG (distinguishable)
 *
 * Build/run:  make -C test/host test
 */
#include <stdio.h>
#include <string.h>

#include "hiflow_frame.h"
#include "vectors.h"

#define TMP_CAP 1024

static int g_pass;
static int g_fail;

static int bytes_eq(const uint8_t *a, const uint8_t *b, size_t n)
{
    return memcmp(a, b, n) == 0;
}

static void print_hex(const char *label, const uint8_t *p, size_t n)
{
    size_t i;
    printf("      %s=", label);
    for (i = 0; i < n; i++)
        printf("%02x", p[i]);
    printf("\n");
}

/* ---------- CRC ---------- */

static void test_crc(int *pass, int *total)
{
    size_t i;
    *pass = 0;
    *total = (int)VEC_CRC_COUNT;

    for (i = 0; i < VEC_CRC_COUNT; i++) {
        uint16_t got = hiflow_crc16_modbus(VEC_CRC[i].data, VEC_CRC[i].len);
        if (got == VEC_CRC[i].crc) {
            (*pass)++;
            g_pass++;
        } else {
            g_fail++;
            printf("  FAIL crc[%zu]: got 0x%04x want 0x%04x\n", i, got,
                   VEC_CRC[i].crc);
        }
    }
}

/* ---------- V1 key/nonce/aad ---------- */

static void test_v1_keys(int *pass, int *total)
{
    size_t i;
    *pass = 0;
    *total = (int)VEC_V1_KEYS_COUNT;

    for (i = 0; i < VEC_V1_KEYS_COUNT; i++) {
        uint8_t key[HIFLOW_KEY_LEN], nonce[HIFLOW_NONCE_LEN], aad[HIFLOW_AAD_LEN];
        const vec_v1_key_t *v = &VEC_V1_KEYS[i];
        int ok = 1;

        hiflow_v1_derive_key(VEC_ENC_RAND, key);
        hiflow_v1_derive_nonce(VEC_ENC_RAND, v->cmd, v->tid, nonce);
        hiflow_v1_derive_aad(v->cmd, v->tid, aad);

        if (!bytes_eq(key, v->key, HIFLOW_KEY_LEN))
            ok = 0;
        if (!bytes_eq(nonce, v->nonce, HIFLOW_NONCE_LEN))
            ok = 0;
        if (!bytes_eq(aad, v->aad, HIFLOW_AAD_LEN))
            ok = 0;

        if (ok) {
            (*pass)++;
            g_pass++;
        } else {
            g_fail++;
            printf("  FAIL v1 key case %zu (cmd=0x%04x tid=%u)\n", i, v->cmd,
                   v->tid);
            print_hex("key  got", key, HIFLOW_KEY_LEN);
            print_hex("key want", v->key, HIFLOW_KEY_LEN);
            print_hex("nnce got", nonce, HIFLOW_NONCE_LEN);
            print_hex("nnce want", v->nonce, HIFLOW_NONCE_LEN);
            print_hex("aad  got", aad, HIFLOW_AAD_LEN);
            print_hex("aad want", v->aad, HIFLOW_AAD_LEN);
        }
    }
}

/* ---------- V1 frames ---------- */

static void test_v1_frames(int *pass, int *total)
{
    size_t i;
    *pass = 0;
    *total = (int)VEC_V1_FRAMES_COUNT;

    for (i = 0; i < VEC_V1_FRAMES_COUNT; i++) {
        const vec_v1_frame_t *v = &VEC_V1_FRAMES[i];
        uint8_t buf[TMP_CAP];
        size_t out_len = 0;
        uint16_t cmd = 0, tid = 0;
        uint8_t pt[TMP_CAP];
        size_t pt_len = 0;
        uint16_t crc_field;
        size_t ct_len;
        int ok = 1;
        int rc;

        /* build */
        rc = hiflow_build_frame_v1(VEC_ENC_RAND, v->cmd, v->tid, v->pt,
                                   v->pt_len, buf, sizeof buf, &out_len);
        if (rc != HIFLOW_OK || out_len != v->frame_len ||
            !bytes_eq(buf, v->frame, v->frame_len)) {
            ok = 0;
            printf("  FAIL v1 build case %zu (cmd=0x%04x tid=%u) rc=%d "
                   "len=%zu want %zu\n",
                   i, v->cmd, v->tid, rc, out_len, v->frame_len);
            if (rc == HIFLOW_OK) {
                print_hex("frame got ", buf, out_len);
                print_hex("frame want", v->frame, v->frame_len);
            }
        }

        /* header CRC field + length field must match reference too */
        if (rc == HIFLOW_OK) {
            ct_len = v->frame_len - HIFLOW_HEADER_LEN - HIFLOW_TAG_LEN;
            crc_field = hiflow_crc16_modbus(buf + HIFLOW_HEADER_LEN, ct_len);
            if (crc_field != v->crc) {
                ok = 0;
                g_fail++;
                printf("  FAIL v1 crc case %zu: got 0x%04x want 0x%04x\n", i,
                       crc_field, v->crc);
            }
        }

        /* parse our own build -> round-trip */
        rc = hiflow_parse_frame_v1(VEC_ENC_RAND, buf, out_len, &cmd, &tid, pt,
                                   sizeof pt, &pt_len);
        if (rc != HIFLOW_OK || cmd != v->cmd || tid != v->tid ||
            pt_len != v->pt_len || !bytes_eq(pt, v->pt, v->pt_len)) {
            ok = 0;
            printf("  FAIL v1 parse(build) case %zu rc=%d\n", i, rc);
        }

        /* parse the ground-truth frame bytes -> original plaintext */
        rc = hiflow_parse_frame_v1(VEC_ENC_RAND, v->frame, v->frame_len, &cmd,
                                   &tid, pt, sizeof pt, &pt_len);
        if (rc != HIFLOW_OK || cmd != v->cmd || tid != v->tid ||
            pt_len != v->pt_len || !bytes_eq(pt, v->pt, v->pt_len)) {
            ok = 0;
            printf("  FAIL v1 parse(reference) case %zu rc=%d\n", i, rc);
        }

        if (ok) {
            (*pass)++;
            g_pass++;
        } else {
            g_fail++;
        }
    }
}

/* ---------- V0 key/iv ---------- */

static void test_v0_keys(int *pass, int *total)
{
    size_t i;
    *pass = 0;
    *total = (int)VEC_V0_KEYS_COUNT;

    for (i = 0; i < VEC_V0_KEYS_COUNT; i++) {
        uint8_t key[HIFLOW_KEY_LEN], iv[HIFLOW_IV_LEN];
        const vec_v0_key_t *v = &VEC_V0_KEYS[i];
        int ok = 1;

        hiflow_v0_derive_key(VEC_SN, key);
        hiflow_v0_derive_iv(VEC_SN, v->cmd, v->tid, iv);

        if (!bytes_eq(key, v->key, HIFLOW_KEY_LEN))
            ok = 0;
        if (!bytes_eq(iv, v->iv, HIFLOW_IV_LEN))
            ok = 0;

        if (ok) {
            (*pass)++;
            g_pass++;
        } else {
            g_fail++;
            printf("  FAIL v0 key case %zu (cmd=0x%04x tid=%u)\n", i, v->cmd,
                   v->tid);
            print_hex("key got ", key, HIFLOW_KEY_LEN);
            print_hex("key want", v->key, HIFLOW_KEY_LEN);
            print_hex("iv  got ", iv, HIFLOW_IV_LEN);
            print_hex("iv  want", v->iv, HIFLOW_IV_LEN);
        }
    }
}

/* ---------- V0 frames ---------- */

static void test_v0_frames(int *pass, int *total)
{
    size_t i;
    *pass = 0;
    *total = (int)VEC_V0_FRAMES_COUNT;

    for (i = 0; i < VEC_V0_FRAMES_COUNT; i++) {
        const vec_v0_frame_t *v = &VEC_V0_FRAMES[i];
        uint8_t buf[TMP_CAP];
        size_t out_len = 0;
        uint16_t cmd = 0, tid = 0;
        uint8_t pt[TMP_CAP];
        size_t pt_len = 0;
        int ok = 1;
        int rc;

        rc = hiflow_build_frame_v0(VEC_SN, v->cmd, v->tid, v->pt, v->pt_len,
                                   buf, sizeof buf, &out_len);
        if (rc != HIFLOW_OK || out_len != v->frame_len ||
            !bytes_eq(buf, v->frame, v->frame_len)) {
            ok = 0;
            printf("  FAIL v0 build case %zu (cmd=0x%04x tid=%u) rc=%d "
                   "len=%zu want %zu\n",
                   i, v->cmd, v->tid, rc, out_len, v->frame_len);
            if (rc == HIFLOW_OK) {
                print_hex("frame got ", buf, out_len);
                print_hex("frame want", v->frame, v->frame_len);
            }
        }

        rc = hiflow_parse_frame_v0(VEC_SN, buf, out_len, &cmd, &tid, pt,
                                   sizeof pt, &pt_len);
        if (rc != HIFLOW_OK || cmd != v->cmd || tid != v->tid ||
            pt_len != v->pt_len || !bytes_eq(pt, v->pt, v->pt_len)) {
            ok = 0;
            printf("  FAIL v0 parse(build) case %zu rc=%d\n", i, rc);
        }

        rc = hiflow_parse_frame_v0(VEC_SN, v->frame, v->frame_len, &cmd, &tid,
                                   pt, sizeof pt, &pt_len);
        if (rc != HIFLOW_OK || cmd != v->cmd || tid != v->tid ||
            pt_len != v->pt_len || !bytes_eq(pt, v->pt, v->pt_len)) {
            ok = 0;
            printf("  FAIL v0 parse(reference) case %zu rc=%d\n", i, rc);
        }

        if (ok) {
            (*pass)++;
            g_pass++;
        } else {
            g_fail++;
        }
    }
}

/* ---------- negative tests ---------- */

static void test_negative(int *pass, int *total)
{
    const vec_v1_frame_t *v = &VEC_V1_FRAMES[0];
    uint8_t buf[TMP_CAP];
    uint8_t pt[TMP_CAP];
    uint16_t cmd, tid;
    size_t pt_len;
    int rc_crc, rc_tag;

    *pass = 0;
    *total = 2;

    /* 1) corrupt a byte of the frame-header CRC field (byte 6).
     *    Ciphertext is untouched, so CRC check must fail as CRC (not GCM). */
    memcpy(buf, v->frame, v->frame_len);
    buf[6] ^= 0xFF;
    rc_crc = hiflow_parse_frame_v1(VEC_ENC_RAND, buf, v->frame_len, &cmd, &tid,
                                   pt, sizeof pt, &pt_len);
    if (rc_crc == HIFLOW_ERR_CRC && rc_crc != HIFLOW_ERR_GCM_TAG) {
        (*pass)++;
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL negative[0]: corrupted CRC returned %d (want %d)\n",
               rc_crc, HIFLOW_ERR_CRC);
    }

    /* 2) corrupt the last byte (inside the 16-byte GCM tag). CRC still valid,
     *    so the failure must be a GCM tag failure, distinct from CRC. */
    memcpy(buf, v->frame, v->frame_len);
    buf[v->frame_len - 1] ^= 0x01;
    rc_tag = hiflow_parse_frame_v1(VEC_ENC_RAND, buf, v->frame_len, &cmd, &tid,
                                   pt, sizeof pt, &pt_len);
    if (rc_tag == HIFLOW_ERR_GCM_TAG && rc_tag != HIFLOW_ERR_CRC) {
        (*pass)++;
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL negative[1]: corrupted GCM tag returned %d (want %d)\n",
               rc_tag, HIFLOW_ERR_GCM_TAG);
    }

    /* The two failures must be distinguishable from each other. */
    if (rc_crc == rc_tag) {
        g_fail++;
        printf("  FAIL negative: CRC and GCM errors not distinguishable\n");
    }
}

int main(void)
{
    int crc_p, crc_t;
    int v1k_p, v1k_t;
    int v1f_p, v1f_t;
    int v0k_p, v0k_t;
    int v0f_p, v0f_t;
    int neg_p, neg_t;

    printf("HiFlow-BLE C port — host vector test (OpenSSL backend)\n");
    printf("vectors: crc=%d v1_keys=%d v1_frames=%d v0_keys=%d v0_frames=%d\n\n",
           (int)VEC_CRC_COUNT, (int)VEC_V1_KEYS_COUNT,
           (int)VEC_V1_FRAMES_COUNT, (int)VEC_V0_KEYS_COUNT,
           (int)VEC_V0_FRAMES_COUNT);

    test_crc(&crc_p, &crc_t);
    test_v1_keys(&v1k_p, &v1k_t);
    test_v1_frames(&v1f_p, &v1f_t);
    test_v0_keys(&v0k_p, &v0k_t);
    test_v0_frames(&v0f_p, &v0f_t);
    test_negative(&neg_p, &neg_t);

    printf("crc %d/%d, v1 keys %d/%d, v1 frames %d/%d, v0 keys %d/%d, "
           "v0 frames %d/%d, negative %d/%d — %s\n",
           crc_p, crc_t, v1k_p, v1k_t, v1f_p, v1f_t, v0k_p, v0k_t, v0f_p,
           v0f_t, neg_p, neg_t, (g_fail == 0) ? "ALL PASS" : "FAILURES");

    printf("PASS %d/%d\n", g_pass, g_pass + g_fail);

    return (g_fail == 0) ? 0 : 1;
}
