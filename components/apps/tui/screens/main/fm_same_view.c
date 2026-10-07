#include "fm_same_view.h"
#include "apps/fm/same_store.h"
#include "../../ls_notify.h"
#include "../../ls_motion.h"
#include "../../ls_tui_ui.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
extern const ls_tui_screen_t ls_scr_fm;
static EXT_RAM_BSS_ATTR same_alert_t shown, notice;
static int selected;
static bool detail;
static tui_rect list, buttons;
static const ls_btn_t controls[] = {
    { .label = "UP", .key = 'u' }, { .label = "OPEN / BACK", .key = 'a' },
    { .label = "DOWN", .key = 'd' }
};
static bool probe(ls_notice_t *out)
{
    if (!same_store_notice(&notice)) return false;
    memset(out, 0, sizeof(*out));
    snprintf(out->title, sizeof(out->title), "SAME %s", notice.test ? "TEST" : notice.event);
    snprintf(out->body, sizeof(out->body), "%s / %s", same_event_name(notice.event), notice.sender);
    out->accent = notice.test ? TUI_CYAN : TUI_RED;
    out->screen = ls_tui_screen_index_of(&ls_scr_fm);
    out->quiet = notice.test; out->haptic_only = true;
    out->notify_visible = true;
    return true;
}
void fm_same_enter(void) { ls_notify_add_probe(probe); }
static int option_get(const ls_opt_t *o)
{
    same_options_t v; same_options_get(&v);
    return o->arg == 0 ? v.only_mine : o->arg == 1 ? v.include_tests : v.log_sd;
}
static void option_set(const ls_opt_t *o, int value)
{
    same_options_t v; same_options_get(&v);
    if (o->arg == 0) v.only_mine = value;
    else if (o->arg == 1) v.include_tests = value;
    else v.log_sd = value;
    same_options_set(&v);
}
static double fips_get(const ls_opt_t *o)
{ same_options_t v; same_options_get(&v); return v.fips[o->arg]; }
static void fips_set(const ls_opt_t *o, double value)
{
    if (value < 0 || value > 99999 || value != (uint32_t)value) return;
    same_options_t v; same_options_get(&v); v.fips[o->arg] = (uint32_t)value;
    same_options_set(&v);
}
static void fips_show(const ls_opt_t *o, char *out, size_t n)
{
    unsigned f = (unsigned)fips_get(o);
    if (f) snprintf(out, n, "%05u", f); else snprintf(out, n, "unset");
}
#define FIPS_ROW(n) { .label = "LOCATION " #n, .kind = LS_OPT_NUMBER, .arg = n-1, \
    .num = fips_get, .set_num = fips_set, .lo = 0, .hi = 99999, \
    .unit = "SSCCC FIPS; 0 clears", .show = fips_show }
static const ls_opt_t locations[] = { FIPS_ROW(1), FIPS_ROW(2), FIPS_ROW(3),
    FIPS_ROW(4), FIPS_ROW(5), FIPS_ROW(6) };
static const ls_opt_ctx_t location_options = {
    .name = "MY LOCATIONS", .job = -1, LS_OPT_ROWS(locations)
};
static const ls_opt_t options[] = {
    { .label = "MY LOCATIONS", .kind = LS_OPT_MENU, .sub = &location_options },
    { .label = "ONLY MY LOCATIONS", .kind = LS_OPT_TOGGLE, .arg = 0, .get = option_get, .set = option_set },
    { .label = "INCLUDE TESTS", .kind = LS_OPT_TOGGLE, .arg = 1, .get = option_get, .set = option_set },
    { .label = "LOG TO SD", .kind = LS_OPT_TOGGLE, .arg = 2, .get = option_get, .set = option_set }
};
const ls_opt_ctx_t fm_same_options = {
    .name = "SAME", .job = LS_RSEL_FM, LS_OPT_ROWS(options)
};
static void move(int step)
{
    selected += step;
    int n = same_store_count();
    if (selected >= n) selected = n - 1;
    if (selected < 0) selected = 0;
}
static void line(tui_surface *sf, tui_rect a, int row, const char *text, uint8_t attr)
{ ls_safe_line(sf, tui_rect_make(a.x + 2, a.y, a.w - 4, a.h - 1), a.y + row, text, attr); }
void fm_same_draw(tui_surface *sf, tui_rect a)
{
    move(0);
    int h = ls_btn_raised_height(a, 3);
    buttons = tui_rect_make(a.x, a.y + a.h - h, a.w, h);
    list = tui_rect_make(a.x, a.y, a.w, a.h - h);
    ls_btn_bar_raised_slot(sf, buttons, controls, 3, -1, LS_BTN_SLOT_WATERFALL);
    ls_panel_box(sf, list, detail ? "SAME / ALERT" : "SAME / RECEIVED ALERTS", TUI_CYAN);
    if (!same_store_get(selected, &shown)) {
        line(sf, list, 2, "Listening / NOAA 162.400 - 162.550", LS_ATTR_DIM);
        line(sf, list, 4, "520.83 baud / waiting for repeated headers", LS_ATTR_DIM);
        ls_motion_busy(sf, list, true); return;
    }
    char buf[96];
    if (!detail) {
        int start = selected >= list.h - 3 ? selected - list.h + 4 : 0;
        for (int i = start; i < same_store_count() && i - start < list.h - 3; ++i) {
            if (!same_store_get(i, &shown)) break;
            snprintf(buf, sizeof(buf), "%c %s %-3s / %s%s", i == selected ? '>' : ' ',
                     shown.test ? "TEST" : "ALERT", shown.event, shown.sender, shown.ended ? " / END" : "");
            line(sf, list, 2 + i - start, buf, i == selected ? TUI_ATTR(TUI_BLACK, TUI_CYAN | TUI_BRIGHT) : LS_ATTR_DIM);
        }
        return;
    }
    uint8_t hue = shown.test || shown.ended ? TUI_CYAN : TUI_RED;
    uint8_t attr = TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK);
    static const char *const pulse[] = {"[!    ]", "[ !   ]", "[  !  ]", "[   ! ]", "[    !]", "[   ! ]", "[  !  ]", "[ !   ]"};
    snprintf(buf, sizeof(buf), "%s %s", shown.test ? "[ TEST ]" : shown.ended ? "[ END ]" :
             pulse[ls_motion_phase(8, 1200)], shown.test ? "Quiet test" : same_event_name(shown.event));
    line(sf, list, 2, buf, attr);
    if (!shown.test && !shown.ended) {
        ls_motion_busy(sf, list, true);
        int phase = ls_motion_phase(8, 1200);
        for (int x = list.x + 2; x < list.x + list.w - 2; ++x)
            tui_put_char(sf, list, x, list.y + 3, ((x + phase) & 7) < 2 ? '>' : ' ', attr);
    } else if (shown.test) line(sf, list, 3, same_event_name(shown.event), attr);
    snprintf(buf, sizeof(buf), "FROM %s / %s", shown.originator, shown.sender);
    line(sf, list, 4, buf, LS_ATTR_DIM);
    snprintf(buf, sizeof(buf), "ISSUED day %03u / %02u:%02u UTC", shown.day, shown.hour, shown.minute);
    line(sf, list, 5, buf, LS_ATTR_DIM);
    snprintf(buf, sizeof(buf), "VALID %u h %02u min / %s", shown.valid_minutes / 60,
             shown.valid_minutes % 60, shown.ended ? "ended" : "received");
    line(sf, list, 6, buf, LS_ATTR_DIM);
    line(sf, list, 7, "LOCATIONS / PSSCCC (state + county)", LS_ATTR_DIM);
    /* Pack all 31 counties into clipped rows so the short posture stays readable. */
    int row = 8, used = 0; buf[0] = 0;
    for (int i = 0; i < shown.location_count; ++i) {
        if (used + 8 >= list.w - 4 || used + 8 >= (int)sizeof(buf)) {
            line(sf, list, row++, buf, LS_ATTR_DIM); used = 0;
        }
        used += snprintf(buf + used, sizeof(buf) - used, "%06lu ", (unsigned long)shown.locations[i]);
    }
    if (used) line(sf, list, row, buf, LS_ATTR_DIM);
}
bool fm_same_key(ls_tk_t k)
{
    if (k == LS_TK_UP) { move(-1); return true; }
    if (k == LS_TK_DOWN) { move(1); return true; }
    if (k == LS_TK_ENTER) { detail = !detail; return true; }
    if (k == LS_TK_ESC && detail) { detail = false; return true; }
    return false;
}
bool fm_same_touch(int col, int row)
{
    if (tui_rect_contains(buttons, col, row)) {
        int slot = ls_btn_hit_slot(col, row, LS_BTN_SLOT_WATERFALL);
        if (slot == 0) move(-1); else if (slot == 2) move(1); else if (slot == 1) detail = !detail;
        return true;
    }
    if (!detail && tui_rect_contains(list, col, row)) {
        int start = selected >= list.h - 3 ? selected - list.h + 4 : 0;
        int i = start + row - list.y - 2;
        if (i >= 0 && i < same_store_count()) { if (i == selected) detail = true; else selected = i; }
        return true;
    }
    return false;
}
