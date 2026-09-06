#ifndef MBEDTLS_AES_H
#define MBEDTLS_AES_H

#include <stddef.h>
#include <stdint.h>

#define MBEDTLS_AES_ENCRYPT     1
#define MBEDTLS_AES_DECRYPT     0

/* Error codes */
#define MBEDTLS_ERR_AES_INVALID_KEY_LENGTH                -0x0020
#define MBEDTLS_ERR_AES_INVALID_PARAM                     -0x002F
#define MBEDTLS_ERR_AES_BAD_INPUT_DATA                    -0x0022

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mbedtls_aes_context {
    int nr;                     /*!< The number of rounds. */
    uint32_t *rk;               /*!< AES round keys. */
    uint32_t buf[68];           /*!< Unaligned data buffer for round keys. */
    int ce_mode;                /*!< ARMv8 CE status: 0=software path,
                                     1=hardware-accelerated path (self-check
                                     verified, see aesce.c). */
} mbedtls_aes_context;

void mbedtls_aes_init(mbedtls_aes_context *ctx);
void mbedtls_aes_free(mbedtls_aes_context *ctx);

int mbedtls_aes_setkey_enc(mbedtls_aes_context *ctx, const unsigned char *key, unsigned int keybits);
int mbedtls_aes_setkey_dec(mbedtls_aes_context *ctx, const unsigned char *key, unsigned int keybits);

int mbedtls_aes_crypt_ecb(mbedtls_aes_context *ctx, int mode, const unsigned char input[16], unsigned char output[16]);

/*
 * Software (table-based) primitives, always compiled; the ARMv8 CE path in
 * aesce.c uses them as the reference for its enable-time self-check and as
 * the universal fallback.
 */
int mbedtls_aes_setkey_enc_soft(mbedtls_aes_context *ctx, const unsigned char *key, unsigned int keybits);
int mbedtls_aes_crypt_ecb_soft(mbedtls_aes_context *ctx, int mode, const unsigned char input[16], unsigned char output[16]);

#ifdef __cplusplus
}
#endif

#endif /* MBEDTLS_AES_H */

