/* ADS-B in a desk window: scr_adsb.c hands its calls here while the desk is
   drawing it (ls_tui_compact) and neither an aircraft's own page, FIS-B
   weather, MAP ONLY nor the handheld layout is up. A build without the desk
   has none of this. */
#ifndef SCR_ADSB_DESK_H
#define SCR_ADSB_DESK_H

#include "../../ls_tui_ui.h"
#include "apps/adsb/adsb_state.h"

#if LS_TUI_HAS_COMPACT

/* scr_adsb_desk.c */
void ls_adsb_desk_enter(void);
void ls_adsb_desk_draw(tui_surface *sf, tui_rect a, int64_t now);
/* False for a key that is not the table's: the handheld's letters (F R O V
   M H W + -) then do what they always did. */
bool ls_adsb_desk_key(ls_tk_t k, char ch);
bool ls_adsb_desk_touch(int col, int row);
bool ls_adsb_desk_classic(void);
bool ls_adsb_desk_classic_key(ls_tk_t k, char ch);
void ls_adsb_desk_export_dir(const char *dir);

/* scr_adsb.c: the handheld's own pieces, drawn into the panes given. */
void ls_scr_adsb_radar(tui_surface *sf, tui_rect r);                /* the map; its taps stay the handheld's */
void ls_scr_adsb_dash(tui_surface *sf, tui_rect r, int64_t now);    /* TRAFFIC, six rows */
void ls_scr_adsb_trend(tui_surface *sf, tui_rect r, const adsb_aircraft_t *a);
const char *ls_scr_adsb_alarm(int slot, int64_t now);               /* "EMERGENCY"..., NULL for none */
bool ls_scr_adsb_here(double *lat, double *lon);
/* "A3 large"; false when no identification has said. */
bool ls_scr_adsb_category(const adsb_aircraft_t *a, char *out, size_t n);
int  ls_scr_adsb_sort_get(void);                                    /* 0 table, 1 nearest, 2 lowest */
void ls_scr_adsb_sort_set(int sort);
void ls_scr_adsb_page(bool on);                                     /* the selected aircraft's own page */
const char *ls_scr_adsb_follow(void);
bool ls_scr_adsb_letter(char ch);                                   /* one of the handheld's letters */
void ls_scr_adsb_map_at(double lat, double lon);                    /* MAP, centred there */

#endif
#endif /* SCR_ADSB_DESK_H */
