#include "cartocore/cellset.h"
#include <string.h>
static uint32_t u32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int acquire(cc_cellset *s,uint32_t id) {
    unsigned victim=0;
    for(unsigned i=0;i<4;i++) {
        if(s->slots[i].used && s->slots[i].id==id) { s->slots[i].age=++s->clock; return (int)i; }
        if(!s->slots[i].used || (s->slots[victim].used && s->slots[i].age<s->slots[victim].age)) victim=i;
    }
    if(s->slots[victim].used) { s->io.close(s->slots[victim].handle); s->slots[victim].used=0; s->evictions++; }
    void *handle=0; uint64_t bytes=0; int result=s->io.open(s->io.ctx,id,&handle,&bytes);
    if(result!=1) { if(!result) s->missing=1; return result==0?-2:-1; }
    uint8_t header[64];
    if(!s->io.read(handle,0,header,64) || !cc_ctile_header_check(header,64,bytes)) { s->io.close(handle); return -1; }
    s->slots[victim].used=1; s->slots[victim].id=id; s->slots[victim].handle=handle;
    s->slots[victim].bytes=bytes; s->slots[victim].age=++s->clock; s->opens++;
    return (int)victim;
}
static int read_virtual(void *ctx,uint64_t off,void *dst,size_t n) {
    cc_cellset *s=ctx;
    if(off<UINT64_C(0x100000000)) return off<=s->index_bytes && n<=s->index_bytes-off && s->index_read(s->index_ctx,off,dst,n);
    uint64_t key=(off>>32)-1; if(key>65536) return 0;
    int slot=acquire(s,(uint32_t)key); if(slot<0) return 0;
    off&=UINT32_MAX;
    return off<=s->slots[slot].bytes && n<=s->slots[slot].bytes-off && s->io.read(s->slots[slot].handle,off,dst,n);
}
int cc_cellset_open(cc_cellset *s,cc_ctile_read read,void *ctx,uint64_t bytes,cc_cellset_io io,
                    void *dir,size_t dirbytes,void *staging,size_t stagingbytes) {
    uint8_t h[64]; memset(s,0,sizeof(*s));
    if(!read || !io.open || !io.read || !io.close || bytes<64 || bytes>=UINT64_C(0x100000000) || !read(ctx,0,h,64) ||
       memcmp(h,"CTILE1\0\0",8) || u32(h+8)!=7 || h[28]!=0 || h[29]!=14 ||
       64+(uint64_t)u32(h+32)*24!=bytes) return 0;
    uint64_t virtual_size=(uint64_t)u32(h+48)|((uint64_t)u32(h+52)<<32);
    if(virtual_size!=UINT64_C(65538)*UINT64_C(0x100000000)) return 0;
    s->index_read=read; s->index_ctx=ctx; s->index_bytes=bytes; s->io=io;
    if(!cc_ctile_open_source(&s->map,&s->source,read_virtual,s,virtual_size,dir,dirbytes,staging,stagingbytes)) return 0;
    /* Validate cell ownership without opening thousands of files. */
    for(uint32_t first=0;first<s->map.count;) {
        uint32_t count=s->map.count-first;
        if(count>s->source.page_entries) count=s->source.page_entries;
        if(!read(ctx,64+(uint64_t)first*24,dir,(size_t)count*24)) return 0;
        for(uint32_t i=0;i<count;i++) {
            const uint8_t *p=(const uint8_t*)dir+i*24;unsigned z=p[0];
            uint32_t id=z<8?0:1+(u32(p+4)>>(z-8))*256+(u32(p+8)>>(z-8));
            if(u32(p+16)!=id+1 || u32(p+12)<64 || (uint64_t)u32(p+12)+u32(p+20)>UINT64_C(0x100000000)) return 0;
        }
        s->source.page_first=first;s->source.page_count=count;first+=count;
    }
    s->map.cellset=s; return 1;
}
void cc_cellset_close(cc_cellset *s) {
    for(unsigned i=0;i<4;i++) if(s->slots[i].used) { s->io.close(s->slots[i].handle); s->slots[i].used=0; }
    memset(&s->map,0,sizeof(s->map));
}
int cc_cellset_resolve(cc_cellset *s,unsigned z,uint32_t x,uint32_t y,uint64_t *off,uint32_t *len) {
    if(z>16 || x>=(1u<<z) || y>=(1u<<z)) return -1;
    if(z>14) { x>>=z-14; y>>=z-14; z=14; }
    uint32_t id=z<8?0:1+(x>>(z-8))*256+(y>>(z-8));
    int slot=acquire(s,id); if(slot<0) return slot==-2?0:-1;
    return cc_ctile_locate(&s->map,z,x,y,off,len);
}
#include "cellset_clip.h"
int cc_cellset_scene(cc_cellset *s,unsigned z,int64_t left,int64_t top,int cols,int rows,
                     cc_arena *a,cc_scene *scene,cc_decoded_cache *cache) {
    memset(scene,0,sizeof(*scene));
    if(z>16 || cols<1 || rows<1 || cols>16384 || rows>16384) return 0;
    int64_t world=INT64_C(256)<<z;
    if(left<-32768 || top<-32768 || left>world+32768 || top>world+32768) return 0;
    size_t mark=a->used;s->missing=0;
    if(z>14) {
        unsigned shift=z-14;int factor=1<<shift;
        int64_t pl=left/factor-(left<0 && left%factor),pt=top/factor-(top<0 && top%factor);
        if(!cc_cellset_scene(s,14,pl,pt,(cols+factor-1)/factor+1,(rows+factor-1)/factor+1,a,scene,cache) ||
           !cc_scene_overzoom(scene,shift,(int32_t)(left-pl*factor),(int32_t)(top-pt*factor),a)) goto fail;
        return 1;
    }
    cc_ctile map=s->map;map.cellset=0;
    if(z<8) return cc_ctile_scene_cached(&map,z,left,top,cols,rows,a,scene,cache);
    int64_t span=INT64_C(256)<<(z-8),limit=INT64_C(1)<<z;
    int64_t x0=(left-8)/256,y0=(top-8)/256,x1=(left+cols*2+8)/256,y1=(top+rows*4+8)/256;
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>=limit)x1=limit-1;
    if(y1>=limit)y1=limit-1;
    for(int64_t x=x0>>(z-8);x<=x1>>(z-8);x++) for(int64_t y=y0>>(z-8);y<=y1>>(z-8);y++)
        if(acquire(s,1+(uint32_t)x*256+(uint32_t)y)==-1) goto fail;
    if(!s->missing) return cc_ctile_scene_cached(&map,z,left,top,cols,rows,a,scene,cache);
    if(cache) cc_decoded_cache_begin(cache);
    for(int64_t x=x0;x<=x1;x++) for(int64_t y=y0;y<=y1;y++) {
        uint32_t ownerx=(uint32_t)x>>(z-8),ownery=(uint32_t)y>>(z-8);
        int slot=acquire(s,1+ownerx*256+ownery);if(slot==-1) goto fail;
        unsigned tz=z;uint32_t tx=(uint32_t)x,ty=(uint32_t)y;
        if(slot==-2) {
            int64_t firstx=(int64_t)ownerx<<(z-8),firsty=(int64_t)ownery<<(z-8);
            if(x!=(firstx>x0?firstx:x0) || y!=(firsty>y0?firsty:y0)) continue;
            tz=7;tx=ownerx/2;ty=ownery/2;s->fallbacks++;
        }
        uint64_t off;uint32_t len;int found=cc_ctile_locate(&map,tz,tx,ty,&off,&len);
        if(found<0) goto fail;
        if(!found) continue; /* installed empty is empty */
        cc_str tile;cc_scene part={0};
        if(!cc_ctile_source_tile(&map,off,len,a,cache,&tile) || !cc_ctile_tile_scene(tile,0,0,a,&part)) goto fail;
        if(tz!=z && !clip_scene(&part,(int32_t)((ownerx&1)*128),(int32_t)((ownery&1)*128),
                                (int32_t)((ownerx&1)*128+128),(int32_t)((ownery&1)*128+128),a)) goto fail;
        if(!cc_scene_overzoom(&part,z-tz,(int32_t)(left-((int64_t)tx*256<<(z-tz))),
                             (int32_t)(top-((int64_t)ty*256<<(z-tz))),a)) goto fail;
        if(tz==z && !clip_scene(&part,(int32_t)(ownerx*span-left),(int32_t)(ownery*span-top),
                       (int32_t)((ownerx+1)*span-left),(int32_t)((ownery+1)*span-top),a)) goto fail;
        merge_scene(scene,&part);
    }
    return 1;
fail:
    a->used=mark;memset(scene,0,sizeof(*scene));return 0;
}
