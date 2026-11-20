/* CHAT: anonymous text chat with everyone, on terminalbay.com's relay so nobody sees an address.
   Three rooms: #global, your country and your area (from the position). Handles are made by the
   server and each one keeps its own colour; yours is marked. Typing goes straight into the line
   at the bottom (a tap opens the keyboard), ENTER sends, and what the server says back (slow down,
   no links) lands under it. Nothing moves unless a message does. Polls only while open, unless
   LISTEN keeps a light once-a-minute poll after you leave. Network: core/ls_hub.h. */
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_keyboard.h"
#include "../../ls_app.h"
#include "../../ls_text.h"
#include "ls_hub.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_timer.h"

enum { CH_GLOBAL, CH_CC, CH_AREA, CH_N };

EXT_RAM_BSS_ATTR static struct {
    int ch, up;                      /* the room shown; lines scrolled up from the newest */
    char cc[4], area[12];            /* the rooms for here, "" without a position */
    uint32_t seen[CH_N];             /* the newest id read in each room */
    uint32_t send_seq;
    int64_t send_at, here_at;
    bool sending;
    char draft[LS_CHAT_TEXT], why[72];
    int tab_x[CH_N], tab_w[CH_N], tab_y;
    tui_rect list, compose;
    int nlines[LS_CHAT_RING], total, cache_w;
    uint32_t cache_seq;
    ls_chat_msg_t m;
    char wrap[12][96];
    char tile[16], room_v[12];
} C;

#define A(fg) TUI_ATTR((fg), TUI_BLACK)

/* Six hues a handle can be, never red: red says something is wrong. */
static uint8_t handle_hue(const char *h)
{
    static const uint8_t HUE[8] = {TUI_CYAN, TUI_GREEN, TUI_YELLOW, TUI_MAGENTA, TUI_BLUE | TUI_BRIGHT,
                                   TUI_CYAN | TUI_BRIGHT, TUI_GREEN | TUI_BRIGHT, TUI_MAGENTA | TUI_BRIGHT};
    uint32_t x = 2166136261u;
    for (; *h; h++) x = (x ^ (uint8_t)*h) * 16777619u;
    return HUE[x & 7];
}

static const char *room(int ch) { return ch == CH_CC ? C.cc : ch == CH_AREA ? C.area : "global"; }

static void here(void)
{
    char cc[4], area[12];
    C.here_at = esp_timer_get_time();
    if (ls_chat_here_names(cc, area)) { memcpy(C.cc, cc, sizeof(C.cc)); memcpy(C.area, area, sizeof(C.area)); }
    for (int i = 0; i < CH_N; i++) ls_chat_watch(i, room(i));
}

static void go(int ch)
{
    if (ch < 0 || ch >= CH_N || !room(ch)[0]) return;
    C.ch = ch;
    C.up = 0;
    C.cache_w = 0;
    if (strcmp(ls_chat_channel(), room(ch))) ls_chat_join(room(ch));
}

static void enter(void)
{
    here();
    if (!room(C.ch)[0]) C.ch = CH_GLOBAL;
    if (!ls_chat_channel()[0]) go(C.ch);
    else for (int i = 0; i < CH_N; i++) if (room(i)[0] && !strcmp(room(i), ls_chat_channel())) C.ch = i;
    ls_chat_focus(true);
}

static void leave(void)
{
    ls_chat_focus(false);
    if (!ls_chat_background()) ls_chat_leave();     /* nothing polls after CHAT unless LISTEN is on */
}

/* ASCII only on this font: a UTF-8 sequence becomes one '?'. */
static void clean(char *s)
{
    char *o = s;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x80) *o++ = (char)(*p < ' ' ? ' ' : *p);
        else if (*p >= 0xC0) *o++ = '?';
    }
    *o = 0;
}

static int text_w(int w) { const int t = w >= 80 ? w - 28 : w - 2; return t > 95 ? 95 : t; }

/* Lines each held message takes at this width, kept until the ring or the width changes. */
static void layout(int w)
{
    if (C.cache_w == w && C.cache_seq == ls_chat_seq()) return;
    C.cache_w = w;
    C.cache_seq = ls_chat_seq();
    C.total = 0;
    const int n = ls_chat_count();
    for (int i = 0; i < n && i < LS_CHAT_RING; i++) {
        if (!ls_chat_get(i, &C.m)) { C.nlines[i] = 0; continue; }
        clean(C.m.text);
        int k = ls_wrap_text(C.m.text, text_w(w), C.wrap[0], sizeof(C.wrap[0]), 12);
        if (k < 1) k = 1;
        C.nlines[i] = k + (w >= 80 ? 0 : 1);
        C.total += C.nlines[i];
    }
}

static int unread(int ch)
{
    if (ch == C.ch) return 0;
    const uint32_t last = ls_chat_last_id(ch);
    if (!last) return 0;
    if (!C.seen[ch]) C.seen[ch] = last;          /* counted from when CHAT first looked */
    return last > C.seen[ch] ? (int)(last - C.seen[ch]) : 0;
}

/* CHAT's HOME tile: "3 new" while LISTEN keeps a room open in the background. */
void ls_chat_tile_tick(void)
{
    const ls_app_t *cur = ls_app_current();
    const int n = ls_chat_count();
    if (!ls_chat_background() || !ls_chat_channel()[0] || !n || (cur && !strcmp(cur->id, "chat"))) { snprintf(C.tile, sizeof(C.tile), "talk"); return; }
    if (!ls_chat_get(n - 1, &C.m)) return;
    const uint32_t s = C.seen[C.ch];
    if (s && C.m.id > s) snprintf(C.tile, sizeof(C.tile), "%lu new", (unsigned long)(C.m.id - s));
    else snprintf(C.tile, sizeof(C.tile), "listening");
}

/* ---------------------------------------------------------------- draw -- */

static void draw_tabs(tui_surface *sf, tui_rect r)
{
    static const uint8_t HUE[CH_N] = {TUI_CYAN, TUI_GREEN, TUI_YELLOW};
    tui_fill(sf, r, ' ', TUI_DEFAULT_ATTR);
    C.tab_y = r.y;
    int x = r.x;
    for (int c = 0; c < CH_N; c++) {
        char t[24];
        const int u = unread(c);
        const char *nm = c == CH_AREA ? "area" : room(c);
        if (!room(c)[0]) snprintf(t, sizeof(t), " #%s ", c == CH_CC ? "country" : "area");
        else if (u) snprintf(t, sizeof(t), " #%s %d ", nm, u > 99 ? 99 : u);
        else snprintf(t, sizeof(t), " #%s ", nm);
        const int w = (int)strlen(t);
        const bool on = c == C.ch;
        uint8_t at = on ? ls_ink(LS_INK_SELECT, TUI_ATTR(TUI_BLACK, HUE[c] | TUI_BRIGHT)) : A(HUE[c]);
        if (!room(c)[0]) at = LS_ATTR_FAINT;
        tui_put_str(sf, r, x, r.y, t, at);
        if (u && !on) tui_put_char(sf, r, x + w - 1, r.y, '*', ls_ink_hue(A(TUI_YELLOW | TUI_BRIGHT)));
        C.tab_x[c] = x;
        C.tab_w[c] = w;
        x += w + 1;
    }
}

/* Two speech bubbles and who you are. */
static void draw_head(tui_surface *sf, tui_rect r)
{
    static const char *const ART[4] = {" .-----.      ", "( hi! ) .----.", " '-.-'  ( ... )", "   '     '-.-' "};
    tui_fill(sf, r, ' ', TUI_DEFAULT_ATTR);
    const bool art = r.w >= 44;
    if (art)
        for (int y = 0; y < 4; y++)
            for (int x = 0; ART[y][x]; x++) {
                const char ch = ART[y][x];
                const bool right = x >= 8;
                uint8_t at = A(right ? TUI_MAGENTA : TUI_CYAN);
                if (ch >= 'a' && ch <= 'z') at = A(TUI_WHITE | TUI_BRIGHT);
                else if (ch == '!' || (ch == '.' && y == 2 && right && x > 9 && x < 13)) at = A(TUI_YELLOW | TUI_BRIGHT);
                tui_put_char(sf, r, r.x + 1 + x, r.y + y, ch, at);
            }
    const tui_rect in = tui_rect_make(r.x + (art ? 18 : 0), r.y, r.w - (art ? 18 : 0), r.h);
    char t[64];
    const char *h = ls_chat_handle();
    snprintf(t, sizeof(t), "#%s", ls_chat_channel()[0] ? ls_chat_channel() : room(C.ch));
    tui_put_str(sf, in, in.x, in.y, t, ls_ink(LS_INK_STRONG, A(TUI_WHITE | TUI_BRIGHT)));
    if (C.ch == CH_AREA && C.area[0]) tui_put_str(sf, in, in.x + (int)strlen(t) + 1, in.y, "(about 600 km)", LS_ATTR_DIM);
    tui_put_str(sf, in, in.x, in.y + 1, "you are", LS_ATTR_DIM);
    tui_put_str(sf, in, in.x + 8, in.y + 1, h[0] ? h : "(once you join)", h[0] ? A(handle_hue(h) | TUI_BRIGHT) : LS_ATTR_FAINT);
    tui_put_str(sf, in, in.x, in.y + 2, "anonymous, text only, no links", LS_ATTR_DIM);
    snprintf(t, sizeof(t), "%s  listen %s", ls_hub_online() ? "online" : "NO WI-FI", ls_chat_background() ? "on" : "off");
    tui_put_str(sf, in, in.x, in.y + 3, t, ls_ink_hue(A(ls_hub_online() ? TUI_GREEN : TUI_YELLOW | TUI_BRIGHT)));
}

static void draw_msgs(tui_surface *sf, tui_rect r)
{
    tui_fill(sf, r, ' ', TUI_DEFAULT_ATTR);
    C.list = r;
    const int n = ls_chat_count();
    if (!n) {
        ls_hub_status_t st;
        ls_hub_status(&st);
        if (!ls_hub_online()) ls_panel_notice(sf, r, "NO WI-FI", "Connect Wi-Fi in Settings", "to talk with everyone.");
        else if (st.state == LS_HUB_BUSY && !strcmp(st.what, "chat")) ls_panel_notice(sf, r, "JOINING", "Opening the room...", "");
        else ls_panel_notice(sf, r, "QUIET HERE", "Nobody has said anything yet.", "Say hello.");
        return;
    }
    const bool wide = r.w >= 80;
    layout(r.w);
    if (C.up > C.total - r.h) C.up = C.total - r.h;
    if (C.up < 0) C.up = 0;
    const uint32_t now = (uint32_t)time(NULL);
    const char *me = ls_chat_handle();
    int y = r.y + r.h + C.up;                   /* the row under the newest message's last line */
    for (int i = n - 1; i >= 0 && y > r.y; i--) {
        const int h = C.nlines[i];
        y -= h;
        if (y >= r.y + r.h) continue;           /* below the window: scrolled past */
        if (!ls_chat_get(i, &C.m)) continue;
        clean(C.m.text);
        const bool mine = me[0] && !strcmp(C.m.handle, me);
        const uint8_t hue = handle_hue(C.m.handle);
        char age[8];
        ls_age_str(age, sizeof(age), now > 1600000000u ? C.m.ts : 0, now);
        const int k = ls_wrap_text(C.m.text, text_w(r.w), C.wrap[0], sizeof(C.wrap[0]), 12);
        int ty = y;
        if (wide) {                             /* IRC: age, handle column, text */
            tui_put_str(sf, r, r.x, ty, age, LS_ATTR_FAINT);
            tui_put_str(sf, r, r.x + 5, ty, mine ? ">" : " ", A(TUI_WHITE | TUI_BRIGHT));
            tui_rect hc = tui_rect_make(r.x + 6, ty, 21, 1);
            tui_put_str(sf, tui_rect_intersect(hc, r), hc.x, ty, C.m.handle, A(hue | (mine ? TUI_BRIGHT : 0)));
        } else {                                /* narrow: the handle over its words */
            if (mine) tui_put_str(sf, r, r.x, ty, "you", ls_ink(LS_INK_SELECT, TUI_ATTR(TUI_BLACK, hue | TUI_BRIGHT)));
            tui_put_str(sf, r, r.x + (mine ? 4 : 0), ty, C.m.handle, A(hue | TUI_BRIGHT));
            tui_put_str(sf, r, r.x + r.w - 4, ty, age, LS_ATTR_FAINT);
            ty++;
        }
        for (int l = 0; l < k && ty < r.y + r.h; l++, ty++)
            if (ty >= r.y) tui_put_str(sf, r, wide ? r.x + 28 : r.x + 2, ty, C.wrap[l],
                                       ls_ink(LS_INK_TEXT, A(mine ? TUI_WHITE | TUI_BRIGHT : TUI_WHITE)));
    }
    /* scrolled up: how many arrived below */
    if (ls_chat_get(n - 1, &C.m)) {
        if (!C.up) C.seen[C.ch] = C.m.id;
        else if (C.m.id > C.seen[C.ch]) {
            char t[24];
            snprintf(t, sizeof(t), " v %lu new ", (unsigned long)(C.m.id - C.seen[C.ch]));
            tui_put_str(sf, r, r.x + r.w - (int)strlen(t), r.y + r.h - 1, t, ls_ink(LS_INK_SELECT, TUI_ATTR(TUI_BLACK, TUI_YELLOW | TUI_BRIGHT)));
        }
    }
}

static void draw_compose(tui_surface *sf, tui_rect r)
{
    char t[40], lines[2][96];
    snprintf(t, sizeof(t), "SAY IN #%.12s", room(C.ch)[0] ? (C.ch == CH_AREA ? "area" : room(C.ch)) : "global");
    ls_panel_box(sf, r, t, C.sending ? TUI_GREEN : TUI_CYAN);
    const int len = (int)strlen(C.draft);
    snprintf(t, sizeof(t), " %d/280 ", len);
    tui_put_str(sf, r, r.x + r.w - 1 - (int)strlen(t), r.y, t, ls_ink_hue(A(len > 260 ? TUI_YELLOW | TUI_BRIGHT : TUI_WHITE)));
    const tui_rect in = tui_rect_make(r.x + 2, r.y + 1, r.w - 4, r.h - 2);
    C.compose = r;
    if (!len) { tui_put_str(sf, in, in.x, in.y, ls_tui_keyboard_mode() ? "Type, ENTER sends." : "Tap here to type.", LS_ATTR_FAINT); return; }
    /* the end of what is being typed, where the cursor is */
    const int w = in.w, rows = in.h < 2 ? in.h : 2;
    const char *s = C.draft;
    if (len > w * rows - 1) s += len - (w * rows - 1);
    for (int i = 0; i < rows; i++) {
        snprintf(lines[i], sizeof(lines[i]), "%.*s", w, (int)strlen(s) > i * w ? s + i * w : "");
        tui_put_str(sf, in, in.x, in.y + i, lines[i], C.sending ? LS_ATTR_DIM : ls_ink(LS_INK_TEXT, A(TUI_WHITE | TUI_BRIGHT)));
    }
    const int cl = (int)strlen(s);
    if (!C.sending && cl / w < rows) tui_put_char(sf, in, in.x + cl % w, in.y + cl / w, '_', A(TUI_CYAN | TUI_BRIGHT));
}

static int buttons(ls_btn_t *b)
{
    int n = 0;
    b[n++] = (ls_btn_t){.label = "TYPE", .value = "KEYS"};
    b[n++] = (ls_btn_t){.label = "SEND", .value = C.sending ? "..." : "ENTER", .dim = !C.draft[0] || C.sending};
    snprintf(C.room_v, sizeof(C.room_v), "#%.8s", C.ch == CH_AREA ? "area" : room(C.ch));
    b[n++] = (ls_btn_t){.label = "ROOM", .value = C.room_v};
    b[n++] = (ls_btn_t){.label = "LISTEN", .value = ls_chat_background() ? "ON" : "OFF", .on = ls_chat_background()};
    return n;
}

static void draw(tui_surface *sf, tui_rect area)
{
    ls_hub_status_t st;
    ls_hub_status(&st);
    if (C.sending && esp_timer_get_time() - C.send_at > 30000000) { C.sending = false; snprintf(C.why, sizeof(C.why), "no answer from the hub"); }
    if (C.sending && st.seq != C.send_seq && !strcmp(st.what, "chat") && st.state != LS_HUB_BUSY) {
        C.sending = false;
        if (st.state == LS_HUB_OK) { C.draft[0] = 0; C.why[0] = 0; C.up = 0; }
        else snprintf(C.why, sizeof(C.why), "%s", st.text);    /* slow down, links, language: the words stay */
    }
    if ((!C.cc[0] || !C.area[0]) && esp_timer_get_time() - C.here_at > 5000000) here();
    ls_btn_t btn[4];
    const int nb = buttons(btn);
    const int bar_h = ls_btn_raised_height(area, nb);
    const tui_rect bar = tui_rect_make(area.x, area.y + area.h - bar_h, area.w, bar_h);
    const tui_rect msg = tui_rect_make(area.x, bar.y - 1, area.w, 1);
    const tui_rect comp = tui_rect_make(area.x, msg.y - 4, area.w, 4);
    const int head_h = area.h >= 40 ? 4 : 0;
    if (head_h) draw_head(sf, tui_rect_make(area.x, area.y, area.w, head_h));
    const tui_rect tabs = tui_rect_make(area.x, area.y + head_h + (head_h ? 1 : 0), area.w, 1);
    draw_tabs(sf, tabs);
    if (!head_h && ls_chat_handle()[0] && area.w >= 60) {     /* no room for the header: who you are, on the tab row */
        const char *h = ls_chat_handle();
        tui_put_str(sf, tabs, tabs.x + tabs.w - (int)strlen(h) - 1, tabs.y, h, A(handle_hue(h) | TUI_BRIGHT));
        tui_put_str(sf, tabs, tabs.x + tabs.w - (int)strlen(h) - 9, tabs.y, "you are", LS_ATTR_DIM);
    }
    tui_hline(sf, area, area.x, tabs.y + 1, area.w, '-', ls_ink(LS_INK_RULE, LS_ATTR_FAINT));
    tui_rect mr = tui_rect_make(area.x + 1, tabs.y + 2, area.w - 2, comp.y - tabs.y - 2);
    tui_rect hole;                       /* a landscape camera hole at the edge: the text wraps short of it */
    if (ls_tui_cutout(&hole) && hole.x > mr.x && hole.x < mr.x + mr.w && hole.y < mr.y + mr.h && hole.y + hole.h > mr.y)
        mr.w = hole.x - 1 - mr.x;
    draw_msgs(sf, mr);
    draw_compose(sf, comp);
    tui_fill(sf, msg, ' ', TUI_DEFAULT_ATTR);
    if (C.why[0]) {
        char t[96];
        snprintf(t, sizeof(t), "Not sent: %s", C.why);
        ls_safe_line(sf, msg, msg.y, t, ls_ink_hue(A(TUI_YELLOW | TUI_BRIGHT)));
    } else if (!strcmp(st.what, "chat") && st.state == LS_HUB_FAIL) ls_safe_line(sf, msg, msg.y, st.text, ls_ink_hue(A(TUI_YELLOW | TUI_BRIGHT)));
    else ls_safe_line(sf, msg, msg.y, "Be kind. One message every 3 seconds.", A(TUI_CYAN));
    ls_btn_bar_raised_row(sf, bar, btn, nb, -1);
}

/* --------------------------------------------------------------- input -- */

static void send(void)
{
    if (!C.draft[0] || C.sending) return;
    C.why[0] = 0;
    if (!ls_chat_send(C.draft)) { snprintf(C.why, sizeof(C.why), "%s", ls_hub_online() ? "still joining" : "no Wi-Fi"); return; }
    ls_hub_status_t st;
    ls_hub_status(&st);
    C.send_seq = st.seq;
    C.send_at = esp_timer_get_time();
    C.sending = true;
}

static void typed(const char *t) { snprintf(C.draft, sizeof(C.draft), "%s", t); }

static void type(void) { if (!C.sending) ls_keyboard_open_plain("SAY SOMETHING", C.draft, LS_CHAT_TEXT - 1, typed); }

static void room_next(int d)
{
    for (int k = 1; k <= CH_N; k++) {
        const int c = (C.ch + d * k + CH_N * 2) % CH_N;
        if (room(c)[0]) { go(c); return; }
    }
}

static void listen_toggle(void) { ls_chat_set_background(!ls_chat_background()); }

static bool key(ls_tk_t k, char ch)
{
    const int len = (int)strlen(C.draft);
    switch (k) {
    case LS_TK_UP:         C.up++; return true;
    case LS_TK_DOWN:       C.up--; return true;
    case LS_TK_WHEEL_UP:   C.up += 3; return true;
    case LS_TK_WHEEL_DOWN: C.up -= 3; return true;
    case LS_TK_PGUP:       C.up += C.list.h - 1; return true;
    case LS_TK_PGDN:       C.up -= C.list.h - 1; return true;
    case LS_TK_END:        C.up = 0; return true;
    case LS_TK_LEFT:       room_next(-1); return true;
    case LS_TK_RIGHT: case LS_TK_TAB: room_next(1); return true;
    case LS_TK_ENTER:      if (len) send(); else type(); return true;
    case LS_TK_BACKSPACE:
        if (!len || C.sending) return false;
        C.draft[len - 1] = 0;
        return true;
    case LS_TK_CHAR:
        if (C.sending || ch < ' ' || ch > '~' || len >= LS_CHAT_TEXT - 1) return true;
        C.draft[len] = ch;
        C.draft[len + 1] = 0;
        C.why[0] = 0;
        return true;
    default: return false;
    }
}

static bool touch(int col, int row)
{
    ls_btn_t btn[4];
    const int nb = buttons(btn);
    const int b = ls_btn_hit(col, row);
    if (b == 0) type();
    else if (b == 1) send();
    else if (b == 2) room_next(1);
    else if (b == 3) listen_toggle();
    if (b >= 0 && b < nb) return true;
    if (row == C.tab_y || row == C.tab_y + 1)
        for (int c = 0; c < CH_N; c++)
            if (col >= C.tab_x[c] && col < C.tab_x[c] + C.tab_w[c]) { go(c); return true; }
    if (tui_rect_contains(C.compose, col, row)) { type(); return true; }
    return tui_rect_contains(C.list, col, row);
}

const ls_tui_screen_t ls_scr_chat = {
    .name = "CHAT", .hint = "type to talk  ENTER send  left/right room  up/down scroll  END newest",
    .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch, .diff_switch = true, .wheel = true,
};

/* The tile's line under CHAT on HOME: a fixed buffer ls_chat_tile_tick rewrites. */
const char *ls_chat_tile_line(void) { if (!C.tile[0]) snprintf(C.tile, sizeof(C.tile), "talk"); return C.tile; }
