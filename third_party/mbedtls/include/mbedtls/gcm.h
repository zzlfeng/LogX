#ifndef MBEDTLS_GCM_H
#define MBEDTLS_GCM_H

#include "cipher.h"
#include "aes.h"
#include <stddef.h>
#include <stdint.h>

#define MBEDTLS_GCM_ENCRYPT     1
#define MBEDTLS_GCM_DECRYPT     0

#define MBEDTLS_ERR_GCM_AUTH_FAILED                       -0x0012
#define MBEDTLS_ERR_GCM_BAD_INPUT                         -0x0014
#define MBEDTLS_ERR_GCM_BUFFER_TOO_SMALL                  -0x0016

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mbedtls_gcm_context {
    mbedtls_aes_context aes_ctx;
    uint64_t HL[16];            /*!< Precalculated table for 4-bit multiplication */
    uint64_t HH[16];
    uint64_t len;               /*!< Total length of the encrypted/decrypted data */
    uint64_t add_len;           /*!< Total length of the additional data */
    unsigned char base_ectr[16];/*!< The first counter-mode block */
    unsigned char y[16];        /*!< The current state of the accumulator */
    unsigned char buf[16];      /*!< The buf working value */
    int mode;                   /*!< The operation to perform: #MBEDTLS_GCM_ENCRYPT or #MBEDTLS_GCM_DECRYPT */
} mbedtls_gcm_context;

void mbedtls_gcm_init(mbedtls_gcm_context *ctx);

int mbedtls_gcm_setkey(mbedtls_gcm_context *ctx,
                       mbedtls_cipher_id_t cipher,
                       const unsigned char *key,
                       unsigned int keybits);

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
                              unsigned char *tag);

int mbedtls_gcm_auth_decrypt(mbedtls_gcm_context *ctx,
                             size_t length,
                             const unsigned char *iv,
                             size_t iv_len,
                             const unsigned char *add,
                             size_t add_len,
                             const unsigned char *tag,
                             size_t tag_len,
                             const unsigned char *input,
                              unsigned char *output);

void mbedtls_gcm_free(mbedtls_gcm_context *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MBEDTLS_GCM_H */

