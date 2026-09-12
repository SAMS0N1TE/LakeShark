#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_keyboard.h"
#include "../../ls_wireless.h"
#include <stdio.h>
#include <string.h>

#define INK(c) TUI_ATTR((c), TUI_BLACK)
#define WHITE INK(TUI_WHITE | TUI_BRIGHT)
#define CYAN INK(TUI_CYAN | TUI_BRIGHT)

typedef struct { tui_rect rect; char key; int ap; } hit_t;
static hit_t s_hits[24];
static int s_hit_count, s_tab, s_selected, s_page_size = 3;
static ls_wireless_snapshot_t s_view;
static char s_join_ssid[33], s_note[80];
static bool s_join_secure;

static tui_rect inset(tui_rect a)
{
    return tui_rect_make(a.x + 1, a.y + 1, a.w - 2, a.h - 2);
}

static void line(tui_surface *sf, tui_rect a, int row, const char *text, uint8_t attr)
{
    tui_put_str(sf, a, a.x + 1, a.y + row, text, attr);
}

static void button(tui_surface *sf, tui_rect r, const char *label, char key,
                   bool on, bool dim, int ap)
{
    uint8_t hue = dim ? LS_FAINT_FG : on ? TUI_GREEN | TUI_BRIGHT : TUI_CYAN;
    ls_fill_dither(sf, inset(r), on ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT, hue);
    ls_panel_box(sf, r, "", hue);
    char text[48];
    snprintf(text, sizeof(text), "[%c] %s", key, label);
    ls_dither_label(sf, inset(r), (r.h - 3) / 2,
                    text, dim ? LS_ATTR_DIM : WHITE);
    if (s_hit_count < (int)(sizeof(s_hits) / sizeof(s_hits[0])))
        s_hits[s_hit_count++] = (hit_t){r, dim ? 0 : key, ap};
}

static void request(ls_wireless_op_t op, const char *ssid, const char *pass)
{
    if (ls_wireless_request(op, ssid, pass)) s_note[0] = 0;
    else snprintf(s_note, sizeof(s_note), "A connection update is already running");
}

static void password_done(const char *pass)
{
    size_t n = strlen(pass);
    if ((s_join_secure && n < 8) || (n && (n < 8 || n > 63))) {
        ls_keyboard_open_secret("USE 8-63 CHARACTERS", 63, password_done);
        return;
    }
    request(LS_WIRELESS_JOIN, s_join_ssid, pass);
}

static void ssid_done(const char *ssid)
{
    if (!ssid[0]) return;
    snprintf(s_join_ssid, sizeof(s_join_ssid), "%s", ssid);
    s_join_secure = false;
    ls_keyboard_open_secret("PASSWORD / OPEN: EMPTY", 63, password_done);
}

static void join_ap(int index)
{
    if (s_view.busy || index < 0 || index >= s_view.ap_count) return;
    s_selected = index;
    const ls_wireless_ap_t *ap = &s_view.aps[index];
    snprintf(s_join_ssid, sizeof(s_join_ssid), "%s", ap->ssid);
    s_join_secure = ap->secure;
    if (ap->secure) ls_keyboard_open_secret("WI-FI PASSWORD", 63, password_done);
    else request(LS_WIRELESS_JOIN, s_join_ssid, "");
}

static void act(char ch)
{
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if (ch == 'w' || ch == 'b') { s_tab = ch == 'b'; s_hit_count = 0; return; }
    if (ch == '[' || ch == ']') {
        int p = s_selected / s_page_size + (ch == ']' ? 1 : -1);
        int pages = (s_view.ap_count + s_page_size - 1) / s_page_size;
        if (pages > 0) s_selected = ((p + pages) % pages) * s_page_size;
        return;
    }
    if (s_view.busy) return;
    if (s_tab) {
        if (!s_view.bt_available) return;
        if (ch == 's') request(LS_WIRELESS_BT_RESCAN, NULL, NULL);
        if (ch == 'd') request(LS_WIRELESS_BT_STOP, NULL, NULL);
        return;
    }
    if (!s_view.wifi_available) return;
    switch (ch) {
    case 's': request(LS_WIRELESS_SCAN, NULL, NULL); break;
    case 'r': request(LS_WIRELESS_SAVED, NULL, NULL); break;
    case 'm': ls_keyboard_open("NETWORK NAME", "", 32, ssid_done); break;
    case 'j': join_ap(s_selected); break;
    case 'd': request(LS_WIRELESS_LEAVE, NULL, NULL); break;
    case 'f': request(LS_WIRELESS_FORGET, NULL, NULL); break;
    }
}

static void trace(tui_surface *sf, tui_rect a, bool bt, bool traffic)
{
    ls_panel_box(sf, a, traffic ? "CONTROL TRAFFIC / SEC" : "SIGNAL / dBm", TUI_CYAN);
    tui_rect g = tui_rect_make(a.x + 5, a.y + 2, a.w - 7, a.h - 4);
    if (g.w < 4 || g.h < 2) return;
    line(sf, a, 1, traffic ? "RX commands +  TX telemetry *" : "Fixed scale  -100 to -20", LS_ATTR_DIM);
    int top = traffic ? 10 : -20, bottom = traffic ? 0 : -100;
    const ls_wireless_history_t *h = &s_view.history;
    if (traffic) {
        for (unsigned i = 0; i < h->count; i++) {
            const ls_wireless_sample_t *p = &h->samples[i];
            if (!(p->valid & LS_WIRELESS_RATE_VALID)) continue;
            while (top < p->rx || top < p->tx) top *= 2;
        }
    }
    char text[20];
    snprintf(text, sizeof(text), "%d", top);
    tui_put_str(sf, a, a.x + 1, g.y, text, LS_ATTR_DIM);
    snprintf(text, sizeof(text), "%d", bottom);
    tui_put_str(sf, a, a.x + 1, g.y + g.h - 1, text, LS_ATTR_DIM);
    for (int x = 0; x < g.w; x += 2)
        tui_put_char(sf, g, g.x + x, g.y + g.h / 2, '.', INK(TUI_CYAN));
    bool any = false;
    unsigned oldest = (h->next + LS_WIRELESS_SAMPLES - h->count) % LS_WIRELESS_SAMPLES;
    for (unsigned i = 0; i < h->count; i++) {
        const ls_wireless_sample_t *p = &h->samples[(oldest + i) % LS_WIRELESS_SAMPLES];
        unsigned mask = traffic ? LS_WIRELESS_RATE_VALID : bt ? LS_WIRELESS_BT_VALID : LS_WIRELESS_WIFI_VALID;
        if (!(p->valid & mask)) continue;
        any = true;
        int x = g.x + (int)(LS_WIRELESS_SAMPLES - h->count + i) * (g.w - 1) / (LS_WIRELESS_SAMPLES - 1);
        for (int series = 0; series < (traffic ? 2 : 1); series++) {
            int value = traffic ? (series ? p->tx : p->rx) : bt ? p->bt : p->wifi;
            if (value < bottom) value = bottom;
            if (value > top) value = top;
            int y = g.y + (top - value) * (g.h - 1) / (top - bottom);
            tui_put_char(sf, g, x, y, series ? '*' : '+',
                INK(series ? TUI_YELLOW | TUI_BRIGHT : TUI_GREEN | TUI_BRIGHT));
        }
    }
    if (!any) ls_dither_label(sf, g, g.h / 2,
        traffic ? "Waiting for link traffic" : "Connect to measure signal", WHITE);
    line(sf, a, a.h - 2, "60s ago", LS_ATTR_DIM);
    tui_put_str(sf, a, a.x + a.w - 5, a.y + a.h - 2, "now", LS_ATTR_DIM);
}

static void channels(tui_surface *sf, tui_rect a)
{
    ls_panel_box(sf, a, "2.4 GHz / SCAN RESULTS", TUI_CYAN);
    tui_rect g = tui_rect_make(a.x + 2, a.y + 1, a.w - 4, a.h - 4);
    if (g.h < 2 || g.w < 14) return;
    int counts[14] = {0}, max = 1;
    for (int i = 0; i < s_view.ap_count; i++) {
        int ch = s_view.aps[i].channel;
        if (ch > 0 && ch <= 14 && ++counts[ch - 1] > max) max = counts[ch - 1];
    }
    for (int i = 0; i < 14; i++) {
        int x = g.x + i * g.w / 14;
        int n = counts[i] * g.h / max;
        for (int y = 0; y < n; y++)
            tui_put_char(sf, g, x, g.y + g.h - 1 - y, LS_TUI_SHADE_50, CYAN);
        if (i == 0 || i == 5 || i == 10 || i == 13) {
            char text[4]; snprintf(text, sizeof(text), "%d", i + 1);
            tui_put_str(sf, a, x, g.y + g.h, text, WHITE);
        }
    }
    char text[72];
    if (s_view.scan_revision)
        snprintf(text, sizeof(text), "%d networks / %lus ago", s_view.ap_count,
                 (unsigned long)((s_view.now_ms - s_view.scan_ms) / 1000));
    else snprintf(text, sizeof(text), "SCAN to measure nearby channels");
    line(sf, a, a.h - 2, text, LS_ATTR_DIM);
}

static void networks(tui_surface *sf, tui_rect a)
{
    ls_panel_box(sf, a, "NEARBY / TAP TO JOIN", TUI_CYAN);
    int row_h = ls_tui_is_wide() ? 3 : 4;
    int nav_h = ls_tui_is_wide() ? 3 : 4;
    s_page_size = (a.h - 3 - nav_h) / row_h;
    if (s_page_size < 1) s_page_size = 1;
    if (s_page_size > 5) s_page_size = 5;
    int start = s_selected / s_page_size * s_page_size;
    if (!s_view.ap_count) {
        line(sf, a, 3, s_view.busy ? "Scanning..." : "Press SCAN to find networks", WHITE);
    }
    for (int i = 0; i < s_page_size && start + i < s_view.ap_count; i++) {
        const ls_wireless_ap_t *ap = &s_view.aps[start + i];
        tui_rect r = tui_rect_make(a.x + 1, a.y + 2 + i * row_h, a.w - 2, row_h);
        if (r.y + r.h > a.y + a.h - nav_h - 1) break;
        bool selected = start + i == s_selected;
        if (selected) ls_fill_dither(sf, r, LS_DITHER_LIGHT, TUI_CYAN);
        char text[64];
        snprintf(text, sizeof(text), "%c %.32s", selected ? '>' : ' ', ap->ssid);
        line(sf, r, 0, text, WHITE);
        snprintf(text, sizeof(text), "  %d dBm  CH %d  %s", ap->rssi, ap->channel,
                 ap->secure ? "PASSWORD" : "OPEN");
        line(sf, r, 1, text, INK(TUI_YELLOW | TUI_BRIGHT));
        if (s_hit_count < 24) s_hits[s_hit_count++] = (hit_t){r, 'j', start + i};
    }
    int width = (a.w - 2) / 2;
    int y = a.y + a.h - nav_h - 1;
    button(sf, tui_rect_make(a.x + 1, y, width, nav_h), "PREV", '[', false,
           s_view.ap_count <= s_page_size, -1);
    button(sf, tui_rect_make(a.x + 1 + width, y, a.w - 2 - width, nav_h), "NEXT", ']', false,
           s_view.ap_count <= s_page_size, -1);
}

static void activity(tui_surface *sf, tui_rect a)
{
    ls_panel_box(sf, a, "FLIPPER CONTROL HEAD", TUI_CYAN);
    bool moving = s_view.bt_state > 0 && !s_view.bt_ready;
    const ls_wireless_history_t *h = &s_view.history;
    if (h->count) {
        const ls_wireless_sample_t *p = &h->samples[(h->next + LS_WIRELESS_SAMPLES - 1) % LS_WIRELESS_SAMPLES];
        moving |= s_view.bt_ready && (p->rx || p->tx) && s_view.now_ms - h->last_ms < 2000;
    }
    unsigned phase = moving ? s_view.now_ms / 180 % 4 : 0;
    int cx = a.x + a.w / 2, cy = a.y + a.h / 2;
    for (int n = 1; n <= 3; n++) {
        uint8_t attr = INK((unsigned)n == phase ? TUI_YELLOW | TUI_BRIGHT : TUI_CYAN);
        tui_put_char(sf, a, cx - 4 - n * 3, cy, '(', attr);
        tui_put_char(sf, a, cx + 4 + n * 3, cy, ')', attr);
    }
    tui_put_str(sf, a, cx - 3, cy, s_view.bt_ready ? "LINKED" : "  BT  ", WHITE);
    line(sf, a, a.h - 2, s_view.bt_ready ? "Live commands + radio telemetry" :
         s_view.stock_head_seen ? "Open LakeShark on the Flipper" : "SCAN searches for LakeShark heads", LS_ATTR_DIM);
}

static void draw(tui_surface *sf, tui_rect area)
{
    uint32_t old_scan = s_view.scan_revision;
    char selected[33] = "";
    if (s_selected < s_view.ap_count) snprintf(selected, sizeof(selected), "%s", s_view.aps[s_selected].ssid);
    ls_wireless_get(&s_view);
    if (old_scan != s_view.scan_revision) {
        s_selected = 0;
        for (int i = 0; i < s_view.ap_count; i++)
            if (!strcmp(selected, s_view.aps[i].ssid)) { s_selected = i; break; }
    }
    if (s_selected >= s_view.ap_count) s_selected = 0;
    s_hit_count = 0;
    if (area.w < 28 || area.h < 22) {
        ls_panel_notice(sf, area, "LINK", "More screen space needed", "Use a smaller display font");
        return;
    }
    bool wide = ls_tui_is_wide();
    int tab_h = wide ? 3 : 4, action_h = wide ? 3 : 8, msg_h = wide ? 1 : 2;
    button(sf, tui_rect_make(area.x, area.y, area.w / 2, tab_h), "WI-FI", 'W', !s_tab, false, -1);
    button(sf, tui_rect_make(area.x + area.w / 2, area.y, area.w - area.w / 2, tab_h),
           "BLUETOOTH", 'B', s_tab, false, -1);
    tui_rect status = tui_rect_make(area.x, area.y + tab_h, area.w, wide ? 4 : 6);
    ls_panel_box(sf, status, s_tab ? "BLUETOOTH STATUS" : "WI-FI STATUS", TUI_CYAN);
    char text[128];
    if (!s_tab) {
        line(sf, status, 1, s_view.wifi_available ? s_view.wifi_status : "Wi-Fi unavailable in this build", WHITE);
        if (s_view.wifi_signal) snprintf(text, sizeof(text), "%d dBm  CH %d  IP %s", s_view.wifi_rssi, s_view.channel, s_view.ip);
        else snprintf(text, sizeof(text), "Station mode / saved network reconnect");
    } else {
        snprintf(text, sizeof(text), "%s  %s", s_view.bt_available ? s_view.bt_status : "Bluetooth unavailable", s_view.peer);
        line(sf, status, 1, text, WHITE);
        if (s_view.bt_signal) snprintf(text, sizeof(text), "%s  %d dBm", s_view.address, s_view.bt_rssi);
        else snprintf(text, sizeof(text), "LakeShark app on Flipper / BLE");
    }
    line(sf, status, 2, text, INK(TUI_YELLOW | TUI_BRIGHT));
    if (s_tab) snprintf(text, sizeof(text), "RX %lu commands  TX %lu frames  drop %lu",
        (unsigned long)s_view.rx, (unsigned long)s_view.tx, (unsigned long)s_view.drops);
    else snprintf(text, sizeof(text), "Tap a network or use MANUAL for a hidden SSID");
    if (!wide) line(sf, status, 4, text, LS_ATTR_DIM);
    tui_rect content = tui_rect_make(area.x, status.y + status.h, area.w,
        area.h - tab_h - status.h - action_h - msg_h);
    tui_rect first, second;
    ls_tui_split(content, &first, &second);
    if (s_tab) {
        if (wide) activity(sf, first);
        else {
            int h = first.h > 14 ? 7 : 5;
            activity(sf, tui_rect_make(first.x, first.y, first.w, h));
            trace(sf, tui_rect_make(first.x, first.y + h, first.w, first.h - h), true, false);
        }
        if (wide) {
            int h = second.h / 2;
            trace(sf, tui_rect_make(second.x, second.y, second.w, h), true, false);
            trace(sf, tui_rect_make(second.x, second.y + h, second.w, second.h - h), true, true);
        } else trace(sf, second, true, true);
    } else {
        networks(sf, first);
        int h = second.h / 2;
        trace(sf, tui_rect_make(second.x, second.y, second.w, h), false, false);
        channels(sf, tui_rect_make(second.x, second.y + h, second.w, second.h - h));
    }
    int msg_y = area.y + area.h - action_h - msg_h;
    const char *message = s_note[0] ? s_note : s_view.message;
    if (s_view.busy) {
        snprintf(text, sizeof(text), "%c %s", "|/-\\"[(s_view.now_ms / 180) % 4],
            s_view.operation == LS_WIRELESS_SCAN ? "Scanning nearby networks..." : "Updating connection...");
        message = text;
    }
    tui_put_str(sf, area, area.x + 1, msg_y, message, WHITE);
    static const char *labels[] = {"SCAN", "SAVED", "MANUAL", "JOIN", "LEAVE", "FORGET"};
    static const char keys[] = {'S', 'R', 'M', 'J', 'D', 'F'};
    int count = s_tab ? 2 : 6, columns = wide ? count : s_tab ? 2 : 3;
    int rows = (count + columns - 1) / columns, h = action_h / rows;
    if (h > 5) h = 5;
    int w = area.w / columns;
    for (int i = 0; i < count; i++) {
        bool dim = s_view.busy || (s_tab ? !s_view.bt_available : !s_view.wifi_available);
        if (!s_tab && i == 3 && !s_view.ap_count) dim = true;
        button(sf, tui_rect_make(area.x + i % columns * w, msg_y + msg_h + i / columns * h,
                i % columns == columns - 1 ? area.w - (columns - 1) * w : w, h),
               s_tab ? (i ? "STOP" : "SCAN") : labels[i], s_tab ? (i ? 'D' : 'S') : keys[i],
               false, dim, -1);
    }
}

static bool key(ls_tk_t k, char ch)
{
    if (k == LS_TK_TAB) { act(s_tab ? 'w' : 'b'); return true; }
    if (!s_tab && (k == LS_TK_UP || k == LS_TK_DOWN)) {
        if (s_view.ap_count) s_selected = (s_selected + s_view.ap_count + (k == LS_TK_UP ? -1 : 1)) % s_view.ap_count;
        return true;
    }
    if (!s_tab && k == LS_TK_ENTER) { join_ap(s_selected); return true; }
    if (k == LS_TK_CHAR && strchr("wWbBsSrRmMjJdDfF[]", ch) && ch) { act(ch); return true; }
    return false;
}

static bool touch(int x, int y)
{
    for (int i = 0; i < s_hit_count; i++) {
        hit_t hit = s_hits[i];
        if (x < hit.rect.x || x >= hit.rect.x + hit.rect.w || y < hit.rect.y || y >= hit.rect.y + hit.rect.h) continue;
        if (hit.ap >= 0) join_ap(hit.ap);
        else if (hit.key) act(hit.key);
        break;
    }
    return true;
}

static void enter(void) { s_hit_count = 0; s_note[0] = 0; ls_wireless_set_active(true); }
static void leave(void) { s_hit_count = 0; ls_wireless_set_active(false); }

const ls_tui_screen_t ls_scr_wireless = {
    .name = "LINK", .hint = "W Wi-Fi  B Bluetooth  ARROWS select  ENTER join",
    .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch,
};
