/*
 *  Minimal configuration for AES-GCM in LogX
 */
#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_MODE_CTR

/* Platform options */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY

#endif /* MBEDTLS_CONFIG_H */

