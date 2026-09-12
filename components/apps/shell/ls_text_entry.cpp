#include "shell/ls_text_entry.h"
#include "shell/ls_input.h"

#include "ls_board.h"
#include "sdr_ui/sdr_ui.h"
#include "ui/ls_ui.h"

struct ls_text_entry {
    lv_obj_t *modal;
    lv_obj_t *textarea;
    ls_text_entry_done_cb_t done;
    void *user_data;
    bool active;
};

static ls_text_entry s_entry = {};

static void finish_entry(bool accepted, bool notify)
{
    if (!s_entry.active) return;

    lv_obj_t *modal = s_entry.modal;
    const char *text = lv_textarea_get_text(s_entry.textarea);
    ls_text_entry_done_cb_t done = s_entry.done;
    void *user_data = s_entry.user_data;

    /* Keep the LVGL-owned text alive through the callback, but mark the entry
       inactive so a callback cannot finish it twice. */
    s_entry.active = false;
    if (notify && done) done(accepted, text, user_data);
    lv_obj_del(modal);
    s_entry = {};
    ls_input_focus_screen();
}

static void entry_event(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY) finish_entry(true, true);
    else if (code == LV_EVENT_CANCEL) finish_entry(false, true);
}

ls_text_entry_t *ls_text_entry_open(const ls_text_entry_config_t *config,
                                    ls_text_entry_done_cb_t done,
                                    void *user_data)
{
    if (!config || s_entry.modal) return nullptr;

    lv_obj_t *bg = lv_obj_create(lv_layer_top());
    if (!bg) return nullptr;
    lv_obj_add_event_cb(bg,entry_event,LV_EVENT_CANCEL,nullptr);
    lv_obj_set_size(bg, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_80, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 10, 0);
    int safe_x=0,safe_y=0;
    ls_ui_get_safe_insets(&safe_x,&safe_y);
    lv_obj_set_style_pad_hor(bg,10+safe_x,0);
    lv_obj_set_style_pad_ver(bg,10+safe_y,0);
    lv_obj_set_style_pad_row(bg, 8, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = sdr_label(bg, &lv_font_montserrat_16, SDR_CYAN);
    lv_label_set_text(title, config->title ? config->title : "ENTER TEXT");

    lv_obj_t *ta = lv_textarea_create(bg);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, config->password);
    if (config->password) lv_textarea_set_password_show_time(ta, 0);
    if (config->accepted_chars)
        lv_textarea_set_accepted_chars(ta, config->accepted_chars);
    if (config->max_length) lv_textarea_set_max_length(ta, config->max_length);
    if (config->placeholder)
        lv_textarea_set_placeholder_text(ta, config->placeholder);
    lv_textarea_set_text(ta, config->text ? config->text : "");
    lv_obj_set_width(ta, config->width);

    /* small entries inherited the light default theme. Size selects
       typography only; every shared text/password/numeric entry uses our palette. */
    {
        lv_obj_set_style_text_font(ta, config->large ? &lv_font_montserrat_32 : sdr_font_mono(), 0);
        lv_obj_set_style_bg_color(ta, SDR_PANEL, 0);
        lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(ta, SDR_TEXT, 0);
        lv_obj_set_style_border_color(ta, sdr_accent(), 0);
        lv_obj_set_style_border_width(ta, 2, 0);
        lv_obj_set_style_radius(ta, 4, 0);
        lv_obj_set_style_pad_all(ta, 10, 0);
        lv_obj_set_style_text_color(ta, SDR_DIM, LV_PART_TEXTAREA_PLACEHOLDER);
    }

    lv_obj_add_event_cb(ta, entry_event, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(ta, entry_event, LV_EVENT_CANCEL, nullptr);

#if !LS_HAS_KEYBOARD || LS_HAS_COMPACT_UI
    lv_obj_t *kb = lv_keyboard_create(bg);
    lv_keyboard_set_mode(kb, config->mode == LS_TEXT_ENTRY_NUMBER
                             ? LV_KEYBOARD_MODE_NUMBER
                             : LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_set_width(kb, lv_pct(100));
    lv_obj_set_flex_grow(kb, 1);
    {
        lv_obj_set_style_bg_color(kb, SDR_BG, 0);
        lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(kb, 0, 0);
        lv_obj_set_style_pad_all(kb, 6, 0);
        lv_obj_set_style_text_font(kb, config->large ? &lv_font_montserrat_24 : sdr_font_mono(), 0);
        lv_obj_set_style_text_font(kb, config->large ? &lv_font_montserrat_24 : sdr_font_mono(), LV_PART_ITEMS);
        lv_obj_set_style_bg_color(kb, SDR_PANEL, LV_PART_ITEMS);
        lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
        lv_obj_set_style_text_color(kb, SDR_TEXT, LV_PART_ITEMS);
        lv_obj_set_style_border_color(kb, SDR_DIM, LV_PART_ITEMS);
        lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
        lv_obj_set_style_radius(kb, 4, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(kb, sdr_accent_bg(),
            (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_text_color(kb, SDR_TEXT,
            (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(kb, SDR_PANEL,
            (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(kb, SDR_TEXT,
            (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(kb, sdr_accent_bg(),
            (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_PRESSED);
    }
#endif

    s_entry = {bg, ta, done, user_data, true};
    ls_input_focus_modal(bg);
#if LS_HAS_KEYBOARD
    lv_group_focus_obj(ta);
#endif
    return &s_entry;
}

void ls_text_entry_close(ls_text_entry_t *entry)
{
    if (entry == &s_entry && s_entry.active) finish_entry(false, false);
}
