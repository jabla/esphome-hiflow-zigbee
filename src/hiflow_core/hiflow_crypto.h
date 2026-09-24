/*
 * hiflow_crypto.h — thin crypto abstraction for the HiFlow-BLE C port.
 *
 * The frame/protocol layer (hiflow_frame.c) only ever talks to this header.
 * Two backends implement it:
 *
 *   hiflow_crypto_openssl.c  — host tests / desktop tooling (OpenSSL EVP)
 *   hiflow_crypto_mbedtls.c  — target firmware (ESP-IDF / mbedTLS)
 *
 * Exactly one backend is compiled in, chosen at build time via a preprocessor
 * define (per Makefile / CMake, never runtime detection):
 *
 *   -DHIFLOW_CRYPTO_BACKEND_OPENSSL
 *   -DHIFLOW_CRYPTO_BACKEND_MBEDTLS
 *
 * No dynamic allocation. All output buffers are caller-owned.
 */
#ifndef HIFLOW_CRYPTO_H
#define HIFLOW_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HIFLOW_CRYPTO_SHA256_LEN 32
#define HIFLOW_CRYPTO_KEY_LEN    16
#define HIFLOW_CRYPTO_IV_LEN     16
#define HIFLOW_CRYPTO_NONCE_LEN  12
#define HIFLOW_CRYPTO_TAG_LEN    16

/* Generic status: 0 == success, non-zero == failure. Backends never abort. */
#define HIFLOW_CRYPTO_OK   0
#define HIFLOW_CRYPTO_FAIL 1

/* SHA-256. out must hold HIFLOW_CRYPTO_SHA256_LEN bytes. Returns 0 on success. */
int hiflow_crypto_sha256(const uint8_t *in, size_t len,
                         uint8_t out[HIFLOW_CRYPTO_SHA256_LEN]);

/*
 * AES-128-CBC, no padding applied here (the frame layer does PKCS#7 itself).
 * len must be a non-zero multiple of 16. in and out may alias.
 * Returns 0 on success.
 */
int hiflow_crypto_aes128_cbc_encrypt(const uint8_t key[HIFLOW_CRYPTO_KEY_LEN],
                                     const uint8_t iv[HIFLOW_CRYPTO_IV_LEN],
                                     const uint8_t *in, size_t len,
                                     uint8_t *out);

int hiflow_crypto_aes128_cbc_decrypt(const uint8_t key[HIFLOW_CRYPTO_KEY_LEN],
                                     const uint8_t iv[HIFLOW_CRYPTO_IV_LEN],
                                     const uint8_t *in, size_t len,
                                     uint8_t *out);

/*
 * AES-128-GCM.
 *   encrypt: writes len ciphertext bytes to out and 16 tag bytes to tag.
 *   decrypt: verifies tag (16 bytes) while decrypting; returns HIFLOW_CRYPTO_OK
 *            only if the tag authenticates. On auth failure the contents of out
 *            are unspecified and the return is non-zero.
 */
int hiflow_crypto_aes128_gcm_encrypt(const uint8_t key[HIFLOW_CRYPTO_KEY_LEN],
                                     const uint8_t nonce[HIFLOW_CRYPTO_NONCE_LEN],
                                     const uint8_t *aad, size_t aad_len,
                                     const uint8_t *in, size_t len,
                                     uint8_t *out,
                                     uint8_t tag[HIFLOW_CRYPTO_TAG_LEN]);

int hiflow_crypto_aes128_gcm_decrypt(const uint8_t key[HIFLOW_CRYPTO_KEY_LEN],
                                     const uint8_t nonce[HIFLOW_CRYPTO_NONCE_LEN],
                                     const uint8_t *aad, size_t aad_len,
                                     const uint8_t *in, size_t len,
                                     const uint8_t tag[HIFLOW_CRYPTO_TAG_LEN],
                                     uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HIFLOW_CRYPTO_H */
