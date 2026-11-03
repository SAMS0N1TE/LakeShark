/* Contiguous 2D pictures. Op 5 scrolls source pixels (signed row/n fields,
   positive means sample old[x+dx,y+dy]); its u16 payload fills exposed ground.
   Op 6 patches a span: u16 x,width,u8 codec, then raw565 (0), or u16 runs
   followed by (u16 length,u16 colour) pairs (1); palette indices (2: bytes,
   3: low nibble first). Op 7 replaces the palette: u16 count, count*u16 RGB565.
   Header row/n for op 6 is y/1. */
static size_t picture_span(cell_sink *k,int y,int lo,int hi,size_t budget,bool allow_index) {
    const uint16_t *p=s_picture.src+(size_t)y*s_picture.sw+lo;
    int width=hi-lo,runs=0;
    for(int x=0;x<width;) {int end=x+1;while(end<width && p[end]==p[x])end++;runs++;x=end;}
    bool indexed=allow_index && s_picture.palette_generation && s_picture.palette_lut && s_picture.palette;
    bool nibble=indexed && s_picture.palette_n<=16;
    size_t packed=nibble?((size_t)width+1)/2:indexed?(size_t)width:(size_t)width*2;
    /* Long flat runs beat indexing; textured spans retain spatial indices for
       deflate. The small threshold avoids trading cheap matches for RLE tags. */
    bool rle=2+(size_t)runs*4<(indexed?packed/4:packed);
    int codec=rle?1:nibble?3:indexed?2:0;
    size_t cost=27+(rle?2+(size_t)runs*4:packed);
    if(cost>budget)return 0;
    picture_header(k,6,y,1);cs_u16(k,lo);cs_u16(k,width);cs_u8(k,codec);
    if(codec==0)cs_put(k,p,(size_t)width*2);
    else if(codec>=2) {
        uint8_t indices[256];
        for(int x=0;x<width;) {
            unsigned n=0;
            while(x<width && n<sizeof(indices)) {
                unsigned a=s_picture.palette_lut[p[x++]];
                if(codec==3) {unsigned b=x<width?s_picture.palette_lut[p[x++]]:0;a|=b<<4;}
                indices[n++]=(uint8_t)a;
            }
            cs_put(k,indices,n);
        }
    }
    else {
        cs_u16(k,runs);
        for(int x=0;x<width;) {
            int end=x+1;while(end<width && p[end]==p[x])end++;
            cs_u16(k,end-x);cs_u16(k,p[x]);x=end;
        }
    }
    return cost;
}
static void visual_pan_render(visual_view *v,cell_sink *k,bool input) {
    size_t budget=v->budget;
    if(v->pan_active && !v->invalidate && v->sw==s_picture.sw && v->sh==s_picture.sh &&
       (memcmp(&v->rect,&s_picture.rect,sizeof(v->rect)) || v->w!=s_picture.w || v->h!=s_picture.h)) {
        if(budget<22)return;
        picture_header(k,3,0,0);budget-=22;
        v->rect=s_picture.rect;v->w=s_picture.w;v->h=s_picture.h;
        v->masks_hash=v->masks_pending=0;v->mask_cursor=0;
    }
    bool reset=v->invalidate || !v->pan_active || memcmp(&v->rect,&s_picture.rect,sizeof(v->rect)) ||
        v->sw!=s_picture.sw || v->sh!=s_picture.sh || v->w!=s_picture.w || v->h!=s_picture.h;
    if(reset) {
        if(budget<22)return;
        picture_header(k,0,0,0);budget-=22;
        v->rect=s_picture.rect;v->sw=s_picture.sw;v->sh=s_picture.sh;v->w=s_picture.w;v->h=s_picture.h;
        v->x0=v->y0=0;v->picture_serial=0;v->pan_active=true;v->invalidate=false;
        v->masks_hash=v->masks_pending=0;v->palette_generation=0;
    }
    if(v->picture_serial!=s_picture.generation) {
        bool reuse=!reset && !v->remaining && v->picture_serial==s_picture.pan_base;
        if(reuse && (s_picture.pan_dx || s_picture.pan_dy)) {
            if(budget<24)return;
            picture_header(k,5,(uint16_t)s_picture.pan_dx,(uint16_t)s_picture.pan_dy);
            cs_u16(k,s_picture.pan_ground);budget-=24;
        }
        v->pan_full=!reuse;v->cursor=0;v->remaining=v->sh;
        ls_map_trace("send pan",(int32_t)s_picture.generation,v->pan_full);
        v->picture_serial=s_picture.generation;
    }
    if(!input && s_picture.palette_generation && v->palette_generation!=s_picture.palette_generation) {
        size_t cost=24+s_picture.palette_n*2;
        if(budget<cost) {v->budget=budget;return;}
        picture_header(k,7,0,0);cs_u16(k,s_picture.palette_n);
        for(unsigned i=0;i<s_picture.palette_n;i++)cs_u16(k,s_picture.palette[i]);
        budget-=cost;v->palette_generation=s_picture.palette_generation;
    }
    const bool sending=v->remaining>0;
    while(v->remaining) {
        int y=v->cursor,lo=v->pan_full?0:s_picture.dirty_lo[y],hi=v->pan_full?v->sw:s_picture.dirty_hi[y];
        if(hi>lo && input)break;
        if(hi>lo) {size_t used=picture_span(k,y,lo,hi,budget,true);if(!used)break;budget-=used;}
        v->cursor++;v->remaining--;
    }
    if(sending && !v->remaining) ls_map_trace("sent pan",(int32_t)v->picture_serial,0);
    /* Reuse the standard O record packing. A picture update never keeps stale
       labels, and incomplete mask delivery resumes on the next opportunity. */
    if(v->masks_pending!=s_picture.masks_hash && budget>=4) {
        cs_u8(k,'O');cs_u8(k,0);cs_u16(k,0);budget-=4;
        v->masks_pending=s_picture.masks_hash;v->mask_cursor=0;
    }
    if(!input && v->masks_hash!=s_picture.masks_hash && s_picture.masks_n>=3) {
        size_t stride=5+((size_t)s_cell_w*s_cell_h+1)/2,count=(s_picture.masks_n-3)/stride;
        size_t n=budget>=4?(budget-4)/stride:0;
        if(n>count-v->mask_cursor)n=count-v->mask_cursor;
        if(n) {
            cs_u8(k,'O');cs_u8(k,1);cs_u16(k,n);
            cs_put(k,s_picture.masks+3+v->mask_cursor*stride,n*stride);
            budget-=4+n*stride;v->mask_cursor+=n;
        }
        if(v->mask_cursor==count)v->masks_hash=s_picture.masks_hash;
    }
    v->budget=budget;
}

/* Four probes wins full-frame CPU + 4-Mbaud wire cost on the host bench;
   one probe keeps small pan deltas/input scrolls responsive. */
static unsigned picture_deflate_probes(const visual_view *v) {
    if(!v->pixels || !s_picture.pan)return 32;
    bool full=v->invalidate || !v->pan_active || v->remaining ||
        v->sw!=s_picture.sw || v->sh!=s_picture.sh || v->w!=s_picture.w || v->h!=s_picture.h ||
        memcmp(&v->rect,&s_picture.rect,sizeof(v->rect)) ||
        (v->picture_serial!=s_picture.generation && v->picture_serial!=s_picture.pan_base);
    return full?4:1;
}
