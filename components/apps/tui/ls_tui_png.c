/* See ls_tui_png.h. Firmware only: the compressor is the ROM's. */
#include "ls_tui_png.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "miniz.h"

typedef struct {
    ls_tui_png_line_fn emit;
    void    *ctx;
    uint32_t crc;          /* of every PNG byte, for the receiver to check */
    size_t   total;
    uint8_t  carry[57];    /* 57 bytes is exactly one 76-character line */
    int      n;
} png_out_t;

/* Bitwise and table-free. A screenshot is tens of kilobytes, so the speed a
   table buys is milliseconds, and the kilobyte it costs would come from the
   board that has the least RAM to spare. */
static uint32_t crc32_step(uint32_t crc, const uint8_t *p, size_t len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void line_out(png_out_t *o)
{
    char line[80];
    int j = 0;
    for (int i = 0; i < o->n; i += 3) {
        const int rem = o->n - i;
        const uint32_t v = ((uint32_t)o->carry[i] << 16) |
                           (rem > 1 ? (uint32_t)o->carry[i + 1] << 8 : 0u) |
                           (rem > 2 ? (uint32_t)o->carry[i + 2] : 0u);
        line[j++] = B64[(v >> 18) & 63];
        line[j++] = B64[(v >> 12) & 63];
        line[j++] = rem > 1 ? B64[(v >> 6) & 63] : '=';
        line[j++] = rem > 2 ? B64[v & 63] : '=';
    }
    line[j] = '\0';
    o->emit(line, o->ctx);
    o->n = 0;
}

static void out_bytes(png_out_t *o, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    o->crc = crc32_step(o->crc, p, len);
    o->total += len;
    while (len) {
        size_t take = sizeof(o->carry) - (size_t)o->n;
        if (take > len) take = len;
        memcpy(o->carry + o->n, p, take);
        o->n += (int)take;
        p += take;
        len -= take;
        if (o->n == (int)sizeof(o->carry)) line_out(o);
    }
}

static void put_be32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}

static void chunk(png_out_t *o, const char type[4], const void *data,
                  uint32_t len)
{
    uint8_t hdr[8];
    put_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    out_bytes(o, hdr, sizeof(hdr));
    if (len) out_bytes(o, data, len);
    uint32_t c = crc32_step(0, (const uint8_t *)type, 4);
    c = crc32_step(c, (const uint8_t *)data, len);
    uint8_t tail[4];
    put_be32(tail, c);
    out_bytes(o, tail, sizeof(tail));
}

/* Each block the compressor finishes becomes one IDAT chunk, so nothing has
   to hold the whole compressed image. */
static mz_bool idat_out(const void *buf, int len, void *user)
{
    chunk((png_out_t *)user, "IDAT", buf, (uint32_t)len);
    return MZ_TRUE;
}

static inline void rgb888(uint16_t v, uint8_t *d)
{
    const uint32_t r = (v >> 11) & 0x1Fu, g = (v >> 5) & 0x3Fu, b = v & 0x1Fu;
    d[0] = (uint8_t)((r << 3) | (r >> 2));
    d[1] = (uint8_t)((g << 2) | (g >> 4));
    d[2] = (uint8_t)((b << 3) | (b >> 2));
}

size_t ls_tui_png_emit(const uint16_t *px, int native_w, int native_h,
                       bool landscape, ls_tui_png_line_fn emit, void *ctx,
                       uint32_t *crc_out)
{
    if (!px || !emit || native_w <= 0 || native_h <= 0) return 0;
    const int w = landscape ? native_h : native_w;
    const int h = landscape ? native_w : native_h;
    const size_t row_len = (size_t)w * 3u + 1u;

    /* Both in PSRAM: the compressor is its dictionary and hash chains, and a
       row is 3.7 kB at the widest - neither belongs in internal RAM. */
    tdefl_compressor *d = heap_caps_malloc(sizeof(*d), MALLOC_CAP_SPIRAM);
    uint8_t *row = heap_caps_malloc(row_len, MALLOC_CAP_SPIRAM);
    if (!d || !row) {
        heap_caps_free(d);
        heap_caps_free(row);
        return 0;
    }

    png_out_t o = { .emit = emit, .ctx = ctx };
    static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    out_bytes(&o, SIG, sizeof(SIG));

    uint8_t ihdr[13];
    put_be32(ihdr, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8]  = 8;     /* bits per channel */
    ihdr[9]  = 2;     /* truecolour, RGB  */
    ihdr[10] = 0;     /* deflate          */
    ihdr[11] = 0;     /* adaptive filters */
    ihdr[12] = 0;     /* not interlaced   */
    chunk(&o, "IHDR", ihdr, sizeof(ihdr));

    bool ok = tdefl_init(d, idat_out, &o,
                         TDEFL_WRITE_ZLIB_HEADER | TDEFL_DEFAULT_MAX_PROBES)
              == TDEFL_STATUS_OKAY;
    for (int y = 0; ok && y < h; y++) {
        row[0] = 0;   /* filter: none - the long runs of ground deflate well */
        uint8_t *dst = row + 1;
        if (!landscape) {
            const uint16_t *src = px + (size_t)y * (size_t)native_w;
            for (int x = 0; x < w; x++, dst += 3) rgb888(src[x], dst);
        } else {
            /* The blitter writes logical (x, y) to native row (w - 1 - x),
               column y - the clockwise transpose in ls_tui.c - so it is read
               back the same way. */
            for (int x = 0; x < w; x++, dst += 3)
                rgb888(px[(size_t)(w - 1 - x) * (size_t)native_w + (size_t)y], dst);
        }
        ok = tdefl_compress_buffer(d, row, row_len, TDEFL_NO_FLUSH)
             == TDEFL_STATUS_OKAY;
    }
    if (ok) ok = tdefl_compress_buffer(d, NULL, 0, TDEFL_FINISH)
                 == TDEFL_STATUS_DONE;
    chunk(&o, "IEND", NULL, 0);
    if (o.n) line_out(&o);

    heap_caps_free(row);
    heap_caps_free(d);
    if (crc_out) *crc_out = o.crc;
    return ok ? o.total : 0;
}
