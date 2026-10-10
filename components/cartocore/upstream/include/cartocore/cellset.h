#ifndef CC_CELLSET_H
#define CC_CELLSET_H
#include "ctile.h"
/* id 0 = overview.ctile; id 1+x*256+y = 8-x-y.ctile.
   Open: 1 success, 0 not installed, -1 I/O error. Read is exact pread.
   Index handle is separately caller-owned. At most four data handles are open.
   Immutable files/index must outlive the set; reopen + invalidate caches after
   atomic manifest activation. All callbacks and this object are single-owner. */
typedef struct {
    void *ctx;
    int (*open)(void *ctx,uint32_t id,void **handle,uint64_t *bytes);
    int (*read)(void *handle,uint64_t offset,void *dst,size_t bytes);
    void (*close)(void *handle);
} cc_cellset_io;
typedef struct cc_cellset {
    cc_ctile map; cc_ctile_source source;
    cc_cellset_io io; cc_ctile_read index_read; void *index_ctx;
    uint64_t index_bytes,clock;
    struct { void *handle; uint64_t bytes,age; uint32_t id; int used; } slots[4];
    size_t opens,evictions,fallbacks; int missing;
} cc_cellset;
int cc_cellset_open(cc_cellset *set,cc_ctile_read index_read,void *index_ctx,uint64_t index_bytes,
                    cc_cellset_io io,void *directory,size_t directory_bytes,void *staging,size_t staging_bytes);
void cc_cellset_close(cc_cellset *set);
/* 1 available, 0 not installed/empty, -1 I/O error; resolved offset is virtual. */
int cc_cellset_resolve(cc_cellset *set,unsigned z,uint32_t x,uint32_t y,uint64_t *offset,uint32_t *length);
int cc_cellset_scene(cc_cellset *set,unsigned z,int64_t left,int64_t top,int cols,int rows,
                     cc_arena *arena,cc_scene *scene,cc_decoded_cache *cache);
#endif
