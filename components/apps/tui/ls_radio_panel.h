#ifndef LS_RADIO_PANEL_H
#define LS_RADIO_PANEL_H
#include "ls_tui_ui.h"
#include "iq_app_control.h"

typedef struct {
    bool lists, scan_choice;
    int selected, focus, slot, preset;
    tui_rect list_area, previous, next;
    int visible[64], count, first;
    ls_btn_t buttons[6];
    char notice[64];
} ls_radio_panel_t;

typedef struct {
    bool fm;
    uint32_t frequency, standby;
    const char *mode;
    ls_iq_control_status_t receiver;
    float power;
    char detail[4][64];
} ls_radio_view_t;

void ls_radio_panel_draw(ls_radio_panel_t *, const ls_radio_view_t *, tui_surface *, tui_rect);
char ls_radio_panel_key(ls_radio_panel_t *, const ls_radio_view_t *, ls_tk_t, char);
char ls_radio_panel_touch(ls_radio_panel_t *, const ls_radio_view_t *, int, int);
#endif
