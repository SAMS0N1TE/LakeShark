#include "ui/ls_shade.h"
#include "ui/ls_ui.h"
#include "sdr_ui/sdr_ui.h"
#include "ls_board.h"
#include "link_ctl.h"  /*LS-785*/
#include "esp_heap_caps.h"  /*LS-800*/

#include <cstdio>
#include <cstring>

/*LS-990  See ls_shade.h. The shade sits above content but below the toast so
   the capture confirmation, which lands after the shade has hidden itself,
   still appears on top. */

static ls_shade_hooks_t s_hooks;

static lv_obj_t *s_scrim         = nullptr;
static lv_obj_t *s_shade         = nullptr;
static lv_obj_t *s_toast         = nullptr;
static lv_obj_t *s_toast_label   = nullptr;
static lv_obj_t *s_wifi_label    = nullptr;
/*LS-785*/
static lv_obj_t *s_bt_label      = nullptr;
/*LS-800*/
static lv_obj_t *s_mem_label     = nullptr;
static void toast_show(const char *text);
static lv_timer_t *s_toast_timer = nullptr;

static void refresh_wifi_button(void)
{
    if (!s_wifi_label) return;
    const bool on = s_hooks.wifi_running && s_hooks.wifi_running();
    lv_label_set_text(s_wifi_label, on ? "WIFI: ON  (tap to stop)"
                                       : "WIFI: OFF  (tap to start)");
}

/*LS-785  The head link retries forever when the head refuses it, and there is
   no console on a handheld. OFF has to be one swipe away, beside WIFI. */
static void refresh_bt_button(void)
{
    if (!s_bt_label) return;
    if (!ls_link_ctl_available()) {
        lv_label_set_text(s_bt_label, "BT: UNAVAILABLE");
        return;
    }
    if (!ls_link_ctl_ble_on())
        lv_label_set_text(s_bt_label, "BT: OFF  (tap to start)");
    else if (ls_link_ctl_ble_connected())
        lv_label_set_text(s_bt_label, "BT: CONNECTED  (tap to stop)");
    else
        lv_label_set_text(s_bt_label, "BT: ON  (tap to stop)");
}

static void on_bt(lv_event_t *e)
{
    (void)e;
    if (ls_link_ctl_available()) ls_link_ctl_set_ble(!ls_link_ctl_ble_on());
    refresh_bt_button();
}

/*LS-800  The shade is what an operator reaches for when something is wrong, so
   it should say what is wrong. An app refusing to load prints "Low memory" and
   nothing else; these are the numbers the shell's admission check actually
   tests - it refuses below 1 KB free or a 256 B largest block. */
static void refresh_memory(void)
{
    if (!s_mem_label) return;
    unsigned internal = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    unsigned largest  = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    unsigned dma      = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    lv_label_set_text_fmt(s_mem_label,
                          "FREE %u B   LARGEST %u B   DMA %u B", internal, largest, dma);
}

static void on_usb_reset(lv_event_t *e)
{
    (void)e;
    if (!ls_link_ctl_can_reset_sdr()) { toast_show("USB reset unavailable"); return; }
    ls_link_ctl_reset_sdr();
    toast_show("USB receiver reset requested");
}

/*LS-800  Hold, not tap: this drops the receiver and any capture in progress.
   Same affordance as the recovery screen (LS-1010/LS-1012). */
static void on_reboot(lv_event_t *e)
{
    (void)e;
    if (!ls_link_ctl_can_reboot()) { toast_show("Reboot unavailable"); return; }
    toast_show("Rebooting");
    ls_link_ctl_reboot();
}

static void toast_dismiss(lv_timer_t *t)
{
    (void)t;
    if (s_toast) lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_timer = nullptr;
}

static void toast_show(const char *text)
{
    if (!s_toast || !s_toast_label) return;
    lv_label_set_text(s_toast_label, text ? text : "");
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);

    if (s_toast_timer) lv_timer_del(s_toast_timer);
    s_toast_timer = lv_timer_create(toast_dismiss, 3500, nullptr);
    if (s_toast_timer) lv_timer_set_repeat_count(s_toast_timer, 1);
}

static const char *basename_of(const char *path)
{
    const char *base = path ? path : "";
    for (const char *p = base; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

static void on_screenshot(lv_event_t *e)
{
    (void)e;
    /* Close first so nothing here lands in the image. lv_snapshot_take walks
       the object tree and skips LV_OBJ_FLAG_HIDDEN, so hiding the shade
       before the capture keeps it out. The toast is added after the capture
       returns, so it cannot appear in the image either. */
    ls_shade_close();

    if (!s_hooks.capture) { toast_show("screenshot: not wired"); return; }

    char path[128] = "";
    const bool ok = s_hooks.capture(path, sizeof(path));
    if (!ok) { toast_show("screenshot failed - see log"); return; }

    char hint[64] = "";
    if (s_hooks.fetch_hint) s_hooks.fetch_hint(hint, sizeof(hint));

    char msg[240];
    if (hint[0])
        snprintf(msg, sizeof(msg), "shot: %s\nfetch: %s",
                 basename_of(path), hint);
    else
        snprintf(msg, sizeof(msg), "shot: %s\nfetch: turn WiFi on",
                 basename_of(path));
    toast_show(msg);
}

static void on_wifi(lv_event_t *e)
{
    (void)e;
    if (s_hooks.wifi_toggle) s_hooks.wifi_toggle();
    refresh_wifi_button();
}

static void on_home(lv_event_t *e)
{
    (void)e;
    ls_shade_close();
    if (s_hooks.go_home) s_hooks.go_home();
}

static void on_scrim_click(lv_event_t *e)
{
    (void)e;
    ls_shade_close();
}

void ls_shade_configure(const ls_shade_hooks_t *hooks)
{
    if (hooks) s_hooks = *hooks;
    else       memset(&s_hooks, 0, sizeof(s_hooks));
    refresh_wifi_button();
}

static lv_obj_t *shade_button(lv_obj_t *parent, const char *text,
                              lv_event_cb_t cb, lv_obj_t **out_label)
{
    lv_obj_t *b = ls_ui_button(parent, text, LS_BTN_DEFAULT, cb, nullptr, out_label);
    lv_obj_set_width(b, lv_pct(100));
    return b;
}

void ls_shade_build(lv_obj_t *parent)
{
    if (!parent || s_shade) return;

    const int hor = lv_disp_get_hor_res(NULL);
    const int ver = lv_disp_get_ver_res(NULL);

    /* Scrim behind the shade. Clicking anywhere off the shade dismisses it -
       tap-away close, which is what an operator reaches for after a swipe. */
    s_scrim = lv_obj_create(parent);
    lv_obj_set_size(s_scrim, hor, ver);
    lv_obj_set_pos(s_scrim, 0, 0);
    lv_obj_set_style_bg_color(s_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_scrim, 0, 0);
    lv_obj_set_style_radius(s_scrim, 0, 0);
    lv_obj_set_style_pad_all(s_scrim, 0, 0);
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_scrim, on_scrim_click, LV_EVENT_CLICKED, nullptr);

    /* The shade itself is a panel from the kit, docked to the top and sized
       to the display. Screen dimensions never a literal - LS-905. */
    s_shade = ls_ui_panel(parent, "QUICK");
    lv_obj_set_width(s_shade, hor);
    lv_obj_set_pos(s_shade, 0, 0);
    lv_obj_set_style_radius(s_shade, 0, 0);
    lv_obj_set_style_pad_row(s_shade, 8, 0);
    lv_obj_add_flag(s_shade, LV_OBJ_FLAG_HIDDEN);

    /* Board identity - so two boards on a bench are distinguishable without
       unplugging one. Framed row, per the rule that nothing sits on the
       background. */
    ls_ui_value_t bd = {};
    ls_ui_value(s_shade, "BOARD", &bd);
    if (bd.value) lv_label_set_text(bd.value, LS_BOARD_NAME);

    shade_button(s_shade, "SCREENSHOT", on_screenshot, nullptr);
    shade_button(s_shade, "WIFI",       on_wifi,       &s_wifi_label);
    /*LS-785*/
    shade_button(s_shade, "BT",         on_bt,         &s_bt_label);
    /*LS-800*/
    shade_button(s_shade, "USB RESET",  on_usb_reset,  nullptr);
    shade_button(s_shade, "HOME",       on_home,       nullptr);

    /*LS-800  Reboot is destructive enough to need a hold, and the hint line
       states the rule before anything is pressed. */
    ls_ui_hold_hint(s_shade, 1200);
    lv_obj_t *rb = ls_ui_hold_button(s_shade, "REBOOT", 1200, LS_BTN_DANGER,
                                     on_reboot, nullptr);
    if (rb) lv_obj_set_width(rb, lv_pct(100));

    /*LS-800  Live memory, because "Low memory" on its own is not actionable. */
    s_mem_label = ls_ui_note(s_shade, "FREE --");

    refresh_wifi_button();
    /*LS-785*/
    refresh_bt_button();

    /* Confirmation toast. Same panel kit as everything else. Lives at the
       bottom above the rail so it does not obscure whatever the operator
       was doing when they took the shot. Kept hidden until show. */
    s_toast = ls_ui_panel(parent, nullptr);
    lv_obj_set_width(s_toast, hor - 24);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -(SDR_RAIL_H + 12));
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    s_toast_label = sdr_label(s_toast, sdr_font_mono_sm(), SDR_TEXT);
    lv_label_set_long_mode(s_toast_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_toast_label, lv_pct(100));
}

void ls_shade_open(void)
{
    if (!s_shade || !s_scrim) return;
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_scrim);
    lv_obj_clear_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_shade);
    refresh_wifi_button();
    /*LS-785*/
    refresh_bt_button();
    /*LS-800*/
    refresh_memory();
}

void ls_shade_close(void)
{
    if (s_scrim) lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    if (s_shade) lv_obj_add_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
}

bool ls_shade_is_open(void)
{
    return s_shade && !lv_obj_has_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
}
