#include "ui/ls_ui.h"

#include "sdr_ui/sdr_ui.h"
#include "ui/ls_safe_screen_hint.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

lv_color_t ls_ui_color(ls_ui_color_role_t role)
{
    return lv_color_hex(ls_ui_palette_hex(role));
}

void ls_ui_style_screen(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, LS_UI_BACKGROUND, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
}

void ls_ui_style_content(lv_obj_t *obj)
{
    lv_obj_set_width(obj, lv_pct(100));
    lv_obj_set_style_pad_all(obj, 6, 0);
    lv_obj_set_style_pad_row(obj, 6, 0);
    lv_obj_set_style_pad_column(obj, 6, 0);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    /*LS-782  This styled geometry but never the background, so the container
       kept the LVGL default theme's light fill and showed as a white box on a
       dark screen - the REC/SCOUT readout panels are the visible case. */
    lv_obj_set_style_bg_color(obj, LS_UI_BACKGROUND, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    /*LS-783  AUTO, not OFF. OFF removed the clutter and the affordance with
     * it: on the REC tab the controls below the fold became unreachable
     * because nothing showed the panel could scroll at all. AUTO draws the
     * bar only while scrolling, which is what "no visible scrollbars" was
     * actually asking for. */
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);
}

lv_obj_t *ls_ui_readout(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = sdr_label(parent, sdr_font_mono(), LS_UI_WARN);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, text ? text : "--");
    return label;
}

/*LS-786  ls_ui_readout grows to fill the remaining space, which is correct
   inside a value row and wrong on a column panel: on the Settings screen the
   location note and the two Wi-Fi lines each absorbed the leftover height and
   pushed the rows above them apart. */
lv_obj_t *ls_ui_note(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = ls_ui_readout(parent, text);
    lv_obj_set_flex_grow(label, 0);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

void ls_ui_readout_set(lv_obj_t *readout, const char *text)
{
    if (readout) sdr_text_if_changed(readout, text ? text : "--");
}

lv_obj_t *ls_ui_controls(lv_obj_t *parent)
{
    lv_obj_t *controls = ls_ui_panel(parent, nullptr);
    lv_obj_set_width(controls, lv_pct(100));
    lv_obj_set_height(controls, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(controls, 4, 0);
    lv_obj_set_style_pad_row(controls, 4, 0);
    lv_obj_set_style_pad_column(controls, 6, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW_WRAP);
    /* LS-661: SPACE_EVENLY distributes each line independently, so a full
     * first row of four buttons and a second row holding one leaves that one
     * floating in the middle of an otherwise empty line with gaps unlike any
     * other row. CENTER keeps the gap between buttons constant everywhere and
     * centres each line as a unit, so a partial row reads as a partial row. */
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return controls;
}

lv_obj_t *ls_ui_button_group(lv_obj_t *parent)
{
    lv_obj_t *group = ls_ui_row(parent, LV_FLEX_ALIGN_START);
    lv_obj_set_width(group, lv_pct(100));
    lv_obj_set_style_min_width(group, 0, 0);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(group, 6, 0);
    return group;
}

/* LS-736: lv_tabview owns a button matrix that divides the strip into N equal
 * cells and then draws each label at its full measured text width regardless
 * of the cell.  With seven P25 tabs on the 480 px panel a cell is 68 px and
 * "TALK GROUPS" measures 120, so every label from PROGRAM rightwards painted
 * across its neighbour and none of the three was readable or reliably
 * tappable.  A smaller font is the wrong answer twice over: it is unreadable,
 * and it overlaps again as soon as a longer tab name or a narrower board
 * turns up.
 *
 * The strip is therefore kit-owned: one content-sized button per tab in a
 * wrapping row, so a button is by construction at least as wide as its own
 * text, and a line that cannot hold the next button starts a new line.  The
 * lv_tabview is still the object apps drive with lv_tabview_set_act(); only
 * its button matrix is hidden.  Selection is mirrored back from the tabview
 * VALUE_CHANGED so a swipe or a programmatic switch moves the highlight. */
#define LS_UI_TAB_HEIGHT 40

static void ls_ui_tab_bar_sync(lv_obj_t *bar, uint32_t active)
{
    if (!bar) return;
    const uint32_t count = lv_obj_get_child_cnt(bar);
    for (uint32_t i = 0; i < count; ++i)
        ls_ui_button_set_role(lv_obj_get_child(bar, (int32_t)i),
                              i == active ? LS_BTN_TOGGLE_ON
                                          : LS_BTN_TOGGLE_OFF);
}

static void ls_ui_tab_button_cb(lv_event_t *event)
{
    lv_obj_t *button = lv_event_get_target(event);
    lv_obj_t *bar = lv_obj_get_parent(button);
    lv_obj_t *tabs = static_cast<lv_obj_t *>(lv_obj_get_user_data(bar));
    if (!tabs) return;
    const uint32_t index = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(lv_obj_get_user_data(button)));
    lv_tabview_set_act(tabs, (uint16_t)index, LV_ANIM_OFF);
    ls_ui_tab_bar_sync(bar, index);
}

static void ls_ui_tab_changed_cb(lv_event_t *event)
{
    lv_obj_t *bar = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    ls_ui_tab_bar_sync(bar, lv_tabview_get_tab_act(lv_event_get_target(event)));
}

void ls_ui_screen_create(lv_obj_t *parent, const char *name,
                         bool with_tabs, ls_ui_color_role_t phase_tint,
                         ls_ui_screen_t *out)
{
    /* LS-759: opting out only in P25 left the duplicate strip in FM and all
     * other apps. Shell chrome is now the default contract, not a per-app fix. */
    (void)name;
    ls_ui_standalone_screen_create(parent, nullptr, with_tabs, phase_tint, out);
}

void ls_ui_standalone_screen_create(lv_obj_t *parent, const char *name,
                         bool with_tabs, ls_ui_color_role_t phase_tint,
                         ls_ui_screen_t *out)
{
    if (!out) return;
    *out = {};
    ls_ui_style_screen(parent);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    out->root = lv_obj_create(parent);
    lv_obj_set_size(out->root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(out->root, LV_OPA_0, 0);
    lv_obj_set_style_border_width(out->root, 0, 0);
    lv_obj_set_style_radius(out->root, 0, 0);
    lv_obj_set_style_pad_all(out->root, 0, 0);
    lv_obj_set_style_pad_row(out->root, 6, 0);
    lv_obj_set_flex_flow(out->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(out->root, LV_OBJ_FLAG_SCROLLABLE);

    /* LS-736: the shell already draws battery, link and status across the top
     * of every screen.  An app that repeats its own name and frequency under
     * that spends a whole panel of height on chrome the operator already has,
     * and on the 480 px panel that panel is what pushed the DECODE actions
     * below the fold.  A NULL name means the app has nothing to add. */
    if (name && name[0]) {
        out->header = ls_ui_panel(out->root, nullptr);
        lv_obj_set_style_border_color(out->header, ls_ui_color(phase_tint), 0);
        lv_obj_set_flex_flow(out->header, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(out->header, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        out->name = sdr_label(out->header, sdr_font_mono(),
                              ls_ui_color(phase_tint));
        lv_obj_set_style_text_letter_space(out->name, 2, 0);
        lv_label_set_text(out->name, name);
        out->readout = ls_ui_readout(out->header, "--");
        out->lamp = ls_ui_lamp(out->header, phase_tint);
    }

    if (with_tabs) {
        out->tabbar = ls_ui_panel(out->root, nullptr);
        lv_obj_set_style_pad_all(out->tabbar, 4, 0);
        lv_obj_set_style_pad_row(out->tabbar, 4, 0);
        lv_obj_set_style_pad_column(out->tabbar, 4, 0);
        lv_obj_set_flex_flow(out->tabbar, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(out->tabbar, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

        out->tabs = ls_ui_tab_strip(out->root, LV_DIR_TOP, 0);
        lv_obj_set_height(out->tabs, 0);
        lv_obj_set_flex_grow(out->tabs, 1);
        lv_obj_t *btns = lv_tabview_get_tab_btns(out->tabs);
        if (btns) {
            lv_obj_add_flag(btns, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(btns, 0);
            lv_obj_set_style_border_width(btns, 0, 0);
        }
        lv_obj_set_user_data(out->tabbar, out->tabs);
        lv_obj_add_event_cb(out->tabs, ls_ui_tab_changed_cb,
                            LV_EVENT_VALUE_CHANGED, out->tabbar);
    } else {
        out->content = ls_ui_panel(out->root, nullptr);
        lv_obj_set_height(out->content, 0);
        lv_obj_set_flex_grow(out->content, 1);
        lv_obj_add_flag(out->content, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(out->content, LV_DIR_VER);
    }

    out->controls = ls_ui_controls(out->root);
    lv_obj_add_flag(out->controls, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *ls_ui_screen_add_tab(ls_ui_screen_t *screen, const char *name)
{
    if (!screen || !screen->tabs) return nullptr;
    lv_obj_t *tab = lv_tabview_add_tab(screen->tabs, name ? name : "");
    ls_ui_style_content(tab);

    if (screen->tabbar) {
        const uint32_t index = lv_obj_get_child_cnt(screen->tabbar);
        lv_obj_t *button = ls_ui_button(screen->tabbar, name ? name : "",
                                        index == 0 ? LS_BTN_TOGGLE_ON
                                                   : LS_BTN_TOGGLE_OFF,
                                        ls_ui_tab_button_cb, nullptr, nullptr);
        if (button) {
            /* Content width with no floor beyond a touchable minimum: the
             * button is measured from its own label, so the label cannot be
             * wider than the button it sits in. */
            lv_obj_set_width(button, LV_SIZE_CONTENT);
            lv_obj_set_height(button, LS_UI_TAB_HEIGHT);
            lv_obj_set_style_min_width(button, 56, 0);
            lv_obj_set_style_pad_hor(button, 10, 0);
            lv_obj_set_style_pad_ver(button, 0, 0);
            lv_obj_set_user_data(button,
                reinterpret_cast<void *>(static_cast<uintptr_t>(index)));
        }
    }
    return tab;
}

void ls_ui_tab_split(lv_obj_t *tab, ls_ui_split_t *out)
{
    if (!out) return;
    *out = {};
    if (!tab) return;

    /* One scroller, and it is the body.  A scrollable tab page holding a
     * scrollable panel means the actions can be pushed off by either of two
     * scrollers and the operator cannot tell which one to drag (LS-736). */
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *body = lv_obj_create(tab);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_height(body, 0);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_bg_opa(body, LV_OPA_0, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_row(body, 6, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_style_width(body, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(body, 3, LV_PART_SCROLLBAR);
    /*LS-783*/
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(body, LS_UI_ACCENT, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(body, LV_OPA_60, LV_PART_SCROLLBAR);

    out->body = body;
    out->actions = ls_ui_controls(tab);
}

lv_coord_t ls_ui_free_height(lv_obj_t *parent)
{
    if (!parent) return 0;
    lv_obj_update_layout(parent);
    lv_coord_t left = lv_obj_get_content_height(parent);
    const lv_coord_t gap = lv_obj_get_style_pad_row(parent, LV_PART_MAIN);
    const uint32_t count = lv_obj_get_child_cnt(parent);
    for (uint32_t i = 0; i < count; ++i) {
        left -= lv_obj_get_height(lv_obj_get_child(parent, (int32_t)i));
        left -= gap;
    }
    return left > 0 ? left : 0;
}

lv_obj_t *ls_ui_stepper(ls_ui_value_t *row,
                        lv_event_cb_t prev_cb, void *prev_user,
                        lv_event_cb_t next_cb, void *next_user)
{
    if (!row || !row->controls) return nullptr;
    /* LS-748: measured from the two buttons rather than asking for the whole
     * row, so the pair rides on the value's own line.  At 100% it started a
     * track of its own, and CONFIG and ScanPanel between them spend fourteen
     * rows on a stepper - fourteen lines for two 64 px buttons each.
     *
     * START, not END: lv_flex.c forces the cross-axis placement to START when
     * a container is content-sized, but not the main axis, so an END-placed
     * content-width row lays its children out against a width it has not
     * measured yet - place_content() returns a negative start and the buttons
     * land outside their own group.  The group is exactly its content wide, so
     * there is nothing for END to do anyway; the row's own END placement is
     * what puts the group on the right. */
    lv_obj_t *group = ls_ui_row(row->controls, LV_FLEX_ALIGN_START);
    lv_obj_set_width(group, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(group, 0, 0);
    lv_obj_set_style_pad_column(group, 6, 0);

    static const char *const glyph[2] = { "<", ">" };
    const lv_event_cb_t callback[2] = { prev_cb, next_cb };
    void *const user[2] = { prev_user, next_user };
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *button = ls_ui_button(group, glyph[i], LS_BTN_DEFAULT,
                                        callback[i], user[i], nullptr);
        if (!button) continue;
        /* A stepper is two buttons however long the list is, so it does not
         * need - and on a 480 px row cannot afford - the screen/6 floor a
         * labelled action gets.  64 px still clears a fingertip. */
        lv_obj_set_width(button, LV_SIZE_CONTENT);
        lv_obj_set_style_min_width(button, 64, 0);
    }
    return group;
}

void ls_ui_screen_set_readout(ls_ui_screen_t *screen, const char *text)
{
    if (screen) ls_ui_readout_set(screen->readout, text);
}

void ls_ui_screen_set_lamp(ls_ui_screen_t *screen, bool on,
                           ls_ui_color_role_t role)
{
    if (screen) ls_ui_lamp_set(screen->lamp, on, role);
}

lv_obj_t *ls_ui_tab_strip(lv_obj_t *parent, lv_dir_t side, lv_coord_t size)
{
    lv_obj_t *tabs = lv_tabview_create(parent, side, size);
    lv_obj_set_width(tabs, lv_pct(100));
    sdr_style_tabview(tabs);
    return tabs;
}

lv_obj_t *ls_ui_row(lv_obj_t *parent, lv_flex_align_t justify)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, justify, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t *ls_ui_panel(lv_obj_t *parent, const char *title)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(panel, LS_UI_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, LS_UI_PANEL_BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_outline_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 2, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 4, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    if (title && title[0])
        ls_ui_section(panel, title);
    return panel;
}

lv_obj_t *ls_ui_section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *row = ls_ui_row(parent, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(row, 8, 0);
    lv_obj_set_style_pad_bottom(row, 3, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_border_color(row, LS_UI_PANEL_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);

    lv_obj_t *tick = lv_obj_create(row);
    lv_obj_set_size(tick, 3, 14);
    lv_obj_set_style_bg_color(tick, LS_UI_ACCENT, 0);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tick, 0, 0);
    lv_obj_set_style_radius(tick, 0, 0);
    lv_obj_set_style_pad_all(tick, 0, 0);
    lv_obj_clear_flag(tick, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = sdr_label(row, sdr_font_mono_sm(), LS_UI_DIM_TEXT);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_text_letter_space(label, 2, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, title ? title : "");
    return row;
}

/* LS-734: a content-sized controls object with a percentage max-width,
 * followed by ScanPanel putting a percentage-width group inside it, made LVGL
 * solve the parent from the child and the child from the parent.  On the
 * 480 px panel the intermediate widths collapsed during layout: value text ran
 * through the buttons and four-button groups wrapped or shared one coordinate.
 * The fix was two unambiguous nested lines per row - a heading container and a
 * controls container - and it held.
 *
 * LS-748: it also cost two extra objects on every value row and a whole line
 * for a single small toggle, which is what the operator rejected.  Forty-one
 * of these rows are built eagerly across the P25 tabs; on f947755 the board
 * came up with 215 B of free internal RAM.
 *
 * The row is now the controls host itself: one wrapping flex line holding the
 * name, the value and whatever the caller adds, with no wrapper per row.
 * bench/tests/test_ls_ui_alloc.cpp measures both shapes on the same LVGL
 * engine and prints the per-row saving.
 *
 * The three properties LS-734 bought are kept, and they come from the item
 * widths rather than from a container:
 *
 *  - No percentage below an LV_SIZE_CONTENT parent.  The row is a percentage
 *    of its own parent, so a full-width group inside it still resolves.
 *  - A group that asks for 100% is wider than the space left beside the value,
 *    so lv_flex.c puts it on its own track: the LS-703/LS-661 one-line group
 *    behaviour is unchanged, and it is unchanged because of the group's width,
 *    not because a wrapper forced it.
 *  - The value label is the row's only grow item, and a grow item never
 *    triggers a wrap - find_track_end() counts only its gap.  It therefore
 *    absorbs exactly what the name and the controls leave, and no control can
 *    be pushed past the right edge by a long value.  That is the overflow
 *    LS-734 was fixing, now structural rather than arranged. */
void ls_ui_value(lv_obj_t *parent, const char *name, ls_ui_value_t *out)
{
    lv_obj_t *row = ls_ui_row(parent, LV_FLEX_ALIGN_END);
    lv_obj_set_style_bg_color(row, LS_UI_PANEL, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, LS_UI_PANEL_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 2, 0);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_style_pad_row(row, 4, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);

    lv_obj_t *name_label = sdr_label(row, sdr_font_mono_sm(), LS_UI_DIM_TEXT);
    /* Measured from its own text so it cannot be clipped by a share it was
     * given, and capped at half the row so a long name cannot take the line
     * the value and the controls have to share. */
    /*LS-787  The name was LV_SIZE_CONTENT with min_width 0, so on any row
     * whose value is long - BAUD carrying "AUTO last 1200", BAND carrying
     * "VHF HI 150-162" - the grown value label took the width and flex shrank
     * the name to about one character. The text then wrapped one letter per
     * line: "B/A/U/D". It was never about the name's length; GAIN, the same
     * four characters, was fine next to a short value.
     *
     * A percentage min_width did not hold - flex shrink ignores it for a
     * SIZE_CONTENT child - so give the name a definite share of the row. That
     * also lines the values up in a column, which reads better than the
     * ragged edge it replaces. 40% fits the longest name in the tree
     * ("PREFERENCE WRITE", 16 chars) at this font with room to spare; DOT is
     * the backstop so an unexpected name can never wrap and change the row's
     * height. */
    /* No percentage width here, and in particular no percentage max_width.
     * That cap is what actually broke: it resolves against the parent's
     * width, and during layout that width is not yet settled, so the cap
     * collapsed to a few pixels and the text wrapped inside it - "B/A/U/D",
     * "FRE/QUE/NCY". It was never the name's length; GAIN, the same four
     * characters, sat next to a short value and was fine. Sized from its own
     * text with clipping instead, the label cannot wrap at all. A long name
     * now pushes the controls onto the next line, which ROW_WRAP already
     * handles, rather than shredding itself. */
    lv_obj_set_width(name_label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_letter_space(name_label, 1, 0);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(name_label, name ? name : "");

    lv_obj_t *value_label = sdr_label(row, sdr_font_mono(), LS_UI_TEXT);
    lv_obj_set_width(value_label, 0);
    lv_obj_set_style_min_width(value_label, 0, 0);
    lv_obj_set_flex_grow(value_label, 1);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(value_label, "");
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_CLIP);

    if (out) {
        out->row = row;
        out->name = name_label;
        out->value = value_label;
        out->controls = row;
    }
}

/* LS-748: the reported form was a button captioned TOGGLE beside a value
 * column reading ON or OFF - the state said twice, once in a column a setting
 * with more than two choices needs, and the control itself distinguished only
 * by colour.  Here the caption never changes, the marker carries the state,
 * and the marker is the same two mono glyphs wide either way, so pressing it
 * cannot reflow the row.  ls_ui_button_set_role() adds the outline. */
static void toggle_caption(const char *text, bool on, char *out, size_t size)
{
    if (!out || size == 0) return;
    out[0] = '\0';
    if (size < 4) return;
    out[0] = on ? '[' : ' ';
    out[1] = on ? 'X' : ' ';
    out[2] = on ? ']' : ' ';
    out[3] = '\0';
    if (text && text[0]) {
        const size_t room = size - 4;
        if (room > 1) {
            out[3] = ' ';
            strncpy(out + 4, text, room - 1);
            out[size - 1] = '\0';
        }
    }
}

lv_obj_t *ls_ui_toggle(ls_ui_value_t *row, const char *text, bool on,
                       lv_event_cb_t callback, void *user_data)
{
    if (!row || !row->controls) return nullptr;
    char caption[LS_UI_TOGGLE_TEXT_MAX];
    toggle_caption(text, on, caption, sizeof(caption));
    return ls_ui_button(row->controls, caption,
                        on ? LS_BTN_TOGGLE_ON : LS_BTN_TOGGLE_OFF,
                        callback, user_data, nullptr);
}

void ls_ui_toggle_set(lv_obj_t *toggle, bool on)
{
    if (!toggle) return;
    lv_obj_t *label = lv_obj_get_child(toggle, 0);
    const char *current = label ? lv_label_get_text(label) : nullptr;
    /* Rebuilt from the caption that is up, so the control keeps whatever the
     * caller named it and this stays allocation-free on a refresh path that
     * several apps run four times a second.  The marker is a fixed four
     * characters, so the name starts at offset 4. */
    if (current && strlen(current) >= 3) {
        char caption[LS_UI_TOGGLE_TEXT_MAX];
        toggle_caption(strlen(current) >= 4 ? current + 4 : nullptr,
                       on, caption, sizeof(caption));
        sdr_text_if_changed(label, caption);
    }
    ls_ui_button_set_role(toggle, on ? LS_BTN_TOGGLE_ON : LS_BTN_TOGGLE_OFF);
}

lv_obj_t *ls_ui_lamp(lv_obj_t *parent, ls_ui_color_role_t role)
{
    lv_obj_t *lamp = lv_led_create(parent);
    lv_obj_set_size(lamp, 14, 14);
    lv_led_set_color(lamp, ls_ui_color(role));
    lv_obj_set_style_shadow_width(lamp, 0, 0);
    lv_obj_set_style_shadow_spread(lamp, 0, 0);
    lv_led_off(lamp);
    lv_obj_set_user_data(lamp,
        reinterpret_cast<void *>(static_cast<uintptr_t>(role) + 1U));
    return lamp;
}

void ls_ui_lamp_set(lv_obj_t *lamp, bool on, ls_ui_color_role_t role)
{
    if (!lamp) return;
    /* lv_led_set_color() invalidates even when the colour is unchanged.  Lamp
     * refreshes run four times a second on several apps, so keep the last role
     * in this kit-owned object's user-data slot and avoid a needless redraw. */
    void *marker = reinterpret_cast<void *>(static_cast<uintptr_t>(role) + 1U);
    if (lv_obj_get_user_data(lamp) != marker) {
        lv_led_set_color(lamp, ls_ui_color(role));
        lv_obj_set_user_data(lamp, marker);
    }
    if (on) lv_led_on(lamp); else lv_led_off(lamp);
}

lv_obj_t *ls_ui_button(lv_obj_t *parent, const char *text,
                       ls_ui_button_role_t role,
                       lv_event_cb_t callback, void *user_data,
                       lv_obj_t **out_label)
{
    /* Keep sdr_btn's established font and 50 px control height.  LS-460 is
     * framing and colour vocabulary, not a control-size redesign. */
    lv_obj_t *button = sdr_btn(parent, text, callback, user_data, out_label);
    /* LS-734: forcing every control to screen/6 made the button narrower than
     * sdr_btn's content-sized label plus standard padding.  LVGL does not clip
     * an unbounded child label to that forced width, so labels painted across
     * neighbours.  Keep content sizing and only establish a common minimum;
     * the full-width controls row decides where a whole button wraps. */
    lv_coord_t min_width = lv_disp_get_hor_res(lv_obj_get_disp(parent)) / 6;
    if (min_width > 0) lv_obj_set_style_min_width(button, min_width, 0);
    lv_obj_set_style_max_width(button, lv_pct(100), 0);
    ls_ui_button_set_role(button, role);
    return button;
}

lv_obj_t *ls_ui_group_button(lv_obj_t *group, const char *text,
                             ls_ui_button_role_t role,
                             lv_event_cb_t callback, void *user_data,
                             lv_obj_t **out_label)
{
    lv_obj_t *button = ls_ui_button(group, text, role, callback, user_data,
                                    out_label);
    if (!button) return nullptr;
    /* LS-703: screen/6 widths inside a capped settings row caused the fourth
     * SCAN nudge to wrap by itself. A named group owns one line; flex divides
    * its measured content width equally and cannot create a surprise row. */
    lv_obj_set_width(button, 0);
    lv_obj_set_style_min_width(button, 0, 0);
    lv_obj_set_flex_grow(button, 1);
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (label) {
        /* A flex item has a concrete width after its group is measured.  Bind
         * the label to that content box so even translated/long runtime text
         * cannot escape into the adjacent button. */
        lv_obj_set_width(label, lv_pct(100));
        lv_obj_set_style_min_width(label, 0, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    }
    return button;
}

void ls_ui_button_set_role(lv_obj_t *button, ls_ui_button_role_t role)
{
    if (!button) return;
    ls_ui_color_role_t bg = LS_UI_COLOR_PANEL;
    ls_ui_color_role_t fg = LS_UI_COLOR_TEXT;
    switch (role) {
        case LS_BTN_PRIMARY:
        case LS_BTN_TOGGLE_ON: bg = LS_UI_COLOR_ACCENT; fg = LS_UI_COLOR_BACKGROUND; break;
        case LS_BTN_DANGER:    bg = LS_UI_COLOR_ALARM;  fg = LS_UI_COLOR_BACKGROUND; break;
        case LS_BTN_TOGGLE_OFF:bg = LS_UI_COLOR_PANEL;  fg = LS_UI_COLOR_DIM_TEXT; break;
        default: break;
    }
    lv_obj_set_style_bg_color(button, ls_ui_color(bg), 0);
    lv_obj_set_style_border_color(button,
        ls_ui_color(role == LS_BTN_DANGER ? LS_UI_COLOR_ALARM : LS_UI_COLOR_PANEL_BORDER), 0);

    /* LS-748: selected state was accent fill against panel fill and nothing
     * else, so on the flashed board the operator could not tell a selected tab
     * or an engaged toggle from an ordinary one at a glance.  An outline is
     * drawn outside the object's box and is not part of its size, so the cue
     * costs no width: a content-sized tab button measures the same selected as
     * unselected and the strip does not re-wrap when the tab changes. */
    const bool selected = (role == LS_BTN_TOGGLE_ON);
    lv_obj_set_style_outline_color(button, ls_ui_color(LS_UI_COLOR_ACCENT), 0);
    lv_obj_set_style_outline_pad(button, 0, 0);
    lv_obj_set_style_outline_opa(button, selected ? LV_OPA_COVER : LV_OPA_0, 0);
    lv_obj_set_style_outline_width(button, selected ? 2 : 0, 0);

    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (label) lv_obj_set_style_text_color(label, ls_ui_color(fg), 0);
}

/* LS-1011: the caption the button is currently showing, as tenths of a
 * second.  UINT32_MAX means the resting text is up, not a countdown - the old
 * -1 sentinel came with an int that also had to hold a rounded-up second. */
static const uint32_t HOLD_CAPTION_IDLE = UINT32_MAX;

typedef struct {
    lv_obj_t *label;
    char text[20];
    uint32_t shown_tenths;
    ls_ui_button_role_t role;
    ls_ui_confirm_state_t confirm;
    /* LS-1012: the line this control narrates its hold into, or nullptr.
     * Owned by the caller and outlives nothing: it is a sibling on the same
     * screen, torn down with it. */
    lv_obj_t *hint_label;
    ls_safe_hint_t hint;
    char action[LS_SAFE_HINT_ACTION_MAX];
} ls_ui_hold_button_state_t;

static void hold_button_reset(lv_obj_t *button, ls_ui_hold_button_state_t *state)
{
    state->shown_tenths = HOLD_CAPTION_IDLE;
    sdr_text_if_changed(state->label, state->text);
    ls_ui_button_set_role(button, state->role);
}

/* LS-1012: repaints the shared hold line.  Only called when the rendered text
 * would actually change, so lv_obj_is_valid() - which walks the display's
 * object tree - runs on a tenth boundary rather than on every PRESSING.  The
 * check is there because the label belongs to the screen, not to this button,
 * and nothing forbids a caller from deleting it first. */
static void hold_button_paint_hint(ls_ui_hold_button_state_t *state)
{
    if (!state->hint_label || !lv_obj_is_valid(state->hint_label)) return;
    char text[LS_SAFE_HINT_TEXT_MAX];
    ls_safe_hint_format(&state->hint, text, sizeof(text));
    sdr_text_if_changed(state->hint_label, text);
    lv_obj_set_style_text_color(state->hint_label,
                                ls_ui_color(ls_safe_hint_color(&state->hint)), 0);
}

static void hold_button_event(lv_event_t *event)
{
    ls_ui_hold_button_state_t *state = static_cast<ls_ui_hold_button_state_t *>(
        lv_event_get_user_data(event));
    lv_obj_t *button = lv_event_get_target(event);
    if (!state) return;

    ls_ui_confirm_effect_t effect = LS_UI_CONFIRM_EFFECT_NONE;
    const uint32_t now = lv_tick_get();
    switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED:
        effect = ls_ui_confirm_step(&state->confirm, LS_UI_CONFIRM_PRESS, now);
        break;
    case LV_EVENT_PRESSING:
        effect = ls_ui_confirm_step(&state->confirm, LS_UI_CONFIRM_UPDATE, now);
        break;
    case LV_EVENT_RELEASED:
        effect = ls_ui_confirm_step(&state->confirm, LS_UI_CONFIRM_RELEASE, now);
        break;
    case LV_EVENT_PRESS_LOST:
        effect = ls_ui_confirm_step(&state->confirm, LS_UI_CONFIRM_CANCEL, now);
        break;
    case LV_EVENT_DELETE:
        free(state);
        return;
    default:
        return;
    }

    /* LS-1011: ARMED repaints too.  The countdown used to start only at the
     * first LV_EVENT_PRESSING, so the frame the operator sees on contact was
     * still the resting caption. */
    if (effect == LS_UI_CONFIRM_EFFECT_ARMED ||
        effect == LS_UI_CONFIRM_EFFECT_PROGRESS) {
        const uint32_t tenths = ls_ui_confirm_remaining_tenths(&state->confirm, now);
        if (tenths != state->shown_tenths) {
            state->shown_tenths = tenths;
            char label[LS_UI_CONFIRM_CAPTION_MAX];
            ls_ui_confirm_caption(tenths, label, sizeof(label));
            sdr_text_if_changed(state->label, label);
        }
    } else if (effect == LS_UI_CONFIRM_EFFECT_FIRE) {
        sdr_text_if_changed(state->label, "DONE");
        lv_event_send(button, LV_EVENT_READY, nullptr);
    } else if (effect == LS_UI_CONFIRM_EFFECT_RESET) {
        hold_button_reset(button, state);
    }

    if (state->hint_label &&
        ls_safe_hint_update(&state->hint, effect, state->action,
                            ls_ui_confirm_remaining_ms(&state->confirm, now)))
        hold_button_paint_hint(state);
}

/* LS-1012: the affordance line, in the shared kit rather than inside the
 * screen that needed it first.  Every hold-to-confirm control has the same
 * reported failure - a tap resets the caption and reads as a dead button -
 * so the sentence that prevents it is shared too.  ls_safe_screen_hint.c
 * decides the words; this creates the label they land on and paints the
 * before-anything-is-pressed phase. */
lv_obj_t *ls_ui_hold_hint(lv_obj_t *parent, uint32_t hold_ms)
{
    if (!parent) return nullptr;
    lv_obj_t *label = sdr_label(parent, sdr_font_mono_sm(), LS_UI_DIM_TEXT);
    if (!label) return nullptr;

    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    ls_safe_hint_t hint;
    ls_safe_hint_init(&hint, hold_ms);
    char text[LS_SAFE_HINT_TEXT_MAX];
    ls_safe_hint_format(&hint, text, sizeof(text));
    lv_label_set_text(label, text);
    return label;
}

/* LS-698: destructive actions previously used ordinary click callbacks.
 * The role-aware shared variant keeps the normal button sizing/palette while
 * firing READY only after the deterministic confirmation reaches its hold. */
lv_obj_t *ls_ui_hold_button_hinted(lv_obj_t *parent, const char *text,
                                   uint32_t hold_ms, ls_ui_button_role_t role,
                                   lv_event_cb_t callback, void *user_data,
                                   lv_obj_t *hint, const char *action)
{
    ls_ui_hold_button_state_t *state = static_cast<ls_ui_hold_button_state_t *>(
        calloc(1, sizeof(ls_ui_hold_button_state_t)));
    if (!state) return nullptr;

    state->role = role;
    state->shown_tenths = HOLD_CAPTION_IDLE;
    strncpy(state->text, text ? text : "HOLD", sizeof(state->text) - 1);
    state->text[sizeof(state->text) - 1] = '\0';
    ls_ui_confirm_init(&state->confirm, hold_ms);

    /* LS-1012: the hint quotes the action back, so it wants the name of what
     * the control does ("RESTART"), not the button's own "HOLD 3". */
    state->hint_label = hint;
    if (hint) {
        ls_safe_hint_init(&state->hint, state->confirm.hold_ms);
        const char *name = (action && action[0]) ? action : state->text;
        strncpy(state->action, name, sizeof(state->action) - 1);
        state->action[sizeof(state->action) - 1] = '\0';
    }

    lv_obj_t *button = ls_ui_button(parent, state->text, role, nullptr, nullptr,
                                    &state->label);
    if (!button) {
        free(state);
        return nullptr;
    }
    lv_obj_add_event_cb(button, hold_button_event, LV_EVENT_PRESSED, state);
    lv_obj_add_event_cb(button, hold_button_event, LV_EVENT_PRESSING, state);
    lv_obj_add_event_cb(button, hold_button_event, LV_EVENT_RELEASED, state);
    lv_obj_add_event_cb(button, hold_button_event, LV_EVENT_PRESS_LOST, state);
    lv_obj_add_event_cb(button, hold_button_event, LV_EVENT_DELETE, state);
    if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_READY, user_data);
    return button;
}

lv_obj_t *ls_ui_hold_button(lv_obj_t *parent, const char *text,
                            uint32_t hold_ms, ls_ui_button_role_t role,
                            lv_event_cb_t callback, void *user_data)
{
    return ls_ui_hold_button_hinted(parent, text, hold_ms, role, callback,
                                    user_data, nullptr, nullptr);
}

void ls_ui_style_table(lv_obj_t *table)
{
    lv_obj_set_style_border_width(table, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(table, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(table, 4, LV_PART_ITEMS);
    lv_obj_set_style_border_width(table, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(table, 0, LV_PART_MAIN);
}

void ls_ui_style_scroll_panel(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, LS_UI_BACKGROUND, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, LS_UI_PANEL_BORDER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, 2, 0);
    lv_obj_set_style_pad_all(obj, 6, 0);
    lv_obj_set_style_pad_row(obj, 6, 0);
    /*LS-783  A scroll panel must advertise that it scrolls; AUTO shows the
     * bar only while it is moving. */
    lv_obj_set_style_width(obj, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(obj, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(obj, LS_UI_ACCENT, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);
}

void ls_ui_style_plot(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, LS_UI_PANEL, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, LS_UI_PANEL_BORDER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, 2, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

void ls_ui_style_circle(lv_obj_t *obj, ls_ui_color_role_t role, bool outline)
{
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    if (outline) {
        lv_obj_set_style_border_color(obj, ls_ui_color(role), 0);
        lv_obj_set_style_border_opa(obj, LV_OPA_60, 0);
        lv_obj_set_style_border_width(obj, 1, 0);
    } else {
        lv_obj_set_style_bg_color(obj, ls_ui_color(role), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    }
}

void ls_ui_style_overlay(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, LS_UI_PANEL, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(obj, 4, 0);
    lv_obj_set_style_pad_ver(obj, 2, 0);
    lv_obj_set_style_radius(obj, 2, 0);
}

void ls_ui_style_card(lv_obj_t *obj, bool newest)
{
    lv_obj_set_style_bg_color(obj, LS_UI_PANEL, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, newest ? LS_UI_ACCENT : LS_UI_PANEL_BORDER, 0);
    lv_obj_set_style_border_width(obj, newest ? 2 : 1, 0);
    lv_obj_set_style_radius(obj, 2, 0);
    lv_obj_set_style_pad_all(obj, 6, 0);
    lv_obj_set_style_pad_row(obj, 2, 0);
}

void ls_ui_style_lcd_row(lv_obj_t *obj)
{
    lv_obj_set_style_bg_opa(obj, LV_OPA_0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_pad_column(obj, 6, 0);
}

void ls_ui_frame_color(lv_obj_t *obj, lv_color_t color)
{
    if (obj) lv_obj_set_style_border_color(obj, color, 0);
}
