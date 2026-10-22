#ifndef LS_SPLASH_H
#define LS_SPLASH_H
#include "ls_tui.h"
#ifdef __cplusplus
extern "C" {
#endif
void ls_splash_draw(tui_surface *sf, int cols, int rows, int frame, int frames);
/* A short line for the splash's corner, the build's version; kept by pointer. */
void ls_splash_set_caption(const char *text);
/* What boot is doing, under the progress line, in place of the labels the
   frame count picks; NULL goes back to those. Kept by pointer. */
void ls_splash_set_status(const char *text);
#ifdef __cplusplus
}
#endif
#endif
