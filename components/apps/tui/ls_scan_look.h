/* The SUB-GHZ analyser's look, shared: the same bands, palettes and column
   glyphs, as the operator chose them in settings, for any plot that wants to
   read like it. Defined in screens/ext/scr_subghz.c. */
#pragma once
#include <stdint.h>

#define LS_SCAN_WINDOW_DB 45.0f   /* what a full-height column means, in dB */

void    ls_scan_look_load(void);
/* `over` is the reading in the same units as `span`; a plot that is not in
   dB passes its fraction scaled by LS_SCAN_WINDOW_DB. */
uint8_t ls_scan_colour(float over, float span);
int     ls_scan_level(float over, float span);
/* `eighths` is how much of the top cell is filled, 1..8. */
char    ls_scan_glyph(int level, int eighths);
