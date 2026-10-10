#ifndef LS_AUDIO_VIEW_H
#define LS_AUDIO_VIEW_H
#include "ls_tui_ui.h"
/* One foreground media owner; history is allocated in PSRAM on the P4. */
void ls_audio_view_open(void);
void ls_audio_view_close(void);
void ls_audio_view_reset(void);
uint8_t ls_audio_view_color(int scheme, int level);
void ls_audio_view_draw(tui_surface *sf, tui_rect r, int view, int palette,
                        int span, bool live);
#endif
