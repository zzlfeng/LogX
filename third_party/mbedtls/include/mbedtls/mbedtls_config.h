/*
 *  Minimal configuration for AES-GCM in LogX
 */
#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_MODE_CTR
/*
 * ARMv8 Crypto Extensions (AESE/AESMC) path. aesce.c compiles away unless
 * the target is aarch64 with the crypto extensions enabled; runtime use is
 * additionally gated by an auxv probe plus a software-vs-hardware
 * self-check, so enabling this macro is safe on any platform.
 */
#define MBEDTLS_AESCE_C

/* Platform options */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY

#endif /* MBEDTLS_CONFIG_H */

