#include "cartocore/places.h"
#include <string.h>
#include "fold_table.h"

#define HDR 128u
#define KEY_REC 12u
#define AREA_REC 56u
#define CITY_REC 24u
#define SHARD_REC 56u
#define REC_ID(r) ((r) & 0xFFFFFu)
#define REC_COUNTRY(r) ((r) >> 20)
#define ALIAS_MAX 240u

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static int32_t rds32(const uint8_t *p) { return (int32_t)rd32(p); }

/* ---- CRC32 (IEEE), nibble table ---- */
static uint32_t crc_update(uint32_t c, const uint8_t *p, size_t n) {
    static const uint32_t t[16] = {0, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
        0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u,
        0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    while (n--) { c ^= *p++; c = (c >> 4) ^ t[c & 15]; c = (c >> 4) ^ t[c & 15]; }
    return c;
}

/* ---- I/O through page caches ---- */
static int raw_read(cc_places_file *f, uint32_t off, void *buf, size_t len) {
    if (!len) return 1;
    if (off > f->size || len > f->size - off) return 0;
    if (!f->read(f->ctx, off, buf, len)) return 0;
    f->reads++; f->bytes_read += len; return 1;
}
typedef struct { uint32_t off, len; uint8_t buf[CC_PLACES_PAGE]; } page_t;
/* Copy len bytes at off through cache c (len may exceed one page). */
static int cached(cc_places_file *f, page_t *c, uint32_t off, void *out, size_t len) {
    uint8_t *o = out;
    if (off > f->size || len > f->size - off) return 0;
    while (len) {
        if (!(c->len && off >= c->off && off - c->off < c->len)) {
            uint32_t base = off - off % CC_PLACES_PAGE;
            size_t n = f->size - base < CC_PLACES_PAGE ? (size_t)(f->size - base) : CC_PLACES_PAGE;
            c->len = 0;
            if (!raw_read(f, base, c->buf, n)) return 0;
            c->off = base; c->len = (uint32_t)n;
        }
        size_t k = c->len - (off - c->off);
        if (k > len) k = len;
        memcpy(o, c->buf + (off - c->off), k);
        o += k; off += (uint32_t)k; len -= k;
    }
    return 1;
}

/* ---- header ---- */
static int open_file(cc_places_file *f, cc_places_read read, void *ctx, uint64_t size, int want_type) {
    uint8_t h[HDR];
    memset(f, 0, sizeof(*f));
    if (!read || size < HDR || size > 0xFFFFFFF0u) return 0;
    f->read = read; f->ctx = ctx; f->size = size;
    if (!raw_read(f, 0, h, HDR) || memcmp(h, "CCPLACE1", 8) || rd16(h + 8) != 1) return 0;
    if (crc_update(0xFFFFFFFFu, h, 124) ^ 0xFFFFFFFFu ^ rd32(h + 124)) return 0;
    f->type = h[10]; f->zoom = h[11];
    f->nkeys = rd32(h + 16); f->narea = rd32(h + 20); f->ncity = rd32(h + 24);
    f->keys = rd32(h + 28); f->pool = rd32(h + 32); f->pool_len = rd32(h + 36);
    f->area = rd32(h + 40); f->city = rd32(h + 44); f->cells = rd32(h + 48); f->cells_len = rd32(h + 52);
    f->shard = rd32(h + 56); f->nshard = rd32(h + 60);
    f->credit_off = rd32(h + 72); f->credit_len = rd16(h + 76); f->bloom = rd32(h + 84);
    f->iso2[0] = (char)h[80]; f->iso2[1] = (char)h[81];
    if (f->type != want_type || rd32(h + 64) != size || f->zoom > 20) return 0;
    {   /* sections ordered, inside the file, sized for their counts */
        uint64_t k = (uint64_t)f->keys + (uint64_t)f->nkeys * KEY_REC;
        uint64_t a = (uint64_t)f->area + (uint64_t)f->narea * AREA_REC;
        uint64_t c = (uint64_t)f->city + (uint64_t)f->ncity * CITY_REC;
        uint64_t s = (uint64_t)f->shard + (uint64_t)f->nshard * SHARD_REC;
        if (f->keys < HDR || k > f->pool || (uint64_t)f->pool + f->pool_len > f->area || a > f->city ||
            c > f->cells || (uint64_t)f->cells + f->cells_len > f->shard || s > f->bloom || f->bloom > size) return 0;
        if ((uint64_t)f->credit_off + f->credit_len > f->pool_len) return 0;
        if (want_type == 1 && f->nkeys && !f->narea && !f->ncity) return 0;
        if (want_type == 2 && (f->narea || f->zoom)) return 0;
    }
    f->open = 1; f->reads = 0; f->bytes_read = 0; return 1;
}

int cc_places_verify(cc_places_read read, void *ctx, uint64_t size, void *buf, size_t buf_bytes) {
    uint8_t *b = buf; uint8_t h[HDR]; uint32_t c = 0xFFFFFFFFu; uint64_t off = HDR;
    if (!read || !buf || buf_bytes < 16 || size < HDR || size > 0xFFFFFFF0u) return 0;
    if (!read(ctx, 0, h, HDR) || memcmp(h, "CCPLACE1", 8) || rd32(h + 64) != size) return 0;
    if ((crc_update(0xFFFFFFFFu, h, 124) ^ 0xFFFFFFFFu) != rd32(h + 124)) return 0;
    while (off < size) {
        size_t n = size - off < buf_bytes ? (size_t)(size - off) : buf_bytes;
        if (!read(ctx, off, b, n)) return 0;
        c = crc_update(c, b, n); off += n;
    }
    return (c ^ 0xFFFFFFFFu) == rd32(h + 68);
}

int cc_places_open(cc_places *p, cc_places_read read, void *ctx, uint64_t size) {
    memset(p, 0, sizeof(*p));
    return open_file(&p->file[0], read, ctx, size, 1);
}
int cc_places_attach(cc_places *p, cc_places_read read, void *ctx, uint64_t size) {
    cc_places_file t;
    if (!p->file[0].open || !open_file(&t, read, ctx, size, 2)) return 0;
    if (cc_places_shard_attached(p, t.iso2)) return 0;
    for (int i = 1; i <= CC_PLACES_MAX_SHARDS; ++i)
        if (!p->file[i].open) { p->file[i] = t; return i; }
    return 0;
}
void cc_places_detach(cc_places *p, int slot) {
    if (slot >= 1 && slot <= CC_PLACES_MAX_SHARDS) memset(&p->file[slot], 0, sizeof(p->file[slot]));
}
int cc_places_shard_attached(const cc_places *p, const char iso2[2]) {
    for (int i = 1; i <= CC_PLACES_MAX_SHARDS; ++i)
        if (p->file[i].open && p->file[i].iso2[0] == iso2[0] && p->file[i].iso2[1] == iso2[1]) return 1;
    return 0;
}
unsigned cc_places_zoom(const cc_places *p) { return p->file[0].zoom; }
size_t cc_places_credit(const cc_places *p, char *buf, size_t n) {
    const cc_places_file *cf = &p->file[0];
    size_t len;
    if (!cf->open || !n) return 0;
    len = cf->credit_len < n - 1 ? cf->credit_len : n - 1;
    if (!cached((cc_places_file *)cf, (page_t *)&((cc_places_file *)cf)->pc, cf->pool + cf->credit_off, buf, len)) len = 0;
    buf[len] = 0; return len;
}

/* ---- folding ---- */
static size_t fold_query(const char *s, char *out) {
    size_t n = 0; int pend = 0;
    const uint8_t *u = (const uint8_t *)s;
    while (*u && n < CC_PLACES_KEY_MAX) {
        uint32_t cp = *u++; const char *rep = 0; size_t rl = 0; char one;
        if (cp >= 0xC0 && cp < 0xF0) {
            int cont = cp >= 0xE0 ? 2 : 1; uint32_t v = cp & (cp >= 0xE0 ? 0x0F : 0x1F);
            const uint8_t *w = u;
            for (int i = 0; i < cont; ++i) {
                if ((*w & 0xC0) != 0x80) { w = 0; break; }
                v = v << 6 | (*w++ & 0x3F);
            }
            if (w) { u = w; cp = v; }       /* else a Latin-1 byte (Windows ANSI argv): cp stays */
        } else if (cp >= 0xF0) { while ((*u & 0xC0) == 0x80) u++; cp = 0xFFFFFFFFu; }
        else if (cp >= 0x80) cp = 0xFFFFFFFFu;
        if (cp < 0x80) {
            if (cp >= 'A' && cp <= 'Z') { one = (char)(cp + 32); rep = &one; rl = 1; }
            else if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) { one = (char)cp; rep = &one; rl = 1; }
            else if (cp == '.' || cp == '\'' || cp == '`') rl = 0;
            else { pend = n > 0; continue; }
        } else {
            int found = 0;
            for (size_t i = 0; i < sizeof(fold_drop) / sizeof(fold_drop[0]); ++i)
                if (fold_drop[i] == cp) { found = 1; break; }
            if (found) rl = 0;
            else {
                for (size_t i = 0; i < sizeof(fold_ranges) / sizeof(fold_ranges[0]); ++i)
                    if (cp >= fold_ranges[i].lo && cp < fold_ranges[i].hi) {
                        rep = fold_tab[fold_ranges[i].base + (cp - fold_ranges[i].lo)];
                        while (rl < 3 && rep[rl]) rl++;
                        found = 1; break;
                    }
                if (!found) { pend = n > 0; continue; }
            }
        }
        if (rl && pend) { if (n < CC_PLACES_KEY_MAX) out[n++] = ' '; pend = 0; }
        for (size_t i = 0; i < rl && n < CC_PLACES_KEY_MAX; ++i) out[n++] = rep[i];
    }
    out[n] = 0; return n;
}

/* ---- key table ---- */
typedef struct { uint32_t key_off, rec; uint16_t rank; uint8_t keylen, flags; } key_t_;
static int read_key(cc_places_file *f, uint32_t i, key_t_ *k) {
    uint8_t b[KEY_REC];
    if (i >= f->nkeys || !cached(f, (page_t *)&f->kc, f->keys + i * KEY_REC, b, KEY_REC)) return 0;
    k->key_off = rd32(b); k->rec = rd32(b + 4); k->rank = (uint16_t)rd16(b + 8); k->keylen = b[10]; k->flags = b[11];
    return k->keylen <= CC_PLACES_KEY_MAX && (uint64_t)k->key_off + k->keylen <= f->pool_len;
}
/* compare key i with q: <0, 0, >0. prefix: a key starting with q compares equal. */
static int cmp_key(cc_places_file *f, uint32_t i, const char *q, size_t ql, int prefix, int *err) {
    key_t_ k; uint8_t t[CC_PLACES_KEY_MAX]; size_t m; int c;
    if (!read_key(f, i, &k) || !cached(f, (page_t *)&f->pc, f->pool + k.key_off, t, k.keylen)) { *err = 1; return 0; }
    m = k.keylen < ql ? k.keylen : ql;
    c = memcmp(t, q, m);
    if (c) return c;
    if (prefix) return k.keylen >= ql ? 0 : -1;
    return k.keylen == ql ? 0 : (k.keylen < ql ? -1 : 1);
}
/* first index in [lo, hi] whose key is >= q (prefix == 0) or > the whole prefix range of q (prefix == 1) */
static int bound_in(cc_places_file *f, const char *q, size_t ql, int prefix, uint32_t lo, uint32_t hi, uint32_t *out) {
    int err = 0;
    while (lo < hi) {
        uint32_t m = lo + (hi - lo) / 2;
        int c = cmp_key(f, m, q, ql, prefix, &err);
        if (err) return 0;
        if (prefix ? c <= 0 : c < 0) lo = m + 1; else hi = m;
    }
    *out = lo; return 1;
}
static int bound(cc_places_file *f, const char *q, size_t ql, int prefix, uint32_t *out) {
    return bound_in(f, q, ql, prefix, 0, f->nkeys, out);
}

/* End of the prefix range that starts at lo: gallop from lo (ranges are usually tiny,
   so the probes stay inside pages already cached by the lower-bound search). */
static int bound_hi(cc_places_file *f, const char *q, size_t ql, uint32_t lo, uint32_t *out) {
    uint32_t a = lo, b, step = 1; int err = 0, c;
    *out = lo;
    if (lo >= f->nkeys) return 1;
    c = cmp_key(f, lo, q, ql, 1, &err);
    if (err) return 0;
    if (c != 0) return 1;
    for (;;) {
        b = a + step;
        if (b < a || b >= f->nkeys) { b = f->nkeys; break; }
        c = cmp_key(f, b, q, ql, 1, &err);
        if (err) return 0;
        if (c > 0) break;
        a = b; step <<= 1;
    }
    while (a + 1 < b) {          /* a is inside the range, b is past it */
        uint32_t m = a + (b - a) / 2;
        c = cmp_key(f, m, q, ql, 1, &err);
        if (err) return 0;
        if (c > 0) b = m; else a = m;
    }
    *out = b; return 1;
}

/* ---- hit set: top N, one entry per record ---- */
typedef struct { cc_place_hit *h; unsigned n, max; } hits_t;
static void hit_add(hits_t *hs, uint8_t file, uint32_t rec, uint16_t rank, uint32_t tier) {
    uint32_t score = tier << 16 | rank; unsigned i, at;
    for (i = 0; i < hs->n; ++i)
        if (hs->h[i].file == file && hs->h[i].id == rec) {
            if (hs->h[i].score >= score) return;
            memmove(hs->h + i, hs->h + i + 1, (hs->n - i - 1) * sizeof(*hs->h)); hs->n--; break;
        }
    if (hs->n == hs->max && score <= hs->h[hs->n - 1].score) return;
    for (at = 0; at < hs->n && hs->h[at].score >= score; ++at) {}
    if (hs->n < hs->max) hs->n++;
    memmove(hs->h + at + 1, hs->h + at, (hs->n - at - 1) * sizeof(*hs->h));
    hs->h[at].id = rec; hs->h[at].score = score; hs->h[at].file = file;
    hs->h[at].kind = (uint8_t)(4 - (rank >> 14));
}

/* ---- records ---- */
typedef struct {
    uint32_t name_off, alias_off, parent, pop, cells_off, ncells; uint16_t alias_len, shard_idx;
    uint8_t name_len, kind; int32_t lat, lon, bbox[4];
} area_t;
static int read_area(cc_places_file *f, uint32_t id, area_t *a) {
    uint8_t b[AREA_REC];
    if (id >= f->narea || !cached(f, (page_t *)&f->rc, f->area + id * AREA_REC, b, AREA_REC)) return 0;
    a->name_off = rd32(b); a->alias_off = rd32(b + 4); a->alias_len = (uint16_t)rd16(b + 8);
    a->name_len = b[10]; a->kind = b[11]; a->parent = rd32(b + 12); a->pop = rd32(b + 16);
    a->lat = rds32(b + 20); a->lon = rds32(b + 24);
    for (int i = 0; i < 4; ++i) a->bbox[i] = rds32(b + 28 + 4 * i);
    a->cells_off = rd32(b + 44); a->ncells = rd32(b + 48); a->shard_idx = (uint16_t)rd16(b + 52);
    return (uint64_t)a->name_off + a->name_len <= f->pool_len && (uint64_t)a->alias_off + a->alias_len <= f->pool_len &&
           a->alias_len <= ALIAS_MAX && a->cells_off <= f->cells_len;
}
typedef struct { uint32_t name_off, parent, pop; int32_t lat, lon; uint8_t name_len, flags; } city_t;
static int read_city(cc_places_file *f, uint32_t i, city_t *c) {
    uint8_t b[CITY_REC];
    if (i >= f->ncity || !cached(f, (page_t *)&f->rc, f->city + i * CITY_REC, b, CITY_REC)) return 0;
    c->name_off = rd32(b); c->parent = rd32(b + 4); c->pop = rd32(b + 8); c->lat = rds32(b + 12);
    c->lon = rds32(b + 16); c->name_len = b[20]; c->flags = b[21];
    return (uint64_t)c->name_off + c->name_len <= f->pool_len;
}

/* Does `rest` (folded, tokens separated by one space) describe an ancestor chain
   starting at area `at` of the base file? Each alias may be consumed whole as a leading
   chunk; the final chunk may be a prefix. Levels may be skipped. */
static int ctx_match(cc_places_file *bf, const char *rest, size_t rl, uint32_t at, int depth, int *exact, int *err) {
    area_t a; char al[ALIAS_MAX + 1];
    if (at == CC_PLACES_NONE || depth > 2) return 0;
    if (!read_area(bf, at, &a) || !cached(bf, (page_t *)&bf->pc, bf->pool + a.alias_off, al, a.alias_len)) { *err = 1; return 0; }
    for (size_t s = 0; s < a.alias_len;) {
        size_t e = s; while (e < a.alias_len && al[e]) e++;
        size_t n = e - s;
        if (rl <= n && !memcmp(al + s, rest, rl)) { *exact = rl == n; return 1; }
        if (rl > n && !memcmp(al + s, rest, n) && rest[n] == ' ') {
            if (ctx_match(bf, rest + n + 1, rl - n - 1, a.parent, depth + 1, exact, err)) return 1;
            if (*err) return 0;
        }
        s = e + 1;
    }
    return ctx_match(bf, rest, rl, a.parent, depth + 1, exact, err);
}

static int scan_prefix(cc_places *p, int fi, const char *q, size_t ql, hits_t *hs, cc_places_stats *st) {
    cc_places_file *f = &p->file[fi]; uint32_t lo, hi, i, end, lo0 = 0, hi0 = f->nkeys;
    if (f->memo.ql && ql >= f->memo.ql && !memcmp(q, f->memo.q, f->memo.ql)) { lo0 = f->memo.lo; hi0 = f->memo.hi; }
    f->memo.ql = 0;                       /* typing "ver" then "verm": search only inside the last range */
    if (hi0 > f->nkeys || lo0 > hi0) { lo0 = 0; hi0 = f->nkeys; }
    if (!bound_in(f, q, ql, 0, lo0, hi0, &lo) || !bound_hi(f, q, ql, lo, &hi)) return 0;
    memcpy(f->memo.q, q, ql); f->memo.ql = (uint8_t)ql; f->memo.lo = lo; f->memo.hi = hi;
    end = hi - lo > CC_PLACES_MAX_SCAN ? lo + CC_PLACES_MAX_SCAN : hi;
    if (end < hi) st->truncated = 1;
    for (i = lo; i < end; ++i) {
        key_t_ k;
        if (!read_key(f, i, &k)) return 0;
        st->scanned++;
        hit_add(hs, (uint8_t)fi, REC_ID(k.rec), k.rank, k.keylen == ql ? (k.flags & 1 ? 3u : 4u) : (k.flags & 1 ? 1u : 2u));
    }
    return 1;
}

/* Exact name `head` whose ancestors satisfy `rest`. */
static int scan_head(cc_places *p, int fi, const char *head, size_t hl, const char *rest, size_t rl,
                     hits_t *hs, cc_places_stats *st) {
    cc_places_file *f = &p->file[fi], *bf = &p->file[0]; uint32_t i; int err = 0;
    if (!bound(f, head, hl, 0, &i)) return 0;
    for (unsigned n = 0; i < f->nkeys && n < CC_PLACES_MAX_SCAN; ++i, ++n) {
        key_t_ k; uint32_t parent; int exact = 0;
        int c = cmp_key(f, i, head, hl, 0, &err);
        if (err) return 0;
        if (c) break;
        if (!read_key(f, i, &k)) return 0;
        st->scanned++;
        if (REC_ID(k.rec) < f->narea) { area_t a; if (!read_area(f, REC_ID(k.rec), &a)) return 0; parent = a.parent; }
        else { city_t ct; if (!read_city(f, REC_ID(k.rec) - f->narea, &ct)) return 0; parent = ct.parent; }
        if (ctx_match(bf, rest, rl, parent, 0, &exact, &err)) hit_add(hs, (uint8_t)fi, REC_ID(k.rec), k.rank, exact ? 6u : 5u);
        if (err) return 0;
    }
    return 1;
}

typedef struct { char iso2[3]; uint32_t ncities, size, crc32; uint8_t sha256[32]; uint32_t bloom_off, bloom_len; } cc_places_shard_dir_;
static int read_shard_dir(cc_places_file *bf, unsigned idx, cc_places_shard_dir_ *d);
/* Blocked bloom of a country shard's city names (kept in the base): 1 read. */
static int shard_has_name(cc_places_file *bf, const cc_places_shard_dir_ *d, const char *key, size_t kl) {
    uint8_t blk[64]; uint32_t h1 = 2166136261u, h2, g, nblocks = d->bloom_len / 64, i;
    if (!nblocks || (uint64_t)d->bloom_off + d->bloom_len > bf->size - bf->bloom) return 1;  /* unknown: assume yes */
    for (size_t j = 0; j < kl; ++j) h1 = (h1 ^ (uint8_t)key[j]) * 16777619u;
    h2 = (h1 * 2654435761u) ^ (h1 >> 16);
    if (!cached(bf, (page_t *)&bf->pc, bf->bloom + d->bloom_off + (h1 % nblocks) * 64, blk, 64)) return -1;
    for (g = h2, i = 0; i < 6; ++i) {
        uint32_t bit;
        g = g * 1664525u + 1013904223u; bit = (g >> 16) & 511;
        if (!(blk[bit >> 3] & (1u << (bit & 7)))) return 0;
    }
    return 1;
}

/* Shard suggestions for "head rest": countries whose states/countries match `rest` and
   whose city shard (not attached) holds a name equal to `head`, best match first. */
static int add_hints(cc_places *p, const char *head, size_t hl, const char *rest, size_t rl, cc_places_stats *st) {
    cc_places_file *bf = &p->file[0]; uint32_t lo, hi, i; unsigned nb = 0;
    struct { uint32_t country, score; } best[12];
    if (!bound(bf, rest, rl, 0, &lo) || !bound_hi(bf, rest, rl, lo, &hi)) return 0;
    if (hi - lo > 2048) hi = lo + 2048;
    for (i = lo; i < hi; ++i) {
        key_t_ k; uint32_t score, c; unsigned j;
        if (!read_key(bf, i, &k)) return 0;
        if (REC_ID(k.rec) >= bf->narea) continue;
        c = REC_COUNTRY(k.rec); score = (k.keylen == rl ? 1u : 0u) << 16 | k.rank;
        for (j = 0; j < nb && best[j].country != c; ++j) {}
        if (j < nb) { if (best[j].score >= score) continue; } else if (nb < 12) nb++;
        else if (score <= best[11].score) continue; else j = 11;
        best[j].country = c; best[j].score = score;
        for (; j > 0 && best[j].score > best[j - 1].score; --j) {
            uint32_t t = best[j].country, sc = best[j].score;
            best[j].country = best[j - 1].country; best[j].score = best[j - 1].score;
            best[j - 1].country = t; best[j - 1].score = sc;
        }
    }
    for (unsigned b = 0; b < nb && st->nhints < CC_PLACES_MAX_HINTS; ++b) {
        area_t a; cc_places_shard_dir_ d; unsigned e; int has;
        for (e = 0; e < st->nhints && st->shard_area[e] != best[b].country; ++e) {}
        if (e < st->nhints) continue;
        if (!read_area(bf, best[b].country, &a)) return 0;
        if (a.kind != 1 || a.shard_idx >= bf->nshard) continue;
        if (!read_shard_dir(bf, a.shard_idx, &d)) return 0;
        if (cc_places_shard_attached(p, d.iso2)) continue;
        has = shard_has_name(bf, &d, head, hl);
        if (has < 0) return 0;
        if (has) st->shard_area[st->nhints++] = best[b].country;
    }
    return 1;
}

int cc_places_search(cc_places *p, const char *query, cc_place_hit *out, unsigned max_out, cc_places_stats *st) {
    char q[CC_PLACES_KEY_MAX + 1]; size_t ql, sp[8]; unsigned nsp = 0, phrase = 0; size_t before; hits_t hs; cc_places_stats local;
    if (!st) st = &local;
    memset(st, 0, sizeof(*st));
    if (!p->file[0].open || !out || !max_out || !query) return 0;
    hs.h = out; hs.n = 0; hs.max = max_out;
    ql = fold_query(query, q);
    if (!ql) return 0;
    for (size_t i = 0; i < ql && nsp < 8; ++i) if (q[i] == ' ') sp[nsp++] = i;
    for (int fi = 0; fi <= CC_PLACES_MAX_SHARDS; ++fi) {
        if (!p->file[fi].open) continue;
        before = st->scanned;
        if (!scan_prefix(p, fi, q, ql, &hs, st)) return -1;
        phrase += (unsigned)(st->scanned - before);
        for (unsigned k = nsp; k >= 1; --k)
            if (!scan_head(p, fi, q, sp[k - 1], q + sp[k - 1] + 1, ql - sp[k - 1] - 1, &hs, st)) return -1;
    }
    if (!phrase)                              /* the whole phrase names nothing: a qualifier */
      for (unsigned k = 0; k < nsp; ++k)      /* which country shards would add results? */
        if (!add_hints(p, q, sp[k], q + sp[k] + 1, ql - sp[k] - 1, st)) return -1;
    for (int i = 0; i <= CC_PLACES_MAX_SHARDS; ++i) { st->reads += p->file[i].reads; st->bytes += p->file[i].bytes_read; }
    return (int)hs.n;
}

static int read_shard_dir(cc_places_file *bf, unsigned idx, cc_places_shard_dir_ *d) {
    uint8_t b[SHARD_REC];
    if (idx >= bf->nshard || !cached(bf, (page_t *)&bf->rc, bf->shard + idx * SHARD_REC, b, SHARD_REC)) return 0;
    d->iso2[0] = (char)b[0]; d->iso2[1] = (char)b[1]; d->iso2[2] = 0;
    d->ncities = rd32(b + 4); d->size = rd32(b + 8); d->crc32 = rd32(b + 12); memcpy(d->sha256, b + 16, 32);
    d->bloom_off = rd32(b + 48); d->bloom_len = rd32(b + 52);
    return 1;
}

int cc_places_shard_info(cc_places *p, uint32_t country_area, cc_places_shard *out) {
    cc_places_file *bf = &p->file[0]; area_t a; cc_places_shard_dir_ d;
    if (!bf->open) return -1;
    if (!read_area(bf, country_area, &a)) return -1;
    if (a.kind != 1 || a.shard_idx >= bf->nshard) return 0;
    if (!read_shard_dir(bf, a.shard_idx, &d)) return -1;
    memcpy(out->iso2, d.iso2, 3); out->ncities = d.ncities; out->size = d.size; out->crc32 = d.crc32;
    memcpy(out->sha256, d.sha256, 32); return 1;
}

static void copy_name(cc_places_file *f, uint32_t off, uint8_t len, char *dst, size_t cap) {
    size_t n = len < cap - 1 ? len : cap - 1;
    if (!cached(f, (page_t *)&f->pc, f->pool + off, dst, n)) n = 0;
    dst[n] = 0;
}

int cc_places_info(cc_places *p, const cc_place_hit *hit, cc_place_info *in) {
    cc_places_file *f, *bf = &p->file[0]; area_t a; uint32_t parent;
    memset(in, 0, sizeof(*in));
    if (!hit || hit->file > CC_PLACES_MAX_SHARDS || !p->file[hit->file].open || !bf->open) return -1;
    f = &p->file[hit->file];
    if (hit->id < f->narea) {
        if (!read_area(f, hit->id, &a)) return -1;
        in->kind = a.kind; in->pop = a.pop; in->lat_e5 = a.lat; in->lon_e5 = a.lon; in->ncells = a.ncells;
        in->area_id = hit->id; memcpy(in->bbox_e5, a.bbox, sizeof(a.bbox));
        copy_name(f, a.name_off, a.name_len, in->name, sizeof(in->name));
        parent = a.parent;
    } else {
        city_t c;
        if (!read_city(f, hit->id - f->narea, &c)) return -1;
        in->kind = CC_PLACE_CITY; in->pop = c.pop; in->lat_e5 = c.lat; in->lon_e5 = c.lon; in->area_id = CC_PLACES_NONE;
        copy_name(f, c.name_off, c.name_len, in->name, sizeof(in->name));
        parent = c.parent;
    }
    if (parent != CC_PLACES_NONE) {
        if (!read_area(bf, parent, &a)) return -1;
        copy_name(bf, a.name_off, a.name_len, in->parent, sizeof(in->parent));
        if (a.kind == 2 && a.parent != CC_PLACES_NONE) {
            if (!read_area(bf, a.parent, &a)) return -1;
            copy_name(bf, a.name_off, a.name_len, in->country, sizeof(in->country));
        } else memcpy(in->country, in->parent, sizeof(in->country));
    }
    if (in->kind == CC_PLACE_COUNTRY) { in->country[0] = 0; in->parent[0] = 0; }
    return 0;
}

/* ---- tiny math (no libm): enough for one city centre ---- */
static double m_sin(double x) {          /* |x| <= pi/2 */
    double x2 = x * x, t = x, s = x;
    for (int i = 1; i < 10; ++i) { t *= -x2 / ((2.0 * i) * (2.0 * i + 1.0)); s += t; }
    return s;
}
static double m_sqrt(double x) {
    double g = x > 1 ? x : 1; if (x <= 0) return 0;
    for (int i = 0; i < 40; ++i) g = 0.5 * (g + x / g);
    return g;
}
static double m_ln(double x) {
    int k = 0; double z, z2, s = 0, t;
    while (x >= 1.4142135623730951) { x *= 0.5; k++; }
    while (x < 0.7071067811865476) { x *= 2.0; k--; }
    z = (x - 1.0) / (x + 1.0); z2 = z * z; t = z;
    for (int i = 1; i < 40; i += 2) { s += t / i; t *= z2; }
    return 2.0 * s + k * 0.6931471805599453;
}
static double m_floor(double x) { long long i = (long long)x; return (double)(i > x ? i - 1 : i); }

/* ---- cell iteration ---- */
static int vfill(cc_places_cells *it) {
    uint32_t left = it->end - it->pos; size_t n = left < sizeof(it->buf) ? left : sizeof(it->buf);
    it->bi = 0; it->bn = 0;
    if (!n || !raw_read(it->f, it->pos, it->buf, n)) return 0;
    it->pos += (uint32_t)n; it->bn = (uint8_t)n; return 1;
}
static int varint(cc_places_cells *it, uint32_t *v) {
    uint32_t r = 0; unsigned shift = 0;
    for (;;) {
        uint8_t b;
        if (it->bi == it->bn && !vfill(it)) { it->err = 1; return 0; }
        b = it->buf[it->bi++];
        if (shift > 28) { it->err = 1; return 0; }
        r |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    *v = r; return 1;
}

int cc_places_cells_open(cc_places *p, const cc_place_hit *hit, unsigned radius_km, cc_places_cells *it) {
    cc_places_file *bf = &p->file[0], *hf;
    memset(it, 0, sizeof(*it));
    if (!bf->open || !hit || hit->file > CC_PLACES_MAX_SHARDS || !p->file[hit->file].open) return 0;
    hf = &p->file[hit->file];
    it->f = bf; it->n = 1u << bf->zoom;
    if (hit->id < hf->narea) {
        area_t a;
        if (!read_area(hf, hit->id, &a) || a.cells_off >= bf->cells_len) return 0;
        it->mode = 1; it->pos = bf->cells + a.cells_off; it->end = bf->cells + bf->cells_len;
        if (!varint(it, &it->rows_left) || it->rows_left > it->n) { it->mode = 0; return 0; }
        return 1;
    } else {
        city_t c; double lat, lon, s, cs, y, r, ylo, yhi;
        if (!radius_km || radius_km > 500 || !read_city(hf, hit->id - hf->narea, &c)) return 0;
        lat = c.lat / 100000.0; lon = c.lon / 100000.0;
        if (lat > 85.0511287798066) lat = 85.0511287798066;
        if (lat < -85.0511287798066) lat = -85.0511287798066;
        s = m_sin(lat * 0.017453292519943295);
        cs = m_sqrt(1.0 - s * s);
        y = 0.5 - 0.5 * m_ln((1.0 + s) / (1.0 - s)) / 6.283185307179586;
        it->fx = (lon + 180.0) / 360.0 * it->n; it->fy = y * it->n;
        if (it->fx > it->n - 1e-9) it->fx = it->n - 1e-9;
        if (it->fx < 0) it->fx = 0;
        if (it->fy > it->n - 1e-9) it->fy = it->n - 1e-9;
        if (it->fy < 0) it->fy = 0;
        r = radius_km / (6.283185307179586 * 6371.0088 / it->n * cs);
        it->r2 = r * r;
        it->x_lo = (int32_t)m_floor(it->fx - r); it->x_hi = (int32_t)m_floor(it->fx + r);
        if ((uint32_t)(it->x_hi - it->x_lo) >= it->n) it->x_hi = it->x_lo + (int32_t)it->n - 1;
        ylo = m_floor(it->fy - r); yhi = m_floor(it->fy + r);
        it->cy = ylo < 0 ? 0 : (uint32_t)ylo; it->y_hi = yhi > it->n - 1 ? it->n - 1 : (uint32_t)yhi;
        it->cx = it->x_lo; it->mode = 2;
        return 1;
    }
}

int cc_places_cells_next_run(cc_places_cells *it, uint32_t *x0, uint32_t *y, uint32_t *len) {
    uint32_t gap, l;
    if (it->mode == 2) return -2;
    if (it->mode != 1) return 0;
    if (it->err) return -1;
    while (!it->runs_left) {
        uint32_t dy, nr;
        if (!it->rows_left) return 0;
        if (!varint(it, &dy) || !varint(it, &nr)) return -1;
        it->rows_left--; it->row_y += dy; it->runs_left = nr; it->col_x = 0;
        if (it->row_y >= it->n) { it->err = 1; return -1; }
    }
    if (!varint(it, &gap) || !varint(it, &l)) return -1;
    l += 1;
    if (gap >= it->n || it->col_x + gap >= it->n || l > it->n - (it->col_x + gap)) { it->err = 1; return -1; }
    it->col_x += gap; *x0 = it->col_x; *y = it->row_y; *len = l; it->col_x += l; it->runs_left--;
    return 1;
}

int cc_places_cells_next(cc_places_cells *it, uint32_t *x, uint32_t *y) {
    if (it->mode == 1) {
        if (!it->cur_left) {
            int r = cc_places_cells_next_run(it, &it->cur_x, &it->cur_y, &it->cur_left);
            if (r <= 0) return r;
        }
        *x = it->cur_x++; *y = it->cur_y; it->cur_left--; return 1;
    }
    if (it->mode != 2) return 0;
    for (; it->cy <= it->y_hi; ++it->cy, it->cx = it->x_lo) {
        for (; it->cx <= it->x_hi; ++it->cx) {
            double dx = it->fx - (double)it->cx, dy;
            double ax = (double)it->cx - it->fx, bx = it->fx - ((double)it->cx + 1.0);
            double ay = (double)it->cy - it->fy, by = it->fy - ((double)it->cy + 1.0);
            dx = ax > bx ? ax : bx; if (dx < 0) dx = 0;
            dy = ay > by ? ay : by; if (dy < 0) dy = 0;
            if (dx * dx + dy * dy <= it->r2) {
                int64_t w = it->cx % (int64_t)it->n; if (w < 0) w += it->n;
                *x = (uint32_t)w; *y = it->cy; it->cx++; return 1;
            }
        }
    }
    return 0;
}

uint32_t cc_places_cells_count(cc_places *p, const cc_place_hit *hit, unsigned radius_km) {
    cc_places_cells it; uint32_t x, y, n = 0; int r;
    if (hit && hit->file <= CC_PLACES_MAX_SHARDS && p->file[hit->file].open && hit->id < p->file[hit->file].narea) {
        area_t a;
        return read_area(&p->file[hit->file], hit->id, &a) ? a.ncells : UINT32_MAX;
    }
    if (!cc_places_cells_open(p, hit, radius_km, &it)) return UINT32_MAX;
    while ((r = cc_places_cells_next(&it, &x, &y)) > 0) n++;
    return r < 0 ? UINT32_MAX : n;
}

int cc_places_cells_bytes(cc_places *p, const cc_place_hit *hit, unsigned radius_km,
                          uint32_t (*cell_bytes)(void *, uint32_t, uint32_t), void *ctx, uint64_t *total) {
    cc_places_cells it; uint32_t x, y; int r; uint64_t sum = 0;
    if (!cell_bytes || !cc_places_cells_open(p, hit, radius_km, &it)) return 0;
    while ((r = cc_places_cells_next(&it, &x, &y)) > 0) sum += cell_bytes(ctx, x, y);
    if (r < 0) return -1;
    *total = sum; return 1;
}
