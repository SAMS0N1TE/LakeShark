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
/* Picture view: MAP's palette as 0xRRGGBB (ground, water, park, building, road_lo, road_hi,
   major), relief 0..100. Then request a pw x ph picture for a tw x th cell area; true while
   dst holds one (copied in when *serial differs), names then come from ls_carto_map_labels. */
void ls_carto_map_pixel_style(const uint32_t pal[7],int relief);
bool ls_carto_map_pixels(double lat,double lon,unsigned z,int pw,int ph,int tw,int th,bool labels,uint16_t *dst,uint32_t *serial,
                         double *flat,double *flon,unsigned *fz);
size_t ls_carto_map_files(char (*)[128],size_t);
#else
#define LS_CARTOCORE_AVAILABLE 0
static inline bool ls_carto_map_draw(tui_surface *s,tui_rect a,double lat,double lon,unsigned z,bool b,bool labels) {
    (void)s;(void)a;(void)lat;(void)lon;(void)z;(void)b;(void)labels;return false;
}
static inline size_t ls_carto_map_files(char (*p)[128],size_t n) { (void)p;(void)n;return 0; }
static inline size_t ls_carto_map_labels(ls_carto_label *p,size_t n,int w,int h) { (void)p;(void)n;(void)w;(void)h;return 0; }
static inline void ls_carto_map_pixel_style(const uint32_t p[7],int e) { (void)p;(void)e; }
static inline bool ls_carto_map_pixels(double a,double b,unsigned z,int pw,int ph,int tw,int th,bool l,uint16_t *d,uint32_t *s,
                                       double *fa,double *fo,unsigned *fz) {
    (void)fa;(void)fo;(void)fz;
    (void)a;(void)b;(void)z;(void)pw;(void)ph;(void)tw;(void)th;(void)l;(void)d;(void)s;return false; }
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
