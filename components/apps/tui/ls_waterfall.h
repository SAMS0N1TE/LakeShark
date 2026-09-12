/* The waterfall. */

#ifndef LS_WATERFALL_H
#define LS_WATERFALL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "ls_tui.h"
#include "ls_tui_screen.h"

/* History is PSRAM and sized once. 256 bins is more than the widest panel has
   columns, so the display never interpolates upward; 128 rows is about two
   minutes of scroll at the slowest speed. */
/* How many controls the instrument's own bar carries. Named because three
   places size arrays from it and a mismatch between them is a stack write
   past the end, not a layout bug. */
/* Ten, not nine. Portrait wraps the bar at five to a row, so nine
   left a hole in the bottom right and the tenth slot was free for the asking.
   CONTRAST is what went in it: REF and RANGE are the two halves of the same
   knob and neither name says so, which is why the screen read as having no
   contrast control at all. */
#define LS_WF_BTNS      10

#define LS_WF_BINS_MAX  256
#define LS_WF_ROWS_MAX  128

typedef enum {
    LS_WF_OWNER_NONE = 0,
    LS_WF_OWNER_P25,
    LS_WF_OWNER_FM,
    LS_WF_OWNER_REC,
    /* The board's own LoRa radio, swept. A separate owner from the
       dongle's decoders because a claim change is what clears the history,
       and a 26 MHz swept survey and a 240 kHz FFT must never be read as one
       picture - the bins mean different widths and the rows different
       durations. */
    LS_WF_OWNER_LORA,
    LS_WF_OWNER_USER,
} ls_wf_owner_t;

typedef struct {
    uint32_t center_hz;
    uint32_t span_hz;
    float    floor_db;   /* what 0.0 means, for the readout                */
    float    top_db;     /* what 1.0 means                                 */
    bool     live;       /* source says this row describes the present     */
    const char *note;    /* short caption, e.g. "following voice"          */
    /* How long the source takes to make one row, in ms, when it can
       say; 0 when it cannot. A band sweep can, and it is seconds - see
       stall_after_ms in ls_waterfall.c for what reads it. */
    uint32_t period_ms;
} ls_wf_feed_t;

typedef enum {
    LS_WF_PAL_HEAT = 0,  /* blue -> cyan -> green -> yellow -> white       */
    LS_WF_PAL_ICE,       /* deep blue -> white, calm, good in daylight     */
    LS_WF_PAL_PHOSPHOR,  /* one green, intensity only                      */
    LS_WF_PAL_NEON,      /* magenta -> cyan, the loud one                  */
    LS_WF_PAL__COUNT
} ls_wf_palette_t;

/* How a history cell is filled. */

typedef enum {
    LS_WF_GRAIN_SHADE = 0,
    LS_WF_GRAIN_ASCII,
    LS_WF_GRAIN_DENSE,
    LS_WF_GRAIN_BARS,
    LS_WF_GRAIN__COUNT
} ls_wf_grain_t;

typedef struct {
    uint8_t split_pct;   /* spectrum's share of the pane: 0,25,50,75,100   */
    int8_t  ref;         /* shifts the floor, -8..+8 in sixteenths         */
    uint8_t range;       /* contrast window, 4..16 in sixteenths           */
    uint8_t palette;     /* ls_wf_palette_t                                */
    uint8_t avg;         /* rows averaged into one: 1,2,4,8                */
    uint8_t decim;       /* keep one row in N: slows the scroll            */
    bool    peak_hold;   /* spectrum keeps a decaying maximum              */
    uint8_t grain;       /* ls_wf_grain_t: how a history cell is filled    */
    bool    paused;
} ls_wf_cfg_t;

typedef struct {
    uint32_t draw_us;    /* the last ls_wf_draw                            */
    uint32_t worst_us;   /* since the last claim                           */
    uint32_t pushes;     /* rows accepted                                  */
    uint32_t dropped;    /* rows refused: wrong owner, paused, decimated   */
    uint32_t row_ms;     /* mean interval between accepted rows            */
    uint32_t age_ms;     /* since the last accepted row                    */
    int      bins;       /* columns on screen now                          */
    int      rows;       /* history rows on screen now                     */
    int      hist_rows;  /* history rows held                              */
    bool     ready;      /* the PSRAM history exists                       */

    uint8_t  scale_lo, scale_hi, scale_top;
} ls_wf_stats_t;

/* Claim the instrument. A different owner clears the history, because two
   sources drawn as one picture is a lie about what was on the air. Cheap
   enough to call every frame; it only does work when the owner changes. */
void ls_wf_claim(ls_wf_owner_t owner, const char *label);
ls_wf_owner_t ls_wf_owner(void);

/* Push one spectrum row, values normalised 0..1 against feed->floor_db and
   feed->top_db. Refused, and counted as dropped, when the caller is not the
   current owner - so a background task cannot corrupt the visible picture. */
void ls_wf_push(ls_wf_owner_t owner, const float *bins, int n,
                const ls_wf_feed_t *feed);

/* The whole instrument: spectrum, waterfall, frequency scale, marker readout
   and the control bar. `area` is everything it may use. */
void ls_wf_draw(tui_surface *sf, tui_rect area);

/* A spectrum and waterfall with no chrome and no controls, for a screen that
   wants one among other panels. Same history, same settings. */
void ls_wf_draw_mini(tui_surface *sf, tui_rect area);

/* The palette slot level 0..15 is drawn in, for scale `palette`
   (ls_wf_palette_t), on a dark ground or a light one. The draw asks the
   blitter which ground it has; this is the table itself, so the bench can
   hold the light scales to running from light to dark. */
uint8_t ls_wf_level_colour(int palette, int level, bool light_ground);

/* Controls. The key path and the touch path reach the same actions; neither
   is the real one. Both return true when they consumed the input. */
bool ls_wf_key(ls_tk_t key, char ch);
bool ls_wf_touch(int col, int row);

/* Marker frequency, or 0 when there is no marker or no known span. */
uint32_t ls_wf_marker_hz(void);

/* How many rows have ever been accepted. */

uint32_t ls_wf_seq(void);

void ls_wf_stats(ls_wf_stats_t *out);

/* Why there is nothing to draw, or NULL when there is. */

const char *ls_wf_idle_reason(void);
const ls_wf_cfg_t *ls_wf_cfg(void);
void ls_wf_cfg_set(const ls_wf_cfg_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* LS_WATERFALL_H */
