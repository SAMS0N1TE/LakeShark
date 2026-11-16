#include "ls_test.h"
#include "ls_carto_block_cache.h"
#include <stdlib.h>

/* A cell file as the engine sees it: a header and a sorted directory of
   24-byte entries, behind a reader that counts its calls. */
#define ENTRIES 87381u                    /* z13..z16 under one z8 cell, a detail cell */
#define FILE_BYTES (64u + ENTRIES * 24u + 1000u)
static uint8_t *s_file;
static unsigned s_reads, s_fail_after;
static int file_read(void *ctx, uint64_t offset, void *dst, size_t bytes)
{
    (void)ctx;
    s_reads++;
    if (s_fail_after && s_reads > s_fail_after) return 0;
    if (offset > FILE_BYTES || bytes > FILE_BYTES - offset) return 0;
    memcpy(dst, s_file + offset, bytes);
    return 1;
}
static void make_file(void)
{
    if (s_file) return;
    s_file = malloc(FILE_BYTES);
    srand(7);
    for (size_t i = 0; i < FILE_BYTES; i++) s_file[i] = (uint8_t)rand();
    for (uint32_t i = 0; i < ENTRIES; i++) memcpy(s_file + 64 + (size_t)i * 24, &i, 4);
}
/* The engine's lookup: the header, then a binary search by entry. */
static uint32_t lookup(ls_carto_block_cache *c, uint32_t want)
{
    uint8_t h[64], r[24];
    if (!ls_carto_block_cached(c, file_read, NULL, FILE_BYTES, 0, h, 64)) return UINT32_MAX;
    uint32_t lo = 0, hi = ENTRIES;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2; uint32_t key;
        if (!ls_carto_block_cached(c, file_read, NULL, FILE_BYTES, 64 + (uint64_t)mid * 24, r, 24)) return UINT32_MAX;
        memcpy(&key, r, 4);
        if (key < want) lo = mid + 1; else hi = mid;
    }
    return lo;
}

LS_CASE(every_read_returns_the_files_bytes)
{
    make_file();
    static uint8_t memory[LS_CARTO_BLOCK_SLOTS * LS_CARTO_BLOCK_BYTES];
    ls_carto_block_cache c = { .memory = memory };
    uint8_t got[600];
    srand(11);
    for (int i = 0; i < 20000; i++) {
        const size_t n = 1 + (size_t)rand() % (i % 7 ? 64 : 600);
        uint64_t at = (uint64_t)rand() % (FILE_BYTES - n + 1);
        if (i % 5 == 0) at = (at / 4096) * 4096 + 4096 - n / 2;      /* across a block edge */
        if (i % 11 == 0) at = FILE_BYTES - n;                         /* the short last block */
        if (at + n > FILE_BYTES) at = FILE_BYTES - n;
        LS_CHECK(ls_carto_block_cached(&c, file_read, NULL, FILE_BYTES, at, got, n));
        LS_CHECK(!memcmp(got, s_file + at, n));
    }
    LS_CHECK(!ls_carto_block_cached(&c, file_read, NULL, FILE_BYTES, FILE_BYTES - 3, got, 4));
    LS_CHECK(!ls_carto_block_cached(&c, file_read, NULL, FILE_BYTES, FILE_BYTES + 1, got, 0));
}

LS_CASE(the_tiles_of_one_view_share_their_directory_reads)
{
    make_file();
    static uint8_t memory[LS_CARTO_BLOCK_SLOTS * LS_CARTO_BLOCK_BYTES];
    ls_carto_block_cache none = { 0 }, c = { .memory = memory };
    /* a z16 view: three columns of tiles 256 entries apart, four rows each,
       looked up twice (the frame, then its prefetch hints) */
    unsigned plain = 0, cached = 0;
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t col = 0; col < 3; col++)
            for (uint32_t row = 0; row < 4; row++) {
                const uint32_t want = 40000 + col * 256 + row;
                s_reads = 0; LS_EQ_UINT(lookup(&none, want), want); plain += s_reads;
                s_reads = 0; LS_EQ_UINT(lookup(&c, want), want); cached += s_reads;
            }
    LS_CHECK_MSG(plain >= 24 * 17, "uncached lookups read %u times", plain);
    LS_CHECK_MSG(cached <= 24, "cached lookups read %u times for 24 lookups", cached);
}

LS_CASE(a_failed_block_read_is_not_kept)
{
    make_file();
    static uint8_t memory[LS_CARTO_BLOCK_SLOTS * LS_CARTO_BLOCK_BYTES];
    ls_carto_block_cache c = { .memory = memory };
    uint8_t got[24];
    s_reads = 0; s_fail_after = 1;
    LS_CHECK(ls_carto_block_cached(&c, file_read, NULL, FILE_BYTES, 64, got, 24));
    LS_CHECK(!ls_carto_block_cached(&c, file_read, NULL, FILE_BYTES, 64 + 50000, got, 24));
    s_fail_after = 0;
    LS_CHECK(ls_carto_block_cached(&c, file_read, NULL, FILE_BYTES, 64 + 50000, got, 24));
    LS_CHECK(!memcmp(got, s_file + 64 + 50000, 24));
}
