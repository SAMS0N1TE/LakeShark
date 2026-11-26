/* Bluetooth and MAC names off the card: see lap_ids.h. The app engine: card
   reads through lap_sd_*. The format, UUIDs and line parsing are in
   lap_ids_fmt.c. */
#include "lap_ids.h"
#include "lap_ids_fmt.h"
#include "lakeshark_app.h"
#include <stdio.h>
#include <string.h>

/* ---- the engine ---------------------------------------------------------- */

enum { L_NONE, L_USER, L_DEVSIZE, L_DEVREAD, L_HEAD, L_INDEX, L_READY, L_NOFILE };
static int lst = L_NONE, path_i, idx_i, job = -1;
static const char *const PATHS[] = { LAP_IDS_FILE1, LAP_IDS_FILE2, LAP_IDS_FILE3 };
static lap_ids_head_t hd;
static uint8_t headbuf[LAP_IDS_HEAD_MAX];
static uint8_t blk[64 * 7];             /* one block of records: every <= 64, key <= 4 */
static char namebuf[LAP_IDS_NAME];
#define IDX_POOL 12288
static uint8_t idx_pool[IDX_POOL];
static uint32_t idx_used;
static char io[LAP_IDS_USER_BYTES];     /* a text file being read */
static uint32_t dev_size, dev_off;

#define CACHE 128
enum { C_EMPTY, C_WAIT, C_BLOCK, C_NAME, C_FOUND, C_ABSENT };
typedef struct {
    uint8_t kind, st;
    uint32_t key, first, n;
    char name[LAP_IDS_NAME];
} cent_t;
static cent_t cache[CACHE];
static int cache_next, busy = -1;

static lap_ids_user_t users[LAP_IDS_USER_MAX];
static int nusers;

typedef struct {
    uint8_t mac[6];                     /* shown order */
    bool owner;
    uint32_t age;                       /* bigger is newer */
    char name[LAP_IDS_NAME];
} dev_t_;
static dev_t_ devs[LAP_IDS_DEVICES];
static int ndevs;
static uint32_t dev_age;

/* appends waiting to go to the card, one write job at a time */
#define WQ 12
typedef struct { const char *path; uint16_t n; char text[96]; } wline_t;
static wline_t wq[WQ];
static int wq_head, wq_n, wjob = -1;
static bool compact_due;

static void wq_push(const char *path, const char *text)
{
    if (wq_n == WQ) return;             /* full: the name stays held for this session */
    wline_t *w = &wq[(wq_head + wq_n) % WQ];
    w->path = path;
    snprintf(w->text, sizeof(w->text), "%s\n", text);
    w->n = (uint16_t)strlen(w->text);
    wq_n++;
}

static void writes_step(void)
{
    if (wjob >= 0) {
        const int r = lap_sd_poll(wjob);
        if (r == LAP_SD_PENDING) return;
        wjob = -1;
    }
    if (!wq_n) return;
    const wline_t *w = &wq[wq_head];
    const int j = lap_sd_write(w->path, LAP_SD_APPEND, w->text, w->n);
    if (j == LAP_SD_EBUSY) return;      /* next frame */
    wq_head = (wq_head + 1) % WQ;
    wq_n--;
    if (j >= 0) wjob = j;
}

static dev_t_ *dev_find(const uint8_t be[6])
{
    for (int i = 0; i < ndevs; i++) if (!memcmp(devs[i].mac, be, 6)) return &devs[i];
    return NULL;
}

/* A slot for a new address: a free one, else the oldest learned one. */
static dev_t_ *dev_slot(void)
{
    if (ndevs < LAP_IDS_DEVICES) { memset(&devs[ndevs], 0, sizeof(devs[0])); return &devs[ndevs++]; }
    dev_t_ *old = NULL;
    for (int i = 0; i < ndevs; i++) if (!devs[i].owner && (!old || devs[i].age < old->age)) old = &devs[i];
    if (old) memset(old, 0, sizeof(*old));
    return old;
}

static void dev_put(const uint8_t be[6], const char *name, bool owner)
{
    dev_t_ *d = dev_find(be);
    if (d && d->owner && !owner) return;
    if (!d && !(d = dev_slot())) return;
    memcpy(d->mac, be, 6);
    d->owner = owner;
    d->age = ++dev_age;
    snprintf(d->name, sizeof(d->name), "%s", name);
}

static int user_put(const lap_ids_user_t *u)
{
    if (u->kind == LAP_IDS_DEVICE) { dev_put(u->mac, u->name, true); return 0; }
    for (int i = 0; i < nusers; i++)
        if (users[i].kind == u->kind && users[i].key == u->key) { users[i] = *u; return 0; }
    if (nusers == LAP_IDS_USER_MAX) return -2;
    users[nusers++] = *u;
    return 0;
}

/* Every whole line of io[0..n). skip_first: the read started mid-file. */
static void parse_text(uint32_t n, bool devices, bool skip_first)
{
    uint32_t i = 0;
    if (skip_first) { while (i < n && io[i] != '\n') i++; i++; }
    while (i < n) {
        uint32_t e = i;
        while (e < n && io[e] != '\n') e++;
        if (e == n && n == sizeof(io)) break;           /* cut by the buffer: not a whole line */
        const char *l = io + i;
        const size_t len = e - i;
        if (devices) {
            uint8_t m[6];
            char nm[LAP_IDS_NAME];
            if (lap_ids_device_line(l, len, m, nm, sizeof(nm))) dev_put(m, nm, false);
        } else {
            lap_ids_user_t u;
            if (lap_ids_parse_line(l, len, &u) == 1) user_put(&u);
        }
        i = e + 1;
    }
}

void lap_ids_open(void)
{
    memset(&hd, 0, sizeof(hd));
    memset(cache, 0, sizeof(cache));
    cache_next = 0; busy = -1; job = -1; wjob = -1;
    nusers = 0; ndevs = 0; dev_age = 0; idx_used = 0;
    wq_head = wq_n = 0; compact_due = false;
    path_i = 0; idx_i = 0;
    lst = L_USER;
}

void lap_ids_close(void)
{
    /* an unfinished read is the firmware's to drop: the buffers are static */
    lst = L_NONE;
    job = -1;
    busy = -1;
}

static int start(const char *path, uint32_t off, void *buf, uint32_t len)
{
    const int j = lap_sd_read(path, off, buf, len);
    if (j == LAP_SD_EBUSY) return 0;
    if (j < 0) return -1;
    job = j;
    return 1;
}

static int finish(void)
{
    if (job < 0) return LAP_SD_PENDING - 1;
    const int r = lap_sd_poll(job);
    if (r != LAP_SD_PENDING) job = -1;
    return r;
}

/* The device file rewritten from what is held, owner names left out (they
   live in the user file). */
static void compact(void)
{
    uint32_t n = 0;
    for (int i = 0; i < ndevs && n + 64 < sizeof(io); i++) {
        if (devs[i].owner) continue;
        char m[18];
        lap_ids_mac_text(devs[i].mac, m, sizeof(m));
        n += (uint32_t)snprintf(io + n, sizeof(io) - n, "%s %s\n", m, devs[i].name);
    }
    if (lap_sd_write(LAP_IDS_DEVICE_FILE, 0, io, n) >= 0) compact_due = false;
}

static void load_step(void)
{
    if (lst == L_USER || lst == L_DEVREAD) {
        const bool devs_file = lst == L_DEVREAD;
        if (job < 0) {
            const int s = devs_file ? start(LAP_IDS_DEVICE_FILE, dev_off, io, sizeof(io))
                                    : start(LAP_IDS_USER_FILE, 0, io, sizeof(io));
            if (s < 0) lst = devs_file ? L_HEAD : L_DEVSIZE;
            return;
        }
        const int r = finish();
        if (r == LAP_SD_PENDING) return;
        if (r > 0) parse_text((uint32_t)r, devs_file, devs_file && dev_off > 0);
        if (devs_file) { lst = L_HEAD; if (compact_due) compact(); }
        else lst = L_DEVSIZE;
        return;
    }
    if (lst == L_DEVSIZE) {
        if (job < 0) {
            const int j = lap_sd_size(LAP_IDS_DEVICE_FILE);
            if (j == LAP_SD_EBUSY) return;
            if (j < 0) { lst = L_HEAD; return; }
            job = j;
            return;
        }
        const int r = finish();
        if (r == LAP_SD_PENDING) return;
        if (r <= 0) { lst = L_HEAD; return; }
        dev_size = (uint32_t)r;
        /* the newest lines: later lines win, so the tail is what counts */
        dev_off = dev_size > sizeof(io) ? dev_size - sizeof(io) : 0;
        compact_due = dev_size > 3u * sizeof(io);
        lst = L_DEVREAD;
        return;
    }
    if (lst == L_HEAD) {
        if (job < 0) {
            if (start(PATHS[path_i], 0, headbuf, sizeof(headbuf)) < 0) {
                if (++path_i >= (int)(sizeof(PATHS) / sizeof(PATHS[0]))) lst = L_NOFILE;
            }
            return;
        }
        const int r = finish();
        if (r == LAP_SD_PENDING) return;
        if (r > 0 && lap_ids_head(&hd, headbuf, (uint32_t)r)) { lst = L_INDEX; idx_i = 0; return; }
        if (++path_i >= (int)(sizeof(PATHS) / sizeof(PATHS[0]))) lst = L_NOFILE;
        return;
    }
    if (lst == L_INDEX) {
        if (idx_i >= hd.nt) { lst = L_READY; return; }
        lap_ids_tab_t *t = &hd.t[idx_i];
        const uint32_t len = t->nidx * t->keyb;
        if (job < 0) {
            if (!t->idx) {
                if (idx_used + len > IDX_POOL) { t->nidx = 0; idx_i++; return; }   /* too big: that table is left out */
                t->idx = idx_pool + idx_used;
                idx_used += len;
            }
            if (start(PATHS[path_i], t->idx_off, t->idx, len) < 0) { t->idx = NULL; t->nidx = 0; idx_i++; }
            return;
        }
        const int r = finish();
        if (r == LAP_SD_PENDING) return;
        if (r != (int)len) { t->idx = NULL; t->nidx = 0; }
        idx_i++;
    }
}

static void lookup_step(void)
{
    if (busy < 0) {
        for (int k = 0; k < CACHE; k++) {
            const int i = (cache_next + k) % CACHE;
            if (cache[i].st == C_WAIT) { busy = i; break; }
        }
        if (busy < 0) return;
    }
    cent_t *c = &cache[busy];
    lap_ids_tab_t *t = lap_ids_table(&hd, c->kind);
    if (c->st == C_WAIT) {
        if (!t || !lap_ids_block(t, c->key, &c->first, &c->n) || c->n * t->recb > sizeof(blk)) {
            c->st = C_ABSENT;
            busy = -1;
            return;
        }
        const int s = start(PATHS[path_i], t->rec_off + c->first * t->recb, blk, c->n * t->recb);
        if (s > 0) c->st = C_BLOCK;
        else if (s < 0) { c->st = C_ABSENT; busy = -1; }
        return;
    }
    const int r = finish();
    if (r == LAP_SD_PENDING) return;
    if (c->st == C_BLOCK) {
        const long off = r == (int)(c->n * t->recb) ? lap_ids_in_block(t, blk, c->n, c->key) : -1;
        if (off < 0 || (uint32_t)off >= hd.pool_len) { c->st = C_ABSENT; busy = -1; return; }
        uint32_t len = hd.pool_len - (uint32_t)off;
        if (len > LAP_IDS_NAME) len = LAP_IDS_NAME;
        memset(namebuf, 0, sizeof(namebuf));
        if (start(PATHS[path_i], hd.pool_off + (uint32_t)off, namebuf, len) <= 0) { c->st = C_WAIT; return; }
        c->st = C_NAME;
        return;
    }
    if (c->st == C_NAME) {
        if (r > 0) {
            memcpy(c->name, namebuf, sizeof(c->name));
            lap_ids_name_fix(c->name, sizeof(c->name));
            c->st = c->name[0] ? C_FOUND : C_ABSENT;
        } else c->st = C_ABSENT;
        busy = -1;
    }
}

void lap_ids_pump(void)
{
    if (lst == L_NONE) return;
    writes_step();
    if (lst != L_READY && lst != L_NOFILE) load_step();
    else if (lst == L_READY) for (int i = 0; i < 2; i++) lookup_step();
}

static const char *user_get(int kind, uint32_t key)
{
    for (int i = 0; i < nusers; i++) if (users[i].kind == kind && users[i].key == key) return users[i].name;
    return NULL;
}

const char *lap_ids_get(int kind, uint32_t key)
{
    const char *u = user_get(kind, key);
    if (u) return u;
    if (lst != L_READY && lst != L_INDEX && lst != L_HEAD && lst != L_USER && lst != L_DEVSIZE && lst != L_DEVREAD)
        return NULL;
    for (int i = 0; i < CACHE; i++)
        if (cache[i].st != C_EMPTY && cache[i].kind == kind && cache[i].key == key)
            return cache[i].st == C_FOUND ? cache[i].name : NULL;
    int i = cache_next;                 /* new: the oldest slot not being read */
    for (int k = 0; k < CACHE && i == busy; k++) i = (i + 1) % CACHE;
    if (i == busy) return NULL;
    cache_next = (i + 1) % CACHE;
    cache[i].kind = (uint8_t)kind;
    cache[i].key = key;
    cache[i].st = C_WAIT;
    cache[i].name[0] = 0;
    return NULL;
}

const char *lap_ids_attr(int kind, const uint8_t be[16])
{
    const int s = lap_ids_uuid_short(be);
    if (s < 0) return lap_ids_get(LAP_IDS_UUID128, lap_ids_uuid_key(be));
    const char *n = lap_ids_get(kind, (uint32_t)s);
    if (!n && kind != LAP_IDS_SERVICE) n = lap_ids_get(LAP_IDS_SERVICE, (uint32_t)s);
    return n;
}

const char *lap_ids_maker(const uint8_t mac[6])
{
    if (mac[0] & 0x02) return NULL;     /* locally administered: random, no maker */
    return lap_ids_get(LAP_IDS_OUI, (uint32_t)mac[0] << 16 | (uint32_t)mac[1] << 8 | mac[2]);
}

int lap_ids_state(char *out, size_t cap)
{
    const int st = lst == L_READY ? 2 : lst == L_NOFILE || lst == L_NONE ? 0 : 1;
    if (out && cap) {
        if (lst == L_READY) {
            uint32_t n[LAP_IDS_KINDS] = {0};
            for (int i = 0; i < hd.nt; i++) if (hd.t[i].kind < LAP_IDS_KINDS && hd.t[i].nidx) n[hd.t[i].kind] = hd.t[i].count;
            snprintf(out, cap, "%u co %u svc %u chr %u vendor %u makers, %d yours, %d devices",
                     (unsigned)n[LAP_IDS_COMPANY], (unsigned)n[LAP_IDS_SERVICE], (unsigned)n[LAP_IDS_CHAR],
                     (unsigned)n[LAP_IDS_UUID128], (unsigned)n[LAP_IDS_OUI], nusers, ndevs);
        } else if (st == 1) snprintf(out, cap, "loading names");
        else snprintf(out, cap, "no name file: ble-ids from PACKS; %d yours, %d devices", nusers, ndevs);
    }
    return st;
}

int lap_ids_user_count(void) { return nusers; }
int lap_ids_device_count(void) { return ndevs; }

int lap_ids_user_add(const char *kind_word, const char *key_text, const char *name)
{
    if (!kind_word || !key_text || !name) return -1;
    char line[128];
    snprintf(line, sizeof(line), "%s %s %s", kind_word, key_text, name);
    lap_ids_user_t u;
    if (lap_ids_parse_line(line, strlen(line), &u) != 1) return -1;
    const int r = user_put(&u);
    if (r) return r;
    /* the file gets the line in canonical form */
    char key[48];
    if (u.kind == LAP_IDS_DEVICE) lap_ids_mac_text(u.mac, key, sizeof(key));
    else snprintf(key, sizeof(key), "%s", key_text);
    snprintf(line, sizeof(line), "%s %s %s", u.kind == LAP_IDS_UUID128 ? lap_ids_kind_word(LAP_IDS_SERVICE) : kind_word,
             key, u.name);
    if (wq_n == WQ) return -3;
    wq_push(LAP_IDS_USER_FILE, line);
    return 0;
}

const char *lap_ids_device(const uint8_t addr[6], uint8_t type)
{
    uint8_t be[6];
    for (int i = 0; i < 6; i++) be[i] = addr[5 - i];
    if (!lap_ids_keepable(be, type)) return NULL;
    const dev_t_ *d = dev_find(be);
    return d ? d->name : NULL;
}

int lap_ids_device_learn(const uint8_t addr[6], uint8_t type, const char *name)
{
    uint8_t be[6];
    for (int i = 0; i < 6; i++) be[i] = addr[5 - i];
    if (!name || !lap_ids_keepable(be, type)) return 0;
    char nm[LAP_IDS_NAME];
    if (!lap_ids_heard_name(name, nm, sizeof(nm))) return 0;
    dev_t_ *d = dev_find(be);
    if (d && (d->owner || !strcmp(d->name, nm))) return 0;
    dev_put(be, nm, false);
    char line[64];
    char m[18];
    lap_ids_mac_text(be, m, sizeof(m));
    snprintf(line, sizeof(line), "%s %s", m, nm);
    wq_push(LAP_IDS_DEVICE_FILE, line);
    return 1;
}
