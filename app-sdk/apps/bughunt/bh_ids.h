/* BUGHUNT's device names: the reader for ids.bin (tools/build_ids.py), the
   public registries packed small. Pure C with no board calls: the app reads
   bytes off the card and hands them in; the bench tests it the same way.

   Three reads a name at most: the header and indexes once, then a block of
   records and the name itself. */
#ifndef BH_IDS_H
#define BH_IDS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { BHID_COMPANY = 1, BHID_UUID = 2, BHID_OUI = 3, BHID_APPEARANCE = 4, BHID_KINDS = 5 };
#define BHID_TABLES 8
#define BHID_HEAD_MAX (32 + 16 * BHID_TABLES)
#define BHID_NAME 32
#define BHID_IDX_MAX (1u << 20)     /* a table's index, bytes: a real one is a few KB */

typedef struct {
    uint8_t kind, keyb, recb;
    uint16_t every;
    uint32_t count, rec_off, idx_off;
    uint32_t nidx;
    uint8_t *idx;                   /* the app fills it: nidx keys of keyb bytes */
} bhid_tab_t;
typedef struct {
    uint32_t pool_off, pool_len, built;
    int nt;
    bhid_tab_t t[BHID_TABLES];
} bhid_head_t;

/* Parse the header and table list. 0 bad, 1 good. */
int bhid_head(bhid_head_t *h, const uint8_t *b, uint32_t n);
bhid_tab_t *bhid_table(bhid_head_t *h, int kind);
/* Where the records that could hold key are: records [*first, *first + *n). False: none can. */
bool bhid_block(const bhid_tab_t *t, uint32_t key, uint32_t *first, uint32_t *n);
/* Search a block read from rec_off + first * recb; the name's offset in the pool, or -1. */
long bhid_in_block(const bhid_tab_t *t, const uint8_t *recs, uint32_t n, uint32_t key);
/* A name read from the pool (up to BHID_NAME bytes, maybe cut): made printable, ended. */
void bhid_name_fix(char *s, size_t cap);
/* Makers whose devices are cameras first: Hikvision, Dahua, Wyze, Ring ... */
bool bhid_camera_maker(const char *name);
#endif
