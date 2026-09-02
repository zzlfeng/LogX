#ifndef MBEDTLS_PLATFORM_UTIL_H
#define MBEDTLS_PLATFORM_UTIL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void mbedtls_platform_zeroize(void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MBEDTLS_PLATFORM_UTIL_H */

