/* HERE: the systems and stations near you from the data packs on the card.

   One key in P25, FM and FREQS opens it over the app's own area: the GPS
   key, PACKS and BACK on one row, a radar of what is around (each system
   at its real bearing and distance), and the list, nearest first. A tap on
   a row opens its frequencies; a tap on a frequency tunes it (through FREQS'
   TUNE, so it lands in the app that mode belongs to), and an app may give
   the whole site an action of its own (P25: hunt its control channel).
   With no pack for here the list says so and offers PACKS and INSTALL MY
   AREA. Reads only the RAM list (ls_packs_nearest) while drawing; the card
   is read once, when a row is opened. UI task only. */
#ifndef LS_HERE_H
#define LS_HERE_H

#include <stdbool.h>
#include <stddef.h>
#include "ls_packs.h"
#include "ls_tui_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The whole site at once: P25 writes it as a profile and hunts. Answers
   whether it went; `msg` says what happened either way. */
typedef bool (*ls_here_site_fn)(const ls_pack_sys_t *sys, const ls_pack_freq_t *f, int n,
                                char *msg, size_t cap);

typedef struct {
    uint16_t kinds;            /* LS_PK_* the app lists */
    uint8_t  hue;              /* the app's colour */
    const char *site_label;    /* the site action's key, NULL for none */
    ls_here_site_fn site;
} ls_here_cfg_t;

void ls_here_open(const ls_here_cfg_t *cfg);
void ls_here_close(void);
/* The overlay is up for this cfg (each app passes its own). */
bool ls_here_up(const ls_here_cfg_t *cfg);
void ls_here_draw(tui_surface *sf, tui_rect area);
bool ls_here_key(ls_tk_t k, char ch);
bool ls_here_touch(int col, int row);
/* The key's value for an app's bar: "12 near", "NO PACK". */
const char *ls_here_count(const ls_here_cfg_t *cfg);

/* Open PACKS, coming back to the screen showing now on BACK (the screen
   index in ls_packs_back, -1 for HOME). */
void ls_packs_app_open(void);
extern int ls_packs_back;
/* Each kind's letter and colour, in LS_PK_* bit order, for HERE and PACKS. */
extern const char LS_HERE_GLYPH[];
extern const uint8_t LS_HERE_HUE[LS_PK_COUNT];

#ifdef __cplusplus
}
#endif
#endif
