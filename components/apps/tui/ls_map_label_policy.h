#ifndef LS_MAP_LABEL_POLICY_H
#define LS_MAP_LABEL_POLICY_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MAP_LABEL_MEMORY 128
#define MAP_LABEL_HOLD_US 400000
typedef struct {
    uint32_t key, used;
    int16_t dx, dy;
    bool visible;
    int64_t retry;
} map_label_memory;
typedef struct { map_label_memory at[MAP_LABEL_MEMORY]; uint32_t tick; } map_label_history;
typedef bool (*map_label_free_fn)(void *, int, int, int, int);

static inline uint32_t map_label_key(const char *name, unsigned domain) {
    uint32_t h=2166136261u ^ domain;
    for(;*name;name++) h=(h^(uint8_t)*name)*16777619u;
    return h?h:1;
}
/* Keep the previous anchor-relative slot. A newly blocked name disappears
 * immediately (live overlays win), then waits 400 ms before reappearing.
 * This is data-driven layout hysteresis, not an animation or a frame timer. */
static inline bool map_label_choose(map_label_history *history,uint32_t key,
        int ax,int ay,int w,int rows,const void *spots,int stride,int count,
        int64_t now,map_label_free_fn free_at,void *ctx,int *x,int *y) {
    map_label_memory *m=NULL,*old=&history->at[0];
    for(int i=0;i<MAP_LABEL_MEMORY;i++) {
        map_label_memory *p=&history->at[i];
        if(p->key==key) { m=p;break; }
        if(p->used<old->used) old=p;
    }
    if(!m) { m=old;memset(m,0,sizeof(*m));m->key=key; }
    m->used=++history->tick;
    if(m->visible) {
        *x=ax+m->dx;*y=ay+m->dy;
        if(free_at(ctx,*x,*y,w,rows)) return true;
        m->visible=false;m->retry=now+MAP_LABEL_HOLD_US;return false;
    }
    if(now<m->retry) return false;
    for(int i=0;i<count;i++) {
        const int *p=(const int *)spots+i*stride;
        if(!free_at(ctx,p[0],p[1],w,rows)) continue;
        *x=p[0];*y=p[1];m->dx=*x-ax;m->dy=*y-ay;
        m->visible=true;m->retry=0;return true;
    }
    m->retry=now+MAP_LABEL_HOLD_US;return false;
}
#endif
