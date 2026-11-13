/* BOOT LOG: what this boot logged from its first line, and the boot before
   (from the SD card), to read and to send. A board that came up with a
   black screen leaves its log on the card; the next boot shows it here.
   Errors red, warnings yellow; the strip on the right is the whole log,
   marked where the errors and warnings are. Nothing moves unless the log
   grew. All of this screen's state is in PSRAM. */
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_drag_scroll.h"
#include "../../desk/ls_desk_host.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "ls_bootlog.h"

#define LINES_MAX 4096
#define GUTTER 5

EXT_RAM_BSS_ATTR static struct {
    int tab;                            /* 0 this boot, 1 the boot before */
    int top, col, rows;
    int n, n_err, n_warn;
    size_t len;
    uint32_t seen_seq;
    int64_t read_us;
    int pc_id;                          /* a save on the PC under way */
    char msg[72];
    uint8_t msg_attr;
    ls_drag_scroll_t drag;
    tui_rect body;
    int tab_x[2], tab_w[2], tab_y;
    uint16_t line[LINES_MAX];
    char lvl[LINES_MAX];
    char text[LS_BOOTLOG_TEXT_MAX + 1];
} S;

static void say(const char *text, uint8_t hue)
{
    snprintf(S.msg, sizeof(S.msg), "%s", text);
    S.msg_attr = TUI_ATTR(hue, TUI_BLACK);
}

/* 'E' 'W' 'I' 'D' 'V' for a log line, 'H' for the lines this firmware puts
   on top, 'M' for a note about lines left out, 0 for anything else. */
static char level_of(const char *l)
{
    if (l[0] && l[1] == ' ' && l[2] == '(' && strchr("EWIDV", l[0])) return l[0];
    if (!strncmp(l, "reset: ", 7)) return l[7] >= 'A' && l[7] <= 'Z' ? 'E' : 'H';
    if (!strncmp(l, "LakeShark", 9)) return 'H';
    if (!strncmp(l, "... ", 4) || l[0] == '(') return 'M';
    return 0;
}

/* Cuts S.text into lines in place. Running it again is harmless. */
static void index_text(void)
{
    S.n = S.n_err = S.n_warn = 0;
    size_t i = 0;
    while (i < S.len && S.n < LINES_MAX) {
        size_t j = i;
        for (; j < S.len && S.text[j] != '\n' && S.text[j]; j++) {
            const unsigned char c = (unsigned char)S.text[j];
            if (c < 0x20) S.text[j] = ' ';
            else if (c >= 0x7f) S.text[j] = '?';
        }
        S.text[j] = '\0';
        S.line[S.n] = (uint16_t)i;
        const char lv = level_of(S.text + i);
        S.lvl[S.n] = lv;
        if (lv == 'E') S.n_err++;
        if (lv == 'W') S.n_warn++;
        S.n++;
        i = j + 1;
    }
}

static int last_top(void) { return S.n > S.rows ? S.n - S.rows : 0; }

static void clamp(void)
{
    if (S.top > last_top()) S.top = last_top();
    if (S.top < 0) S.top = 0;
    if (S.col < 0) S.col = 0;
    if (S.col > 200) S.col = 200;
}

static void load(bool to_end)
{
    if (S.tab == 0) {
        S.seen_seq = ls_bootlog_seq();
        S.len = ls_bootlog_text(S.text, sizeof(S.text));
    } else {
        S.len = ls_bootlog_load(LS_BOOTLOG_DIR, true, S.text, sizeof(S.text));
    }
    S.read_us = esp_timer_get_time();
    index_text();
    if (to_end) S.top = last_top();
    clamp();
}

static void set_tab(int tab)
{
    if (tab == S.tab) return;
    S.tab = tab;
    S.top = S.col = 0;
    S.msg[0] = '\0';
    load(false);
}

static void enter(void)
{
    if (S.rows < 1) S.rows = 10;
    S.msg[0] = '\0';
    load(false);
}

static void pc_drop(void)
{
    if (!S.pc_id) return;
    ls_desk_host_cancel(S.pc_id);
    ls_desk_host_status(S.pc_id, 1);
    S.pc_id = 0;
}

static void leave(void) { pc_drop(); }

static bool pc_can_save(void) { return (ls_desk_host_features(NULL) & LS_DESK_SAVE) != 0; }

static void save_pc(void)
{
    if (!S.len) { say("Nothing to save.", TUI_YELLOW | TUI_BRIGHT); return; }
    pc_drop();
    const int id = ls_desk_host_begin(LS_DESK_SAVE, S.tab ? LS_BOOTLOG_PREV : LS_BOOTLOG_FILE);
    if (id <= 0) { say("The PC window cannot save right now.", TUI_YELLOW | TUI_BRIGHT); return; }
    for (size_t k = 0; k < S.len; k++) if (!S.text[k]) S.text[k] = '\n';
    const int e = ls_desk_host_payload(id, S.text, S.len);
    index_text();
    S.pc_id = id;
    if (e) { pc_drop(); say("Could not hand the log to the PC.", TUI_RED | TUI_BRIGHT); return; }
    say("Pick where to save it on the PC...", TUI_CYAN | TUI_BRIGHT);
}

static void save_card(void)
{
    if (S.tab) {
        say(S.len ? "Already on the card: lakeshark/" LS_BOOTLOG_PREV : "No SD card, or no log of the boot before.",
            S.len ? TUI_CYAN | TUI_BRIGHT : TUI_YELLOW | TUI_BRIGHT);
        return;
    }
    if (ls_bootlog_save(LS_BOOTLOG_DIR)) say("Saved to the card: lakeshark/" LS_BOOTLOG_FILE, TUI_GREEN | TUI_BRIGHT);
    else say("Not saved: no SD card?", TUI_RED | TUI_BRIGHT);
}

static void next_problem(void)
{
    for (int k = 1; k <= S.n; k++) {
        const int i = (S.top + k) % S.n;
        if (S.lvl[i] == 'E' || S.lvl[i] == 'W') { S.top = i; clamp(); return; }
    }
    say("No errors or warnings in this log.", TUI_GREEN | TUI_BRIGHT);
}

static int buttons(ls_btn_t *b)
{
    int n = 0;
    b[n++] = (ls_btn_t){.label = "T BOOT", .value = S.tab ? "THE LAST" : "THIS ONE", .key = 't'};
    b[n++] = (ls_btn_t){.label = "E NEXT", .value = "ERR/WARN", .key = 'e', .dim = !(S.n_err + S.n_warn)};
    b[n++] = (ls_btn_t){.label = "S SAVE", .value = "TO CARD", .key = 's'};
    if (pc_can_save()) b[n++] = (ls_btn_t){.label = "P SAVE", .value = "TO PC", .key = 'p', .on = S.pc_id != 0};
    return n;
}

static bool act(char key)
{
    switch (key) {
    case 't': set_tab(!S.tab); return true;
    case 'e': next_problem(); return true;
    case 's': save_card(); return true;
    case 'p': if (pc_can_save()) save_pc(); return true;
    default:  return false;
    }
}

/* ---------------------------------------------------------------- draw -- */

static uint8_t line_attr(char lv)
{
    switch (lv) {
    case 'E': return TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK);
    case 'W': return TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    case 'H': return TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    case 'M': return TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK);
    case 'D': case 'V': return LS_ATTR_DIM;
    case 'I': return TUI_ATTR(TUI_GREEN, TUI_BLACK);
    default:  return TUI_ATTR(TUI_WHITE, TUI_BLACK);
    }
}

static void draw_head(tui_surface *sf, tui_rect r)
{
    static const char *const NAME[2] = {" THIS BOOT ", " THE BOOT BEFORE "};
    tui_fill(sf, r, ' ', LS_ATTR_DIM);
    int x = r.x;
    S.tab_y = r.y;
    for (int t = 0; t < 2; t++) {
        const bool sel = t == S.tab;
        tui_put_char(sf, r, x, r.y, sel ? '[' : ' ', TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        tui_put_str(sf, r, x + 1, r.y, NAME[t],
                    sel ? TUI_ATTR(TUI_BLACK, TUI_CYAN) : TUI_ATTR(TUI_CYAN, TUI_BLACK));
        const int w = (int)strlen(NAME[t]) + 2;
        tui_put_char(sf, r, x + w - 1, r.y, sel ? ']' : ' ', TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        S.tab_x[t] = x; S.tab_w[t] = w;
        x += w;
    }
    char t[48];
    int k = snprintf(t, sizeof(t), "%d line%s", S.n, S.n == 1 ? "" : "s");
    int rx = r.x + r.w - k - 1;
    if (rx > x) tui_put_str(sf, r, rx, r.y, t, LS_ATTR_DIM);
    k = snprintf(t, sizeof(t), "W %d", S.n_warn);
    rx -= k + 2;
    if (rx > x) tui_put_str(sf, r, rx, r.y, t, S.n_warn ? line_attr('W') : LS_ATTR_FAINT);
    k = snprintf(t, sizeof(t), "E %d", S.n_err);
    rx -= k + 2;
    if (rx > x) tui_put_str(sf, r, rx, r.y, t, S.n_err ? line_attr('E') : LS_ATTR_FAINT);
}

/* The whole log in one column: a row per slice of it, '!' where the slice
   holds an error, '*' a warning; the rows on screen are the lit ones. */
static void draw_strip(tui_surface *sf, tui_rect r)
{
    for (int y = 0; y < r.h; y++) {
        const int a = (int)((long)y * S.n / r.h), b = (int)((long)(y + 1) * S.n / r.h);
        char worst = 0;
        for (int i = a; i < b && worst != 'E'; i++)
            if (S.lvl[i] == 'E' || S.lvl[i] == 'W') worst = S.lvl[i];
        const bool in_view = S.n > 0 && a < S.top + S.rows && (b > S.top || a == S.top);
        const uint8_t bg = in_view ? TUI_BLUE : TUI_BLACK;
        const uint8_t fg = worst == 'E' ? (TUI_RED | TUI_BRIGHT) : worst == 'W' ? (TUI_YELLOW | TUI_BRIGHT)
                         : in_view ? (TUI_WHITE | TUI_BRIGHT) : LS_FAINT_FG;
        tui_put_char(sf, r, r.x, r.y + y, worst == 'E' ? '!' : worst == 'W' ? '*' : in_view ? '|' : ':',
                     TUI_ATTR(fg, bg));
    }
}

static void draw(tui_surface *sf, tui_rect area)
{
    /* this boot's log is live: read it again when it grew, at most twice a second */
    if (S.tab == 0 && ls_bootlog_seq() != S.seen_seq && esp_timer_get_time() - S.read_us > 500000)
        load(S.top >= last_top());
    if (S.pc_id) {
        const int r = ls_desk_host_status(S.pc_id, 1);
        if (r) {
            S.pc_id = 0;
            if (r == 1) say("Saved on the PC. Attach the file to a GitHub issue.", TUI_GREEN | TUI_BRIGHT);
            else say("Not saved on the PC.", TUI_YELLOW | TUI_BRIGHT);
        }
    }

    ls_btn_t btn[4];
    const int nb = buttons(btn);
    const int bar_h = ls_btn_raised_height(area, nb);
    const tui_rect bar = tui_rect_make(area.x, area.y + area.h - bar_h, area.w, bar_h);
    const tui_rect msg = tui_rect_make(area.x, bar.y - 1, area.w, 1);
    const tui_rect head = tui_rect_make(area.x, area.y, area.w, 1);
    const tui_rect body = tui_rect_make(area.x, area.y + 1, area.w - 1, area.h - bar_h - 2);
    const tui_rect strip = tui_rect_make(area.x + area.w - 1, body.y, 1, body.h);
    S.body = body;
    S.rows = body.h > 0 ? body.h : 1;
    ls_drag_scroll(&S.drag, body, 1, &S.top, S.n, S.rows);
    clamp();

    draw_head(sf, head);
    tui_fill(sf, body, ' ', TUI_DEFAULT_ATTR);
    if (!S.n) {
        if (S.tab)
            ls_panel_notice(sf, body, "THE BOOT BEFORE", "No log of it on the SD card.",
                            "With a card in, every boot leaves one.");
        else
            ls_panel_notice(sf, body, "THIS BOOT", "Nothing logged yet.", "");
    } else {
        const int gut = body.w >= 70 ? GUTTER : 0;
        const tui_rect text = tui_rect_make(body.x + gut, body.y, body.w - gut, body.h);
        for (int r = 0; r < S.rows && S.top + r < S.n; r++) {
            const int i = S.top + r;
            if (gut) {
                char num[12];
                snprintf(num, sizeof(num), "%4d", i + 1);
                tui_put_str(sf, body, body.x, body.y + r, num, TUI_ATTR(TUI_CYAN, TUI_BLACK));
            }
            const char *l = S.text + S.line[i];
            if ((int)strlen(l) > S.col) tui_put_str(sf, text, text.x, text.y + r, l + S.col, line_attr(S.lvl[i]));
            else if (S.col && *l) tui_put_char(sf, text, text.x, text.y + r, '<', LS_ATTR_FAINT);
        }
    }
    draw_strip(sf, strip);

    tui_fill(sf, msg, ' ', LS_ATTR_DIM);
    if (S.msg[0]) ls_safe_line(sf, msg, msg.y, S.msg, S.msg_attr);
    else ls_safe_line(sf, msg, msg.y, "Send: SAVE, attach bootlog.txt to a GitHub issue.",
                      TUI_ATTR(TUI_CYAN, TUI_BLACK));
    ls_btn_bar_raised_row(sf, bar, btn, nb, -1);
}

/* --------------------------------------------------------------- input -- */

static bool key(ls_tk_t k, char ch)
{
    switch (k) {
    case LS_TK_UP:         S.top--; break;
    case LS_TK_DOWN:       S.top++; break;
    case LS_TK_WHEEL_UP:   S.top -= 3; break;
    case LS_TK_WHEEL_DOWN: S.top += 3; break;
    case LS_TK_PGUP:       S.top -= S.rows - 1; break;
    case LS_TK_PGDN:       S.top += S.rows - 1; break;
    case LS_TK_HOME:       S.top = 0; S.col = 0; break;
    case LS_TK_END:        S.top = last_top(); break;
    case LS_TK_LEFT:       S.col -= 8; break;
    case LS_TK_RIGHT:      S.col += 8; break;
    case LS_TK_TAB:        set_tab(!S.tab); return true;
    case LS_TK_CHAR:
        if (ch == '1' || ch == '2') { set_tab(ch - '1'); return true; }
        if (ch == ' ') { S.top += S.rows - 1; break; }
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        return act(ch);
    default:
        return false;
    }
    clamp();
    return true;
}

static bool touch(int col, int row)
{
    if (ls_drag_scroll_tap_is_lift(&S.drag)) return true;
    ls_btn_t btn[4];
    const int nb = buttons(btn);
    const int b = ls_btn_hit(col, row);
    if (b >= 0 && b < nb) return act(btn[b].key);
    if (row == S.tab_y)
        for (int t = 0; t < 2; t++)
            if (col >= S.tab_x[t] && col < S.tab_x[t] + S.tab_w[t]) { set_tab(t); return true; }
    return false;
}

const ls_tui_screen_t ls_scr_bootlog = {
    .name = "BOOT LOG", .hint = "arrows PgUp PgDn scroll  t this/last boot  e next error  s save",
    .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch,
    .diff_switch = true, .wheel = true,
};
