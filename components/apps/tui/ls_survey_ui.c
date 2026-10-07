#include "ls_survey_ui.h"
#include "ls_survey.h"
#include "ls_tui_ui.h"
#include "ls_options.h"
#include "ls_motion.h"
#include "ls_compass_art.h"
#include "esp_attr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define WHITE TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK)
#define CYAN TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK)
static ls_survey_options_t s_options = {true, true, true, 10, 10};
static ls_survey_view_t s_view;
static ls_survey_entry_t s_detail;
static unsigned s_pick;
static int s_sort, s_rows = 1, s_top = 3, s_pitch = 3;
static bool s_details;
static tui_rect s_list;
static EXT_RAM_BSS_ATTR ls_dot_t s_dots[160 * 120];

static const char *why(const ls_opt_t *o)
{
    (void)o;
    ls_survey_view(&s_view);
    return s_view.running || s_view.busy ? "Stop the session to change options" : NULL;
}
static int get(const ls_opt_t *o)
{
    return o->arg == 0 ? s_options.wifi : o->arg == 1 ? s_options.ble : s_options.only_fix;
}
static void set(const ls_opt_t *o, int v)
{
    if (o->arg == 0) s_options.wifi = v; else if (o->arg == 1) s_options.ble = v; else s_options.only_fix = v;
}
static double num(const ls_opt_t *o) { return o->arg == 3 ? s_options.interval_s : s_options.keep; }
static void set_num(const ls_opt_t *o, double v)
{
    if (o->arg == 3) s_options.interval_s = (unsigned)v; else s_options.keep = (unsigned)v;
}
static const ls_opt_t RADIOS[] = {
    {.label="SCAN WI-FI", .kind=LS_OPT_TOGGLE, .arg=0, .get=get, .set=set, .why_not=why},
    {.label="LISTEN BLE", .kind=LS_OPT_TOGGLE, .arg=1, .get=get, .set=set, .why_not=why},
    {.label="SCAN INTERVAL", .kind=LS_OPT_LEVEL, .arg=3, .num=num, .set_num=set_num,
     .lo=3, .hi=120, .step=1, .unit="seconds", .why_not=why},
};
static const ls_opt_t RECORDING[] = {
    {.label="ONLY WITH GPS FIX", .kind=LS_OPT_TOGGLE, .arg=2, .get=get, .set=set, .why_not=why},
    {.label="KEEP SESSIONS", .kind=LS_OPT_LEVEL, .arg=4, .num=num, .set_num=set_num,
     .lo=1, .hi=50, .step=1, .unit="sessions", .why_not=why},
};
static const ls_opt_ctx_t CTX_RADIOS = {.name="RADIOS", .job=-1, .radio=LS_RSEL_NONE, LS_OPT_ROWS(RADIOS)};
static const ls_opt_ctx_t CTX_RECORDING = {.name="RECORDING", .job=-1, .radio=LS_RSEL_NONE, LS_OPT_ROWS(RECORDING)};
static const ls_opt_t OPTIONS[] = {
    {.label="RADIOS", .kind=LS_OPT_MENU, .sub=&CTX_RADIOS},
    {.label="RECORDING", .kind=LS_OPT_MENU, .sub=&CTX_RECORDING},
};
static const ls_opt_ctx_t CTX = {.name="SURVEY", .job=-1, .radio=LS_RSEL_NONE, LS_OPT_ROWS(OPTIONS)};

static void text(tui_surface *sf, tui_rect a, int row, const char *s, uint8_t ink)
{
    ls_safe_line(sf, a, a.y + row, s, ink);
}

static void display_name(char out[33], const char *name)
{
    snprintf(out, 33, "%s", name);
    for (unsigned i = 0; out[i]; i++)
        if ((unsigned char)out[i] < 32 || (unsigned char)out[i] > 126) out[i] = '?';
}

static void sweep(tui_surface *sf, tui_rect a)
{
    ls_panel_box(sf, a, "PASSIVE / ACTIVITY", TUI_GREEN);
    tui_rect pic = tui_rect_make(a.x + 1, a.y + 2, a.w - 2, a.h - 5);
    if (pic.w < 4 || pic.h < 2 || pic.w * 2 * pic.h * 3 > 160 * 120) return;
    int cw = 10, ch = 17;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    if (cw < 1) cw = 10;
    if (ch < 1) ch = 17;
    float radius = fminf(pic.w * cw, pic.h * ch) * 0.46f;
    float phase = s_view.running ? ls_motion_phase(72, 5000) * 0.08726646f : 0;
    int w = pic.w * 2, h = pic.h * 3;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        float dx = ((x + 0.5f) * cw / 2 - pic.w * cw / 2.0f) / radius;
        float dy = (pic.h * ch / 2.0f - (y + 0.5f) * ch / 3) / radius;
        float r = sqrtf(dx * dx + dy * dy), angle = atan2f(dx, dy);
        float off = fmodf(phase - angle + 12.566371f, 6.283185f);
        float width = fmaxf(cw / 2.0f, ch / 3.0f) / radius;
        ls_dot_t *d = &s_dots[y * w + x];
        *d = (ls_dot_t){0};
        if (r > 1 + width) continue;
        if (fabsf(r - 1) < width * 0.6f) *d = (ls_dot_t){TUI_GREEN, true};
        else if ((fabsf(r - 0.33f) < width * 0.4f || fabsf(r - 0.67f) < width * 0.4f) && (x + y) % 2)
            *d = (ls_dot_t){TUI_CYAN, true};
        else if (s_view.running && r < 1 && off < 0.07f)
            *d = (ls_dot_t){TUI_GREEN | TUI_BRIGHT, true};
        else if (s_view.running && r < 1 && off < 0.65f && (x + 2 * y) % 4 == 0)
            *d = (ls_dot_t){TUI_GREEN, true};
    }
    ls_dots_blit(sf, pic, s_dots, w);
    text(sf, a, a.h - 2, "Activity animation / no bearings", LS_ATTR_DIM);
}

static void action(int i)
{
    ls_survey_view(&s_view);
    if (i == 0) { if (ls_survey_run(!s_view.running, &s_options)) { s_details = false; s_pick = 0; } }
    if (i == 1) { s_sort = (s_sort + 1) % 3; s_pick = 0; s_details = false; }
    if (i == 2) ls_opt_open(&CTX);
    if (i == 4 || i == 5) {
        unsigned count = s_view.wifi + s_view.ble;
        if (count) s_pick = (unsigned)(((int)s_pick + (i == 4 ? -s_rows : s_rows) + (int)count * s_rows) % (int)count);
        s_details = false;
    }
    if (i == 3) {
        if (s_details) s_details = false;
        else if (ls_survey_at(s_pick, s_sort, &s_detail)) s_details = true;
    }
}

void ls_survey_draw(tui_surface *sf, tui_rect a)
{
    ls_survey_view(&s_view);
    char line[160], name[33];
    const unsigned count = s_view.wifi + s_view.ble;
    if (s_pick >= count) s_pick = 0;
    ls_panel_box(sf, tui_rect_make(a.x, a.y, a.w, 5), s_view.running ? "SURVEY / WALK" : s_view.path[0] ? "SURVEY / SESSION SUMMARY" : "SURVEY / READY", TUI_CYAN);
    snprintf(line, sizeof(line), "%c Wi-Fi %u  BLE %u  New/min %u", ls_motion_pip(s_view.running), s_view.wifi, s_view.ble, s_view.new_minute);
    text(sf, a, 1, line, WHITE);
    snprintf(line, sizeof(line), "%us  %u/%u  Drop %u  GPS %s", s_view.elapsed_s, count, LS_SURVEY_CAP, s_view.dropped, s_view.gps_fresh ? "FRESH" : "WAIT");
    text(sf, a, 2, line, CYAN);
    text(sf, a, 3, s_view.status[0] ? s_view.status : "START a passive walk / OPTIONS sets recording", LS_ATTR_DIM);
    int bar_h = ls_tui_is_wide() ? 3 : 6;
    tui_rect body = tui_rect_make(a.x, a.y + 5, a.w, a.h - 5 - bar_h - 2);
    tui_rect art;
    ls_tui_split(body, &art, &s_list);
    if (s_details) s_list = body;
    else sweep(sf, art);
    ls_panel_box(sf, s_list, s_details ? "OBSERVATION / BEST SIGNAL" : "SEEN / SELECT FOR DETAILS", TUI_CYAN);
    if (s_details) {
        display_name(name, s_detail.name);
        text(sf, s_list, 1, name[0] ? name : "(no advertised name / hidden SSID)", WHITE);
        snprintf(line, sizeof(line), "%s %02x:%02x:%02x:%02x:%02x:%02x", s_detail.kind == LS_SURVEY_WIFI ? "Wi-Fi" : "BLE",
                 s_detail.addr[0], s_detail.addr[1], s_detail.addr[2], s_detail.addr[3], s_detail.addr[4], s_detail.addr[5]);
        text(sf, s_list, 2, line, CYAN);
        if (s_detail.kind == LS_SURVEY_WIFI) snprintf(line, sizeof(line), "%d dBm  CH %u  Auth %u", s_detail.rssi, s_detail.channel, s_detail.auth);
        else snprintf(line, sizeof(line), "%d dBm  Address type %u", s_detail.rssi, s_detail.addr_type);
        text(sf, s_list, 3, line, WHITE);
        snprintf(line, sizeof(line), "First %lu  Last %lu (%s)", (unsigned long)s_detail.first_s, (unsigned long)s_detail.last_s, s_detail.epoch ? "UTC seconds" : "uptime");
        text(sf, s_list, 4, line, LS_ATTR_DIM);
        if (s_detail.positioned) snprintf(line, sizeof(line), "Best: %.6f, %.6f", s_detail.lat, s_detail.lon);
        else snprintf(line, sizeof(line), "Best signal has no fresh GPS position");
        text(sf, s_list, 5, line, WHITE);
        if (s_detail.kind == LS_SURVEY_BLE) text(sf, s_list, 6, "Random BLE addresses rotate", LS_ATTR_DIM);
    } else {
        static const char *SORT[] = {"STRONGEST", "NEWEST", "NAME"};
        /* A short pane (landscape) packs two-line rows from the row under the
           header, so the height it has goes to the list. */
        const bool tight = s_list.h < 14;
        s_top = tight ? 2 : 3;
        s_pitch = tight ? 2 : 3;
        s_rows = (s_list.h - s_top - 1) / s_pitch;
        if (s_rows < 1) s_rows = 1;
        unsigned start = s_pick / s_rows * s_rows;
        unsigned end = start + s_rows;
        if (end > count) end = count;
        snprintf(line, sizeof(line), "Sort %s  %u-%u / %u", SORT[s_sort], count ? start + 1 : 0, end, count);
        text(sf, s_list, 1, line, LS_ATTR_DIM);
        if (!count) text(sf, s_list, s_top, "No observations recorded", WHITE);
        for (int i = 0; i < s_rows; i++) {
            ls_survey_entry_t e;
            if (!ls_survey_at(start + i, s_sort, &e)) break;
            tui_rect row = tui_rect_make(s_list.x + 1, s_list.y + s_top + i * s_pitch, s_list.w - 2, s_pitch);
            if (start + i == s_pick) ls_fill_dither(sf, row, LS_DITHER_LIGHT, TUI_CYAN);
            display_name(name, e.name);
            snprintf(line, sizeof(line), "%c %s %.32s", start + i == s_pick ? '>' : ' ', e.kind == LS_SURVEY_WIFI ? "W" : "B", name[0] ? name : "(unnamed)");
            text(sf, row, 0, line, WHITE);
            snprintf(line, sizeof(line), "%d dBm  %02x:%02x:%02x:%02x:%02x:%02x %s", e.rssi,
                e.addr[0], e.addr[1], e.addr[2], e.addr[3], e.addr[4], e.addr[5], e.positioned ? "GPS" : "--");
            text(sf, row, 1, line, CYAN);
        }
    }
    text(sf, a, a.h - bar_h - 2, s_view.running ? "Passive only / random BLE addresses rotate" : s_view.path, LS_ATTR_DIM);
    text(sf, a, a.h - bar_h - 1, "[ PREV   ] NEXT   ENTER detail", LS_ATTR_DIM);
    ls_btn_t buttons[] = {
        {s_view.running ? "STOP" : "START", NULL, 's', s_view.running, s_view.busy, false},
        {"SORT", NULL, 'r', false, false, false},
        {"OPTIONS", NULL, 'o', false, false, false},
        {s_details ? "BACK" : "DETAIL", NULL, 'd', s_details, !count, false},
        {"PREV", NULL, '[', false, !count, false},
        {"NEXT", NULL, ']', false, !count, false},
    };
    ls_btn_bar_raised(sf, tui_rect_make(a.x, a.y + a.h - bar_h, a.w, bar_h), buttons, 6, -1);
}

bool ls_survey_key(ls_tk_t k, char ch)
{
    if (k == LS_TK_ESC && s_details) { s_details = false; return true; }
    if (k == LS_TK_CHAR && ch && strchr("sS", ch) && s_view.busy) return true;
    if (k == LS_TK_ENTER) { action(3); return true; }
    if (k == LS_TK_UP || k == LS_TK_DOWN || ch == '[' || ch == ']') {
        unsigned count = s_view.wifi + s_view.ble;
        int step = k == LS_TK_UP ? -1 : k == LS_TK_DOWN ? 1 : ch == '[' ? -s_rows : s_rows;
        if (count) s_pick = (unsigned)(((int)s_pick + step + (int)count * s_rows) % (int)count);
        s_details = false;
        return true;
    }
    if (k == LS_TK_CHAR) {
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        const char *p = ch ? strchr("srod", ch) : NULL;
        if (p) { action((int)(p - "srod")); return true; }
    }
    return false;
}

bool ls_survey_touch(int x, int y)
{
    int b = ls_btn_hit(x, y);
    if (b >= 0) { action(b); return true; }
    if (!s_details && x >= s_list.x && x < s_list.x + s_list.w && y >= s_list.y + s_top && y < s_list.y + s_list.h - 1) {
        unsigned rank = s_pick / s_rows * s_rows + (y - s_list.y - s_top) / s_pitch;
        if (ls_survey_at(rank, s_sort, &s_detail)) { s_pick = rank; s_details = true; }
    }
    return true;
}
