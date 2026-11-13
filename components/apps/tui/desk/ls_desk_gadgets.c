/* Desktop gadgets: small panels on the ground, top right, each fed by the
   board's own state. The clock and the GPS fix; what the receiver is doing;
   the mesh; memory. They are switched on and off from the desktop's menu
   (Gadgets) and a click on one opens the app behind it.

   Nothing in a gadget moves by itself: a figure changes when its value did,
   a trace gains a sample a second while it is on. */

#include "ls_desk_priv.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "ls_gps.h"
#include "../ls_tui_screen.h"
#include "../ls_value.h"

extern void ls_gps_get(ls_gps_state_t *out) __attribute__((weak));
extern int  ls_notify_unread(void) __attribute__((weak));

#define GW 31
EXT_RAM_BSS_ATTR static ls_gps_state_t s_g;
static struct { tui_rect r; uint8_t which; } s_hit[4];
static int s_hits;
#define TRACE_N 40
EXT_RAM_BSS_ATTR static int s_level[TRACE_N];
static int s_level_n;
static int64_t s_level_us;

static uint8_t ground(uint8_t fg) { return (uint8_t)((dk_attr(R_DESK) & 0xF0) | (fg & 0x0F)); }
/* A colour on the desktop's ground: bright on a dark one, plain on a light. */
static uint8_t tint(uint8_t hue)
{
    uint8_t c = hue & 7;
    if (!dk_light_desk()) return ground((uint8_t)(c | TUI_BRIGHT));
    /* on a light ground (Classic's teal) blue and green sink into it: the
       accents are black there, and red keeps its warning */
    if (c == TUI_YELLOW || c == TUI_RED) return ground(TUI_RED);
    if (c == TUI_MAGENTA) return ground(TUI_MAGENTA);
    return ground(TUI_BLACK);
}

/* A panel: a rounded line frame in the label colour, its name in the top
   line, the ground cleared under it so a picture wallpaper does not show
   through the words. Returns where its first line of text goes. */
static tui_rect panel(tui_surface *sf, tui_rect work, int *y, int rows, const char *name, uint8_t hue, uint8_t which)
{
    const tui_rect r = tui_rect_make(work.x + work.w - GW - 1, *y, GW, rows + 2);
    const tui_rect clip = tui_rect_intersect(work, r);
    const uint8_t line = dk_attr(R_DESK_TEXT);
    dk_fill(sf, clip, dk_attr(R_DESK_TEXT));
    ls_box_put(sf, clip, r.x, r.y, LS_BOX_ARC_DR, line);
    ls_box_put(sf, clip, r.x + r.w - 1, r.y, LS_BOX_ARC_DL, line);
    ls_box_put(sf, clip, r.x, r.y + r.h - 1, LS_BOX_ARC_UR, line);
    ls_box_put(sf, clip, r.x + r.w - 1, r.y + r.h - 1, LS_BOX_ARC_UL, line);
    dk_hline(sf, clip, r.x + 1, r.y, r.w - 2, LS_BOX_H, line);
    dk_hline(sf, clip, r.x + 1, r.y + r.h - 1, r.w - 2, LS_BOX_H, line);
    dk_vline(sf, clip, r.x, r.y + 1, rows, LS_BOX_V, line);
    dk_vline(sf, clip, r.x + r.w - 1, r.y + 1, rows, LS_BOX_V, line);
    char head[24];
    snprintf(head, sizeof(head), " %s ", name);
    dk_text(sf, clip, r.x + 2, r.y, head, tint(hue));
    if (s_hits < 4) { s_hit[s_hits].r = r; s_hit[s_hits++].which = which; }
    *y += rows + 3;
    return tui_rect_intersect(work, tui_rect_make(r.x + 2, r.y + 1, r.w - 4, rows));
}

static void clock_gadget(tui_surface *sf, tui_rect work, int *y)
{
    const tui_rect in = panel(sf, work, y, 3, "CLOCK", TUI_CYAN, DKG_CLOCK);
    static const char *const DAY[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const time_t now = time(NULL);
    const struct tm *gp = gmtime(&now);                 /* UI task only */
    char line[48];
    if (gp && (dk_sys_time_source() > 0 || gp->tm_year > 120)) {
        snprintf(line, sizeof(line), "%02d:%02d:%02d", gp->tm_hour, gp->tm_min, gp->tm_sec);
        int x = in.x + dk_text(sf, in, in.x, in.y, line, ground(TUI_ATTR_FG(dk_attr(R_DESK_TEXT)) | TUI_BRIGHT));
        dk_text(sf, in, x + 1, in.y, "UTC", tint(TUI_CYAN));
        snprintf(line, sizeof(line), "%s %d %s", DAY[gp->tm_wday % 7], gp->tm_mday, MON[gp->tm_mon % 12]);
        dk_text(sf, in, in.x + in.w - dk_text_len(line), in.y, line, dk_attr(R_DESK_TEXT));
    } else {
        dk_text(sf, in, in.x, in.y, "The clock is not set", tint(TUI_YELLOW));
    }
    if (ls_gps_get) {
        ls_gps_get(&s_g);
        if (!s_g.alive) dk_text(sf, in, in.x, in.y + 1, "GPS off", dk_attr(R_DESK_TEXT));
        else if (s_g.fix) {
            int x = in.x + dk_text(sf, in, in.x, in.y + 1, "GPS ", dk_attr(R_DESK_TEXT));
            snprintf(line, sizeof(line), "%s %d sats", s_g.sats_used >= 4 ? "3D" : "2D", s_g.sats_used);
            x += dk_text(sf, in, x, in.y + 1, line, tint(TUI_GREEN));
            snprintf(line, sizeof(line), "%.4f %c  %.4f %c", s_g.lat_deg < 0 ? -s_g.lat_deg : s_g.lat_deg, s_g.lat_deg < 0 ? 'S' : 'N',
                     s_g.lon_deg < 0 ? -s_g.lon_deg : s_g.lon_deg, s_g.lon_deg < 0 ? 'W' : 'E');
            dk_text(sf, in, in.x, in.y + 2, line, dk_attr(R_DESK_TEXT));
        } else {
            snprintf(line, sizeof(line), "GPS no fix, %d in view", s_g.sats_visible);
            dk_text(sf, in, in.x, in.y + 1, line, tint(TUI_YELLOW));
        }
    }
}

static const ls_app_t *claimed_app(const char *claimed)
{
    for (int i = 0; claimed && i < ls_app_count(); i++) {
        const ls_app_t *a = ls_app_at(i);
        if (a && a->screen && a->screen->radio && !strcmp(a->screen->radio, claimed)) return a;
    }
    return NULL;
}

static void radio_gadget(tui_surface *sf, tui_rect work, int *y)
{
    const tui_rect in = panel(sf, work, y, 3, "RECEIVER", TUI_GREEN, DKG_RADIO);
    const char *claimed = ls_tui_radio_claimed();
    if (!claimed) {
        dk_text(sf, in, in.x, in.y, "Parked. Open a radio app", dk_attr(R_DESK_TEXT));
        dk_text(sf, in, in.x, in.y + 1, "to start receiving.", dk_attr(R_DESK_TEXT));
        s_level_n = 0;
        return;
    }
    const ls_app_t *a = claimed_app(claimed);
    char live[24];
    const char *lv = a ? dk_live_line(a, live, sizeof(live)) : NULL;
    int x = in.x;
    ls_box_put(sf, in, x, in.y, LS_SHAPE_DOT, tint(TUI_GREEN));
    x += 2;
    x += dk_text(sf, in, x, in.y, claimed, tint(a ? a->hue : TUI_GREEN));
    if (lv) dk_text(sf, in, x + 1, in.y, lv, ground(TUI_ATTR_FG(dk_attr(R_DESK_TEXT)) | TUI_BRIGHT));
    /* the receiver's own level, when its app publishes one: a meter now and
       a trace of the last seconds, a sample a second */
    static const char *const PATH[2][2] = {{"fm", "fm.level"}, {"p25", "p25.level"}};
    ls_val_t v;
    bool have = false;
    for (int k = 0; k < 2 && a && !have; k++)
        if (!strcmp(a->id, PATH[k][0])) have = ls_value_read(PATH[k][1], &v, NULL) && v.kind == LS_VAL_FLOAT;
    if (!have) { s_level_n = 0; return; }
    int pm = (int)(v.f * 1000.0f);
    pm = pm < 0 ? 0 : pm > 1000 ? 1000 : pm;
    const int64_t now = esp_timer_get_time();
    if (now - s_level_us >= 1000000 || !s_level_n) {
        s_level_us = now;
        if (s_level_n == TRACE_N) { memmove(s_level, s_level + 1, sizeof(int) * (TRACE_N - 1)); s_level_n--; }
        s_level[s_level_n++] = pm;
    }
    dk_text(sf, in, in.x, in.y + 1, "level", dk_attr(R_DESK_TEXT));
    const int mw = in.w - 6, on = (mw * pm + 500) / 1000;
    for (int i = 0; i < mw; i++)
        ls_box_put(sf, in, in.x + 6 + i, in.y + 1, i < on ? LS_BOX_DBL_H : LS_BOX_DASH_H, i < on ? tint(TUI_GREEN) : dk_attr(R_DESK_TEXT));
    for (int i = 0; i < s_level_n; i += 2)
        dk_spark(sf, in, in.x + 6 + i / 2, in.y + 2, s_level + i, i + 1 < s_level_n ? 2 : 1, 1000, tint(TUI_CYAN));
    dk_text(sf, in, in.x, in.y + 2, "trace", dk_attr(R_DESK_TEXT));
}

static void mesh_gadget(tui_surface *sf, tui_rect work, int *y)
{
    const tui_rect in = panel(sf, work, y, 2, "MESH", TUI_MAGENTA, DKG_MESH);
    ls_val_t up, v;
    char line[48];
    if (!ls_value_read("mesh.up", &up, NULL)) { dk_text(sf, in, in.x, in.y, "No mesh in this build", dk_attr(R_DESK_TEXT)); return; }
    const int unread = ls_notify_unread ? ls_notify_unread() : 0;
    ls_box_put(sf, in, in.x, in.y, LS_SHAPE_DOT, up.i ? tint(TUI_GREEN) : dk_attr(R_DESK_TEXT));
    int x = in.x + 2 + dk_text(sf, in, in.x + 2, in.y, up.i ? "up" : "off", dk_attr(R_DESK_TEXT));
    if (unread > 0) {
        snprintf(line, sizeof(line), "%d unread", unread);
        dk_text(sf, in, x + 2, in.y, line, tint(TUI_MAGENTA));
    } else if (up.i) dk_text(sf, in, x + 2, in.y, "nothing unread", dk_attr(R_DESK_TEXT));
    long rx = -1, tx = -1;
    if (ls_value_read("mesh.rx", &v, NULL) && v.kind == LS_VAL_INT) rx = v.i;
    if (ls_value_read("mesh.tx", &v, NULL) && v.kind == LS_VAL_INT) tx = v.i;
    if (rx >= 0 || tx >= 0) {
        x = in.x + snprintf(line, sizeof(line), "heard %ld  sent %ld", rx < 0 ? 0 : rx, tx < 0 ? 0 : tx);
        dk_text(sf, in, in.x, in.y + 1, line, dk_attr(R_DESK_TEXT));
        if (ls_value_read("mesh.rssi", &v, NULL) && v.kind == LS_VAL_FLOAT) {
            snprintf(line, sizeof(line), "%.0f dBm", (double)v.f);
            dk_text(sf, in, in.x + in.w - dk_text_len(line), in.y + 1, line, tint(TUI_CYAN));
        }
    }
}

static void system_gadget(tui_surface *sf, tui_rect work, int *y)
{
    const tui_rect in = panel(sf, work, y, 2, "BOARD", TUI_YELLOW, DKG_SYSTEM);
    dk_perf_t p;
    dk_sys_perf(&p);
    char line[48];
    snprintf(line, sizeof(line), "RAM %ld KB   PSRAM %ld KB", p.internal_kb, p.psram_kb);
    dk_text(sf, in, in.x, in.y, line, p.internal_kb < 8 ? tint(TUI_RED) : dk_attr(R_DESK_TEXT));
    snprintf(line, sizeof(line), "up %ldd %02ld:%02ld   %d windows", p.uptime_s / 86400, p.uptime_s / 3600 % 24, p.uptime_s / 60 % 60,
             ls_desk_window_count());
    dk_text(sf, in, in.x, in.y + 1, line, dk_attr(R_DESK_TEXT));
}

void dk_gadgets_draw(tui_surface *sf, tui_rect work)
{
    s_hits = 0;
    const int mask = dk_shell_gadgets();
    if (!mask || work.w < 100) return;                  /* a small grid keeps its room for windows */
    int y = work.y + 1;
    if (mask & DKG_CLOCK) clock_gadget(sf, work, &y);
    if ((mask & DKG_RADIO) && y + 5 < work.h - 3) radio_gadget(sf, work, &y);
    if ((mask & DKG_MESH) && y + 4 < work.h - 3) mesh_gadget(sf, work, &y);
    if ((mask & DKG_SYSTEM) && y + 4 < work.h - 3) system_gadget(sf, work, &y);
}

static int which_at(int col, int row)
{
    for (int i = 0; i < s_hits; i++) if (tui_rect_contains(s_hit[i].r, col, row)) return s_hit[i].which;
    return 0;
}

bool dk_gadgets_hit(int col, int row) { return which_at(col, row) != 0; }

bool dk_gadgets_click(int col, int row)
{
    switch (which_at(col, row)) {
    case DKG_CLOCK: ls_desk_open_app("gps"); return true;
    case DKG_RADIO: {
        const ls_app_t *a = claimed_app(ls_tui_radio_claimed());
        if (a) dk_open_app_win(a);
        else ls_desk_open_app("radios");
        return true;
    }
    case DKG_MESH: ls_desk_open_app("mesh"); return true;
    case DKG_SYSTEM: dk_tasks_open(); return true;
    default: return false;
    }
}
