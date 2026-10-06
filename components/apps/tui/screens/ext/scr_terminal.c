/* TERMINAL: the console's commands on the board's own screen and keyboard.
   ls_term.h says where they run; this is the scrollback, the line being
   typed, and a few buttons for when there is no keyboard. */

#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_keyboard.h"
#include "../../ls_motion.h"
#include "../../ls_term.h"
#include "esp_attr.h"

#include <stdio.h>
#include <string.h>

#define A(fg, bg) TUI_ATTR(fg, bg)

EXT_RAM_BSS_ATTR static char              s_view[LS_TERM_SCROLL_BYTES + 1];
EXT_RAM_BSS_ATTR static ls_term_history_t s_hist;
EXT_RAM_BSS_ATTR static char              s_in[LS_TERM_INPUT_MAX + 1];
EXT_RAM_BSS_ATTR static char              s_draft[LS_TERM_INPUT_MAX + 1];
EXT_RAM_BSS_ATTR static char              s_last[LS_TERM_INPUT_MAX + 1];
EXT_RAM_BSS_ATTR static char              s_note[64];
static uint32_t s_view_gen;
static bool     s_view_ok;
static int      s_hist_at;       /* 0: the line being typed; n: the n-th newest */
static int      s_back;          /* rows scrolled back from the newest */
static int      s_page = 8;      /* rows one scroll moves; set by draw */
static bool     s_runner;
static bool     s_greeted;
static int      s_bar_n;
static tui_rect s_out_hit, s_in_hit;

/* ------------------------------------------------------------- the line */

static void set_input(const char *text)
{
    snprintf(s_in, sizeof(s_in), "%s", text ? text : "");
}

static void run(const char *text)
{
    char line[LS_TERM_INPUT_MAX + 1];      /* text may be s_in, cleared below */
    while (*text == ' ') text++;
    snprintf(line, sizeof(line), "%s", text);
    if (ls_term_busy()) {
        snprintf(s_note, sizeof(s_note), "still running '%.40s'", s_last);
        return;
    }
    char echo[LS_TERM_INPUT_MAX + 16];
    snprintf(echo, sizeof(echo), "%s%s\n", LS_TERM_PROMPT, line);
    s_note[0] = '\0';
    s_back = 0;
    s_hist_at = 0;
    set_input("");
    if (!*line) { ls_term_out(echo); return; }
    ls_term_history_add(&s_hist, line);
    if (!strcmp(line, "clear") || !strcmp(line, "cls")) { ls_term_clear(); return; }
    ls_term_out(echo);
    if (!s_runner) s_runner = ls_term_start();
    if (!s_runner) {
        ls_term_out("no internal memory for the command runner; close a radio app and try again\n");
        return;
    }
    snprintf(s_last, sizeof(s_last), "%s", line);
    if (!ls_term_submit(line)) ls_term_out("the command runner is not ready\n");
}

static void history(int dir)
{
    int at = s_hist_at + dir;
    if (at < 0) at = 0;
    if (at > 0 && !ls_term_history_get(&s_hist, at)) return;   /* past the oldest */
    if (s_hist_at == 0 && at > 0) snprintf(s_draft, sizeof(s_draft), "%s", s_in);
    s_hist_at = at;
    set_input(at ? ls_term_history_get(&s_hist, at) : s_draft);
}

static void complete(void)
{
    char out[LS_TERM_INPUT_MAX + 2];
    EXT_RAM_BSS_ATTR static char list[512];
    const int n = ls_term_complete(s_in, out, sizeof(out), list, sizeof(list));
    if (n == 1) {
        set_input(out);
    } else if (n > 1) {
        char echo[LS_TERM_INPUT_MAX + 16];
        snprintf(echo, sizeof(echo), "%s%s\n", LS_TERM_PROMPT, s_in);
        ls_term_out(echo);
        ls_term_out(list);
        ls_term_out("\n");
        s_back = 0;
    } else {
        snprintf(s_note, sizeof(s_note), "no command starts '%.30s'", s_in);
    }
}

static void keys_done(const char *text)
{
    set_input(text);
    run(s_in);
}

static void open_keys(void)
{
    ls_keyboard_open("COMMAND", s_in, LS_TERM_INPUT_MAX, keys_done);
}

static void scroll(int rows)
{
    s_back += rows;
    if (s_back < 0) s_back = 0;           /* draw caps the other end */
}

/* ------------------------------------------------------------- the bar */

static const char *const BAR_RUN[] = { NULL, NULL, "crumb log", "heap", NULL };

static void action(int i)
{
    switch (i) {
    case 0: ls_tui_screen_show(0); break;
    case 1: open_keys(); break;
    case 2: case 3: run(BAR_RUN[i]); break;
    case 4: ls_term_clear(); s_back = 0; break;
    default: break;
    }
}

/* The sizing rule the other pages use: three rows in a wide pane when the
   labels fit the compact form, the raised form otherwise. */
static void bar(tui_surface *sf, tui_rect *a, const ls_btn_t *btn, int n)
{
    const bool wide = a->w > a->h * 2;
    const bool thumbs = !ls_tui_keyboard_mode() && a->h >= 22;
    int h = wide ? (!thumbs && ls_btn_compact_fits(tui_rect_make(a->x, a->y, a->w, 3), btn, n) ? 3 : 4)
                 : ls_btn_raised_height(*a, n);
    if (h > a->h - 8) h = a->h > 12 ? 3 : 0;
    s_bar_n = 0;
    if (h <= 0) return;
    ls_btn_bar_raised(sf, tui_rect_make(a->x, a->y, a->w, h), btn, n, -1);
    s_bar_n = n;
    a->y += h;
    a->h -= h;
}

/* ------------------------------------------------------------ the text */

static uint8_t line_attr(const char *line)
{
    if (!strncmp(line, LS_TERM_PROMPT, sizeof(LS_TERM_PROMPT) - 1)) return A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    if (!strncmp(line, "E (", 3)) return A(TUI_RED | TUI_BRIGHT, TUI_BLACK);
    if (!strncmp(line, "W (", 3) || !strncmp(line, "unknown command", 15) || !strncmp(line, "(exit", 5))
        return A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    return A(TUI_WHITE, TUI_BLACK);
}

/* How much of a line (len bytes left) goes on a row w wide: all of it if
   it fits, else up to the last space in the row's second half, else w. */
static int wrap_take(const char *s, int len, int w)
{
    if (len <= w) return len;
    for (int sp = w; sp > w / 2; sp--)
        if (s[sp] == ' ') return sp;
    return w;
}

/* Rows of `text` wrapped at w columns: counts them all, and draws those from
   `first` into r while there is room. Returns the count. */
static int rows_of(tui_surface *sf, tui_rect r, const char *text, int w, int first)
{
    int row = 0;
    char cut[128];
    if (w > (int)sizeof(cut) - 1) w = (int)sizeof(cut) - 1;
    for (const char *p = text; *p;) {
        const char *e = strchr(p, '\n');
        const int len = e ? (int)(e - p) : (int)strlen(p);
        const uint8_t attr = sf ? line_attr(p) : 0;
        int off = 0;
        do {
            const int take = wrap_take(p + off, len - off, w);
            if (sf && row >= first && row - first < r.h) {
                snprintf(cut, sizeof(cut), "%.*s", take, p + off);
                tui_put_str(sf, r, r.x, r.y + row - first, cut, attr);
            }
            row++;
            off += take;
            if (off > 0 && off < len && p[off] == ' ') off++;   /* the space it broke at */
        } while (off < len);
        p += len + (e ? 1 : 0);
    }
    return row;
}

/* ----------------------------------------------------------- the screen */

static void enter(void)
{
    s_runner = ls_term_start();
    if (!s_greeted) {
        s_greeted = true;
        ls_term_out("TERMINAL: the console's commands, run here. 'help' lists them.\n");
    }
    s_note[0] = '\0';
}

static void leave(void)
{
    ls_term_stop();
    s_runner = false;
}

static void draw(tui_surface *sf, tui_rect a)
{
    s_out_hit = s_in_hit = tui_rect_make(0, 0, 0, 0);
    if (a.w < 24 || a.h < 10) {
        s_bar_n = 0;
        ls_panel_notice(sf, a, "TERMINAL", "Enlarge the pane", "A running command keeps going");
        return;
    }
    const bool busy = ls_term_busy();
    const ls_btn_t btn[] = {
        { "BACK", "HOME", 0, false, false },
        { "KEYS", "type", 0, false, false },
        { "ERRORS", "crumb log", 0, false, busy },
        { "HEAP", "memory", 0, false, busy },
        { "CLEAR", "screen", 0, false, false },
    };
    bar(sf, &a, btn, 5);

    tui_rect body = tui_rect_make(a.x, a.y, a.w, a.h - 1);
    ls_panel_box(sf, body, "TERMINAL", TUI_GREEN);
    ls_motion_busy(sf, body, busy);
    tui_rect in = tui_rect_make(body.x + 2, body.y + 1, body.w - 4, body.h - 2);
    if (in.w < 8 || in.h < 2) return;

    if (!s_view_ok || ls_term_gen() != s_view_gen) {
        s_view_gen = ls_term_gen();
        ls_term_text(s_view, sizeof(s_view));
        s_view_ok = true;
    }
    tui_rect out = tui_rect_make(in.x, in.y, in.w, in.h - 1);
    s_page = out.h > 2 ? out.h - 2 : 1;
    const int total = rows_of(NULL, out, s_view, out.w, 0);
    const int most = total > out.h ? total - out.h : 0;
    if (s_back > most) s_back = most;
    rows_of(sf, out, s_view, out.w, most - s_back);
    s_out_hit = out;
    if (s_back) {
        char mark[24];
        snprintf(mark, sizeof(mark), " back %d ", s_back);
        tui_put_str(sf, body, body.x + body.w - 2 - (int)strlen(mark), body.y, mark,
                    A(TUI_BLACK, TUI_YELLOW));
    }

    /* the line being typed, or what is running */
    const int y = in.y + in.h - 1;
    s_in_hit = tui_rect_make(in.x, y, in.w, 1);
    if (busy) {
        char run_line[LS_TERM_INPUT_MAX + 24];
        snprintf(run_line, sizeof(run_line), "%c running '%s'", ls_motion_pip(true), s_last);
        tui_put_str(sf, in, in.x, y, run_line, A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    } else {
        tui_put_str(sf, in, in.x, y, LS_TERM_PROMPT, A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
        const int px = in.x + (int)sizeof(LS_TERM_PROMPT) - 1;
        const int room = in.x + in.w - 1 - px;
        const int len = (int)strlen(s_in);
        const char *shown = len > room && room > 0 ? s_in + (len - room) : s_in;
        tui_put_str(sf, in, px, y, shown, A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
        const int cx = px + (int)strlen(shown);
        if (cx < in.x + in.w)
            tui_put_char(sf, in, cx, y, LS_TUI_BLOCK_FULL, A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    }

    /* the keys are on the hint row already; this line is for news */
    const char *foot = s_note[0] ? s_note
                     : !s_runner ? "no command runner: not enough internal memory"
                     : ls_tui_keyboard_mode() ? ""
                     : "KEYS to type; tap the text to scroll";
    ls_safe_line(sf, a, a.y + a.h - 1, foot, s_note[0] || !s_runner ? A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
}

static bool key(ls_tk_t k, char ch)
{
    const int len = (int)strlen(s_in);
    switch (k) {
    case LS_TK_CHAR:
        if ((unsigned char)ch >= 0x20 && (unsigned char)ch < 0x7F && len < LS_TERM_INPUT_MAX) {
            s_in[len] = ch;
            s_in[len + 1] = '\0';
            s_hist_at = 0;
            s_note[0] = '\0';
        }
        return true;
    case LS_TK_BACKSPACE:
        if (len) s_in[len - 1] = '\0';
        return true;
    case LS_TK_ENTER: run(s_in); return true;
    case LS_TK_ESC:
        if (len) { set_input(""); s_hist_at = 0; return true; }
        return false;                                   /* HOME */
    case LS_TK_UP:    history(1);  return true;
    case LS_TK_DOWN:  history(-1); return true;
    case LS_TK_LEFT:  scroll(s_page); return true;
    case LS_TK_RIGHT: scroll(-s_page); return true;
    case LS_TK_TAB:   complete(); return true;
    default: return false;
    }
}

static bool touch(int x, int y)
{
    const int i = ls_btn_hit(x, y);
    if (i >= 0 && i < s_bar_n) { action(i); return true; }
    if (s_in_hit.w > 0 && tui_rect_contains(s_in_hit, x, y)) { open_keys(); return true; }
    if (s_out_hit.w > 0 && tui_rect_contains(s_out_hit, x, y)) {
        scroll(y < s_out_hit.y + s_out_hit.h / 2 ? s_page : -s_page);
        return true;
    }
    return true;           /* a tap anywhere else is not ENTER */
}

const ls_tui_screen_t ls_scr_terminal = {
    .name  = "TERMINAL",
    .hint  = "ENTER run  UP/DOWN history  TAB complete  LEFT/RIGHT scroll  ESC home",
    .enter = enter,
    .leave = leave,
    .draw  = draw,
    .key   = key,
    .touch = touch,
};
