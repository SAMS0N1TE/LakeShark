/* Small reads of one cell file, served from a few cached 4 KiB blocks.

   A downloaded cell is found by binary search over its directory on the
   card: its 64-byte header, then one 24-byte entry per step, 17 steps in a
   detail cell. Each was a card read of its own, about twenty for every tile
   looked up, and the tiles of one view walk nearly the same steps. Kept by
   block, the steps they share are read once.

   Single owner (the map worker). Reads longer than LS_CARTO_BLOCK_SMALL, or
   across a block edge, go straight to the file. */
#ifndef LS_CARTO_BLOCK_CACHE_H
#define LS_CARTO_BLOCK_CACHE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define LS_CARTO_BLOCK_BYTES 4096u
#define LS_CARTO_BLOCK_SLOTS 12u
#define LS_CARTO_BLOCK_SMALL 256u
typedef int (*ls_carto_block_read)(void *ctx,uint64_t offset,void *dst,size_t bytes);
typedef struct {
    uint8_t *memory;                       /* LS_CARTO_BLOCK_SLOTS blocks, or NULL: no cache */
    uint32_t block[LS_CARTO_BLOCK_SLOTS];  /* block number + 1, 0 empty */
    uint32_t age[LS_CARTO_BLOCK_SLOTS],clock;
} ls_carto_block_cache;
/* 1 read, 0 failed (as the reader itself answers). size is the file's. */
static inline int ls_carto_block_cached(ls_carto_block_cache *c,ls_carto_block_read read,void *ctx,uint64_t size,
                                 uint64_t offset,void *dst,size_t bytes) {
    if(offset>size || bytes>size-offset) return 0;
    const uint64_t number=offset/LS_CARTO_BLOCK_BYTES;
    if(!c->memory || !bytes || bytes>LS_CARTO_BLOCK_SMALL || number>=UINT32_MAX ||
       (offset+bytes-1)/LS_CARTO_BLOCK_BYTES!=number) return read(ctx,offset,dst,bytes);
    unsigned slot=0;
    for(unsigned i=0;i<LS_CARTO_BLOCK_SLOTS;i++) {
        if(c->block[i]==(uint32_t)number+1) { slot=i;goto found; }
        if(c->age[i]<c->age[slot]) slot=i;
    }
    {
        const uint64_t first=number*LS_CARTO_BLOCK_BYTES;
        const size_t n=size-first<LS_CARTO_BLOCK_BYTES?(size_t)(size-first):LS_CARTO_BLOCK_BYTES;
        c->block[slot]=0;
        if(!read(ctx,first,c->memory+(size_t)slot*LS_CARTO_BLOCK_BYTES,n)) return 0;
        c->block[slot]=(uint32_t)number+1;
    }
found:
    c->age[slot]=++c->clock;
    memcpy(dst,c->memory+(size_t)slot*LS_CARTO_BLOCK_BYTES+(size_t)(offset%LS_CARTO_BLOCK_BYTES),bytes);
    return 1;
}
#endif
