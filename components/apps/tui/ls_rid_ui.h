#ifndef LS_RID_UI_H
#define LS_RID_UI_H
#include "ls_app.h"
#include "ls_notify.h"
#ifdef __cplusplus
extern "C" {
#endif
extern const ls_tui_screen_t ls_scr_drones;
extern const ls_app_doc_t ls_doc_drones;
void ls_rid_draw(tui_surface *sf,tui_rect area);
bool ls_rid_key(ls_tk_t key,char ch);
bool ls_rid_touch(int x,int y);
bool ls_rid_notice(ls_notice_t *out);
#ifdef __cplusplus
}
#endif
#endif
