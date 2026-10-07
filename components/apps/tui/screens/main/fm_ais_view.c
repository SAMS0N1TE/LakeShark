#include "fm_ais_view.h"
#include "apps/fm/ais_store.h"
#include "ls_gps.h"
#include "../../ls_motion.h"
#include "../../ls_geo.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
static EXT_RAM_BSS_ATTR ais_vessel_t opened, vessels[AIS_VESSELS];
static EXT_RAM_BSS_ATTR ls_gps_state_t gps;
static EXT_RAM_BSS_ATTR int rows[AIS_VESSELS];
static int selected, row_count, start;
static bool detail;
static tui_rect list, buttons;
static const ls_btn_t controls[] = {
    { .label = "UP", .key = 'u' }, { .label = "OPEN / BACK", .key = 'a' },
    { .label = "DOWN", .key = 'd' }
};
static int option_get(const ls_opt_t *o)
{
    ais_options_t v; ais_options_get(&v); return o->arg ? v.map : v.names;
}
static void option_set(const ls_opt_t *o, int value)
{
    ais_options_t v; ais_options_get(&v);
    if (o->arg) v.map = value; else v.names = value;
    ais_options_set(&v);
}
static double number_get(const ls_opt_t *o)
{
    ais_options_t v; ais_options_get(&v); return o->arg ? v.max_km : v.keep_minutes;
}
static void number_set(const ls_opt_t *o, double value)
{
    ais_options_t v; ais_options_get(&v);
    if (o->arg) v.max_km = value; else v.keep_minutes = value;
    ais_options_set(&v);
}
static void number_show(const ls_opt_t *o, char *out, size_t n)
{
    double value = number_get(o);
    if (!o->arg) snprintf(out, n, "%.0f min", value);
    else if (!value) snprintf(out, n, "all");
    else snprintf(out, n, "%.0f km", value);
}
static const ls_opt_t tracking_rows[] = {
    { .label = "KEEP VESSELS", .kind = LS_OPT_LEVEL, .arg = 0, .num = number_get, .set_num = number_set,
      .lo = 1, .hi = 1440, .step = 5, .unit = "minutes", .show = number_show },
    { .label = "MAX DISTANCE", .kind = LS_OPT_LEVEL, .arg = 1, .num = number_get, .set_num = number_set,
      .lo = 0, .hi = 20000, .step = 10, .unit = "km / 0 shows all / fresh GPS", .show = number_show }
};
static const ls_opt_t display_rows[] = {
    { .label = "SHOW NAMES", .kind = LS_OPT_TOGGLE, .arg = 0, .get = option_get, .set = option_set },
    { .label = "SHOW ON MAP", .kind = LS_OPT_TOGGLE, .arg = 1, .get = option_get, .set = option_set }
};
static const ls_opt_ctx_t tracking = { .name = "VESSELS", .job = LS_RSEL_FM, LS_OPT_ROWS(tracking_rows) };
static const ls_opt_ctx_t display = { .name = "DISPLAY", .job = LS_RSEL_FM, LS_OPT_ROWS(display_rows) };
static const ls_opt_t options_rows[] = {
    { .label = "VESSELS", .kind = LS_OPT_MENU, .sub = &tracking },
    { .label = "DISPLAY", .kind = LS_OPT_MENU, .sub = &display }
};
const ls_opt_ctx_t fm_ais_options = { .name = "AIS", .job = LS_RSEL_FM, LS_OPT_ROWS(options_rows) };
static void move(int step)
{
    if (detail && step) for (int i = 0; i < row_count; ++i)
        if (vessels[rows[i]].mmsi == opened.mmsi) { selected = i; break; }
    selected += step;
    if (selected >= row_count) selected = row_count-1;
    if (selected < 0) selected = 0;
    if (detail && step && row_count) opened = vessels[rows[selected]];
}
static void line(tui_surface *sf, tui_rect a, int row, const char *s, uint8_t attr)
{ if (row < a.h-1) ls_safe_line(sf, tui_rect_make(a.x+2, a.y, a.w-4, a.h-1), a.y+row, s, attr); }
static bool visible(const ais_vessel_t *p, const ais_options_t *o, int64_t now, double *km, double *brg)
{ return ais_visible(p, o, now, gps.fix, gps.lat_deg, gps.lon_deg, gps.last_fix_us, km, brg); }
static const char *type_name(unsigned type)
{
    if (type >= 70 && type <= 79) return "CARGO";
    if (type >= 80 && type <= 89) return "TANKER";
    if (type >= 60 && type <= 69) return "PASSENGER";
    if (type >= 40 && type <= 49) return "HSC";
    switch (type) {
    case 30: return "FISHING"; case 31: case 32: return "TOWING";
    case 36: return "SAILING"; case 37: return "PLEASURE";
    case 50: return "PILOT"; case 51: return "SAR"; case 52: return "TUG";
    default: return "VESSEL";
    }
}
static void motion(const ais_vessel_t *p, char *out, size_t n)
{
    char speed[16], course[16];
    if (p->sog < 1023) snprintf(speed, sizeof(speed), "%.1fkn", p->sog/10.0);
    else snprintf(speed, sizeof(speed), "--kn");
    if (p->cog < 3600) snprintf(course, sizeof(course), "%03.0fdeg", p->cog/10.0);
    else snprintf(course, sizeof(course), "---deg");
    snprintf(out, n, "%s / %s", speed, course);
}
void fm_ais_draw(tui_surface *sf, tui_rect a)
{
    int64_t now = esp_timer_get_time(); ais_options_t o; ais_options_get(&o); ls_gps_get(&gps);
    int count = ais_store_snapshot(vessels, AIS_VESSELS, now);
    row_count = 0;
    for (int i = 0; i < count; ++i) if (visible(&vessels[i], &o, now, NULL, NULL)) rows[row_count++] = i;
    move(0);
    int h = ls_btn_raised_height(a, 3);
    buttons = tui_rect_make(a.x, a.y+a.h-h, a.w, h); list = tui_rect_make(a.x, a.y, a.w, a.h-h);
    ls_btn_bar_raised_slot(sf, buttons, controls, 3, -1, LS_BTN_SLOT_WATERFALL);
    ls_panel_box(sf, list, detail ? "AIS / VESSEL" : "AIS / HEARD VESSELS", TUI_CYAN);
    int64_t last = ais_store_last_heard();
    bool live = last > 0 && now >= last && now-last < 2500000;
    static const char *const pulse[] = { "[>    ]", "[ >   ]", "[  >  ]", "[   > ]", "[    >]", "[   < ]", "[  <  ]", "[ <   ]" };
    char b[160], speed[48];
    snprintf(b, sizeof(b), "%s %s / 87B+88B RX", live ? pulse[ls_motion_phase(8, 1000)] : "[  .  ]",
             live ? "PACKET HEARD" : "LISTENING");
    line(sf, list, 2, b, live ? TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
    ls_motion_busy(sf, list, live);
    if (detail) {
        bool found = false;
        for (int i = 0; i < count; ++i) if (vessels[i].mmsi == opened.mmsi) { opened = vessels[i]; found = true; break; }
        if (!found) { line(sf, list, 4, "Vessel expired / BACK returns to list", LS_ATTR_DIM); return; }
        double km, brg; visible(&opened, &o, now, &km, &brg);
        snprintf(b, sizeof(b), "%s / %09lu", opened.name[0] ? opened.name : "VESSEL", (unsigned long)opened.mmsi);
        line(sf, list, 4, b, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        snprintf(b, sizeof(b), "%s / type %u / call %s", opened.message == 4 ? "BASE STATION" : type_name(opened.ship_type),
                 opened.ship_type, opened.callsign[0] ? opened.callsign : "--");
        line(sf, list, 5, b, LS_ATTR_DIM);
        if (opened.position) snprintf(b, sizeof(b), "%.5f / %.5f / pos %llds", opened.lat, opened.lon, (long long)((now-opened.position_us)/1000000));
        else snprintf(b, sizeof(b), "Position unavailable");
        line(sf, list, 6, b, LS_ATTR_DIM);
        motion(&opened, speed, sizeof(speed));
        if (opened.heading < 360) snprintf(b, sizeof(b), "%s / HDG %03u", speed, opened.heading);
        else snprintf(b, sizeof(b), "%s / HDG ---", speed);
        line(sf, list, 7, b, LS_ATTR_DIM);
        if (km >= 0) snprintf(b, sizeof(b), "%.1f km / %03.0f %s / %llds ago", km, brg, ls_geo_compass(brg), (long long)((now-opened.heard_us)/1000000));
        else snprintf(b, sizeof(b), "Range needs fresh GPS / %llds ago", (long long)((now-opened.heard_us)/1000000));
        line(sf, list, 8, b, LS_ATTR_DIM);
        static const char *const nav[] = { "UNDER WAY / ENGINE", "AT ANCHOR", "NOT UNDER COMMAND", "RESTRICTED MANOEUVRE",
            "CONSTRAINED BY DRAUGHT", "MOORED", "AGROUND", "FISHING", "UNDER WAY / SAIL", "RESERVED", "RESERVED",
            "TOWING ASTERN", "PUSHING / TOWING", "RESERVED", "AIS SART", "UNAVAILABLE" };
        snprintf(b, sizeof(b), "NAV %s / %s / MSG %u", nav[opened.nav_status & 15], opened.channel ? "88B" : "87B", opened.message);
        line(sf, list, 9, b, LS_ATTR_DIM);
        snprintf(b, sizeof(b), "DEST %s", opened.destination[0] ? opened.destination : "--"); line(sf, list, 10, b, LS_ATTR_DIM);
        return;
    }
    int capacity = (list.h-5)/2;
    if (capacity < 1) return;
    start = selected >= capacity ? selected-capacity+1 : 0;
    for (int i = start; i < row_count && i-start < capacity; ++i) {
        const ais_vessel_t *p = &vessels[rows[i]];
        double km, brg; visible(p, &o, now, &km, &brg);
        char identity[24], distance[32];
        if (o.names && p->name[0]) snprintf(identity, sizeof(identity), "%s", p->name);
        else snprintf(identity, sizeof(identity), "%09lu", (unsigned long)p->mmsi);
        if (km >= 0) snprintf(distance, sizeof(distance), "%.0fkm %03.0f", km, brg); else snprintf(distance, sizeof(distance), "--km ---");
        motion(p, speed, sizeof(speed));
        uint8_t attr = i == selected ? TUI_ATTR(TUI_BLACK, TUI_CYAN | TUI_BRIGHT) : LS_ATTR_DIM;
        snprintf(b, sizeof(b), "%c %.20s / %s %u", i == selected ? '>' : ' ', identity, p->message == 4 ? "BASE" : type_name(p->ship_type), p->ship_type);
        line(sf, list, 4+2*(i-start), b, attr);
        snprintf(b, sizeof(b), "  %s %s %lldm", speed, distance, (long long)((now-p->heard_us)/60000000));
        line(sf, list, 5+2*(i-start), b, attr);
    }
    if (!row_count) line(sf, list, 4, "Waiting for AIS / 9600 GMSK", LS_ATTR_DIM);
}
static void open(void)
{
    if (detail) detail = false;
    else if (row_count) { opened = vessels[rows[selected]]; detail = true; }
}
bool fm_ais_key(ls_tk_t k)
{
    if (k == LS_TK_UP) { move(-1); return true; }
    if (k == LS_TK_DOWN) { move(1); return true; }
    if (k == LS_TK_ENTER) { open(); return true; }
    if (k == LS_TK_ESC && detail) { detail = false; return true; }
    return false;
}
bool fm_ais_touch(int col, int row)
{
    if (tui_rect_contains(buttons, col, row)) {
        int slot = ls_btn_hit_slot(col, row, LS_BTN_SLOT_WATERFALL);
        if (slot == 0) move(-1); else if (slot == 2) move(1); else if (slot == 1) open();
        return true;
    }
    if (!detail && tui_rect_contains(list, col, row) && row >= list.y+4) {
        int i = start+(row-list.y-4)/2;
        if (i >= 0 && i < row_count) { if (i == selected) open(); else selected = i; }
        return true;
    }
    return false;
}
