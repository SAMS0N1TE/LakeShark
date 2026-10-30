#ifndef LS_CELLS_H
#define LS_CELLS_H
#include "ls_cartocore_map.h"
#include "ls_tui_screen.h"
#define LS_CELLS_HITS 8
#define LS_CELLS_PLACES 32
#define LS_CELLS_PREVIEW 48
typedef struct { char name[64],parent[64]; double lat,lon; unsigned kind; } ls_place_row;
typedef struct {
    char states[8192],state[128],progress[128],outlines[128],free_max_zoom[8];float maps_progress;
    char query[64],server[256],message[128],credit[160],queue_name[64];
    ls_place_row hits[LS_CELLS_HITS],picked;double queue_lat,queue_lon;
    unsigned count,radius,cells,installed,unavailable,preview_count,generation,action_done;
    uint16_t preview[LS_CELLS_PREVIEW];uint8_t present[LS_CELLS_PREVIEW];
    uint64_t bytes,free,done,total;bool busy,ready,downloading,queued;
    char library[LS_CELLS_PLACES][64];uint32_t keys[LS_CELLS_PLACES];unsigned library_count;
} ls_cells_view;
#if LS_CARTOCORE_AVAILABLE
bool ls_cells_snapshot(ls_cells_view *out);
/* Text slots: 0 states, 1 progress, 2 state, 3 outline availability, 4 free maximum zoom. */
bool ls_cells_maps_text(unsigned which,char *out,size_t cap);
bool ls_cells_maps_progress(float *out);
bool ls_cells_state_action(bool remove,const char *st);
bool ls_cells_query(const char *text);
bool ls_cells_pick(unsigned index,unsigned radius);
bool ls_cells_action(unsigned action,uint32_t key); /* 1 download, 2 delete, 3 update, 4 refresh */
bool ls_cells_server(const char *url);
#else
static inline bool ls_cells_maps_progress(float *out) { if(out)*out=0;return false; }
static inline bool ls_cells_maps_text(unsigned w,char *o,size_t n) { (void)w;if(n)o[0]=0;return false; }
static inline bool ls_cells_state_action(bool r,const char *s) { (void)r;(void)s;return false; }
static inline bool ls_cells_snapshot(ls_cells_view *v) { (void)v;return false; }
static inline bool ls_cells_query(const char *s) { (void)s;return false; }
static inline bool ls_cells_pick(unsigned i,unsigned r) { (void)i;(void)r;return false; }
static inline bool ls_cells_action(unsigned a,uint32_t k) { (void)a;(void)k;return false; }
static inline bool ls_cells_server(const char *s) { (void)s;return false; }
#endif
void ls_place_search_open(bool jump);
void ls_place_search_goto(void (*go)(double lat,double lon));
void ls_place_search_progress(void);
/* Opens on the downloaded places, to look at or delete one. */
void ls_place_search_installed(void);
void ls_place_search_close(void);
bool ls_place_search_active(void);
const char *ls_place_search_hint(void);
bool ls_place_search_key(ls_tk_t key,char ch);
bool ls_place_search_touch(int x,int y);
void ls_place_search_draw(tui_surface *sf,tui_rect area);
#endif
