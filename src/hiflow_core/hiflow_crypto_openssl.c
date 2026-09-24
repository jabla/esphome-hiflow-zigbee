/*
 * hiflow_crypto_openssl.c — OpenSSL EVP backend (host tests, desktop tooling).
 *
 * Build with: -DHIFLOW_CRYPTO_BACKEND_OPENSSL -lcrypto
 * OpenSSL 3.x: EVP only (the legacy mbedtls-style gcm.h API is gone).
 */
#if defined(HIFLOW_CRYPTO_BACKEND_OPENSSL)

#include "hiflow_crypto.h"

#include <openssl/evp.h>

int hiflow_crypto_sha256(const uint8_t *in, size_t len,
                         uint8_t out[HIFLOW_CRYPTO_SHA256_LEN])
{
    unsigned int out_len = 0;
    if (EVP_Digest(in, len, out, &out_len, EVP_sha256(), NULL) != 1)
        return HIFLOW_CRYPTO_FAIL;
    return (out_len == HIFLOW_CRYPTO_SHA256_LEN) ? HIFLOW_CRYPTO_OK
                                                 : HIFLOW_CRYPTO_FAIL;
}

int hiflow_crypto_aes128_cbc_encrypt(const uint8_t key[HIFLOW_CRYPTO_KEY_LEN],
                                     const uint8_t iv[HIFLOW_CRYPTO_IV_LEN],
                                     const uint8_t *in, size_t len,
                                     uint8_t *out)
{
    EVP_CIPHER_CTX *ctx = NULL;
    int n = 0;
    int total = 0;
    int rc = HIFLOW_CRYPTO_FAIL;

    if (len == 0 || (len % 16u) != 0)
        return HIFLOW_CRYPTO_FAIL;

    ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return HIFLOW_CRYPTO_FAIL;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, key, iv) != 1)
        goto done;
    /* Disable padding: the frame layer applies PKCS#7 itself. */
    if (EVP_CIPHER_CTX_set_padding(ctx, 0) != 1)
        goto done;
    if (EVP_EncryptUpdate(ctx, out, &n, in, (int)len) != 1)
        goto done;
    total = n;
    if (EVP_EncryptFinal_ex(ctx, out + total, &n) != 1)
        goto done;
    total += n;
    rc = ((size_t)total == len) ? HIFLOW_CRYPTO_OK : HIFLOW_CRYPTO_FAIL;

done:
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

int hiflow_crypto_aes128_cbc_decrypt(const uint8_t key[HIFLOW_CRYPTO_KEY_LEN],
                                     const uint8_t iv[HIFLOW_CRYPTO_IV_LEN],
                                     const uint8_t *in, size_t len,
                                     uint8_t *out)
{
    EVP_CIPHER_CTX *ctx = NULL;
    int n = 0;
    int total = 0;
    int rc = HIFLOW_CRYPTO_FAIL;

    if (len == 0 || (len % 16u) != 0)
        return HIFLOW_CRYPTO_FAIL;

    ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return HIFLOW_CRYPTO_FAIL;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, key, iv) != 1)
        goto done;
    if (EVP_CIPHER_CTX_set_padding(ctx, 0) != 1)
        goto done;
    if (EVP_DecryptUpdate(ctx, out, &n, in, (int)len) != 1)
        goto done;
    total = n;
    if (EVP_DecryptFinal_ex(ctx, out + total, &n) != 1)
        goto done;
    total += n;
    rc = ((size_t)total == len) ? HIFLOW_CRYPTO_OK : HIFLOW_CRYPTO_FAIL;

done:
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

int hiflow_crypto_aes128_gcm_encrypt(const uint8_t key[HIFLOW_CRYPTO_KEY_LEN],
                                     const uint8_t nonce[HIFLOW_CRYPTO_NONCE_LEN],
                                     const uint8_t *aad, size_t aad_len,
                                     const uint8_t *in, size_t len,
                                     uint8_t *out,
                                     uint8_t tag[HIFLOW_CRYPTO_TAG_LEN])
{
    EVP_CIPHER_CTX *ctx = NULL;
    int n = 0;
    int rc = HIFLOW_CRYPTO_FAIL;

    ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return HIFLOW_CRYPTO_FAIL;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL) != 1)
        goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                            HIFLOW_CRYPTO_NONCE_LEN, NULL) != 1)
        goto done;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1)
        goto done;
    if (aad_len) {
        if (EVP_EncryptUpdate(ctx, NULL, &n, aad, (int)aad_len) != 1)
            goto done;
    }
    if (len) {
        if (EVP_EncryptUpdate(ctx, out, &n, in, (int)len) != 1)
            goto done;
    }
    if (EVP_EncryptFinal_ex(ctx, out + (len ? n : 0), &n) != 1)
        goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG,
                            HIFLOW_CRYPTO_TAG_LEN, tag) != 1)
        goto done;
    rc = HIFLOW_CRYPTO_OK;

done:
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

int hiflow_crypto_aes128_gcm_decrypt(const uint8_t key[HIFLOW_CRYPTO_KEY_LEN],
                                     const uint8_t nonce[HIFLOW_CRYPTO_NONCE_LEN],
                                     const uint8_t *aad, size_t aad_len,
                                     const uint8_t *in, size_t len,
                                     const uint8_t tag[HIFLOW_CRYPTO_TAG_LEN],
                                     uint8_t *out)
{
    EVP_CIPHER_CTX *ctx = NULL;
    int n = 0;
    int rc = HIFLOW_CRYPTO_FAIL;

    ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return HIFLOW_CRYPTO_FAIL;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL) != 1)
        goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                            HIFLOW_CRYPTO_NONCE_LEN, NULL) != 1)
        goto done;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1)
        goto done;
    if (aad_len) {
        if (EVP_DecryptUpdate(ctx, NULL, &n, aad, (int)aad_len) != 1)
            goto done;
    }
    if (len) {
        if (EVP_DecryptUpdate(ctx, out, &n, in, (int)len) != 1)
            goto done;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                            HIFLOW_CRYPTO_TAG_LEN, (void *)tag) != 1)
        goto done;
    /* Final_ex returns <=0 on GCM authentication failure. */
    if (EVP_DecryptFinal_ex(ctx, out + (len ? n : 0), &n) != 1)
        goto done;
    rc = HIFLOW_CRYPTO_OK;

done:
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

#endif /* HIFLOW_CRYPTO_BACKEND_OPENSSL */
