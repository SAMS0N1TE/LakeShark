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
    if(off<UINT64_C(0x100000000)) {
        if(s->indexless) { int slot=acquire(s,0);return slot>=0 && off<=s->slots[slot].bytes && n<=s->slots[slot].bytes-off && s->io.read(s->slots[slot].handle,off,dst,n); }
        return off<=s->index_bytes && n<=s->index_bytes-off && s->index_read(s->index_ctx,off,dst,n);
    }
    uint64_t key=(off>>32)-1; if(key>131072) return 0;
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
int cc_cellset_open_direct(cc_cellset *s,cc_cellset_io io,void *dir,size_t dirbytes,void *staging,size_t stagingbytes) {
    memset(s,0,sizeof(*s));
    if(!io.open || !io.read || !io.close) return 0;
    s->io=io;s->indexless=1;
    int slot=acquire(s,0);if(slot<0) return 0;
    if(!cc_ctile_open_source(&s->map,&s->source,read_virtual,s,s->slots[slot].bytes,dir,dirbytes,staging,stagingbytes)) { cc_cellset_close(s);return 0; }
    if(s->map.zmax>7 || s->source.version!=7) { cc_cellset_close(s);return 0; }
    s->map.zmin=0;s->map.zmax=16;s->map.cellset=s;s->map.directory_owner=s;
    s->source.file_size=UINT64_C(131074)<<32;
    return 1;
}
int cc_cellset_resolve(cc_cellset *s,unsigned z,uint32_t x,uint32_t y,uint64_t *off,uint32_t *len) {
    if(z>16 || x>=(1u<<z) || y>=(1u<<z)) return -1;
    if(!s->indexless && z>s->map.zmax) { x>>=z-s->map.zmax; y>>=z-s->map.zmax; z=s->map.zmax; }
    uint32_t id=z<8?0:1+(x>>(z-8))*256+(y>>(z-8));
    int slot=-2;
    if(s->indexless && z>FREE_MAX_ZOOM) {
        slot=acquire(s,id+CC_DETAIL_ID);
        if(slot>=0) id+=CC_DETAIL_ID;
        else if(slot==-1) return -1;
    }
    if(slot==-2) slot=acquire(s,id);
    if(slot<0) return slot==-2?0:-1;
    if(s->indexless) {
        uint8_t h[64],r[24];void *handle=s->slots[slot].handle;uint64_t bytes=s->slots[slot].bytes;
        if(!s->io.read(handle,0,h,64) || u32(h+8)!=7) return -1;
        if(z>h[29]) { x>>=z-h[29]; y>>=z-h[29]; z=h[29]; }
        uint32_t count=u32(h+32),lo=0,hi=count;
        while(lo<hi) {
            uint32_t mid=lo+(hi-lo)/2;
            if(!s->io.read(handle,64+(uint64_t)mid*24,r,24)) return -1;
            int less=r[0]<z || (r[0]==z && (u32(r+4)<x || (u32(r+4)==x && u32(r+8)<y)));
            if(less)lo=mid+1;else hi=mid;
        }
        if(lo==count)return 0;
        if(!s->io.read(handle,64+(uint64_t)lo*24,r,24))return -1;
        if(r[0]!=z || u32(r+4)!=x || u32(r+8)!=y)return 0;
        uint64_t offset=(uint64_t)u32(r+12)|((uint64_t)u32(r+16)<<32);uint32_t length=u32(r+20);
        if(offset<64+(uint64_t)count*24 || offset>UINT32_MAX || offset>bytes || length>bytes-offset)return -1;
        *off=((uint64_t)(id+1)<<32)|offset;*len=length;return 1;
    }
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
    cc_ctile map=s->map;map.cellset=0;
    if(z<8) return cc_ctile_scene_cached(&map,z,left,top,cols,rows,a,scene,cache);
    int64_t span=INT64_C(256)<<(z-8),limit=INT64_C(1)<<z;
    int64_t x0=(left-8)/256,y0=(top-8)/256,x1=(left+cols*2+8)/256,y1=(top+rows*4+8)/256;
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>=limit)x1=limit-1;
    if(y1>=limit)y1=limit-1;
    /* Retain native phase/raster paths when every visible owner has this zoom.
       Missing optional detail must not disable the legacy monolithic fast path. */
    int native=1;
    for(int64_t x=x0>>(z-8);x<=x1>>(z-8);x++) for(int64_t y=y0>>(z-8);y<=y1>>(z-8);y++) {
        uint32_t id=1+(uint32_t)x*256+(uint32_t)y;
        int slot=-2;
        if(s->indexless && z>FREE_MAX_ZOOM)slot=acquire(s,id+CC_DETAIL_ID);
        if(slot==-2)slot=acquire(s,id);
        if(slot==-1)goto fail;
        if(slot==-2) { native=0;continue; }
        uint8_t h[64];if(!s->io.read(s->slots[slot].handle,0,h,64))goto fail;
        if(z>h[29])native=0;
    }
    if(native)return cc_ctile_scene_cached(&map,z,left,top,cols,rows,a,scene,cache);
    if(cache) cc_decoded_cache_begin(cache);
    for(int64_t x=x0;x<=x1;x++) for(int64_t y=y0;y<=y1;y++) {
        uint32_t ownerx=(uint32_t)x>>(z-8),ownery=(uint32_t)y>>(z-8);
        uint32_t id=1+ownerx*256+ownery;
        int slot=-2;
        if(s->indexless && z>FREE_MAX_ZOOM) {
            slot=acquire(s,id+CC_DETAIL_ID);
            if(slot==-1) goto fail;
        }
        if(slot==-2) slot=acquire(s,id);
        if(slot==-1) goto fail;
        unsigned tz=z;uint32_t tx=(uint32_t)x,ty=(uint32_t)y;
        if(slot==-2) { tz=7; s->fallbacks++; }
        else {
            uint8_t h[64];
            if(!s->io.read(s->slots[slot].handle,0,h,64)) goto fail;
            if(tz>h[29]) { tz=h[29]; s->fallbacks++; }
        }
        unsigned shift=z-tz;
        tx>>=shift; ty>>=shift;
        if(shift) {
            int64_t firstx=(int64_t)tx<<shift,firsty=(int64_t)ty<<shift;
            int64_t owner_firstx=(int64_t)ownerx<<(z-8),owner_firsty=(int64_t)ownery<<(z-8);
            if(firstx<owner_firstx)firstx=owner_firstx;
            if(firsty<owner_firsty)firsty=owner_firsty;
            if(x!=(firstx>x0?firstx:x0) || y!=(firsty>y0?firsty:y0))continue;
        }
        uint64_t off;uint32_t len;int found=cc_ctile_locate(&map,tz,tx,ty,&off,&len);
        if(found<0) goto fail;
        if(!found) continue; /* installed empty is empty */
        cc_str tile;cc_scene part={0};
        if(!cc_ctile_source_tile(&map,off,len,a,cache,&tile) || !cc_ctile_tile_scene(tile,0,0,a,&part)) goto fail;
        if(!cc_scene_overzoom(&part,z-tz,(int32_t)(left-((int64_t)tx*256<<(z-tz))),
                             (int32_t)(top-((int64_t)ty*256<<(z-tz))),a)) goto fail;
        if(!clip_scene(&part,(int32_t)(ownerx*span-left),(int32_t)(ownery*span-top),
                       (int32_t)((ownerx+1)*span-left),(int32_t)((ownery+1)*span-top),a)) goto fail;
        merge_scene(scene,&part);
    }
    return 1;
fail:
    a->used=mark;memset(scene,0,sizeof(*scene));return 0;
}
