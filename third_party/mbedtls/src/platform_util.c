#include "mbedtls/platform_util.h"
#include <string.h>

void mbedtls_platform_zeroize(void *buf, size_t len) {
    if (buf == NULL || len == 0) {
        return;
    }
    volatile unsigned char *p = (volatile unsigned char *) buf;
    while (len--) {
        *p++ = 0;
    }
}

