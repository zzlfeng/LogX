#include "mbedtls/gcm.h"
#include "mbedtls/platform_util.h"
#include <string.h>

#ifndef GET_UINT32_BE
#define GET_UINT32_BE(n,b,i)                            \
{                                                       \
    (n) = ( (uint32_t) (b)[(i)    ] << 24 )             \
        | ( (uint32_t) (b)[(i) + 1] << 16 )             \
        | ( (uint32_t) (b)[(i) + 2] <<  8 )             \
        | ( (uint32_t) (b)[(i) + 3]       );            \
}
#endif

#ifndef PUT_UINT32_BE
#define PUT_UINT32_BE(n,b,i)                            \
{                                                       \
    (b)[(i)    ] = (unsigned char) ( ( (n) >> 24 ) & 0xFF );    \
    (b)[(i) + 1] = (unsigned char) ( ( (n) >> 16 ) & 0xFF );    \
    (b)[(i) + 2] = (unsigned char) ( ( (n) >>  8 ) & 0xFF );    \
    (b)[(i) + 3] = (unsigned char) ( ( (n)       ) & 0xFF );    \
}
#endif

#ifndef GET_UINT64_BE
#define GET_UINT64_BE(n,b,i)                            \
{                                                       \
    (n) = ( (uint64_t) (b)[(i)    ] << 56 )             \
        | ( (uint64_t) (b)[(i) + 1] << 48 )             \
        | ( (uint64_t) (b)[(i) + 2] << 40 )             \
        | ( (uint64_t) (b)[(i) + 3] << 32 )             \
        | ( (uint64_t) (b)[(i) + 4] << 24 )             \
        | ( (uint64_t) (b)[(i) + 5] << 16 )             \
        | ( (uint64_t) (b)[(i) + 6] <<  8 )             \
        | ( (uint64_t) (b)[(i) + 7]       );            \
}
#endif

#ifndef PUT_UINT64_BE
#define PUT_UINT64_BE(n,b,i)                            \
{                                                       \
    (b)[(i)    ] = (unsigned char) ( ( (n) >> 56 ) & 0xFF );    \
    (b)[(i) + 1] = (unsigned char) ( ( (n) >> 48 ) & 0xFF );    \
    (b)[(i) + 2] = (unsigned char) ( ( (n) >> 40 ) & 0xFF );    \
    (b)[(i) + 3] = (unsigned char) ( ( (n) >> 32 ) & 0xFF );    \
    (b)[(i) + 4] = (unsigned char) ( ( (n) >> 24 ) & 0xFF );    \
    (b)[(i) + 5] = (unsigned char) ( ( (n) >> 16 ) & 0xFF );    \
    (b)[(i) + 6] = (unsigned char) ( ( (n) >>  8 ) & 0xFF );    \
    (b)[(i) + 7] = (unsigned char) ( ( (n)       ) & 0xFF );    \
}
#endif

/* Precalculated polynomial multiplication table */
static const uint64_t last4[16] = {
    0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
    0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0
};

static void gcm_mult(mbedtls_gcm_context *ctx, const unsigned char x[16], unsigned char output[16]) {
    int i;
    unsigned char lo, hi, rem;
    uint64_t zh, zl;

    lo = (unsigned char)(x[15] & 0x0f);
    zh = ctx->HH[lo];
    zl = ctx->HL[lo];

    for (i = 15; i >= 0; i--) {
        lo = (unsigned char)(x[i] & 0x0f);
        hi = (unsigned char)((x[i] >> 4) & 0x0f);

        if (i != 15) {
            rem = (unsigned char)(zl & 0x0f);
            zl = (zh << 60) | (zl >> 4);
            zh = (zh >> 4);
            zh ^= (uint64_t)last4[rem] << 48;
            zh ^= ctx->HH[lo];
            zl ^= ctx->HL[lo];
        }

        rem = (unsigned char)(zl & 0x0f);
        zl = (zh << 60) | (zl >> 4);
        zh = (zh >> 4);
        zh ^= (uint64_t)last4[rem] << 48;
        zh ^= ctx->HH[hi];
        zl ^= ctx->HL[hi];
    }

    PUT_UINT64_BE(zh, output, 0);
    PUT_UINT64_BE(zl, output, 8);
}

void mbedtls_gcm_init(mbedtls_gcm_context *ctx) {
    if (ctx == NULL) return;
    memset(ctx, 0, sizeof(mbedtls_gcm_context));
    mbedtls_aes_init(&ctx->aes_ctx);
}

void mbedtls_gcm_free(mbedtls_gcm_context *ctx) {
    if (ctx == NULL) return;
    mbedtls_aes_free(&ctx->aes_ctx);
    mbedtls_platform_zeroize(ctx, sizeof(mbedtls_gcm_context));
}

int mbedtls_gcm_setkey(mbedtls_gcm_context *ctx,
                       mbedtls_cipher_id_t cipher,
                       const unsigned char *key,
                       unsigned int keybits) {
    unsigned char h[16];
    uint64_t vl, vh;
    int i, j, ret;

    if (ctx == NULL || key == NULL) return MBEDTLS_ERR_GCM_BAD_INPUT;
    (void) cipher;

    mbedtls_aes_free(&ctx->aes_ctx);
    mbedtls_aes_init(&ctx->aes_ctx);
    ret = mbedtls_aes_setkey_enc(&ctx->aes_ctx, key, keybits);
    if (ret != 0) return ret;

    /* Compute H = AES_K(0^128) */
    memset(h, 0, 16);
    mbedtls_aes_crypt_ecb(&ctx->aes_ctx, MBEDTLS_AES_ENCRYPT, h, h);

    GET_UINT64_BE(vh, h, 0);
    GET_UINT64_BE(vl, h, 8);

    ctx->HL[8] = vl;
    ctx->HH[8] = vh;

    ctx->HH[0] = 0;
    ctx->HL[0] = 0;

    for (i = 4; i > 0; i >>= 1) {
        uint32_t T = (uint32_t)(vl & 1) * 0xe1000000U;
        vl = (vh << 63) | (vl >> 1);
        vh = (vh >> 1) ^ ((uint64_t)T << 32);

        ctx->HL[i] = vl;
        ctx->HH[i] = vh;
    }

    for (i = 2; i <= 8; i *= 2) {
        vh = ctx->HH[i];
        vl = ctx->HL[i];
        for (j = 1; j < i; j++) {
            ctx->HH[i + j] = vh ^ ctx->HH[j];
            ctx->HL[i + j] = vl ^ ctx->HL[j];
        }
    }

    return 0;
}

static int gcm_starts(mbedtls_gcm_context *ctx,
                      int mode,
                      const unsigned char *iv,
                      size_t iv_len,
                      const unsigned char *add,
                      size_t add_len) {
    size_t i;

    if (ctx == NULL || iv == NULL) return MBEDTLS_ERR_GCM_BAD_INPUT;
    if (add_len > 0 && add == NULL) return MBEDTLS_ERR_GCM_BAD_INPUT;
    if (iv_len != 12) {
        /* Standard GCM for LogX uses 12-byte IV/Nonce */
        return MBEDTLS_ERR_GCM_BAD_INPUT;
    }

    ctx->mode = mode;
    ctx->len = 0;
    ctx->add_len = add_len;
    memset(ctx->y, 0, 16);
    memset(ctx->buf, 0, 16);

    /* For 96-bit IV: J0 = IV || 0^31 || 1 */
    memcpy(ctx->base_ectr, iv, 12);
    ctx->base_ectr[12] = 0;
    ctx->base_ectr[13] = 0;
    ctx->base_ectr[14] = 0;
    ctx->base_ectr[15] = 1;

    /* Process additional authenticated data */
    while (add_len > 0) {
        size_t use_len = (add_len < 16) ? add_len : 16;
        for (i = 0; i < use_len; i++) {
            ctx->y[i] ^= add[i];
        }
        gcm_mult(ctx, ctx->y, ctx->y);
        add += use_len;
        add_len -= use_len;
    }

    return 0;
}

int mbedtls_gcm_crypt_and_tag(mbedtls_gcm_context *ctx,
                              int mode,
                              size_t length,
                              const unsigned char *iv,
                              size_t iv_len,
                              const unsigned char *add,
                              size_t add_len,
                              const unsigned char *input,
                              unsigned char *output,
                              size_t tag_len,
                              unsigned char *tag) {
    int ret;
    size_t i, pos;
    unsigned char ctr[16];
    unsigned char ectr[16];
    unsigned char work_buf[16];
    uint64_t orig_len = length;

    if (ctx == NULL || iv == NULL || tag == NULL) return MBEDTLS_ERR_GCM_BAD_INPUT;
    if (length > 0 && (input == NULL || output == NULL)) return MBEDTLS_ERR_GCM_BAD_INPUT;
    if (add_len > 0 && add == NULL) return MBEDTLS_ERR_GCM_BAD_INPUT;

    ret = gcm_starts(ctx, mode, iv, iv_len, add, add_len);
    if (ret != 0) return ret;

    memcpy(ctr, ctx->base_ectr, 16);

    pos = 0;
    while (length > 0) {
        size_t use_len = (length < 16) ? length : 16;

        /* Increment counter (standard 32-bit big-endian increment) */
        for (i = 16; i > 12; i--) {
            if (++ctr[i - 1] != 0) break;
        }

        mbedtls_aes_crypt_ecb(&ctx->aes_ctx, MBEDTLS_AES_ENCRYPT, ctr, ectr);

        if (mode == MBEDTLS_GCM_ENCRYPT) {
            for (i = 0; i < use_len; i++) {
                output[pos + i] = (unsigned char)(input[pos + i] ^ ectr[i]);
                ctx->y[i] ^= output[pos + i];
            }
        } else {
            for (i = 0; i < use_len; i++) {
                ctx->y[i] ^= input[pos + i];
                output[pos + i] = (unsigned char)(input[pos + i] ^ ectr[i]);
            }
        }

        gcm_mult(ctx, ctx->y, ctx->y);

        pos += use_len;
        length -= use_len;
    }

    /* Compute GCTR_K(J0, S) */
    mbedtls_aes_crypt_ecb(&ctx->aes_ctx, MBEDTLS_AES_ENCRYPT, ctx->base_ectr, ectr);

    /* Form final block: [len(A)]_64 || [len(C)]_64 in bits */
    PUT_UINT64_BE(ctx->add_len * 8, work_buf, 0);
    PUT_UINT64_BE(orig_len * 8, work_buf, 8);

    for (i = 0; i < 16; i++) {
        ctx->y[i] ^= work_buf[i];
    }
    gcm_mult(ctx, ctx->y, ctx->y);

    for (i = 0; i < tag_len && i < 16; i++) {
        tag[i] = (unsigned char)(ctx->y[i] ^ ectr[i]);
    }

    return 0;
}

int mbedtls_gcm_auth_decrypt(mbedtls_gcm_context *ctx,
                             size_t length,
                             const unsigned char *iv,
                             size_t iv_len,
                             const unsigned char *add,
                             size_t add_len,
                             const unsigned char *tag,
                             size_t tag_len,
                             const unsigned char *input,
                             unsigned char *output) {
    int ret;
    unsigned char check_tag[16];
    size_t i;
    unsigned char diff = 0;

    if (tag == NULL || tag_len > 16 || tag_len == 0) return MBEDTLS_ERR_GCM_BAD_INPUT;

    ret = mbedtls_gcm_crypt_and_tag(ctx, MBEDTLS_GCM_DECRYPT, length,
                                    iv, iv_len, add, add_len,
                                    input, output, tag_len, check_tag);
    if (ret != 0) return ret;

    /* Constant-time comparison */
    for (i = 0; i < tag_len; i++) {
        diff |= (tag[i] ^ check_tag[i]);
    }

    if (diff != 0) {
        if (output && length > 0) {
            mbedtls_platform_zeroize(output, length);
        }
        return MBEDTLS_ERR_GCM_AUTH_FAILED;
    }

    return 0;
}
