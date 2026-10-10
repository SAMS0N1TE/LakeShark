#ifndef CC_CTILE_H
#define CC_CTILE_H
#include "core.h"
#define CC_CTILE_VERSION_MIN 2
#define CC_CTILE_VERSION_MAX 7
/* Structural 64-byte header check; directory/blob validation remains at open. */
int cc_ctile_header_check(const void *header,size_t header_bytes,uint64_t file_bytes);
typedef enum { CT_WATER=1, CT_RIVER, CT_STREAM, CT_CANAL, CT_WOOD, CT_PARK, CT_FARM,
 CT_RESIDENTIAL, CT_INDUSTRIAL, CT_BUILDING, CT_MOTORWAY, CT_TRUNK, CT_PRIMARY,
 CT_SECONDARY, CT_TERTIARY, CT_LOCAL, CT_SERVICE, CT_PATH, CT_RAIL, CT_BOUNDARY,
 CT_PLACE, CT_CLASS_COUNT } ct_class;
/* Terrain extension classes do not change the legacy 21-class tables. */
enum { CT_CONTOUR=22, CT_INDEX_CONTOUR=23 };
/* Exact pread: return nonzero only when all len bytes were read. */
typedef int (*cc_ctile_read)(void *ctx,uint64_t offset,void *buf,size_t len);
typedef struct {
    uint64_t generation;
    const void *generation_owner;
    uint8_t header[64];
    cc_ctile_read read; void *ctx;
    uint8_t *directory,*staging; size_t directory_capacity,staging_capacity;
    uint64_t file_size; uint32_t page_first,page_count,page_entries,max_blob;
    uint32_t version; int resident;
    size_t reads,bytes_read,directory_reads,blob_reads;
} cc_ctile_source;
typedef struct { cc_str bytes; uint32_t count; uint8_t zmin,zmax; cc_ctile_source *source; struct cc_cellset *cellset; } cc_ctile;
/* Header and directory validated at open; blobs validated on first decode.
   At least 24 directory bytes required. Smaller buffers use paged binary search.
   Source, buffers and immutable backing file must outlive all scenes/cache entries.
   One source/cache is single-threaded; serialize idle prefetch with rendering. */
int cc_ctile_open_source(cc_ctile *file,cc_ctile_source *source,cc_ctile_read read,void *ctx,
                        uint64_t file_size,void *directory,size_t directory_bytes,
                        void *staging,size_t staging_bytes);
/* 1 found, 0 absent, -1 read error. Does not read the blob. */
int cc_ctile_locate(const cc_ctile *file,unsigned z,uint32_t x,uint32_t y,uint64_t *offset,uint32_t *length);
/* Caller-owned variable-size blocks, LRU eviction by byte budget. Entries used
   in a scene are pinned until the next begin/scene call. No heap allocation.
   Do not share concurrently. Map bytes must stay immutable; reset before freeing
   or replacing them. The storage can live in PSRAM, independently of the arena. */
typedef struct {
    uint8_t *data; size_t size, used, peak;
    size_t hits, misses, evictions, bypasses;
    uint64_t clock, frame;
} cc_decoded_cache;
int cc_ctile_source_tile(const cc_ctile *file,uint64_t offset,uint32_t length,cc_arena *arena,cc_decoded_cache *cache,cc_str *tile);
typedef int (*cc_ctile_hint)(void *ctx,unsigned z,uint32_t x,uint32_t y,uint64_t offset,uint32_t length);
/* Emit tile requests for a predicted viewport plus one tile halo. The host can
   queue these for its idle task. No blob reads; paged lookup can read directory
   pages. Return nonzero from hint to continue. */
int cc_ctile_prefetch_hint(const cc_ctile *file,unsigned z,int64_t left,int64_t top,
                           int32_t cols,int32_t rows,int32_t dx,int32_t dy,
                           cc_ctile_hint hint,void *ctx);
/* Call in an idle task with a temporary arena. Loads the target viewport plus
   one tile halo, shifted by the predicted pan in dots. Keeps existing scene pins.
   Invalidate the renderer before its next frame; no concurrent cache access. */
int cc_ctile_prefetch(const cc_ctile *file,unsigned z,int64_t left,int64_t top,
                      int32_t cols,int32_t rows,int32_t dx,int32_t dy,
                      cc_arena *arena,cc_decoded_cache *cache);
int cc_decoded_cache_init(cc_decoded_cache *cache,void *storage,size_t bytes);
void cc_decoded_cache_reset(cc_decoded_cache *cache);
void cc_decoded_cache_begin(cc_decoded_cache *cache);
/* Returns tile-local CT3 for compact CT5; other validated formats are retained.
   Output borrows input or arena. Failure rolls back the arena. */
int cc_ctile_decode_tile(cc_str tile,cc_arena *arena,cc_str *decoded);
int cc_decoded_cache_get(cc_decoded_cache *cache,cc_str tile,cc_arena *arena,cc_str *decoded);
int cc_ctile_open(cc_str bytes, cc_ctile *file);
int cc_ctile_find(const cc_ctile *file, unsigned z, uint32_t x,uint32_t y,cc_str *tile);
/* With a callback source, find borrows staging until the next blob read.
   For retained scenes use scene_cached/source_tile instead. Never pass a staging
   slice to decoded_cache_get: use source_tile's stable source/offset key. */
/* Index zero is the empty name; returned strings borrow the mapped tile. */
/* Direct name lookup requires an uncompressed tile. Compact names are exposed
   by scene descriptors and live until their caller arena is reset. */
int cc_ctile_name(cc_str tile,uint32_t index,cc_str *name);
/* Integer world pixels at the selected zoom; no allocations except caller arena. */
int cc_ctile_scene(const cc_ctile *file,unsigned z,int64_t left,int64_t top,
                   int32_t cols,int32_t rows,cc_arena *arena,cc_scene *scene);
int cc_ctile_scene_cached(const cc_ctile *file,unsigned z,int64_t left,int64_t top,
                   int32_t cols,int32_t rows,cc_arena *arena,cc_scene *scene,cc_decoded_cache *cache);
/* Append one tile at a caller-selected dot origin. Initialize scene to zero
   before the first append; the encompassing scene API provides rollback. */
int cc_ctile_tile_scene(cc_str tile,int64_t ox,int64_t oy,cc_arena *arena,cc_scene *scene);
cc_style cc_ctile_style(unsigned class_id);
#endif
