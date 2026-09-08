#pragma once

#include "lvgl.h"
#include "ui/ls_ui_palette.h"
#include "ui/ls_ui_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LS-748: `controls` is the row itself.  A value row is one wrapping line -
 * name, value, then whatever the caller adds - so a single small button rides
 * beside the value instead of taking a line of its own, and no row pays for a
 * container it does not need.  Callers keep passing `controls` as the parent;
 * do not assume it is a distinct object, and do not assume a control is at a
 * fixed child index of `row`. */
typedef struct {
    lv_obj_t *row;
    lv_obj_t *name;
    lv_obj_t *value;
    lv_obj_t *controls;
} ls_ui_value_t;

/* LS-590: primitives still let every app invent its own chrome.  This is the
 * composed screen contract: applications receive content and controls, while
 * the shared kit owns the header, optional tabs, padding, gaps and wrapping. */
typedef struct {
    lv_obj_t *root;
    lv_obj_t *header;
    lv_obj_t *name;
    lv_obj_t *readout;
    lv_obj_t *lamp;
    /* LS-736: the kit-owned strip of tab buttons.  `tabs` is still the
     * lv_tabview every app switches with lv_tabview_set_act(); its own button
     * matrix is hidden because it cannot fit its labels (see ls_ui.cpp). */
    lv_obj_t *tabbar;
    lv_obj_t *tabs;
    lv_obj_t *content;
    lv_obj_t *controls;
} ls_ui_screen_t;

/* LS-736: a tab whose action row is never scrolled away.  The body takes
 * whatever height is left and scrolls; the actions keep their measured height
 * and stay on screen at any display size. */
typedef struct {
    lv_obj_t *body;
    lv_obj_t *actions;
} ls_ui_split_t;

lv_color_t ls_ui_color(ls_ui_color_role_t role);

void      ls_ui_style_screen(lv_obj_t *screen);
/* Shell apps never add a second status header. Name is retained for callers;
 * standalone recovery screens explicitly use the constructor below. */
void      ls_ui_screen_create(lv_obj_t *parent, const char *name,
                              bool with_tabs, ls_ui_color_role_t phase_tint,
                              ls_ui_screen_t *out);
void      ls_ui_standalone_screen_create(lv_obj_t *parent, const char *name,
                              bool with_tabs, ls_ui_color_role_t phase_tint,
                              ls_ui_screen_t *out);
lv_obj_t *ls_ui_screen_add_tab(ls_ui_screen_t *screen, const char *name);
void      ls_ui_screen_set_readout(ls_ui_screen_t *screen, const char *text);
void      ls_ui_screen_set_lamp(ls_ui_screen_t *screen, bool on,
                                ls_ui_color_role_t role);
lv_obj_t *ls_ui_tab_strip(lv_obj_t *parent, lv_dir_t side, lv_coord_t size);
lv_obj_t *ls_ui_panel(lv_obj_t *parent, const char *title);
lv_obj_t *ls_ui_section(lv_obj_t *parent, const char *title);
lv_obj_t *ls_ui_row(lv_obj_t *parent, lv_flex_align_t justify);
lv_obj_t *ls_ui_controls(lv_obj_t *parent);
/* One explicit row of equal-width controls. Children never wrap: callers
 * choose the semantic grouping instead of letting LVGL split it arbitrarily. */
lv_obj_t *ls_ui_button_group(lv_obj_t *parent);
/* Compact previous/next selector for a value that is a position in a list.
 * Two fixed-width buttons on one right-aligned line - it cannot grow with the
 * number of choices the way a button per choice does (LS-736). */
lv_obj_t *ls_ui_stepper(ls_ui_value_t *row,
                        lv_event_cb_t prev_cb, void *prev_user,
                        lv_event_cb_t next_cb, void *next_user);
void      ls_ui_tab_split(lv_obj_t *tab, ls_ui_split_t *out);
/* Height left in a flex-column parent after the children built so far.
 * Measured from the laid-out tree, never derived from the display size. */
lv_coord_t ls_ui_free_height(lv_obj_t *parent);
void      ls_ui_value(lv_obj_t *parent, const char *name, ls_ui_value_t *out);
/* Longest toggle caption the kit renders: the four-character state marker plus
 * the caller's name. */
#define LS_UI_TOGGLE_TEXT_MAX 24
/* LS-748: a two-state control that carries its own state.  The caption is
 * stable - it names the setting and never becomes the state - and the state
 * shows as a marker glyph and a selected outline, not as colour alone and not
 * as a second ON/OFF reading in the row's value column.  That column is left
 * for settings with more than two choices, which must keep showing which one
 * is selected.  `text` is the setting's name; NULL gives a bare marker. */
lv_obj_t *ls_ui_toggle(ls_ui_value_t *row, const char *text, bool on,
                       lv_event_cb_t callback, void *user_data);
void      ls_ui_toggle_set(lv_obj_t *toggle, bool on);
lv_obj_t *ls_ui_readout(lv_obj_t *parent, const char *text);
/*LS-786  A readout placed directly on a column panel, not inside a value
 * row: full width, its own height, and no flex growth to spread the panel. */
lv_obj_t *ls_ui_note(lv_obj_t *parent, const char *text);
void      ls_ui_readout_set(lv_obj_t *readout, const char *text);
lv_obj_t *ls_ui_lamp(lv_obj_t *parent, ls_ui_color_role_t role);
void      ls_ui_lamp_set(lv_obj_t *lamp, bool on, ls_ui_color_role_t role);
lv_obj_t *ls_ui_button(lv_obj_t *parent, const char *text,
                       ls_ui_button_role_t role,
                       lv_event_cb_t callback, void *user_data,
                       lv_obj_t **out_label);
void      ls_ui_button_set_role(lv_obj_t *button, ls_ui_button_role_t role);
lv_obj_t *ls_ui_group_button(lv_obj_t *group, const char *text,
                             ls_ui_button_role_t role,
                             lv_event_cb_t callback, void *user_data,
                             lv_obj_t **out_label);
lv_obj_t *ls_ui_hold_button(lv_obj_t *parent, const char *text,
                            uint32_t hold_ms, ls_ui_button_role_t role,
                            lv_event_cb_t callback, void *user_data);
/* LS-1012: one full-width line stating the hold rule before anything is
 * pressed, and narrating a hold, a refusal and a confirmation while one
 * happens.  Place it above the controls it belongs to; several hold buttons
 * may share one.  Wording and colour come from ls_safe_screen_hint.c, which
 * the host gate pins. */
lv_obj_t *ls_ui_hold_hint(lv_obj_t *parent, uint32_t hold_ms);
/* As ls_ui_hold_button(), narrating into `hint`.  `action` is the name quoted
 * back to the operator ("RESTART"); NULL falls back to the button caption. */
lv_obj_t *ls_ui_hold_button_hinted(lv_obj_t *parent, const char *text,
                                   uint32_t hold_ms, ls_ui_button_role_t role,
                                   lv_event_cb_t callback, void *user_data,
                                   lv_obj_t *hint, const char *action);

/* Shared styling for content widgets whose behaviour remains app-owned. */
void ls_ui_style_content(lv_obj_t *obj);
void ls_ui_style_table(lv_obj_t *table);
void ls_ui_style_scroll_panel(lv_obj_t *obj);
void ls_ui_style_plot(lv_obj_t *obj);
void ls_ui_style_circle(lv_obj_t *obj, ls_ui_color_role_t role, bool outline);
void ls_ui_style_overlay(lv_obj_t *obj);
void ls_ui_style_card(lv_obj_t *obj, bool newest);
void ls_ui_style_lcd_row(lv_obj_t *obj);
void ls_ui_frame_color(lv_obj_t *obj, lv_color_t color);

#define LS_UI_BACKGROUND   ls_ui_color(LS_UI_COLOR_BACKGROUND)
#define LS_UI_PANEL        ls_ui_color(LS_UI_COLOR_PANEL)
#define LS_UI_PANEL_BORDER ls_ui_color(LS_UI_COLOR_PANEL_BORDER)
#define LS_UI_TEXT         ls_ui_color(LS_UI_COLOR_TEXT)
#define LS_UI_DIM_TEXT     ls_ui_color(LS_UI_COLOR_DIM_TEXT)
#define LS_UI_ACCENT       ls_ui_color(LS_UI_COLOR_ACCENT)
#define LS_UI_WARN         ls_ui_color(LS_UI_COLOR_WARN)
#define LS_UI_ALARM        ls_ui_color(LS_UI_COLOR_ALARM)

#ifdef __cplusplus
}
#endif
