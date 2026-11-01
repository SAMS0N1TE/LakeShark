/* Included beside the production cells encoder; uses its streaming z sink. */
static uint32_t picture_row_hash(int y) {
    return visual_hash(s_picture.src+(size_t)y*s_picture.sw,(size_t)s_picture.sw*2);
}
static void picture_header(cell_sink *k,int op,int row,int rows) {
    cs_u8(k,'I');cs_u8(k,op);
    cs_u16(k,s_picture.rect.x*s_cell_w);cs_u16(k,s_picture.rect.y*s_cell_h);
    cs_u16(k,s_picture.w);cs_u16(k,s_picture.h);cs_u16(k,row);cs_u16(k,rows);
    cs_u16(k,s_picture.sw);cs_u16(k,s_picture.sh);cs_u16(k,s_picture.x0);cs_u16(k,s_picture.y0);
}
#include "ls_rterm_picture_patch.h"
static bool visual_options(visual_view *v,bool cells,bool z,const char *arg) {
    int pixels=0,vectors=0;
    if(!arg || sscanf(arg,"%d %d",&pixels,&vectors)!=2) return false;
    v->pixels=cells && z && pixels==1;v->vectors=cells && z && vectors==1;
    return true;
}
static bool visual_dot_visible(size_t i) {
    const visual_dot *d=&s_frame_dots[i];const tui_cell *c=&s_snap[i];
    uint16_t glyph=(uint16_t)c->ch;if(glyph>=0xff00) glyph&=255;
    return (d->bits&(uint16_t)c->ch) && d->attr==c->attr &&
        (glyph&(d->glyph==0xC0?0xffc0:0xff00))==d->glyph;
}
static size_t visual_vectors(session_t *s,cell_sink *k,size_t budget) {
    visual_view *v=&s->visual;
    if(!v->vectors || (!v->vectors_full && v->vectors_hash==s_vectors_hash)) return 0;
    unsigned count=0;
    if(s_frame_line_count) for(size_t i=0;i<(size_t)s->cols*s->rows && count<VECTOR_CELL_CAP;i++)
        if(visual_dot_visible(i)) count++;
    size_t cost=5+s_frame_line_count*10+count*6;
    if(cost>budget) return 0;
    cs_u8(k,'V');cs_u16(k,s_frame_line_count);
    for(unsigned i=0;i<s_frame_line_count;i++) {
        const visual_line *l=&s_frame_lines[i];
        cs_u16(k,l->x0);cs_u16(k,l->y0);cs_u16(k,l->x1);cs_u16(k,l->y1);
        cs_u16(k,s_pal[l->attr&15]);
    }
    cs_u16(k,count);
    for(size_t i=0;count && i<(size_t)s->cols*s->rows;i++) {
        if(!visual_dot_visible(i)) continue;
        const visual_dot *d=&s_frame_dots[i];const tui_cell *c=&s_snap[i];
        uint16_t glyph=(uint16_t)c->ch;if(glyph>=0xff00) glyph&=255;
        cs_u16(k,i);cs_u16(k,glyph);cs_u8(k,c->attr);cs_u8(k,d->bits&glyph);count--;
    }
    v->vectors_hash=s_vectors_hash;v->vectors_full=false;
    return cost;
}
static void visual_render(session_t *s,cell_sink *k,bool full,bool input) {
    visual_view *v=&s->visual;
    int64_t now=esp_timer_get_time();
    if(full) {v->invalidate=true;v->vectors_full=true;}
    /* No extra pixels behind an unpainted frame, or a frame answering input.
       No queued pixel deltas: next opportunity reads the newest snapshot. */
    if(now-v->last_us>=(s_picture.pan?16000:PICTURE_US)) {v->last_us=now;v->budget=ls_rterm_picture_budget(s==&s_web);}
    /* Placement is cheap control data. Reuse the viewer's source rows when a
       window moves/scales; do not blank and retransmit the whole picture. */
    if(v->pixels && !v->invalidate && v->hash && v->valid && !s_picture.unavailable &&
       v->sw==s_picture.sw && v->sh==s_picture.sh && v->picture_serial==s_picture.generation &&
       v->x0==s_picture.x0 && v->y0==s_picture.y0 && v->budget>=22 &&
       (memcmp(&v->rect,&s_picture.rect,sizeof(v->rect)) || v->w!=s_picture.w || v->h!=s_picture.h)) {
        picture_header(k,3,0,0);v->budget-=22;
        v->rect=s_picture.rect;v->w=s_picture.w;v->h=s_picture.h;
        v->masks_hash=v->masks_pending=0;v->mask_cursor=0;v->vectors_full=true;
    }
    if(s->unacked)return;
    if(v->pixels && s_picture.pan) {
        if(v->vectors && !input && now-v->last_vectors_us>=PICTURE_US) {
            size_t used=visual_vectors(s,k,v->budget>24?v->budget-24:0);
            if(used) {v->budget-=used;v->last_vectors_us=now;}
        }
        if(s_picture.ready) visual_pan_render(v,k,input);
        return;
    }
    if(v->pan_active) {v->invalidate=true;v->pan_active=false;}
    if(input)return;
    size_t budget=v->budget;
    if(v->vectors && now-v->last_vectors_us>=PICTURE_US) {
        size_t available=budget;
        /* A wide picture still advances when every line frame changes. */
        if(v->pixels && s_picture.ready && now-v->last_picture_us>=2*PICTURE_US) {
            size_t reserve=(size_t)s_picture.sw*2+48;
            available=budget>reserve?budget-reserve:0;
        }
        size_t used=visual_vectors(s,k,available);
        if(used) {budget-=used;v->last_vectors_us=now;}
    }
    if(v->pixels && s_picture.ready && budget>=26 && now-v->last_picture_us>=PICTURE_US) {
        v->last_picture_us=now;
        bool reset=v->invalidate || memcmp(&v->rect,&s_picture.rect,sizeof(v->rect)) ||
            v->w!=s_picture.w || v->h!=s_picture.h || v->sw!=s_picture.sw || v->sh!=s_picture.sh ||
            (s_picture.sh && (!v->hash || !v->valid || !v->columns_valid));
        if(reset) {
            heap_caps_free(v->hash);heap_caps_free(v->valid);heap_caps_free(v->columns_valid);
            v->hash=NULL;v->valid=NULL;v->columns_valid=NULL;
            if(s_picture.sh) {
                v->hash=heap_caps_calloc(s_picture.sh,sizeof(*v->hash),MALLOC_CAP_SPIRAM);
                v->valid=heap_caps_calloc(s_picture.sh,1,MALLOC_CAP_SPIRAM);
                v->columns_valid=heap_caps_calloc(s_picture.sw,1,MALLOC_CAP_SPIRAM);
                if(v->columns_valid) memset(v->columns_valid,1,s_picture.sw);
            }
            v->rect=s_picture.rect;v->w=s_picture.w;v->h=s_picture.h;
            v->sw=s_picture.sw;v->sh=s_picture.sh;v->cursor=s_picture.y0;v->masks_hash=0;v->remaining=v->sh;
            v->by_columns=(size_t)v->sw*2+22>PICTURE_BYTES;
            v->pending_columns=0;v->column_cursor=0;v->column_step=1;v->full_epoch=s_picture.full_epoch;
            if(v->by_columns && v->columns_valid) {
                memset(v->columns_valid,0,v->sw);v->pending_columns=v->sw;v->remaining=0;
            }
            picture_header(k,0,0,0); /* reset, including an empty rectangle */
            budget-=22;v->invalidate=false;
        }
        bool trusted=s_picture.ring && v->full_epoch==s_picture.full_epoch;
        bool columns_only=trusted && s_picture.partial_axes==1 && (size_t)v->sh*2+22<=PICTURE_BYTES;
        if(v->picture_serial!=s_picture.generation || v->x0!=s_picture.x0 || v->y0!=s_picture.y0) {
            if(v->by_columns) {
                if(!columns_only && v->columns_valid) {memset(v->columns_valid,0,v->sw);v->pending_columns=v->sw;}
            } else if(!columns_only) v->remaining=v->sh;
        }
        v->picture_serial=s_picture.generation;
        v->full_epoch=s_picture.full_epoch;
        /* Rings remain in source storage order. Only the sampling origin
           moves, including non-integer scaled pictures. Prioritize new rows. */
        if(!reset && v->sh && (v->x0!=s_picture.x0 || v->y0!=s_picture.y0 ||
            (trusted && (v->ring_rows!=s_picture.ring_rows || v->ring_columns!=s_picture.ring_columns)))) {
            if(v->x0!=s_picture.x0 || v->y0!=s_picture.y0) {picture_header(k,2,0,0);budget-=22;}
            if(v->valid) {
                int shift=(v->y0-s_picture.y0+v->sh)%v->sh;
                if(trusted) {
                    uint32_t delta=s_picture.ring_rows-v->ring_rows;
                    shift=delta>(uint32_t)v->sh?v->sh:(int)delta;
                } else if(shift>v->sh/2) shift-=v->sh;
                for(int i=0;i<abs(shift);i++) {
                    int y=(shift>0?s_picture.y0+i:v->y0+i)%v->sh;v->valid[y]=0;
                }
                v->cursor=shift>=0?s_picture.y0:v->y0;
            }
            if(v->columns_valid && (v->x0!=s_picture.x0 || (trusted && v->ring_columns!=s_picture.ring_columns)) && (size_t)v->sh*2+22<=PICTURE_BYTES) {
                int shift=(s_picture.x0-v->x0+v->sw)%v->sw;
                if(trusted) {
                    uint32_t delta=s_picture.ring_columns-v->ring_columns;
                    shift=delta>(uint32_t)v->sw?v->sw:(int)delta;
                } else if(shift>v->sw/2) shift-=v->sw;
                int start=shift>0?v->x0:s_picture.x0;
                for(int i=0;i<abs(shift);i++) {
                    int x=(start+i)%v->sw;
                    if(v->columns_valid[x]) {v->columns_valid[x]=0;v->pending_columns++;}
                }
                v->column_cursor=shift>0?(s_picture.x0+v->sw-1)%v->sw:s_picture.x0;
                v->column_step=shift>0?-1:1;
            }
        }
        v->x0=s_picture.x0;v->y0=s_picture.y0;
        v->ring_columns=s_picture.ring_columns;v->ring_rows=s_picture.ring_rows;
        /* A hard raw-byte ceiling bounds both wire cost and compressor work,
           even for noise. Deflate and framing add <1% to this allocation. */
        if(reset || v->masks_pending!=s_picture.masks_hash) {
            v->masks_pending=s_picture.masks_hash;v->mask_cursor=0;
            cs_u8(k,'O');cs_u8(k,0);cs_u16(k,0);budget-=4;
        }
        if(v->masks_hash!=s_picture.masks_hash && s_picture.masks_n>=3) {
            size_t stride=5+((size_t)s_cell_w*s_cell_h+1)/2;
            size_t count=(s_picture.masks_n-3)/stride;
            size_t n=budget>=4?(budget-4)/stride:0;
            if(n>count-v->mask_cursor) n=count-v->mask_cursor;
            if(n) {
                cs_u8(k,'O');cs_u8(k,1);cs_u16(k,n);
                cs_put(k,s_picture.masks+3+v->mask_cursor*stride,n*stride);
                v->mask_cursor+=n;budget-=4+n*stride;
            }
            if(v->mask_cursor==count) v->masks_hash=s_picture.masks_hash;
        }
        int checked=0;
        while(v->columns_valid && v->pending_columns && checked<v->sw && (size_t)v->sh*2+22<=budget) {
            int x=v->column_cursor;v->column_cursor=(x+v->column_step+v->sw)%v->sw;checked++;
            if(v->columns_valid[x]) continue;
            picture_header(k,4,x,1);
            for(int y=0;y<v->sh;y++) cs_u16(k,s_picture.src[(size_t)y*v->sw+x]);
            budget-=(size_t)v->sh*2+22;v->columns_valid[x]=1;v->pending_columns--;
        }
        checked=0;
        while(v->hash && v->valid && v->remaining>0 && checked<v->sh && (size_t)v->sw*2+22<=budget) {
            int y=v->cursor;v->cursor=(y+1)%v->sh;checked++;
            v->remaining--;
            if(trusted && s_picture.partial_axes==2 && v->valid[y]) continue;
            uint32_t h=picture_row_hash(y);
            if(v->valid[y] && v->hash[y]==h) continue;
            picture_header(k,1,y,1);
            cs_put(k,s_picture.src+(size_t)y*v->sw,(size_t)v->sw*2);
            budget-=(size_t)v->sw*2+22;v->hash[y]=h;v->valid[y]=1;
        }
    }
    v->budget=budget;
}
