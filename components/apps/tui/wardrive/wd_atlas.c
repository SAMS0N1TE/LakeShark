/* WARDRIVE's memory across drives (wd_atlas.h).

   FILES, in wd_feed_dir()/atlas (/sdcard/wardrive/atlas on the board)
     atlas.id   this card's board id, 8 hex digits, made once at random. It
                is the origin every record made here carries.
     log.bin    the truth: an append-only log of records (below). Nothing in
                it is ever rewritten. A power cut mid-write leaves a partial
                record at the end; loading stops at the last whole record and
                the next write goes over the rest.
     state.bin  a snapshot of the runs, routes and place cells as of a log
                seq and offset; loading reads it and replays the log after
                it. Written to state.tmp, then swapped in.
     dev.idx    the device index, compacted from the log: one 88-byte entry
                per device (kind + address), sorted, as of a log seq and
                offset. Rebuilt by merging the log's newer DEV records into
                the old index (dev.tmp, then swapped in) after each drive.
                Losing state.bin or dev.idx costs a rebuild from the log,
                nothing else.

   RECORDS (log.bin and the home-sync stream carry the same bytes)
     log.bin starts "WDAL" 01 00 00 00; a delta buffer starts "WA" 01 00.
     Each record, little endian:
       A7            sync
       type          1 RUN, 2 CELLS, 3 DEV
       len           payload bytes, at most 240
       seq   u32     the writer's change number (a receiver gives its own)
       payload[len]
       crc   u16     CRC-16/CCITT (0x1021, from 0xFFFF) over type..payload
     Every payload starts origin u32 (the board that made the run; 0 in a
     stream means "the sender", filled in from wd_atlas_apply's origin) and
     run u16 (that board's run number).
     RUN   origin run start:u32 end:u32 stem:u32 dist_m:u32 ndev:u16
           nnew:u16 src:u8 flags:u8                                 (28)
           A run begins (end 0) and ends (end set) with one of these. Merged
           field by field (start min, end/dist/ndev/nnew max, src|flags OR,
           stem kept), so a repeat changes nothing.
     CELLS origin run ri:u16 n:u8 cx:i32 cy:i32 src:u8 then n-1 times
           dx:i8 dy:i8 src:u8                                (18+3(n-1))
           Grid cells entered, in order; src is the WD_AS_* bits listening.
     DEV   origin run ri:u16 kind:u8 addr[6] radios:u8 sub:u8 tags:u16
           rssi:i8 lat:i32 lon:i32 (1e-7 deg, INT32_MIN none) first:u32
           last:u32 (UTC) seen:u16 nlen:u8 name[nlen]          (39+nlen)
           A device heard on the run: `seen` sightings since its last
           record on this run, its strongest place so far on the run.
     ri numbers a run's CELLS and DEV records 0, 1, 2... A receiver keeps,
     per run, the next ri it expects and skips anything below it, so the
     same records sent twice, or sent back to the board that made them,
     count once. Records within a run arrive in order (every log keeps them
     in the order it got them); runs from different boards merge in any
     order to the same totals: counts add once per run, firsts/lasts are
     min/max, the strongest place wins, tags and radios OR, a name keeps the
     one heard last.

   ROUTES are this board's own reading of the runs (not synced): a run whose
   distinct cells lie mostly on a route's cells is a run of that route. With
   one cell of slack each way, it matches when at least 60% of the run's
   cells are on the route and it covers at least 30% of the route, or it
   covers 80% of the route with at least 30% of its cells on it. Of the
   routes that match, the best (both fractions summed) wins; a run of six
   cells or more that matches none starts a new route. A route's cells are
   every cell its runs drove, with a count; matching uses the ones at least
   a third of its runs drove, so a one-off detour falls away.

   MEMORY: runs, routes, place cells, the known-device set for the area
   around the car and the run's own cells live in PSRAM tables (about
   0.9 MB, sizes below); the device index stays on the card and is read a
   slice at a time. A compaction borrows 0.4 MB while it runs. */
#include "wd_atlas.h"
#include "wd_feed.h"
#include "wd_replay.h"
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <unistd.h>
#else
#include <direct.h>
#include <time.h>
#define EXT_RAM_BSS_ATTR
#endif

#define CELL_DEG    640.0           /* the place grid: 1/640 degree, ~174 m north-south */
#define CELL_CAP    16384           /* place cells (16 B each) */
#define RUN_CAP     4096            /* runs (40 B each): years of twice-a-day commutes */
#define ROUTE_CAP   16
#define ROUTE_CELLS 512             /* ~80 km of road per route */
#define KNOWN_CAP   16384           /* known devices around the car (16 B each) */
#define REG_CAP     384             /* the regulars kept by name for the MISSING list (36 B each) */
#define ACC_N       3               /* run cell sets: the live run, an import, a peer's */
#define ACC_CELLS   1536
#define ACC_HASH    4096
#define PEND_CAP    4096            /* devices one compaction pass or one import folds */
#define PEND_HASH   8192
#define REC_MAX     240
#define REC_OVH     9
#define SYNC        0xA7
#define LOG_HDR     8
#define MIN_CELLS   6
#define AREA_M      8000.0f         /* the known set: devices this far round the car */
#define AREA_MOVE_M 4000.0f         /* ...read again when the car is this far from its centre */
#define REG_DRIVES  2               /* REGULAR: heard on at least this many earlier drives */
#define FLUSH_MS    60000u          /* devices heard go to the log this often (a power cut loses at most this) */
#define MOVED_M     400.0f
#define NOPOS       INT32_MIN
#define PRUNE_S     (180u * 86400u) /* one sighting on one drive, half a year ago: dropped */
#define RD_BUF      8192

enum { R_RUN = 1, R_CELLS = 2, R_DEV = 3 };
enum { RF_IMPORT = 1, RF_CRASH = 2, RF_ROUTED = 0x80 };
#define RF_SYNCED 0x7F              /* flags that travel; ROUTED is this board's */
#define DYN_TAGS ((uint16_t)(WD_T_REGULAR | WD_T_NEWHERE | WD_T_FOLLOW | WD_T_PEER))

/* ------------------------------------------------------------ tables */

typedef struct { int32_t cx, cy; uint16_t visits, lastrun; uint8_t src[3], used; } cell_t;
typedef struct {
    uint32_t origin;
    uint16_t no, rec_next;
    uint32_t start, end, stem, dist_m;
    uint16_t ndev, nnew, ncells, ngain;
    uint8_t src, flags, route, pad;     /* route: index + 1 */
    uint16_t num, pad2;
} run_t;
typedef struct { int32_t cx, cy; uint16_t hits; uint8_t mask, pad; } rcell_t;
typedef struct {
    char name[12];
    uint16_t nruns, ncells, nref, pad;
    wd_atlas_run_t best, last;
    rcell_t c[ROUTE_CELLS];
} route_t;
#define ROUTE_HEAD ((size_t)offsetof(route_t, c))
typedef struct {
    uint8_t kind, radios, nlen, sub;
    uint8_t addr[6];
    uint16_t tags;
    uint32_t first, last, name_utc;
    int32_t lat, lon;                   /* the strongest place */
    int32_t alat, alon;                 /* weighted by sightings and strength */
    float w;
    uint32_t seen;
    uint16_t drives;
    int8_t rssi;
    uint8_t flags;
    uint32_t last_run;                  /* hash of the last run folded in */
    char name[32];
} adev_t;
typedef struct { adev_t d; uint32_t first_run; } pend_t;
enum { K_SEEN = 1, K_MOVED = 2, K_REG = 4 };
typedef struct { uint8_t kind, addr[6], flags; uint16_t drives, runh; int16_t px, py; } known_t;
typedef struct { uint8_t kind, addr[6], flags; uint16_t drives; int16_t px, py; char name[22]; } reg_t;
typedef struct { int32_t cx, cy; uint8_t mask; } acell_t;
typedef struct { uint32_t runh; int n; uint32_t age; acell_t c[ACC_CELLS]; uint16_t h[ACC_HASH]; } acc_t;
typedef struct { uint8_t type, len; uint16_t size; uint32_t seq; uint8_t p[REC_MAX]; } rec_t;
typedef struct { FILE *f; uint8_t *buf; int n, pos; long off; bool eof; } lrd_t;

_Static_assert(sizeof(cell_t) == 16, "cell layout");
_Static_assert(sizeof(run_t) == 40, "run layout");
_Static_assert(sizeof(adev_t) == 88, "device layout");
_Static_assert(sizeof(wd_atlas_run_t) == 20, "run summary layout");

static cell_t *C;
static run_t *RU;
static route_t *RT;
static known_t *KN;
static reg_t *RG;
static acc_t *AC;
static struct { uint8_t kind, addr[6], named; uint32_t seen; } *FL;
static uint32_t *DIRTY, *CHECKED;
static uint8_t *BUF_LOAD, *BUF_KNOWN, *BUF_JOB;
static pend_t *PD;
static uint16_t *PH;
static int npend;

static struct {
    int state;
    bool loading_failed;
    uint32_t id, seq;
    long log_end;
    uint32_t state_seq;
    uint32_t dev_seq, dev_count, dev_kind[8];
    long dev_off;
    uint32_t tail_dev;
    uint16_t next_no, route_names;
    int nruns, nroutes;
    uint32_t ncells, total_m, done_runs;
    bool cells_full, state_dirty, known_stale;
    int req, job, job_pct, import_left;
    char note[56];
    char dir[128];
    uint32_t last_try;
    int run_cache[4];
    bool applied;                   /* a peer's records came in since the last step */
    uint32_t quiet_from;            /* ...and the step that noticed: compaction waits for quiet */
} A = { .import_left = -1 };

static struct {
    bool on;
    unsigned evt_begin, evt_end, done_begin, done_end;
    struct { unsigned gen; uint32_t end_ms; unsigned ndev, nnew; char stamp[24]; } fin;
    bool have_run;
    uint32_t origin, runh;
    uint16_t no;
    bool have_cell;
    int32_t ccx, ccy;
    uint8_t src_union;
    bool have_p;
    double plat, plon;
    float dist;
    uint32_t flush_ms, think_ms, start_ms;
    unsigned ndev, nnew, nreg, nmoved, nmissing;
    unsigned gain_src[WD_AS_N];
    int route_guess;
    wd_atlas_item_t newr[32];
    int new_head, new_n;
    wd_atlas_item_t movr[16];
    int mov_head, mov_n;
} L = { .route_guess = -1 };

enum { KS_IDLE, KS_IDX, KS_LOG };
static struct {
    bool ready;
    int state;
    double lat0, lon0;
    float m_lat, m_lon;
    unsigned n;
    int nreg;
    FILE *idx;
    uint32_t idx_left;
    lrd_t rd;
} K;

/* ------------------------------------------------------------ small things */

#ifdef ESP_PLATFORM
static SemaphoreHandle_t s_io;
/* Card IO, one at a time: the worker's step, a home-sync delta or apply, a
   reload. Never taken inside itself, and never under wd_lock. */
static void io_lock(void)
{
    if (!s_io) s_io = xSemaphoreCreateMutex();
    if (s_io) xSemaphoreTake(s_io, portMAX_DELAY);
}
static void io_unlock(void) { if (s_io) xSemaphoreGive(s_io); }
static uint32_t entropy(void) { return (uint32_t)esp_timer_get_time() ^ (uint32_t)(esp_timer_get_time() >> 32); }
#else
static void io_lock(void) {}
static void io_unlock(void) {}
static uint32_t entropy(void) { return (uint32_t)time(NULL) ^ (uint32_t)clock() * 2654435761u ^ (uint32_t)(uintptr_t)&A; }
#endif

static void say(const char *s) { snprintf(A.note, sizeof(A.note), "%s", s); }
static inline bool bit_get(const uint32_t *m, int i) { return (m[i >> 5] >> (i & 31)) & 1u; }
static inline void bit_set(uint32_t *m, int i) { m[i >> 5] |= 1u << (i & 31); }
static inline void bit_clr(uint32_t *m, int i) { m[i >> 5] &= ~(1u << (i & 31)); }

static uint32_t runh(uint32_t origin, uint16_t no)
{
    uint32_t h = origin * 2654435761u ^ ((uint32_t)no * 40503u + 0x9E3779B9u);
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    return h ? h : 1;
}
static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h ? h : 1;
}
static uint32_t key_hash(uint8_t kind, const uint8_t a[6])
{
    uint32_t h = 2166136261u ^ kind;
    for (int i = 0; i < 6; i++) h = (h ^ a[i]) * 16777619u;
    return h ^ (h >> 16);
}
static int key_cmp(uint8_t ka, const uint8_t *aa, uint8_t kb, const uint8_t *ab)
{
    if (ka != kb) return ka < kb ? -1 : 1;
    return memcmp(aa, ab, 6);
}
static uint32_t min_nz(uint32_t a, uint32_t b) { return !a ? b : !b ? a : a < b ? a : b; }
static uint32_t cell_hash(int32_t cx, int32_t cy) { return (uint32_t)cx * 73856093u ^ (uint32_t)cy * 19349663u; }
static int32_t to_e7(double d) { return (int32_t)lround(d * 1e7); }
static void cell_of(double lat, double lon, int32_t *cx, int32_t *cy)
{
    *cx = (int32_t)floor(lon * CELL_DEG);
    *cy = (int32_t)floor(lat * CELL_DEG);
}

static uint16_t crc16(const uint8_t *p, size_t n, uint16_t c)
{
    while (n--) {
        c ^= (uint16_t)(*p++ << 8);
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}
static uint32_t crc32(const void *v, size_t n, uint32_t c)
{
    const uint8_t *p = v;
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* little-endian writer and reader */
typedef struct { uint8_t *p; int n, cap; } wr_t;
static void w8(wr_t *w, uint32_t v) { if (w->n < w->cap) w->p[w->n] = (uint8_t)v; w->n++; }
static void w16(wr_t *w, uint32_t v) { w8(w, v); w8(w, v >> 8); }
static void w32(wr_t *w, uint32_t v) { w16(w, v); w16(w, v >> 16); }
typedef struct { const uint8_t *p; int n, at; bool bad; } rd_t;
static uint32_t r8(rd_t *r) { if (r->at < r->n) return r->p[r->at++]; r->bad = true; return 0; }
static uint32_t r16(rd_t *r) { uint32_t a = r8(r); return a | r8(r) << 8; }
static uint32_t r32(rd_t *r) { uint32_t a = r16(r); return a | r16(r) << 16; }

/* ------------------------------------------------------------ records */

typedef struct {
    uint32_t origin; uint16_t no;
    uint32_t start, end, stem, dist_m;
    uint16_t ndev, nnew;
    uint8_t src, flags;
} m_run_t;
#define CELLS_MAX 75
typedef struct {
    uint32_t origin; uint16_t no, ri;
    int n;
    int32_t cx[CELLS_MAX], cy[CELLS_MAX];
    uint8_t mask[CELLS_MAX];
} m_cells_t;
typedef struct {
    uint32_t origin; uint16_t no, ri;
    uint8_t kind, addr[6], radios, sub, nlen;
    uint16_t tags;
    int8_t rssi;
    int32_t lat, lon;
    uint32_t first, last;
    uint16_t seen;
    char name[33];
} m_dev_t;

static int enc_run(uint8_t *p, const m_run_t *m)
{
    wr_t w = { p, 0, REC_MAX };
    w32(&w, m->origin); w16(&w, m->no); w32(&w, m->start); w32(&w, m->end); w32(&w, m->stem);
    w32(&w, m->dist_m); w16(&w, m->ndev); w16(&w, m->nnew); w8(&w, m->src); w8(&w, m->flags);
    return w.n;
}
static bool dec_run(const rec_t *r, m_run_t *m)
{
    rd_t d = { r->p, r->len, 0, false };
    m->origin = r32(&d); m->no = (uint16_t)r16(&d); m->start = r32(&d); m->end = r32(&d); m->stem = r32(&d);
    m->dist_m = r32(&d); m->ndev = (uint16_t)r16(&d); m->nnew = (uint16_t)r16(&d);
    m->src = (uint8_t)r8(&d); m->flags = (uint8_t)r8(&d);
    return !d.bad;
}
static int enc_cells(uint8_t *p, const m_cells_t *m)
{
    wr_t w = { p, 0, REC_MAX };
    w32(&w, m->origin); w16(&w, m->no); w16(&w, m->ri); w8(&w, (uint32_t)m->n);
    w32(&w, (uint32_t)m->cx[0]); w32(&w, (uint32_t)m->cy[0]); w8(&w, m->mask[0]);
    for (int i = 1; i < m->n; i++) {
        w8(&w, (uint32_t)(int8_t)(m->cx[i] - m->cx[i - 1]));
        w8(&w, (uint32_t)(int8_t)(m->cy[i] - m->cy[i - 1]));
        w8(&w, m->mask[i]);
    }
    return w.n;
}
static bool dec_cells(const rec_t *r, m_cells_t *m)
{
    rd_t d = { r->p, r->len, 0, false };
    m->origin = r32(&d); m->no = (uint16_t)r16(&d); m->ri = (uint16_t)r16(&d); m->n = (int)r8(&d);
    if (m->n < 1 || m->n > CELLS_MAX) return false;
    m->cx[0] = (int32_t)r32(&d); m->cy[0] = (int32_t)r32(&d); m->mask[0] = (uint8_t)r8(&d);
    for (int i = 1; i < m->n; i++) {
        m->cx[i] = m->cx[i - 1] + (int8_t)r8(&d);
        m->cy[i] = m->cy[i - 1] + (int8_t)r8(&d);
        m->mask[i] = (uint8_t)r8(&d);
    }
    return !d.bad && d.at == d.n;
}
static int enc_dev(uint8_t *p, const m_dev_t *m)
{
    wr_t w = { p, 0, REC_MAX };
    w32(&w, m->origin); w16(&w, m->no); w16(&w, m->ri); w8(&w, m->kind);
    for (int i = 0; i < 6; i++) w8(&w, m->addr[i]);
    w8(&w, m->radios); w8(&w, m->sub); w16(&w, m->tags); w8(&w, (uint32_t)(uint8_t)m->rssi);
    w32(&w, (uint32_t)m->lat); w32(&w, (uint32_t)m->lon); w32(&w, m->first); w32(&w, m->last);
    w16(&w, m->seen);
    const int nl = m->nlen > 32 ? 32 : m->nlen;
    w8(&w, (uint32_t)nl);
    for (int i = 0; i < nl; i++) w8(&w, (uint8_t)m->name[i]);
    return w.n;
}
static bool dec_dev(const rec_t *r, m_dev_t *m)
{
    rd_t d = { r->p, r->len, 0, false };
    m->origin = r32(&d); m->no = (uint16_t)r16(&d); m->ri = (uint16_t)r16(&d); m->kind = (uint8_t)r8(&d);
    for (int i = 0; i < 6; i++) m->addr[i] = (uint8_t)r8(&d);
    m->radios = (uint8_t)r8(&d); m->sub = (uint8_t)r8(&d); m->tags = (uint16_t)r16(&d); m->rssi = (int8_t)r8(&d);
    m->lat = (int32_t)r32(&d); m->lon = (int32_t)r32(&d); m->first = r32(&d); m->last = r32(&d);
    m->seen = (uint16_t)r16(&d);
    m->nlen = (uint8_t)r8(&d);
    if (m->nlen > 32) return false;
    for (int i = 0; i < m->nlen; i++) m->name[i] = (char)r8(&d);
    m->name[m->nlen] = 0;
    return !d.bad && d.at == d.n && m->kind < WD_KINDS;
}
/* origin and run are the first six bytes of every payload */
static uint32_t rec_origin(const rec_t *r) { return r->p[0] | r->p[1] << 8 | r->p[2] << 16 | (uint32_t)r->p[3] << 24; }
static uint16_t rec_no(const rec_t *r) { return (uint16_t)(r->p[4] | r->p[5] << 8); }
static uint16_t rec_ri(const rec_t *r) { return (uint16_t)(r->p[6] | r->p[7] << 8); }
static void rec_set_origin(rec_t *r, uint32_t o) { r->p[0] = (uint8_t)o; r->p[1] = (uint8_t)(o >> 8); r->p[2] = (uint8_t)(o >> 16); r->p[3] = (uint8_t)(o >> 24); }

static int frame(uint8_t *out, uint8_t type, uint32_t seq, const uint8_t *p, int len)
{
    out[0] = SYNC; out[1] = type; out[2] = (uint8_t)len;
    out[3] = (uint8_t)seq; out[4] = (uint8_t)(seq >> 8); out[5] = (uint8_t)(seq >> 16); out[6] = (uint8_t)(seq >> 24);
    memcpy(out + 7, p, (size_t)len);
    const uint16_t c = crc16(out + 1, (size_t)len + 6, 0xFFFF);
    out[7 + len] = (uint8_t)c; out[8 + len] = (uint8_t)(c >> 8);
    return len + REC_OVH;
}
/* A record at b: its size; 0 when more bytes are needed; -1 not a record. */
static int unframe(const uint8_t *b, size_t avail, rec_t *r)
{
    if (avail < 1) return 0;
    if (b[0] != SYNC) return -1;
    if (avail < 3) return 0;
    const int len = b[2];
    if (len > REC_MAX || b[1] < R_RUN || b[1] > R_DEV) return -1;
    if (avail < (size_t)len + REC_OVH) return 0;
    const uint16_t c = crc16(b + 1, (size_t)len + 6, 0xFFFF);
    if ((b[7 + len] | b[8 + len] << 8) != c) return -1;
    r->type = b[1];
    r->len = (uint8_t)len;
    r->size = (uint16_t)(len + REC_OVH);
    r->seq = b[3] | b[4] << 8 | b[5] << 16 | (uint32_t)b[6] << 24;
    memcpy(r->p, b + 7, (size_t)len);
    return len + REC_OVH;
}
static bool rec_valid(const rec_t *r)
{
    if (r->len < 6) return false;
    static union { m_run_t a; m_cells_t b; m_dev_t c; } u;
    switch (r->type) {
    case R_RUN: return dec_run(r, &u.a);
    case R_CELLS: return dec_cells(r, &u.b);
    case R_DEV: return dec_dev(r, &u.c);
    default: return false;
    }
}

/* ------------------------------------------------------------ paths and the log reader */

static void apath(char *out, size_t n, const char *leaf) { snprintf(out, n, "%s/%s", A.dir, leaf); }

static bool lrd_open(lrd_t *r, uint8_t *buf, long at)
{
    char p[160];
    apath(p, sizeof(p), "log.bin");
    memset(r, 0, sizeof(*r));
    r->buf = buf;
    r->f = fopen(p, "rb");
    if (!r->f) return false;
    if (fseek(r->f, at, SEEK_SET)) { fclose(r->f); r->f = NULL; return false; }
    r->off = at;
    return true;
}
static void lrd_close(lrd_t *r) { if (r->f) fclose(r->f); r->f = NULL; }
/* 1 a record (*at its offset), 0 the end or a partial tail, -1 bad bytes. */
static int lrd_next(lrd_t *r, rec_t *rec, long *at)
{
    if (!r->f) return 0;
    if (r->n - r->pos < 256 && !r->eof) {
        memmove(r->buf, r->buf + r->pos, (size_t)(r->n - r->pos));
        r->n -= r->pos;
        r->pos = 0;
        const size_t got = fread(r->buf + r->n, 1, (size_t)(RD_BUF - r->n), r->f);
        r->n += (int)got;
        if (got == 0 || r->n < RD_BUF) r->eof = true;
    }
    if (r->pos >= r->n) return 0;
    const int k = unframe(r->buf + r->pos, (size_t)(r->n - r->pos), rec);
    if (k == 0) return 0;
    if (k < 0) return -1;
    if (at) *at = r->off;
    r->pos += k;
    r->off += k;
    return 1;
}

/* ------------------------------------------------------------ the log writer */

static FILE *s_lw;
static EXT_RAM_BSS_ATTR uint8_t s_frame[REC_MAX + REC_OVH];

static bool log_create(void)
{
    char p[160];
    apath(p, sizeof(p), "log.bin");
    FILE *f = fopen(p, "wb");
    if (!f) return false;
    static const uint8_t H[LOG_HDR] = { 'W', 'D', 'A', 'L', 1, 0, 0, 0 };
    const bool ok = fwrite(H, 1, LOG_HDR, f) == LOG_HDR;
    fclose(f);
    A.log_end = LOG_HDR;
    return ok;
}
static void log_close(void)
{
    if (!s_lw) return;
    fflush(s_lw);
#ifdef ESP_PLATFORM
    fsync(fileno(s_lw));
#endif
    fclose(s_lw);
    s_lw = NULL;
}
static bool log_write(const uint8_t *b, int n)
{
    if (!s_lw) {
        char p[160];
        apath(p, sizeof(p), "log.bin");
        s_lw = fopen(p, "r+b");
        if (!s_lw || fseek(s_lw, A.log_end, SEEK_SET)) {
            if (s_lw) fclose(s_lw);
            s_lw = NULL;
            A.state = WD_ATLAS_NOCARD;
            say("the card would not take the atlas");
            return false;
        }
        /* the reader of a delta or a scan sees what was written */
        setvbuf(s_lw, NULL, _IOFBF, 4096);
    }
    if (fwrite(b, 1, (size_t)n, s_lw) != (size_t)n) {
        /* Part of it may be on the card past log_end. Let go: the next write
           opens at log_end again and goes over it, instead of every later
           record landing after a cut one that stops each boot's replay. */
        fclose(s_lw);
        s_lw = NULL;
        say("atlas write failed");
        return false;
    }
    A.log_end += n;
    return true;
}

/* ------------------------------------------------------------ cells, runs, routes, sets */

static cell_t *cell_find(int32_t cx, int32_t cy)
{
    if (!C) return NULL;
    for (uint32_t i = cell_hash(cx, cy) & (CELL_CAP - 1), k = 0; k < CELL_CAP; k++, i = (i + 1) & (CELL_CAP - 1)) {
        if (!C[i].used) return NULL;
        if (C[i].cx == cx && C[i].cy == cy) return &C[i];
    }
    return NULL;
}
static cell_t *cell_add(int32_t cx, int32_t cy)
{
    uint32_t i = cell_hash(cx, cy) & (CELL_CAP - 1);
    for (uint32_t k = 0; k < CELL_CAP; k++, i = (i + 1) & (CELL_CAP - 1)) {
        if (!C[i].used) break;
        if (C[i].cx == cx && C[i].cy == cy) return &C[i];
    }
    if (A.ncells >= CELL_CAP * 7 / 8) { A.cells_full = true; return NULL; }
    memset(&C[i], 0, sizeof(C[i]));
    C[i].cx = cx; C[i].cy = cy; C[i].used = 1;
    A.ncells++;
    return &C[i];
}
static int nib(const uint8_t s[3], int k) { return (s[k >> 1] >> ((k & 1) * 4)) & 15; }
static void nib_inc(uint8_t s[3], int k)
{
    if (nib(s, k) < 15) s[k >> 1] = (uint8_t)(s[k >> 1] + (1u << ((k & 1) * 4)));
}

static run_t *run_find(uint32_t origin, uint16_t no)
{
    for (int c = 0; c < 4; c++) {
        const int i = A.run_cache[c];
        if (i >= 0 && i < A.nruns && RU[i].origin == origin && RU[i].no == no) return &RU[i];
    }
    for (int i = A.nruns - 1; i >= 0; i--)
        if (RU[i].origin == origin && RU[i].no == no) {
            memmove(A.run_cache + 1, A.run_cache, sizeof(int) * 3);
            A.run_cache[0] = i;
            return &RU[i];
        }
    return NULL;
}
static run_t *run_add(uint32_t origin, uint16_t no)
{
    run_t *e = run_find(origin, no);
    if (e) return e;
    if (A.nruns >= RUN_CAP) {
        /* full: the oldest run leaves (its counts stay folded in) */
        int old = 0;
        for (int i = 1; i < A.nruns; i++) if (min_nz(RU[i].start, 0xFFFFFFFFu) < min_nz(RU[old].start, 0xFFFFFFFFu)) old = i;
        RU[old] = RU[--A.nruns];
        for (int c = 0; c < 4; c++) A.run_cache[c] = -1;
    }
    e = &RU[A.nruns++];
    memset(e, 0, sizeof(*e));
    e->origin = origin;
    e->no = no;
    return e;
}

static acc_t *acc_get(uint32_t h, bool make)
{
    static uint32_t tick;
    acc_t *free_one = NULL, *oldest = NULL;
    for (int i = 0; i < ACC_N; i++) {
        acc_t *a = &AC[i];
        if (a->runh == h) { a->age = ++tick; return a; }
        if (!a->runh && !free_one) free_one = a;
        /* slot 0 belongs to the live run while one records */
        if (i == 0 && L.have_run) continue;
        if (!oldest || a->age < oldest->age) oldest = a;
    }
    if (!make) return NULL;
    acc_t *a = free_one ? free_one : oldest;
    if (!a) return NULL;
    a->runh = h; a->n = 0; a->age = ++tick;
    memset(a->h, 0, sizeof(a->h));
    return a;
}
static void acc_free(acc_t *a) { if (a) { a->runh = 0; a->n = 0; } }
static int acc_find(const acc_t *a, int32_t cx, int32_t cy)
{
    for (uint32_t i = cell_hash(cx, cy) & (ACC_HASH - 1), k = 0; k < ACC_HASH; k++, i = (i + 1) & (ACC_HASH - 1)) {
        if (!a->h[i]) return -1;
        const acell_t *c = &a->c[a->h[i] - 1];
        if (c->cx == cx && c->cy == cy) return a->h[i] - 1;
    }
    return -1;
}
static bool acc_near(const acc_t *a, int32_t cx, int32_t cy)
{
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) if (acc_find(a, cx + dx, cy + dy) >= 0) return true;
    return false;
}
static void acc_add(acc_t *a, int32_t cx, int32_t cy, uint8_t mask)
{
    uint32_t i = cell_hash(cx, cy) & (ACC_HASH - 1);
    for (uint32_t k = 0; k < ACC_HASH; k++, i = (i + 1) & (ACC_HASH - 1)) {
        if (!a->h[i]) break;
        acell_t *c = &a->c[a->h[i] - 1];
        if (c->cx == cx && c->cy == cy) { c->mask |= mask; return; }
    }
    if (a->n >= ACC_CELLS) return;
    a->c[a->n] = (acell_t){ cx, cy, mask };
    a->h[i] = (uint16_t)(++a->n);
}

/* a scratch set of one route's cells for matching */
static EXT_RAM_BSS_ATTR uint16_t s_scr[2048];
static void scr_build(const route_t *r, int minh)
{
    memset(s_scr, 0, sizeof(s_scr));
    for (int j = 0; j < r->ncells; j++) {
        if (r->c[j].hits < minh) continue;
        uint32_t i = cell_hash(r->c[j].cx, r->c[j].cy) & 2047;
        while (s_scr[i]) i = (i + 1) & 2047;
        s_scr[i] = (uint16_t)(j + 1);
    }
}
static int scr_find(const route_t *r, int32_t cx, int32_t cy)
{
    for (uint32_t i = cell_hash(cx, cy) & 2047, k = 0; k < 2048 && s_scr[i]; k++, i = (i + 1) & 2047) {
        const rcell_t *c = &r->c[s_scr[i] - 1];
        if (c->cx == cx && c->cy == cy) return s_scr[i] - 1;
    }
    return -1;
}
static bool scr_near(const route_t *r, int32_t cx, int32_t cy)
{
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) if (scr_find(r, cx + dx, cy + dy) >= 0) return true;
    return false;
}
static int route_minh(const route_t *r) { const int m = (r->nruns + 2) / 3; return m < 1 ? 1 : m; }

/* The route a run's cells are on, -1 none. `loose`: the live guess (only
   its share on the route counts, as the run is not over). */
static int route_match(const acc_t *a, bool loose)
{
    if (!a || a->n < MIN_CELLS) return -1;
    int best = -1;
    float bs = 0;
    for (int r = 0; r < A.nroutes; r++) {
        const route_t *R = &RT[r];
        scr_build(R, route_minh(R));
        int in = 0, core = 0, cov = 0;
        for (int i = 0; i < a->n; i++) if (scr_near(R, a->c[i].cx, a->c[i].cy)) in++;
        for (int j = 0; j < R->ncells; j++) {
            if (R->c[j].hits < route_minh(R)) continue;
            core++;
            if (acc_near(a, R->c[j].cx, R->c[j].cy)) cov++;
        }
        const float fr = (float)in / (float)a->n, fc = core ? (float)cov / (float)core : 0;
        const bool ok = loose ? fr >= 0.7f : (fr >= 0.6f && fc >= 0.3f) || (fc >= 0.8f && fr >= 0.3f);
        if (ok && fr + fc > bs) { bs = fr + fc; best = r; }
    }
    return best;
}
static void route_name(route_t *R)
{
    bool commute = false;
    for (int i = 0; i < A.nroutes; i++) if (!strcmp(RT[i].name, "COMMUTE")) commute = true;
    if (!commute && R->nruns >= 3) { snprintf(R->name, sizeof(R->name), "COMMUTE"); return; }
    if (!R->name[0]) snprintf(R->name, sizeof(R->name), "ROUTE %c", 'A' + (A.route_names++ % 26));
}
static wd_atlas_run_t run_sum(const run_t *u)
{
    wd_atlas_run_t s = { u->num, u->ndev, u->nnew, u->ncells, u->ngain, u->start, u->dist_m };
    return s;
}
/* A finished run onto its route, or a new route. */
static void route_assign(run_t *u, acc_t *a)
{
    u->flags |= RF_ROUTED;
    if (!a || a->n < MIN_CELLS) return;
    int r = route_match(a, false);
    route_t *R;
    if (r < 0) {
        if (A.nroutes < ROUTE_CAP) r = A.nroutes++;
        else {
            /* full: the oldest route driven once makes room */
            for (int i = 0; i < A.nroutes; i++)
                if (RT[i].nruns <= 1 && (r < 0 || RT[i].last.start < RT[r].last.start)) r = i;
            if (r < 0) return;
            for (int i = 0; i < A.nruns; i++) if (RU[i].route == r + 1) RU[i].route = 0;
        }
        R = &RT[r];
        memset(R, 0, ROUTE_HEAD);
        for (int i = 0; i < a->n && R->ncells < ROUTE_CELLS; i++)
            R->c[R->ncells++] = (rcell_t){ a->c[i].cx, a->c[i].cy, 0, a->c[i].mask, 0 };
        R->nref = R->ncells;
        for (int j = 0; j < R->ncells; j++) R->c[j].hits = 1;
    } else {
        R = &RT[r];
        scr_build(R, 0);
        for (int i = 0; i < a->n; i++) {
            const int j = scr_find(R, a->c[i].cx, a->c[i].cy);
            if (j >= 0) { if (R->c[j].hits < 0xFFFF) R->c[j].hits++; R->c[j].mask |= a->c[i].mask; }
            else if (R->ncells < ROUTE_CELLS) R->c[R->ncells++] = (rcell_t){ a->c[i].cx, a->c[i].cy, 1, a->c[i].mask, 0 };
        }
    }
    R->nruns++;
    u->route = (uint8_t)(r + 1);
    u->num = R->nruns;
    route_name(R);
    R->last = run_sum(u);
    if (u->ndev >= R->best.ndev || R->nruns == 1) R->best = run_sum(u);
}

static known_t *known_find(uint8_t kind, const uint8_t addr[6])
{
    if (!KN) return NULL;
    for (uint32_t i = key_hash(kind, addr) & (KNOWN_CAP - 1), k = 0; k < KNOWN_CAP; k++, i = (i + 1) & (KNOWN_CAP - 1)) {
        known_t *e = &KN[i];
        if (!e->drives) return NULL;
        if (e->kind == kind && !memcmp(e->addr, addr, 6)) return e;
    }
    return NULL;
}
static known_t *known_add(uint8_t kind, const uint8_t addr[6])
{
    uint32_t i = key_hash(kind, addr) & (KNOWN_CAP - 1);
    for (uint32_t k = 0; k < KNOWN_CAP; k++, i = (i + 1) & (KNOWN_CAP - 1)) {
        known_t *e = &KN[i];
        if (!e->drives) break;
        if (e->kind == kind && !memcmp(e->addr, addr, 6)) return e;
    }
    if (K.n >= KNOWN_CAP * 7 / 8) return NULL;
    known_t *e = &KN[i];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    memcpy(e->addr, addr, 6);
    K.n++;
    return e;
}
static void known_pos(int32_t lat, int32_t lon, int16_t *px, int16_t *py)
{
    const float x = (float)((lon * 1e-7 - K.lon0) * K.m_lon) / 10.0f, y = (float)((lat * 1e-7 - K.lat0) * K.m_lat) / 10.0f;
    *px = (int16_t)(x > 32000 ? 32000 : x < -32000 ? -32000 : x);
    *py = (int16_t)(y > 32000 ? 32000 : y < -32000 ? -32000 : y);
}
bool wd_atlas_locate(const wd_obs_t *o, int n, double *lat, double *lon, float *err_m, int *used)
{
    if (!KN || !K.ready || n <= 0) return false;
    enum { MAXU = 24 };
    float px[MAXU], py[MAXU], wv[MAXU];
    double sw = 0, sx = 0, sy = 0;
    int u = 0;
    for (int i = 0; i < n && u < MAXU; i++) {
        if (o[i].kind != WD_WIFI || o[i].radio == WD_RADIO_PEER) continue;
        const known_t *k = known_find(o[i].kind, o[i].addr);
        if (!k || k->drives < 2) continue;                 /* one drive: could be a hotspot passing by */
        const float w = powf(10.0f, ((float)o[i].rssi + 95.0f) / 20.0f);    /* -55 dBm: 100, -85: 3 */
        px[u] = k->px; py[u] = k->py; wv[u] = w;
        sw += w; sx += (double)w * k->px; sy += (double)w * k->py;
        u++;
    }
    if (u < 3 || sw <= 0) return false;
    const double cx = sx / sw, cy = sy / sw;
    double var = 0;
    for (int i = 0; i < u; i++) var += wv[i] * ((px[i] - cx) * (px[i] - cx) + (py[i] - cy) * (py[i] - cy));
    const float spread = (float)(sqrt(var / sw) * 10.0);   /* metres: the cells are 10 m */
    if (spread > 150.0f) return false;
    *err_m = 15.0f + spread / sqrtf((float)u);      /* the access points' spread, averaged down by how many */
    *lat = K.lat0 + cy * 10.0 / K.m_lat;
    *lon = K.lon0 + cx * 10.0 / K.m_lon;
    *used = u;
    return true;
}

static bool in_area(int32_t lat, int32_t lon)
{
    if (lat == NOPOS) return false;
    const float dy = (float)((lat * 1e-7 - K.lat0) * K.m_lat), dx = (float)((lon * 1e-7 - K.lon0) * K.m_lon);
    return dx * dx + dy * dy <= AREA_M * AREA_M;
}

/* ------------------------------------------------------------ folding records into memory */

/* Every record, made here or merged from a peer or replayed at load, goes
   through here, under wd_lock. *changed: a RUN record changed something. */
static void fold(const rec_t *r, bool *changed)
{
    if (changed) *changed = false;
    if (r->seq > A.seq) A.seq = r->seq;
    switch (r->type) {
    case R_RUN: {
        m_run_t m;
        if (!dec_run(r, &m)) return;
        run_t *u = run_find(m.origin, m.no);
        const bool fresh = !u;
        if (!u) u = run_add(m.origin, m.no);
        const run_t before = *u;
        u->start = min_nz(u->start, m.start);
        if (m.end > u->end) u->end = m.end;
        if (m.stem && !u->stem) u->stem = m.stem;
        if (m.dist_m > u->dist_m) u->dist_m = m.dist_m;
        if (m.ndev > u->ndev) u->ndev = m.ndev;
        if (m.nnew > u->nnew) u->nnew = m.nnew;
        u->src |= m.src;
        u->flags |= (uint8_t)(m.flags & RF_SYNCED);
        if (m.origin == A.id && m.no >= A.next_no) A.next_no = (uint16_t)(m.no + 1);
        if (changed) *changed = fresh || memcmp(&before, u, sizeof(before)) != 0;
        if (u->end && !(u->flags & RF_ROUTED)) {
            acc_t *a = acc_get(runh(u->origin, u->no), false);
            route_assign(u, a);
            if (a && a != &AC[0]) acc_free(a);
            A.total_m += u->dist_m;
            A.done_runs++;
        } else if (u->route && memcmp(&before, u, sizeof(before))) {
            /* a late end record (a peer's numbers) refreshes its route's figures */
            route_t *R = &RT[u->route - 1];
            if (R->last.num == u->num) R->last = run_sum(u);
            if (R->best.num == u->num || u->ndev > R->best.ndev) R->best = run_sum(u);
        }
        break;
    }
    case R_CELLS: {
        static m_cells_t m;
        if (!dec_cells(r, &m)) return;
        run_t *u = run_add(m.origin, m.no);
        if (m.ri + 1 > u->rec_next) u->rec_next = (uint16_t)(m.ri + 1);
        const uint32_t h = runh(m.origin, m.no);
        const bool live = L.have_run && h == L.runh;
        acc_t *a = acc_get(h, true);
        for (int i = 0; i < m.n; i++) {
            if (a) acc_add(a, m.cx[i], m.cy[i], m.mask[i]);
            cell_t *c = cell_add(m.cx[i], m.cy[i]);
            if (!c || c->lastrun == (uint16_t)h) continue;
            c->lastrun = (uint16_t)h;
            if (!c->visits) u->ngain++;
            if (c->visits < 0xFFFF) c->visits++;
            u->ncells++;
            for (int s = 0; s < WD_AS_N; s++)
                if (m.mask[i] & (1u << s)) {
                    if (live && !nib(c->src, s)) L.gain_src[s]++;
                    nib_inc(c->src, s);
                }
        }
        break;
    }
    case R_DEV: {
        if (r->len < 8) return;
        run_t *u = run_add(rec_origin(r), rec_no(r));
        const uint16_t ri = rec_ri(r);
        if (ri + 1 > u->rec_next) u->rec_next = (uint16_t)(ri + 1);
        if (r->seq > A.dev_seq) A.tail_dev++;
        break;
    }
    }
}

/* Writes a record to the log with the next seq and folds it. */
static bool emit(uint8_t type, const uint8_t *p, int len, bool *changed)
{
    rec_t r;
    r.type = type;
    r.len = (uint8_t)len;
    r.seq = A.seq + 1;
    memcpy(r.p, p, (size_t)len);
    const int n = frame(s_frame, type, r.seq, p, len);
    r.size = (uint16_t)n;
    if (!log_write(s_frame, n)) return false;
    wd_lock();
    fold(&r, changed);
    wd_unlock();
    return true;
}

/* ------------------------------------------------------------ snapshots */

typedef struct {
    char magic[4];
    uint32_t ver, seq, log_off, id, nruns, nroutes, ncells;
    uint16_t next_no, route_names;
    uint32_t total_m, done_runs, crc;
} state_hdr_t;
typedef struct {
    char magic[4];
    uint32_t ver, count, seq, log_off, kind[8], crc;
} dev_hdr_t;

static bool swap_in(const char *tmp_leaf, const char *leaf)
{
    char a[160], b[160];
    apath(a, sizeof(a), tmp_leaf);
    apath(b, sizeof(b), leaf);
    remove(b);
    return rename(a, b) == 0;
}

static bool state_write(void)
{
    char p[160];
    apath(p, sizeof(p), "state.tmp");
    FILE *f = fopen(p, "wb");
    if (!f) return false;
    static state_hdr_t h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "WDAS", 4);
    h.ver = 1; h.seq = A.seq; h.log_off = (uint32_t)A.log_end; h.id = A.id;
    h.nruns = (uint32_t)A.nruns; h.nroutes = (uint32_t)A.nroutes; h.ncells = A.ncells;
    h.next_no = A.next_no; h.route_names = A.route_names; h.total_m = A.total_m; h.done_runs = A.done_runs;
    uint32_t c = crc32(RU, sizeof(run_t) * (size_t)A.nruns, 0);
    for (int i = 0; i < A.nroutes; i++) {
        c = crc32(&RT[i], ROUTE_HEAD, c);
        c = crc32(RT[i].c, sizeof(rcell_t) * RT[i].ncells, c);
    }
    for (int i = 0; i < CELL_CAP; i++) if (C[i].used) c = crc32(&C[i], sizeof(cell_t), c);
    h.crc = c;
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1;
    ok = ok && fwrite(RU, sizeof(run_t), (size_t)A.nruns, f) == (size_t)A.nruns;
    for (int i = 0; ok && i < A.nroutes; i++) {
        ok = fwrite(&RT[i], ROUTE_HEAD, 1, f) == 1;
        ok = ok && fwrite(RT[i].c, sizeof(rcell_t), RT[i].ncells, f) == RT[i].ncells;
    }
    for (int i = 0; ok && i < CELL_CAP; i++) if (C[i].used) ok = fwrite(&C[i], sizeof(cell_t), 1, f) == 1;
    ok = fflush(f) == 0 && ok;
#ifdef ESP_PLATFORM
    fsync(fileno(f));
#endif
    fclose(f);
    if (ok) ok = swap_in("state.tmp", "state.bin");
    if (ok) { A.state_seq = A.seq; A.state_dirty = false; }
    return ok;
}

static void tables_clear(void)
{
    memset(C, 0, sizeof(cell_t) * CELL_CAP);
    A.nruns = A.nroutes = 0;
    A.ncells = A.total_m = A.done_runs = 0;
    A.next_no = 1;
    A.route_names = 0;
    A.cells_full = false;
    for (int c = 0; c < 4; c++) A.run_cache[c] = -1;
    for (int i = 0; i < ACC_N; i++) acc_free(&AC[i]);
}

static FILE *open_either(const char *leaf, const char *tmp_leaf)
{
    char p[160];
    apath(p, sizeof(p), leaf);
    FILE *f = fopen(p, "rb");
    if (f) return f;
    /* a swap cut short: the new file is still under its temporary name */
    apath(p, sizeof(p), tmp_leaf);
    f = fopen(p, "rb");
    if (f) { fclose(f); if (swap_in(tmp_leaf, leaf)) { apath(p, sizeof(p), leaf); return fopen(p, "rb"); } }
    return NULL;
}

static bool state_read(void)
{
    FILE *f = open_either("state.bin", "state.tmp");
    if (!f) return false;
    static state_hdr_t h;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && !memcmp(h.magic, "WDAS", 4) && h.ver == 1 &&
              h.nruns <= RUN_CAP && h.nroutes <= ROUTE_CAP && h.ncells <= CELL_CAP;
    if (ok) {
        ok = fread(RU, sizeof(run_t), h.nruns, f) == h.nruns;
        uint32_t c = crc32(RU, sizeof(run_t) * h.nruns, 0);
        for (uint32_t i = 0; ok && i < h.nroutes; i++) {
            ok = fread(&RT[i], ROUTE_HEAD, 1, f) == 1 && RT[i].ncells <= ROUTE_CELLS;
            ok = ok && fread(RT[i].c, sizeof(rcell_t), RT[i].ncells, f) == RT[i].ncells;
            if (ok) { c = crc32(&RT[i], ROUTE_HEAD, c); c = crc32(RT[i].c, sizeof(rcell_t) * RT[i].ncells, c); }
        }
        for (uint32_t i = 0; ok && i < h.ncells; i++) {
            cell_t e;
            ok = fread(&e, sizeof(e), 1, f) == 1;
            if (!ok) break;
            c = crc32(&e, sizeof(e), c);
            cell_t *d = cell_add(e.cx, e.cy);
            if (d) *d = e;
        }
        ok = ok && c == h.crc;
    }
    fclose(f);
    if (!ok) { tables_clear(); return false; }
    A.nruns = (int)h.nruns;
    A.nroutes = (int)h.nroutes;
    A.state_seq = A.seq = h.seq;
    A.log_end = (long)h.log_off;
    A.next_no = h.next_no ? h.next_no : 1;
    A.route_names = h.route_names;
    A.total_m = h.total_m;
    A.done_runs = h.done_runs;
    return true;
}

static void dev_hdr_read(void)
{
    A.dev_seq = 0; A.dev_off = LOG_HDR; A.dev_count = 0;
    memset(A.dev_kind, 0, sizeof(A.dev_kind));
    FILE *f = open_either("dev.idx", "dev.tmp");
    if (!f) return;
    static dev_hdr_t h;
    if (fread(&h, sizeof(h), 1, f) == 1 && !memcmp(h.magic, "WDIX", 4) && h.ver == 1 &&
        crc32(&h, offsetof(dev_hdr_t, crc), 0) == h.crc) {
        A.dev_seq = h.seq; A.dev_off = (long)h.log_off; A.dev_count = h.count;
        memcpy(A.dev_kind, h.kind, sizeof(A.dev_kind));
    }
    fclose(f);
}

/* ------------------------------------------------------------ loading */

static bool alloc_all(void)
{
    if (C) return true;
    C = wd_feed_alloc(sizeof(cell_t) * CELL_CAP);
    RU = wd_feed_alloc(sizeof(run_t) * RUN_CAP);
    RT = wd_feed_alloc(sizeof(route_t) * ROUTE_CAP);
    KN = wd_feed_alloc(sizeof(known_t) * KNOWN_CAP);
    RG = wd_feed_alloc(sizeof(reg_t) * REG_CAP);
    AC = wd_feed_alloc(sizeof(acc_t) * ACC_N);
    FL = wd_feed_alloc(sizeof(*FL) * WD_CAP);
    DIRTY = wd_feed_alloc(WD_CAP / 8);
    CHECKED = wd_feed_alloc(WD_CAP / 8);
    BUF_LOAD = wd_feed_alloc(RD_BUF);
    BUF_KNOWN = wd_feed_alloc(RD_BUF);
    BUF_JOB = wd_feed_alloc(RD_BUF);
    if (C && RU && RT && KN && RG && AC && FL && DIRTY && CHECKED && BUF_LOAD && BUF_KNOWN && BUF_JOB) return true;
    void *all[] = { C, RU, RT, KN, RG, AC, FL, DIRTY, CHECKED, BUF_LOAD, BUF_KNOWN, BUF_JOB };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) wd_feed_free(all[i]);
    C = NULL; RU = NULL; RT = NULL; KN = NULL; RG = NULL; AC = NULL; FL = NULL;
    DIRTY = CHECKED = NULL; BUF_LOAD = BUF_KNOWN = BUF_JOB = NULL;
    return false;
}

/* false: the id file is there but the card would not open it just now. A
   new id would make the home base drop this board's next runs as repeats. */
static bool id_load(void)
{
    char p[160];
    apath(p, sizeof(p), "atlas.id");
    errno = 0;
    FILE *f = fopen(p, "r");
    if (!f && errno != ENOENT) return false;
    unsigned v = 0;
    if (f) { if (fscanf(f, "%8x", &v) != 1) v = 0; fclose(f); }
    if (!v) {
        uint32_t h = entropy() * 2654435761u;
        h ^= hash_str(wd_feed_board()) ^ hash_str(A.dir) * 31u;
        h ^= h >> 16; h *= 2246822519u; h ^= h >> 13;
        v = h ? h : 0x5EED;
        f = fopen(p, "w");
        if (f) { fprintf(f, "%08x\n", v); fclose(f); }
    }
    A.id = v;
    return true;
}

static void replay_from(long off, uint32_t after)
{
    lrd_t r;
    static rec_t rec;
    A.log_end = off;
    if (!lrd_open(&r, BUF_LOAD, off)) return;
    long at = off;
    for (int k; (k = lrd_next(&r, &rec, &at)) == 1;) {
        /* the log counts on one by one: a gap means bytes left from a cut */
        if (rec.seq != A.seq + 1 && A.seq) break;
        if (rec.seq > after) fold(&rec, NULL);
        else A.seq = rec.seq;
        A.log_end = r.off;
    }
    lrd_close(&r);
}

/* Runs this board left open (a power cut mid-drive or mid-import) are
   closed where they stopped. */
static void close_open_runs(void)
{
    for (int i = 0; i < A.nruns; i++) {
        run_t *u = &RU[i];
        if (u->origin != A.id || u->end) continue;
        m_run_t m = { u->origin, u->no, u->start, u->start ? u->start : 1, 0, u->dist_m, u->ndev, u->nnew, u->src,
                      (uint8_t)((u->flags & RF_SYNCED) | RF_CRASH) };
        uint8_t p[REC_MAX];
        emit(R_RUN, p, enc_run(p, &m), NULL);
    }
}

static void load(uint32_t now)
{
    A.last_try = now ? now : 1;
    const char *base = wd_feed_dir();
    if (!base || !*base) { A.state = WD_ATLAS_NOCARD; say("no card"); return; }
    if (!alloc_all()) { A.state = WD_ATLAS_NOMEM; say("not enough memory for the atlas"); return; }
    snprintf(A.dir, sizeof(A.dir), "%s/atlas", base);
#ifdef ESP_PLATFORM
    mkdir(A.dir, 0775);
#else
    _mkdir(A.dir);
#endif
    A.state = WD_ATLAS_LOADING;
    if (!id_load()) { A.state = WD_ATLAS_NOCARD; say("the card would not open the atlas"); return; }
    tables_clear();
    A.seq = 0;
    bool have_state = state_read();
    dev_hdr_read();
    char p[160];
    apath(p, sizeof(p), "log.bin");
    errno = 0;
    FILE *f = fopen(p, "rb");
    /* Only a log that is not there, or one that opened and is not ours, makes
       a fresh atlas. A card that will not open or read it right now (a bad
       moment, no free file handle) wipes nothing: try again later. */
    if (!f && errno != ENOENT) { A.state = WD_ATLAS_NOCARD; say("the card would not open the atlas"); return; }
    uint8_t hb[LOG_HDR] = { 0 };
    const size_t got = f ? fread(hb, 1, LOG_HDR, f) : 0;
    if (f && got < LOG_HDR && ferror(f)) { fclose(f); A.state = WD_ATLAS_NOCARD; say("the card would not read the atlas"); return; }
    const bool log_ok = f && got == LOG_HDR && !memcmp(hb, "WDAL", 4) && hb[4] == 1;
    long size = 0;
    if (f && !fseek(f, 0, SEEK_END)) size = ftell(f);
    if (f) fclose(f);
    /* a snapshot or an index past the end of the log is not this log's */
    if (log_ok && have_state && A.log_end > size) { tables_clear(); have_state = false; }
    if (log_ok && A.dev_off > size) { A.dev_seq = 0; A.dev_off = LOG_HDR; A.dev_count = 0; }
    if (!log_ok) {
        /* no log (or not ours): a fresh atlas */
        tables_clear();
        A.seq = 0;
        A.dev_seq = 0; A.dev_off = LOG_HDR; A.dev_count = 0;
        memset(A.dev_kind, 0, sizeof(A.dev_kind));
        remove(p);
        char q[160];
        apath(q, sizeof(q), "dev.idx"); remove(q);
        apath(q, sizeof(q), "state.bin"); remove(q);
        if (!log_create()) { A.state = WD_ATLAS_NOCARD; say("the card would not take the atlas"); return; }
    } else {
        A.tail_dev = 0;
        const uint32_t s0 = A.seq;
        if (have_state) replay_from(A.log_end, A.seq);
        else { A.seq = 0; replay_from(LOG_HDR, 0); }
        /* the device index's tail is counted from its own place */
        if (A.dev_seq > A.seq) { A.dev_seq = 0; A.dev_off = LOG_HDR; A.dev_count = 0; }
        if (have_state && A.dev_seq < s0) {
            /* DEV records between the device index and the snapshot */
            lrd_t r;
            static rec_t rec;
            if (lrd_open(&r, BUF_LOAD, A.dev_off)) {
                for (long at; lrd_next(&r, &rec, &at) == 1 && rec.seq <= s0;)
                    if (rec.type == R_DEV && rec.seq > A.dev_seq) A.tail_dev++;
                lrd_close(&r);
            }
        }
        if (A.seq != s0 || !have_state) A.state_dirty = true;
    }
    wd_lock();
    A.state = WD_ATLAS_READY;
    wd_unlock();
    close_open_runs();
    log_close();
    if (A.state_dirty) state_write();
    if (A.tail_dev) A.req |= WD_ATLAS_REQ_COMPACT;
    say(A.done_runs ? "atlas ready" : "a new atlas: drive, or IMPORT old drives");
}

/* ------------------------------------------------------------ the session hooks */

static uint32_t utc_of(const wd_stats_t *s, uint32_t ms)
{
    return s->utc_base > 0 ? (uint32_t)(s->utc_base + (long)(ms / 1000)) : 0;
}

void wd_atlas_session(bool begin, bool replay, uint32_t now)
{
    if (begin && !replay) {
        L.on = true;
        L.evt_begin++;
        L.start_ms = now;
        L.ndev = L.nnew = L.nreg = L.nmoved = L.nmissing = 0;
        memset(L.gain_src, 0, sizeof(L.gain_src));
        L.new_n = L.new_head = L.mov_n = L.mov_head = 0;
        L.route_guess = -1;
        if (DIRTY) { memset(DIRTY, 0, WD_CAP / 8); memset(CHECKED, 0, WD_CAP / 8); memset(FL, 0, sizeof(*FL) * WD_CAP); }
        if (K.ready) {
            for (int i = 0; i < KNOWN_CAP; i++) KN[i].flags &= (uint8_t)~(K_SEEN | K_MOVED);
            for (int i = 0; i < K.nreg; i++) RG[i].flags &= (uint8_t)~(K_SEEN | K_MOVED);
        }
    } else if (!begin) {
        if (!L.on) return;
        L.on = false;
        L.fin.gen = L.evt_begin;
        L.fin.end_ms = now;
        L.fin.ndev = L.ndev;
        L.fin.nnew = L.nnew;
        snprintf(L.fin.stamp, sizeof(L.fin.stamp), "%s", wd_stats()->stamp);
        L.evt_end++;
    } else {
        L.on = false;               /* a replay is not a drive */
    }
}

static void ring_push(wd_atlas_item_t *ring, int cap, int *head, int *n, const wd_dev_t *e, uint16_t drives, uint16_t dist)
{
    wd_atlas_item_t *it = &ring[*head];
    memset(it, 0, sizeof(*it));
    it->kind = e->kind;
    it->rssi = e->rssi;
    it->drives = drives;
    it->dist_m = dist;
    memcpy(it->addr, e->addr, 6);
    snprintf(it->name, sizeof(it->name), "%.21s", e->name);
    *head = (*head + 1) % cap;
    if (*n < cap) (*n)++;
}

static int cell_prior(int32_t cx, int32_t cy)
{
    const cell_t *c = cell_find(cx, cy);
    if (!c) return 0;
    return c->visits - (L.have_run && c->lastrun == (uint16_t)L.runh ? 1 : 0);
}

/* A known device heard on this run, and its regular's entry. */
static void seen_mark(known_t *k)
{
    k->flags |= K_SEEN;
    if (k->flags & K_REG)
        for (int i = 0; i < K.nreg; i++)
            if (RG[i].kind == k->kind && !memcmp(RG[i].addr, k->addr, 6)) { RG[i].flags |= K_SEEN; break; }
}

/* REGULAR, NEWHERE and MOVED for one device (wd_lock held). */
static void tag(int idx, wd_dev_t *e)
{
    known_t *k = known_find(e->kind, e->addr);
    if (k) {
        if (k->drives >= REG_DRIVES) e->tags |= WD_T_REGULAR;
        if (!(k->flags & K_SEEN)) {
            if (k->drives >= REG_DRIVES) L.nreg++;
            seen_mark(k);
        }
    } else if (L.have_cell && in_area(to_e7((L.ccy + 0.5) / CELL_DEG), to_e7((L.ccx + 0.5) / CELL_DEG)) &&
               (cell_prior(L.ccx, L.ccy) > 0 || L.route_guess >= 0)) {
        /* never heard here before, where the set covers and the road was driven */
        e->tags |= WD_T_NEWHERE;
        L.nnew++;
        ring_push(L.newr, 32, &L.new_head, &L.new_n, e, 0, 0);
    }
    bit_set(CHECKED, idx);
}

void wd_atlas_note(int idx, wd_dev_t *e, bool added, uint32_t now)
{
    (void)now;
    if (!L.on || A.state != WD_ATLAS_READY || idx < 0 || idx >= WD_CAP || !e) return;
    bit_set(DIRTY, idx);
    if (added) { L.ndev++; bit_clr(CHECKED, idx); }
    if (!K.ready) return;
    if (!bit_get(CHECKED, idx)) { tag(idx, e); return; }
    /* heard stronger somewhere else than the atlas has it: moved */
    if (!(e->flags & WD_F_PLACED) || e->rssi < -75 || e->rssi != e->last_rssi) return;
    known_t *k = known_find(e->kind, e->addr);
    if (!k || (k->flags & K_MOVED) || k->drives < REG_DRIVES) return;
    int16_t px, py;
    known_pos(to_e7(e->lat), to_e7(e->lon), &px, &py);
    const float d = hypotf((float)(px - k->px), (float)(py - k->py)) * 10.0f;
    if (d < MOVED_M) return;
    k->flags |= K_MOVED;
    L.nmoved++;
    ring_push(L.movr, 16, &L.mov_head, &L.mov_n, e, k->drives, (uint16_t)(d > 65000 ? 65000 : d));
}

/* ------------------------------------------------------------ the live run */

static bool ensure_run(const wd_stats_t *snap, uint32_t now)
{
    if (L.have_run) return true;
    wd_lock();
    L.origin = A.id;
    L.no = A.next_no;
    L.runh = runh(L.origin, L.no);
    L.have_run = true;
    acc_t *a = &AC[0];
    a->runh = L.runh; a->n = 0;
    memset(a->h, 0, sizeof(a->h));
    wd_unlock();
    m_run_t m = { L.origin, L.no, utc_of(snap, L.start_ms ? L.start_ms : now), 0, 0, 0, 0, 0, 0, 0 };
    uint8_t p[REC_MAX];
    return emit(R_RUN, p, enc_run(p, &m), NULL);
}

typedef struct {
    uint8_t kind, addr[6], radio, sub, placed, named, nlen;
    int8_t rssi;
    uint16_t tags;
    uint32_t dseen, first_ms, last_ms;
    double lat, lon;
    char name[33];
} flush_item_t;
static EXT_RAM_BSS_ATTR flush_item_t s_fi[64];

/* The devices heard since the last flush, as DEV records. */
static void flush_devs(const wd_stats_t *snap, uint32_t now, unsigned gen, bool all)
{
    for (int pass = 0; pass < (all ? 128 : 16); pass++) {
        int n = 0;
        wd_lock();
        if (L.evt_begin != gen) { wd_unlock(); return; }   /* a new session holds the table now */
        const int nd = wd_dev_count();
        for (int w = 0; w < WD_CAP / 32 && n < 64; w++) {
            if (!DIRTY[w]) continue;
            for (int b = 0; b < 32 && n < 64; b++) {
                const int i = w * 32 + b;
                if (!bit_get(DIRTY, i)) continue;
                bit_clr(DIRTY, i);
                const wd_dev_t *e = i < nd ? wd_dev_at(i) : NULL;
                if (!e) continue;
                if (FL[i].kind != e->kind || memcmp(FL[i].addr, e->addr, 6)) {
                    FL[i].kind = e->kind; memcpy(FL[i].addr, e->addr, 6);
                    FL[i].seen = 0; FL[i].named = 0;
                }
                flush_item_t *it = &s_fi[n];
                it->dseen = e->seen - FL[i].seen;
                const bool name_due = e->name[0] && !FL[i].named;
                if (!it->dseen && !name_due) continue;
                it->kind = e->kind; memcpy(it->addr, e->addr, 6);
                it->radio = e->radio; it->sub = e->sub;
                it->placed = (e->flags & WD_F_PLACED) != 0;
                it->rssi = e->rssi;
                it->tags = (uint16_t)(e->tags & ~DYN_TAGS);
                it->first_ms = e->first_ms; it->last_ms = e->last_ms;
                it->lat = e->lat; it->lon = e->lon;
                it->named = name_due;
                snprintf(it->name, sizeof(it->name), "%s", e->name);
                it->nlen = (uint8_t)strlen(it->name);
                FL[i].seen = e->seen;
                if (e->name[0]) FL[i].named = 1;
                n++;
            }
        }
        wd_unlock();
        if (!n) return;
        if (!ensure_run(snap, now)) return;
        for (int j = 0; j < n; j++) {
            const flush_item_t *it = &s_fi[j];
            m_dev_t m;
            memset(&m, 0, sizeof(m));
            m.origin = L.origin; m.no = L.no;
            wd_lock();
            const run_t *u = run_find(L.origin, L.no);
            m.ri = u ? u->rec_next : 0;
            wd_unlock();
            m.kind = it->kind; memcpy(m.addr, it->addr, 6);
            m.radios = (uint8_t)(1u << (it->radio & 7)); m.sub = it->sub; m.tags = it->tags; m.rssi = it->rssi;
            m.lat = it->placed ? to_e7(it->lat) : NOPOS;
            m.lon = it->placed ? to_e7(it->lon) : NOPOS;
            m.first = utc_of(snap, it->first_ms); m.last = utc_of(snap, it->last_ms);
            m.seen = (uint16_t)(it->dseen > 0xFFFF ? 0xFFFF : it->dseen);
            if (it->named) { m.nlen = it->nlen > 32 ? 32 : it->nlen; memcpy(m.name, it->name, m.nlen); }
            uint8_t p[REC_MAX];
            if (!emit(R_DEV, p, enc_dev(p, &m), NULL)) return;
        }
    }
}

static uint8_t src_mask(const wd_stats_t *s)
{
    uint8_t m = 0;
    for (int k = 0; k < WD_KINDS; k++) if (s->src[k]) m |= (uint8_t)(1u << wd_atlas_src(k));
    return m;
}

static void finish_run(const wd_stats_t *snap, uint32_t now)
{
    if (L.fin.gen == L.evt_begin) flush_devs(snap, now, L.fin.gen, true);
    if (!L.have_run) return;
    m_run_t m = { L.origin, L.no, utc_of(snap, L.start_ms), utc_of(snap, L.fin.end_ms), L.fin.stamp[0] ? hash_str(L.fin.stamp) : 0,
                  (uint32_t)L.dist, (uint16_t)(L.fin.ndev > 0xFFFF ? 0xFFFF : L.fin.ndev),
                  (uint16_t)(L.fin.nnew > 0xFFFF ? 0xFFFF : L.fin.nnew), L.src_union, 0 };
    if (!m.end) m.end = 1;          /* no clock: still ended */
    uint8_t p[REC_MAX];
    emit(R_RUN, p, enc_run(p, &m), NULL);
    log_close();
    wd_lock();
    L.have_run = false;
    acc_free(&AC[0]);
    wd_unlock();
    state_write();
    A.req |= WD_ATLAS_REQ_COMPACT;
}

/* Missing regulars and the route guess, every few seconds (wd_lock held). */
static void think(void)
{
    acc_t *a = L.have_run ? &AC[0] : NULL;
    L.route_guess = a ? route_match(a, true) : -1;
    /* regulars (three drives or more) this run passed and did not hear */
    unsigned miss = 0;
    for (int i = 0; K.ready && a && i < KNOWN_CAP; i++) {
        const known_t *k = &KN[i];
        if (k->drives < 3 || (k->flags & K_SEEN)) continue;
        int32_t cx, cy;
        cell_of(K.lat0 + k->py * 10.0 / K.m_lat, K.lon0 + k->px * 10.0 / K.m_lon, &cx, &cy);
        if (acc_near(a, cx, cy)) miss++;
    }
    for (int i = 0; i < K.nreg; i++) {
        reg_t *g = &RG[i];
        g->flags &= (uint8_t)~K_MOVED;  /* K_MOVED on a kept regular: missing (by name) */
        if ((g->flags & K_SEEN) || !a) continue;
        int32_t cx, cy;
        cell_of(K.lat0 + g->py * 10.0 / K.m_lat, K.lon0 + g->px * 10.0 / K.m_lon, &cx, &cy);
        if (acc_near(a, cx, cy)) g->flags |= K_MOVED;
    }
    L.nmissing = miss;
}

static void live_step(uint32_t now)
{
    static EXT_RAM_BSS_ATTR wd_stats_t snap;
    wd_lock();
    const wd_stats_t *s = wd_stats();
    snap.utc_base = s->utc_base;
    memcpy(snap.src, s->src, sizeof(snap.src));
    const bool fresh = s->fix.fix && s->fix_ms && now - s->fix_ms <= 5000;
    const double lat = s->fix.lat, lon = s->fix.lon;
    const unsigned eb = L.evt_begin, ee = L.evt_end;
    const bool on = L.on;
    wd_unlock();
    if (L.done_end != ee) {
        L.done_end = ee;
        finish_run(&snap, now);
    }
    if (L.done_begin != eb) {
        L.done_begin = eb;
        wd_lock();
        L.have_run = false;
        L.have_cell = false;
        L.have_p = false;
        L.dist = 0;
        L.src_union = 0;
        L.flush_ms = now;
        L.think_ms = now;
        wd_unlock();
    }
    if (!on) return;
    if (fresh) {
        if (L.have_p) {
            const float dy = (float)((lat - L.plat) * 111195.0), dx = (float)((lon - L.plon) * 111195.0 * cos(lat * M_PI / 180));
            const float d = hypotf(dx, dy);
            if (d > 5 && d < 1000) { L.dist += d; L.plat = lat; L.plon = lon; }
            else if (d >= 1000) { L.plat = lat; L.plon = lon; }
        } else { L.have_p = true; L.plat = lat; L.plon = lon; }
        int32_t cx, cy;
        cell_of(lat, lon, &cx, &cy);
        if (!L.have_cell || cx != L.ccx || cy != L.ccy) {
            if (ensure_run(&snap, now)) {
                const uint8_t mask = src_mask(&snap);
                static m_cells_t m;
                m.origin = L.origin; m.no = L.no;
                wd_lock();
                const run_t *u = run_find(L.origin, L.no);
                m.ri = u ? u->rec_next : 0;
                wd_unlock();
                m.n = 1; m.cx[0] = cx; m.cy[0] = cy; m.mask[0] = mask;
                uint8_t p[REC_MAX];
                emit(R_CELLS, p, enc_cells(p, &m), NULL);
                wd_lock();
                L.ccx = cx; L.ccy = cy; L.have_cell = true;
                L.src_union |= mask;
                wd_unlock();
            }
        }
    }
    /* Early only when nearly every slot holds something not yet written: the
       table evicts the least recently heard, so a slot heard since the last
       flush goes only once no older one is left. A merely full table (every
       city drive) waits for the minute like any other. */
    wd_lock();
    int dirty = 0;
    for (int w = 0; w < WD_CAP / 32; w++) dirty += __builtin_popcount(DIRTY[w]);
    wd_unlock();
    if (now - L.flush_ms >= FLUSH_MS || dirty >= WD_CAP - 64) {
        L.flush_ms = now;
        flush_devs(&snap, now, eb, false);
        log_close();
    }
    if (now - L.think_ms >= 5000) {
        L.think_ms = now;
        wd_lock();
        think();
        wd_unlock();
    }
}

/* ------------------------------------------------------------ the known set */

static void known_finish(void)
{
    if (K.idx) fclose(K.idx);
    K.idx = NULL;
    lrd_close(&K.rd);
    wd_lock();
    K.ready = true;
    K.state = KS_IDLE;
    /* devices heard while the set was read get their tags; the ones this
       run already knew keep "heard" and "moved" in the new area's set */
    const int nd = L.on ? wd_dev_count() : 0;
    for (int i = 0; i < nd; i++) {
        wd_dev_t *e = (wd_dev_t *)wd_dev_at(i);
        if (!e) continue;
        if (!bit_get(CHECKED, i)) { tag(i, e); continue; }
        known_t *k = known_find(e->kind, e->addr);
        if (k) seen_mark(k);
    }
    for (int j = 0; j < L.mov_n; j++) {
        known_t *k = known_find(L.movr[j].kind, L.movr[j].addr);
        if (k) k->flags |= K_MOVED;
    }
    wd_unlock();
}

static void reg_offer(const adev_t *d)
{
    if (d->drives < 3) return;
    int at = -1;
    if (K.nreg < REG_CAP) at = K.nreg++;
    else {
        for (int i = 0; i < REG_CAP; i++) if (at < 0 || RG[i].drives < RG[at].drives) at = i;
        if (RG[at].drives >= d->drives) return;
        known_t *o = known_find(RG[at].kind, RG[at].addr);
        if (o) o->flags &= (uint8_t)~K_REG;
    }
    reg_t *g = &RG[at];
    memset(g, 0, sizeof(*g));
    g->kind = d->kind;
    memcpy(g->addr, d->addr, 6);
    g->drives = d->drives;
    known_pos(d->lat, d->lon, &g->px, &g->py);
    const int nl = d->nlen < 21 ? d->nlen : 21;
    memcpy(g->name, d->name, (size_t)nl);
    g->name[nl] = 0;
    known_t *k = known_find(d->kind, d->addr);
    if (k) k->flags |= K_REG;
}

static void known_step(uint32_t now)
{
    (void)now;
    if (!L.on) {
        if (K.state != KS_IDLE) { if (K.idx) fclose(K.idx); K.idx = NULL; lrd_close(&K.rd); K.state = KS_IDLE; }
        return;
    }
    if (K.state == KS_IDLE) {
        wd_lock();
        const wd_stats_t *s = wd_stats();
        /* only a trusted GPS position moves the known area: a weak one wanders,
           and the Wi-Fi fix needs the set that is loaded */
        const bool fresh = s->fix.fix && s->fix_ms && !s->fix_weak && !s->wifi_fix;
        const double lat = s->fix.lat, lon = s->fix.lon;
        wd_unlock();
        if (!fresh) return;
        bool want = !K.ready || A.known_stale;
        if (K.ready && !want) {
            const float dy = (float)((lat - K.lat0) * K.m_lat), dx = (float)((lon - K.lon0) * K.m_lon);
            want = dx * dx + dy * dy > AREA_MOVE_M * AREA_MOVE_M;
        }
        /* a compaction paused by the drive only reads dev.idx; it swaps the
           new one in after the drive, when this scan has closed it */
        if (!want) return;
        wd_lock();
        K.ready = false;
        wd_unlock();
        A.known_stale = false;
        memset(KN, 0, sizeof(known_t) * KNOWN_CAP);
        K.n = 0;
        K.nreg = 0;
        K.lat0 = lat; K.lon0 = lon;
        K.m_lat = 111195.0f;
        K.m_lon = (float)(111195.0 * cos(lat * M_PI / 180));
        char p[160];
        apath(p, sizeof(p), "dev.idx");
        K.idx = fopen(p, "rb");
        static dev_hdr_t h;
        K.idx_left = 0;
        if (K.idx && fread(&h, sizeof(h), 1, K.idx) == 1 && !memcmp(h.magic, "WDIX", 4) && h.seq == A.dev_seq) K.idx_left = h.count;
        K.state = KS_IDX;
        return;
    }
    if (K.state == KS_IDX) {
        static EXT_RAM_BSS_ATTR adev_t d[64];
        for (int round = 0; round < 16 && K.idx_left; round++) {
            const size_t want = K.idx_left < 64 ? K.idx_left : 64;
            const size_t got = K.idx ? fread(d, sizeof(adev_t), want, K.idx) : 0;
            K.idx_left = got == want ? K.idx_left - (uint32_t)got : 0;
            for (size_t i = 0; i < got; i++) {
                if (!in_area(d[i].lat, d[i].lon)) continue;
                known_t *k = known_add(d[i].kind, d[i].addr);
                if (!k) continue;
                k->drives = d[i].drives ? d[i].drives : 1;
                k->runh = (uint16_t)d[i].last_run;
                known_pos(d[i].lat, d[i].lon, &k->px, &k->py);
                reg_offer(&d[i]);
            }
        }
        if (K.idx_left) return;
        if (K.idx) fclose(K.idx);
        K.idx = NULL;
        log_close();
        if (!lrd_open(&K.rd, BUF_KNOWN, A.dev_off)) { known_finish(); return; }
        K.state = KS_LOG;
        return;
    }
    /* KS_LOG: the device records the index has not folded yet, but not this run's */
    static rec_t rec;
    static m_dev_t m;
    for (int i = 0; i < 2000; i++) {
        long at;
        if (lrd_next(&K.rd, &rec, &at) != 1) { known_finish(); return; }
        if (rec.type != R_DEV || rec.seq <= A.dev_seq || !dec_dev(&rec, &m)) continue;
        if (L.have_run && m.origin == L.origin && m.no == L.no) continue;
        if (!in_area(m.lat, m.lon)) continue;
        const uint16_t h = (uint16_t)runh(m.origin, m.no);
        known_t *k = known_find(m.kind, m.addr);
        if (!k) {
            k = known_add(m.kind, m.addr);
            if (!k) continue;
            k->drives = 1; k->runh = h;
            known_pos(m.lat, m.lon, &k->px, &k->py);
        } else if (k->runh != h) { k->drives++; k->runh = h; }
    }
}

/* ------------------------------------------------------------ the pending device table */

static bool pend_alloc(void)
{
    if (!PD) PD = wd_feed_alloc(sizeof(pend_t) * PEND_CAP);
    if (!PH) PH = wd_feed_alloc(sizeof(uint16_t) * PEND_HASH);
    if (!PD || !PH) { wd_feed_free(PD); wd_feed_free(PH); PD = NULL; PH = NULL; return false; }
    memset(PH, 0, sizeof(uint16_t) * PEND_HASH);
    npend = 0;
    return true;
}
static void pend_free(void) { wd_feed_free(PD); wd_feed_free(PH); PD = NULL; PH = NULL; npend = 0; }
static pend_t *pend_get(uint8_t kind, const uint8_t addr[6], bool *full)
{
    uint32_t i = key_hash(kind, addr) & (PEND_HASH - 1);
    for (uint32_t k = 0; k < PEND_HASH; k++, i = (i + 1) & (PEND_HASH - 1)) {
        if (!PH[i]) break;
        pend_t *p = &PD[PH[i] - 1];
        if (p->d.kind == kind && !memcmp(p->d.addr, addr, 6)) return p;
    }
    if (npend >= PEND_CAP) { if (full) *full = true; return NULL; }
    pend_t *p = &PD[npend];
    memset(p, 0, sizeof(*p));
    p->d.kind = kind;
    memcpy(p->d.addr, addr, 6);
    p->d.lat = p->d.lon = p->d.alat = p->d.alon = NOPOS;
    p->d.rssi = -128;
    PH[i] = (uint16_t)(++npend);
    return p;
}
/* The stronger of two places; a tie goes to the smaller latitude, then
   longitude, so boards that merge in different orders agree. */
static bool stronger(int8_t r, int32_t lat, int32_t lon, int8_t r0, int32_t lat0, int32_t lon0)
{
    if (lat0 == NOPOS) return true;
    if (r != r0) return r > r0;
    return lat < lat0 || (lat == lat0 && lon < lon0);
}
/* The name heard last; a tie goes to the larger bytes. */
static bool name_wins(uint32_t t, const char *n, int nl, uint32_t t0, const char *n0, int nl0)
{
    if (!nl0) return true;
    if (t != t0) return t > t0;
    const int c = memcmp(n, n0, (size_t)(nl < nl0 ? nl : nl0));
    return c > 0 || (c == 0 && nl > nl0);
}
static float weight(int rssi, uint32_t seen)
{
    const float s = (float)(rssi + 110 < 1 ? 1 : rssi + 110);
    return s * s * 0.01f * (float)(seen ? seen : 1);
}
/* One DEV record into a device's running totals. */
static void dev_fold(adev_t *d, uint32_t *first_run, const m_dev_t *m)
{
    const uint32_t h = runh(m->origin, m->no);
    if (!d->drives) { d->drives = 1; d->last_run = h; if (first_run) *first_run = h; }
    else if (d->last_run != h) { d->drives++; d->last_run = h; }
    d->first = min_nz(d->first, m->first);
    if (m->last > d->last) d->last = m->last;
    d->seen += m->seen;
    d->tags |= m->tags;
    d->radios |= m->radios;
    if (m->sub) d->sub = m->sub;
    if (m->lat != NOPOS) {
        if (stronger(m->rssi, m->lat, m->lon, d->rssi, d->lat, d->lon)) { d->lat = m->lat; d->lon = m->lon; d->rssi = m->rssi; }
        const float w = weight(m->rssi, m->seen), t = d->w + w;
        if (d->alat == NOPOS || d->w <= 0) { d->alat = m->lat; d->alon = m->lon; }
        else {
            d->alat = (int32_t)llround(((double)d->alat * d->w + (double)m->lat * w) / t);
            d->alon = (int32_t)llround(((double)d->alon * d->w + (double)m->lon * w) / t);
        }
        d->w = t;
    } else if (m->rssi > d->rssi && d->lat == NOPOS) d->rssi = m->rssi;
    if (m->nlen && name_wins(m->last, m->name, m->nlen, d->name_utc, d->name, d->nlen)) {
        memcpy(d->name, m->name, m->nlen);
        if (m->nlen < 32) memset(d->name + m->nlen, 0, (size_t)(32 - m->nlen));
        d->nlen = m->nlen;
        d->name_utc = m->last;
    }
}
/* A device's pending totals onto its index entry. */
static void dev_merge(adev_t *o, const pend_t *p)
{
    const adev_t *n = &p->d;
    o->drives = (uint16_t)(o->drives + n->drives - (p->first_run == o->last_run ? 1 : 0));
    o->last_run = n->last_run;
    o->first = min_nz(o->first, n->first);
    if (n->last > o->last) o->last = n->last;
    o->seen += n->seen;
    o->tags |= n->tags;
    o->radios |= n->radios;
    if (n->sub) o->sub = n->sub;
    if (n->lat != NOPOS && stronger(n->rssi, n->lat, n->lon, o->rssi, o->lat, o->lon)) { o->lat = n->lat; o->lon = n->lon; o->rssi = n->rssi; }
    if (n->alat != NOPOS && n->w > 0) {
        if (o->alat == NOPOS || o->w <= 0) { o->alat = n->alat; o->alon = n->alon; o->w = n->w; }
        else {
            const float t = o->w + n->w;
            o->alat = (int32_t)llround(((double)o->alat * o->w + (double)n->alat * n->w) / t);
            o->alon = (int32_t)llround(((double)o->alon * o->w + (double)n->alon * n->w) / t);
            o->w = t;
        }
    }
    if (n->nlen && name_wins(n->name_utc, n->name, n->nlen, o->name_utc, o->name, o->nlen)) { memcpy(o->name, n->name, 32); o->nlen = n->nlen; o->name_utc = n->name_utc; }
}
static int pend_cmp(const void *a, const void *b)
{
    const adev_t *x = &((const pend_t *)a)->d, *y = &((const pend_t *)b)->d;
    return key_cmp(x->kind, x->addr, y->kind, y->addr);
}

/* ------------------------------------------------------------ compaction */

enum { C_GATHER, C_MERGE };
static struct {
    int stage;
    lrd_t rd;
    long g_off;
    uint32_t g_seq;
    bool g_eof;
    FILE *in, *out;
    uint32_t in_left, out_n, kind[8];
    int pi;
    adev_t cur;
    bool have_cur;
} CP;

static void compact_abort(void)
{
    lrd_close(&CP.rd);
    if (CP.in) fclose(CP.in);
    if (CP.out) fclose(CP.out);
    CP.in = CP.out = NULL;
    pend_free();
    A.job = WD_ATLAS_JOB_NONE;
}

static bool compact_start(void)
{
    if (!pend_alloc()) { say("not enough memory to compact"); return false; }
    memset(&CP, 0, sizeof(CP));
    log_close();
    CP.g_off = A.dev_off;
    CP.g_seq = A.dev_seq;
    if (!lrd_open(&CP.rd, BUF_JOB, A.dev_off)) { pend_free(); return false; }
    CP.stage = C_GATHER;
    A.job = WD_ATLAS_JOB_COMPACT;
    A.job_pct = 0;
    say("compacting the device index");
    return true;
}

static uint32_t utc_now(void)
{
    const wd_stats_t *s = wd_stats();
    if (s->utc_base > 0) {
#ifdef ESP_PLATFORM
        return (uint32_t)(s->utc_base + (long)(esp_timer_get_time() / 1000000));
#else
        return (uint32_t)time(NULL);
#endif
    }
    return 0;
}

static void out_put(const adev_t *d, uint32_t now_utc)
{
    if (now_utc && d->drives <= 1 && d->seen <= 2 && d->last && now_utc > d->last + PRUNE_S) return;
    if (fwrite(d, sizeof(*d), 1, CP.out) == 1) {
        CP.out_n++;
        if (d->kind < 8) CP.kind[d->kind]++;
    }
}

static void compact_step(void)
{
    if (CP.stage == C_GATHER) {
        static rec_t rec;
        static m_dev_t m;
        for (int i = 0; i < 3000; i++) {
            long at;
            const int k = lrd_next(&CP.rd, &rec, &at);
            if (k != 1) { CP.g_eof = true; break; }
            if (rec.type == R_DEV && rec.seq > A.dev_seq && dec_dev(&rec, &m)) {
                bool full = false;
                pend_t *p = pend_get(m.kind, m.addr, &full);
                if (!p) break;                              /* full: this record and the rest next pass */
                dev_fold(&p->d, &p->first_run, &m);
            }
            CP.g_off = CP.rd.off;
            CP.g_seq = rec.seq;
        }
        const long span = A.log_end - A.dev_off;
        A.job_pct = span > 0 ? (int)(40 * (int64_t)(CP.g_off - A.dev_off) / span) : 40;
        if (!CP.g_eof && npend < PEND_CAP) return;
        lrd_close(&CP.rd);
        if (!npend) {
            /* nothing for the index: just say how far it reaches */
            A.dev_seq = CP.g_seq; A.dev_off = CP.g_off;
            if (CP.g_eof) { A.tail_dev = 0; compact_abort(); say("atlas compacted"); }
            return;
        }
        qsort(PD, (size_t)npend, sizeof(pend_t), pend_cmp);
        char p[160];
        apath(p, sizeof(p), "dev.idx");
        CP.in = fopen(p, "rb");
        static dev_hdr_t h;
        CP.in_left = 0;
        /* only the index this pass starts from: one left from before a rebuild would count twice */
        if (CP.in && fread(&h, sizeof(h), 1, CP.in) == 1 && !memcmp(h.magic, "WDIX", 4) && h.seq == A.dev_seq &&
            (long)h.log_off == A.dev_off)
            CP.in_left = h.count;
        apath(p, sizeof(p), "dev.tmp");
        CP.out = fopen(p, "wb");
        if (!CP.out) { compact_abort(); say("compact: the card would not take the index"); return; }
        static const dev_hdr_t blank;
        fwrite(&blank, sizeof(blank), 1, CP.out);
        CP.out_n = 0;
        memset(CP.kind, 0, sizeof(CP.kind));
        CP.pi = 0;
        CP.have_cur = false;
        CP.stage = C_MERGE;
        return;
    }
    /* C_MERGE: the old index and the sorted pending devices, in key order */
    const uint32_t now_utc = utc_now();
    for (int i = 0; i < 2048; i++) {
        if (!CP.have_cur && CP.in_left) {
            if (fread(&CP.cur, sizeof(CP.cur), 1, CP.in) == 1) { CP.have_cur = true; CP.in_left--; }
            else CP.in_left = 0;
        }
        const pend_t *p = CP.pi < npend ? &PD[CP.pi] : NULL;
        if (!CP.have_cur && !p) break;
        const int c = !CP.have_cur ? 1 : !p ? -1 : key_cmp(CP.cur.kind, CP.cur.addr, p->d.kind, p->d.addr);
        if (c < 0) { out_put(&CP.cur, now_utc); CP.have_cur = false; }
        else if (c > 0) { out_put(&p->d, now_utc); CP.pi++; }
        else { dev_merge(&CP.cur, p); out_put(&CP.cur, now_utc); CP.have_cur = false; CP.pi++; }
    }
    A.job_pct = 40 + (npend ? 60 * CP.pi / npend : 60);
    if (CP.have_cur || CP.in_left || CP.pi < npend) return;
    static dev_hdr_t h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "WDIX", 4);
    h.ver = 1; h.count = CP.out_n; h.seq = CP.g_seq; h.log_off = (uint32_t)CP.g_off;
    memcpy(h.kind, CP.kind, sizeof(h.kind));
    h.crc = crc32(&h, offsetof(dev_hdr_t, crc), 0);
    bool ok = fseek(CP.out, 0, SEEK_SET) == 0 && fwrite(&h, sizeof(h), 1, CP.out) == 1 && fflush(CP.out) == 0;
#ifdef ESP_PLATFORM
    fsync(fileno(CP.out));
#endif
    fclose(CP.out);
    CP.out = NULL;
    if (CP.in) fclose(CP.in);
    CP.in = NULL;
    ok = ok && swap_in("dev.tmp", "dev.idx");
    if (!ok) { compact_abort(); say("compact: the card would not take the index"); return; }
    wd_lock();
    A.dev_seq = h.seq; A.dev_off = (long)h.log_off; A.dev_count = h.count;
    memcpy(A.dev_kind, h.kind, sizeof(A.dev_kind));
    K.ready = false;                /* the known set is read again from the new index */
    wd_unlock();
    A.known_stale = true;
    if (!CP.g_eof) {
        /* more than one pass holds: again from where this one stopped */
        memset(PH, 0, sizeof(uint16_t) * PEND_HASH);
        npend = 0;
        if (!lrd_open(&CP.rd, BUF_JOB, A.dev_off)) { compact_abort(); return; }
        CP.stage = C_GATHER;
        return;
    }
    A.tail_dev = 0;
    compact_abort();
    say("atlas compacted");
}

/* ------------------------------------------------------------ import */

#define IMP_MAX 128
#define IMP_NAME 28
static struct {
    char (*names)[IMP_NAME];
    int n, at, stage;
    FILE *f;
    long size, read;
    uint32_t origin, runh, first, last;
    uint16_t no;
    bool have_p, track;
    double plat, plon;
    float dist;
    uint8_t src;
    acc_t *acc;
    int done_runs;
    unsigned ndev;
} IM;
enum { I_GPX, I_CSV, I_WRITE };

static bool stem_known(uint32_t h)
{
    for (int i = 0; i < A.nruns; i++)
        if (RU[i].stem == h && RU[i].end && !(RU[i].flags & RF_CRASH)) return true;
    return false;
}

/* The drives on the card the atlas does not hold, oldest first (wd_drives:
   each drive's folder, or its old loose files). names NULL: just count. */
static int import_list(char (*names)[IMP_NAME], int max)
{
    static EXT_RAM_BSS_ATTR char all[96][WD_STEM];
    const int m = wd_drives(all, 96);
    char live[24];
    wd_lock();
    snprintf(live, sizeof(live), "%s", wd_stats()->running ? wd_stats()->stamp : "");
    wd_unlock();
    int n = 0;
    for (int k = m - 1; k >= 0; k--) {
        const char *stem = all[k];
        if ((live[0] && !strcmp(stem, live)) || stem_known(hash_str(stem))) continue;
        if (!names) { n++; continue; }
        if (n >= max) break;
        snprintf(names[n++], IMP_NAME, "%s", stem);
    }
    return n;
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static bool parse_mac(const char *s, uint8_t a[6])
{
    for (int i = 0; i < 6; i++) {
        const int h = hexv(s[i * 3]), l = hexv(s[i * 3 + 1]);
        if (h < 0 || l < 0 || (i < 5 && s[i * 3 + 2] != ':')) return false;
        a[i] = (uint8_t)(h << 4 | l);
    }
    return true;
}
static bool attr(const char *tag, const char *key, double *out)
{
    char k[12];
    snprintf(k, sizeof(k), "%s=\"", key);
    const char *p = strstr(tag, k);
    if (!p) return false;
    *out = strtod(p + strlen(k), NULL);
    return isfinite(*out);
}
static bool inner(const char *s, const char *el, char *out, size_t n)
{
    char a[16], b[16];
    snprintf(a, sizeof(a), "<%s>", el);
    snprintf(b, sizeof(b), "</%s>", el);
    const char *p = strstr(s, a), *q;
    if (!p) return false;
    p += strlen(a);
    if (!(q = strstr(p, b))) return false;
    size_t k = (size_t)(q - p);
    if (k >= n) k = n - 1;
    memcpy(out, p, k);
    out[k] = 0;
    return true;
}

static void imp_point(double lat, double lon, uint32_t t)
{
    if (!isfinite(lat) || !isfinite(lon) || (lat == 0 && lon == 0)) return;
    if (t) { IM.first = min_nz(IM.first, t); if (t > IM.last) IM.last = t; }
    if (IM.have_p) {
        const float dy = (float)((lat - IM.plat) * 111195.0), dx = (float)((lon - IM.plon) * 111195.0 * cos(lat * M_PI / 180));
        const float d = hypotf(dx, dy);
        if (d < 1000) IM.dist += d;
    }
    IM.have_p = true; IM.plat = lat; IM.plon = lon;
    int32_t cx, cy;
    cell_of(lat, lon, &cx, &cy);
    wd_lock();
    /* a peer's run may have borrowed the slot meanwhile */
    if (!IM.acc || IM.acc->runh != IM.runh) IM.acc = acc_get(IM.runh, true);
    if (IM.acc) acc_add(IM.acc, cx, cy, 0);
    wd_unlock();
}

static void imp_seen(uint8_t kind, const uint8_t addr[6], const char *name, int rssi, double lat, double lon, uint32_t t, uint8_t radio);

static bool imp_flush_pend(void)
{
    for (int i = 0; i < npend; i++) {
        const adev_t *d = &PD[i].d;
        m_dev_t m;
        memset(&m, 0, sizeof(m));
        m.origin = IM.origin; m.no = IM.no;
        wd_lock();
        const run_t *u = run_find(IM.origin, IM.no);
        m.ri = u ? u->rec_next : 0;
        wd_unlock();
        m.kind = d->kind; memcpy(m.addr, d->addr, 6);
        m.radios = d->radios; m.sub = d->sub; m.tags = d->tags; m.rssi = d->rssi;
        m.lat = d->lat; m.lon = d->lon; m.first = d->first; m.last = d->last;
        m.seen = (uint16_t)(d->seen > 0xFFFF ? 0xFFFF : d->seen);
        m.nlen = d->nlen; memcpy(m.name, d->name, d->nlen);
        uint8_t p[REC_MAX];
        if (!emit(R_DEV, p, enc_dev(p, &m), NULL)) return false;
    }
    IM.ndev += (unsigned)npend;
    memset(PH, 0, sizeof(uint16_t) * PEND_HASH);
    npend = 0;
    return true;
}

static void imp_seen(uint8_t kind, const uint8_t addr[6], const char *name, int rssi, double lat, double lon, uint32_t t, uint8_t radio)
{
    bool full = false;
    pend_t *p = pend_get(kind, addr, &full);
    if (!p) { imp_flush_pend(); p = pend_get(kind, addr, &full); if (!p) return; }
    m_dev_t m;
    memset(&m, 0, sizeof(m));
    m.origin = IM.origin; m.no = IM.no; m.kind = kind; memcpy(m.addr, addr, 6);
    m.radios = (uint8_t)(1u << radio); m.rssi = (int8_t)(rssi < -127 ? -127 : rssi > 0 ? 0 : rssi);
    const bool pos = isfinite(lat) && isfinite(lon) && !(lat == 0 && lon == 0);
    m.lat = pos ? to_e7(lat) : NOPOS; m.lon = pos ? to_e7(lon) : NOPOS;
    m.first = m.last = t; m.seen = 1;
    snprintf(m.name, sizeof(m.name), "%.32s", name);
    m.nlen = (uint8_t)strlen(m.name);
    dev_fold(&p->d, &p->first_run, &m);
    IM.src |= (uint8_t)(1u << wd_atlas_src(kind));
}

static bool imp_open(const char *ext)
{
    char path[200];
    wd_drive_path(path, sizeof(path), IM.names[IM.at], ext, false);
    IM.f = fopen(path, "r");
    IM.size = IM.read = 0;
    if (!IM.f) return false;
    static char *vbuf;
    if (!vbuf) vbuf = wd_feed_alloc(16384);
    if (vbuf) setvbuf(IM.f, vbuf, _IOFBF, 16384);
    struct stat st;
    if (!stat(path, &st)) IM.size = (long)st.st_size;
    return true;
}

static void imp_begin_stem(void)
{
    IM.origin = A.id;
    wd_lock();
    IM.no = A.next_no++;
    IM.runh = runh(IM.origin, IM.no);
    IM.acc = acc_get(IM.runh, true);
    wd_unlock();
    IM.first = IM.last = 0;
    IM.have_p = IM.track = false;
    IM.dist = 0;
    IM.src = 0;
    IM.ndev = 0;
    memset(PH, 0, sizeof(uint16_t) * PEND_HASH);
    npend = 0;
    m_run_t m = { IM.origin, IM.no, 0, 0, hash_str(IM.names[IM.at]), 0, 0, 0, 0, RF_IMPORT };
    uint8_t p[REC_MAX];
    emit(R_RUN, p, enc_run(p, &m), NULL);
    IM.stage = imp_open("gpx") ? I_GPX : imp_open("csv") ? I_CSV : I_WRITE;
}

static void imp_end_stem(void)
{
    /* cells in driving order, then the devices, then the end */
    acc_t *a = IM.acc && IM.acc->runh == IM.runh ? IM.acc : NULL;
    int i = 0;
    while (a && i < a->n) {
        static m_cells_t m;
        m.origin = IM.origin; m.no = IM.no;
        wd_lock();
        const run_t *u = run_find(IM.origin, IM.no);
        m.ri = u ? u->rec_next : 0;
        wd_unlock();
        m.n = 0;
        while (i < a->n && m.n < CELLS_MAX) {
            const acell_t *c = &a->c[i];
            if (m.n && (abs(c->cx - m.cx[m.n - 1]) > 127 || abs(c->cy - m.cy[m.n - 1]) > 127)) break;
            m.cx[m.n] = c->cx; m.cy[m.n] = c->cy; m.mask[m.n] = IM.src; m.n++;
            i++;
        }
        uint8_t p[REC_MAX];
        if (!emit(R_CELLS, p, enc_cells(p, &m), NULL)) break;
    }
    const unsigned ndev = IM.ndev + (unsigned)npend;
    imp_flush_pend();
    m_run_t m = { IM.origin, IM.no, IM.first, IM.last ? IM.last : (IM.first ? IM.first : 1), hash_str(IM.names[IM.at]),
                  (uint32_t)IM.dist, (uint16_t)(ndev > 0xFFFF ? 0xFFFF : ndev), 0, IM.src, RF_IMPORT };
    uint8_t p[REC_MAX];
    emit(R_RUN, p, enc_run(p, &m), NULL);
    log_close();
    IM.at++;
    IM.done_runs++;
}

static void import_abort(void)
{
    if (IM.f) fclose(IM.f);
    IM.f = NULL;
    wd_feed_free(IM.names);
    IM.names = NULL;
    pend_free();
    A.job = WD_ATLAS_JOB_NONE;
}

static bool import_start(void)
{
    if (!IM.names) IM.names = wd_feed_alloc(sizeof(*IM.names) * IMP_MAX);
    if (!IM.names || !pend_alloc()) { import_abort(); say("not enough memory to import"); return false; }
    IM.n = import_list(IM.names, IMP_MAX);
    IM.at = 0;
    IM.done_runs = 0;
    if (!IM.n) { import_abort(); A.import_left = 0; say("nothing new on the card to import"); return false; }
    A.job = WD_ATLAS_JOB_IMPORT;
    A.job_pct = 0;
    imp_begin_stem();
    return true;
}

static void import_step(void)
{
    static char line[640];
    if (IM.stage == I_GPX || IM.stage == I_CSV) {
        for (int k = 0; k < 2000; k++) {
            if (!fgets(line, sizeof(line), IM.f)) {
                fclose(IM.f);
                IM.f = NULL;
                if (IM.stage == I_GPX && imp_open("csv")) IM.stage = I_CSV;
                else IM.stage = I_WRITE;
                break;
            }
            IM.read += (long)strlen(line);
            if (IM.stage == I_GPX) {
                const bool pt = strstr(line, "<trkpt ") != NULL, wpt = strstr(line, "<wpt ") != NULL;
                double lat, lon;
                if ((!pt && !wpt) || !attr(line, "lat", &lat) || !attr(line, "lon", &lon)) continue;
                char t[40];
                const long u = inner(line, "time", t, sizeof(t)) ? wd_replay_utc(t) : -1;
                if (pt) { IM.track = true; imp_point(lat, lon, u > 0 ? (uint32_t)u : 0); continue; }
                char ty[16] = "", nm[40] = "", desc[80] = "";
                inner(line, "type", ty, sizeof(ty));
                inner(line, "name", nm, sizeof(nm));
                inner(line, "desc", desc, sizeof(desc));
                const uint8_t kind = !strcmp(ty, "drone") ? WD_DRONE : WD_CELL;
                const char *db = strstr(desc, "strongest ");
                uint8_t a[6] = { 0x02, kind, 0, 0, 0, 0 };
                unsigned e, pci;
                if (kind == WD_CELL && sscanf(nm, "LTE %u PCI %u", &e, &pci) == 2) {
                    a[0] = a[1] = 0;
                    a[2] = (uint8_t)(e >> 8); a[3] = (uint8_t)e; a[4] = (uint8_t)(pci >> 8); a[5] = (uint8_t)pci;
                } else {
                    const uint32_t h = hash_str(nm);
                    a[2] = (uint8_t)(h >> 24); a[3] = (uint8_t)(h >> 16); a[4] = (uint8_t)(h >> 8); a[5] = (uint8_t)h;
                }
                imp_seen(kind, a, nm, db ? atoi(db + 10) : -90, lat, lon, u > 0 ? (uint32_t)u : 0, WD_RADIO_C6);
            } else {
                if (!strncmp(line, "WigleWifi", 9) || !strncmp(line, "MAC,", 4)) continue;
                char *fl[12];
                if (wd_replay_csv_fields(line, fl, 12) < 11) continue;
                uint8_t a[6];
                if (!parse_mac(fl[0], a)) continue;
                const long t = wd_replay_utc(fl[3]);
                const double lat = strtod(fl[6], NULL), lon = strtod(fl[7], NULL);
                const uint8_t kind = !strncmp(fl[10], "BLE", 3) || !strncmp(fl[10], "BT", 2) ? WD_BLE : WD_WIFI;
                imp_seen(kind, a, fl[1], atoi(fl[5]), lat, lon, t > 0 ? (uint32_t)t : 0, WD_RADIO_C6);
                if (!IM.track) imp_point(lat, lon, t > 0 ? (uint32_t)t : 0);
            }
        }
        /* the GPX is the first half of a drive, the CSV the second */
        const int within = (IM.size ? (int)(50LL * IM.read / IM.size) : 0) + (IM.stage == I_CSV ? 50 : 0);
        A.job_pct = (IM.at * 100 + (within > 100 ? 100 : within)) / (IM.n ? IM.n : 1);
        if (A.job_pct > 99) A.job_pct = 99;
        return;
    }
    imp_end_stem();
    if (IM.at < IM.n) { imp_begin_stem(); return; }
    char t[56];
    snprintf(t, sizeof(t), "imported %d drive%s", IM.done_runs, IM.done_runs == 1 ? "" : "s");
    import_abort();
    say(t);
    A.import_left = 0;
    state_write();
    A.req |= WD_ATLAS_REQ_COMPACT;
}

/* ------------------------------------------------------------ the worker's pass */

void wd_atlas_step(uint32_t now)
{
    io_lock();
    if (A.state != WD_ATLAS_READY) {
        if (A.state == WD_ATLAS_OFF || A.state == WD_ATLAS_LOADING || now - A.last_try >= 10000 || A.req) load(now);
        if (A.state != WD_ATLAS_READY) { A.req = 0; io_unlock(); return; }
    }
    live_step(now);
    known_step(now);
    /* a peer's records are compacted in once it has been quiet for 30 s */
    if (A.applied) { A.applied = false; A.quiet_from = now ? now : 1; }
    if (A.quiet_from && now - A.quiet_from >= 30000) { A.quiet_from = 0; if (A.tail_dev) A.req |= WD_ATLAS_REQ_COMPACT; }
    if (!L.on && L.done_end == L.evt_end) {
        if (A.job == WD_ATLAS_JOB_COMPACT) compact_step();
        else if (A.job == WD_ATLAS_JOB_IMPORT) import_step();
        else if (A.req & WD_ATLAS_REQ_IMPORT) { A.req &= ~WD_ATLAS_REQ_IMPORT; import_start(); }
        else if ((A.req & WD_ATLAS_REQ_COMPACT) && K.state == KS_IDLE) {
            A.req &= ~WD_ATLAS_REQ_COMPACT;
            if (A.tail_dev || !A.dev_count) compact_start();
        } else if (A.req & WD_ATLAS_REQ_COUNT) {
            A.req &= ~WD_ATLAS_REQ_COUNT;
            A.import_left = import_list(NULL, 0);
        } else if (A.state_dirty) state_write();
    }
    log_close();
    io_unlock();
}

/* ------------------------------------------------------------ home base sync */

static void ensure_loaded(void)
{
    if (A.state != WD_ATLAS_READY) load(1);
}

uint32_t wd_atlas_seq(void)
{
    io_lock();
    ensure_loaded();
    const uint32_t s = A.seq;
    io_unlock();
    return s;
}

static struct { uint32_t seq; long off; } s_dcache;

/* The offset of the first record after `since`: a binary search over the
   log, resynchronising on the sync byte and CRC at each probe. */
static long log_find(uint32_t since)
{
    if (s_dcache.seq == since && s_dcache.off >= LOG_HDR && s_dcache.off <= A.log_end) return s_dcache.off;
    long lo = LOG_HDR, hi = A.log_end;
    char p[160];
    apath(p, sizeof(p), "log.bin");
    FILE *f = fopen(p, "rb");
    if (!f) return LOG_HDR;
    static uint8_t b[1024];
    static rec_t r;
    while (hi - lo > 1024) {
        const long mid = lo + (hi - lo) / 2;
        long o = -1;
        uint32_t s = 0;
        if (!fseek(f, mid, SEEK_SET)) {
            const size_t n = fread(b, 1, sizeof(b), f);
            for (size_t i = 0; i < n; i++)
                if (b[i] == SYNC && unframe(b + i, n - i, &r) > 0) { o = mid + (long)i; s = r.seq; break; }
        }
        if (o < 0 || o >= hi) { hi = mid; continue; }
        if (s <= since) lo = o; else hi = o;
    }
    fclose(f);
    return lo;
}

size_t wd_atlas_delta(uint32_t since, uint8_t *out, size_t cap, uint32_t *next)
{
    if (next) *next = since;
    if (!out || cap < 4 + REC_OVH + 8) return 0;
    io_lock();
    ensure_loaded();
    log_close();
    if (A.state != WD_ATLAS_READY || since >= A.seq) { io_unlock(); return 0; }
    lrd_t r;
    if (!lrd_open(&r, BUF_LOAD, log_find(since))) { io_unlock(); return 0; }
    out[0] = 'W'; out[1] = 'A'; out[2] = 1; out[3] = 0;
    size_t n = 4;
    uint32_t last = since;
    static rec_t rec;
    for (long at; lrd_next(&r, &rec, &at) == 1;) {
        if (rec.seq <= since) continue;
        if (n + rec.size > cap) break;
        n += (size_t)frame(out + n, rec.type, rec.seq, rec.p, rec.len);
        last = rec.seq;
        s_dcache.seq = rec.seq;
        s_dcache.off = r.off;
    }
    lrd_close(&r);
    io_unlock();
    if (n == 4) return 0;
    if (next) *next = last;
    return n;
}

int wd_atlas_apply(const uint8_t *buf, size_t len, uint32_t origin)
{
    if (!buf || len < 4 || buf[0] != 'W' || buf[1] != 'A' || buf[2] != 1) return -1;
    static rec_t r;
    for (size_t off = 4; off < len;) {
        const int k = unframe(buf + off, len - off, &r);
        if (k <= 0 || !rec_valid(&r)) return -1;
        off += (size_t)k;
    }
    io_lock();
    ensure_loaded();
    if (A.state != WD_ATLAS_READY) { io_unlock(); return -1; }
    int merged = 0;
    for (size_t off = 4; off < len;) {
        off += (size_t)unframe(buf + off, len - off, &r);
        uint32_t o = rec_origin(&r);
        if (!o) { if (!origin) continue; o = origin; rec_set_origin(&r, o); }
        const uint16_t no = rec_no(&r);
        bool take = true;
        wd_lock();
        const run_t *u = run_find(o, no);
        if (r.type == R_RUN) {
            /* a repeat changes nothing: only what is newer is logged */
            m_run_t m;
            dec_run(&r, &m);
            take = !u || min_nz(u->start, m.start) != u->start || m.end > u->end || (m.stem && !u->stem) ||
                   m.dist_m > u->dist_m || m.ndev > u->ndev || m.nnew > u->nnew || (u->src | m.src) != u->src ||
                   ((u->flags | (m.flags & RF_SYNCED)) != u->flags);
        } else {
            take = !u || rec_ri(&r) >= u->rec_next;
        }
        wd_unlock();
        if (take && emit(r.type, r.p, r.len, NULL)) merged++;
    }
    log_close();
    if (merged) { A.state_dirty = true; A.applied = true; }
    if (A.tail_dev > 4096) A.req |= WD_ATLAS_REQ_COMPACT;
    io_unlock();
    return merged;
}

/* ------------------------------------------------------------ what the page reads */

int wd_atlas_src(int kind)
{
    switch (kind) {
    case WD_WIFI: return WD_AS_WIFI;
    case WD_BLE: return WD_AS_BT;
    case WD_RF: return WD_AS_RF;
    case WD_LORA: return WD_AS_LORA;
    case WD_CELL: return WD_AS_CELL;
    default: return WD_AS_AIR;      /* aircraft and drones */
    }
}
const char *wd_atlas_src_name(int s)
{
    static const char *const N[WD_AS_N] = { "WI-FI", "BT", "RF", "AIR", "LORA", "CELL" };
    return s >= 0 && s < WD_AS_N ? N[s] : "?";
}

void wd_atlas_info(wd_atlas_info_t *o)
{
    memset(o, 0, sizeof(*o));
    wd_lock();
    o->state = A.state;
    o->job = A.job;
    o->job_pct = A.job_pct;
    snprintf(o->note, sizeof(o->note), "%s", A.note);
    o->board = A.id;
    o->seq = A.seq;
    o->devices = A.dev_count;
    for (int k = 0; k < WD_KINDS; k++) o->kind_dev[k] = A.dev_kind[k];
    o->runs = A.done_runs;
    o->cells = A.ncells;
    o->km = A.total_m / 1000;
    o->tail = A.tail_dev;
    o->routes = A.nroutes;
    o->import_left = A.import_left;
    o->live = L.on && A.state == WD_ATLAS_READY;
    o->known_ready = K.ready;
    o->route = -1;
    o->recent = !o->live && A.state == WD_ATLAS_READY && L.evt_begin > 0 && L.done_end == L.evt_end && L.origin;
    if (o->live || o->recent) {
        const run_t *u = L.origin && (L.have_run || !o->live) ? run_find(L.origin, L.no) : NULL;
        if (o->live) {
            o->route = L.route_guess;
            o->provisional = true;
            o->run_num = (uint16_t)(L.route_guess >= 0 ? RT[L.route_guess].nruns + 1 : 0);
        } else if (u && u->route) {
            o->route = u->route - 1;
            o->run_num = u->num;
        }
        o->run_dev = L.ndev; o->run_new = L.nnew; o->run_reg = L.nreg;
        o->run_missing = L.nmissing; o->run_moved = L.nmoved;
        o->run_cells = L.have_run ? (unsigned)AC[0].n : u ? u->ncells : 0;
        o->run_gain = u ? u->ngain : 0;
        memcpy(o->run_gain_src, L.gain_src, sizeof(o->run_gain_src));
        o->run_m = (uint32_t)L.dist;
        o->run_src = L.src_union;
    }
    wd_unlock();
}

bool wd_atlas_route_info(int i, wd_atlas_route_t *o)
{
    memset(o, 0, sizeof(*o));
    wd_lock();
    const bool ok = A.state == WD_ATLAS_READY && i >= 0 && i < A.nroutes;
    if (ok) {
        const route_t *R = &RT[i];
        memcpy(o->name, R->name, sizeof(o->name));
        o->runs = R->nruns;
        o->cells = R->ncells;
        o->best = R->best;
        o->last = R->last;
    }
    wd_unlock();
    return ok;
}

static void cell_out(wd_atlas_cell_t *o, int32_t cx, int32_t cy, uint16_t hits, uint8_t mask)
{
    memset(o, 0, sizeof(*o));
    o->cx = cx; o->cy = cy; o->hits = hits; o->mask = mask;
    const cell_t *c = cell_find(cx, cy);
    if (c) for (int s = 0; s < WD_AS_N; s++) { o->passes[s] = (uint8_t)nib(c->src, s); if (o->passes[s]) o->mask |= (uint8_t)(1u << s); }
    if (L.have_run) {
        const int j = acc_find(&AC[0], cx, cy);
        if (j >= 0) o->now = AC[0].c[j].mask ? AC[0].c[j].mask : 0x80;
    }
}

int wd_atlas_route_cells(int i, wd_atlas_cell_t *out, int cap)
{
    int n = 0;
    wd_lock();
    if (A.state == WD_ATLAS_READY && i >= 0 && i < A.nroutes) {
        const route_t *R = &RT[i];
        for (int j = 0; j < R->ncells && n < cap; j++) cell_out(&out[n++], R->c[j].cx, R->c[j].cy, R->c[j].hits, R->c[j].mask);
    }
    wd_unlock();
    return n;
}

static int by_num(const void *a, const void *b)
{
    return (int)((const wd_atlas_run_t *)a)->num - (int)((const wd_atlas_run_t *)b)->num;
}

int wd_atlas_route_runs(int i, wd_atlas_run_t *out, int cap)
{
    int n = 0;
    wd_lock();
    if (A.state == WD_ATLAS_READY && i >= 0 && i < A.nroutes)
        for (int k = 0; k < A.nruns; k++) {
            if (RU[k].route != i + 1 || !RU[k].end) continue;
            if (n < cap) out[n++] = run_sum(&RU[k]);
            else {
                /* full: keep the newest */
                int lo = 0;
                for (int j = 1; j < cap; j++) if (out[j].num < out[lo].num) lo = j;
                if (RU[k].num > out[lo].num) out[lo] = run_sum(&RU[k]);
            }
        }
    wd_unlock();
    qsort(out, (size_t)n, sizeof(*out), by_num);
    return n;
}

int wd_atlas_run_cells(wd_atlas_cell_t *out, int cap)
{
    int n = 0;
    wd_lock();
    if (A.state == WD_ATLAS_READY && L.have_run)
        for (int j = 0; j < AC[0].n && n < cap; j++) cell_out(&out[n++], AC[0].c[j].cx, AC[0].c[j].cy, 1, AC[0].c[j].mask);
    wd_unlock();
    return n;
}

static int by_drives(const void *a, const void *b)
{
    return ((const wd_atlas_item_t *)b)->drives - ((const wd_atlas_item_t *)a)->drives;
}

int wd_atlas_list(int which, wd_atlas_item_t *out, int cap)
{
    int n = 0;
    wd_lock();
    if (A.state == WD_ATLAS_READY) {
        if (which == WD_ATLAS_L_NEW || which == WD_ATLAS_L_MOVED) {
            const wd_atlas_item_t *ring = which == WD_ATLAS_L_NEW ? L.newr : L.movr;
            const int rc = which == WD_ATLAS_L_NEW ? 32 : 16;
            const int head = which == WD_ATLAS_L_NEW ? L.new_head : L.mov_head, cnt = which == WD_ATLAS_L_NEW ? L.new_n : L.mov_n;
            for (int k = 0; k < cnt && n < cap; k++) out[n++] = ring[(head - 1 - k + 2 * rc) % rc];
        } else {
            for (int i = 0; i < K.nreg && n < cap; i++) {
                const reg_t *g = &RG[i];
                const bool want = which == WD_ATLAS_L_MISSING ? (g->flags & K_MOVED) != 0 : (g->flags & K_SEEN) != 0;
                if (!want) continue;
                wd_atlas_item_t *it = &out[n++];
                memset(it, 0, sizeof(*it));
                it->kind = g->kind;
                it->drives = g->drives;
                it->rssi = 0;
                memcpy(it->addr, g->addr, 6);
                snprintf(it->name, sizeof(it->name), "%s", g->name);
            }
        }
    }
    wd_unlock();
    if (which == WD_ATLAS_L_MISSING || which == WD_ATLAS_L_REGULAR) qsort(out, (size_t)n, sizeof(*out), by_drives);
    return n;
}

void wd_atlas_request(int what)
{
    wd_lock();
    A.req |= what;
    if (A.state == WD_ATLAS_NOCARD || A.state == WD_ATLAS_NOMEM) A.last_try = 0;
    wd_unlock();
}

bool wd_atlas_pending(void)
{
    return A.state == WD_ATLAS_OFF || A.state == WD_ATLAS_LOADING || A.job != WD_ATLAS_JOB_NONE ||
           (A.state == WD_ATLAS_READY && (A.req || A.state_dirty || A.applied || A.quiet_from)) || L.done_end != L.evt_end;
}

void wd_atlas_reload(void)
{
    io_lock();
    log_close();
    if (A.job == WD_ATLAS_JOB_COMPACT) compact_abort();
    if (A.job == WD_ATLAS_JOB_IMPORT) import_abort();
    if (K.idx) fclose(K.idx);
    K.idx = NULL;
    lrd_close(&K.rd);
    wd_lock();
    A.state = WD_ATLAS_OFF;
    A.req = 0;
    A.import_left = -1;
    K.ready = false;
    K.state = KS_IDLE;
    L.have_run = false;
    L.route_guess = -1;
    s_dcache.seq = 0; s_dcache.off = 0;
    wd_unlock();
    io_unlock();
}

uint32_t wd_atlas_board(void) { return A.state == WD_ATLAS_READY ? A.id : 0; }
