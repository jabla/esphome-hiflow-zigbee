/*
 * hiflow_frame.c — HiFlow-BLE frame + key-derivation layer (C99 port).
 *
 * Byte-for-byte port of TheTiEr/hiflow-ble:
 *   crc_util  -> hiflow_crc16_modbus
 *   crypt_util->_sha3 / derive_*  -> hiflow_sha3_256 / hiflow_v1_* / hiflow_v0_*
 *   frame.py  -> hiflow_build_frame_v1 / hiflow_parse_frame_v1
 *                hiflow_build_frame_v0 / hiflow_parse_frame_v0
 */

#include "hiflow_frame.h"
#include "hiflow_crypto.h"

#include <string.h>

/* V0 salt, identical to const.SALT_V0 (16 bytes, not NUL-terminated). */
static const char HIFLOW_SALT_V0[] = "Hoymiles@#123456";
#define HIFLOW_SALT_V0_LEN 16

/* ---------- small helpers ---------- */

static uint16_t rd_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void wr_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

/* ---------- primitives ---------- */

uint16_t hiflow_crc16_modbus(const uint8_t *data, size_t len)
{
    /* poly 0xA001, init 0xFFFF, no final XOR — bitwise form of the table
     * implementation in frame.py (numerically identical). */
    uint16_t crc = 0xFFFF;
    size_t i;
    int bit;

    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (bit = 0; bit < 8; bit++) {
            if (crc & 0x0001u)
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            else
                crc = (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

void hiflow_sha3_256(const uint8_t *in, size_t len,
                     uint8_t out[HIFLOW_SHA256_LEN])
{
    uint8_t tmp1[HIFLOW_SHA256_LEN];
    uint8_t tmp2[HIFLOW_SHA256_LEN];

    hiflow_crypto_sha256(in, len, tmp1);
    hiflow_crypto_sha256(tmp1, HIFLOW_SHA256_LEN, tmp2);
    hiflow_crypto_sha256(tmp2, HIFLOW_SHA256_LEN, out);
}

/* ---------- V1 key derivation ---------- */

void hiflow_v1_derive_key(const uint8_t enc_rand[HIFLOW_ENC_RAND_LEN],
                          uint8_t key[HIFLOW_KEY_LEN])
{
    uint8_t d[HIFLOW_SHA256_LEN];

    hiflow_sha3_256(enc_rand, HIFLOW_ENC_RAND_LEN, d);
    memcpy(key, d, HIFLOW_KEY_LEN); /* _sha3(enc_rand)[:16] */
}

void hiflow_v1_derive_nonce(const uint8_t enc_rand[HIFLOW_ENC_RAND_LEN],
                            uint16_t cmd, uint16_t tid,
                            uint8_t nonce[HIFLOW_NONCE_LEN])
{
    /* struct.pack("<HH", cmd, tid) + enc_rand  -> 20 bytes */
    uint8_t in[4 + HIFLOW_ENC_RAND_LEN];
    uint8_t d[HIFLOW_SHA256_LEN];

    in[0] = (uint8_t)(cmd & 0xFF);
    in[1] = (uint8_t)(cmd >> 8);
    in[2] = (uint8_t)(tid & 0xFF);
    in[3] = (uint8_t)(tid >> 8);
    memcpy(in + 4, enc_rand, HIFLOW_ENC_RAND_LEN);

    hiflow_sha3_256(in, sizeof(in), d);
    memcpy(nonce, d + 20, HIFLOW_NONCE_LEN); /* _sha3(...)[20:32] */
}

void hiflow_v1_derive_aad(uint16_t cmd, uint16_t tid,
                          uint8_t aad[HIFLOW_AAD_LEN])
{
    /* struct.pack("<HH", cmd, tid) */
    aad[0] = (uint8_t)(cmd & 0xFF);
    aad[1] = (uint8_t)(cmd >> 8);
    aad[2] = (uint8_t)(tid & 0xFF);
    aad[3] = (uint8_t)(tid >> 8);
}

/* ---------- V1 build / parse ---------- */

int hiflow_build_frame_v1(const uint8_t enc_rand[HIFLOW_ENC_RAND_LEN],
                          uint16_t cmd, uint16_t tid,
                          const uint8_t *plaintext, size_t pt_len,
                          uint8_t *buf, size_t cap, size_t *out_len)
{
    uint8_t key[HIFLOW_KEY_LEN];
    uint8_t nonce[HIFLOW_NONCE_LEN];
    uint8_t aad[HIFLOW_AAD_LEN];
    uint8_t *ct;
    uint8_t *tag;
    uint16_t crc;
    size_t total;

    if (!enc_rand || !buf || !out_len || (!plaintext && pt_len))
        return HIFLOW_ERR_ARG;

    total = HIFLOW_HEADER_LEN + pt_len + HIFLOW_TAG_LEN;
    if (cap < total)
        return HIFLOW_ERR_BUFFER;

    hiflow_v1_derive_key(enc_rand, key);
    hiflow_v1_derive_nonce(enc_rand, cmd, tid, nonce);
    hiflow_v1_derive_aad(cmd, tid, aad);

    ct = buf + HIFLOW_HEADER_LEN;
    tag = ct + pt_len;

    if (hiflow_crypto_aes128_gcm_encrypt(key, nonce, aad, HIFLOW_AAD_LEN,
                                         plaintext, pt_len, ct, tag) !=
        HIFLOW_CRYPTO_OK)
        return HIFLOW_ERR_CRYPTO;

    crc = hiflow_crc16_modbus(ct, pt_len);

    buf[0] = HIFLOW_MAGIC0;
    buf[1] = HIFLOW_MAGIC1;
    wr_be16(buf + 2, cmd);
    wr_be16(buf + 4, tid);
    wr_be16(buf + 6, crc);
    wr_be16(buf + 8, (uint16_t)(pt_len + HIFLOW_HEADER_LEN));

    *out_len = total;
    return HIFLOW_OK;
}

int hiflow_parse_frame_v1(const uint8_t enc_rand[HIFLOW_ENC_RAND_LEN],
                          const uint8_t *buf, size_t len,
                          uint16_t *cmd, uint16_t *tid,
                          uint8_t *pt_out, size_t pt_cap, size_t *pt_len)
{
    uint8_t key[HIFLOW_KEY_LEN];
    uint8_t nonce[HIFLOW_NONCE_LEN];
    uint8_t aad[HIFLOW_AAD_LEN];
    uint16_t c_cmd, c_tid, c_crc, c_len;
    size_t ct_len;
    const uint8_t *ct;
    const uint8_t *tag;

    if (!enc_rand || !buf)
        return HIFLOW_ERR_ARG;
    if (len < HIFLOW_HEADER_LEN)
        return HIFLOW_ERR_TOO_SHORT;
    if (buf[0] != HIFLOW_MAGIC0 || buf[1] != HIFLOW_MAGIC1)
        return HIFLOW_ERR_BAD_MAGIC;

    c_cmd = rd_be16(buf + 2);
    c_tid = rd_be16(buf + 4);
    c_crc = rd_be16(buf + 6);
    c_len = rd_be16(buf + 8);

    if (c_len < HIFLOW_HEADER_LEN)
        return HIFLOW_ERR_BAD_LENGTH;
    ct_len = (size_t)c_len - HIFLOW_HEADER_LEN;
    if (len < HIFLOW_HEADER_LEN + ct_len + HIFLOW_TAG_LEN)
        return HIFLOW_ERR_TOO_SHORT;

    ct = buf + HIFLOW_HEADER_LEN;
    tag = ct + ct_len;

    if (hiflow_crc16_modbus(ct, ct_len) != c_crc)
        return HIFLOW_ERR_CRC;

    if (!pt_out || !pt_len)
        return HIFLOW_ERR_ARG;
    if (pt_cap < ct_len)
        return HIFLOW_ERR_BUFFER;

    hiflow_v1_derive_key(enc_rand, key);
    hiflow_v1_derive_nonce(enc_rand, c_cmd, c_tid, nonce);
    hiflow_v1_derive_aad(c_cmd, c_tid, aad);

    if (hiflow_crypto_aes128_gcm_decrypt(key, nonce, aad, HIFLOW_AAD_LEN,
                                         ct, ct_len, tag, pt_out) !=
        HIFLOW_CRYPTO_OK)
        return HIFLOW_ERR_GCM_TAG;

    if (cmd)
        *cmd = c_cmd;
    if (tid)
        *tid = c_tid;
    *pt_len = ct_len;
    return HIFLOW_OK;
}

/* ---------- V0 key derivation ---------- */

void hiflow_v0_derive_key(const char *sn, uint8_t key[HIFLOW_KEY_LEN])
{
    uint8_t in[HIFLOW_SN_LEN + HIFLOW_SALT_V0_LEN];
    uint8_t d[HIFLOW_SHA256_LEN];

    memcpy(in, sn, HIFLOW_SN_LEN);
    memcpy(in + HIFLOW_SN_LEN, HIFLOW_SALT_V0, HIFLOW_SALT_V0_LEN);
    hiflow_sha3_256(in, sizeof(in), d);
    memcpy(key, d, HIFLOW_KEY_LEN); /* _sha3(sn + SALT_V0)[:16] */
}

void hiflow_v0_derive_iv(const char *sn, uint16_t cmd, uint16_t tid,
                         uint8_t iv[HIFLOW_IV_LEN])
{
    /* struct.pack(">HH", cmd, tid) + sn.encode()  -> 16 bytes */
    uint8_t in[4 + HIFLOW_SN_LEN];
    uint8_t d[HIFLOW_SHA256_LEN];

    in[0] = (uint8_t)(cmd >> 8);
    in[1] = (uint8_t)(cmd & 0xFF);
    in[2] = (uint8_t)(tid >> 8);
    in[3] = (uint8_t)(tid & 0xFF);
    memcpy(in + 4, sn, HIFLOW_SN_LEN);

    hiflow_sha3_256(in, sizeof(in), d);
    memcpy(iv, d + 16, HIFLOW_IV_LEN); /* _sha3(...)[16:32] */
}

/* ---------- V0 build / parse (PKCS#7, block size 16) ---------- */

int hiflow_build_frame_v0(const char *sn, uint16_t cmd, uint16_t tid,
                          const uint8_t *plaintext, size_t pt_len,
                          uint8_t *buf, size_t cap, size_t *out_len)
{
    uint8_t key[HIFLOW_KEY_LEN];
    uint8_t iv[HIFLOW_IV_LEN];
    uint8_t padded[HIFLOW_MAX_PLAINTEXT + 16];
    size_t pad_len, padded_len, total;
    uint8_t *ct;
    uint16_t crc;
    size_t i;

    if (!sn || !buf || !out_len || (!plaintext && pt_len))
        return HIFLOW_ERR_ARG;
    if (pt_len > HIFLOW_MAX_PLAINTEXT)
        return HIFLOW_ERR_BUFFER;

    /* PKCS#7: always append between 1 and 16 bytes. */
    pad_len = 16 - (pt_len % 16);
    padded_len = pt_len + pad_len;
    if (pt_len)
        memcpy(padded, plaintext, pt_len);
    for (i = 0; i < pad_len; i++)
        padded[pt_len + i] = (uint8_t)pad_len;

    total = HIFLOW_HEADER_LEN + padded_len;
    if (cap < total)
        return HIFLOW_ERR_BUFFER;

    hiflow_v0_derive_key(sn, key);
    hiflow_v0_derive_iv(sn, cmd, tid, iv);

    ct = buf + HIFLOW_HEADER_LEN;
    if (hiflow_crypto_aes128_cbc_encrypt(key, iv, padded, padded_len, ct) !=
        HIFLOW_CRYPTO_OK)
        return HIFLOW_ERR_CRYPTO;

    crc = hiflow_crc16_modbus(ct, padded_len);

    buf[0] = HIFLOW_MAGIC0;
    buf[1] = HIFLOW_MAGIC1;
    wr_be16(buf + 2, cmd);
    wr_be16(buf + 4, tid);
    wr_be16(buf + 6, crc);
    wr_be16(buf + 8, (uint16_t)(padded_len + HIFLOW_HEADER_LEN));

    *out_len = total;
    return HIFLOW_OK;
}

int hiflow_parse_frame_v0(const char *sn,
                          const uint8_t *buf, size_t len,
                          uint16_t *cmd, uint16_t *tid,
                          uint8_t *pt_out, size_t pt_cap, size_t *pt_len)
{
    uint8_t key[HIFLOW_KEY_LEN];
    uint8_t iv[HIFLOW_IV_LEN];
    uint16_t c_cmd, c_tid, c_crc, c_len;
    size_t ct_len;
    const uint8_t *ct;
    uint8_t pad;
    size_t i;

    if (!sn || !buf)
        return HIFLOW_ERR_ARG;
    if (len < HIFLOW_HEADER_LEN)
        return HIFLOW_ERR_TOO_SHORT;
    if (buf[0] != HIFLOW_MAGIC0 || buf[1] != HIFLOW_MAGIC1)
        return HIFLOW_ERR_BAD_MAGIC;

    c_cmd = rd_be16(buf + 2);
    c_tid = rd_be16(buf + 4);
    c_crc = rd_be16(buf + 6);
    c_len = rd_be16(buf + 8);

    if (c_len < HIFLOW_HEADER_LEN)
        return HIFLOW_ERR_BAD_LENGTH;
    ct_len = (size_t)c_len - HIFLOW_HEADER_LEN;
    if (ct_len == 0 || (ct_len % 16u) != 0)
        return HIFLOW_ERR_BAD_LENGTH;
    if (len < HIFLOW_HEADER_LEN + ct_len)
        return HIFLOW_ERR_TOO_SHORT;

    ct = buf + HIFLOW_HEADER_LEN;
    if (hiflow_crc16_modbus(ct, ct_len) != c_crc)
        return HIFLOW_ERR_CRC;

    if (!pt_out || !pt_len)
        return HIFLOW_ERR_ARG;
    if (pt_cap < ct_len)
        return HIFLOW_ERR_BUFFER;

    hiflow_v0_derive_key(sn, key);
    hiflow_v0_derive_iv(sn, c_cmd, c_tid, iv);

    if (hiflow_crypto_aes128_cbc_decrypt(key, iv, ct, ct_len, pt_out) !=
        HIFLOW_CRYPTO_OK)
        return HIFLOW_ERR_CRYPTO;

    /* PKCS#7 unpad. */
    pad = pt_out[ct_len - 1];
    if (pad < 1 || pad > 16 || (size_t)pad > ct_len)
        return HIFLOW_ERR_PADDING;
    for (i = 0; i < pad; i++) {
        if (pt_out[ct_len - 1 - i] != pad)
            return HIFLOW_ERR_PADDING;
    }

    if (cmd)
        *cmd = c_cmd;
    if (tid)
        *tid = c_tid;
    *pt_len = ct_len - pad;
    return HIFLOW_OK;
}
