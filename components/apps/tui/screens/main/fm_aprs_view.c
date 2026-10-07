#include "fm_aprs_view.h"
#include "apps/fm/aprs_store.h"
#include "apps/fm/fm_state.h"
#include "lakeshark_backend.h"
#include "ls_gps.h"
#include "../../ls_motion.h"
#include "../../ls_geo.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
static EXT_RAM_BSS_ATTR aprs_packet_t shown, opened;
static EXT_RAM_BSS_ATTR aprs_packet_t stations[APRS_STATIONS];
static EXT_RAM_BSS_ATTR ls_gps_state_t gps;
static int selected, row_count, start;
static EXT_RAM_BSS_ATTR int rows[APRS_STATIONS];
static bool detail;
static tui_rect list, buttons;
static const ls_btn_t controls[] = {
    { .label = "UP", .key = 'u' }, { .label = "OPEN / BACK", .key = 'a' },
    { .label = "DOWN", .key = 'd' }
};
static int option_get(const ls_opt_t *o)
{
    aprs_options_t v; aprs_options_get(&v);
    return o->arg == 0 ? v.preset : o->arg == 1 ? v.messages : v.map;
}
static uint32_t frequency(const aprs_options_t *v)
{ return v->preset == 0 ? 144390000 : v->preset == 1 ? 144800000 : v->custom_hz; }
static void option_apply(aprs_options_t *v, bool tune)
{
    /* A frequency lock refuses the change before the preference is saved. */
    if (tune && FM.mode == FM_MODE_APRS && !lakeshark_fm_set_freq(frequency(v))) return;
    aprs_options_set(v);
}
static void option_set(const ls_opt_t *o, int value)
{
    aprs_options_t v; aprs_options_get(&v);
    if (o->arg == 0) v.preset = value;
    else if (o->arg == 1) v.messages = value;
    else v.map = value;
    option_apply(&v, o->arg == 0);
}
static double number_get(const ls_opt_t *o)
{
    aprs_options_t v; aprs_options_get(&v);
    return o->arg == 0 ? v.custom_hz/1e6 : o->arg == 1 ? v.keep_minutes : v.max_km;
}
static void number_set(const ls_opt_t *o, double value)
{
    aprs_options_t v; aprs_options_get(&v);
    if (o->arg == 0) { v.custom_hz = (uint32_t)(value*1e6+0.5); v.preset = 2; }
    else if (o->arg == 1) v.keep_minutes = value;
    else v.max_km = value;
    option_apply(&v, o->arg == 0);
}
static const char *const presets[] = { "US 144.390", "EU 144.800", "CUSTOM" };
static double preset_get(const ls_opt_t *o)
{ return option_get(o); }
static void preset_set(const ls_opt_t *o, double value)
{ if (value >= 0 && value <= 2 && value == (int)value) option_set(o, (int)value); }
static void preset_show(const ls_opt_t *o, char *out, size_t n)
{ snprintf(out, n, "%s", presets[option_get(o)]); }
static void number_show(const ls_opt_t *o, char *out, size_t n)
{
    double value = number_get(o);
    if (o->arg == 0) snprintf(out, n, "%.3f", value);
    else if (o->arg == 1) snprintf(out, n, "%.0f min", value);
    else if (value == 0) snprintf(out, n, "all");
    else snprintf(out, n, "%.0f km", value);
}
static const ls_opt_t frequency_rows[] = {
    { .label = "PRESET", .kind = LS_OPT_LEVEL, .arg = 0, .num = preset_get, .set_num = preset_set,
      .lo = 0, .hi = 2, .step = 1, .unit = "0 US / 1 EU / 2 custom", .show = preset_show },
    { .label = "CUSTOM MHz", .kind = LS_OPT_LEVEL, .arg = 0, .num = number_get, .set_num = number_set,
      .lo = 24, .hi = 1766, .step = 0.005, .unit = "MHz / NFM receive only", .show = number_show }
};
static const ls_opt_ctx_t frequency_options = { .name = "FREQUENCY", .job = LS_RSEL_FM, LS_OPT_ROWS(frequency_rows) };
static const ls_opt_t rows_options[] = {
    { .label = "FREQUENCY", .kind = LS_OPT_MENU, .sub = &frequency_options },
    { .label = "KEEP STATIONS", .kind = LS_OPT_LEVEL, .arg = 1, .num = number_get, .set_num = number_set,
      .lo = 1, .hi = 1440, .step = 5, .unit = "minutes", .show = number_show },
    { .label = "MAX DISTANCE", .kind = LS_OPT_LEVEL, .arg = 2, .num = number_get, .set_num = number_set,
      .lo = 0, .hi = 20000, .step = 10, .unit = "km / 0 shows all / fresh GPS", .show = number_show },
    { .label = "SHOW MESSAGES", .kind = LS_OPT_TOGGLE, .arg = 1, .get = option_get, .set = option_set },
    { .label = "SHOW ON MAP", .kind = LS_OPT_TOGGLE, .arg = 2, .get = option_get, .set = option_set }
};
const ls_opt_ctx_t fm_aprs_options = { .name = "APRS", .job = LS_RSEL_FM, LS_OPT_ROWS(rows_options) };
static void move(int step)
{
    if (detail && step) {
        for (int i = 0; i < row_count; ++i) {
            const aprs_packet_t *p = &stations[rows[i]];
            if (!strcmp(p->call, opened.call) && !strcmp(p->name, opened.name)) { selected = i; break; }
        }
    }
    selected += step;
    if (selected >= row_count) selected = row_count-1;
    if (selected < 0) selected = 0;
    if (detail && step && row_count) opened = stations[rows[selected]];
}
static void line(tui_surface *sf, tui_rect a, int row, const char *s, uint8_t attr)
{ if (row < a.h-1) ls_safe_line(sf, tui_rect_make(a.x+2, a.y, a.w-4, a.h-1), a.y+row, s, attr); }
static const char *kind(const aprs_packet_t *p)
{
    if (p->weather) return "WEATHER";
    static const char *const names[] = { "POSITION", "OBJECT", "ITEM", "WEATHER", "MESSAGE", "STATUS" };
    return names[p->kind];
}
static bool visible(const aprs_packet_t *p, const aprs_options_t *o, int64_t now, double *km, double *brg)
{ return aprs_visible(p, o, now, gps.fix, gps.lat_deg, gps.lon_deg, gps.last_fix_us, km, brg); }
void fm_aprs_draw(tui_surface *sf, tui_rect a)
{
    int64_t now = esp_timer_get_time(); aprs_options_t o; aprs_options_get(&o); ls_gps_get(&gps);
    int count = aprs_store_snapshot(stations, APRS_STATIONS, now);
    row_count = 0;
    for (int i = 0; i < count; ++i)
        if (visible(&stations[i], &o, now, NULL, NULL)) rows[row_count++] = i;
    move(0);
    int h = ls_btn_raised_height(a, 3);
    buttons = tui_rect_make(a.x, a.y+a.h-h, a.w, h); list = tui_rect_make(a.x, a.y, a.w, a.h-h);
    ls_btn_bar_raised_slot(sf, buttons, controls, 3, -1, LS_BTN_SLOT_WATERFALL);
    ls_panel_box(sf, list, detail ? "APRS / STATION" : "APRS / HEARD STATIONS", TUI_CYAN);
    int64_t last = aprs_store_last_heard();
    bool live = last > 0 && now >= last && now-last < 2500000;
    static const char *const pulse[] = { "[>    ]", "[ >   ]", "[  >  ]", "[   > ]", "[    >]", "[   < ]", "[  <  ]", "[ <   ]" };
    char b[192];
    snprintf(b, sizeof(b), "%s %s / 1200 baud RX", live ? pulse[ls_motion_phase(8, 1000)] : "[  .  ]",
             live ? "PACKET HEARD" : "LISTENING");
    line(sf, list, 2, b, live ? TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
    ls_motion_busy(sf, list, live);
    if (detail) {
        /* Follow the opened identity as new arrivals reorder the live table. */
        bool found = false;
        for (int i = 0; i < count; ++i) {
            if (!strcmp(stations[i].call, opened.call) && !strcmp(stations[i].name, opened.name)) {
                opened = stations[i]; found = true; break;
            }
        }
        if (!found) { line(sf, list, 4, "Station expired / BACK returns to list", LS_ATTR_DIM); return; }
        double km, brg; visible(&opened, &o, now, &km, &brg);
        snprintf(b, sizeof(b), "%c %s %s / %s", opened.symbol, opened.call, opened.name, kind(&opened));
        line(sf, list, 4, b, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        if (opened.position) snprintf(b, sizeof(b), "%s%.5f / %.5f", opened.ambiguous ? "~ " : "", opened.lat, opened.lon);
        else snprintf(b, sizeof(b), "No position in this packet");
        line(sf, list, 5, b, LS_ATTR_DIM);
        if (km >= 0) snprintf(b, sizeof(b), "%.1f km / %03.0f %s / %llds ago", km, brg, ls_geo_compass(brg), (long long)((now-opened.heard_us)/1000000));
        else snprintf(b, sizeof(b), "Range needs fresh GPS / %llds ago", (long long)((now-opened.heard_us)/1000000));
        line(sf, list, 6, b, LS_ATTR_DIM);
        if (opened.kind == APRS_MESSAGE) { snprintf(b, sizeof(b), "TO %s", opened.recipient); line(sf, list, 7, b, LS_ATTR_DIM); }
        else if (opened.weather) {
            char temp[24], wind[32], humidity[24];
            if (opened.temperature_valid) snprintf(temp, sizeof(temp), "%d F", opened.temperature_f); else snprintf(temp, sizeof(temp), "temp --");
            if (opened.wind_valid) snprintf(wind, sizeof(wind), "%03d / %d mph", opened.wind_deg, opened.wind_mph); else snprintf(wind, sizeof(wind), "wind --");
            if (opened.humidity_valid) snprintf(humidity, sizeof(humidity), "%d%% RH", opened.humidity); else snprintf(humidity, sizeof(humidity), "RH --");
            snprintf(b, sizeof(b), "%s / %s / %s", temp, wind, humidity); line(sf, list, 7, b, LS_ATTR_DIM);
        }
        int width = list.w-4;
        if (width > 0) for (int row = 8, at = 0; opened.text[at] && row < list.h-1; ++row) {
            snprintf(b, sizeof(b), "%.*s", width, opened.text+at); line(sf, list, row, b, LS_ATTR_DIM);
            at += strlen(b);
        }
        return;
    }
    int capacity = list.h-5;
    if (capacity < 1) return;
    start = selected >= capacity ? selected-capacity+1 : 0;
    for (int i = start; i < row_count && i-start < capacity; ++i) {
        shown = stations[rows[i]];
        double km, brg; visible(&shown, &o, now, &km, &brg);
        char distance[32];
        if (km >= 0) snprintf(distance, sizeof(distance), "%.0fkm %03.0f", km, brg); else snprintf(distance, sizeof(distance), "--km ---");
        snprintf(b, sizeof(b), "%c %c %-11s %s %lldm%s", i == selected ? '>' : ' ', shown.symbol,
                 shown.call, distance, (long long)((now-shown.heard_us)/60000000), shown.weather ? " WX" : "");
        line(sf, list, 4+i-start, b, i == selected ? TUI_ATTR(TUI_BLACK, TUI_CYAN | TUI_BRIGHT) : LS_ATTR_DIM);
    }
    if (!row_count) line(sf, list, 4, "Waiting for AX.25 UI packets / NFM", LS_ATTR_DIM);
}
static void open(void)
{
    if (detail) detail = false;
    else if (row_count) { opened = stations[rows[selected]]; detail = true; }
}
bool fm_aprs_key(ls_tk_t k)
{
    if (k == LS_TK_UP) { move(-1); return true; }
    if (k == LS_TK_DOWN) { move(1); return true; }
    if (k == LS_TK_ENTER) { open(); return true; }
    if (k == LS_TK_ESC && detail) { detail = false; return true; }
    return false;
}
bool fm_aprs_touch(int col, int row)
{
    if (tui_rect_contains(buttons, col, row)) {
        int slot = ls_btn_hit_slot(col, row, LS_BTN_SLOT_WATERFALL);
        if (slot == 0) move(-1); else if (slot == 2) move(1); else if (slot == 1) open();
        return true;
    }
    if (!detail && tui_rect_contains(list, col, row)) {
        int i = start+row-list.y-4;
        if (i >= 0 && i < row_count) { if (i == selected) open(); else selected = i; }
        return true;
    }
    return false;
}
