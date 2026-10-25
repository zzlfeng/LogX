#include "mbedtls/aes.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/mbedtls_config.h"
#include <string.h>

/*
 * ARMv8 Crypto Extensions hook (aesce.c). Call sites are guarded by
 * MBEDTLS_AESCE_C && __aarch64__ - NOT by the compiler feature macros,
 * because the -march=armv8-a+crypto option is applied only to aesce.c, so
 * the feature macros differ between the two TUs. When aesce.c was compiled
 * without the crypto features its stubs return an error, and the runtime
 * auxv probe keeps the intrinsics path inert on CPUs lacking the
 * extension; both cases fall back to the table-based software path here.
 */
#if defined(MBEDTLS_AESCE_C) && defined(__aarch64__)
#define LOGX_AESCE_HOOK 1
int aesce_setkey_enc(mbedtls_aes_context *ctx, const unsigned char *key, unsigned int keybits);
int aesce_crypt_ecb(mbedtls_aes_context *ctx, const unsigned char input[16], unsigned char output[16]);
unsigned int mbedtls_aesce_has_support(void);
#define CE_MODE_ENABLED() (mbedtls_aesce_has_support())
#else
static inline unsigned int mbedtls_aesce_has_support(void) { return 0; }
#define CE_MODE_ENABLED() 0
#endif

#ifndef GET_UINT32_LE
#define GET_UINT32_LE(n,b,i)                            \
{                                                       \
    (n) = ( (uint32_t) (b)[(i)    ]       )             \
        | ( (uint32_t) (b)[(i) + 1] <<  8 )             \
        | ( (uint32_t) (b)[(i) + 2] << 16 )             \
        | ( (uint32_t) (b)[(i) + 3] << 24 );            \
}
#endif

#ifndef PUT_UINT32_LE
#define PUT_UINT32_LE(n,b,i)                            \
{                                                       \
    (b)[(i)    ] = (unsigned char) ( ( (n)       ) & 0xFF );    \
    (b)[(i) + 1] = (unsigned char) ( ( (n) >>  8 ) & 0xFF );    \
    (b)[(i) + 2] = (unsigned char) ( ( (n) >> 16 ) & 0xFF );    \
    (b)[(i) + 3] = (unsigned char) ( ( (n) >> 24 ) & 0xFF );    \
}
#endif

/* Forward S-box & Tables */
static const unsigned char FSb[256] = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16
};

/* Reverse S-box */
static const unsigned char RSb[256] = {
    0x52, 0x09, 0x6A, 0xD5, 0x30, 0x36, 0xA5, 0x38, 0xBF, 0x40, 0xA3, 0x9E, 0x81, 0xF3, 0xD7, 0xFB,
    0x7C, 0xE3, 0x39, 0x82, 0x9B, 0x2F, 0xFF, 0x87, 0x34, 0x8E, 0x43, 0x44, 0xC4, 0xDE, 0xE9, 0xCB,
    0x54, 0x7B, 0x94, 0x32, 0xA6, 0xC2, 0x23, 0x3D, 0xEE, 0x4C, 0x95, 0x0B, 0x42, 0xFA, 0xC3, 0x4E,
    0x08, 0x2E, 0xA1, 0x66, 0x28, 0xD9, 0x24, 0xB2, 0x76, 0x5B, 0xA2, 0x49, 0x6D, 0x8B, 0xD1, 0x25,
    0x72, 0xF8, 0xF6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xD4, 0xA4, 0x5C, 0xCC, 0x5D, 0x65, 0xB6, 0x92,
    0x6C, 0x70, 0x48, 0x50, 0xFD, 0xED, 0xB9, 0xDA, 0x5E, 0x15, 0x46, 0x57, 0xA7, 0x8D, 0x9D, 0x84,
    0x90, 0xD8, 0xAB, 0x00, 0x8C, 0xBC, 0xD3, 0x0A, 0xF7, 0xE4, 0x58, 0x05, 0xB8, 0xB3, 0x45, 0x06,
    0xD0, 0x2C, 0x1E, 0x8F, 0xCA, 0x3F, 0x0F, 0x02, 0xC1, 0xAF, 0xBD, 0x03, 0x01, 0x13, 0x8A, 0x6B,
    0x3A, 0x91, 0x11, 0x41, 0x4F, 0x67, 0xDC, 0xEA, 0x97, 0xF2, 0xCF, 0xCE, 0xF0, 0xB4, 0xE6, 0x73,
    0x96, 0xAC, 0x74, 0x22, 0xE7, 0xAD, 0x35, 0x85, 0xE2, 0xF9, 0x37, 0xE8, 0x1C, 0x75, 0xDF, 0x6E,
    0x47, 0xF1, 0x1A, 0x71, 0x1D, 0x29, 0xC5, 0x89, 0x6F, 0xB7, 0x62, 0x0E, 0xAA, 0x18, 0xBE, 0x1B,
    0xFC, 0x56, 0x3E, 0x4B, 0xC6, 0xD2, 0x79, 0x20, 0x9A, 0xDB, 0xC0, 0xFE, 0x78, 0xCD, 0x5A, 0xF4,
    0x1F, 0xDD, 0xA8, 0x33, 0x88, 0x07, 0xC7, 0x31, 0xB1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xEC, 0x5F,
    0x60, 0x51, 0x7F, 0xA9, 0x19, 0xB5, 0x4A, 0x0D, 0x2D, 0xE5, 0x7A, 0x9F, 0x93, 0xC9, 0x9C, 0xEF,
    0xA0, 0xE0, 0x3B, 0x4D, 0xAE, 0x2A, 0xF5, 0xB0, 0xC8, 0xEB, 0xBB, 0x3C, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2B, 0x04, 0x7E, 0xBA, 0x77, 0xD6, 0x26, 0xE1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0C, 0x7D
};

/* Round constants */
static const uint32_t RCON[10] = {
    0x00000001, 0x00000002, 0x00000004, 0x00000008,
    0x00000010, 0x00000020, 0x00000040, 0x00000080,
    0x0000001B, 0x00000036
};

#define ROTL8(x)  (((x) << 8) | ((x) >> 24))
#define ROTL16(x) (((x) << 16) | ((x) >> 16))
#define ROTL24(x) (((x) << 24) | ((x) >> 8))

#define SUB_WORD(x)                                     \
    ( ((uint32_t) FSb[((x)      ) & 0xFF]      ) |      \
      ((uint32_t) FSb[((x) >>  8) & 0xFF] <<  8) |      \
      ((uint32_t) FSb[((x) >> 16) & 0xFF] << 16) |      \
      ((uint32_t) FSb[((x) >> 24) & 0xFF] << 24) )

static inline uint32_t xtime(uint32_t x) {
    return ((x << 1) ^ (((x >> 7) & 1) * 0x11B)) & 0xFF;
}

static inline uint32_t mul(uint32_t a, uint32_t b) {
    uint32_t res = 0;
    while (b > 0) {
        if (b & 1) res ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return res;
}

void mbedtls_aes_init(mbedtls_aes_context *ctx) {
    if (ctx == NULL) return;
    memset(ctx, 0, sizeof(mbedtls_aes_context));
    ctx->rk = ctx->buf;
}

void mbedtls_aes_free(mbedtls_aes_context *ctx) {
    if (ctx == NULL) return;
    mbedtls_platform_zeroize(ctx, sizeof(mbedtls_aes_context));
}

/* Software key schedule (table-based). Public as the CE self-check reference. */
int mbedtls_aes_setkey_enc_soft(mbedtls_aes_context *ctx, const unsigned char *key, unsigned int keybits) {
    unsigned int i;
    uint32_t *RK;

    if (ctx == NULL || key == NULL) return MBEDTLS_ERR_AES_INVALID_PARAM;

    switch (keybits) {
        case 128: ctx->nr = 10; break;
        case 192: ctx->nr = 12; break;
        case 256: ctx->nr = 14; break;
        default: return MBEDTLS_ERR_AES_INVALID_KEY_LENGTH;
    }
    ctx->ce_mode = 0;

    ctx->rk = RK = ctx->buf;

    for (i = 0; i < (keybits >> 5); i++) {
        GET_UINT32_LE(RK[i], key, i << 2);
    }

    switch (ctx->nr) {
        case 10:
            for (i = 0; i < 10; i++, RK += 4) {
                uint32_t temp = RK[3];
                RK[4] = RK[0] ^ SUB_WORD(ROTL24(temp)) ^ RCON[i];
                RK[5] = RK[1] ^ RK[4];
                RK[6] = RK[2] ^ RK[5];
                RK[7] = RK[3] ^ RK[6];
            }
            break;
        case 12:
            for (i = 0; i < 8; i++, RK += 6) {
                uint32_t temp = RK[5];
                RK[6] = RK[0] ^ SUB_WORD(ROTL24(temp)) ^ RCON[i];
                RK[7] = RK[1] ^ RK[6];
                RK[8] = RK[2] ^ RK[7];
                RK[9] = RK[3] ^ RK[8];
                if (i == 7) break;
                RK[10] = RK[4] ^ RK[9];
                RK[11] = RK[5] ^ RK[10];
            }
            break;
        case 14:
            for (i = 0; i < 7; i++, RK += 8) {
                uint32_t temp = RK[7];
                RK[8] = RK[0] ^ SUB_WORD(ROTL24(temp)) ^ RCON[i];
                RK[9] = RK[1] ^ RK[8];
                RK[10] = RK[2] ^ RK[9];
                RK[11] = RK[3] ^ RK[10];
                if (i == 6) break;
                temp = RK[11];
                RK[12] = RK[4] ^ SUB_WORD(temp);
                RK[13] = RK[5] ^ RK[12];
                RK[14] = RK[6] ^ RK[13];
                RK[15] = RK[7] ^ RK[14];
            }
            break;
    }
    return 0;
}

/*
 * Public setkey: try the ARMv8 CE path when the CPU probe passes; any
 * failure (unsupported key size, self-check mismatch) falls back to the
 * table-based software schedule. The soft schedule always runs first, so
 * the software path remains correct even when CE refuses.
 */
int mbedtls_aes_setkey_enc(mbedtls_aes_context *ctx, const unsigned char *key, unsigned int keybits) {
    if (ctx == NULL || key == NULL) return MBEDTLS_ERR_AES_INVALID_PARAM;
#if defined(LOGX_AESCE_HOOK)
    if (CE_MODE_ENABLED() && mbedtls_aes_setkey_enc_soft(ctx, key, keybits) == 0) {
        if (aesce_setkey_enc(ctx, key, keybits) == 0) {
            return 0;   /* ctx left in CE key form; crypt_ecb takes the CE path */
        }
        /* On CE failure, soft schedule below ensures clean fallback with ce_mode=0. */
    }
#endif
    return mbedtls_aes_setkey_enc_soft(ctx, key, keybits);
}

int mbedtls_aes_setkey_dec(mbedtls_aes_context *ctx, const unsigned char *key, unsigned int keybits) {
    int ret;
    mbedtls_aes_context cty;
    uint32_t *RK, *SK;
    int i, j;

    mbedtls_aes_init(&cty);
    /*
     * Derive from the software encryption schedule: decryption in this library
     * uses software only, ensuring keys are populated in standard form.
     */
    ret = mbedtls_aes_setkey_enc_soft(&cty, key, keybits);
    if (ret != 0) {
        mbedtls_aes_free(&cty);
        return ret;
    }
    ctx->ce_mode = 0;

    ctx->nr = cty.nr;
    ctx->rk = RK = ctx->buf;
    SK = cty.rk + (cty.nr << 2);

    /* First round key */
    for (j = 0; j < 4; j++) RK[j] = SK[j];
    RK += 4;
    SK -= 4;

    /* Intermediate round keys */
    for (i = 1; i < ctx->nr; i++, RK += 4, SK -= 4) {
        for (j = 0; j < 4; j++) {
            uint32_t val = SK[j];
            unsigned char b0 = (unsigned char) (val & 0xFF);
            unsigned char b1 = (unsigned char) ((val >> 8) & 0xFF);
            unsigned char b2 = (unsigned char) ((val >> 16) & 0xFF);
            unsigned char b3 = (unsigned char) ((val >> 24) & 0xFF);

            uint32_t c0 = mul(b0, 0x0E) ^ mul(b1, 0x0B) ^ mul(b2, 0x0D) ^ mul(b3, 0x09);
            uint32_t c1 = mul(b0, 0x09) ^ mul(b1, 0x0E) ^ mul(b2, 0x0B) ^ mul(b3, 0x0D);
            uint32_t c2 = mul(b0, 0x0D) ^ mul(b1, 0x09) ^ mul(b2, 0x0E) ^ mul(b3, 0x0B);
            uint32_t c3 = mul(b0, 0x0B) ^ mul(b1, 0x0D) ^ mul(b2, 0x09) ^ mul(b3, 0x0E);

            RK[j] = c0 | (c1 << 8) | (c2 << 16) | (c3 << 24);
        }
    }

    /* Last round key */
    for (j = 0; j < 4; j++) RK[j] = SK[j];

    mbedtls_aes_free(&cty);
    return 0;
}

static void aes_encrypt_block(mbedtls_aes_context *ctx, const unsigned char input[16], unsigned char output[16]) {
    uint32_t *RK = ctx->rk;
    unsigned char state[16];
    int r;

    memcpy(state, input, 16);

    /* Initial AddRoundKey */
    for (int i = 0; i < 4; i++) {
        uint32_t rk = RK[i];
        state[i * 4 + 0] ^= (unsigned char) (rk & 0xFF);
        state[i * 4 + 1] ^= (unsigned char) ((rk >> 8) & 0xFF);
        state[i * 4 + 2] ^= (unsigned char) ((rk >> 16) & 0xFF);
        state[i * 4 + 3] ^= (unsigned char) ((rk >> 24) & 0xFF);
    }
    RK += 4;

    for (r = 1; r < ctx->nr; r++, RK += 4) {
        /* SubBytes */
        for (int i = 0; i < 16; i++) state[i] = FSb[state[i]];

        /* ShiftRows */
        unsigned char tmp[16];
        tmp[0] = state[0];  tmp[1] = state[5];  tmp[2] = state[10]; tmp[3] = state[15];
        tmp[4] = state[4];  tmp[5] = state[9];  tmp[6] = state[14]; tmp[7] = state[3];
        tmp[8] = state[8];  tmp[9] = state[13]; tmp[10] = state[2]; tmp[11] = state[7];
        tmp[12] = state[12]; tmp[13] = state[1]; tmp[14] = state[6]; tmp[15] = state[11];

        /* MixColumns + AddRoundKey */
        for (int i = 0; i < 4; i++) {
            unsigned char a0 = tmp[i * 4 + 0], a1 = tmp[i * 4 + 1], a2 = tmp[i * 4 + 2], a3 = tmp[i * 4 + 3];
            unsigned char m0 = (unsigned char) (mul(a0, 2) ^ mul(a1, 3) ^ a2 ^ a3);
            unsigned char m1 = (unsigned char) (a0 ^ mul(a1, 2) ^ mul(a2, 3) ^ a3);
            unsigned char m2 = (unsigned char) (a0 ^ a1 ^ mul(a2, 2) ^ mul(a3, 3));
            unsigned char m3 = (unsigned char) (mul(a0, 3) ^ a1 ^ a2 ^ mul(a3, 2));

            uint32_t rk = RK[i];
            state[i * 4 + 0] = m0 ^ (unsigned char)(rk & 0xFF);
            state[i * 4 + 1] = m1 ^ (unsigned char)((rk >> 8) & 0xFF);
            state[i * 4 + 2] = m2 ^ (unsigned char)((rk >> 16) & 0xFF);
            state[i * 4 + 3] = m3 ^ (unsigned char)((rk >> 24) & 0xFF);
        }
    }

    /* Last round: SubBytes + ShiftRows + AddRoundKey (No MixColumns) */
    for (int i = 0; i < 16; i++) state[i] = FSb[state[i]];

    unsigned char tmp[16];
    tmp[0] = state[0];  tmp[1] = state[5];  tmp[2] = state[10]; tmp[3] = state[15];
    tmp[4] = state[4];  tmp[5] = state[9];  tmp[6] = state[14]; tmp[7] = state[3];
    tmp[8] = state[8];  tmp[9] = state[13]; tmp[10] = state[2]; tmp[11] = state[7];
    tmp[12] = state[12]; tmp[13] = state[1]; tmp[14] = state[6]; tmp[15] = state[11];

    for (int i = 0; i < 4; i++) {
        uint32_t rk = RK[i];
        output[i * 4 + 0] = tmp[i * 4 + 0] ^ (unsigned char)(rk & 0xFF);
        output[i * 4 + 1] = tmp[i * 4 + 1] ^ (unsigned char)((rk >> 8) & 0xFF);
        output[i * 4 + 2] = tmp[i * 4 + 2] ^ (unsigned char)((rk >> 16) & 0xFF);
        output[i * 4 + 3] = tmp[i * 4 + 3] ^ (unsigned char)((rk >> 24) & 0xFF);
    }
}

static void aes_decrypt_block(mbedtls_aes_context *ctx, const unsigned char input[16], unsigned char output[16]) {
    uint32_t *RK = ctx->rk;
    unsigned char state[16];
    int r;

    memcpy(state, input, 16);

    for (int i = 0; i < 4; i++) {
        uint32_t rk = RK[i];
        state[i * 4 + 0] ^= (unsigned char) (rk & 0xFF);
        state[i * 4 + 1] ^= (unsigned char) ((rk >> 8) & 0xFF);
        state[i * 4 + 2] ^= (unsigned char) ((rk >> 16) & 0xFF);
        state[i * 4 + 3] ^= (unsigned char) ((rk >> 24) & 0xFF);
    }
    RK += 4;

    for (r = 1; r < ctx->nr; r++, RK += 4) {
        for (int i = 0; i < 16; i++) state[i] = RSb[state[i]];

        unsigned char tmp[16];
        tmp[0] = state[0];  tmp[1] = state[13]; tmp[2] = state[10]; tmp[3] = state[7];
        tmp[4] = state[4];  tmp[5] = state[1];  tmp[6] = state[14]; tmp[7] = state[11];
        tmp[8] = state[8];  tmp[9] = state[5];  tmp[10] = state[2];  tmp[11] = state[15];
        tmp[12] = state[12]; tmp[13] = state[9]; tmp[14] = state[6]; tmp[15] = state[3];

        for (int i = 0; i < 4; i++) {
            unsigned char a0 = tmp[i * 4 + 0], a1 = tmp[i * 4 + 1], a2 = tmp[i * 4 + 2], a3 = tmp[i * 4 + 3];
            unsigned char m0 = (unsigned char) (mul(a0, 0x0E) ^ mul(a1, 0x0B) ^ mul(a2, 0x0D) ^ mul(a3, 0x09));
            unsigned char m1 = (unsigned char) (mul(a0, 0x09) ^ mul(a1, 0x0E) ^ mul(a2, 0x0B) ^ mul(a3, 0x0D));
            unsigned char m2 = (unsigned char) (mul(a0, 0x0D) ^ mul(a1, 0x09) ^ mul(a2, 0x0E) ^ mul(a3, 0x0B));
            unsigned char m3 = (unsigned char) (mul(a0, 0x0B) ^ mul(a1, 0x0D) ^ mul(a2, 0x09) ^ mul(a3, 0x0E));

            uint32_t rk = RK[i];
            state[i * 4 + 0] = m0 ^ (unsigned char)(rk & 0xFF);
            state[i * 4 + 1] = m1 ^ (unsigned char)((rk >> 8) & 0xFF);
            state[i * 4 + 2] = m2 ^ (unsigned char)((rk >> 16) & 0xFF);
            state[i * 4 + 3] = m3 ^ (unsigned char)((rk >> 24) & 0xFF);
        }
    }

    for (int i = 0; i < 16; i++) state[i] = RSb[state[i]];

    unsigned char tmp[16];
    tmp[0] = state[0];  tmp[1] = state[13]; tmp[2] = state[10]; tmp[3] = state[7];
    tmp[4] = state[4];  tmp[5] = state[1];  tmp[6] = state[14]; tmp[7] = state[11];
    tmp[8] = state[8];  tmp[9] = state[5];  tmp[10] = state[2];  tmp[11] = state[15];
    tmp[12] = state[12]; tmp[13] = state[9]; tmp[14] = state[6]; tmp[15] = state[3];

    for (int i = 0; i < 4; i++) {
        uint32_t rk = RK[i];
        output[i * 4 + 0] = tmp[i * 4 + 0] ^ (unsigned char)(rk & 0xFF);
        output[i * 4 + 1] = tmp[i * 4 + 1] ^ (unsigned char)((rk >> 8) & 0xFF);
        output[i * 4 + 2] = tmp[i * 4 + 2] ^ (unsigned char)((rk >> 16) & 0xFF);
        output[i * 4 + 3] = tmp[i * 4 + 3] ^ (unsigned char)((rk >> 24) & 0xFF);
    }
}

/* Software ECB (table-based). Public as the CE self-check reference. */
int mbedtls_aes_crypt_ecb_soft(mbedtls_aes_context *ctx, int mode, const unsigned char input[16], unsigned char output[16]) {
    if (ctx == NULL || input == NULL || output == NULL) return MBEDTLS_ERR_AES_INVALID_PARAM;
    if (mode == MBEDTLS_AES_ENCRYPT) {
        aes_encrypt_block(ctx, input, output);
    } else {
        aes_decrypt_block(ctx, input, output);
    }
    return 0;
}

/*
 * Public ECB: ENCRYPT direction takes the CE path only when this context's
 * keys were converted to the self-check-approved CE form (ce_mode != 0).
 * GCM only ever calls ENCRYPT; DECRYPT always uses the software path.
 */
int mbedtls_aes_crypt_ecb(mbedtls_aes_context *ctx, int mode, const unsigned char input[16], unsigned char output[16]) {
    if (ctx == NULL || input == NULL || output == NULL) return MBEDTLS_ERR_AES_INVALID_PARAM;
    if (mode == MBEDTLS_AES_ENCRYPT) {
#if defined(LOGX_AESCE_HOOK)
        if (ctx->ce_mode != 0) {
            return aesce_crypt_ecb(ctx, input, output);
        }
#endif
        aes_encrypt_block(ctx, input, output);
    } else {
        aes_decrypt_block(ctx, input, output);
    }
    return 0;
}
