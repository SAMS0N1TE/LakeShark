#include "../src/ingest/host_cellset.h"
#include "cartocore/render.h"
#include "cartocore/cache.h"
#include "cartocore/zstd.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/core/cellset_clip.h"
static unsigned char memory[32*1024*1024];
static void clipped_overview_edge(void) {
    cc_arena a;cc_arena_init(&a,memory,sizeof(memory));
    cc_point points[]={{-10,-10},{266,-10},{266,266},{-10,266}};
    cc_path path={.points=points,.count=4,.closed=1};
    cc_feature feature={.paths=&path,.count=1};cc_scene scene={0};scene.first[0]=scene.last[0]=&feature;
    assert(clip_scene(&scene,0,0,128,128,&a));assert(cc_scene_overzoom(&scene,5,0,0,&a));
    int32_t right=0,bottom=0;
    for(size_t i=0;i<feature.paths[0].count;i++) { cc_point p=cc_feature_point(&feature,feature.paths,i);
        if(p.x>right)right=p.x;
        if(p.y>bottom)bottom=p.y;
    }
    cc_scene merged={0};merge_scene(&merged,&scene);assert(merged.first[0]==&feature);
    assert(right==4096 && bottom==4096); /* no amplified one-source-pixel seam */
}
static unsigned char cache_memory[1024*1024];
static cc_decoded_cache decoded;
static cc_str load(const char *root,const char *name) {
    char path[1400]; snprintf(path,sizeof(path),"%s/%s",root,name);FILE *f=fopen(path,"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0);rewind(f);uint8_t *p=malloc((size_t)n);assert(p && fread(p,1,(size_t)n,f)==(size_t)n);fclose(f);return (cc_str){p,(size_t)n};
}
static int labels=1;
static size_t render(const cc_ctile *map,unsigned z,int64_t left,int64_t top,cc_mode mode,cc_cell *cells) {
    cc_arena a;cc_arena_init(&a,memory,sizeof(memory));cc_renderer r;cc_cell saved[80*30];
    assert(cc_renderer_init(&r,&a,80,30,saved));r.terrain_strength=80;r.decoded_cache=&decoded;
    assert(cc_render(&r,map,z,left,top,mode,labels,cells));return a.peak;
}
/* Raster-only pans must be exact through v7 virtual cells, source-boundary
   clipping, overview fallback and overzoom, as well as a monolithic file. */
static void pan_pixels(const cc_ctile *map) {
    static unsigned char reference_memory[32*1024*1024];
    static cc_cell saved[2][80*30];static uint16_t picture[2][160*120],previous[160*120];
    const int moves[][2]={{0,0},{10,8},{-20,-16},{2,4},{1,3},{-40,0},{0,64}};
    cc_pixel_style style={0};for(int i=0;i<CC_INK_COUNT;i++)style.rgb[i]=cc_palette(i);style.river=0x80a0ff;style.relief=80;
    for(unsigned z=7;z<=16;z+=(z==7?1:z==8?6:1)) {
        cc_arena a[2];cc_renderer r[2];
        cc_arena_init(&a[0],memory,sizeof(memory));cc_arena_init(&a[1],reference_memory,sizeof(reference_memory));
        for(int k=0;k<2;k++){assert(cc_renderer_init(&r[k],&a[k],80,30,saved[k]));r[k].terrain_strength=80;}
        int64_t left=(INT64_C(77)*256<<z)/256-80,top=(INT64_C(93)*256<<z)/256-60;
        for(unsigned j=0;j<sizeof(moves)/sizeof(*moves);j++) {
            left+=moves[j][0];top+=moves[j][1];
            for(int k=0;k<2;k++) {
                if(k)cc_renderer_invalidate(&r[k]);
                assert(cc_render(&r[k],map,z,left,top,CC_QUADRANT,0,NULL));
                if(!k && r[k].pixel_reuse)cc_pixels_rgb565_pan(&r[k].planes,&style,picture[k],previous,r[k].saved,r[k].pixel_dx,r[k].pixel_dy);
                else cc_pixels_rgb565(&r[k].planes,&style,picture[k]);
            }
            assert(!memcmp(picture[0],picture[1],sizeof(picture[0])));memcpy(previous,picture[0],sizeof(previous));
        }
    }
}
int main(int argc,char **argv) {
    clipped_overview_edge();
    assert(cc_decoded_cache_init(&decoded,cache_memory,sizeof(cache_memory)));
    assert(argc==2);cc_str blob=load(argv[1],"tile.cz7"),raw=load(argv[1],"tile.ct6"),bad=load(argv[1],"window.cz7"),mono=load(argv[1],"mono.ctile");
    cc_arena a;cc_str out;cc_arena_init(&a,memory,sizeof(memory));assert(cc_zstd_decode(blob,&a,&out));assert(out.size==raw.size && !memcmp(out.data,raw.data,raw.size));
    size_t peak=a.peak;assert(cc_zstd_context_bytes()<128*1024);
    for(size_t n=0;n<blob.size;n++) { a.used=0;assert(!cc_zstd_decode((cc_str){blob.data,n},&a,&out));assert(a.used==0); }
    a.used=0;assert(!cc_zstd_decode(bad,&a,&out));
    uint8_t *corrupt=malloc(blob.size+1);assert(corrupt);memcpy(corrupt,blob.data,blob.size);corrupt[blob.size-1]^=1;
    assert(!cc_zstd_decode((cc_str){corrupt,blob.size},&a,&out));assert(a.used==0);
    memcpy(corrupt,blob.data,blob.size);corrupt[blob.size]=0;assert(!cc_zstd_decode((cc_str){corrupt,blob.size+1},&a,&out));
    cc_arena small;cc_arena_init(&small,memory,64);assert(!cc_zstd_decode(blob,&small,&out));assert(small.used==0);
    clock_t start=clock();for(int i=0;i<10000;i++) { a.used=0;assert(cc_zstd_decode(blob,&a,&out)); }
    printf("zstd static context=%zu scratch_peak=%zu mean_decode_us=%.3f\n",cc_zstd_context_bytes(),peak,1e6*(double)(clock()-start)/CLOCKS_PER_SEC/10000);
    cc_ctile m;((uint8_t*)mono.data)[8]=6;assert(!cc_ctile_open(mono,&m));((uint8_t*)mono.data)[8]=7;
    assert(cc_ctile_open(mono,&m));cc_host_cellset h;assert(cc_host_cellset_open(&h,argv[1]));cc_cell expected[80*30],actual[80*30];
    pan_pixels(&m);pan_pixels(&h.set.map);
    puts("v7 monolithic/virtual-cell pixel pans exact at z7/8/14/15/16");
    size_t render_peak=0;
    for(unsigned mode=0;mode<2;mode++) for(unsigned z=7;z<=16;z+=(z==7?1:z==8?6:1)) {
        int64_t cx=(INT64_C(77)*256<<z)/256,cy=(INT64_C(93)*256<<z)/256;
        size_t p=render(&m,z,cx-80,cy-60,(cc_mode)mode,expected);if(p>render_peak)render_peak=p;
        render(&h.set.map,z,cx-80,cy-60,(cc_mode)mode,actual);assert(!memcmp(expected,actual,sizeof(actual)));
    }
    for(int k=0;k<3;k++) for(unsigned x=76;x<=78;x++) for(unsigned y=92;y<=93;y++) { uint64_t off;uint32_t n;assert(cc_cellset_resolve(&h.set,8,x,y,&off,&n)==1); }
    assert(h.set.evictions>4);printf("cell-exact overview/corner/overzoom; opens=%zu evictions=%zu render_peak=%zu\n",h.set.opens,h.set.evictions,render_peak);
    render(&m,16,INT64_C(77)*256*256-80,INT64_C(93)*256*256-60,CC_BRAILLE,expected);
    render(&h.set.map,16,INT64_C(77)*256*256-80,INT64_C(93)*256*256-60,CC_BRAILLE,actual);
    assert(decoded.hits && !memcmp(expected,actual,sizeof(actual)));
    cc_host_cellset_close(&h);
    /* The same scenes must resolve from each cell's directory without index.cci. */
    char idx[1400],oldidx[1400],cat[1400];
    snprintf(idx,sizeof(idx),"%s/index.cci",argv[1]);snprintf(oldidx,sizeof(oldidx),"%s/index.saved",argv[1]);
    snprintf(cat,sizeof(cat),"%s/catalog.ccm",argv[1]);
    FILE *catalog=fopen(cat,"wb");assert(catalog);
    uint8_t ch[128]={0};memcpy(ch,"CCMAPS2",8);ch[8]=7;assert(fwrite(ch,1,128,catalog)==128);
    for(unsigned i=0;i<7;i++) {
        unsigned id=i?1+(76+(i-1)/2)*256+92+(i-1)%2:0;char name[80];
        if(id)snprintf(name,sizeof(name),"8-%u-%u.ctile",(id-1)/256,(id-1)%256);else strcpy(name,"overview.ctile");
        cc_str data=load(argv[1],name);uint8_t row[48]={0};
        for(unsigned j=0;j<4;j++) { row[j]=(uint8_t)(id>>(8*j));row[4+j]=(uint8_t)(data.size>>(8*j)); }
        row[44]=1;assert(fwrite(row,1,48,catalog)==48);free((void*)data.data);
    }
    fclose(catalog);
    assert(!rename(idx,oldidx));assert(cc_host_cellset_open(&h,argv[1]));assert(h.set.indexless);pan_pixels(&h.set.map);
    for(unsigned z=7;z<=16;z+=(z==7?1:z==8?6:1)) {
        int64_t dx=(INT64_C(77)*256<<z)/256,dy=(INT64_C(93)*256<<z)/256;
        render(&m,z,dx-80,dy-60,CC_BRAILLE,expected);
        render(&h.set.map,z,dx-80,dy-60,CC_BRAILLE,actual);assert(!memcmp(expected,actual,sizeof(actual)));
        uint64_t off;uint32_t len;
        assert(cc_cellset_resolve(&h.set,z,(uint32_t)(dx/256),(uint32_t)(dy/256),&off,&len)==1);
        unsigned rz=z>14?14:z;uint32_t tx=(uint32_t)(dx/256)>>(z-rz),ty=(uint32_t)(dy/256)>>(z-rz);
        uint32_t owner=rz<8?0:1+(tx>>(rz-8))*256+(ty>>(rz-8));assert(off>>32==owner+1);
    }
    cc_cellset_close(&h.set);assert(cc_cellset_open_direct(&h.set,(cc_cellset_io){0},h.directory,4096,h.staging,1)==0);
    cc_host_cellset_close(&h);assert(!rename(oldidx,idx));assert(!remove(cat));
    char path[1400],hidden[1400];snprintf(path,sizeof(path),"%s/8-77-93.ctile",argv[1]);snprintf(hidden,sizeof(hidden),"%s/8-77-93.hidden",argv[1]);assert(!rename(path,hidden));
    assert(cc_host_cellset_open(&h,argv[1]));pan_pixels(&h.set.map);cc_ctile overview=m;overview.zmax=7;
    int64_t cx=INT64_C(77)*256*64,cy=INT64_C(93)*256*64;
    labels=0;cc_cell detail[80*30];
    render(&overview,14,cx-80,cy-60,CC_BRAILLE,expected);
    render(&m,14,cx-80,cy-60,CC_BRAILLE,detail);
    render(&h.set.map,14,cx-80,cy-60,CC_BRAILLE,actual);
    assert(h.set.fallbacks);unsigned different=0;
    for(int y=2;y<28;y++) for(int x=2;x<78;x++) {
        if(x>=38 && x<=42) continue;
        if(y>=13 && y<=17) continue;
        unsigned i=y*80+x;const cc_cell *want=x>=40 && y>=15?expected:detail;
        assert(!memcmp(actual+i,want+i,sizeof(cc_cell)));
        if(x<38 && memcmp(actual+i,expected+i,sizeof(cc_cell))) different++;
    }
    assert(different); /* available detail must not degrade with its neighbour */
    cc_host_cellset_close(&h);assert(!rename(hidden,path));
    free((void*)blob.data);free((void*)raw.data);free((void*)bad.data);free((void*)mono.data);free(corrupt);
    puts("cells/decoder bounds/checksum/missing overview/LRU: passed");return 0;
}
