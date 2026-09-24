/*
 * hiflow_crypto_mbedtls.c — mbedTLS backend (ESP-IDF target firmware).
 *
 * Build with: -DHIFLOW_CRYPTO_BACKEND_MBEDTLS
 * Requires mbedtls component (bundled with ESP-IDF): aes.h, gcm.h, sha256.h.
 *
 * This file is intentionally NOT compiled during host tests, but implements
 * the exact same hiflow_crypto.h contract as the OpenSSL backend so the frame
 * layer is byte-identical on target.
 */
#if defined(HIFLOW_CRYPTO_BACKEND_MBEDTLS)

#include "hiflow_crypto.h"

#include <mbedtls/aes.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>

int hiflow_crypto_sha256(const uint8_t *in, size_t len,
                         uint8_t out[HIFLOW_CRYPTO_SHA256_LEN])
{
    mbedtls_sha256_context ctx;
    int rc;

    mbedtls_sha256_init(&ctx);
    rc = mbedtls_sha256_starts(&ctx, 0); /* 0 = SHA-256 (not SHA-224) */
    if (rc == 0)
        rc = mbedtls_sha256_update(&ctx, in, len);
    if (rc == 0)
        rc = mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);

    return (rc == 0) ? HIFLOW_CRYPTO_OK : HIFLOW_CRYPTO_FAIL;
}

int hiflow_crypto_aes128_cbc_encrypt(const uint8_t key[HIFLOW_CRYPTO_KEY_LEN],
                                     const uint8_t iv[HIFLOW_CRYPTO_IV_LEN],
                                     const uint8_t *in, size_t len,
                                     uint8_t *out)
{
    mbedtls_aes_context aes;
    uint8_t iv_copy[HIFLOW_CRYPTO_IV_LEN];
    int rc;

    if (len == 0 || (len % 16u) != 0)
        return HIFLOW_CRYPTO_FAIL;
    for (size_t i = 0; i < HIFLOW_CRYPTO_IV_LEN; i++)
        iv_copy[i] = iv[i];

    mbedtls_aes_init(&aes);
    rc = mbedtls_aes_setkey_enc(&aes, key, 128);
    if (rc == 0)
        rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, len, iv_copy,
                                   in, out);
    mbedtls_aes_free(&aes);

    return (rc == 0) ? HIFLOW_CRYPTO_OK : HIFLOW_CRYPTO_FAIL;
}

int hiflow_crypto_aes128_cbc_decrypt(const uint8_t key[HIFLOW_CRYPTO_KEY_LEN],
                                     const uint8_t iv[HIFLOW_CRYPTO_IV_LEN],
                                     const uint8_t *in, size_t len,
                                     uint8_t *out)
{
    mbedtls_aes_context aes;
    uint8_t iv_copy[HIFLOW_CRYPTO_IV_LEN];
    int rc;

    if (len == 0 || (len % 16u) != 0)
        return HIFLOW_CRYPTO_FAIL;
    for (size_t i = 0; i < HIFLOW_CRYPTO_IV_LEN; i++)
        iv_copy[i] = iv[i];

    mbedtls_aes_init(&aes);
    rc = mbedtls_aes_setkey_dec(&aes, key, 128);
    if (rc == 0)
        rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, len, iv_copy,
                                   in, out);
    mbedtls_aes_free(&aes);

    return (rc == 0) ? HIFLOW_CRYPTO_OK : HIFLOW_CRYPTO_FAIL;
}

int hiflow_crypto_aes128_gcm_encrypt(const uint8_t key[HIFLOW_CRYPTO_KEY_LEN],
                                     const uint8_t nonce[HIFLOW_CRYPTO_NONCE_LEN],
                                     const uint8_t *aad, size_t aad_len,
                                     const uint8_t *in, size_t len,
                                     uint8_t *out,
                                     uint8_t tag[HIFLOW_CRYPTO_TAG_LEN])
{
    mbedtls_gcm_context ctx;
    int rc;

    mbedtls_gcm_init(&ctx);
    rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (rc == 0)
        rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, len, nonce,
                                       HIFLOW_CRYPTO_NONCE_LEN, aad, aad_len,
                                       in, out, HIFLOW_CRYPTO_TAG_LEN, tag);
    mbedtls_gcm_free(&ctx);

    return (rc == 0) ? HIFLOW_CRYPTO_OK : HIFLOW_CRYPTO_FAIL;
}

int hiflow_crypto_aes128_gcm_decrypt(const uint8_t key[HIFLOW_CRYPTO_KEY_LEN],
                                     const uint8_t nonce[HIFLOW_CRYPTO_NONCE_LEN],
                                     const uint8_t *aad, size_t aad_len,
                                     const uint8_t *in, size_t len,
                                     const uint8_t tag[HIFLOW_CRYPTO_TAG_LEN],
                                     uint8_t *out)
{
    mbedtls_gcm_context ctx;
    int rc;

    mbedtls_gcm_init(&ctx);
    rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (rc == 0)
        rc = mbedtls_gcm_auth_decrypt(&ctx, len, nonce,
                                      HIFLOW_CRYPTO_NONCE_LEN, aad, aad_len,
                                      tag, HIFLOW_CRYPTO_TAG_LEN, in, out);
    mbedtls_gcm_free(&ctx);

    return (rc == 0) ? HIFLOW_CRYPTO_OK : HIFLOW_CRYPTO_FAIL;
}

#endif /* HIFLOW_CRYPTO_BACKEND_MBEDTLS */
