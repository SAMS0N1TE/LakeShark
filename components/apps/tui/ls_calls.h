#ifndef LS_CALLS_H
#define LS_CALLS_H
#include "ls_options.h"
extern const ls_opt_ctx_t ls_calls_options;
void ls_calls_open(const char *receiver);
void ls_calls_leave(void);
bool ls_calls_active(void);
void ls_calls_draw(tui_surface *sf, tui_rect area);
bool ls_calls_key(ls_tk_t key, char ch);
bool ls_calls_touch(int col, int row);
#define LS_CALLS_MENU { .label = "Call archive", .kind = LS_OPT_MENU, .sub = &ls_calls_options }
#endif
