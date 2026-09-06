/*
 * aesce.c - ARMv8 Crypto Extensions (AESE/AESMC) hardware-accelerated AES.
 *
 * Adapted for the trimmed vendored mbedtls used by LogX.
 * Only the ENCRYPT direction is implemented: GCM-CTR keystream generation
 * and GHASH H-block computation only ever invoke AES encryption.
 *
 * Implementation notes:
 * - Runtime CPU feature detection via getauxval(AT_HWCAP) & HWCAP_AES.
 * - Hardware AESE instruction executes: ShiftRows(SubBytes(state ^ round_key)).
 * - For rounds 0 to nr - 2 (first nr - 1 rounds):
 *     state = vaesmcq_u8(vaeseq_u8(state, rk[i]));
 * - Round nr - 1 (last sub-round, no MixColumns):
 *     state = vaeseq_u8(state, rk[nr - 1]);
 * - Final AddRoundKey:
 *     state = veorq_u8(state, rk[nr]);
 *
 * Memory layout:
 * - ctx->buf holds standard AES round keys in column-major order as generated
 *   by mbedtls_aes_setkey_enc_soft().
 * - No transposition or key manipulation is needed for encryption.
 */

#include <mbedtls/mbedtls_config.h>
#include <mbedtls/aes.h>

#include <string.h>

#if defined(MBEDTLS_AESCE_C) && defined(__aarch64__) && \
    (defined(__ARM_FEATURE_CRYPTO) || defined(__ARM_FEATURE_AES))
#define LOGX_AESCE_COMPILED 1
#include <arm_neon.h>
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/auxv.h>
#ifndef AT_HWCAP
#define AT_HWCAP 16
#endif
#ifndef HWCAP_AES
#define HWCAP_AES (1 << 3)
#endif
#endif /* __linux__ || __ANDROID__ */
#endif /* MBEDTLS_AESCE_C && __aarch64__ && crypto */

unsigned int mbedtls_aesce_has_support(void)
{
#if defined(LOGX_AESCE_COMPILED) && (defined(__linux__) || defined(__ANDROID__))
    return (getauxval(AT_HWCAP) & HWCAP_AES) != 0 ? 1 : 0;
#elif defined(LOGX_AESCE_COMPILED)
    return 1;   /* aarch64 with compile-time crypto, without auxv */
#else
    return 0;   /* compiled without crypto features or non-aarch64 */
#endif
}

#if defined(LOGX_AESCE_COMPILED)

int aesce_crypt_ecb(mbedtls_aes_context *ctx,
                    const unsigned char input[16],
                    unsigned char output[16])
{
    const unsigned char *rk_bytes = (const unsigned char *) ctx->buf;
    uint8x16_t state = vld1q_u8(input);
    const int rounds = ctx->nr;

    for (int i = 0; i < rounds - 1; i++) {
        uint8x16_t rk = vld1q_u8(rk_bytes + (i << 4));
        state = vaesmcq_u8(vaeseq_u8(state, rk));
    }
    uint8x16_t rk_last_sub = vld1q_u8(rk_bytes + ((rounds - 1) << 4));
    state = vaeseq_u8(state, rk_last_sub);

    uint8x16_t rk_final = vld1q_u8(rk_bytes + (rounds << 4));
    state = veorq_u8(state, rk_final);

    vst1q_u8(output, state);
    return 0;
}

int aesce_setkey_enc(mbedtls_aes_context *ctx,
                     const unsigned char *key, unsigned int keybits)
{
    (void) key;
    switch (keybits) {
        case 128: break;
        case 192: break;
        case 256: break;
        default: return MBEDTLS_ERR_AES_INVALID_KEY_LENGTH;
    }

    /*
     * Self-check against the software reference implementation using
     * the expanded round keys currently in ctx->buf.
     */
    static const unsigned char test_pt[2][16] = {
        { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
          0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff },
        { 0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
          0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a }
    };

    for (int i = 0; i < 2; i++) {
        unsigned char ref[16];
        unsigned char out[16];
        if (mbedtls_aes_crypt_ecb_soft(ctx, MBEDTLS_AES_ENCRYPT, test_pt[i], ref) != 0) {
            ctx->ce_mode = 0;
            return MBEDTLS_ERR_AES_BAD_INPUT_DATA;
        }
        if (aesce_crypt_ecb(ctx, test_pt[i], out) != 0 ||
            memcmp(out, ref, 16) != 0) {
            ctx->ce_mode = 0;
            return MBEDTLS_ERR_AES_BAD_INPUT_DATA;
        }
    }

    ctx->ce_mode = 1;
    return 0;
}

#else  /* !LOGX_AESCE_COMPILED */

int aesce_setkey_enc(mbedtls_aes_context *ctx,
                     const unsigned char *key, unsigned int keybits)
{
    (void) ctx; (void) key; (void) keybits;
    return MBEDTLS_ERR_AES_BAD_INPUT_DATA;
}

int aesce_crypt_ecb(mbedtls_aes_context *ctx,
                    const unsigned char input[16],
                    unsigned char output[16])
{
    (void) ctx; (void) input; (void) output;
    return MBEDTLS_ERR_AES_BAD_INPUT_DATA;
}

#endif /* LOGX_AESCE_COMPILED */
