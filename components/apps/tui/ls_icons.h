/* Pixel art for the directory: colour dots packed into sextant cells, two
   colours a cell, the way COMPASS draws (icons/app_icons.py). */

#ifndef LS_ICONS_H
#define LS_ICONS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "ls_tui.h"

/* Ten cells by six, not six by six. */

#define LS_ICON_COLS 10
#define LS_ICON_ROWS 6

typedef enum {
    LS_ICON_NONE = 0,
    LS_ICON_TOWER,      /* P25, trunking, anything with a control channel */
    LS_ICON_WAVE,       /* FM and the analogue family                     */
    LS_ICON_PLANE,      /* ADS-B                                          */
    LS_ICON_RECORD,     /* REC                                            */
    LS_ICON_FALLS,      /* the waterfall / scout                          */
    LS_ICON_MAP,
    LS_ICON_FILES,
    LS_ICON_GEAR,
    LS_ICON_PAGER,      /* POCSAG / FLEX                                  */
    LS_ICON_CHIP,       /* system, health                                 */
    LS_ICON_STAR,       /* user apps loaded from the card                 */
    LS_ICON_SHARK,      /* home                                           */
    LS_ICON_MESH,       /* MeshCore: nodes and the links between them     */
    /* GPS had the map pin and RADIOS had the P25 tower. Two tiles
       with one picture is worse than an imperfect picture: it says they do
       the same thing. */
    LS_ICON_SAT,        /* GPS: a satellite over a horizon                */
    LS_ICON_POWER,      /* RADIOS: what is on, and how to switch it off   */
    LS_ICON_WIRELESS,
    LS_ICON_LABS,
    LS_ICON_JOURNAL,
    LS_ICON_COMPASS,    /* COMPASS: a ring and its needle                 */
    LS_ICON_MUSIC,      /* MUSIC: a cassette, distinct from FM's wave */
    LS_ICON_EXPERIMENT, /* EXPERIMENTS: a magnifier over a wave           */
    LS_ICON_TERMINAL,   /* TERMINAL: a screen with a prompt on it         */
    LS_ICON_UPDATE,     /* UPDATE: an arrow coming down into a tray       */
    LS_ICON_TILES,      /* four map sheets */
    LS_ICON_DRONE,      /* DRONES: four rotors around a central body */
    LS_ICON_SWEEP,      /* SWEEP: a radar ring, beam and contact */
    LS_ICON_DETECTOR,   /* DETECTOR: a warning triangle */
    /* New numbers go last: native apps name built-in icons by number. */
    LS_ICON_CELL,       /* CELL WATCH: a handset and its bars, not P25's mast */
    LS_ICON_SUBGHZ,     /* SUB-GHZ: a key fob and its burst, not REC's lamp */
    LS_ICON_HEAT,       /* RF SURVEY: heat on the ground, and the walk */
    LS_ICON_FOX,        /* FOX HUNT: bearings crossing in their ellipse */
    LS_ICON_DIGI,       /* DIGI: the FT8 Costas array, tones stepping     */
    LS_ICON_TMACH,      /* TIME MACHINE: a dial turning back over a burst */
    LS_ICON_RADIO,      /* RADIO: a handheld receiver, not FM's wave        */
    LS_ICON__COUNT
} ls_icon_t;

/* 10x6 cells at (x, y), clipped to `clip`. The icon's accent is the
   attribute's colour: its foreground, or its background when the foreground
   is black (a selected tile); clear dots are its background. */

void ls_icon_draw(tui_surface *sf, tui_rect clip, int x, int y,
                  int icon, uint8_t attr);

/* Character art for an icon the firmware does not have: six rows of up to ten
   printable ASCII characters, as a native app's manifest carries it. `slot`
   (0..LS_ICON_ART_MAX-1) is the caller's; setting it again replaces the art.
   Returns an icon number ls_icon_draw takes, or `fallback` when the slot is
   out of range or the art is empty. The art is copied. */
#define LS_ICON_ART_FIRST 64
#define LS_ICON_ART_MAX   24
int ls_icon_art(int slot, const char art[LS_ICON_ROWS][LS_ICON_COLS + 1], int fallback);
/* The same for a colour icon in dots, 20 x 18, colours 0-15, 0xFF clear. */
int ls_icon_px(int slot, const uint8_t px[LS_ICON_ROWS * 3][LS_ICON_COLS * 2], int fallback);
/* Draw such an icon: each cell keeps its two commonest colours as a sextant,
   as COMPASS does; clear dots are the attribute's background. */
void ls_icon_draw_px(tui_surface *sf, tui_rect clip, int x, int y,
                     const uint8_t px[LS_ICON_ROWS * 3][LS_ICON_COLS * 2], uint8_t attr);

#ifdef __cplusplus
}
#endif

#endif /* LS_ICONS_H */
