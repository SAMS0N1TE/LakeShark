#include "cartocore/ctile.h"
#include "cartocore/zstd.h"
#include "cartocore/cellset.h"
#include "cartocore/mvt.h"
#include "cartocore/out.h"
#include <string.h>
#include "cartocore/profile.h"
/* Native coordinates overwhelmingly use one or two bytes. The general wire
   reader remains the bounded fallback for long values and malformed input. */
static inline int native_var(cc_wire *r,uint64_t *v) {
    if(r->p<r->end && r->p[0]<128) { *v=*r->p++; return 1; }
    if(r->end-r->p>=2 && r->p[1]<128) { *v=(r->p[0]&127)|((uint64_t)r->p[1]<<7); r->p+=2; return 1; }
    return cc_varint(r,v);
}
static inline int64_t native_zig(uint64_t v) { return (v&1)?-(int64_t)(v>>1)-1:(int64_t)(v>>1); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t u64(const uint8_t *p) { return u32(p)|((uint64_t)u32(p+4)<<32); }
int cc_ctile_header_check(const void *header,size_t header_bytes,uint64_t file_bytes) {
    const uint8_t *h=header;
    if(!h || header_bytes<64 || file_bytes<64 || memcmp(h,"CTILE1\0\0",8)) return 0;
    uint32_t version=u32(h+8);
    return version>=CC_CTILE_VERSION_MIN && version<=CC_CTILE_VERSION_MAX &&
        h[28]<=h[29] && h[29]<=22 && u32(h+36)==CT_CLASS_COUNT-1 &&
        u64(h+48)==file_bytes && u32(h+32)<=(file_bytes-64)/24;
}
/* Bounded tile LZ: literal packets, or a backward byte-range copy. */
static int zstd_tile(cc_str t) { return t.size>=8 && !memcmp(t.data,"CZ7\0",4); }
static int compressed(cc_str t) { return t.size>=8 && !memcmp(t.data,"CTZ\0",4); }
static int unpack(cc_str t,uint8_t *out) {
    size_t size=u32(t.data+4),at=0; const uint8_t *p=t.data+8,*end=t.data+t.size;
    if(size<32 || size>16u*1024u*1024u) return 0;
    while(p<end) {
        unsigned token=*p++; size_t n;
        if(token<128) {
            n=token+1; if(n>(size_t)(end-p) || n>size-at) return 0;
            if(out) memcpy(out+at,p,n);
            p+=n;
        } else {
            n=(token&127)+3; if(end-p<2 || n>size-at) return 0;
            size_t distance=(unsigned)p[0]|(unsigned)p[1]<<8; p+=2;
            if(!distance || distance>at) return 0;
            if(out) {
                if(distance>=n) memcpy(out+at,out+at-distance,n);
                else for(size_t j=0;j<n;j++) out[at+j]=out[at+j-distance];
            }
        }
        at+=n;
    }
    return at==size;
}
static int v6(cc_str t) { return t.size>=16 && !memcmp(t.data,"CT6\0",4); }
static int validate6(cc_str t);
static int validate3(cc_str t);
static int validate4(cc_str t);
static int expand(cc_str *t,cc_arena *a) {
    if(zstd_tile(*t)) { cc_str out; if(!cc_zstd_decode(*t,a,&out) || !v6(out) || !validate6(out)) return 0; *t=out; return 1; }
    if(!compressed(*t)) return 1;
    uint32_t n=u32(t->data+4); if(n>16u*1024u*1024u) return 0;
    uint8_t *b=cc_arena_alloc(a,n,1); if(!b || !unpack(*t,b)) return 0;
    *t=(cc_str){b,n}; return (!memcmp(b,"CT4\0",4) || !memcmp(b,"CT5\0",4))?validate4(*t):validate3(*t);
}
static int16_t i16(const uint8_t *p) { return (int16_t)((unsigned)p[0]|(unsigned)p[1]<<8); }
static int v3(cc_str t) { return t.size>=200 && !memcmp(t.data,"CT3\0",4); }
static int fixed_range(cc_str t,uint32_t off,uint32_t n,unsigned stride) {
    return off>=200 && off<=t.size && n<=(t.size-off)/stride;
}
/* Bounds drive viewport culling and must contain every referenced coordinate.
   Bound repeated references too, so malformed metadata cannot make validation
   perform quadratic work on shared paths. */
#define BOUNDS_WORK_LIMIT (16u*1024u*1024u)
static int contains(int x,int y,int x0,int y0,int x1,int y1) {
    return x>=x0 && x<=x1 && y>=y0 && y<=y1;
}
static int validate3(cc_str t) {
    if(!v3(t)) return 0;
    const uint8_t *b=t.data; uint32_t ns=u32(b+4),nf=u32(b+8),np=u32(b+12),nv=u32(b+16);
    uint32_t no=u32(b+20),fo=u32(b+24),po=u32(b+28),xo=po+np*8;
    if(!fixed_range(t,no,ns,8) || !fixed_range(t,fo,nf,40) || !fixed_range(t,po,np,8) ||
       (uint64_t)po+(uint64_t)np*8>UINT32_MAX || !fixed_range(t,xo,nv,4)) return 0;
    for(uint32_t i=0;i<ns;i++) { const uint8_t *p=b+no+i*8; if(u32(p)>t.size || u32(p+4)>t.size-u32(p)) return 0; }
    uint32_t end=0;
    for(unsigned c=1;c<CT_CLASS_COUNT;c++) {
        const uint8_t *p=b+32+(c-1)*8; if(u32(p)!=end || u32(p+4)>nf-end) return 0;
        end+=u32(p+4);
        for(uint32_t j=u32(p);j<end;j++) if(b[fo+j*40]!=c) return 0;
    }
    if(end!=nf) return 0;
    for(uint32_t i=0;i<nf;i++) {
        const uint8_t *p=b+fo+i*40;
        if(p[1]<1 || p[1]>3 || p[2]>5 || u32(p+4)>ns || u32(p+8)>np || u32(p+12)>np-u32(p+8) ||
           i16(p+24)>i16(p+28) || i16(p+26)>i16(p+30)) return 0;
    }
    for(uint32_t i=0;i<nf;i++) {
        const uint8_t *f=b+fo+i*40; uint32_t off=u32(f+32);
        if(!off) continue;
        if(f[1]!=3 || !u32(f+12) || off>t.size || t.size-off<64) return 0;
        for(unsigned phase=0;phase<8;phase++) {
            const uint8_t *h=b+off+phase*8; uint32_t start=u32(h),nr=(uint16_t)i16(h+4),ne=(uint16_t)i16(h+6);
            if(start<64 || start>t.size-off || (uint64_t)nr+ne>(t.size-off-start)/3) return 0;
            const uint8_t *r=b+off+start;
            for(uint32_t j=0;j<nr+ne;j++,r+=3) {
                unsigned y=j<nr?r[0]:r[0]&127,x=r[1];
                if(y>=72 || x>=144) return 0;
                if(j<nr && (!r[2] || r[2]>144-x)) return 0;
            }
        }
    }
    for(uint32_t i=0;i<np;i++) { const uint8_t *p=b+po+i*8; if(u32(p)>nv || u32(p+4)>nv-u32(p)) return 0; }
    for(uint32_t i=0;i<nv*2;i++) if(i16(b+xo+i*2)<-8 || i16(b+xo+i*2)>264) return 0;
    size_t work=0;
    for(uint32_t i=0;i<nf;i++) {
        const uint8_t *f=b+fo+i*40;
        for(uint32_t k=0;k<u32(f+12);k++) {
            const uint8_t *path=b+po+(u32(f+8)+k)*8;
            uint32_t first=u32(path),n=u32(path+4);
            if(n>BOUNDS_WORK_LIMIT-work) return 0;
            work+=n;
            for(uint32_t j=first;j<first+n;j++)
                if(!contains(i16(b+xo+j*2),i16(b+xo+nv*2+j*2),i16(f+24),i16(f+26),i16(f+28),i16(f+30))) return 0;
        }
    }
    return 1;
}
static int v4(cc_str t) { return t.size>=32 && (!memcmp(t.data,"CT4\0",4) || !memcmp(t.data,"CT5\0",4)); }
static int range4(cc_str t,uint32_t off,uint32_t n,unsigned stride) {
    return off>=32 && off<=t.size && n<=(t.size-off)/stride;
}
static int bound4(const uint8_t *f,unsigned k) {
    uint64_t v=(uint64_t)u32(f+10)|((uint64_t)f[14]<<32);
    return (int)((v>>(k*9))&511)-8;
}
static int bounds4(const uint8_t *b,uint32_t nf,uint32_t fo,uint32_t np,uint32_t po,int delta) {
    size_t work=0; unsigned cursor=0;
    const uint8_t *coords=b+po+np*4,*v=coords+(delta?4:0);
    for(uint32_t i=0;i<nf;i++) {
        const uint8_t *f=b+fo+i*24;
        unsigned first=(uint16_t)i16(f+6),n=(uint16_t)i16(f+8);
        /* CT5 has a sequential stream. Skip earlier paths without retaining a
           large index on the embedded target's stack. Class-ordered builder
           records traverse it once; shared/unordered references may rewind. */
        if(delta && first<cursor) { cursor=0; v=coords+4; }
        if(delta) for(unsigned k=cursor;k<first;k++) {
            unsigned count=(uint16_t)i16(b+po+k*4+2);
            if(count>BOUNDS_WORK_LIMIT-work) return 0;
            work+=count;
            for(unsigned j=0;j<count*2;j++) { int d=(int8_t)*v++; if(d==-128) v+=2; }
        }
        for(unsigned k=first;k<first+n;k++) {
            const uint8_t *path=b+po+k*4; unsigned start=(uint16_t)i16(path),count=(uint16_t)i16(path+2);
            int x=0,y=0;
            if(count>BOUNDS_WORK_LIMIT-work) return 0;
            work+=count;
            for(unsigned j=0;j<count;j++) {
                if(delta) {
                    int d=(int8_t)*v++; if(d==-128) { x=i16(v); v+=2; } else x+=d;
                    d=(int8_t)*v++; if(d==-128) { y=i16(v); v+=2; } else y+=d;
                } else {
                    const uint8_t *p=v+(start+j)*3;
                    x=((unsigned)p[0]|((unsigned)p[1]&15)<<8)-8;
                    y=((unsigned)p[1]>>4|(unsigned)p[2]<<4)-8;
                }
                if(!contains(x,y,bound4(f,0),bound4(f,1),bound4(f,2),bound4(f,3))) return 0;
            }
        }
        if(delta) cursor=first+n;
    }
    return 1;
}
static int validate4(cc_str t) {
    if(!v4(t)) return 0;
    const uint8_t *b=t.data; uint32_t ns=u32(b+4),nf=u32(b+8),np=u32(b+12),nv=u32(b+16);
    uint32_t no=u32(b+20),fo=u32(b+24),po=u32(b+28);
    int delta=b[2]=='5';
    if(np>65535 || nv>65535 || (uint64_t)po+np*4>UINT32_MAX ||
       !range4(t,no,ns,8) || !range4(t,fo,nf,24) || !range4(t,po,np,4) || (!delta && !range4(t,po+np*4,nv,3))) return 0;
    for(uint32_t i=0;i<ns;i++) { const uint8_t *p=b+no+i*8; if(u32(p)>t.size || u32(p+4)>t.size-u32(p)) return 0; }
    for(uint32_t i=0;i<np;i++) { const uint8_t *p=b+po+i*4; unsigned first=(uint16_t)i16(p),n=(uint16_t)i16(p+2); if(first>nv || n>nv-first) return 0; }
    for(uint32_t i=0;!delta && i<nv;i++) { const uint8_t *p=b+po+np*4+i*3; if(((unsigned)p[0]|((unsigned)p[1]&15)<<8)>272 || ((unsigned)p[1]>>4|(unsigned)p[2]<<4)>272) return 0; }
    if(delta) {
        size_t xo=(size_t)po+np*4; if(xo>t.size || t.size-xo<4 || u32(b+xo)>t.size-xo-4) return 0;
        const uint8_t *p=b+xo+4,*end=p+u32(b+xo); uint32_t total=0;
        for(uint32_t j=0;j<np;j++) {
            const uint8_t *path=b+po+j*4; unsigned n=(uint16_t)i16(path+2); int x=0,y=0;
            if((uint16_t)i16(path)!=total) return 0;
            for(unsigned i=0;i<n;i++) {
                int *v[2]={&x,&y};
                for(unsigned k=0;k<2;k++) { if(p==end) return 0; int d=(int8_t)*p++;
                    if(d==-128) { if(end-p<2) return 0; *v[k]=i16(p); p+=2; } else *v[k]+=d;
                    if(*v[k]<-8 || *v[k]>264) return 0;
                }
            }
            total+=n;
        }
        if(total!=nv || p!=end) return 0;
    }
    for(uint32_t i=0;i<nf;i++) {
        const uint8_t *f=b+fo+i*24; unsigned first=(uint16_t)i16(f+6),n=(uint16_t)i16(f+8);
        if(!f[0] || f[0]>CT_INDEX_CONTOUR || (delta && f[0]>=CT_CLASS_COUNT) || f[1]<1 || f[1]>3 || f[2]>5 || (uint16_t)i16(f+4)>ns || first>np || n>np-first ||
           bound4(f,0)>bound4(f,2) || bound4(f,1)>bound4(f,3)) return 0;
        uint32_t anchor=u32(f+20),off=u32(f+16);
        if(delta && off) return 0;
        if(f[2] && (anchor<32 || anchor>t.size || t.size-anchor<8)) return 0;
        if(!off) continue;
        if(f[1]!=3 || !n || off>t.size || t.size-off<(f[3]?32u:64u)) return 0;
        if(f[3]) {
            if(f[3]!=1) return 0;
            for(unsigned phase=0;phase<8;phase++) {
                int64_t pos=(int64_t)off+(int32_t)u32(b+off+phase*4);
                if(pos<32 || (uint64_t)pos>t.size || t.size-(size_t)pos<4) return 0;
                const uint8_t *r=b+(size_t)pos; unsigned nr=(uint16_t)i16(r),ne=(uint16_t)i16(r+2); r+=4;
                uint8_t v[3]={0}; unsigned repeat=0;
                for(unsigned j=0;j<nr+ne;j++) {
                    int edge=j>=nr; if(j==nr) repeat=0;
                    if(repeat) { v[edge?1:0]++; repeat--; }
                    else {
                        if(r>=b+t.size) return 0;
                        if(*r==255) {
                            if((edge?j==nr:j==0) || b+t.size-r<2 || !r[1] || r[1]>(edge?nr+ne-j:nr-j)) return 0;
                            repeat=r[1]-1; r+=2; v[edge?1:0]++;
                        } else { if(b+t.size-r<3) return 0; memcpy(v,r,3); r+=3; }
                    }
                    unsigned y=edge?v[0]&127:v[0],x=v[1];
                    if(y>=72 || x>=144 || (!edge && (!v[2] || v[2]>144-x))) return 0;
                }
            }
            continue;
        }
        for(unsigned phase=0;phase<8;phase++) {
            const uint8_t *h=b+off+phase*8; uint32_t start=u32(h),nr=(uint16_t)i16(h+4),ne=(uint16_t)i16(h+6);
            if(start<64 || start>t.size-off || (uint64_t)nr+ne>(t.size-off-start)/3) return 0;
            const uint8_t *r=b+off+start;
            for(uint32_t j=0;j<nr+ne;j++,r+=3) {
                unsigned y=j<nr?r[0]:r[0]&127,x=r[1];
                if(y>=72 || x>=144 || (j<nr && (!r[2] || r[2]>144-x))) return 0;
            }
        }
    }
    return bounds4(b,nf,fo,np,po,delta);
}
static int validate6(cc_str t) {
    if(!v6(t)) return 0;
    uint32_t ng=u32(t.data+4),nc=u32(t.data+8),nr=u32(t.data+12);
    if((uint64_t)16+ng+nc+nr!=t.size || nr<8) return 0;
    cc_str g={t.data+16,ng},c={t.data+16+ng,nc};
    if(!(compressed(g)?unpack(g,0):v4(g)?validate4(g):validate3(g))) return 0;
    if(nc) {
        if(!v4(c) || c.data[2]!='4' || !validate4(c)) return 0;
        uint32_t nf=u32(c.data+8),fo=u32(c.data+24);
        for(uint32_t i=0;i<nf;i++) { const uint8_t *f=c.data+fo+i*24;
            if(f[0]<CT_CONTOUR || f[0]>CT_INDEX_CONTOUR || f[1]!=2 || f[2]) return 0;
        }
    }
    const uint8_t *r=t.data+16+ng+nc;
    if(!memcmp(r,"SH1\0",4)) {
        if(r[4]<1 || r[4]>8 || r[5]<2 || r[5]>8 || r[6]>2 || r[7]) return 0;
        unsigned count=((256u>>r[4])+1)*((256u>>r[5])+1);
        if(r[6]==2) return nr==8;
        if(!r[6]) return nr==8+(count+1)/2 && (!(count&1) || !(r[nr-1]&240));
        unsigned at=0;
        for(unsigned i=8;i<nr;i++) { unsigned n=(r[i]>>4)+1; if(n>count-at) return 0; at+=n; }
        return at==count;
    }
    if(nr<260) return 0;
    if((u32(r)&0x7fffffffu)!=260 || u32(r+256)!=nr) return 0;
    for(unsigned y=0;y<64;y++) {
        uint32_t tag=u32(r+y*4),a=tag&0x7fffffffu,b=u32(r+(y+1)*4)&0x7fffffffu; unsigned x=0;
        if(a<260 || b<=a || b>nr || (b-a)%2) return 0;
        if(tag&0x80000000u) { if(b-a!=64) return 0; continue; }
        for(uint32_t k=a;k<b;k+=2) { unsigned end=r[k];
            if(end<=x || r[k+1]>15) return 0;
            x=end;
        }
        if(x!=128) return 0;
    }
    return 1;
}
int cc_ctile_open(cc_str b,cc_ctile *f) {
    memset(f,0,sizeof(*f));
    if(!cc_ctile_header_check(b.data,b.size,b.size)) return 0;
    f->cellset=0; f->bytes=b; f->zmin=b.data[28]; f->zmax=b.data[29]; f->count=u32(b.data+32);
    for(uint32_t i=0;i<f->count;i++) {
        const uint8_t *p=b.data+64+(size_t)i*24; uint64_t off=u64(p+12); uint32_t len=u32(p+20);
        if(p[0]<f->zmin || p[0]>f->zmax || u32(p+4)>=(1u<<p[0]) || u32(p+8)>=(1u<<p[0]) || off<64+(uint64_t)f->count*24 || off>b.size || len>b.size-off) return 0;
        if(i) { const uint8_t *q=p-24; if(q[0]>p[0] || (q[0]==p[0] && (u32(q+4)>u32(p+4) || (u32(q+4)==u32(p+4) && u32(q+8)>=u32(p+8))))) return 0; }
        if((u32(b.data+8)==7)!=zstd_tile((cc_str){b.data+(size_t)off,len})) return 0;
        if(u32(b.data+8)>=3 || (len>=4 && (!memcmp(b.data+(size_t)off,"CT3\0",4) ||
           !memcmp(b.data+(size_t)off,"CT4\0",4) || !memcmp(b.data+(size_t)off,"CT5\0",4) || !memcmp(b.data+(size_t)off,"CT6\0",4) || !memcmp(b.data+(size_t)off,"CTZ\0",4)))) {
            cc_str tile={b.data+(size_t)off,len};
            size_t zn; int valid=zstd_tile(tile)?cc_zstd_header(tile,&zn):v6(tile)?validate6(tile):compressed(tile)?unpack(tile,0):v4(tile)?validate4(tile):validate3(tile);
            if(!valid) return 0;
        }
    }
    return 1;
}
int cc_ctile_find(const cc_ctile *f,unsigned z,uint32_t x,uint32_t y,cc_str *tile) {
    if(f->source) {
        uint64_t off; uint32_t len;
        int found=cc_ctile_locate(f,z,x,y,&off,&len);
        if(found!=1) return 0;
        cc_ctile_source *src=f->source;
        if(len>src->staging_capacity || !src->read(src->ctx,off,src->staging,len)) return 0;
        src->reads++; src->blob_reads++; src->bytes_read+=len;
        *tile=(cc_str){src->staging,len}; return 1;
    }
    uint32_t lo=0,hi=f->count;
    while(lo<hi) { uint32_t m=lo+(hi-lo)/2; const uint8_t *p=f->bytes.data+64+(size_t)m*24;
        uint32_t px=u32(p+4),py=u32(p+8);
        if(p[0]<z || (p[0]==z && (px<x || (px==x && py<y)))) lo=m+1; else hi=m;
    }
    if(lo==f->count) return 0;
    { const uint8_t *p=f->bytes.data+64+(size_t)lo*24;
      if(p[0]!=z || u32(p+4)!=x || u32(p+8)!=y) return 0;
      *tile=(cc_str){f->bytes.data+(size_t)u64(p+12),u32(p+20)}; return 1; }
}
int cc_ctile_name(cc_str tile,uint32_t index,cc_str *name) {
    *name=(cc_str){0,0};
    if(v6(tile)) { if(!validate6(tile)) return 0; tile=(cc_str){tile.data+16,u32(tile.data+4)}; }
    if(compressed(tile)) return 0;
    if(v3(tile) || v4(tile)) {
        *name=(cc_str){0,0};
        if(index>u32(tile.data+4)) return 0;
        if(index) {
            if(!(v4(tile)?range4(tile,u32(tile.data+20),u32(tile.data+4),8):fixed_range(tile,u32(tile.data+20),u32(tile.data+4),8))) return 0;
            const uint8_t *p=tile.data+u32(tile.data+20)+(index-1)*8;
            if(u32(p)>tile.size || u32(p+4)>tile.size-u32(p)) return 0;
            *name=(cc_str){tile.data+u32(p),u32(p+4)};
        }
        return 1;
    }
    cc_wire r=cc_wire_init(tile.data,tile.size); uint64_t n,len;
    *name=(cc_str){0,0};
    if(!native_var(&r,&n) || index>n) return 0;
    if(!index) return 1;
    for(uint32_t i=1;i<=index;i++) {
        if(!native_var(&r,&len) || len>(uint64_t)(r.end-r.p)) return 0;
        if(i==index) { *name=(cc_str){r.p,(size_t)len}; return 1; } r.p+=(size_t)len;
    }
    return 0;
}
cc_style cc_ctile_style(unsigned c) {
    static const cc_style styles[CT_CLASS_COUNT]={
        {0,0,0},{CC_WATER,0,3},{0,CC_WATER,4},{0,CC_WATER,4},{0,CC_WATER,4},
        {CC_WOOD,0,1},{CC_PARK,0,2},{CC_FARM,0,1},{CC_RESIDENTIAL,0,1},{CC_INDUSTRIAL,0,1},
        {CC_BUILDING,0,5},{0,CC_MOTORWAY,12},{0,CC_MOTORWAY,11},{0,CC_MAJOR,10},
        {0,CC_SECONDARY,9},{0,CC_SECONDARY,8},{0,CC_MINOR,7},{0,CC_SERVICE,6},{0,CC_SERVICE,6},
        {0,CC_RAIL,8},{0,CC_BOUNDARY,6},{0,0,0}};
    if(c==CT_CONTOUR) return (cc_style){0,CC_CONTOUR,2};
    if(c==CT_INDEX_CONTOUR) return (cc_style){0,CC_INDEX_CONTOUR,2};
    return c<CT_CLASS_COUNT?styles[c]:(cc_style){0,0,0};
}
static void append(cc_scene *s,cc_feature *f) {
    unsigned p=f->style.priority,r=f->label_class;
    if(s->last[p]) s->last[p]->next=f; else s->first[p]=f;
    s->last[p]=f; s->features++;
    if(r && f->name.size) {
        if(s->label_last[r][p]) s->label_last[r][p]->label_next=f; else s->label_first[r][p]=f;
        s->label_last[r][p]=f;
    }
    s->labels_indexed=1;
}
static int scene3(cc_str tile,int64_t ox,int64_t oy,int w,int h,cc_arena *a,cc_scene *s) {
    const uint8_t *b=tile.data; uint32_t nf=u32(b+8),np=u32(b+12),nv=u32(b+16),fo=u32(b+24),po=u32(b+28);
    const uint8_t *xs=b+po+np*8,*ys=xs+nv*2;
    if(ox<INT32_MIN+1073741824LL || ox>INT32_MAX-1073741824LL || oy<INT32_MIN+1073741824LL || oy>INT32_MAX-1073741824LL) return 0;
    for(uint32_t i=0;i<nf;i++) {
        const uint8_t *p=b+fo+i*40;
        cc_point lo={(int32_t)ox+i16(p+24),(int32_t)oy+i16(p+26)},hi={(int32_t)ox+i16(p+28),(int32_t)oy+i16(p+30)};
        /* Off-screen polygon/point labels only matter when their independent
           anchor can reserve an interior label row/cell. */
        if(w && (hi.x<0 || lo.x>=w || hi.y<0 || lo.y>=h)) {
            if(!p[2] || p[1]==2) continue;
            int64_t ax=ox+(int32_t)u32(p+16),ay=oy+(int32_t)u32(p+20);
            if(ax<2 || ax>=w-2 || ay<4 || ay>=h-4) continue;
        }
        cc_feature *f=cc_arena_alloc(a,sizeof(*f),_Alignof(cc_feature)); if(!f) return 0;
        memset(f,0,sizeof(*f)); f->type=p[1]; f->label_class=p[2]; f->style=cc_ctile_style(p[0]);
        f->bounds_min=lo; f->bounds_max=hi; f->bounds_valid=1;
        int64_t ax=ox+(int32_t)u32(p+16),ay=oy+(int32_t)u32(p+20);
        if(ax<INT32_MIN || ax>INT32_MAX || ay<INT32_MIN || ay>INT32_MAX) return 0;
        f->anchor=(cc_point){(int32_t)ax,(int32_t)ay};
        if(!cc_ctile_name(tile,u32(p+4),&f->name)) return 0;
        cc_label_metadata(f);
        f->count=u32(p+12); f->paths=cc_arena_array(a,f->count,sizeof(cc_path),_Alignof(cc_path)); if(!f->paths) return 0;
        for(size_t j=0;j<f->count;j++) {
            const uint8_t *q=b+po+(u32(p+8)+j)*8; uint32_t first=u32(q),n=u32(q+4);
            f->paths[j]=(cc_path){0,n,f->type==3,xs+first*2,ys+first*2,(int32_t)ox,(int32_t)oy}; s->points+=n;
        }
        if(u32(p+32)) f->raster=b+u32(p+32);
        append(s,f);
    }
    return 1;
}
static int scene4(cc_str tile,int64_t ox,int64_t oy,int w,int h,cc_arena *a,cc_scene *s) {
    const uint8_t *b=tile.data; uint32_t nf=u32(b+8),np=u32(b+12),fo=u32(b+24),po=u32(b+28);
    if(ox<INT32_MIN+1073741824LL || ox>INT32_MAX-1073741824LL || oy<INT32_MIN+1073741824LL || oy>INT32_MAX-1073741824LL) return 0;
    const uint8_t *coords=b+po+np*4; cc_point *points=0;
    if(b[2]=='5') {
        points=cc_arena_array(a,(size_t)u32(b+16),sizeof(cc_point),_Alignof(cc_point)); if(!points) return 0;
        const uint8_t *p=coords+4; size_t at=0;
        for(uint32_t j=0;j<np;j++) {
            unsigned n=(uint16_t)i16(b+po+j*4+2); int x=0,y=0;
            for(unsigned i=0;i<n;i++) {
                int *v[2]={&x,&y};
                for(unsigned k=0;k<2;k++) { int d=(int8_t)*p++; if(d==-128) { *v[k]=i16(p); p+=2; } else *v[k]+=d; }
                points[at++]=(cc_point){(int32_t)ox+x,(int32_t)oy+y};
            }
        }
    }
    for(uint32_t i=0;i<nf;i++) {
        const uint8_t *p=b+fo+i*24;
        cc_point lo={(int32_t)ox+bound4(p,0),(int32_t)oy+bound4(p,1)},hi={(int32_t)ox+bound4(p,2),(int32_t)oy+bound4(p,3)};
        if(w && (hi.x<0 || lo.x>=w || hi.y<0 || lo.y>=h)) {
            if(!p[2] || p[1]==2) continue;
            const uint8_t *anchor=b+u32(p+20);
            int64_t ax=ox+(int32_t)u32(anchor),ay=oy+(int32_t)u32(anchor+4);
            if(ax<2 || ax>=w-2 || ay<4 || ay>=h-4) continue;
        }
        cc_feature *f=cc_arena_alloc(a,sizeof(*f),_Alignof(cc_feature)); if(!f) return 0;
        memset(f,0,sizeof(*f)); f->type=p[1]; f->label_class=p[2]; f->style=cc_ctile_style(p[0]); f->terrain_feature=p[0]>=CT_CONTOUR; f->bounds_min=lo; f->bounds_max=hi; f->bounds_valid=1;
        if(p[2]) {
            const uint8_t *anchor=b+u32(p+20); int64_t ax=ox+(int32_t)u32(anchor),ay=oy+(int32_t)u32(anchor+4);
            if(ax<INT32_MIN || ax>INT32_MAX || ay<INT32_MIN || ay>INT32_MAX) return 0;
            f->anchor=(cc_point){(int32_t)ax,(int32_t)ay};
        }
        if(!cc_ctile_name(tile,(uint16_t)i16(p+4),&f->name)) return 0;
        cc_label_metadata(f); f->count=(uint16_t)i16(p+8);
        f->paths=cc_arena_array(a,f->count,sizeof(cc_path),_Alignof(cc_path)); if(!f->paths) return 0;
        for(size_t j=0;j<f->count;j++) {
            const uint8_t *q=b+po+((uint16_t)i16(p+6)+j)*4; unsigned first=(uint16_t)i16(q),n=(uint16_t)i16(q+2);
            f->paths[j]=points?(cc_path){points+first,n,f->type==3,0,0,0,0}:(cc_path){0,n,f->type==3,coords+first*3,0,(int32_t)ox,(int32_t)oy}; s->points+=n;
        }
        if(u32(p+16)) { f->raster=b+u32(p+16); f->raster_format=p[3]; }
        append(s,f);
    }
    return 1;
}
static int scene6(cc_str tile,int64_t ox,int64_t oy,int w,int h,cc_arena *a,cc_scene *s) {
    uint32_t ng=u32(tile.data+4),nc=u32(tile.data+8);
    cc_str g={tile.data+16,ng};
    if(!expand(&g,a) || !(v4(g)?scene4(g,ox,oy,w,h,a,s):scene3(g,ox,oy,w,h,a,s))) return 0;
    if(nc && !scene4((cc_str){tile.data+16+ng,nc},ox,oy,w,h,a,s)) return 0;
    const uint8_t *r=tile.data+16+ng+nc;
    if(!memcmp(r,"SH1\0",4) && r[6]==2) return 1;
    cc_terrain *t=cc_arena_alloc(a,sizeof(*t),_Alignof(cc_terrain)); if(!t) return 0;
    *t=(cc_terrain){.next=s->terrain,.runs=r,.ox=(int32_t)ox,.oy=(int32_t)oy}; s->terrain=t;
    if(!memcmp(r,"SH1\0",4)) {
        t->sx=r[4]; t->sy=r[5];
        unsigned count=((256u>>r[4])+1)*((256u>>r[5])+1),at=0,q=8;
        uint8_t *raw=cc_arena_alloc(a,count,1); if(!raw) return 0;
        if(r[6]==1) {
            for(unsigned i=8;i<u32(tile.data+12);i++) for(unsigned k=0;k<(unsigned)(r[i]>>4)+1;k++) {
                q=(q+(r[i]&15))&15; raw[at++]=(uint8_t)q;
            }
        } else for(unsigned i=0;i<count;i++) raw[i]=(uint8_t)((r[8+i/2]>>((i&1)*4))&15);
        t->runs=raw;
    }
    return 1;
}
int cc_ctile_tile_scene(cc_str tile,int64_t ox,int64_t oy,cc_arena *a,cc_scene *s) {
    if(ox<INT32_MIN+1073741824LL || ox>INT32_MAX-1073741824LL ||
       oy<INT32_MIN+1073741824LL || oy>INT32_MAX-1073741824LL) return 0;
    if(zstd_tile(tile) && !cc_ctile_decode_tile(tile,a,&tile)) return 0;
    if(v6(tile)) return validate6(tile) && scene6(tile,ox,oy,0,0,a,s);
    if(!expand(&tile,a)) return 0;
    if(v4(tile)) return validate4(tile) && scene4(tile,ox,oy,0,0,a,s);
    if(v3(tile)) return validate3(tile) && scene3(tile,ox,oy,0,0,a,s);
    cc_wire r=cc_wire_init(tile.data,tile.size); uint64_t n,v,strings;
    if(!native_var(&r,&n) || n>tile.size) return 0;
    strings=n;
    if(n>=SIZE_MAX || n>(uint64_t)(r.end-r.p)) return 0;
    cc_str *names=cc_arena_array(a,(size_t)n+1,sizeof(cc_str),_Alignof(cc_str)); if(!names) return 0;
    names[0]=(cc_str){0,0};
    for(uint64_t i=1;i<=n;i++) { if(!native_var(&r,&v) || v>(uint64_t)(r.end-r.p)) return 0; names[i]=(cc_str){r.p,(size_t)v}; r.p+=(size_t)v; }
    for(unsigned c=1;c<CT_CLASS_COUNT;c++) {
        if(!native_var(&r,&n) || n>tile.size) return 0;
        for(uint64_t i=0;i<n;i++) {
            uint64_t type,name,np,rank,ax,ay; cc_feature *f;
            if(!native_var(&r,&type) || type<1 || type>3 || !native_var(&r,&name) || name>strings || !native_var(&r,&rank) || rank>5 || !native_var(&r,&ax) || !native_var(&r,&ay) || !native_var(&r,&np) || np>tile.size) return 0;
            f=cc_arena_alloc(a,sizeof(*f),_Alignof(cc_feature)); if(!f) return 0;
            memset(f,0,sizeof(*f)); f->type=(uint32_t)type; f->style=cc_ctile_style(c); f->count=(size_t)np; f->name=names[name]; f->label_class=(uint8_t)rank;
            cc_label_metadata(f);
            int64_t gx=native_zig(ax),gy=native_zig(ay);
            if(gx< -INT64_C(4294967296) || gx>INT64_C(4294967296) || gy< -INT64_C(4294967296) || gy>INT64_C(4294967296)) return 0;
            gx=ox+gx/4-(gx<0 && gx%4!=0); gy=oy+gy/4-(gy<0 && gy%4!=0);
            if(gx<INT32_MIN || gx>INT32_MAX || gy<INT32_MIN || gy>INT32_MAX) return 0;
            f->anchor=(cc_point){(int32_t)gx,(int32_t)gy};
            f->bounds_min=(cc_point){INT32_MAX,INT32_MAX}; f->bounds_max=(cc_point){INT32_MIN,INT32_MIN}; f->bounds_valid=1;
            if(np>(uint64_t)(r.end-r.p)) return 0;
            f->paths=cc_arena_array(a,(size_t)np,sizeof(cc_path),_Alignof(cc_path)); if(!f->paths) return 0;
            for(size_t j=0;j<f->count;j++) {
                uint64_t cnt; int64_t x=0,y=0;
                if(!native_var(&r,&cnt) || cnt>(uint64_t)(r.end-r.p)/2) return 0;
                f->paths[j]=(cc_path){cc_arena_array(a,(size_t)cnt,sizeof(cc_point),_Alignof(cc_point)),(size_t)cnt,type==3,0,0,0,0};
                if(!f->paths[j].points) return 0;
                for(size_t k=0;k<(size_t)cnt;k++) {
                    int64_t qx,qy;
                    if(!native_var(&r,&v)) return 0;
                    if(native_zig(v)<-1088 || native_zig(v)>1088) return 0;
                    x+=native_zig(v);
                    if(!native_var(&r,&v)) return 0;
                    if(native_zig(v)<-1088 || native_zig(v)>1088) return 0;
                    y+=native_zig(v);
                    if(x<-32 || x>1056 || y<-32 || y>1056) return 0;
                    qx=ox+x/4-(x<0 && x%4!=0); qy=oy+y/4-(y<0 && y%4!=0);
                    if(qx<INT32_MIN || qx>INT32_MAX || qy<INT32_MIN || qy>INT32_MAX) return 0;
                    f->paths[j].points[k]=(cc_point){(int32_t)qx,(int32_t)qy};
                    if(qx<f->bounds_min.x) f->bounds_min.x=(int32_t)qx;
                    if(qx>f->bounds_max.x) f->bounds_max.x=(int32_t)qx;
                    if(qy<f->bounds_min.y) f->bounds_min.y=(int32_t)qy;
                    if(qy>f->bounds_max.y) f->bounds_max.y=(int32_t)qy;
                }
                s->points+=(size_t)cnt;
            }
            if(s->last[f->style.priority]) s->last[f->style.priority]->next=f; else s->first[f->style.priority]=f;
            s->last[f->style.priority]=f; s->features++;
            unsigned priority=f->style.priority;
            if(rank && f->name.size) {
                if(s->label_last[rank][priority]) s->label_last[rank][priority]->label_next=f; else s->label_first[rank][priority]=f;
                s->label_last[rank][priority]=f;
            }
            s->labels_indexed=1;
        }
    }
    return !r.error && r.p==r.end;
}

static void put32(uint8_t *p,uint32_t n) { p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); p[2]=(uint8_t)(n>>16); p[3]=(uint8_t)(n>>24); }
static void put16(uint8_t *p,int n) { p[0]=(uint8_t)n; p[1]=(uint8_t)((unsigned)n>>8); }
int cc_ctile_decode_tile(cc_str tile,cc_arena *a,cc_str *out) {
    size_t mark=a->used;
    if(zstd_tile(tile) && (!cc_zstd_decode(tile,a,&tile) || !v6(tile))) goto fail;
    if(v6(tile)) {
        if(!validate6(tile)) goto fail;
        uint32_t ng=u32(tile.data+4); cc_str inner;
        if(!cc_ctile_decode_tile((cc_str){tile.data+16,ng},a,&inner)) goto fail;
        if(inner.data==tile.data+16) { *out=tile; return 1; }
        size_t total=tile.size-ng+inner.size;
        if(total>UINT32_MAX) goto fail;
        uint8_t *d=cc_arena_alloc(a,total,1); if(!d) goto fail;
        memcpy(d,tile.data,16); put32(d+4,(uint32_t)inner.size); memcpy(d+16,inner.data,inner.size);
        memcpy(d+16+inner.size,tile.data+16+ng,tile.size-16-ng);
        *out=(cc_str){d,total}; return 1;
    }
    int packed=compressed(tile);
    if(!expand(&tile,a)) goto fail;
    if(!packed && !(v4(tile)?validate4(tile):validate3(tile))) goto fail;
    if(!v4(tile) || tile.data[2]!='5') { *out=tile; return 1; }
    const uint8_t *b=tile.data;
    uint32_t ns=u32(b+4),nf=u32(b+8),np=u32(b+12),nv=u32(b+16),fo=u32(b+24),po=u32(b+28),no=u32(b+20);
    /* Preserve even arbitrary validated feature order. Builder compact tiles
       are class ordered; unordered CT5 is retained in its original form. */
    for(uint32_t i=1;i<nf;i++) if(b[fo+i*24]<b[fo+(i-1)*24]) { *out=tile; return 1; }
    size_t names=0;
    for(uint32_t i=0;i<ns;i++) { size_t n=u32(b+no+i*8+4); if(n>UINT32_MAX-names) goto fail; names+=n; }
    uint64_t total=200+(uint64_t)ns*8+(uint64_t)nf*40+(uint64_t)np*8+(uint64_t)nv*4+names;
    if(total>UINT32_MAX || total>SIZE_MAX) goto fail;
    uint8_t *d=cc_arena_alloc(a,(size_t)total,4); if(!d) goto fail;
    memset(d,0,200); memcpy(d,"CT3\0",4);
    put32(d+4,ns); put32(d+8,nf); put32(d+12,np); put32(d+16,nv);
    uint32_t nout=200,fout=nout+ns*8,pout=fout+nf*40,xout=pout+np*8,yout=xout+nv*2,strout=yout+nv*2;
    put32(d+20,nout); put32(d+24,fout); put32(d+28,pout);
    for(uint32_t i=0;i<ns;i++) { const uint8_t *q=b+no+i*8; uint32_t n=u32(q+4); put32(d+nout+i*8,strout); put32(d+nout+i*8+4,n); memcpy(d+strout,b+u32(q),n); strout+=n; }
    uint32_t at=0;
    for(unsigned c=1;c<CT_CLASS_COUNT;c++) {
        uint32_t first=at; while(at<nf && b[fo+at*24]==c) at++;
        put32(d+32+(c-1)*8,first); put32(d+36+(c-1)*8,at-first);
    }
    for(uint32_t i=0;i<nf;i++) {
        const uint8_t *f=b+fo+i*24; uint8_t *q=d+fout+i*40; memset(q,0,40);
        memcpy(q,f,3); put32(q+4,(uint16_t)i16(f+4)); put32(q+8,(uint16_t)i16(f+6)); put32(q+12,(uint16_t)i16(f+8));
        if(f[2]) memcpy(q+16,b+u32(f+20),8);
        for(unsigned k=0;k<4;k++) put16(q+24+k*2,bound4(f,k));
    }
    const uint8_t *v=b+po+np*4+4; at=0;
    for(uint32_t j=0;j<np;j++) {
        unsigned n=(uint16_t)i16(b+po+j*4+2); int x=0,y=0;
        put32(d+pout+j*8,at); put32(d+pout+j*8+4,n);
        for(unsigned i=0;i<n;i++,at++) {
            int dx=(int8_t)*v++; if(dx==-128) { x=i16(v); v+=2; } else x+=dx;
            int dy=(int8_t)*v++; if(dy==-128) { y=i16(v); v+=2; } else y+=dy;
            put16(d+xout+at*2,x); put16(d+yout+at*2,y);
        }
    }
    *out=(cc_str){d,(size_t)total}; return 1;
fail:
    a->used=mark; *out=(cc_str){0,0}; return 0;
}
int cc_ctile_scene_cached(const cc_ctile *f,unsigned z,int64_t left,int64_t top,int32_t cols,int32_t rows,cc_arena *a,cc_scene *s,cc_decoded_cache *cache) {
    if(f->cellset) return cc_cellset_scene(f->cellset,z,left,top,cols,rows,a,s,cache);
    size_t mark=a->used; int64_t x0,y0,x1,y1,limit;
    if(cache) cc_decoded_cache_begin(cache);
    memset(s,0,sizeof(*s));
    if(z>f->zmax && z<=16 && cols>0 && rows>0) {
        unsigned shift=z-f->zmax; int factor=1<<shift;
        int64_t pl=left/factor-(left<0 && left%factor),pt=top/factor-(top<0 && top%factor);
        if(!cc_ctile_scene_cached(f,f->zmax,pl,pt,(cols+factor-1)/factor+1,(rows+factor-1)/factor+1,a,s,cache) ||
           !cc_scene_overzoom(s,shift,(int32_t)(left-pl*factor),(int32_t)(top-pt*factor),a)) {
            a->used=mark; memset(s,0,sizeof(*s)); return 0;
        }
        return 1;
    }
    if(z<f->zmin || z>f->zmax || cols<1 || rows<1 || cols>16384 || rows>16384) return 0;
    limit=INT64_C(1)<<z;
    if(left<-32768 || top<-32768 || left>limit*256+32768 || top>limit*256+32768) return 0;
    x0=(left-8)/256; y0=(top-8)/256; x1=(left+cols*2+8)/256; y1=(top+rows*4+8)/256;
    if(x0<0) x0=0;
    if(y0<0) y0=0;
    if(x1>=limit) x1=limit-1;
    if(y1>=limit) y1=limit-1;
    for(int64_t x=x0;x<=x1;x++) for(int64_t y=y0;y<=y1;y++) {
        cc_str tile;
        uint64_t stamp=cc_profiling?cc_ticks():0;
        int found;
        if(f->source) {
            uint64_t off; uint32_t len;
            found=cc_ctile_locate(f,z,(uint32_t)x,(uint32_t)y,&off,&len);
            if(found==1 && !cc_ctile_source_tile(f,off,len,a,cache,&tile)) found=-1;
        } else found=cc_ctile_find(f,z,(uint32_t)x,(uint32_t)y,&tile);
        if(cc_profiling) { cc_profiling->ticks[CC_LOOKUP]+=cc_ticks()-stamp; stamp=cc_ticks(); }
        if(found<0 || (found && ((!f->source && !(cache?cc_decoded_cache_get(cache,tile,a,&tile):expand(&tile,a))) || !(v6(tile)?scene6(tile,x*256-left,y*256-top,cols*2,rows*4,a,s):v4(tile)?scene4(tile,x*256-left,y*256-top,cols*2,rows*4,a,s):v3(tile)?scene3(tile,x*256-left,y*256-top,cols*2,rows*4,a,s):cc_ctile_tile_scene(tile,x*256-left,y*256-top,a,s))))) { a->used=mark; memset(s,0,sizeof(*s)); return 0; }
        if(cc_profiling) cc_profiling->ticks[CC_DECODE]+=cc_ticks()-stamp;
    }
    return 1;
}

int cc_ctile_scene(const cc_ctile *f,unsigned z,int64_t left,int64_t top,int32_t cols,int32_t rows,cc_arena *a,cc_scene *s) {
    return cc_ctile_scene_cached(f,z,left,top,cols,rows,a,s,0);
}
