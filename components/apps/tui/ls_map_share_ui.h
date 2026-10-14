#ifndef LS_MAP_SHARE_UI_H
#define LS_MAP_SHARE_UI_H
#include "ls_mesh.h"
#include "ls_notify.h"
/* UI task; feed live receipts only, in arrival order. */
bool ls_scr_map_receive(const ls_mesh_msg_t *msg,ls_notice_t *notice);
void ls_scr_map_share_poll(void);
#endif
