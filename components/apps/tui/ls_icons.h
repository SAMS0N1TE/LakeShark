/* Pixel art for the directory, drawn out of quadrant cells. */

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
    LS_ICON__COUNT
} ls_icon_t;

/* 6x6 cells at (x, y), clipped to `clip`. */

void ls_icon_draw(tui_surface *sf, tui_rect clip, int x, int y,
                  int icon, uint8_t attr);

#ifdef __cplusplus
}
#endif

#endif /* LS_ICONS_H */
