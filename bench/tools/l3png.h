/* PNG of a lap3d scene's dots, each dot 5 x 6 pixels in the host palette (stored DEFLATE). */
#ifndef L3PNG_H
#define L3PNG_H
#include "lap3d.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t crc(const uint8_t *p, size_t n) {
    uint32_t c = ~0u;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ ((0u - (c & 1)) & 0xedb88320u);
    }
    return ~c;
}
static void be(uint8_t *p, uint32_t x) {
    p[0] = (uint8_t)(x >> 24);
    p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8);
    p[3] = (uint8_t)x;
}
static bool chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    uint8_t *p = malloc(n + 12);
    if (!p)
        return false;
    be(p, (uint32_t)n);
    memcpy(p + 4, type, 4);
    if (n)
        memcpy(p + 8, data, n);
    be(p + 8 + n, crc(p + 4, n + 4));
    bool ok = fwrite(p, 1, n + 12, f) == n + 12;
    free(p);
    return ok;
}
static inline bool l3png(const char *path, l3_scene *s) {
    int dw, dh;
    l3_size(s, &dw, &dh);
    int w = dw * 5, h = dh * 6;
    size_t stride = (size_t)w * 3 + 1, n = stride * h;
    uint8_t *raw = malloc(n);
    if (!raw)
        return false;
    uint8_t *dots = l3_dots(s);
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * stride] = 0;
        for (int x = 0; x < w; x++) {
            uint8_t ink = dots[(y / 6) * dw + x / 5];
            uint16_t c = lap_palette(ink == LAP_DOT_CLEAR ? 0 : ink);
            uint8_t *p = raw + (size_t)y * stride + 1 + x * 3;
            p[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
            p[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
            p[2] = (uint8_t)((c & 31) * 255 / 31);
        }
    }
    size_t blocks = (n + 65534) / 65535, zsize = 2 + n + blocks * 5 + 4;
    uint8_t *z = malloc(zsize);
    if (!z) {
        free(raw);
        return false;
    }
    z[0] = 0x78;
    z[1] = 1;
    size_t at = 2, pos = 0;
    while (pos < n) {
        unsigned count = n - pos > 65535 ? 65535 : (unsigned)(n - pos);
        z[at++] = (uint8_t)(pos + count == n);
        z[at++] = (uint8_t)count;
        z[at++] = (uint8_t)(count >> 8);
        z[at++] = (uint8_t)~count;
        z[at++] = (uint8_t)(~count >> 8);
        memcpy(z + at, raw + pos, count);
        at += count;
        pos += count;
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    be(z + at, (b << 16) | a);
    free(raw);
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(z);
        return false;
    }
    uint8_t ihdr[13] = {0};
    be(ihdr, (uint32_t)w);
    be(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;
    ihdr[9] = 2;
    const uint8_t sig[] = {137, 80, 78, 71, 13, 10, 26, 10};
    bool ok = fwrite(sig, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) &&
              chunk(f, "IDAT", z, zsize) && chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(z);
    return ok;
}
#endif
