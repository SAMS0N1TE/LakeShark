/* Non-drawing boundary for CARRIER/API tests using the real app cache. */
#include "ls_lap_api.h"
#include "ls_value.h"
#include <stdbool.h>
void ls_tui_image(tui_rect r,const uint16_t *p,int w,int h,uint32_t serial)
{ (void)r;(void)p;(void)w;(void)h;(void)serial; }
void ls_tui_image_ring(tui_rect r,const uint16_t *p,int w,int h,int x,int y,int dx,int dy,uint32_t serial)
{ (void)x;(void)y;(void)dx;(void)dy;ls_tui_image(r,p,w,h,serial); }
bool ls_value_read(const char *path,ls_val_t *out,const char **unit)
{ (void)path;(void)out;(void)unit;return false; }
/* Backend harnesses have no physical screen or immersive-mode owner. */
void ls_tui_screen_set_full(bool full) { (void)full; }
