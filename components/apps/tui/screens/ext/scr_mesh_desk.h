/* MESH in a desk window: scr_mesh.c hands its calls here while the desk is
   drawing it (ls_tui_compact) and the handheld layout has not been asked
   for. A build without the desk has none of this. */
#ifndef SCR_MESH_DESK_H
#define SCR_MESH_DESK_H

#include "../../ls_tui_ui.h"
#include "ls_mesh.h"

#if LS_TUI_HAS_COMPACT

/* scr_mesh_desk.c */
void ls_mesh_desk_enter(void);
void ls_mesh_desk_draw(tui_surface *sf, tui_rect a, const ls_mesh_stats_t *st);
bool ls_mesh_desk_key(ls_tk_t k, char ch);
bool ls_mesh_desk_touch(int col, int row);
/* The handheld layout was asked for (View, or Ctrl+L); Ctrl+L still
   answers so there is a way back. */
bool ls_mesh_desk_classic(void);
bool ls_mesh_desk_classic_key(ls_tk_t k, char ch);
/* Where exports go; the card's /sdcard/mesh unless told otherwise (lssim). */
void ls_mesh_desk_export_dir(const char *dir);

/* scr_mesh.c: the handheld's own actions, so a menu here and a key there
   cannot come to do different things. */
bool ls_scr_mesh_slash(const char *text);          /* a /command; false when it was not one */
const char *ls_scr_mesh_said(void);                /* the result line while it lasts, else NULL */
void ls_scr_mesh_say(const char *text);
void ls_scr_mesh_options(void);
void ls_scr_mesh_radio(void);
void ls_scr_mesh_start(bool on);
void ls_scr_mesh_arm(bool on);
/* What the node page does: 0 chat with it, 1 save as a contact, 2 show on
   MAP, 3 its INSPECT page, 4 the node page itself. 3 and 4 put the
   handheld's page up in the window until Esc. */
void ls_scr_mesh_node_act(const char *id, int which);
/* Where a typed line goes, shared with the handheld layout. A DM when `id`
   is not empty, else channel `chan`. */
void ls_scr_mesh_dest_set(const char *id, const char *name, int chan);
void ls_scr_mesh_dest_get(char id[17], int *chan);

#endif
#endif /* SCR_MESH_DESK_H */
