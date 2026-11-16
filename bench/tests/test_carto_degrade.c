/* Per-tile degrade (MAP over dense Buenos Aires, 2026-11-15): a scene the arena
   cannot hold whole keeps the tiles that fit at full detail and draws the rest
   from a coarser zoom, clipped to their own squares, instead of failing the
   frame. Bad data still fails the frame. A decode left in the arena keeps only
   its result, and a CT5 group is written as CT3 straight into its CT6.
   argv: franklin_mini.ctile, the generated cells fixture directory. */
#include "../../components/cartocore/upstream/src/ingest/host_cellset.h"
#include "cartocore/render.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char arena_bytes[8u*1024u*1024u];
static unsigned read32(const uint8_t *p) { return p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16|(unsigned)p[3]<<24; }
static cc_str load(const char *path) {
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0);rewind(f);
    uint8_t *p=malloc((size_t)n);assert(p && fread(p,1,(size_t)n,f)==(size_t)n);fclose(f);return (cc_str){p,(size_t)n};
}
static unsigned char cache_bytes[1u<<20];
static cc_decoded_cache decoded;
typedef struct { int ok; size_t degraded,features,peak; } outcome;
static outcome scene(const cc_ctile *map,unsigned z,int64_t left,int64_t top,int cols,int rows,size_t bytes) {
    cc_arena a;cc_arena_init(&a,arena_bytes,bytes);cc_scene s;
    int ok=cc_ctile_scene_cached(map,z,left,top,cols,rows,&a,&s,&decoded);
    if(!ok) assert(!s.features && !s.degraded); /* a failed scene is rolled back whole */
    return (outcome){ok,s.degraded,s.features,a.peak};
}
/* Shrink the arena below what the view needs whole: the view must still draw,
   with some tiles at full detail and some coarser. The fixture's tiles are a
   few hundred bytes, so the zstd context (about 95 KB) would decide every
   outcome: the decoded cache is warmed with the view and its ancestors first,
   and the arena then holds the scene alone. */
static unsigned squeeze(const char *what,const cc_ctile *map,unsigned z,int64_t left,int64_t top,int cols,int rows) {
    assert(cc_decoded_cache_init(&decoded,cache_bytes,sizeof(cache_bytes)));
    for(unsigned k=z+1;k-->0;) assert(scene(map,k,left>>(z-k),top>>(z-k),cols,rows,sizeof(arena_bytes)).ok);
    outcome full=scene(map,z,left,top,cols,rows,sizeof(arena_bytes));
    assert(full.ok && !full.degraded && full.features);
    outcome exact=scene(map,z,left,top,cols,rows,full.peak);
    assert(exact.ok && !exact.degraded && exact.features==full.features);
    unsigned degraded=0,partial=0,lost=0;size_t smallest=full.peak;
    for(unsigned i=1;i<=96;i++) {
        size_t bytes=full.peak-full.peak*i/128;
        outcome o=scene(map,z,left,top,cols,rows,bytes);
        if(!o.ok) { lost++;continue; }
        assert(o.peak<=bytes);
        if(o.degraded) {
            degraded++;smallest=bytes;
            /* Full-detail tiles remain beside the degraded ones. */
            if(o.features && o.features<full.features) partial++;
        } else assert(o.features==full.features);
    }
    printf("%s z%u: whole %zu bytes, %zu features; degraded at %u of 96 smaller arenas (down to %zu bytes, %u with full tiles beside), lost %u\n",
           what,z,full.peak,full.features,degraded,smallest,partial,lost);
    assert(degraded && partial);
    return degraded;
}
/* CTZ: literal runs and back-copies (ctile.c's unpack), to get at a CT5. */
static size_t unpack(cc_str t,uint8_t *out) {
    size_t size=read32(t.data+4),at=0;const uint8_t *p=t.data+8,*end=t.data+t.size;
    while(p<end) {
        unsigned k=*p++;
        if(k<128) { memcpy(out+at,p,k+1u);p+=k+1u;at+=k+1u;continue; }
        size_t m=(k&127)+3,d=p[0]|(size_t)p[1]<<8;p+=2;
        for(size_t j=0;j<m;j++) out[at+j]=out[at+j-d];
        at+=m;
    }
    assert(at==size);return size;
}
static void decode_in_place(const cc_ctile *map) {
    cc_str t={0};
    for(unsigned i=0;i<map->count;i++) {
        const uint8_t *p=map->bytes.data+64+i*24;
        if(p[0]==15 && (!t.data || read32(p+20)>t.size)) t=(cc_str){map->bytes.data+read32(p+12),read32(p+20)};
    }
    assert(t.data && !memcmp(t.data,"CTZ\0",4));
    uint8_t *ct5=malloc(read32(t.data+4));assert(ct5);
    size_t n5=unpack(t,ct5);assert(!memcmp(ct5,"CT5\0",4));
    cc_arena a;cc_arena_init(&a,arena_bytes,sizeof(arena_bytes));
    cc_str ct3,whole;assert(cc_ctile_decode_tile((cc_str){ct5,n5},&a,&ct3) && !memcmp(ct3.data,"CT3\0",4));
    /* The same CT5 as the group of a CT6 (no contours, no relief), as v7 cells carry it. */
    static const uint8_t flat[8]={'S','H','1',0,1,2,2,0};
    uint8_t *ct6=malloc(16+n5+8);assert(ct6);
    memcpy(ct6,"CT6\0",4);
    for(unsigned k=0;k<4;k++) { ct6[4+k]=(uint8_t)(n5>>(8*k));ct6[8+k]=0;ct6[12+k]=(uint8_t)(8u>>(8*k)); }
    memcpy(ct6+16,ct5,n5);memcpy(ct6+16+n5,flat,8);
    size_t before=a.used;
    assert(cc_ctile_decode_tile((cc_str){ct6,16+n5+8},&a,&whole));
    assert(whole.size==16+ct3.size+8 && read32(whole.data+4)==ct3.size);
    assert(!memcmp(whole.data+16,ct3.data,ct3.size) && !memcmp(whole.data+16+ct3.size,flat,8));
    assert(a.used-before<whole.size+8); /* the CT3 went straight into the CT6 */
    /* Bypassing the decoded cache, the arena keeps the result alone, not the
       unpacked CT5 it came from. */
    for(size_t mark=0;mark<8;mark++) {
        cc_arena b;cc_arena_init(&b,arena_bytes+sizeof(arena_bytes)/2,sizeof(arena_bytes)/2);b.used=mark;
        cc_str got;assert(cc_decoded_cache_get(NULL,t,&b,&got));
        assert(got.size==ct3.size && !memcmp(got.data,ct3.data,ct3.size));
        assert(got.data>=b.data+mark && b.used-mark<ct3.size+8 && got.data+got.size==b.data+b.used);
        assert(!((uintptr_t)got.data&7) && b.peak>=mark+n5+ct3.size);
    }
    free(ct5);free(ct6);
}
static void bad_data_fails(const char *root) {
    char path[1400];snprintf(path,sizeof(path),"%s/mono.ctile",root);cc_str mono=load(path);
    uint8_t *bytes=(uint8_t*)mono.data;cc_ctile map;assert(cc_ctile_open(mono,&map));
    unsigned x=0,y=0;int corrupted=0;
    for(unsigned i=0;i<map.count && !corrupted;i++) {
        const uint8_t *p=bytes+64+i*24;
        if(p[0]!=14) continue;
        x=read32(p+4);y=read32(p+8);bytes[read32(p+12)+read32(p+20)/2]^=0x5a;corrupted=1;
    }
    assert(corrupted);
    /* Ample arena: the frame fails, as it did before; it is not drawn coarser. */
    outcome o=scene(&map,14,(int64_t)x*256+40,(int64_t)y*256+40,40,20,sizeof(arena_bytes));
    assert(!o.ok);
    free(bytes);
}
int main(int argc,char **argv) {
    assert(argc==3);setvbuf(stdout,NULL,_IONBF,0);
    cc_str mini=load(argv[1]);cc_ctile map;assert(cc_ctile_open(mini,&map));
    decode_in_place(&map);
    puts("CT3 written into its CT6 equals the group decoded alone; a bypassed decode keeps its result only");
    /* The fixture's z14 tiles each hold one road across rows 100..128; the
       view straddles two of them. Its pyramid skips z9..z13, so a degraded
       tile comes from z8. A single in-memory file (as the NH archives are)
       and the cells: the native loop at z14, the scaled-up loops at z16. */
    char path[1400];snprintf(path,sizeof(path),"%s/mono.ctile",argv[2]);
    cc_str mono=load(path);cc_ctile single;assert(cc_ctile_open(mono,&single));
    cc_host_cellset h;assert(cc_host_cellset_open(&h,argv[2]));
    for(unsigned z=14;z<=16;z+=2) {
        int64_t cx=(INT64_C(77)*256<<z)/256,cy=(INT64_C(93)*256<<z)/256+(INT64_C(100)<<(z-14))-20;
        squeeze("single file",&single,z,cx-80,cy,80,30);
        squeeze("cells",&h.set.map,z,cx-80,cy,80,30);
    }
    cc_host_cellset_close(&h);free((void*)mono.data);
    bad_data_fails(argv[2]);
    puts("bad tile data still fails the frame");
    free((void*)mini.data);
    puts("per-tile degrade: passed");
    return 0;
}
