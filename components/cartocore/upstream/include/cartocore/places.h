#ifndef CC_PLACES_H
#define CC_PLACES_H
/* Offline place index (countries, states, cities) read through the same
   exact-pread callback as ctile streaming. Format and limits: docs/places.md.
   No heap, no OS calls, no globals: every byte of state is in the caller-owned
   structs below plus the caller's result arrays. Single-threaded per cc_places. */
#include "ctile.h"
#include <stddef.h>
#include <stdint.h>

#define CC_PLACES_PAGE 512            /* bytes per page cache (3 per open file) */
#define CC_PLACES_MAX_SHARDS 3        /* country shards attached next to the base */
#define CC_PLACES_MAX_HINTS 4         /* shard suggestions per search */
#define CC_PLACES_KEY_MAX 63          /* folded query/key bytes, longer input is cut */
#define CC_PLACES_MAX_SCAN 16384      /* key entries scanned per range (bounds latency) */
#define CC_PLACES_NONE 0xFFFFFFFFu

typedef cc_ctile_read cc_places_read;

typedef struct {
    cc_places_read read; void *ctx; uint64_t size;
    uint32_t nkeys, narea, ncity, keys, pool, pool_len, area, city, cells, cells_len, shard, nshard, bloom;
    uint32_t credit_off, credit_len;
    uint8_t type, zoom, open; char iso2[3];
    struct { uint32_t off, len; uint8_t buf[CC_PLACES_PAGE]; } kc, pc, rc; /* keys, pool, records */
    struct { char q[CC_PLACES_KEY_MAX + 1]; uint8_t ql; uint32_t lo, hi; } memo; /* last phrase range */
    size_t reads, bytes_read;
} cc_places_file;

typedef struct { cc_places_file file[1+CC_PLACES_MAX_SHARDS]; } cc_places;

enum { CC_PLACE_COUNTRY = 1, CC_PLACE_STATE = 2, CC_PLACE_CITY = 3 };
typedef struct {
    uint32_t id;       /* record id inside file `file` (areas first, then cities) */
    uint32_t score;    /* match tier << 16 | popularity rank; higher first */
    uint8_t file;      /* 0 = base, 1.. = attached shard slot */
    uint8_t kind;      /* CC_PLACE_* */
} cc_place_hit;

typedef struct {
    size_t scanned;        /* key entries examined by the last search */
    int truncated;         /* a range exceeded CC_PLACES_MAX_SCAN (short query) */
    unsigned nhints;       /* country area ids whose city shard is not attached but would
                              add results for the qualifier in the query ("nh" -> several
                              countries), best first. Use cc_places_shard_info on each. */
    uint32_t shard_area[CC_PLACES_MAX_HINTS];
    size_t reads, bytes;   /* reader callback calls and bytes during the call */
} cc_places_stats;

typedef struct {
    char name[64], parent[64], country[64];   /* UTF-8, truncated to fit */
    char iso2[3];                              /* countries */
    uint8_t kind; uint32_t pop;
    int32_t lat_e5, lon_e5;                    /* degrees * 1e5 (centre / label point) */
    int32_t bbox_e5[4];                        /* minlat minlon maxlat maxlon, areas only */
    uint32_t ncells;                           /* areas: cells at the file zoom */
    uint32_t area_id;                          /* areas: id usable with shard_info */
} cc_place_info;

typedef struct { char iso2[3]; uint32_t ncities, size, crc32; uint8_t sha256[32]; } cc_places_shard;

/* Check header CRC and whole-body CRC32 of a file (streams through buf, any size
   >= 16). Returns 1 when intact. Use after download, not on every open. */
int cc_places_verify(cc_places_read read, void *ctx, uint64_t size, void *buf, size_t buf_bytes);
/* Open the base file (header validated, no body reads beyond it). */
int cc_places_open(cc_places *p, cc_places_read read, void *ctx, uint64_t size);
/* Attach a country shard; returns its slot (>= 1) or 0. One shard per country. */
int cc_places_attach(cc_places *p, cc_places_read read, void *ctx, uint64_t size);
void cc_places_detach(cc_places *p, int slot);
int cc_places_shard_attached(const cc_places *p, const char iso2[2]);
unsigned cc_places_zoom(const cc_places *p);                    /* grid zoom G of the base */
size_t cc_places_credit(const cc_places *p, char *buf, size_t n); /* attribution text */
/* Shard directory entry for a country area id: 1 found, 0 none, -1 error. */
int cc_places_shard_info(cc_places *p, uint32_t country_area, cc_places_shard *out);

/* Prefix search over base + attached shards, folded (case, diacritics). Accepts
   multi-token queries: "concord nh", "concord, new hampshire", "new ham". Returns
   the number of hits written (best first, at most max_out) or -1 on read error. */
int cc_places_search(cc_places *p, const char *query, cc_place_hit *out, unsigned max_out, cc_places_stats *st);
/* 0 success, -1 invalid hit or read error. */
int cc_places_info(cc_places *p, const cc_place_hit *hit, cc_place_info *info);

/* Cells covering a hit: areas use the stored shape cover, cities every cell within
   radius_km (1..500) of the centre. Order is (y, x) ascending; city x wraps at
   the antimeridian. */
typedef struct {
    cc_places_file *f; uint8_t mode, err;               /* mode 1 area stream, 2 city box */
    uint32_t pos, end, rows_left, runs_left, row_y, col_x;  /* area stream decoder */
    uint32_t cur_x, cur_y, cur_left;                         /* run being emitted cell by cell */
    uint32_t n, cy, y_hi; int32_t cx, x_lo, x_hi;            /* city box */
    double fx, fy, r2;
    uint8_t buf[48]; uint8_t bi, bn;
} cc_places_cells;
int cc_places_cells_open(cc_places *p, const cc_place_hit *hit, unsigned radius_km, cc_places_cells *it);
/* 1 cell written, 0 end, -1 error */
int cc_places_cells_next(cc_places_cells *it, uint32_t *x, uint32_t *y);
/* Areas only: whole row runs (x0, len) at row y. 1 / 0 end / -1 error / -2 city. */
int cc_places_cells_next_run(cc_places_cells *it, uint32_t *x0, uint32_t *y, uint32_t *len);
/* Exact cell count, or UINT32_MAX on error. Areas: stored count. */
uint32_t cc_places_cells_count(cc_places *p, const cc_place_hit *hit, unsigned radius_km);
/* Sum cell_bytes(ctx, x, y) over the cells (download size); 1 ok, 0 invalid, -1 error. */
int cc_places_cells_bytes(cc_places *p, const cc_place_hit *hit, unsigned radius_km,
                          uint32_t (*cell_bytes)(void *ctx, uint32_t x, uint32_t y), void *ctx,
                          uint64_t *total);

#endif
