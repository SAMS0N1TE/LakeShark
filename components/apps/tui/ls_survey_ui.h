#ifndef LS_SURVEY_UI_H
#define LS_SURVEY_UI_H
#include "ls_tui_screen.h"
void ls_survey_draw(tui_surface *sf, tui_rect area);
bool ls_survey_key(ls_tk_t key, char ch);
bool ls_survey_touch(int x, int y);
#endif
