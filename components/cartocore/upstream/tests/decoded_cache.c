#include "cartocore/render.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char arena_bytes[16000000],cache_bytes[4000000];
static cc_cell cells[240*67],expected[240*67];
static unsigned read32(const uint8_t *p) { return p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16|(unsigned)p[3]<<24; }
static unsigned char hot_bytes[200000];
int main(int argc,char **argv) {
    assert(argc==2); FILE *f=fopen(argv[1],"rb"); assert(f);
    fseek(f,0,SEEK_END); size_t size=(size_t)ftell(f); rewind(f);
    unsigned char *bytes=malloc(size); assert(bytes && fread(bytes,1,size,f)==size); fclose(f);
    cc_ctile map; assert(cc_ctile_open((cc_str){bytes,size},&map));
    cc_decoded_cache c; assert(!cc_decoded_cache_init(&c,cache_bytes,1));
    assert(cc_decoded_cache_init(&c,cache_bytes+1,sizeof(cache_bytes)-1));
    cc_arena a; cc_arena_init(&a,arena_bytes,sizeof(arena_bytes));
    /* Borrowed entry remains stable when a small budget fills during a scene;
       oversized entries fall back, rather than evicting a pinned tile. */
    cc_str tiles[3],decoded[3]; size_t n=0;
    for(unsigned i=0;i<map.count && n<3;i++) {
        const uint8_t *p=bytes+64+i*24;
        if(p[0]==14 && read32(p+20)>1000) assert(cc_ctile_find(&map,14,read32(p+4),read32(p+8),&tiles[n++]));
    }
    assert(n==3); size_t lengths[3];
    for(size_t i=0;i<3;i++) { a.used=0; assert(cc_ctile_decode_tile(tiles[i],&a,&decoded[i])); lengths[i]=decoded[i].size; }
    /* Budget can hold any one of these tiles, but not all three. */
    size_t max=lengths[0]; for(size_t i=1;i<3;i++) if(lengths[i]>max) max=lengths[i];
    assert(cc_decoded_cache_init(&c,cache_bytes,max+128)); cc_decoded_cache_begin(&c);
    a.used=0; assert(cc_decoded_cache_get(&c,tiles[0],&a,&decoded[0])); assert(a.used==0);
    const uint8_t *borrow=decoded[0].data; unsigned char *copy=malloc(lengths[0]); assert(copy); memcpy(copy,borrow,lengths[0]);
    for(size_t i=1;i<3;i++) assert(cc_decoded_cache_get(&c,tiles[i],&a,&decoded[i]));
    assert(!memcmp(copy,borrow,lengths[0])); assert(c.bypasses); free(copy);
    cc_decoded_cache_begin(&c); a.used=0;
    assert(cc_decoded_cache_get(&c,tiles[2],&a,&decoded[2])); assert(c.evictions && a.used==0);
    size_t misses=c.misses; assert(cc_decoded_cache_get(&c,tiles[2],&a,&decoded[2])); assert(c.misses==misses && c.hits==1);
    /* Failed decoding and an exhausted arena do not install entries. */
    unsigned char corrupt[8]={'C','T','Z',0,100,0,0,0}; size_t mark=a.used,used=c.used;
    assert(!cc_decoded_cache_get(&c,(cc_str){corrupt,sizeof(corrupt)},&a,&decoded[0])); assert(a.used==mark && c.used==used);
    cc_decoded_cache_reset(&c); cc_arena tiny; cc_arena_init(&tiny,arena_bytes,1);
    assert(!cc_decoded_cache_get(&c,tiles[0],&tiny,&decoded[0])); assert(!tiny.used && !c.used);
    size_t rank[3]={0,1,2};
    for(size_t i=0;i<3;i++) for(size_t j=i+1;j<3;j++) if(lengths[rank[i]]>lengths[rank[j]]) { size_t swap=rank[i]; rank[i]=rank[j]; rank[j]=swap; }
    assert(cc_decoded_cache_init(&c,cache_bytes,lengths[rank[0]]+lengths[rank[2]]+128));
    a.used=0; assert(cc_decoded_cache_get(&c,tiles[rank[2]],&a,&decoded[2]));
    assert(cc_decoded_cache_get(&c,tiles[rank[0]],&a,&decoded[0]));
    cc_decoded_cache_begin(&c); assert(cc_decoded_cache_get(&c,tiles[rank[0]],&a,&decoded[0]));
    cc_decoded_cache_begin(&c); assert(cc_decoded_cache_get(&c,tiles[rank[1]],&a,&decoded[1]));
    misses=c.misses; assert(cc_decoded_cache_get(&c,tiles[rank[0]],&a,&decoded[0])); assert(c.misses==misses && c.evictions==1);
    /* Equal packets at different immutable addresses are separate identities. */
    assert(cc_decoded_cache_init(&c,cache_bytes,sizeof(cache_bytes))); a.used=0;
    unsigned char *clone=malloc(tiles[0].size); assert(clone); memcpy(clone,tiles[0].data,tiles[0].size);
    assert(cc_decoded_cache_get(&c,tiles[0],&a,&decoded[0]));
    assert(cc_decoded_cache_get(&c,(cc_str){clone,tiles[0].size},&a,&decoded[1]));
    assert(c.misses==2 && !memcmp(decoded[0].data,decoded[1].data,decoded[0].size));
    cc_decoded_cache_reset(&c); free(clone);
    /* Actual pans cross tile boundaries, with mode, label, edge, zoom and map
       identity changes. Compare full output against uncached fresh rendering. */
    for(int budget=0;budget<3;budget++) {
        assert(cc_decoded_cache_init(&c,cache_bytes,budget==0?512:budget==1?80000:sizeof(cache_bytes)));
        for(int z=13;z<=15;z++) for(int i=0;i<64;i++) {
            int64_t left=(INT64_C(1262080)<<(z-13))/2+i*10,top=(INT64_C(1533952)<<(z-13))/2+(i%8);
            cc_mode mode=(cc_mode)(i%4); int labels=i%3!=0;
            cc_edges edges=i&1?CC_EDGES_CRISP:CC_EDGES_SMOOTH;
            cc_renderer r; cc_arena_init(&a,arena_bytes,sizeof(arena_bytes)); assert(cc_renderer_init(&r,&a,240,67,expected)); r.edges_mode=edges;
            assert(cc_render(&r,&map,z,left,top,mode,labels,expected));
            cc_arena h; cc_arena_init(&h,hot_bytes,sizeof(hot_bytes));
            cc_arena_init(&a,arena_bytes,sizeof(arena_bytes)); assert(cc_renderer_init_split(&r,&h,&a,240,67,cells)); r.edges_mode=edges; r.decoded_cache=&c;
            assert(cc_render(&r,&map,z,left,top,mode,labels,cells)); assert(!memcmp(cells,expected,sizeof(cells)));
            size_t hits=c.hits,miss=c.misses,used=a.used;
            cc_renderer_reraster(&r);
            assert(cc_render(&r,&map,z,left,top,mode,labels,cells));
            assert(!memcmp(cells,expected,sizeof(cells)) && c.hits==hits && c.misses==miss && a.used==used);
            cc_renderer_invalidate(&r);
            assert(cc_render(&r,&map,z,left,top,mode,labels,cells)); assert(!memcmp(cells,expected,sizeof(cells)));
            if(budget==2) { assert(c.hits>=hits && c.misses==miss); if(r.scene.features) assert(c.hits>hits); }
            assert(c.used<=c.size);
        }
        if(budget==1) assert(c.evictions);
    }
    cc_arena h; cc_renderer r; cc_arena_init(&h,hot_bytes,cc_renderer_hot_bytes(52,70));
    cc_arena_init(&a,arena_bytes,sizeof(arena_bytes));
    assert(cc_renderer_init_split(&r,&h,&a,52,70,cells)); assert(h.used<=24576);
    cc_arena_init(&h,hot_bytes,1); size_t mark2=a.used;
    assert(!cc_renderer_init_split(&r,&h,&a,52,70,cells) && !h.used && a.used==mark2);
    free(bytes); puts("decoded cache: pinned lifetime, byte budgets, eviction, rollback, pans and warm output passed"); return 0;
}
