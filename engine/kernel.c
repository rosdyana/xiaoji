/* The kernel the engine presents (kernel.h, B18). */
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kconfig.inc"

const char *kernel_release(void)
{
    return XJ_KCONFIG_LTS XJ_KCONFIG_SUFFIX "-xiaoji";
}

const char *kernel_version(void)
{
    return "#1 xiaoji M1";
}

/* KVER(a, b, c) of the release: (a << 16) | (b << 8) | c, with c capped at 255 as the kernel's KERNEL_VERSION does. */
uint32_t kernel_kver(void)
{
    unsigned a = 0, b = 0, c = 0;
    if (sscanf(XJ_KCONFIG_LTS, "%u.%u.%u", &a, &b, &c) != 3)
        return 0;
    return (a << 16) | (b << 8) | (c > 255 ? 255 : c);
}

const char *kernel_proc_version(void)
{
    static char text[160];
    if (!text[0])
        snprintf(text, sizeof text, "Linux version %s (xiaoji@xiaoji) (xiaoji engine) %s\n", kernel_release(),
                 kernel_version());
    return text;
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* RFC 1952 around RFC 1951 stored blocks (BTYPE 00, at most 65535 bytes each): what any inflater reads. */
int kernel_config_gz(uint8_t **data, size_t *len)
{
    const uint8_t *src = (const uint8_t *)xj_kconfig_text;
    size_t n = sizeof xj_kconfig_text - 1, blocks = n / 65535 + 1;
    size_t total = 10 + blocks * 5 + n + 8;
    uint8_t *g = malloc(total), *o = g;
    if (!g)
        return -1;
    static const uint8_t hdr[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3}; /* deflate, no flags, no mtime, OS unix */
    memcpy(o, hdr, sizeof hdr);
    o += sizeof hdr;
    size_t done = 0;
    for (size_t b = 0; b < blocks; b++) {
        size_t k = n - done < 65535 ? n - done : 65535;
        *o++ = b + 1 == blocks ? 1 : 0; /* BFINAL on the last block, BTYPE 00 */
        *o++ = (uint8_t)k;
        *o++ = (uint8_t)(k >> 8);
        *o++ = (uint8_t)~k;
        *o++ = (uint8_t)(~k >> 8);
        memcpy(o, src + done, k);
        o += k;
        done += k;
    }
    put32(o, crc32_of(src, n));
    put32(o + 4, (uint32_t)n);
    *data = g;
    *len = total;
    return 0;
}
