#include "ls_lap.h"
#include "ls_lap_manifest.h"
#include "ls_lap_signature.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    unsigned char *bytes;
    size_t size, reads, fail_read;
} source;
static bool read_at(void *ctx, size_t off, void *out, size_t n) {
    source *s = ctx;
    assert(off <= s->size && n <= s->size - off);
    if (++s->reads == s->fail_read)
        return false;
    memcpy(out, s->bytes + off, n);
    return true;
}
static unsigned u16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned u32(const unsigned char *p) {
    return p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}
static bool accept(void *ctx, const ls_lap_reader_t *r, size_t n, const uint8_t *der, size_t len) {
    (void)ctx;
    (void)r;
    return n >= 52 && len == 8 && der[0] == 0x30;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    size_t n = (size_t)ftell(f);
    rewind(f);
    unsigned char *bytes = malloc(n + 80), *copy = malloc(n);
    assert(bytes && copy);
    assert(fread(bytes, 1, n, f) == n);
    fclose(f);
    memcpy(copy, bytes, n);
    lap_app_t app, out;
    char why[128];
    assert(ls_lap_manifest(bytes, n, &app, why, sizeof(why)));
    assert(!strcmp(app.id, "probe") && app.magic == LAP_MAGIC && !app.enter && !app.draw);
    assert(ls_lap_preflight(bytes, n, why, sizeof(why)));
    size_t checks = 2;
    /* A capability cannot hide in an older manifest or unknown flag bit. */
    lap_app_t capability = app;
    capability.flags = LAP_CAP_NETWORK;
    assert(ls_lap_fields_valid(&capability, why, sizeof(why)));
    capability.api_minor = 4;                                   /* past this firmware (2.2) */
    assert(!ls_lap_fields_valid(&capability, why, sizeof(why)));
    capability.api_minor = 5; capability.flags = 2;
    assert(!ls_lap_fields_valid(&capability, why, sizeof(why)));
    capability.flags = 0; capability.api_minor = 0;
    assert(ls_lap_fields_valid(&capability, why, sizeof(why)));
    checks += 4;
    /* The colour icon: '.' clear, hex digits colours, row by row. */
    uint8_t px[LAP_PX_H][LAP_PX_W];
    {
        source s0 = {bytes, n, 0, 0};
        ls_lap_reader_t r0 = {read_at, &s0, n};
        assert(ls_lap_icon_read(&r0, px));
        assert(px[0][0] == 15 && px[0][1] == 0xFF && px[0][19] == 0 && px[17][0] == 0xFF);
        for (int y = 1; y < 17; y++)
            for (int x = 0; x < LAP_PX_W; x++) assert(px[y][x] == y % 16);
        /* A bad character anywhere drops the icon, and the result is all clear. */
        unsigned char *at = NULL;
        for (size_t i = 0; i + 4 <= n && !at; i++)
            if (!memcmp(bytes + i, "LIC1", 4)) at = bytes + i;
        assert(at);
        unsigned char was = at[4 + 21 * 5 + 3];
        at[4 + 21 * 5 + 3] = 'z';
        assert(!ls_lap_icon_read(&r0, px));
        for (int y = 0; y < LAP_PX_H; y++)
            for (int x = 0; x < LAP_PX_W; x++) assert(px[y][x] == 0xFF);
        at[4 + 21 * 5 + 3] = was;
        at[0] = 'X';
        assert(!ls_lap_icon_read(&r0, px));
        at[0] = 'L';
        checks += 3;
    }
    for (size_t i = 0; i < n; i++) {
        source src = {bytes, i, 0, 0};
        ls_lap_reader_t r = {read_at, &src, i};
        memset(&out, 0xa5, sizeof(out));
        lap_app_t old = out;
        assert(!ls_lap_manifest_read(&r, &out, why, sizeof(why)));
        assert(why[0]);
        assert(!ls_lap_icon_read(&r, px));
        assert(!memcmp(&old, &out, sizeof(out)));
        checks++;
    }
    unsigned sh = u32(bytes + 32), ns = u16(bytes + 48), np = u16(bytes + 44), ph = u32(bytes + 28);
    for (unsigned i = 0; i < 52 + ns * 40 + np * 32; i++) {
        size_t pos = i < 52 ? i : i < 52 + ns * 40 ? sh + i - 52 : ph + i - 52 - ns * 40;
        assert(pos < n);
        for (unsigned bit = 0; bit < 8; bit++) {
            bytes[pos] ^= (unsigned char)(1u << bit);
            source src = {bytes, n, 0, 0};
            ls_lap_reader_t r = {read_at, &src, n};
            (void)ls_lap_manifest_read(&r, &out, why, sizeof(why));
            (void)ls_lap_icon_read(&r, px);
            (void)ls_lap_preflight(bytes, n, why, sizeof(why));
            bytes[pos] ^= (unsigned char)(1u << bit);
            checks++;
        }
    }
    /* Mutations across symbol/relocation tables too, with deterministic seed. */
    unsigned seed = 0x12345678;
    for (unsigned i = 0; i < 10000; i++) {
        seed = seed * 1664525u + 1013904223u;
        size_t at = seed % n;
        unsigned char mask = (unsigned char)(1u << ((seed >> 24) & 7));
        bytes[at] ^= mask;
        (void)ls_lap_preflight(bytes, n, why, sizeof(why));
        bytes[at] ^= mask;
        checks++;
    }
    source src = {bytes, n, 0, 0};
    ls_lap_reader_t r = {read_at, &src, n};
    assert(ls_lap_manifest_read(&r, &out, why, sizeof(why)));
    size_t reads = src.reads;
    for (size_t i = 1; i <= reads; i++) {
        src.reads = 0;
        src.fail_read = i;
        assert(!ls_lap_manifest_read(&r, &out, why, sizeof(why)));
        checks++;
    }
    src.fail_read = 0;
    size_t elf_size;
    bool signed_app;
    assert(ls_lap_signature(&r, accept, NULL, &elf_size, &signed_app, why, sizeof(why)) &&
           !signed_app && elf_size == n);
    checks++;
    memset(bytes + n, 0, 8);
    bytes[n] = 0x30;
    bytes[n + 8] = 8;
    bytes[n + 9] = 0;
    memcpy(bytes + n + 10, "LSG1", 4);
    src.size = r.size = n + 14;
    assert(ls_lap_signature(&r, accept, NULL, &elf_size, &signed_app, why, sizeof(why)) &&
           signed_app && elf_size == n);
    checks++;
    bytes[n] = 0;
    assert(!ls_lap_signature(&r, accept, NULL, &elf_size, &signed_app, why, sizeof(why)));
    checks++;
    bytes[n] = 0x30;
    bytes[n + 8] = 255;
    assert(!ls_lap_signature(&r, accept, NULL, &elf_size, &signed_app, why, sizeof(why)));
    checks++;
    assert(!ls_lap_manifest(bytes, n + 14, &out, why, sizeof(why)));
    checks++;
    assert(!strcmp(ls_lap_status()->reason, "native apps need the board"));
    checks++;
    assert(!memcmp(bytes, copy, n));
    free(bytes);
    free(copy);
    printf("test_lap_manifest: %zu checks passed (all truncations, header bits, 10000 mutations, "
           "callback failures, trailers)\n",
           checks);
    return 0;
}
