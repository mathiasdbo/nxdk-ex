#ifndef SHOWCASE_FMT_H
#define SHOWCASE_FMT_H

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/*
 * Fixed-point float formatting. nxdk's printf (pdclib) prints nothing for %f,
 * so every float on the HUD goes through this and is printed with %s.
 * Returns one of several rotating buffers, so a call can use a few at once.
 */
static inline const char *showcase_fx(float v, int decimals, bool plus, int width) {
    static char bufs[12][24];
    static unsigned next = 0;
    char *out = bufs[next++ % 12u];
    char num[24];
    long scale = 1, iv;
    int d;
    char sign = '\0';

    /* Bounds keep the result in a 32-bit long and in the buffer */
    decimals = (decimals < 0) ? 0 : ((decimals > 3) ? 3 : decimals);
    width = (width > 16) ? 16 : width;
    for (d = 0; d < decimals; d++) {
        scale *= 10;
    }
    if (!isfinite(v)) {
        v = 0.0f;
    }
    v = (v > 99999.0f) ? 99999.0f : ((v < -99999.0f) ? -99999.0f : v);
    iv = (long)(fabsf(v) * (float)scale + 0.5f);
    if (v < 0.0f && iv != 0) {
        sign = '-';
    } else if (plus) {
        sign = '+';
    }

    {
        char *p = num;
        long frac = iv % scale;
        if (sign) {
            *p++ = sign;
        }
        p += snprintf(p, 12, "%ld", iv / scale);
        if (decimals > 0) {
            *p++ = '.';
            for (d = decimals - 1; d >= 0; d--) {
                p[d] = (char)('0' + frac % 10);
                frac /= 10;
            }
            p += decimals;
        }
        *p = '\0';
    }

    /* Right-align in width characters */
    {
        size_t len = strlen(num);
        size_t pad = (width > 0 && (size_t)width > len) ? (size_t)width - len : 0;
        memset(out, ' ', pad);
        memcpy(out + pad, num, len + 1);
    }
    return out;
}

#endif /* SHOWCASE_FMT_H */
