#include "ls_board.h"
#ifndef LS_CARTOCORE_MAP_H
#define LS_CARTOCORE_MAP_H
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#include "ls_tui.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The Detailed look: one 0xRRGGBB per class of ground and line, in this order.
   Declared for every build: MAP's palette code names the classes either way. */
enum { LS_ATLAS_LAND, LS_ATLAS_RESIDENTIAL, LS_ATLAS_INDUSTRIAL, LS_ATLAS_FARM, LS_ATLAS_PARK, LS_ATLAS_WOOD,
       LS_ATLAS_WATER, LS_ATLAS_RIVER, LS_ATLAS_BUILDING, LS_ATLAS_SERVICE, LS_ATLAS_MINOR, LS_ATLAS_SECONDARY,
       LS_ATLAS_MAJOR, LS_ATLAS_MOTORWAY, LS_ATLAS_RAIL, LS_ATLAS_BOUNDARY, LS_ATLAS_CONTOUR, LS_ATLAS_INDEX, LS_ATLAS__COUNT };
#define LS_CARTO_LABEL_MAX 64
typedef struct { int16_t x,y; uint8_t attr, label_class; char text[128]; } ls_carto_label;
#if (defined(ESP_PLATFORM) && LS_HAS_CARTOCORE) || defined(LS_CARTOCORE_HOST)
#define LS_CARTOCORE_AVAILABLE 1
bool ls_carto_map_draw(tui_surface *,tui_rect,double,double,unsigned,bool,bool);
/* STYLE over CartoCore frames: tint 0 or a TUI colour; ground 0 theme, 1 dark, 2 light. */
void ls_carto_map_style(uint8_t tint,uint8_t ground);
void ls_carto_map_leave(void);
bool ls_carto_map_prepare(double *,double *,unsigned *);
bool ls_carto_map_fresh(void);
bool ls_carto_map_select(const char *);
bool ls_carto_map_limits(unsigned *,unsigned *);
const char *ls_carto_map_status(void);
void ls_carto_hw_status(bool reset);
size_t ls_carto_map_labels(ls_carto_label *,size_t,int,int);
size_t ls_carto_map_labels_held(ls_carto_label *,size_t,int,int);
/* The last draw showed the previous frame moved (and, in dots, scaled) to the
   view while the next renders: view dot x is that frame's dot ax + x * k. */
bool ls_carto_map_held_warp(double *ax,double *ay,double *k);
/* Before ls_carto_map_draw: the view's middle in world dots (256 a tile at z). */
void ls_carto_map_view_dot(double lat,double lon,unsigned z,double x,double y);
/* Picture view: MAP's palette as 0xRRGGBB (ground, water, park, building, road_lo, road_hi,
   major), relief 0..100. Then request a pw x ph picture for a tw x th cell area; true while
   dst holds one (copied in when *serial differs), names then come from ls_carto_map_labels. */
void ls_carto_map_pixel_style(const uint32_t pal[7],int relief);
void ls_carto_map_pixel_atlas(const uint32_t ink[LS_ATLAS__COUNT],int relief);
/* Ask the worker to read what the card holds (cells, map files) for ls_carto_map_nearest;
   the serial moves when it has. Never waits. */
void ls_carto_map_scan_request(void);
unsigned ls_carto_map_scan_serial(void);
bool ls_carto_map_pixels(double lat,double lon,unsigned z,int pw,int ph,int tw,int th,bool labels,uint16_t *dst,uint32_t *serial,
                         double *flat,double *flon,unsigned *fz);
bool ls_carto_map_picture_cells(void);
bool ls_carto_map_picture_covered(void);
/* 'carto frames': the newest text frame's number, and how many tiles of the
   newest picture came from a coarser zoom (1000: all of it). */
unsigned ls_carto_map_frame_serial(void);
int ls_carto_map_picture_degraded(void);
bool ls_carto_map_failed(void);
/* The last try was short of memory; ask again now; the map open is the lean one. */
bool ls_carto_map_short(void);
void ls_carto_map_retry(void);
bool ls_carto_map_lean(void);
int ls_carto_map_nearest(double lat,double lon,double *nlat,double *nlon);
/* As ls_carto_map_nearest; *lo: the lowest zoom what covers (lat,lon) draws. */
int ls_carto_map_nearest_lo(double lat,double lon,double *nlat,double *nlon,unsigned *lo);
unsigned ls_carto_map_cells_lo(void);
int ls_carto_covers(double lat,double lon,char *out,size_t cap);
void ls_carto_status_print(void); /* what 'carto status' prints */
size_t ls_carto_map_files(char (*)[128],size_t);
#else
#define LS_CARTOCORE_AVAILABLE 0
static inline bool ls_carto_map_draw(tui_surface *s,tui_rect a,double lat,double lon,unsigned z,bool b,bool labels) {
    (void)s;(void)a;(void)lat;(void)lon;(void)z;(void)b;(void)labels;return false;
}
static inline size_t ls_carto_map_files(char (*p)[128],size_t n) { (void)p;(void)n;return 0; }
static inline size_t ls_carto_map_labels(ls_carto_label *p,size_t n,int w,int h) { (void)p;(void)n;(void)w;(void)h;return 0; }
static inline size_t ls_carto_map_labels_held(ls_carto_label *p,size_t n,int w,int h) { (void)p;(void)n;(void)w;(void)h;return 0; }
static inline bool ls_carto_map_held_warp(double *a,double *b,double *k) { (void)a;(void)b;(void)k;return false; }
static inline void ls_carto_map_view_dot(double a,double b,unsigned z,double x,double y) { (void)a;(void)b;(void)z;(void)x;(void)y; }
static inline void ls_carto_map_pixel_style(const uint32_t p[7],int e) { (void)p;(void)e; }
static inline void ls_carto_map_pixel_atlas(const uint32_t *p,int e) { (void)p;(void)e; }
static inline void ls_carto_map_scan_request(void) {}
static inline unsigned ls_carto_map_scan_serial(void) { return 0; }
static inline bool ls_carto_map_pixels(double a,double b,unsigned z,int pw,int ph,int tw,int th,bool l,uint16_t *d,uint32_t *s,
                                       double *fa,double *fo,unsigned *fz) {
    (void)fa;(void)fo;(void)fz;
    (void)a;(void)b;(void)z;(void)pw;(void)ph;(void)tw;(void)th;(void)l;(void)d;(void)s;return false; }
static inline bool ls_carto_map_picture_cells(void) { return false; }
static inline bool ls_carto_map_picture_covered(void) { return false; }
static inline unsigned ls_carto_map_frame_serial(void) { return 0; }
static inline int ls_carto_map_picture_degraded(void) { return 0; }
static inline bool ls_carto_map_failed(void) { return false; }
static inline bool ls_carto_map_short(void) { return false; }
static inline void ls_carto_map_retry(void) {}
static inline bool ls_carto_map_lean(void) { return false; }
static inline int ls_carto_map_nearest(double a,double b,double *c,double *d) { (void)a;(void)b;(void)c;(void)d;return -1; }
static inline int ls_carto_map_nearest_lo(double a,double b,double *c,double *d,unsigned *e) { (void)a;(void)b;(void)c;(void)d;(void)e;return -1; }
static inline unsigned ls_carto_map_cells_lo(void) { return 0; }
static inline int ls_carto_covers(double a,double b,char *o,size_t n) { (void)a;(void)b;(void)o;(void)n;return 0; }
static inline void ls_carto_status_print(void) {}
static inline void ls_carto_hw_status(bool reset) { (void)reset; }
static inline const char *ls_carto_map_status(void) { return "CartoCore map unavailable"; }
static inline bool ls_carto_map_fresh(void) { return false; }
static inline void ls_carto_map_leave(void) {}
static inline void ls_carto_map_style(uint8_t t,uint8_t g) { (void)t;(void)g; }
static inline bool ls_carto_map_prepare(double *a,double *b,unsigned *z) { (void)a;(void)b;(void)z;return false; }
static inline bool ls_carto_map_select(const char *p) { (void)p;return false; }
static inline bool ls_carto_map_limits(unsigned *lo,unsigned *hi) { (void)lo;(void)hi;return false; }
#endif
#ifdef __cplusplus
}
#endif
#endif
