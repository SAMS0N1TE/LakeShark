/* LakeShark LR: CATCH needs an RTL-SDR to spot bursts, so SUB-GHZ has no
   CATCH tab. These keep its few hooks inert. */
#include "ls_catch_ui.h"

void ls_catch_body_draw(tui_surface *sf, tui_rect a) { (void)sf; (void)a; }
bool ls_catch_body_touch(int x, int y) { (void)x; (void)y; return false; }
void ls_catch_enter(void) {}
bool ls_catch_key(ls_tk_t k, char c) { (void)k; (void)c; return false; }
const ls_opt_ctx_t *ls_catch_options(void) { return 0; }
bool ls_catch_running(void) { return false; }
void ls_catch_save_selected(void) {}
void ls_catch_stop(void) {}
void ls_catch_toggle(void) {}
void ls_catch_watch_cycle(void) {}
const char *ls_catch_watch_label(bool *on) { if (on) *on = false; return "OFF"; }
