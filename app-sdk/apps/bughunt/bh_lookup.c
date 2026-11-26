/* BUGHUNT's name lookups off the card: see bh_lookup.h. */
#include "bh_lookup.h"
#include "lakeshark_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { L_NONE, L_HEAD, L_INDEX, L_READY, L_FAILED };
static int lst = L_NONE, path_i, idx_i, job = -1;
static const char *path;
static bhid_head_t hd;
static uint8_t headbuf[BHID_HEAD_MAX];
static uint8_t blk[64 * 7];             /* one block of records */
static char namebuf[BHID_NAME];
static uint32_t retry_at;

/* answers: a small ring, oldest replaced */
#define CACHE 96
enum { C_EMPTY, C_WAIT, C_BLOCK, C_NAME, C_FOUND, C_ABSENT };
typedef struct {
    uint8_t kind, st;
    uint32_t key;
    uint32_t first, n;                  /* the block being read */
    char name[BHID_NAME];
} cent_t;
static cent_t cache[CACHE];
static int cache_next, busy = -1;       /* busy: the entry whose read is out */

static const char *const PATHS[] = { BHL_FILE1, BHL_FILE2 };

void bhl_open(void)
{
    memset(&hd, 0, sizeof(hd));
    memset(cache, 0, sizeof(cache));
    cache_next = 0; busy = -1; job = -1;
    lst = L_HEAD; path_i = 0; idx_i = 0; retry_at = 0;
    path = PATHS[0];
}
void bhl_close(void)
{
    /* an index still being read stays: the card job writes into it until it ends */
    for (int i = 0; i < hd.nt; i++) if (!(job >= 0 && lst == L_INDEX && i == idx_i)) { free(hd.t[i].idx); hd.t[i].idx = NULL; }
    hd.nt = 0;
    lst = L_NONE;
    job = -1;                           /* an unfinished read is the firmware's to drop */
}
int bhl_state(char *out, size_t cap)
{
    if (out && cap) {
        if (lst == L_READY) {
            const bhid_tab_t *c = bhid_table(&hd, BHID_COMPANY), *u = bhid_table(&hd, BHID_UUID), *o = bhid_table(&hd, BHID_OUI);
            snprintf(out, cap, "%u co, %u svc, %u makers", c ? (unsigned)c->count : 0u, u ? (unsigned)u->count : 0u,
                     o ? (unsigned)o->count : 0u);
        } else if (lst == L_HEAD || lst == L_INDEX) snprintf(out, cap, "loading %s", path ? path : "");
        else snprintf(out, cap, "none: bughunt-ids.bin from PACKS, or %s", BHL_FILE2);
    }
    return lst == L_READY ? 2 : lst == L_HEAD || lst == L_INDEX ? 1 : 0;
}

static int start(uint32_t off, void *buf, uint32_t len)
{
    const int j = lap_sd_read(path, off, buf, len);
    if (j == LAP_SD_EBUSY) return 0;    /* all card jobs out: next frame */
    if (j < 0) return -1;
    job = j;
    return 1;
}
/* The read that is out: LAP_SD_PENDING, bytes, or an error. */
static int finish(void)
{
    if (job < 0) return LAP_SD_PENDING - 1;
    const int r = lap_sd_poll(job);
    if (r != LAP_SD_PENDING) job = -1;
    return r;
}

static void load_step(void)
{
    if (lst == L_HEAD) {
        if (job < 0) {
            if (lap_millis() < retry_at) return;
            if (start(0, headbuf, sizeof(headbuf)) < 0) lst = L_FAILED;
            return;
        }
        const int r = finish();
        if (r == LAP_SD_PENDING) return;
        if (r > 0 && bhid_head(&hd, headbuf, (uint32_t)r)) { lst = L_INDEX; idx_i = 0; return; }
        /* not this file: the next place, then give up */
        if (++path_i < (int)(sizeof(PATHS) / sizeof(PATHS[0]))) path = PATHS[path_i];
        else lst = L_FAILED;
        return;
    }
    if (lst == L_INDEX) {
        if (idx_i >= hd.nt) { lst = L_READY; return; }
        bhid_tab_t *t = &hd.t[idx_i];
        if (job < 0) {
            const uint32_t len = t->nidx * t->keyb;
            if (!t->idx && !(t->idx = malloc(len ? len : 1))) { lst = L_FAILED; return; }
            if (start(t->idx_off, t->idx, len) < 0) lst = L_FAILED;
            return;
        }
        const int r = finish();
        if (r == LAP_SD_PENDING) return;
        if (r != (int)(t->nidx * t->keyb)) { free(t->idx); t->idx = NULL; t->nidx = 0; }   /* that table is left out */
        idx_i++;
    }
}

static void lookup_step(void)
{
    if (busy < 0) {
        for (int k = 0; k < CACHE; k++) {
            const int i = (cache_next + k) % CACHE;      /* oldest asks first */
            if (cache[i].st == C_WAIT) { busy = i; break; }
        }
        if (busy < 0) return;
    }
    cent_t *c = &cache[busy];
    bhid_tab_t *t = bhid_table(&hd, c->kind);
    if (c->st == C_WAIT) {
        if (!t || !bhid_block(t, c->key, &c->first, &c->n) || c->n * t->recb > sizeof(blk)) { c->st = C_ABSENT; busy = -1; return; }
        const int s = start(t->rec_off + c->first * t->recb, blk, c->n * t->recb);
        if (s > 0) c->st = C_BLOCK;
        else if (s < 0) { c->st = C_ABSENT; busy = -1; }
        return;
    }
    const int r = finish();
    if (r == LAP_SD_PENDING) return;
    if (c->st == C_BLOCK) {
        const long off = r == (int)(c->n * t->recb) ? bhid_in_block(t, blk, c->n, c->key) : -1;
        if (off < 0 || (uint32_t)off >= hd.pool_len) { c->st = C_ABSENT; busy = -1; return; }
        uint32_t len = hd.pool_len - (uint32_t)off;
        if (len > BHID_NAME) len = BHID_NAME;
        memset(namebuf, 0, sizeof(namebuf));
        if (start(hd.pool_off + (uint32_t)off, namebuf, len) <= 0) { c->st = C_WAIT; return; }   /* busy: the block again later */
        c->st = C_NAME;
        return;
    }
    if (c->st == C_NAME) {
        if (r > 0) { memcpy(c->name, namebuf, sizeof(c->name)); bhid_name_fix(c->name, sizeof(c->name)); c->st = c->name[0] ? C_FOUND : C_ABSENT; }
        else c->st = C_ABSENT;
        busy = -1;
    }
}

void bhl_pump(void)
{
    if (lst == L_HEAD || lst == L_INDEX) load_step();
    else if (lst == L_READY) {
        for (int i = 0; i < 2; i++) lookup_step();     /* a finished read starts the next at once */
    }
}

const char *bhl_get(int kind, uint32_t key)
{
    if (lst == L_FAILED || lst == L_NONE) return NULL;
    for (int i = 0; i < CACHE; i++)
        if (cache[i].st != C_EMPTY && cache[i].kind == kind && cache[i].key == key)
            return cache[i].st == C_FOUND ? cache[i].name : NULL;
    /* new: take the oldest slot that is not being read */
    int i = cache_next;
    for (int k = 0; k < CACHE && i == busy; k++) i = (i + 1) % CACHE;
    if (i == busy) return NULL;
    cache_next = (i + 1) % CACHE;
    cache[i].kind = (uint8_t)kind;
    cache[i].key = key;
    cache[i].st = C_WAIT;
    cache[i].name[0] = 0;
    return NULL;
}
const char *bhl_maker(const uint8_t mac[6])
{
    if (mac[0] & 0x02) return NULL;     /* locally administered: random, no maker */
    return bhl_get(BHID_OUI, ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2]);
}
