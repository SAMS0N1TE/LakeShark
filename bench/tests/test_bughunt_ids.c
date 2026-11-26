/* BUGHUNT's ids.bin reader (app-sdk/apps/bughunt/bh_ids.c): a good header
   gives its tables and a block search finds a key; a table whose index size
   does not fit in 32 bits (a damaged or made-up file) is left out instead
   of being kept with a buffer far smaller than the search reads. */
#include "ls_test.h"
#include "bh_ids.h"
#include <stdint.h>
#include <string.h>

static void put(uint8_t *p, uint32_t v, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static void head(uint8_t *b, int nt)
{
    memset(b, 0, BHID_HEAD_MAX);
    memcpy(b, "BHID", 4);
    put(b + 4, 1, 2);
    put(b + 6, (uint32_t)nt, 2);
}

static void table(uint8_t *b, int i, int kind, int keyb, uint32_t every, uint32_t count)
{
    uint8_t *d = b + 32 + 16 * i;
    d[0] = (uint8_t)kind;
    d[1] = (uint8_t)keyb;
    put(d + 2, every, 2);
    put(d + 4, count, 4);
    put(d + 8, 1000, 4);
    put(d + 12, 2000, 4);
}

LS_CASE(bughunt_ids_good_header_and_block)
{
    uint8_t b[BHID_HEAD_MAX];
    head(b, 1);
    table(b, 0, 1, 3, 4, 10);
    bhid_head_t h;
    LS_CHECK(bhid_head(&h, b, sizeof(b)));
    bhid_tab_t *t = bhid_table(&h, 1);
    LS_CHECK(t != NULL);
    if (!t) return;
    LS_EQ_UINT(t->nidx, 3);
    LS_EQ_INT(t->recb, 6);
    uint8_t idx[9];
    put(idx, 0x100000, 3);
    put(idx + 3, 0x200000, 3);
    put(idx + 6, 0x300000, 3);
    t->idx = idx;
    uint32_t first = 0, n = 0;
    LS_CHECK(bhid_block(t, 0x250000, &first, &n));
    LS_EQ_UINT(first, 4);
    LS_EQ_UINT(n, 4);
    LS_CHECK(bhid_block(t, 0x300001, &first, &n));
    LS_EQ_UINT(first, 8);
    LS_EQ_UINT(n, 2);
    LS_CHECK(!bhid_block(t, 0x0FFFFF, &first, &n));
}

LS_CASE(bughunt_ids_oversized_index_is_left_out)
{
    uint8_t b[BHID_HEAD_MAX];
    head(b, 2);
    /* count 0x40000001, every 1, 4-byte keys: nidx * keyb wraps to 4 in 32 bits */
    table(b, 0, 1, 4, 1, 0x40000001u);
    table(b, 1, 2, 3, 16, 4000);
    bhid_head_t h;
    LS_CHECK(bhid_head(&h, b, sizeof(b)));
    LS_CHECK(bhid_table(&h, 1) == NULL);
    LS_CHECK(bhid_table(&h, 2) != NULL);
    LS_EQ_INT(h.nt, 1);
}
