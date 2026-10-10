#include "cartocore/ctile.h"
#include "cartocore/zstd.h"
#include <string.h>
/* Blocks never move: previously appended scene paths can borrow their bytes.
   Free neighbors coalesce; oversized/full-pinned frames fall back to arena. */
typedef struct {
    size_t span, length, source_size;
    const uint8_t *source;
    uint64_t age, pin, offset;
    uint64_t generation; const void *generation_owner;
} block;
#define ALIGN _Alignof(block)
static size_t aligned(size_t n) { return (n+ALIGN-1)&~(size_t)(ALIGN-1); }
int cc_decoded_cache_init(cc_decoded_cache *c,void *storage,size_t bytes) {
    memset(c,0,sizeof(*c));
    if(!storage) return 0;
    size_t pad=(size_t)(0-(uintptr_t)storage)&(ALIGN-1);
    if(pad>bytes || bytes-pad<aligned(sizeof(block))+ALIGN) return 0;
    c->data=(uint8_t*)storage+pad; c->size=(bytes-pad)&~(size_t)(ALIGN-1);
    cc_decoded_cache_reset(c); return 1;
}
void cc_decoded_cache_reset(cc_decoded_cache *c) {
    if(c->data && c->size) { block *b=(block*)c->data; memset(b,0,sizeof(*b)); b->span=c->size; }
    c->used=c->peak=c->hits=c->misses=c->evictions=c->bypasses=0; c->clock=0; c->frame=1;
}
void cc_decoded_cache_begin(cc_decoded_cache *c) {
    if(++c->frame==0) { for(size_t at=0;at<c->size;) { block *b=(block*)(c->data+at); b->pin=0; at+=b->span; } c->frame=1; }
}
static block *space(cc_decoded_cache *c,size_t need) {
    for(;;) {
        block *old=0;
        for(size_t at=0;at<c->size;) {
            block *b=(block*)(c->data+at);
            if(!b->source) {
                while(at+b->span<c->size) { block *next=(block*)(c->data+at+b->span); if(next->source) break; b->span+=next->span; }
                if(b->span>=need) return b;
            } else if(b->pin!=c->frame && (!old || b->age<old->age)) old=b;
            at+=b->span;
        }
        if(!old) return 0;
        c->used-=old->span; c->evictions++; old->source=0;
    }
}
static int zstd_read(void *opaque,uint64_t off,void *buf,size_t n) {
    cc_ctile_source *s=opaque;
    if(!s->read(s->ctx,off,buf,n)) return 0;
    s->reads++;s->blob_reads++;s->bytes_read+=n;return 1;
}
static int get(cc_decoded_cache *c,cc_str tile,cc_arena *a,cc_str *out,const cc_ctile *file,uint64_t offset) {
    const uint8_t *key=file?(const uint8_t*)file->source:tile.data;
    size_t source_size=tile.size;
    if(!c || !c->data) goto decode;
    /* Legacy wire tiles keep the existing bounded scene decoder. */
    if(!file && (tile.size<4 || (memcmp(tile.data,"CT3\0",4) && memcmp(tile.data,"CT4\0",4) &&
       memcmp(tile.data,"CT5\0",4) && memcmp(tile.data,"CT6\0",4) && memcmp(tile.data,"CZ7\0",4) && memcmp(tile.data,"CTZ\0",4)))) {
        *out=tile; c->bypasses++; return 1;
    }
    for(size_t at=0;at<c->size;) {
        block *b=(block*)(c->data+at);
        if(b->source==key && b->source_size==source_size && b->offset==offset &&
           (!file || (b->generation==file->source->generation && b->generation_owner==file->source->generation_owner))) {
            b->age=++c->clock; b->pin=c->frame; c->hits++;
            *out=(cc_str){(const uint8_t*)b+aligned(sizeof(*b)),b->length}; return 1;
        }
        at+=b->span;
    }
    c->misses++;
decode:;
    size_t mark=a->used;
    if(file && file->source->version==7) {
        if(!cc_zstd_decode_read(zstd_read,file->source,offset,tile.size,a,&tile) || tile.size<16 || memcmp(tile.data,"CT6\0",4)) { a->used=mark; return 0; }
    } else if(file) {
        cc_ctile_source *src=file->source;
        if(tile.size>src->staging_capacity || !src->read(src->ctx,offset,src->staging,tile.size)) return 0;
        src->reads++; src->blob_reads++; src->bytes_read+=tile.size;
        tile.data=src->staging;
        if(tile.size>=4 && !memcmp(tile.data,"CZ7\0",4)) return 0; /* v7 envelope requires v7 outer header */
    }
    if(file && file->source->version==2 && (tile.size<4 || (memcmp(tile.data,"CT3\0",4) &&
       memcmp(tile.data,"CT4\0",4) && memcmp(tile.data,"CT5\0",4) && memcmp(tile.data,"CT6\0",4) && memcmp(tile.data,"CZ7\0",4) && memcmp(tile.data,"CTZ\0",4)))) *out=tile;
    else if(!cc_ctile_decode_tile(tile,a,out)) { a->used=mark; return 0; }
    /* Staging is overwritten by the next blob. Retain borrowed bytes even on
       cache bypass; decoded arena bytes already have the correct lifetime. */
    if(file && file->source->version!=7 && out->data==tile.data) {
        uint8_t *copy=cc_arena_alloc(a,out->size,1);
        if(!copy) { a->used=mark; return 0; }
        memcpy(copy,out->data,out->size); out->data=copy;
    }
    if(!c || !c->data) return 1;
    size_t header=aligned(sizeof(block));
    if(out->size>c->size-header) { c->bypasses++; return 1; }
    size_t need=header+aligned(out->size); block *b=space(c,need);
    if(!b) { c->bypasses++; return 1; }
    if(b->span-need>=header+ALIGN) {
        block *next=(block*)((uint8_t*)b+need); memset(next,0,sizeof(*next)); next->span=b->span-need; b->span=need;
    }
    b->source=key; b->source_size=source_size; b->offset=offset; b->length=out->size;
    b->generation=file?file->source->generation:0;
    b->generation_owner=file?file->source->generation_owner:0;
    b->age=++c->clock; b->pin=c->frame;
    memcpy((uint8_t*)b+header,out->data,out->size);
    *out=(cc_str){(uint8_t*)b+header,b->length};
    c->used+=b->span; if(c->used>c->peak) c->peak=c->used;
    a->used=mark; return 1;
}

int cc_decoded_cache_get(cc_decoded_cache *c,cc_str tile,cc_arena *a,cc_str *out) {
    return get(c,tile,a,out,0,0);
}
int cc_ctile_source_tile(const cc_ctile *f,uint64_t offset,uint32_t length,cc_arena *a,cc_decoded_cache *c,cc_str *out) {
    if(!f->source || offset<64+(uint64_t)f->count*24 || offset>f->source->file_size || length>f->source->file_size-offset) return 0;
    return get(c,(cc_str){0,length},a,out,f,offset);
}
