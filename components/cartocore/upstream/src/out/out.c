#include "cartocore/out.h"
#include "cartocore/arch.h"
#include "cartocore/simd.h"
#include <string.h>
#include <stdio.h>

static uint32_t plane_colour(const cc_planes *p,uint8_t ink) {
    if(!p->palette) return cc_style_colour(ink,p->colors16);
    uint32_t colour=p->palette[ink]&0xffffff;
    return colour|(p->colors16?((uint32_t)(0x80|cc_palette16(ink))<<24):0);
}
cc_edges cc_edge_defaults[4]={CC_EDGES_SMOOTH,CC_EDGES_CRISP,CC_EDGES_CRISP,CC_EDGES_CRISP};
cc_edges cc_edges_resolve(cc_edges edges,cc_mode mode) {
    return edges==CC_EDGES_DEFAULT?cc_edge_defaults[mode]:edges;
}
static unsigned bitcount(unsigned n) {
    unsigned count = 0;
    while (n) { count += n&1; n >>= 1; }
    return count;
}
static const struct { uint8_t ch, mask; } glyphs[] = {
        {' ',0},{'.',0xc0},{':',0xc9},{'-',0x12},{'|',0x47},
        {'/',0x63},{'\\',0x9c},{'+',0x57},{'=',0x36},{'_',0xc0},
        {'!',0x45},{'o',0x7e},{'*',0x3f},{'#',0xff},{',',0x40},
        {'\'',0x01},{'`',0x08},{'~',0x1a}
    };

void cc_encoder_init(cc_encoder *e) {
    /* Fresh hand-drawn 2x4 glyph silhouettes, in Unicode braille bit order.
       Equal-distance ties prefer the first glyph for stable sparse output. */
    for (unsigned pattern = 0; pattern < 256; ++pattern) {
        unsigned best = 9; uint8_t ch = ' ';
        for (size_t i = 0; i < sizeof(glyphs)/sizeof(glyphs[0]); ++i) {
            unsigned distance = bitcount(pattern ^ glyphs[i].mask);
            if (distance < best) { best = distance; ch = glyphs[i].ch; }
        }
        e->ascii[pattern] = ch;
    }
}

unsigned cc_media_bits(cc_mode mode,unsigned mask) {
    if(mode==CC_QUADRANT) return (mask&1?1:0)|(mask&2?8:0)|(mask&4?4:0)|(mask&8?32:0);
    if(mode==CC_HALF) return (mask&1?1:0)|(mask&2?64:0);
    return (mask&0xe1)|((mask&2)<<2)|((mask&4)>>1)|((mask&8)<<1)|((mask&16)>>2);
}
uint32_t cc_encoder_glyph(const cc_encoder *e,cc_mode mode,unsigned bits) {
    static const uint32_t quadrant[16]={0x20,0x2598,0x259d,0x2580,0x2596,0x258c,0x259e,0x259b,0x2597,0x259a,0x2590,0x259c,0x2584,0x2599,0x259f,0x2588};
    switch(mode) {
    case CC_BRAILLE: return bits?0x2800+bits:0x20;
    case CC_ASCII: return e->ascii[bits&255];
    case CC_QUADRANT: return quadrant[(bits&3?1:0)|(bits&0x18?2:0)|(bits&0x44?4:0)|(bits&0xa0?8:0)];
    case CC_HALF: return !bits?0x20:bits&0x1b?(bits&0xe4?0x2588:0x2580):0x2584;
    }
    return 0x20;
}
unsigned cc_encoder_mask(cc_mode mode,uint32_t cp) {
    if(mode==CC_QUADRANT) {
        static const uint32_t q[16]={0x20,0x2598,0x259d,0x2580,0x2596,0x258c,0x259e,0x259b,0x2597,0x259a,0x2590,0x259c,0x2584,0x2599,0x259f,0x2588};
        for(unsigned i=0;i<16;i++) if(q[i]==cp) return i;
        return 0;
    }
    if(mode==CC_HALF) return cp==0x2580?1:cp==0x2584?2:cp==0x2588?3:0;
    unsigned b=0;
    if(mode==CC_BRAILLE && cp>=0x2800 && cp<=0x28ff) b=cp-0x2800;
    if(mode==CC_ASCII) for(size_t i=0;i<sizeof(glyphs)/sizeof(glyphs[0]);i++) if(glyphs[i].ch==cp) { b=glyphs[i].mask; break; }
    static const unsigned bit[8]={1,8,2,16,4,32,64,128}; unsigned mask=0;
    for(unsigned i=0;i<8;i++) if(b&bit[i]) mask|=1u<<i;
    return mask;
}
/* Lines are one raster sample wide. OR-reducing two braille rows into a
 * quadrant fattens diagonal lines; select the more occupied row instead (tie:
 * lower row). Horizontal segments remain connected. Polygon coverage retains
 * its area/edge reduction and braille retains its native one-dot strokes. */
static unsigned thin_quadrant_lines(unsigned bits) {
    unsigned a=(bits&1)|((bits&8)>>2),b=((bits&2)>>1)|((bits&16)>>3);
    unsigned c=((bits&4)>>2)|((bits&32)>>4),d=((bits&64)>>6)|((bits&128)>>6);
    unsigned top=bitcount(a)>bitcount(b)?a:b,bot=bitcount(c)>bitcount(d)?c:d;
    return (top&1)|((top&2)<<2)|((bot&1)<<2)|((bot&2)<<4);
}
/* Sparse ordered relief only in empty black ground. Roads, water, polygon
   coverage and labels retain their original cells (labels paint afterwards).
   World phase also agrees with tile-cache and staged-row encoders. */
static void relief(const cc_planes *p,cc_mode mode,size_t i,unsigned bits,cc_cell *c) {
    if(!p->colors16 || !p->terrain_active || !p->terrain_strength || bits ||
       p->shade[i]>15 || p->fill[i]!=CC_LAND || p->dots[i] || p->coverage_ink[i]) return;
    static const uint8_t order[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    unsigned x=(unsigned)(i%p->cols+p->terrain_phase_x)&3;
    unsigned y=(unsigned)(i/p->cols+p->terrain_phase_y)&3;
    unsigned density=(p->shade[i]+1)*(unsigned)p->terrain_strength/100;
    if(order[y*4+x]>=density) return;
    static const unsigned dot[4]={1,8,4,128};
    c->codepoint=mode==CC_ASCII?'.':cc_encoder_glyph(NULL,mode,dot[(x+y)&3]);
    c->fg=UINT32_C(0x88505050); /* ANSI BRIGHT BLACK, dim RGB for HTML */
    c->bg=UINT32_C(0x80000000);
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
static inline void encode(const cc_encoder *e, const cc_planes *p, cc_mode mode, cc_cell *cells,const uint8_t *mask,const uint32_t *list,size_t count) {
    size_t n = (size_t)p->cols*(size_t)p->rows;
    uint32_t palette[CC_INK_COUNT];
    for(unsigned k=0;k<CC_INK_COUNT;k++) palette[k]=plane_colour(p,(uint8_t)k);
    if(list) n=count;
    for (size_t j = 0; j < n; ++j) {
        size_t i=list?list[j]:j;
        if(mask && !mask[i]) continue;
        unsigned bits = p->dot_priority[i] >= p->fill_priority[i] ? p->dots[i] : 0;
        uint32_t ink = palette[p->ink[i]], bg = palette[p->fill[i]];
        if(p->edges_mode!=CC_EDGES_CRISP && p->coverage_ink[i] && p->dot_priority[i]<p->coverage_priority[i]) {
            bits=p->coverage[i]; ink=palette[p->coverage_ink[i]]; bg=palette[p->coverage_bg[i]];
            if(bits==255) { bg=ink; bits=0; }
        }
        if(p->terrain_active && p->shade[i]<16) {
            unsigned q=p->shade[i];
            unsigned bi=p->fill[i],fi=p->ink[i];
            if(p->edges_mode!=CC_EDGES_CRISP && p->coverage_ink[i] && p->dot_priority[i]<p->coverage_priority[i]) { fi=p->coverage_ink[i]; bi=p->coverage[i]==255?fi:p->coverage_bg[i]; }
            bg=p->terrain_colours[bi][q]; ink=p->terrain_colours[fi][q];
        }
        if(mode==CC_QUADRANT && !(p->edges_mode!=CC_EDGES_CRISP && p->coverage_ink[i] &&
           p->dot_priority[i]<p->coverage_priority[i])) bits=thin_quadrant_lines(bits);
        cc_cell c = {0x20,ink,bg};
        c.codepoint=cc_encoder_glyph(e,mode,bits);
        if (!bits) c.fg = bg;
        relief(p,mode,i,bits,&c);
        cells[i] = c;
    }
}
/* Direct silhouette lookup avoids four reductions and mode dispatch per cell.
   Crisp planes never have polygon coverage; lines still use braille dot bits. */
static void encode_quadrant_crisp(const cc_planes *p,cc_cell *cells,const uint32_t *palette) {
    static const uint16_t glyph[256]={
        0x20,0x2598,0x2598,0x2598,0x2596,0x258c,0x258c,0x258c,0x259d,0x2580,0x2580,0x2580,0x259e,0x259b,0x259b,0x259b,
        0x259d,0x2580,0x2580,0x2580,0x259e,0x259b,0x259b,0x259b,0x259d,0x2580,0x2580,0x2580,0x259e,0x259b,0x259b,0x259b,
        0x2597,0x259a,0x259a,0x259a,0x2584,0x2599,0x2599,0x2599,0x2590,0x259c,0x259c,0x259c,0x259f,0x2588,0x2588,0x2588,
        0x2590,0x259c,0x259c,0x259c,0x259f,0x2588,0x2588,0x2588,0x2590,0x259c,0x259c,0x259c,0x259f,0x2588,0x2588,0x2588,
        0x2596,0x258c,0x258c,0x258c,0x2596,0x258c,0x258c,0x258c,0x259e,0x259b,0x259b,0x259b,0x259e,0x259b,0x259b,0x259b,
        0x259e,0x259b,0x259b,0x259b,0x259e,0x259b,0x259b,0x259b,0x259e,0x259b,0x259b,0x259b,0x259e,0x259b,0x259b,0x259b,
        0x2584,0x2599,0x2599,0x2599,0x2584,0x2599,0x2599,0x2599,0x259f,0x2588,0x2588,0x2588,0x259f,0x2588,0x2588,0x2588,
        0x259f,0x2588,0x2588,0x2588,0x259f,0x2588,0x2588,0x2588,0x259f,0x2588,0x2588,0x2588,0x259f,0x2588,0x2588,0x2588,
        0x2597,0x259a,0x259a,0x259a,0x2584,0x2599,0x2599,0x2599,0x2590,0x259c,0x259c,0x259c,0x259f,0x2588,0x2588,0x2588,
        0x2590,0x259c,0x259c,0x259c,0x259f,0x2588,0x2588,0x2588,0x2590,0x259c,0x259c,0x259c,0x259f,0x2588,0x2588,0x2588,
        0x2597,0x259a,0x259a,0x259a,0x2584,0x2599,0x2599,0x2599,0x2590,0x259c,0x259c,0x259c,0x259f,0x2588,0x2588,0x2588,
        0x2590,0x259c,0x259c,0x259c,0x259f,0x2588,0x2588,0x2588,0x2590,0x259c,0x259c,0x259c,0x259f,0x2588,0x2588,0x2588,
        0x2584,0x2599,0x2599,0x2599,0x2584,0x2599,0x2599,0x2599,0x259f,0x2588,0x2588,0x2588,0x259f,0x2588,0x2588,0x2588,
        0x259f,0x2588,0x2588,0x2588,0x259f,0x2588,0x2588,0x2588,0x259f,0x2588,0x2588,0x2588,0x259f,0x2588,0x2588,0x2588,
        0x2584,0x2599,0x2599,0x2599,0x2584,0x2599,0x2599,0x2599,0x259f,0x2588,0x2588,0x2588,0x259f,0x2588,0x2588,0x2588,
        0x259f,0x2588,0x2588,0x2588,0x259f,0x2588,0x2588,0x2588,0x259f,0x2588,0x2588,0x2588,0x259f,0x2588,0x2588,0x2588,
    };
    size_t n=(size_t)p->cols*p->rows;
    for(size_t i=0;i<n;i++) {
        unsigned bits=p->dot_priority[i]>=p->fill_priority[i]?p->dots[i]:0;
        uint32_t bg=p->terrain_active && p->shade[i]<16?p->terrain_colours[p->fill[i]][p->shade[i]]:palette[p->fill[i]];
        cells[i]=(cc_cell){glyph[thin_quadrant_lines(bits)],bits?palette[p->ink[i]]:bg,bg};
        relief(p,CC_QUADRANT,i,bits,&cells[i]);
    }
}
void cc_encode_mask(const cc_encoder *e,const cc_planes *p,cc_mode mode,cc_cell *cells,const uint8_t *mask) { encode(e,p,mode,cells,mask,0,0); }
void cc_encode_list(const cc_encoder *e,const cc_planes *p,cc_mode mode,cc_cell *cells,const uint32_t *list,size_t count) { encode(e,p,mode,cells,0,list,count); }
void cc_encode(const cc_encoder *e,const cc_planes *p,cc_mode mode,cc_cell *cells) {
    if(mode==CC_QUADRANT && p->edges_mode==CC_EDGES_CRISP) {
        uint32_t palette[CC_INK_COUNT];
        for(unsigned k=0;k<CC_INK_COUNT;k++) palette[k]=plane_colour(p,(uint8_t)k);
        encode_quadrant_crisp(p,cells,palette);
    }
    else encode(e,p,mode,cells,0,0,0);
}
void cc_encode_staged(const cc_encoder *e,const cc_planes *p,cc_mode mode,cc_cell *cells,cc_cell *scratch) {
    uint32_t palette[CC_INK_COUNT];
    for(unsigned k=0;k<CC_INK_COUNT;k++) palette[k]=plane_colour(p,(uint8_t)k);
    cc_planes row=*p; row.rows=1;
    for(int y=0;y<p->rows;y++) {
        size_t at=(size_t)y*p->cols;
        row.shade=p->shade+at; row.terrain_phase_y=p->terrain_phase_y+y;
        row.dots=p->dots+at; row.ink=p->ink+at; row.dot_priority=p->dot_priority+at;
        row.fill=p->fill+at; row.fill_priority=p->fill_priority+at;
        row.coverage=p->coverage+at; row.coverage_ink=p->coverage_ink+at;
        row.coverage_priority=p->coverage_priority+at; row.coverage_bg=p->coverage_bg+at;
        if(mode==CC_QUADRANT && p->edges_mode==CC_EDGES_CRISP) encode_quadrant_crisp(&row,scratch,palette);
        else cc_encode(e,&row,mode,scratch);
        cc_arch_copy(cells+at,scratch,(size_t)p->cols*sizeof(cc_cell));
    }
}
/* UTF-8 names borrow mapped tile strings. Labels are painted after encoding. */
static uint32_t character(cc_str s,size_t *at) {
    uint32_t c=s.data[(*at)++]; unsigned extra=0;
    if(c<128) return c<32 || c==127?'?':c;
    if(c>=0xc2 && c<0xe0) { c&=31; extra=1; }
    else if(c<0xf0 && c>=0xe0) { c&=15; extra=2; }
    else if(c>=0xf0 && c<0xf5) { c&=7; extra=3; } else return '?';
    while(extra--) { if(*at>=s.size || (s.data[*at]&0xc0)!=0x80) return '?'; c=(c<<6)|(s.data[(*at)++]&63); }
    return c<32 || c>0x10ffff || (c>=0xd800 && c<=0xdfff)?'?':c;
}
static int label_at(cc_planes *p,cc_cell *cells,const cc_feature *f,int length,int x,int y) {
    /* Full-cell black plates and bright semantic ink, independent of terrain. */
    static const uint32_t colours[]={0xf5edce,0xf5edce,0xdacfb3,0x70b5df,0xf4cf8d,0xb4bec5};
    if(!cc_label_reserve(p,x,y,length)) return 0;
    size_t at=0;
    for(int i=0;i<length;i++) { cc_cell *c=cells+(size_t)y*p->cols+x+i; c->codepoint=character(f->name,&at); c->bg=p->colors16?UINT32_C(0x80000000):0; c->fg=colours[f->label_class] | (p->colors16 ? (uint32_t)(0x80 | (uint8_t[]){15,15,15,14,11,15}[f->label_class])<<24 : 0); }
    return 1;
}
void cc_label_metadata(cc_feature *f) {
    if(!f->label_class || !f->name.size) return;
    f->name_hash=UINT64_C(14695981039346656037)^f->label_class;
    for(size_t j=0;j<f->name.size;j++) f->name_hash=(f->name_hash^f->name.data[j])*UINT64_C(1099511628211);
    size_t at=0; f->name_length=0;
    while(at<f->name.size && f->name_length<=16384) { character(f->name,&at); f->name_length++; }
}
void cc_labels(cc_planes *p,const cc_scene *scene,cc_cell *cells) {
    memset(p->collision,0,p->words*(size_t)p->rows*sizeof(cc_bitword));
    memset(p->label_seen,0,p->label_capacity*sizeof(cc_feature*));
    for(unsigned rank=1;rank<=5;rank++) for(unsigned priority=0;priority<16;priority++)
        for(const cc_feature *f=scene->labels_indexed?scene->label_first[rank][priority]:scene->first[priority];f;f=scene->labels_indexed?f->label_next:f->next) {
            if(f->label_class!=rank || !f->name.size) continue;
            if(f->type!=1 && f->type!=3) {
                int minx=INT32_MAX,miny=INT32_MAX,maxx=INT32_MIN,maxy=INT32_MIN;
                if(f->bounds_valid) { minx=f->bounds_min.x; miny=f->bounds_min.y; maxx=f->bounds_max.x; maxy=f->bounds_max.y; }
                else for(size_t k=0;k<f->count;k++) for(size_t j=0;j<f->paths[k].count;j++) {
                    cc_point q=cc_feature_point(f,f->paths+k,j);
                    if(q.x<minx) minx=q.x;
                    if(q.x>maxx) maxx=q.x;
                    if(q.y<miny) miny=q.y;
                    if(q.y>maxy) maxy=q.y;
                }
                if(maxx<2 || minx>=p->cols*2 || maxy<4 || miny>=p->rows*4) continue;
            }
            uint64_t hash=f->name_hash;
            if(!f->name_length) {
                hash=UINT64_C(14695981039346656037)^rank;
                for(size_t j=0;j<f->name.size;j++) hash=(hash^f->name.data[j])*UINT64_C(1099511628211);
            }
            size_t slot=(size_t)hash&(p->label_capacity-1);
            while(p->label_seen[slot]) {
                const cc_feature *q=p->label_seen[slot];
                if(q->label_class==rank && q->name.size==f->name.size && !memcmp(q->name.data,f->name.data,f->name.size)) break;
                slot=(slot+1)&(p->label_capacity-1);
            }
            if(p->label_seen[slot]) continue;
            int length=f->name_length,placed=0; size_t at=0;
            if(!length) while(at<f->name.size && length<=p->cols) { character(f->name,&at); length++; }
            if(length>p->cols-2) continue;
            if(f->type==1 || f->type==3) {
                int x=f->anchor.x/2-(f->anchor.x<0 && f->anchor.x%2!=0)-length/2;
                int y=f->anchor.y/4-(f->anchor.y<0 && f->anchor.y%4!=0);
                placed=label_at(p,cells,f,length,x,y);
            } else if(rank==3) {
                for(size_t k=0;k<f->count && !placed;k++) {
                    const cc_path *r=f->paths+k;
                    for(size_t j=0;j<r->count && !placed;j++) {
                        cc_point q=cc_feature_point(f,r,(j+r->count/2)%r->count);
                        placed=label_at(p,cells,f,length,q.x/2-length/2,q.y/4);
                    }
                }
            } else for(size_t k=0;k<f->count && !placed;k++) {
                const cc_path *r=f->paths+k;
                for(size_t j=0;j+1<r->count && !placed;j++) {
                    cc_point a=cc_feature_point(f,r,j); int lo=a.y,hi=a.y,dir=0;
                    for(size_t t=j+1;t<r->count;t++) {
                        cc_point b=cc_feature_point(f,r,t),prev=cc_feature_point(f,r,t-1);
                        int step=b.x>prev.x?1:b.x<prev.x?-1:0;
                        if(dir && step && step!=dir) break;
                        if(step) dir=step;
                        if(b.y<lo) lo=b.y;
                        if(b.y>hi) hi=b.y;
                        if(hi-lo>4) break;
                        int64_t span=(int64_t)b.x-a.x;
                        if(span<0) span=-span;
                        if(span>=(int64_t)length*2) {
                            int x=(int)(((int64_t)a.x+b.x)/4)-length/2,y=(lo+hi)/8;
                            if(label_at(p,cells,f,length,x,y)) placed=1;
                            break;
                        }
                    }
                }
            }
            if(placed) p->label_seen[slot]=f;
        }
}
void cc_buffer_init(cc_buffer *b, char *data, size_t capacity) {
    b->data = data; b->capacity = capacity; b->size = 0; b->error = 0;
}
static void append(cc_buffer *b, const char *s, size_t n) {
    if (b->error) return;
    if (n > b->capacity-b->size) { b->error = 1; return; }
    if (b->data) memcpy(b->data+b->size,s,n);
    b->size += n;
}
static void literal(cc_buffer *b, const char *s) { append(b,s,strlen(s)); }
static void utf8(cc_buffer *b, uint32_t c) {
    char s[4]; size_t n;
    if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) c = '?';
    if (c < 128) { s[0] = (char)c; n = 1; }
    else if (c < 2048) { s[0] = (char)(0xc0|(c>>6)); s[1] = (char)(0x80|(c&63)); n = 2; }
    else if (c < 65536) {
        s[0] = (char)(0xe0|(c>>12)); s[1] = (char)(0x80|((c>>6)&63));
        s[2] = (char)(0x80|(c&63)); n = 3;
    } else {
        s[0] = (char)(0xf0|(c>>18)); s[1] = (char)(0x80|((c>>12)&63));
        s[2] = (char)(0x80|((c>>6)&63)); s[3] = (char)(0x80|(c&63)); n = 4;
    }
    append(b,s,n);
}
static unsigned quantize(uint32_t rgb) {
    unsigned r = (rgb>>16)&255, g = (rgb>>8)&255, b = rgb&255;
    unsigned channels[3] = {r,g,b}, q[3], levels[6] = {0,95,135,175,215,255};
    int cube_error = 0, gray_error = 0;
    unsigned average = (r+g+b)/3, gray = average < 8 ? 0 : (average-8+5)/10;
    if (gray > 23) gray = 23;
    for (unsigned i = 0; i < 3; ++i) {
        int best = 100000; q[i] = 0;
        for (unsigned j = 0; j < 6; ++j) {
            int d = (int)channels[i]-(int)levels[j];
            if (d*d < best) { best = d*d; q[i] = j; }
        }
        cube_error += best;
        { int d = (int)channels[i]-(int)(8+gray*10); gray_error += d*d; }
    }
    return gray_error < cube_error ? 232+gray : 16+36*q[0]+6*q[1]+q[2];
}
typedef struct { uint32_t fg, bg; int valid; } sgr_state;
static size_t decimal(char *s,uint32_t v) {
    char reverse[10]; size_t n=0;
    do { reverse[n++]=(char)('0'+v%10); v/=10; } while(v);
    for(size_t i=0;i<n;i++) s[i]=reverse[n-1-i];
    return n;
}
static size_t colour(char *s,uint32_t v,int background,int truecolor) {
    if(truecolor==CC_ANSI_16) return decimal(s,(background?40:30)+(v&7)+(v&8?60:0));
    size_t n=0; s[n++]=background?'4':'3'; s[n++]='8'; s[n++]=';';
    s[n++]=truecolor?'2':'5'; s[n++]=';';
    if(truecolor) { n+=decimal(s+n,(v>>16)&255); s[n++]=';'; n+=decimal(s+n,(v>>8)&255); s[n++]=';'; v&=255; }
    return n+decimal(s+n,v);
}
static void ansi_cell(cc_buffer *b,cc_cell c,sgr_state *s,int truecolor) {
    char text[80]; size_t n=0;
    uint32_t fg=truecolor==CC_ANSI_16?cc_index16(c.fg):truecolor?cc_rgb(c.fg):quantize(c.fg),
             bg=truecolor==CC_ANSI_16?cc_index16(c.bg):truecolor?cc_rgb(c.bg):quantize(c.bg);
    int change_fg=!s->valid || fg!=s->fg,change_bg=!s->valid || bg!=s->bg;
    if(change_fg || change_bg) {
        text[n++]=27; text[n++]='[';
        if(change_fg) n+=colour(text+n,fg,0,truecolor);
        if(change_bg) { if(change_fg) text[n++]=';'; n+=colour(text+n,bg,1,truecolor); }
        text[n++]='m'; append(b,text,n); s->fg=fg; s->bg=bg; s->valid=1;
    }
    utf8(b,c.codepoint);
}
static void move_to(cc_buffer *b,int32_t x,int32_t y) {
    char s[40]; size_t n=0; s[n++]=27; s[n++]='[';
    n+=decimal(s+n,(uint32_t)y+1); s[n++]=';'; n+=decimal(s+n,(uint32_t)x+1); s[n++]='H'; append(b,s,n);
}
static int equal(cc_cell a, cc_cell b) {
    return a.codepoint == b.codepoint && a.fg == b.fg && a.bg == b.bg;
}
int cc_ansi(cc_buffer *b, const cc_cell *cells, const cc_cell *previous,
            int32_t cols, int32_t rows, int truecolor) {
    sgr_state state = {0,0,0}; int started = 0;
    if (cols <= 0 || rows <= 0) return 0;
    for (int32_t y = 0; y < rows; ++y) {
        size_t row = (size_t)y*(size_t)cols; int32_t x = 0;
        if(previous) x+=(int)cc_equal_run(cells+row+x,previous+row+x,(size_t)(cols-x));
        if (x == cols) continue;
        if (!started) { literal(b,"\033[?2026h"); started = 1; }
        move_to(b,x,y);
        while (x < cols) {
            ansi_cell(b,cells[row+x],&state,truecolor); ++x;
            if (previous && x < cols && equal(cells[row+x],previous[row+x])) {
                int32_t next = x;
                cc_buffer gap, move; sgr_state estimate = state;
                next+=(int)cc_equal_run(cells+row+next,previous+row+next,(size_t)(cols-next));
                if (next == cols) break;
                cc_buffer_init(&gap,0,SIZE_MAX); cc_buffer_init(&move,0,SIZE_MAX);
                for (int32_t j = x; j < next; ++j) ansi_cell(&gap,cells[row+j],&estimate,truecolor);
                /* Include following cell's SGR, since bridging can change state. */
                ansi_cell(&gap,cells[row+next],&estimate,truecolor);
                move_to(&move,next,y); estimate = state;
                ansi_cell(&move,cells[row+next],&estimate,truecolor);
                if (move.size < gap.size) { move_to(b,next,y); x = next; }
            }
        }
    }
    if (started) literal(b,"\033[?2026l");
    return !b->error;
}
int cc_ansi_diff(cc_buffer *b,const cc_cell *cells,const cc_cell *previous,
    int32_t cols,int32_t rows,int truecolor,cc_ansi_state *saved) {
    sgr_state state={saved->fg,saved->bg,saved->valid}; int started=0;
    if(cols<=0 || rows<=0) return 0;
    if(saved->valid && saved->truecolor!=truecolor) { state.valid=0; previous=0; }
    for(int y=0;y<rows;y++) {
        size_t row=(size_t)y*cols; int x=0;
        while(x<cols) {
            if(previous) x+=(int)cc_equal_run(cells+row+x,previous+row+x,(size_t)(cols-x));
            if(x==cols) break;
            if(!started) { literal(b,"\033[?2026h"); started=1; }
            move_to(b,x,y);
            do { ansi_cell(b,cells[row+x],&state,truecolor); x++; }
            while(x<cols && (!previous || !equal(cells[row+x],previous[row+x])));
        }
    }
    if(started) literal(b,"\033[?2026l");
    if(b->error) { saved->valid=0; return 0; }
    saved->fg=state.fg; saved->bg=state.bg; saved->valid=state.valid; saved->truecolor=truecolor;
    return 1;
}
int cc_html(cc_buffer *b, const cc_cell *cells, int32_t cols, int32_t rows) {
    uint32_t fg = 0, bg = 0; int open = 0;
    if (cols <= 0 || rows <= 0) return 0;
    literal(b,"<!doctype html><html lang=\"en\"><meta charset=\"utf-8\"><title>CartoCore map</title>"
              "<style>body{background:#000;color:#fff;margin:16px}pre{"
              "font-family:Cascadia Mono,Consolas,DejaVu Sans Mono,Segoe UI Symbol,monospace;"
              "line-height:1.0;font-size:9px;white-space:pre;margin:0}</style><pre>");
    for (int32_t y = 0; y < rows; ++y) {
        for (int32_t x = 0; x < cols; ++x) {
            cc_cell c = cells[(size_t)y*(size_t)cols+(size_t)x];
            if (!open || c.fg != fg || c.bg != bg) {
                char s[96]; int n;
                if (open) literal(b,"</span>");
                n = snprintf(s,sizeof(s),"<span style=\"color:#%06x;background:#%06x\">",
                             (unsigned)cc_rgb(c.fg),(unsigned)cc_rgb(c.bg));
                append(b,s,(size_t)n); fg = c.fg; bg = c.bg; open = 1;
            }
            if (c.codepoint == '&') literal(b,"&amp;");
            else if (c.codepoint == '<') literal(b,"&lt;");
            else if (c.codepoint == '>') literal(b,"&gt;");
            else utf8(b,c.codepoint);
        }
        literal(b,"\n");
    }
    if (open) literal(b,"</span>");
    literal(b,"</pre></html>\n"); return !b->error;
}
/* Browser snapshot shapes avoid font bearings for terminal block glyphs. */
int cc_html_raster(cc_buffer *b,const cc_cell *cells,int32_t cols,int32_t rows) {
    if(cols<=0 || rows<=0) return 0;
    literal(b,"<!doctype html><html lang=\"en\"><meta charset=\"utf-8\"><title>CartoCore shapes</title><canvas id=\"map\"></canvas><script>const C=[");
    for(size_t i=0;i<(size_t)cols*rows;i++) {
        char s[64]; int n=snprintf(s,sizeof(s),"%s[%u,%u,%u]",i?",":"",(unsigned)cells[i].codepoint,(unsigned)cc_rgb(cells[i].fg),(unsigned)cc_rgb(cells[i].bg)); append(b,s,(size_t)n);
    }
    char dims[128]; int n=snprintf(dims,sizeof(dims),"];const cols=%d,rows=%d,W=6,H=12;",(int)cols,(int)rows); append(b,dims,(size_t)n);
    literal(b,"const c=document.getElementById('map');c.width=cols*W;c.height=rows*H;const g=c.getContext('2d');"
    "const q={9624:1,9629:2,9600:3,9622:4,9612:5,9630:6,9627:7,9623:8,9626:9,9616:10,9628:11,9604:12,9625:13,9631:14,9608:15};"
    "function color(n){return '#'+n.toString(16).padStart(6,'0')}g.font='10px Consolas,monospace';g.textBaseline='top';"
    "C.forEach(([v,f,b],i)=>{const x=i%cols*W,y=Math.floor(i/cols)*H;g.fillStyle=color(b);g.fillRect(x,y,W,H);g.fillStyle=color(f);"
    "if(q[v]){let m=q[v];for(let j=0;j<4;j++)if(m&(1<<j)){let a=j%2?W/2:0;g.fillRect(x+a,y+(j>1?H/2:0),W/2,H/2)}}"
    "else if(v>=10240&&v<=10495){const bits=[1,8,2,16,4,32,64,128];for(let j=0;j<8;j++)if((v-10240)&bits[j])g.fillRect(x+(j%2?3:0),y+Math.floor(j/2)*3+1,2,2)}"
    "else if(v!==32)g.fillText(String.fromCodePoint(v),x,y)});</script></html>\n");
    return !b->error;
}
