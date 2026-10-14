/* A small instrument: cached session state, shared art, controls and chrome. */
#include "example_app.h"
#if defined(CONFIG_LS_EXAMPLE_APP) && CONFIG_LS_EXAMPLE_APP
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "../ls_tui_ui.h"
#include "../ls_glyph.h"
#include "../ls_motion.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#define HISTORY 24
EXT_RAM_BSS_ATTR static unsigned s_history[HISTORY];
static unsigned s_count;
static int s_head, s_used, s_last;
static ls_fresh_t s_pulse;
static const ls_btn_t BUTTONS[] = {
    {.label = "C COUNT", .value = "+1", .key = 'c'},
    {.label = "R RESET", .value = "ZERO", .key = 'r'},
    {.label = "H HOME", .value = "APPS", .key = 'h'},
};

static void enter(void)
{
    s_count = 0;
    s_head = s_used = 0;
    s_last = -1;
    s_pulse = (ls_fresh_t){0};
}
static bool activate(int i)
{
    if (i == 0) { if (s_count < UINT_MAX) ++s_count; }
    else if (i == 1) s_count = 0;
    else if (i == 2) {
        const ls_app_t *home = ls_app_by_id("home");
        if (home) ls_tui_screen_show(ls_tui_screen_index_of(home->screen));
        return true;
    } else return false;
    /* Record actions, including RESET; drawing never manufactures samples. */
    s_history[s_head] = s_count;
    s_head = (s_head + 1) % HISTORY;
    if (s_used < HISTORY) ++s_used;
    s_last = i;
    ls_fresh_bump(&s_pulse);
    return true;
}

static void instrument(tui_surface *sf, tui_rect r, bool hot)
{
    const uint8_t cyan = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t green = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    ls_panel_box(sf, r, "01 / TAP ENGINE", TUI_CYAN);
    /* Our emblem composes the shared chip and block-letter C, no art table. */
    int ey = r.y + 3, ex = r.x + 3;
    ls_icon_draw(sf, r, ex, ey, LS_ICON_CHIP, cyan);
    tui_fill(sf, tui_rect_make(ex + 3, ey, 4, 6), ' ', cyan);
    ls_glyph_draw(sf, tui_rect_make(ex + 3, ey, 3, 5), 'C',
                  TUI_ATTR(hot ? TUI_YELLOW | TUI_BRIGHT : TUI_BLUE | TUI_BRIGHT, TUI_BLACK));
    tui_put_str(sf, r, ex + 13, ey, "TAP / COUNT", cyan);
    tui_put_str(sf, r, ex + 13, ey + 2, hot ? "[+] EVENT" : "[o] READY",
                hot ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : green);
    tui_put_str(sf, r, ex + 13, ey + 4, "SESSION ONLY", LS_ATTR_DIM);
    char text[48];
    snprintf(text, sizeof(text), "%03u", s_count);
    int n = (int)strlen(text), y = r.y + (r.h - 5) / 2;
    if (y < ey + 7) y = ey + 7;
    for (int i = 0; i < n; ++i)
        ls_glyph_draw(sf, tui_rect_make(r.x + (r.w - (4*n-1))/2 + i*4, y, 3, 5), text[i], green);
    snprintf(text, sizeof(text), "Count: %u", s_count);
    ls_safe_line(sf, r, y + 6, text, LS_ATTR_DIM);
    /* A labelled logarithmic meter fits the complete unsigned count range. */
    unsigned bits = 0;
    for (unsigned v = s_count; v; v >>= 1) ++bits;
    ls_safe_line(sf, r, r.y + r.h - 5, "COUNT MAGNITUDE / LOG2", cyan);
    ls_meter_ascii(sf, r, r.x + 2, r.y + r.h - 3, r.w - 4,
           (float)bits / (sizeof(s_count) * CHAR_BIT), TUI_GREEN | TUI_BRIGHT);
    snprintf(text, sizeof(text), "0 / %u / %u bits", (unsigned)(sizeof(s_count)*CHAR_BIT/2),
             (unsigned)(sizeof(s_count)*CHAR_BIT));
    ls_safe_line(sf, r, r.y + r.h - 2, text, LS_ATTR_DIM);
}

static void history(tui_surface *sf, tui_rect r)
{
    ls_panel_box(sf, r, "02 / EVENT HISTORY", TUI_BLUE | TUI_BRIGHT);
    unsigned peak = 1;
    for (int i = 0; i < s_used; ++i)
        if (s_history[i] > peak) peak = s_history[i];
    char text[48];
    snprintf(text, sizeof(text), "Samples: %d/24  Scale: %u", s_used, peak);
    ls_safe_line(sf, r, r.y + 2, text, LS_ATTR_DIM);
    tui_rect plot = tui_rect_make(r.x + 2, r.y + 4, r.w - 4, r.h - 8);
    /* Fixed grid, real samples only. Latest event is the amber endpoint. */
    for (int y = 0; y < plot.h; y += 3)
        for (int x = 0; x < plot.w; x += 3)
            tui_put_char(sf, plot, plot.x+x, plot.y+y, '.', LS_ATTR_FAINT);
    int previous = 0;
    for (int i = 0; i < s_used; ++i) {
        unsigned value = s_history[(s_head - s_used + i + HISTORY) % HISTORY];
        int y = plot.h - 1 - (int)((uint64_t)value * (plot.h-1) / peak);
        int x = plot.x + i * (plot.w-1) / (HISTORY-1);
        uint8_t ink = TUI_ATTR(i == s_used-1 ? TUI_YELLOW | TUI_BRIGHT : TUI_CYAN, TUI_BLACK);
        if (i) {
            int px = plot.x + (i-1) * (plot.w-1) / (HISTORY-1);
            for (int xx = px+1; xx < x; ++xx)
                tui_put_char(sf, plot, xx, plot.y+previous, '-', ink);
            for (int yy = y < previous ? y : previous; yy <= (y > previous ? y : previous); ++yy)
                tui_put_char(sf, plot, x, plot.y+yy, '|', ink);
        }
        tui_put_char(sf, plot, x, plot.y+y, '+', ink);
        previous = y;
    }
    if (!s_used) ls_safe_line(sf, plot, plot.y + plot.h/2, "Tap COUNT to start a trace", LS_ATTR_DIM);
    ls_safe_line(sf, r, r.y+r.h-3, "COUNT +1 / RESET records zero", LS_ATTR_DIM);
    ls_safe_line(sf, r, r.y+r.h-2, "OLDER  >>>  LAST 24 ACTIONS", TUI_ATTR(TUI_YELLOW, TUI_BLACK));
}

static void draw(tui_surface *sf, tui_rect area)
{
    /* Router owns status, hint and camera runaround. Keep right-side glass
       inside the landscape hole; do not translate the screen or its chrome. */
    bool wide = area.w > area.h * 2;
    tui_rect bar = tui_rect_make(area.x, area.y + area.h - 5, area.w, 5);
    tui_rect body = tui_rect_make(area.x, area.y, area.w - (wide ? 8 : 0), area.h - 5);
    tui_rect face = body, trace = body;
    if (wide) {
        face.w = body.w / 2;
        trace.x += face.w; trace.w -= face.w;
    } else {
        face.h = body.h / 2;
        trace.y += face.h; trace.h -= face.h;
    }
    bool hot = ls_fresh_level(&s_pulse, 600) >= 128;
    instrument(sf, face, hot);
    history(sf, trace);
    ls_btn_bar_raised_row(sf, bar, BUTTONS, 3, hot ? s_last : -1);
}

static bool key(ls_tk_t k, char ch)
{
    return k == LS_TK_CHAR && activate(ls_btn_key(ch, BUTTONS, 3));
}
static bool touch(int col, int row) { return activate(ls_btn_hit(col, row)); }

const ls_tui_screen_t ls_scr_example = {
    .name = "EXAMPLE", .hint = "c COUNT  r RESET  h HOME",
    .enter = enter, .draw = draw, .key = key, .touch = touch,
    .diff_switch = true,
};
const ls_app_doc_t ls_doc_example = {
    .purpose = "Count taps. Opening the app starts a new count.",
    .records = LS_APP_RECORDS_NOTHING, .gps = LS_APP_GPS_UNUSED,
};
#endif
