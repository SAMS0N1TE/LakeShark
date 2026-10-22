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
    bool visible, held;
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
        m->visible=false;m->held=true;m->retry=now+MAP_LABEL_HOLD_US;return false;
    }
    if(now<m->retry) return false;
    m->held=false;
    for(int i=0;i<count;i++) {
        const int *p=(const int *)spots+i*stride;
        if(!free_at(ctx,p[0],p[1],w,rows)) continue;
        *x=p[0];*y=p[1];m->dx=*x-ax;m->dy=*y-ay;
        m->visible=true;m->retry=0;return true;
    }
    m->retry=now+MAP_LABEL_HOLD_US;return false;
}
/* A name that lost its slot keeps its share of a label cap until it retries;
 * otherwise the next name in rank fills the gap for one hold and goes again. */
static inline bool map_label_held(const map_label_history *history,uint32_t key) {
    for(int i=0;i<MAP_LABEL_MEMORY;i++)
        if(history->at[i].key==key) return history->at[i].held;
    return false;
}
/* Live objects have a separate bounded history: basemap names cannot evict
 * aircraft. Keys contain a domain and the object's identity, never its label
 * text, screen position or snapshot index. Storage belongs in PSRAM. */
#define MAP_STICKY_MEMORY 256
#define MAP_STICKY_BLOCK_US 2000000LL
#define MAP_STICKY_DWELL_US 5000000LL
enum { MAP_LABEL_RIGHT, MAP_LABEL_LEFT, MAP_LABEL_ABOVE, MAP_LABEL_BELOW };
typedef struct {
    uint64_t key;
    int64_t blocked_since, moved_at;
    int16_t dx, dy, width;
    bool placed, blocked, seen;
} map_sticky_entry;
typedef struct { map_sticky_entry at[MAP_STICKY_MEMORY]; } map_sticky_history;
static inline uint64_t map_object_key(unsigned domain, uint32_t id) {
    return ((uint64_t)domain << 32) | id;
}
static inline void map_sticky_begin(map_sticky_history *h) {
    for(int i=0;i<MAP_STICKY_MEMORY;i++) h->at[i].seen=false;
}
static inline void map_sticky_end(map_sticky_history *h) {
    for(int i=0;i<MAP_STICKY_MEMORY;i++)
        if(!h->at[i].seen) memset(&h->at[i],0,sizeof(h->at[i]));
}
static inline void map_sticky_retain(map_sticky_history *h,uint64_t key) {
    for(int i=0;i<MAP_STICKY_MEMORY;i++)
        if(h->at[i].key==key) { h->at[i].seen=true;return; }
}
static inline int map_label_side(int dx,int dy,int w) {
    return dx>0 ? MAP_LABEL_RIGHT : dx+w<=0 ? MAP_LABEL_LEFT :
           dy<0 ? MAP_LABEL_ABOVE : MAP_LABEL_BELOW;
}
/* Cardinal cores are 60 degrees wide. Diagonal dead bands choose a fixed
 * horizontal side; heading is consulted only for the first placement, so
 * even a turn through an entire quadrant cannot change a valid slot. */
static inline int map_air_label_side(int heading) {
    if(heading<0 || heading>=360) return MAP_LABEL_RIGHT;
    if(heading>=240 && heading<=300) return MAP_LABEL_RIGHT;
    if(heading>=60 && heading<=120) return MAP_LABEL_LEFT;
    if(heading<=30 || heading>=330) return MAP_LABEL_BELOW;
    if(heading>=150 && heading<=210) return MAP_LABEL_ABOVE;
    return heading>=180 ? MAP_LABEL_RIGHT : MAP_LABEL_LEFT;
}
static inline bool map_sticky_choose(map_sticky_history *h,uint64_t key,
        int preferred,int ax,int ay,int w,int rows,int cols,int height,
        const void *spots,int stride,int count,int64_t now,
        map_label_free_fn free_at,void *ctx,int *x,int *y) {
    map_sticky_entry *m=NULL,*empty=NULL;
    for(int i=0;i<MAP_STICKY_MEMORY;i++) {
        if(h->at[i].key==key) { m=&h->at[i];break; }
        if(!h->at[i].key && !empty) empty=&h->at[i];
    }
    /* Saturation suppresses a new label instead of evicting a live object. */
    if(!m) { if(!empty) return false;m=empty;m->key=key; }
    m->seen=true;
    if(m->placed) {
        int dx=m->dx;
        if(map_label_side(dx,m->dy,m->width)==MAP_LABEL_LEFT) dx+=m->width-w;
        *x=ax+dx;*y=ay+m->dy;
        bool off=*x<0 || *y<0 || *x+w>cols || *y+rows>height;
        if(!off && free_at(ctx,*x,*y,w,rows)) {
            m->blocked=false;m->dx=dx;m->width=w;return true;
        }
        if(!m->blocked) { m->blocked=true;m->blocked_since=now; }
        /* Off-screen placement overrides dwell; overlap never does. */
        if(!off && (now-m->blocked_since<MAP_STICKY_BLOCK_US ||
                    now-m->moved_at<MAP_STICKY_DWELL_US)) return false;
        preferred=map_label_side(dx,m->dy,w);
    }
    for(int pass=0;pass<2;pass++) for(int i=0;i<count;i++) {
        const int *p=(const int *)spots+i*stride;
        if((map_label_side(p[0]-ax,p[1]-ay,w)==preferred)!=(pass==0)) continue;
        if(p[0]<0 || p[1]<0 || p[0]+w>cols || p[1]+rows>height ||
           !free_at(ctx,p[0],p[1],w,rows)) continue;
        *x=p[0];*y=p[1];m->dx=*x-ax;m->dy=*y-ay;m->width=w;
        m->moved_at=m->placed?now:now-MAP_STICKY_DWELL_US;
        m->placed=true;m->blocked=false;return true;
    }
    return false;
}
#endif
