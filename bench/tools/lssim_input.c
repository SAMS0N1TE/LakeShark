/* Host input uses the production finger channels and grid conversion. */
#include "ls_tui_touch.h"
#include "ls_tui.h"
#include "ls_lap_sys.h"
#include <stddef.h>

/* Keep native host apps informed as well as screens reading the TUI finger. */
#define ls_tui_touch_remote host_touch_remote
#define ls_tui_touch_client host_touch_client
#define ls_tui_touch_stats host_touch_stats
#include "../../components/apps/tui/ls_tui_touch.c"
#undef ls_tui_touch_remote
#undef ls_tui_touch_client
#undef ls_tui_touch_stats

static void native_finger(int gx,int gy,bool down)
{
    int cw=10,ch=17;
    ls_tui_geometry(NULL,NULL,&cw,&ch);
    ls_lap_sys_host_finger(gx,gy,cw,ch,down);
}
void ls_tui_touch_remote(int col,int row,int gx,int gy,bool down)
{
    host_touch_remote(col,row,gx,gy,down);
    native_finger(gx,gy,down);
}
void ls_tui_touch_client(int col,int row,int gx,int gy,bool down)
{
    host_touch_client(col,row,gx,gy,down);
    native_finger(gx,gy,down);
}
bool ls_touch_read(uint16_t *x,uint16_t *y,bool *pressed)
{ (void)x;(void)y;*pressed=false;return true; }
