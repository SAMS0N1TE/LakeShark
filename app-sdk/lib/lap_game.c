#include "lap_game.h"
#include <limits.h>
void lap_sprite_draw(lap_canvas_t *c,const lap_sprite_t *s,int frame,int x,int y,int scale,unsigned flags,const uint8_t *pal) {
    if(!c||!c->d||!s||!s->image.pixels||s->frame_w<=0||s->frame_h<=0||scale<1||scale>16||frame<0) return;
    int cols=s->image.w/s->frame_w,rows=s->image.h/s->frame_h;
    if(!cols||!rows||frame/cols>=rows||s->frame_w>INT_MAX/scale||s->frame_h>INT_MAX/scale) return;
    int64_t right=(int64_t)x+s->frame_w*scale,bottom=(int64_t)y+s->frame_h*scale;
    int x0=x<0?0:x,y0=y<0?0:y,x1=right>c->w?c->w:(int)right,y1=bottom>c->h?c->h:(int)bottom;
    int fx=frame%cols*s->frame_w,fy=frame/cols*s->frame_h;
    for(int dy=y0;dy<y1;dy++) for(int dx=x0;dx<x1;dx++) {
        int sx=(int)(((int64_t)dx-x)/scale),sy=(int)(((int64_t)dy-y)/scale);
        if(flags&LAP_SPRITE_FLIP_X) sx=s->frame_w-1-sx;
        if(flags&LAP_SPRITE_FLIP_Y) sy=s->frame_h-1-sy;
        uint8_t ink=s->image.pixels[(size_t)(fy+sy)*s->image.w+fx+sx];
        if(ink<16) c->d[(size_t)dy*c->w+dx]=pal?pal[ink]:ink;
    }
}
int lap_animation_step(lap_animation_t *a,uint32_t dt) {
    if(!a||a->count<=0||!a->period_ms) return a?a->first:0;
    if(a->done) return a->first+a->frame;
    uint64_t time=(uint64_t)a->elapsed+dt,frame=(unsigned)a->frame+time/a->period_ms;
    a->elapsed=(uint32_t)(time%a->period_ms);
    if(a->loop) a->frame=(int)(frame%(unsigned)a->count);
    else if(frame>=(unsigned)a->count) { a->frame=a->count-1; a->done=true; }
    else a->frame=(int)frame;
    return a->first+a->frame;
}
static int map_valid(const lap_tilemap_t *m) {
    return m&&m->sheet&&m->tiles&&m->width>0&&m->height>0&&m->sheet->frame_w>0&&m->sheet->frame_h>0;
}
uint8_t lap_tile_flags(const lap_tilemap_t *m,int x,int y) {
    return m&&m->flags&&x>=0&&y>=0&&x<m->width&&y<m->height?m->flags[(size_t)y*m->width+x]:0;
}
void lap_tilemap_draw(lap_canvas_t *c,const lap_tilemap_t *m,int cx,int cy) {
    if(!c||!c->d||!map_valid(m)) return;
    int tw=m->sheet->frame_w,th=m->sheet->frame_h;
    int64_t x0=cx<0?0:cx/tw,y0=cy<0?0:cy/th,x1=((int64_t)cx+c->w+tw-1)/tw,y1=((int64_t)cy+c->h+th-1)/th;
    if(x1>m->width) x1=m->width;
    if(y1>m->height) y1=m->height;
    for(int64_t y=y0;y<y1;y++) for(int64_t x=x0;x<x1;x++) {
        uint16_t tile=m->tiles[(size_t)y*m->width+(size_t)x];
        if(tile!=65535) lap_sprite_draw(c,m->sheet,tile,(int)(x*tw-cx),(int)(y*th-cy),1,0,NULL);
    }
}
bool lap_collision_aabb(lap_aabb_t a,lap_aabb_t b) {
    return a.w>0&&a.h>0&&b.w>0&&b.h>0&&a.x<b.x+b.w&&b.x<a.x+a.w&&a.y<b.y+b.h&&b.y<a.y+a.h;
}
bool lap_collision_circle(float ax,float ay,float ar,float bx,float by,float br) {
    float x=ax-bx,y=ay-by,r=ar+br; return ar>=0&&br>=0&&x*x+y*y<r*r;
}
bool lap_collision_tiles(const lap_tilemap_t *m,lap_aabb_t b,uint8_t mask) {
    if(!map_valid(m)||!isfinite(b.x)||!isfinite(b.y)||!isfinite(b.w)||!isfinite(b.h)||b.w<=0||b.h<=0) return false;
    float tw=(float)m->sheet->frame_w,th=(float)m->sheet->frame_h;
    float lo=fmaxf(0,floorf(b.x/tw)),hi=fminf((float)m->width,ceilf((b.x+b.w)/tw));
    float top=fmaxf(0,floorf(b.y/th)),bottom=fminf((float)m->height,ceilf((b.y+b.h)/th));
    if(lo>=hi||top>=bottom) return false;
    for(int y=(int)top;y<(int)bottom;y++) for(int x=(int)lo;x<(int)hi;x++) if(lap_tile_flags(m,x,y)&mask) return true;
    return false;
}
bool lap_particle_emit(lap_particles_t *p,lap_particle_t q) {
    if(!p||!p->items||q.life<=0) return false;
    for(unsigned i=0;i<p->capacity;i++) if(p->items[i].life<=0) { p->items[i]=q; return true; }
    return false;
}
void lap_particles_step(lap_particles_t *p,float dt,float g) {
    if(!p||!p->items||!isfinite(dt)||dt<=0) return;
    if(dt>0.25f) dt=0.25f;
    for(unsigned i=0;i<p->capacity;i++) { lap_particle_t *q=p->items+i; if(q->life<=0) continue;
        q->life-=dt; q->vy+=g*dt; q->x+=q->vx*dt; q->y+=q->vy*dt;
    }
}
void lap_particles_draw(lap_canvas_t *c,const lap_particles_t *p,int cx,int cy) {
    if(!c||!c->d||!p||!p->items) return;
    for(unsigned i=0;i<p->capacity;i++) { const lap_particle_t *q=p->items+i; float x=q->x-cx,y=q->y-cy;
        if(q->life>0&&x>=0&&y>=0&&x<c->w&&y<c->h) lc_pset(c,(int)x,(int)y,q->ink);
    }
}
void lap_shake_step(lap_shake_t *s,uint32_t dt) {
    if(!s) return;
    if(dt>=s->left_ms||!s->duration_ms||s->amplitude<=0) { s->left_ms=0; s->x=s->y=0; return; }
    s->left_ms-=dt;
    unsigned amp=(unsigned)((uint64_t)(unsigned)s->amplitude*s->left_ms/s->duration_ms);
    if(amp>4096) amp=4096;
    s->seed=s->seed*1664525u+1013904223u; s->x=(int)(s->seed%(amp*2+1))-(int)amp;
    s->seed=s->seed*1664525u+1013904223u; s->y=(int)(s->seed%(amp*2+1))-(int)amp;
}
void lap_fade(lap_canvas_t *c,uint8_t ink,float amount) {
    if(!c||!c->d||amount<=0) return;
    for(int y=0;y<c->h;y++) for(int x=0;x<c->w;x++) if(amount>=1||amount>lc_bayer(x,y)) lc_pset(c,x,y,ink);
}
