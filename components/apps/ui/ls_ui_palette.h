#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* app screens used raw hues as their interface, so the same meaning
 * changed colour from screen to screen.  These are semantic roles: callers
 * choose what a colour means and this module alone chooses its RGB value. */
typedef enum {
    LS_UI_COLOR_BACKGROUND = 0,
    LS_UI_COLOR_PANEL,
    LS_UI_COLOR_PANEL_BORDER,
    LS_UI_COLOR_TEXT,
    LS_UI_COLOR_DIM_TEXT,
    LS_UI_COLOR_ACCENT,
    LS_UI_COLOR_WARN,
    LS_UI_COLOR_ALARM,

    /* identity, not meaning. */

    LS_UI_COLOR_ID_RED,
    LS_UI_COLOR_ID_ORANGE,
    LS_UI_COLOR_ID_TEAL,
    LS_UI_COLOR_ID_BLUE,
    LS_UI_COLOR_ID_VIOLET,
    LS_UI_COLOR_ID_GREEN,
    LS_UI_COLOR_ID_ROSE,
    LS_UI_COLOR_ID_STEEL,

    LS_UI_COLOR_COUNT
} ls_ui_color_role_t;

typedef enum {
    LS_UI_THEME_ACCENT = 0,
    LS_UI_THEME_DIM,
    LS_UI_THEME_BACKGROUND,
    LS_UI_THEME_PART_COUNT
} ls_ui_theme_part_t;

uint32_t    ls_ui_palette_hex(ls_ui_color_role_t role);
const char *ls_ui_palette_name(ls_ui_color_role_t role);
uint32_t    ls_ui_palette_theme_hex(int theme, ls_ui_theme_part_t part);

#ifdef __cplusplus
}
#endif
