#include "cartocore/ctile.h"
#include <string.h>
/* Thread-local generations avoid mutable process state and distinguish reopen
   of the same source/buffers. The owner address distinguishes opening threads. */
static _Thread_local uint64_t source_generation;
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t u64(const uint8_t *p) { return u32(p)|((uint64_t)u32(p+4)<<32); }
static int read_dir(cc_ctile_source *s,uint64_t off,void *buf,size_t len) {
    if(!s->read(s->ctx,off,buf,len)) return 0;
    s->reads++; s->directory_reads++; s->bytes_read+=len; return 1;
}
static int compare(const uint8_t *p,unsigned z,uint32_t x,uint32_t y) {
    if(p[0]!=z) return p[0]<z?-1:1;
    if(u32(p+4)!=x) return u32(p+4)<x?-1:1;
    if(u32(p+8)!=y) return u32(p+8)<y?-1:1;
    return 0;
}
static int page(const cc_ctile *f,uint32_t first) {
    cc_ctile_source *s=f->source;
    if(s->resident || (s->page_count && s->page_first==first)) return 1;
    uint32_t n=f->count-first; if(n>s->page_entries) n=s->page_entries;
    s->page_count=0;
    if(!read_dir(s,64+(uint64_t)first*24,s->directory,(size_t)n*24)) return 0;
    s->page_first=first; s->page_count=n; return 1;
}
int cc_ctile_open_source(cc_ctile *f,cc_ctile_source *s,cc_ctile_read read,void *ctx,
                        uint64_t size,void *dir,size_t dirbytes,void *staging,size_t stagingbytes) {
    uint8_t previous[24]={0}; uint8_t *h=s->header;
    memset(f,0,sizeof(*f)); memset(s,0,sizeof(*s));
    s->generation=++source_generation; s->generation_owner=&source_generation;
    if(!read || !dir || dirbytes<24 || !staging || size<64) return 0;
    s->read=read; s->ctx=ctx; s->directory=dir; s->directory_capacity=dirbytes;
    s->staging=staging; s->staging_capacity=stagingbytes; s->file_size=size;
    if(!read_dir(s,0,h,64) || !cc_ctile_header_check(h,64,size)) return 0;
    s->version=u32(h+8); f->count=u32(h+32); f->zmin=h[28]; f->zmax=h[29]; f->source=s;
    s->page_entries=dirbytes/24>UINT32_MAX?UINT32_MAX:(uint32_t)(dirbytes/24);
    s->resident=f->count<=s->page_entries;
    /* One sequential directory scan validates ordering/ranges and sizes. */
    for(uint32_t first=0;first<f->count;) {
        uint32_t n=f->count-first; if(n>s->page_entries) n=s->page_entries;
        if(!read_dir(s,64+(uint64_t)first*24,s->directory,(size_t)n*24)) goto fail;
        for(uint32_t j=0;j<n;j++) {
            const uint8_t *p=s->directory+(size_t)j*24; uint64_t off=u64(p+12); uint32_t len=u32(p+20);
            if(p[0]<f->zmin || p[0]>f->zmax || u32(p+4)>=(1u<<p[0]) || u32(p+8)>=(1u<<p[0]) ||
               off<64+(uint64_t)f->count*24 || off>size || len>size-off ||
               ((first || j) && compare(previous,p[0],u32(p+4),u32(p+8))>=0)) goto fail;
            if(len>s->max_blob) s->max_blob=len;
            memcpy(previous,p,24);
        }
        s->page_first=first; s->page_count=n; first+=n;
    }
    return 1;
fail: memset(f,0,sizeof(*f)); return 0;
}
int cc_ctile_locate(const cc_ctile *f,unsigned z,uint32_t x,uint32_t y,uint64_t *off,uint32_t *len) {
    uint32_t lo=0,hi=f->count; cc_ctile_source *s=f->source;
    if(!s) {
        while(lo<hi) { uint32_t m=lo+(hi-lo)/2; const uint8_t *p=f->bytes.data+64+(size_t)m*24;
            if(compare(p,z,x,y)<0) lo=m+1; else hi=m; }
        if(lo==f->count) return 0;
        const uint8_t *p=f->bytes.data+64+(size_t)lo*24;
        if(compare(p,z,x,y)) return 0;
        *off=u64(p+12); *len=u32(p+20); return 1;
    }
    /* Binary search whole pages, then the entries in the candidate page. */
    uint32_t pages=f->count/s->page_entries+(f->count%s->page_entries!=0);
    hi=pages;
    while(lo<hi) {
        uint32_t m=lo+(hi-lo)/2,first=m*s->page_entries;
        if(!page(f,first)) return -1;
        const uint8_t *last=s->directory+(s->resident?(size_t)(first+s->page_count-1)*24:(size_t)(s->page_count-1)*24);
        if(compare(last,z,x,y)<0) lo=m+1; else hi=m;
    }
    if(lo==pages) return 0;
    uint32_t first=lo*s->page_entries;
    if(!page(f,first)) return -1;
    lo=0; hi=s->page_count;
    const uint8_t *base=s->directory+(s->resident?(size_t)first*24:0);
    while(lo<hi) { uint32_t m=lo+(hi-lo)/2; if(compare(base+(size_t)m*24,z,x,y)<0) lo=m+1; else hi=m; }
    if(lo==s->page_count || compare(base+(size_t)lo*24,z,x,y)) return 0;
    *off=u64(base+(size_t)lo*24+12); *len=u32(base+(size_t)lo*24+20); return 1;
}
int cc_ctile_prefetch_hint(const cc_ctile *f,unsigned z,int64_t left,int64_t top,int32_t cols,int32_t rows,
                           int32_t dx,int32_t dy,cc_ctile_hint hint,void *ctx) {
    if(!hint || z<f->zmin || z>f->zmax || cols<1 || rows<1 || cols>16384 || rows>16384) return 0;
    int64_t limit=INT64_C(1)<<z;
    if(left<-32768 || top<-32768 || left>limit*256+32768 || top>limit*256+32768) return 0;
    left+=dx; top+=dy;
    int64_t x0=(left-8)/256-1,y0=(top-8)/256-1,x1=(left+cols*2+8)/256+1,y1=(top+rows*4+8)/256+1;
    if(x0<0) x0=0;
    if(y0<0) y0=0;
    if(x1>=limit) x1=limit-1;
    if(y1>=limit) y1=limit-1;
    for(int64_t x=x0;x<=x1;x++) for(int64_t y=y0;y<=y1;y++) {
        uint64_t off; uint32_t len;
        int found=cc_ctile_locate(f,z,(uint32_t)x,(uint32_t)y,&off,&len);
        if(found<0) return 0;
        if(found && !hint(ctx,z,(uint32_t)x,(uint32_t)y,off,len)) return 0;
    }
    return 1;
}
typedef struct { const cc_ctile *file; cc_arena *arena; cc_decoded_cache *cache; } idle_request;
static int idle_tile(void *ctx,unsigned z,uint32_t x,uint32_t y,uint64_t off,uint32_t len) {
    idle_request *r=ctx; cc_str tile; size_t mark=r->arena->used;
    (void)z; (void)x; (void)y;
    int ok=cc_ctile_source_tile(r->file,off,len,r->arena,r->cache,&tile);
    r->arena->used=mark; return ok;
}
int cc_ctile_prefetch(const cc_ctile *f,unsigned z,int64_t left,int64_t top,int32_t cols,int32_t rows,
                      int32_t dx,int32_t dy,cc_arena *a,cc_decoded_cache *cache) {
    if(!f->source || !cache) return 0;
    idle_request r={f,a,cache};
    return cc_ctile_prefetch_hint(f,z,left,top,cols,rows,dx,dy,idle_tile,&r);
}
