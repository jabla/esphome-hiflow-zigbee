/*
 * hiflow_frame.h — HiFlow-BLE frame + key-derivation layer (C99 port).
 *
 * Byte-for-byte port of TheTiEr/hiflow-ble (const.py, crypt_util.py, frame.py).
 * No dynamic allocation; every output uses a caller-owned (buffer, capacity,
 * length-out) triple. Fixed-size buffers only, safe for embedded use.
 *
 * On-wire header (both V0 and V1), all multi-byte fields big-endian:
 *
 *   [0:2]   "HM" magic (0x48 0x4D)
 *   [2:4]   cmd   (uint16 BE)
 *   [4:6]   tid   (uint16 BE)
 *   [6:8]   CRC16-Modbus of the ciphertext (uint16 BE)
 *   [8:10]  length = len(ciphertext) + 10   (excludes the V1 tag)
 *   [10:N]  ciphertext
 *   [N:N+16] AES-128-GCM tag (V1 only)
 */
#ifndef HIFLOW_FRAME_H
#define HIFLOW_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HIFLOW_HEADER_LEN   10
#define HIFLOW_MAGIC0       0x48u /* 'H' */
#define HIFLOW_MAGIC1       0x4Du /* 'M' */
#define HIFLOW_KEY_LEN      16
#define HIFLOW_IV_LEN       16
#define HIFLOW_NONCE_LEN    12
#define HIFLOW_AAD_LEN      4
#define HIFLOW_TAG_LEN      16
#define HIFLOW_SHA256_LEN   32
#define HIFLOW_ENC_RAND_LEN 16
#define HIFLOW_SN_LEN       12

/* Largest plaintext accepted when building a V0 frame (PKCS#7 pad on top). */
#ifndef HIFLOW_MAX_PLAINTEXT
#define HIFLOW_MAX_PLAINTEXT 512
#endif

/* Status / error codes. */
typedef enum {
    HIFLOW_OK             = 0,
    HIFLOW_ERR_BAD_MAGIC  = -1, /* header does not start with "HM"           */
    HIFLOW_ERR_TOO_SHORT  = -2, /* buffer shorter than header / declared len */
    HIFLOW_ERR_BAD_LENGTH = -3, /* length field inconsistent with buffer     */
    HIFLOW_ERR_CRC        = -4, /* CRC16-Modbus over ciphertext mismatch     */
    HIFLOW_ERR_GCM_TAG    = -5, /* GCM authenticator rejected (stale/tamper) */
    HIFLOW_ERR_BUFFER     = -6, /* caller output buffer too small            */
    HIFLOW_ERR_PADDING    = -7, /* V0 PKCS#7 padding invalid                 */
    HIFLOW_ERR_CRYPTO     = -8, /* underlying crypto backend failure         */
    HIFLOW_ERR_ARG        = -9, /* invalid argument (NULL, len 0, ...)       */
    HIFLOW_ERR_FIELD      = -10 /* decrypted payload lacks an expected field  */
} hiflow_status_t;

/* ---------- primitives ---------- */

/* CRC16-Modbus: poly 0xA001, init 0xFFFF, no final XOR. */
uint16_t hiflow_crc16_modbus(const uint8_t *data, size_t len);

/* sha3(x) == SHA256(SHA256(SHA256(x))). Writes HIFLOW_SHA256_LEN bytes. */
void hiflow_sha3_256(const uint8_t *in, size_t len,
                     uint8_t out[HIFLOW_SHA256_LEN]);

/* ---------- V1: encRand-keyed AES-128-GCM ---------- */

/* key   = sha3(encRand)[0:16]                                   (static)    */
void hiflow_v1_derive_key(const uint8_t enc_rand[HIFLOW_ENC_RAND_LEN],
                          uint8_t key[HIFLOW_KEY_LEN]);
/* nonce = sha3(cmd_LE || tid_LE || encRand)[20:32]              (per frame) */
void hiflow_v1_derive_nonce(const uint8_t enc_rand[HIFLOW_ENC_RAND_LEN],
                            uint16_t cmd, uint16_t tid,
                            uint8_t nonce[HIFLOW_NONCE_LEN]);
/* aad   = cmd_LE || tid_LE                                                 */
void hiflow_v1_derive_aad(uint16_t cmd, uint16_t tid,
                          uint8_t aad[HIFLOW_AAD_LEN]);

/* Build header + ct + tag. Returns HIFLOW_OK, else a hiflow_status_t. */
int hiflow_build_frame_v1(const uint8_t enc_rand[HIFLOW_ENC_RAND_LEN],
                          uint16_t cmd, uint16_t tid,
                          const uint8_t *plaintext, size_t pt_len,
                          uint8_t *buf, size_t cap, size_t *out_len);

/* Parse a V1 frame; on HIFLOW_OK fills cmd/tid and the recovered plaintext. */
int hiflow_parse_frame_v1(const uint8_t enc_rand[HIFLOW_ENC_RAND_LEN],
                          const uint8_t *buf, size_t len,
                          uint16_t *cmd, uint16_t *tid,
                          uint8_t *pt_out, size_t pt_cap, size_t *pt_len);

/* ---------- V0: SN-keyed AES-128-CBC + PKCS#7 (pairing only) ---------- */

/* key = sha3(sn || "Hoymiles@#123456")[0:16]                     (static)   */
void hiflow_v0_derive_key(const char *sn, uint8_t key[HIFLOW_KEY_LEN]);
/* iv  = sha3(cmd_BE || tid_BE || sn)[16:32]                      (per frame) */
void hiflow_v0_derive_iv(const char *sn, uint16_t cmd, uint16_t tid,
                         uint8_t iv[HIFLOW_IV_LEN]);

int hiflow_build_frame_v0(const char *sn, uint16_t cmd, uint16_t tid,
                          const uint8_t *plaintext, size_t pt_len,
                          uint8_t *buf, size_t cap, size_t *out_len);

int hiflow_parse_frame_v0(const char *sn,
                          const uint8_t *buf, size_t len,
                          uint16_t *cmd, uint16_t *tid,
                          uint8_t *pt_out, size_t pt_cap, size_t *pt_len);

#ifdef __cplusplus
}
#endif

#endif /* HIFLOW_FRAME_H */
