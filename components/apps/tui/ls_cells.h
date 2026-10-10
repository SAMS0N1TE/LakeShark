#ifndef LS_CELLS_H
#define LS_CELLS_H
#include "ls_cartocore_map.h"
#include "ls_tui_screen.h"
#define LS_CELLS_HITS 8
#define LS_CELLS_PLACES 32
#define LS_CELLS_PREVIEW 48
typedef struct { char name[64],parent[64]; double lat,lon; unsigned kind; } ls_place_row;
typedef struct {
    char query[64],server[256],message[128],credit[160],queue_name[64];
    ls_place_row hits[LS_CELLS_HITS],picked;double queue_lat,queue_lon;
    unsigned count,radius,cells,installed,unavailable,preview_count,generation;
    uint16_t preview[LS_CELLS_PREVIEW];uint8_t present[LS_CELLS_PREVIEW];
    uint64_t bytes,free,done,total;bool busy,ready,downloading,queued;
    char library[LS_CELLS_PLACES][64];uint32_t keys[LS_CELLS_PLACES];unsigned library_count;
} ls_cells_view;
#if LS_CARTOCORE_AVAILABLE
bool ls_cells_snapshot(ls_cells_view *out);
bool ls_cells_query(const char *text);
bool ls_cells_pick(unsigned index,unsigned radius);
bool ls_cells_action(unsigned action,uint32_t key); /* 1 download, 2 delete, 3 update, 4 refresh */
bool ls_cells_server(const char *url);
#else
static inline bool ls_cells_snapshot(ls_cells_view *v) { (void)v;return false; }
static inline bool ls_cells_query(const char *s) { (void)s;return false; }
static inline bool ls_cells_pick(unsigned i,unsigned r) { (void)i;(void)r;return false; }
static inline bool ls_cells_action(unsigned a,uint32_t k) { (void)a;(void)k;return false; }
static inline bool ls_cells_server(const char *s) { (void)s;return false; }
#endif
void ls_place_search_open(bool jump);
void ls_place_search_progress(void);
void ls_place_search_close(void);
bool ls_place_search_active(void);
bool ls_place_search_key(ls_tk_t key,char ch);
bool ls_place_search_touch(int x,int y);
void ls_place_search_draw(tui_surface *sf,tui_rect area);
#endif
