/* A waterfall drawn in panel pixels behind image cells, for any screen.

   The owner pushes lines of byte codes (0 nothing .. 255 loudest), one per
   time step, `bins` values each, and says which bins each pixel along the
   frequency axis reads. The renderer keeps the lines, turns them into
   pixels through a 256-entry colour table, and hands the panel a ring
   picture: each frame only the new lines are drawn, in place, and the panel
   moves what it already shows instead of drawing the whole picture again
   (ls_tui_image_ring). A change of colours or axis redraws every line from
   the history, so a held picture can be read again in a new way.

   Two flows: LEFT, time running right to left with the newest line at the
   right edge and frequency up the picture (MUSIC); DOWN, the newest line at
   the top and frequency across (the RF waterfalls).

   Everything is PSRAM; nothing here puts more than a few words on the
   caller's stack. One UI task owns a renderer; it is not thread safe. */
#ifndef LS_PIXEL_WF_H
#define LS_PIXEL_WF_H
#include <stdbool.h>
#include <stdint.h>
#include "ls_tui.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LS_PWF_LEFT, LS_PWF_DOWN } ls_pwf_flow_t;

/* A colour map: stops at 0..255 along the scale, ascending, the first at 0. */
typedef struct {
    uint8_t at, r, g, b;
} ls_pwf_stop_t;

typedef struct {
    ls_pwf_flow_t flow;
    int w, h;             /* picture pixels                                  */
    int bins;             /* codes per line                                  */
    int lines;            /* history held: the picture's extent along time   */
    int freq_px;          /* the picture's extent along frequency            */
    uint8_t *hist;        /* lines x bins                                    */
    uint16_t *img;        /* w x h RGB565, a ring along time                 */
    /* Bins read by each pixel along frequency, both inclusive, in picture
       order: LEFT row 0 is the top (highest), DOWN column 0 the left. */
    int16_t *first, *last;
    uint16_t lut[256];
    int head, filled, fresh;
    int pos;              /* LEFT: next column, the left edge; DOWN: top row */
    int moved;            /* pixels moved since the panel was last told      */
    bool redraw;
    uint32_t serial;
    int64_t composed_us;
    uint32_t stat_px;     /* pixels written by the last compose             */
    /* The marker's frequency pixel plus one, 0 for none (so a zeroed
       renderer has none), and its colour. */
    int mark1;
    uint16_t mark_colour;
} ls_pwf_t;

/* (Re)size: a new size or bin count starts an empty history. False when
   PSRAM is short; the renderer is then empty and draws nothing. */
bool ls_pwf_size(ls_pwf_t *p, ls_pwf_flow_t flow, int w, int h, int bins);
/* Give the pixels and the panel's cells back. */
void ls_pwf_free(ls_pwf_t *p);
/* Forget the history; the picture goes blank. */
void ls_pwf_clear(ls_pwf_t *p);
/* After writing first[] and last[]: every line is drawn again. */
void ls_pwf_axis_changed(ls_pwf_t *p);
/* Bins lo..hi spread evenly across the frequency axis, low frequency at the
   bottom (LEFT) or left (DOWN). */
void ls_pwf_axis_even(ls_pwf_t *p, int lo, int hi);
/* The colour table from stops, code `lo` black and `hi` the top stop;
   redraws only when the table changed. */
void ls_pwf_colours(ls_pwf_t *p, const ls_pwf_stop_t *stops, int n, float lo, float hi);
/* A dotted marker two pixels wide along time at frequency pixel px (-1 for
   none); a move draws every line again. */
void ls_pwf_mark(ls_pwf_t *p, int px, uint16_t colour);
/* The next line's codes, `bins` of them, to fill now. */
uint8_t *ls_pwf_push(ls_pwf_t *p);
/* Draw the lines pushed since the last compose into the picture, no more
   often than every `min_us` unless everything must be drawn again. */
void ls_pwf_compose(ls_pwf_t *p, int64_t now_us, int64_t min_us);
/* Put the picture behind `cells` (its size must be cells x cell pixels for
   the cheap path) and mark them as image cells. */
void ls_pwf_show(ls_pwf_t *p, tui_surface *sf, tui_rect cells);
/* Take the picture off the panel if it is the one showing. */
void ls_pwf_release(ls_pwf_t *p);

#ifdef __cplusplus
}
#endif
#endif
