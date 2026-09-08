#include "ui/ls_safe_screen.h"

#include "sdr_ui/sdr_ui.h"

#include <string.h>

/* LS-994  See the header for why this is shared rather than living in main/.
   Nothing here knows what a reset reason is; it renders text and two
   hold-to-confirm actions inside the standard frame. */

namespace {

/* LS-1010  One line above the buttons, carrying the hold rule before the
   first press and the countdown during one. The text itself is decided by
   ls_safe_screen_hint.c so the host gate can pin it.

   LS-1012  The wiring that drives it is no longer here. This screen used to
   run a SECOND ls_ui_confirm_state_t per button, stepped from its own press
   handlers, purely to watch a hold it could not otherwise see - and the
   destructive Settings controls, which have the identical failure, had
   nothing. Both now come from the shared hold button, which already owns a
   confirmation and can narrate its own.

   One safe screen exists at a time, so the action callbacks live in a
   file-static array rather than a heap struct that would have to be freed
   from an LVGL delete hook. It is reinitialised by ls_safe_screen_create(). */
struct action_slot_t {
    void (*fn)(void);
};

action_slot_t s_slots[2];

void fire_cb(lv_event_t *e)
{
    auto *slot = static_cast<action_slot_t *>(lv_event_get_user_data(e));
    if (slot && slot->fn) slot->fn();
}

lv_obj_t *action(lv_obj_t *group, const char *text, uint32_t hold_ms,
                 ls_ui_button_role_t role, action_slot_t *slot, lv_obj_t *hint)
{
    lv_obj_t *b = ls_ui_hold_button_hinted(group, text, hold_ms, role, fire_cb,
                                           slot, hint, text);
    if (!b) return nullptr;

    /* Same reasoning as ls_ui_group_button (LS-703): a fixed screen/6 width
       inside a row makes the second action wrap onto a line of its own on a
       narrow panel. Let flex divide the row. */
    lv_obj_set_width(b, 0);
    lv_obj_set_flex_grow(b, 1);
    return b;
}

}  // namespace

lv_obj_t *ls_safe_screen_create(lv_obj_t *parent, const ls_safe_screen_cfg_t *cfg)
{
    if (!parent || !cfg) return nullptr;

    memset(s_slots, 0, sizeof(s_slots));

    lv_obj_set_style_pad_all(parent, 8, 0);

    ls_ui_screen_t screen;
    ls_ui_standalone_screen_create(parent, cfg->title ? cfg->title : "SAFE MODE",
                        false, LS_UI_COLOR_ALARM, &screen);
    ls_ui_screen_set_readout(&screen, "RECOVERY");
    ls_ui_screen_set_lamp(&screen, true, LS_UI_COLOR_ALARM);

    if (cfg->headline && cfg->headline[0]) {
        lv_obj_t *hp = ls_ui_panel(screen.content, nullptr);
        lv_obj_t *hl = sdr_label(hp, sdr_font_mono(), LS_UI_ALARM);
        lv_obj_set_width(hl, lv_pct(100));
        lv_label_set_text(hl, cfg->headline);
    }

    if (cfg->body && cfg->body[0]) {
        lv_obj_t *bp = ls_ui_panel(screen.content, "REPORT");
        lv_obj_t *bl = sdr_label(bp, sdr_font_mono_sm(), LS_UI_TEXT);
        lv_obj_set_width(bl, lv_pct(100));
        lv_label_set_text(bl, cfg->body);
    }

    if (cfg->footer && cfg->footer[0]) {
        lv_obj_t *fp = ls_ui_panel(screen.content, nullptr);
        lv_obj_t *fl = sdr_label(fp, sdr_font_mono_sm(), LS_UI_DIM_TEXT);
        lv_obj_set_width(fl, lv_pct(100));
        lv_label_set_text(fl, cfg->footer);
    }

    const bool any_action = (cfg->primary && cfg->primary[0]) ||
                            (cfg->secondary && cfg->secondary[0]);
    if (any_action) {
        const uint32_t hold = cfg->hold_ms ? cfg->hold_ms : LS_SAFE_SCREEN_HOLD_MS;
        lv_obj_clear_flag(screen.controls, LV_OBJ_FLAG_HIDDEN);

        /* LS-1010  Before the button group, so it is the first thing read in
           the action area rather than the last line of the scrollable report.
           screen.controls is ROW_WRAP: a full-width child takes a line of its
           own and the buttons wrap onto the next. */
        lv_obj_t *hint = ls_ui_hold_hint(screen.controls, hold);

        lv_obj_t *group = ls_ui_button_group(screen.controls);
        if (cfg->primary && cfg->primary[0]) {
            s_slots[0].fn = cfg->on_primary;
            action(group, cfg->primary, hold, LS_BTN_PRIMARY, &s_slots[0], hint);
        }
        if (cfg->secondary && cfg->secondary[0]) {
            s_slots[1].fn = cfg->on_secondary;
            action(group, cfg->secondary, hold, LS_BTN_DEFAULT, &s_slots[1], hint);
        }
    }

    return screen.root;
}
