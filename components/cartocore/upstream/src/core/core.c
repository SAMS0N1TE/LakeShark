#include "cartocore/arch.h"
#include "cartocore/core.h"
#include <string.h>
#include "cartocore/profile.h"
#include "cartocore/simd.h"
int cc_native_rasters=1;
static uint32_t read32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static unsigned read16(const uint8_t *p) { return (unsigned)p[0]|(unsigned)p[1]<<8; }
static void packed_fill(cc_planes *p,const cc_feature *f,int vx,int vy,int phase) {
    const uint8_t *r=f->raster+(int32_t)read32(f->raster+phase*4);
    unsigned runs=read16(r),edges=read16(r+2); r+=4;
    uint8_t v[3]={0},ink=f->style.fill,priority=f->style.priority; unsigned repeat=0;
    for(unsigned i=0;i<runs+edges;i++) {
        unsigned edge=i>=runs;
        if(i==runs) repeat=0;
        if(repeat) { v[edge?1:0]++; repeat--; }
        else if(*r==255) { repeat=r[1]-1; r+=2; v[edge?1:0]++; }
        else { memcpy(v,r,3); r+=3; }
        int y=vy+(edge?v[0]&127:v[0]),x=vx+v[1];
        if(y<0 || y>=p->rows) continue;
        if(edge) {
            if(x<0 || x>=p->cols) continue;
            size_t at=(size_t)y*p->cols+x; unsigned bits=v[2];
            if(p->edges_mode!=CC_EDGES_CRISP && priority>=p->fill_priority[at] && priority>=p->coverage_priority[at]) {
                if(p->coverage_ink[at]==ink) bits|=p->coverage[at]; else p->coverage_bg[at]=p->fill[at];
                p->coverage[at]=(uint8_t)bits; p->coverage_ink[at]=ink; p->coverage_priority[at]=priority;
            }
            if((v[0]&128) && priority>=p->fill_priority[at]) { p->fill[at]=ink; p->fill_priority[at]=priority; }
        } else {
            int end=x+v[2]; if(x<0) x=0; if(end>p->cols) end=p->cols;
            for(;x<end;x++) { size_t at=(size_t)y*p->cols+x;
                if(priority>=p->fill_priority[at]) {
                    p->fill[at]=ink; p->fill_priority[at]=priority;
                    if(p->edges_mode!=CC_EDGES_CRISP && priority>=p->coverage_priority[at]) p->coverage_ink[at]=p->coverage_priority[at]=0;
                }
            }
        }
    }
}
static void native_fill(cc_planes *p,const cc_feature *f) {
    int ox=f->paths[0].ox,oy=f->paths[0].oy;
    int px=(-ox)%2,py=(-oy)%4; if(px<0) px+=2; if(py<0) py+=4;
    int vx=(ox-12+px)/2,vy=(oy-12+py)/4;
    if(f->raster_format==1) { packed_fill(p,f,vx,vy,py*2+px); return; }
    const uint8_t *h=f->raster+(py*2+px)*8,*r=f->raster+read32(h);
    unsigned runs=read16(h+4),edges=read16(h+6); uint8_t ink=f->style.fill,priority=f->style.priority;
    const uint8_t *e=r+runs*3;
    uint64_t stamp=cc_profiling?cc_ticks():0;
    for(unsigned i=0;i<edges;i++,e+=3) {
        int y=vy+(e[0]&127),x=vx+e[1];
        if(x<0 || x>=p->cols || y<0 || y>=p->rows) continue;
        size_t at=(size_t)y*p->cols+x; unsigned bits=e[2];
        if(p->edges_mode!=CC_EDGES_CRISP && priority>=p->fill_priority[at] && priority>=p->coverage_priority[at]) {
            if(p->coverage_ink[at]==ink) bits|=p->coverage[at]; else p->coverage_bg[at]=p->fill[at];
            p->coverage[at]=(uint8_t)bits; p->coverage_ink[at]=ink; p->coverage_priority[at]=priority;
        }
        if((e[0]&128) && priority>=p->fill_priority[at]) { p->fill[at]=ink; p->fill_priority[at]=priority; }
    }
    if(cc_profiling) cc_profiling->ticks[CC_COVERAGE]+=cc_ticks()-stamp;
    for(unsigned i=0;i<runs;i++,r+=3) {
        int y=vy+r[0],x=vx+r[1],end=x+r[2];
        if(y<0 || y>=p->rows || end<=0 || x>=p->cols) continue;
        if(x<0) x=0;
        if(end>p->cols) end=p->cols;
        for(;x<end;x++) {
            size_t at=(size_t)y*p->cols+x;
            if(priority>=p->fill_priority[at]) {
                p->fill[at]=ink; p->fill_priority[at]=priority;
                if(p->edges_mode!=CC_EDGES_CRISP && priority>=p->coverage_priority[at]) p->coverage_ink[at]=p->coverage_priority[at]=0;
            }
        }
    }
}
void cc_arena_init(cc_arena *a, void *buffer, size_t size) {
    a->data = buffer; a->size = size; a->used = a->peak = 0;
}
void *cc_arena_alloc(cc_arena *a, size_t bytes, size_t alignment) {
    size_t pad;
    if (!alignment || (alignment & (alignment - 1))) return 0;
    pad = (size_t)(0 - ((uintptr_t)a->data + a->used)) & (alignment - 1);
    if (a->used > a->size || pad > a->size - a->used ||
        bytes > a->size - a->used - pad) return 0;
    a->used += pad;
    { void *p = a->data + a->used; a->used += bytes; if(a->used>a->peak) a->peak=a->used; return p; }
}
int cc_str_eq(cc_str s, const char *v) {
    size_t i;
    for (i = 0; i < s.size; ++i) if (!v[i] || s.data[i] != (uint8_t)v[i]) return 0;
    return v[i] == 0;
}
int cc_project(int64_t x, int32_t scale, int32_t offset, int32_t *out) {
    int64_t q;
    if (scale < 0 || (scale && (x > INT64_MAX / scale || x < INT64_MIN / scale))) return 0;
    q = x * scale;
    if ((offset > 0 && q > INT64_MAX - offset) ||
        (offset < 0 && q < INT64_MIN - offset)) return 0;
    q += offset;
    q = q / 65536 - (q < 0 && q % 65536 != 0);
    if (q < INT32_MIN || q > INT32_MAX) return 0;
    *out = (int32_t)q; return 1;
}
int cc_planes_init_split(cc_planes *p, cc_arena *a, cc_arena *cold, int32_t cols, int32_t rows) {
    size_t n, mark = a->used, cm=cold->used;
    if (cols < 1 || rows < 1 || cols > 16384 || rows > 16384) return 0;
    p->terrain_strength=p->terrain_active=0; p->terrain_phase_x=p->terrain_phase_y=0; p->terrain_palette_strength=-1;
    p->palette=NULL; p->ordered=0; p->colors16=0; p->edges_mode=CC_EDGES_SMOOTH; p->native_unclipped_lines=0; p->cols = cols; p->rows = rows; n = (size_t)cols * (size_t)rows;
    p->words = ((size_t)cols + CC_WORD_BITS) / CC_WORD_BITS;
    p->dots = cc_arena_alloc(a, n, 1); p->ink = cc_arena_alloc(a, n, 1);
    p->dot_priority = cc_arena_alloc(a, n, 1); p->fill = cc_arena_alloc(a, n, 1);
    p->fill_priority = cc_arena_alloc(a, n, 1);
    p->edges = cc_arena_alloc(a, p->words * (size_t)rows * sizeof(cc_bitword), _Alignof(cc_bitword));
    p->dot_edges = cc_arena_alloc(cold, ((size_t)cols*2+64)/64 * (size_t)rows*4 * sizeof(uint64_t), _Alignof(uint64_t));
    p->collision = cc_arena_alloc(a, p->words * (size_t)rows * sizeof(cc_bitword), _Alignof(cc_bitword));
    /* Each accepted label reserves at least nine disjoint collision cells. */
    p->label_capacity=1; while(p->label_capacity<n/9+1) p->label_capacity*=2;
    p->label_seen=cc_arena_alloc(a,p->label_capacity*sizeof(cc_feature*),_Alignof(cc_feature*));
    p->edge_list = cc_arena_alloc(cold,n*sizeof(uint32_t),_Alignof(uint32_t));
    p->shade=cc_arena_alloc(cold,n,1);
    p->edge_cells = cc_arena_alloc(cold,n,1);
    p->coverage = cc_arena_alloc(cold,n,1); p->coverage_ink = cc_arena_alloc(cold,n,1);
    p->coverage_bg = cc_arena_alloc(cold,n,1); p->coverage_priority = cc_arena_alloc(cold,n,1);
    if (!p->shade || !p->dots || !p->ink || !p->dot_priority || !p->fill || !p->fill_priority || !p->edges || !p->dot_edges || !p->collision || !p->edge_list || !p->edge_cells || !p->coverage || !p->coverage_ink || !p->coverage_bg || !p->coverage_priority || !p->label_seen) {
        a->used = mark; cold->used=cm; return 0;
    }
    cc_planes_clear(p); return 1;
}
int cc_planes_init(cc_planes *p,cc_arena *a,int32_t cols,int32_t rows) {
    return cc_planes_init_split(p,a,a,cols,rows);
}
void cc_planes_clear(cc_planes *p) {
    size_t n = (size_t)p->cols * (size_t)p->rows;
    cc_arch_clear(p->dots, n); cc_arch_clear(p->ink, n); cc_arch_clear(p->dot_priority, n);
    cc_arch_set(p->fill, CC_LAND, n); cc_arch_clear(p->fill_priority, n);
    if(p->edges_mode!=CC_EDGES_CRISP) {
        cc_arch_clear(p->coverage_ink,n); cc_arch_clear(p->coverage_priority,n);
    }
    cc_arch_clear(p->collision,p->words*(size_t)p->rows*sizeof(cc_bitword));
}
/* Transactional three-row padded reservation. Each run uses word-wide ANDs. */
int cc_label_reserve(cc_planes *p,int32_t x,int32_t y,int32_t length) {
    if(length<=0 || length>p->cols-2 || x<1 || x>p->cols-length-1 || y<1 || y>=p->rows-1) return 0;
    int32_t lo=x-1,hi=x+length+1;
    for(int pass=0;pass<2;pass++) for(int row=y-1;row<=y+1;row++) {
        for(int at=lo;at<hi;) {
            unsigned bit=(unsigned)at%CC_WORD_BITS,take=(unsigned)(hi-at); cc_bitword mask,*word;
            if(take>CC_WORD_BITS-bit) take=CC_WORD_BITS-bit;
            mask=(CC_WORD_MAX>>(CC_WORD_BITS-take))<<bit;
            word=p->collision+(size_t)row*p->words+(unsigned)at/CC_WORD_BITS;
            if(!pass) { if(*word&mask) return 0; } else *word|=mask;
            at+=(int)take;
        }
    }
    return 1;
}
uint8_t cc_dot_bit(int32_t x, int32_t y) {
    static const uint8_t bits[2][4] = {{1,2,4,64},{8,16,32,128}};
    return bits[(uint32_t)x & 1][(uint32_t)y & 3];
}
static inline void dot(cc_planes *p, int32_t x, int32_t y, uint8_t ink, uint8_t priority) {
    size_t at;
    if (x < 0 || y < 0 || x >= p->cols * 2 || y >= p->rows * 4) return;
    at = (size_t)(y / 4) * (size_t)p->cols + (size_t)(x / 2);
    if((ink==CC_CONTOUR || ink==CC_INDEX_CONTOUR) && p->fill[at]==CC_WATER) return;
    /* A cell has one foreground ink. Never recolour lower-priority geometry
       as a wide road when a different line class claims that cell. */
    if(ink!=p->ink[at]) {
        if(priority<p->dot_priority[at]) return;
        p->dots[at]=0;
    }
    p->dots[at] |= cc_dot_bit(x,y);
    if (priority >= p->dot_priority[at]) {
        p->ink[at] = ink; p->dot_priority[at] = priority;
    }
}
void cc_dot(cc_planes *p,int32_t x,int32_t y,uint8_t ink,uint8_t priority) { dot(p,x,y,ink,priority); }
static unsigned region(cc_point a, int32_t w, int32_t h) {
    return (a.x < 0 ? 1u : a.x >= w ? 2u : 0u) |
           (a.y < 0 ? 4u : a.y >= h ? 8u : 0u);
}
/* Differences of int32 coordinates fit unsigned products, including extremes. */
static int64_t muldiv(int64_t a, int64_t b, int64_t c) {
    if(a>=-32767 && a<=32767 && b>=-32767 && b<=32767 && c>=INT32_MIN && c<=INT32_MAX) return (int32_t)a*(int32_t)b/(int32_t)c;
    int neg = (a < 0) ^ (b < 0) ^ (c < 0);
    uint64_t ua = (uint64_t)(a < 0 ? -a : a);
    uint64_t ub = (uint64_t)(b < 0 ? -b : b);
    uint64_t uc = (uint64_t)(c < 0 ? -c : c);
    int64_t q = (int64_t)(ua * ub / uc);
    return neg ? -q : q;
}
int cc_clip_line(int32_t w, int32_t h, cc_point *a, cc_point *b) {
    if (w <= 0 || h <= 0) return 0;
    for (unsigned step = 0; step < 16; ++step) {
        unsigned ca = region(*a,w,h), cb = region(*b,w,h), c;
        int64_t dx = (int64_t)b->x-a->x, dy = (int64_t)b->y-a->y, x, y;
        cc_point q;
        if (!(ca | cb)) return 1;
        if (ca & cb) return 0;
        c = ca ? ca : cb;
        if (c & (4|8)) {
            y = c & 4 ? 0 : h-1;
            x = a->x + muldiv(dx,y-a->y,dy);
        } else {
            x = c & 1 ? 0 : w-1;
            y = a->y + muldiv(dy,x-a->x,dx);
        }
        q.x = (int32_t)x; q.y = (int32_t)y;
        if (ca) *a = q; else *b = q;
    }
    return 0;
}
/* Clipping proves every Bresenham sample is in bounds. Hoist plane pointers
   and dimensions; the public/native-unclipped path keeps checked plotting. */
static inline void line_inside(cc_planes *p,cc_point a,cc_point b,uint8_t ink,uint8_t priority) {
    if(ink==CC_CONTOUR || ink==CC_INDEX_CONTOUR) {
        /* Contours must not contribute any dots to a water cell, including
           cells whose river ink already has a higher priority. */
        int native=p->native_unclipped_lines; p->native_unclipped_lines=1;
        cc_line(p,a,b,ink,priority); p->native_unclipped_lines=native; return;
    }
    int dx=b.x>a.x?b.x-a.x:a.x-b.x,dy=b.y>a.y?b.y-a.y:a.y-b.y;
    int sx=a.x<b.x?1:-1,sy=a.y<b.y?1:-1,err=dx-dy,cols=p->cols;
    uint8_t *dots=p->dots,*inks=p->ink,*priorities=p->dot_priority;
    for(;;) {
        size_t at=(size_t)(a.y>>2)*cols+(unsigned)a.x/2;
        if(ink==inks[at] || priority>=priorities[at]) {
            if(ink!=inks[at]) dots[at]=0;
            dots[at]|=cc_dot_bit(a.x,a.y);
            if(priority>=priorities[at]) { inks[at]=ink; priorities[at]=priority; }
        }
        if(a.x==b.x && a.y==b.y) break;
        int twice=err*2;
        if(twice>-dy) { err-=dy; a.x+=sx; }
        if(twice<dx) { err+=dx; a.y+=sy; }
    }
}
void cc_line(cc_planes *p, cc_point a, cc_point b, uint8_t ink, uint8_t priority) {
    int32_t dx, dy, sx, sy, err;
    if(!p->native_unclipped_lines) {
        unsigned ca=region(a,p->cols*2,p->rows*4),cb=region(b,p->cols*2,p->rows*4);
        if(!(ca|cb)) line_inside(p,a,b,ink,priority);
        else if(!(ca&cb) && cc_clip_line(p->cols*2,p->rows*4,&a,&b)) line_inside(p,a,b,ink,priority);
        return;
    }
    dx = b.x > a.x ? b.x-a.x : a.x-b.x;
    dy = b.y > a.y ? b.y-a.y : a.y-b.y;
    sx = a.x < b.x ? 1 : -1; sy = a.y < b.y ? 1 : -1; err = dx-dy;
    for (;;) {
        int32_t twice;
        dot(p,a.x,a.y,ink,priority);
        if (a.x == b.x && a.y == b.y) break;
        twice = err*2;
        if (twice > -dy) { err -= dy; a.x += sx; }
        if (twice < dx) { err += dx; a.y += sy; }
    }
}
static int64_t ceildiv(int64_t n, int64_t d) {
    return n / d + (n > 0 && n % d != 0);
}
static unsigned trailing(cc_bitword v) {
#if defined(__GNUC__) || defined(__clang__)
#if CC_WORD_BITS == 32
    return (unsigned)__builtin_ctz(v);
#else
    return (unsigned)__builtin_ctzll(v);
#endif
#else
    unsigned n=0; while(!(v&1)) { n++; v>>=1; } return n;
#endif
}
/* The cell-centre plane stays cheap. Only edge cells consume dot coverage. */
static void boundary_coverage(cc_planes *p,const cc_path *paths,size_t count,
                              uint8_t ink,uint8_t priority,int first,int last,int minx,int maxx) {
    size_t dw=((size_t)p->cols*2+64)/64;
    int left=minx<0?0:minx/2,right=maxx>=p->cols*2?p->cols-1:maxx/2;
    size_t wfirst=(unsigned)left/32,wlast=(unsigned)right/32;
    size_t touched=0;
    for(int row=first;row<last;row++) cc_arch_clear(p->edge_cells+(size_t)row*p->cols+left,(size_t)(right-left+1));
    for(int y=first*4;y<last*4;y++) cc_arch_clear(p->dot_edges+(size_t)y*dw+wfirst,(wlast-wfirst+1)*sizeof(uint64_t));
    for(size_t k=0;k<count;k++) {
        const cc_path *r=paths+k;
        if(!r->closed || r->count<3) continue;
        for(size_t j=0;j<r->count;j++) {
            cc_point a=cc_path_point(r,j),b=cc_path_point(r,j+1==r->count?0:j+1);
            int64_t dy;
            if(a.y>b.y) { cc_point t=a; a=b; b=t; }
            dy=(int64_t)b.y-a.y;
            int start=a.y/4-(a.y<0 && a.y%4!=0),end=b.y/4-(b.y<0 && b.y%4!=0);
            if(start<first) start=first;
            if(end>=last) end=last-1;
            for(int row=start;row<=end;row++) {
                int64_t y0=(int64_t)row*4,y1=y0+4,x0,x1;
                if(y0<a.y) y0=a.y;
                if(y1>b.y) y1=b.y;
                x0=dy && y0!=a.y?a.x+muldiv((int64_t)b.x-a.x,y0-a.y,dy):a.x;
                x1=dy && y1!=b.y? a.x+muldiv((int64_t)b.x-a.x,y1-a.y,dy):b.x;
                if(x0>x1) { int64_t t=x0; x0=x1; x1=t; }
                x0=x0/2-(x0<0 && x0%2!=0); x1=x1/2-(x1<0 && x1%2!=0);
                if(x0<0) x0=0;
                if(x1>=p->cols) x1=p->cols-1;
                for(int64_t x=x0;x<=x1;x++) {
                    size_t at=(size_t)row*p->cols+(size_t)x;
                    if(!p->edge_cells[at]) { p->edge_cells[at]=1; p->edge_list[touched++]=((uint32_t)row<<16)|(uint32_t)x; }
                }
            }
            if(!dy) continue;
            int64_t lo=a.y,hi=b.y;
            if(lo<(int64_t)first*4) lo=(int64_t)first*4;
            if(hi>(int64_t)last*4) hi=(int64_t)last*4;
            int64_t dx=(int64_t)b.x-a.x;
            /* Quotient/remainder recurrence: two divisions per edge, none per dot row. */
            int fast=a.x>-1000000 && a.x<1000000 && b.x>-1000000 && b.x<1000000;
            int64_t den=2*dy,q=0,rem=0,step=0,srem=0;
            if(fast && dx) {
                int64_t n=2*((int64_t)a.x*(b.y-lo)+(int64_t)b.x*(lo-a.y))+dx-dy;
                q=n/den; rem=n%den; if(rem<0) { rem+=den; q--; }
                step=2*dx/den; srem=2*dx%den; if(srem<0) { srem+=den; step--; }
            }
            for(int64_t y=lo;y<hi;y++) {
                int64_t x;
                if(!dx) x=a.x;
                else if(fast) { x=q+(rem!=0); q+=step; rem+=srem; if(rem>=den) { rem-=den; q++; } }
                else {
                    int64_t num=(int64_t)a.x*(b.y-y)+(int64_t)b.x*(y-a.y);
                    int64_t qq=num/dy,rr=num%dy;
                    if(rr<0) { qq--; rr+=dy; }
                    x=qq+ceildiv(2*rr+dx-dy,2*dy);
                }
                if(x<0) x=0;
                if(x>p->cols*2) x=p->cols*2;
                p->dot_edges[(size_t)y*dw+(size_t)x/64]^=UINT64_C(1)<<(x%64);
            }
        }
    }
    for(int y=first*4;y<last*4;y++) {
        cc_prefix_words(p->dot_edges+(size_t)y*dw+wfirst,wlast-wfirst+1);
    }
    uint32_t cached=UINT32_MAX; uint8_t transposed[32]; int packed=0;
    for(size_t i=0;i<touched;i++) {
        int row=(int)(p->edge_list[i]>>16),x=(int)(p->edge_list[i]&65535); size_t at=(size_t)row*p->cols+x;
        if(priority<p->fill_priority[at] || priority<p->coverage_priority[at]) continue;
        static const uint8_t masks[4][4]={{0,1,8,9},{0,2,16,18},{0,4,32,36},{0,64,128,192}};
        size_t base=(size_t)row*4*dw+(unsigned)x/32; unsigned shift=(x%32)*2;
        uint32_t key=((uint32_t)row<<16)|((unsigned)x/32);
        if(key!=cached) {
            cached=key; packed=0;
            if(i+7<touched && (p->edge_list[i+7]>>16)==(unsigned)row && ((p->edge_list[i+7]&65535)/32)==(unsigned)x/32) {
                uint64_t rows[4]={p->dot_edges[base],p->dot_edges[base+dw],p->dot_edges[base+2*dw],p->dot_edges[base+3*dw]};
                cc_braille32(rows,transposed); packed=1;
            }
        }
        unsigned bits=packed?transposed[x%32]:masks[0][(p->dot_edges[base]>>shift)&3] |
            masks[1][(p->dot_edges[base+dw]>>shift)&3] |
            masks[2][(p->dot_edges[base+2*dw]>>shift)&3] |
            masks[3][(p->dot_edges[base+3*dw]>>shift)&3];
        if(p->coverage_ink[at]==ink) bits|=p->coverage[at];
        else p->coverage_bg[at]=p->fill[at];
        p->coverage[at]=(uint8_t)bits; p->coverage_ink[at]=ink; p->coverage_priority[at]=priority;
    }
}
static void fill_bounds(cc_planes *p, const cc_path *paths, size_t count, uint8_t ink, uint8_t priority,const cc_feature *feature) {
    int32_t first_row = p->rows, last_row = 0;
    int miny=p->rows*4,maxy=-1,minx=p->cols*2,maxx=-1;
    if(feature && feature->bounds_valid && p->edges_mode==CC_EDGES_CRISP) {
        minx=feature->bounds_min.x; miny=feature->bounds_min.y;
        maxx=feature->bounds_max.x; maxy=feature->bounds_max.y;
    } else for(size_t k=0;k<count;k++) if(paths[k].closed && paths[k].count>=3) for(size_t j=0;j<paths[k].count;j++) {
        cc_point q=cc_path_point(paths+k,j);
        if(q.y<miny) miny=q.y;
        if(q.y>maxy) maxy=q.y;
        if(q.x<minx) minx=q.x;
        if(q.x>maxx) maxx=q.x;
    }
    if(maxx<0 || minx>=p->cols*2 || maxy<0 || miny>=p->rows*4) return;
    int edgefirst=miny<0?0:miny/4,edgelast=maxy>=p->rows*4?p->rows:maxy/4+1;
    uint64_t stamp=cc_profiling?cc_ticks():0;
    if(p->edges_mode!=CC_EDGES_CRISP) boundary_coverage(p,paths,count,ink,priority,edgefirst,edgelast,minx,maxx);
    if(cc_profiling) cc_profiling->ticks[CC_COVERAGE]+=cc_ticks()-stamp;
    cc_arch_clear(p->edges+(size_t)edgefirst*p->words, p->words * (size_t)(edgelast-edgefirst) * sizeof(cc_bitword));
    for (size_t k = 0; k < count; ++k) {
        const cc_path *path = &paths[k];
        if (!path->closed || path->count < 3) continue;
        for (size_t j = 0; j < path->count; ++j) {
            cc_point a = cc_path_point(path,j), b = cc_path_point(path,j+1==path->count?0:j+1);
            int64_t dy, start, end;
            if (a.y == b.y) continue;
            if (a.y > b.y) { cc_point t = a; a = b; b = t; }
            int bounded=a.x>=-16384 && a.x<=16384 && b.x>=-16384 && b.x<=16384 &&
                        a.y>=-16384 && a.y<=16384 && b.y>=-16384 && b.y<=16384;
            dy = (int64_t)b.y-a.y;
            if(bounded) { start=(a.y+1)>>2; end=(b.y+1)>>2; }
            else { start = ceildiv((int64_t)a.y-2,4); end = ceildiv((int64_t)b.y-2,4); }
            if (start < 0) start = 0;
            if (end > p->rows) end = p->rows;
            if (start >= end) continue;
            if (start < first_row) first_row = (int32_t)start;
            if (end > last_row) last_row = (int32_t)end;
            /* Tile-local edges and ordinary view coordinates fit exact int32
               interpolation. Quotient/remainder stepping removes per-row divide
               and 64-bit multiply; extreme-coordinate API calls keep reference. */
            if(bounded) {
                int den=(int)dy*2,dx=b.x-a.x,sample=(int)start*4+2;
                int num=a.x*(b.y-sample)+b.x*(sample-a.y);
                int q=num/den,rem=num%den,step=4*dx/den,srem=4*dx%den;
                if(rem<0) { rem+=den; q--; }
                if(srem<0) { srem+=den; step--; }
                for(int row=(int)start;row<(int)end;row++) {
                    int x=q+(rem>(int)dy);
                    if(x<0) x=0;
                    if(x>p->cols) x=p->cols;
                    p->edges[(size_t)row*p->words+(unsigned)x/CC_WORD_BITS]^=(cc_bitword)1<<(x%CC_WORD_BITS);
                    q+=step; rem+=srem; if(rem>=den) { rem-=den; q++; }
                }
                continue;
            }
            for (int64_t row = start; row < end; ++row) {
                /* Positive interpolation weights also handle int32 extremes. */
                int64_t sample = 4*row+2;
                int64_t num = (int64_t)a.x*(b.y-sample) + (int64_t)b.x*(sample-a.y);
                int64_t x;
                if(a.x==b.x) x=a.x/2-(a.x<0 && a.x%2!=0);
                else {
                    int64_t rem = num % (2*dy);
                    x = num/(2*dy) + (rem > dy ? 1 : rem <= -dy ? -1 : 0);
                }
                if (x < 0) x = 0;
                if (x > p->cols) x = p->cols;
                p->edges[(size_t)row*p->words+(size_t)x/CC_WORD_BITS] ^= (cc_bitword)1 << (x%CC_WORD_BITS);
            }
        }
    }
    for (int32_t y = first_row; y < last_row; ++y) {
        #if CC_WORD_BITS == 32
        cc_prefix32(p->edges+(size_t)y*p->words,p->words);
#else
        cc_prefix_words(p->edges+(size_t)y*p->words,p->words);
#endif
        for (size_t w = 0; w < p->words; ++w) {
            cc_bitword bits = p->edges[(size_t)y*p->words+w];
            /* Scan contiguous runs once, rather than invoking RV32's software
               ctz helper for each filled cell. The pixel body is word-size math. */
            size_t base=(size_t)y*(size_t)p->cols+w*CC_WORD_BITS;
            unsigned consumed=0;
            while(bits) {
                unsigned skip=trailing(bits); bits>>=skip; consumed+=skip;
                unsigned run=bits==CC_WORD_MAX?CC_WORD_BITS:trailing((cc_bitword)~bits);
                size_t x=w*CC_WORD_BITS+consumed,at=base+consumed;
                if(x>=(size_t)p->cols) break;
                if(run>(size_t)p->cols-x) run=(unsigned)((size_t)p->cols-x);
                uint8_t *fill=p->fill+at,*fp=p->fill_priority+at,*ci=p->coverage_ink+at;
                uint8_t *cp=p->coverage_priority+at,*ec=p->edge_cells+at;
                int crisp=p->edges_mode==CC_EDGES_CRISP;
                if(crisp && p->ordered) {
                    /* Bucket traversal guarantees last-writer priority here. */
                    if(run>=16) { cc_arch_set(fill,ink,run); cc_arch_set(fp,priority,run); }
                    else for(unsigned j=0;j<run;j++) { fill[j]=ink; fp[j]=priority; }
                } else for(unsigned j=0;j<run;j++) {
                    if(priority>=fp[j]) {
                        fill[j]=ink; fp[j]=priority;
                        if(!crisp && !ec[j] && priority>=cp[j]) { ci[j]=0; cp[j]=0; }
                    }
                }
                if(run==CC_WORD_BITS) break;
                bits>>=run; consumed+=run;
            }
        }
    }
}
void cc_fill(cc_planes *p,const cc_path *paths,size_t count,uint8_t ink,uint8_t priority) {
    fill_bounds(p,paths,count,ink,priority,NULL);
}
void cc_raster(cc_planes *p, const cc_scene *scene) {
    cc_planes_clear(p); p->ordered=1;
    cc_terrain_blit(p,scene); if(p->terrain_active) cc_terrain_palette(p);
    for (unsigned priority = 0; priority < 16; ++priority) {
        for (const cc_feature *f = scene->first[priority]; f; f = f->next) {
            if(f->terrain_feature && !p->terrain_strength) continue;
            if(f->bounds_valid && (f->bounds_max.x<0 || f->bounds_min.x>=p->cols*2 ||
               f->bounds_max.y<0 || f->bounds_min.y>=p->rows*4)) continue;
            uint64_t stamp=cc_profiling?cc_ticks():0,coverage=cc_profiling?cc_profiling->ticks[CC_COVERAGE]:0;
            if (f->style.fill && f->type == 3) {
                if(f->raster && cc_native_rasters) native_fill(p,f);
                else fill_bounds(p,f->paths,f->count,f->style.fill,f->style.priority,f);
            }
            if(cc_profiling) { cc_profiling->ticks[CC_FILL]+=cc_ticks()-stamp-(cc_profiling->ticks[CC_COVERAGE]-coverage); stamp=cc_ticks(); }
            if (!f->style.line) continue;
            for (size_t k = 0; k < f->count; ++k) {
                const cc_path *path = &f->paths[k];
                if (f->type == 1) {
                    for (size_t j = 0; j < path->count; ++j)
                        cc_dot(p,cc_path_point(path,j).x,cc_path_point(path,j).y,f->style.line,f->style.priority);
                } else {
                    if(path->count) {
                        cc_point first=cc_path_point(path,0),previous=first;
                        for (size_t j = 1; j < path->count; ++j) {
                            cc_point next=cc_path_point(path,j);
                            cc_line(p,previous,next,f->style.line,f->style.priority); previous=next;
                        }
                        if(path->closed) cc_line(p,previous,first,f->style.line,f->style.priority);
                    }
                }
            }
            if(cc_profiling) cc_profiling->ticks[CC_LINES]+=cc_ticks()-stamp;
        }
    }
    p->ordered=0;
}
