/* RF SURVEY heat and FOX HUNT bearings, drawn over MAP.

   MAP hands over how to turn a place into canvas dots (ls_map_ink.h) and
   these draw into the same ink canvas as its other layers, FIELD style:
   heat as a stipple of dots coloured by level (sparse, so the map reads
   through), the walked track as a faint dashed line, each bearing as a line
   from where it was taken, the fix as a cross inside its error ellipse. A
   small legend says what the colours mean, and a tap on it picks the
   channel and the measure. UI task only. */

#ifndef LS_RFMAP_H
#define LS_RFMAP_H

#include <stdbool.h>
#include <stdint.h>

#include "tuilib/tui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*ls_rfmap_dot_t)(double lat, double lon, int *x, int *y);

typedef struct {
    ls_rfmap_dot_t dot;     /* place to canvas dots; false off the map */
    int w, h;               /* canvas size in dots */
    int64_t now_us;
} ls_rfmap_frame_t;

bool ls_rfmap_has_heat(void);
bool ls_rfmap_has_fox(void);
void ls_rfmap_heat(const ls_rfmap_frame_t *f);
void ls_rfmap_fox(const ls_rfmap_frame_t *f);
/* The legend in the top left of `body`; returns its rectangle (h 0 when
   nothing is drawn), which a tap on should go to ls_rfmap_pick. */
tui_rect ls_rfmap_legend(tui_surface *sf, tui_rect body, bool heat, bool fox);
void ls_rfmap_pick(void);

/* Five steps of the heat ramp, coolest first: the colour and the dot glyph
   a legend shows for each. Shared with the SURVEY screen's own heat view. */
uint8_t ls_rfmap_hue(int step);
char    ls_rfmap_glyph(int step);
int     ls_rfmap_step(float v, float lo, float hi);

#ifdef __cplusplus
}
#endif
#endif
