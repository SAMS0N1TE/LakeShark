/* EXPERIMENTS: the list of experiments (ls_experiments.h), and a page for
   each with START/STOP, RADIO, OPTIONS and its live readout. */
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_radio_select.h"
#include "../../ls_options.h"
#include "../../ls_motion.h"
#include "../../ls_experiments.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>

#define LIVE_MAX 24

/* -1 for the list, else the experiment open. */
static int s_open = -1;
static int s_sel;
static int s_focus = -1, s_slot;
/* What the bar drawn last holds, for keys and taps. */
static char s_bar_keys[6];
static bool s_bar_dim[6];
static int s_bar_n;
/* The list's rows as drawn, for taps: two cells each. */
static tui_rect s_rows_hit;
static int s_rows_first;
/* OPTIONS keeps a pointer to its context while its list is up. */
static ls_opt_ctx_t s_ctx;
static EXT_RAM_BSS_ATTR char s_live[LIVE_MAX][LS_EXP_LINE];
static EXT_RAM_BSS_ATTR char s_feedback[64];
static EXT_RAM_BSS_ATTR char s_hint[64];

static const ls_experiment_t *opened(void) { return s_open >= 0 ? ls_exp_at(s_open) : NULL; }

static const ls_opt_ctx_t *options(const ls_experiment_t *e)
{
    /* Under OPTIONS on the button: the console name, which is short, in
       the button's capitals. */
    static char tag[12];
    if (!e || !e->opts || e->n_opts <= 0) return NULL;
    size_t i = 0;
    for (; e->id[i] && i + 1 < sizeof(tag); i++)
        tag[i] = e->id[i] >= 'a' && e->id[i] <= 'z' ? (char)(e->id[i] - 'a' + 'A') : e->id[i];
    tag[i] = 0;
    s_ctx = (ls_opt_ctx_t){ .name = e->name, .job = e->no_radio ? -1 : LS_RSEL_EXPERIMENT,
                            .radio = LS_RSEL_NONE, .opt = e->opts, .n = e->n_opts, .tag = tag };
    return &s_ctx;
}

static uint8_t badge_attr(ls_exp_maturity_t m)
{
    switch (m) {
    case LS_EXP_WORKS:  return TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    case LS_EXP_TRYING: return TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    default:            return LS_ATTR_DIM;
    }
}

static void open_page(int i)
{
    if (!ls_exp_at(i)) return;
    s_open = i;
    s_focus = -1;
    s_feedback[0] = 0;
}

static void close_page(void)
{
    s_sel = s_open >= 0 ? s_open : s_sel;
    s_open = -1;
    s_focus = -1;
    s_feedback[0] = 0;
}

/* Running, or about to be: what START/STOP shows and does. */
static bool is_running_or_starting(const ls_experiment_t *e)
{
    return ls_exp_busy() ? ls_exp_wanted() == e : ls_exp_running() == e;
}

static void action(char k)
{
    const ls_experiment_t *e = opened();
    for (int i = 0; i < s_bar_n; i++)
        if (s_bar_keys[i] == k && s_bar_dim[i]) {
            if (k == 'r') snprintf(s_feedback, sizeof(s_feedback), "Stop it to change the radio");
            return;
        }
    s_feedback[0] = 0;
    switch (k) {
    case 'b':
        if (e) close_page();
        else ls_tui_screen_show(0);
        break;
    case 'x':
        ls_exp_stop();
        break;
    case 's':
        if (!e) break;
        if (is_running_or_starting(e)) ls_exp_stop();
        else ls_exp_start(e);
        break;
    case 'r':
        if (e && !e->no_radio) ls_rsel_open(LS_RSEL_EXPERIMENT, NULL);
        break;
    case 'o':
        ls_opt_open(options(e));
        break;
    default:
        break;
    }
}

/* The bar along the top: BACK first, then the page's own controls. The
   sizing rule SUB-GHZ pages use: three rows in a wide pane when the labels
   fit the compact form, the raised form otherwise. */
static void page_bar(tui_surface *sf, tui_rect *a, const ls_btn_t *btn, int n)
{
    const bool wide = a->w > a->h * 2;
    const bool thumbs = !ls_tui_keyboard_mode() && a->h >= 22;
    int h = wide ? (!thumbs && ls_btn_compact_fits(tui_rect_make(a->x, a->y, a->w, 3), btn, n) ? 3 : 4)
                 : ls_btn_raised_height(*a, n);
    if (h > a->h - 6) h = a->h > 9 ? 3 : 0;
    s_bar_n = 0;
    if (h <= 0) return;
    ls_btn_bar_raised(sf, tui_rect_make(a->x, a->y, a->w, h), btn, n, s_focus);
    s_bar_n = n < (int)sizeof(s_bar_keys) ? n : (int)sizeof(s_bar_keys);
    for (int i = 0; i < s_bar_n; i++) { s_bar_keys[i] = btn[i].key; s_bar_dim[i] = btn[i].dim; }
    a->y += h;
    a->h -= h;
}

/* `text` across up to `rows` lines of `r`, broken at spaces. Returns the
   lines used. */
static int wrap(tui_surface *sf, tui_rect r, int y, int rows, const char *text, uint8_t attr)
{
    const int w = r.w;
    int used = 0;
    while (*text && used < rows && w > 0) {
        int n = (int)strlen(text);
        if (n > w) {
            n = w;
            while (n > 0 && text[n] != ' ') n--;
            if (n == 0) n = w;
        }
        char line[LS_EXP_LINE * 2];
        if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
        memcpy(line, text, (size_t)n);
        line[n] = 0;
        tui_put_str(sf, r, r.x, y + used, line, attr);
        used++;
        text += n;
        while (*text == ' ') text++;
    }
    return used;
}

/* ------------------------------------------------------------------ list */

static void draw_list(tui_surface *sf, tui_rect a)
{
    const ls_experiment_t *run = ls_exp_running();
    const ls_btn_t btn[] = {
        { "BACK", "HOME", 'b', false, false },
        { "STOP", run ? run->name : "NOTHING ON", 'x', run != NULL, run == NULL },
    };
    page_bar(sf, &a, btn, 2);
    const int count = ls_exp_count();
    if (s_sel >= count) s_sel = count ? count - 1 : 0;
    tui_rect body = tui_rect_make(a.x, a.y, a.w, a.h - 1);
    ls_panel_box(sf, body, "EXPERIMENTS", TUI_CYAN);
    ls_motion_busy(sf, body, run != NULL);
    tui_rect list = tui_rect_make(body.x + 2, body.y + 1, body.w - 4, body.h - 2);
    s_rows_hit = tui_rect_make(0, 0, 0, 0);
    if (list.w < 4 || list.h < 2) return;
    if (!count) {
        tui_put_str(sf, list, list.x, list.y, "No experiments in this build", LS_ATTR_DIM);
    } else {
        int rows = list.h / 2;
        if (rows < 1) rows = 1;
        const int first = s_sel / rows * rows;
        s_rows_hit = list;
        s_rows_first = first;
        for (int i = 0; i < rows && first + i < count; i++) {
            const ls_experiment_t *e = ls_exp_at(first + i);
            const int y = list.y + i * 2;
            if (first + i == s_sel) ls_fill_dither(sf, tui_rect_make(list.x, y, list.w, 2), LS_DITHER_LIGHT, TUI_CYAN);
            char badge[10];
            snprintf(badge, sizeof(badge), "%-6s", ls_exp_maturity_name(e->maturity));
            tui_put_str(sf, list, list.x, y, badge, badge_attr(e->maturity));
            tui_put_str(sf, list, list.x + 7, y, e->name, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
            if (e == run) {
                char on[8];
                snprintf(on, sizeof(on), "%c ON", ls_motion_pip(true));
                tui_put_str(sf, list, list.x + list.w - 4, y, on, TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
            }
            if (e->lr2021_only && list.w > 40)
                tui_put_str(sf, list, list.x + list.w - 17, y, "LR2021 only", LS_ATTR_DIM);
            tui_put_str(sf, list, list.x + 7, y + 1, e->sub ? e->sub : "", LS_ATTR_DIM);
        }
    }
    ls_safe_line(sf, a, a.y + a.h - 1, s_feedback[0] ? s_feedback : "UP/DOWN pick, ENTER opens", LS_ATTR_DIM);
}

/* ------------------------------------------------------------------ page */

static void draw_page(tui_surface *sf, tui_rect a, const ls_experiment_t *e)
{
    const bool running = ls_exp_running() == e;
    const bool live = is_running_or_starting(e);
    const ls_opt_ctx_t *ctx = options(e);
    ls_btn_t btn[4];
    int n = 0;
    btn[n++] = (ls_btn_t){ "BACK", "LIST", 'b', false, false };
    btn[n++] = (ls_btn_t){ live ? "STOP" : "START", running ? "RUNNING" : live ? "WAIT" : "STOPPED", 's', live, false };
    if (!e->no_radio) {
        btn[n] = ls_rsel_button(LS_RSEL_EXPERIMENT);
        btn[n++].dim = live;
    }
    if (ls_opt_count(ctx)) btn[n++] = ls_opt_button(ctx);
    page_bar(sf, &a, btn, n);

    tui_rect body = tui_rect_make(a.x, a.y, a.w, a.h - 1);
    char title[48];
    snprintf(title, sizeof(title), "%s / %s", e->name, ls_exp_maturity_name(e->maturity));
    ls_panel_box(sf, body, title, TUI_CYAN);
    ls_motion_busy(sf, body, running);
    tui_rect in = tui_rect_make(body.x + 2, body.y + 1, body.w - 4, body.h - 2);
    if (in.w < 4 || in.h < 2) return;

    int y = in.y;
    const int last = in.y + in.h - 1;    /* the status line's row */
    y += wrap(sf, in, y, last - y > 3 ? 2 : 1, e->sub ? e->sub : "", LS_ATTR_DIM);
    if (e->needs && y < last - 1) {
        char needs[160];
        snprintf(needs, sizeof(needs), "NEEDS %s", e->needs);
        y += wrap(sf, in, y, 2, needs, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    }
    if (y < last - 1) y++;

    int room = last - y - 1;
    if (room > LIVE_MAX) room = LIVE_MAX;
    const int got = ls_exp_read_lines(e, s_live, room);
    const uint8_t ink = running ? TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM;
    for (int i = 0; i < got; i++) tui_put_str(sf, in, in.x, y + i, s_live[i], ink);

    char state[80];
    ls_exp_state_line(e, state, sizeof(state));
    const bool failed = !live && strcmp(state, "stopped") != 0;
    const uint8_t sattr = running ? TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK)
                        : failed  ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)
                                  : LS_ATTR_DIM;
    char status[96];
    snprintf(status, sizeof(status), "%c %s", ls_motion_pip(running), state);
    tui_put_str(sf, in, in.x, last, status, sattr);

    const char *keys = e->no_radio ? "S start/stop  O options  ESC back" : "S start/stop  R radio  O options  ESC back";
    ls_safe_line(sf, a, a.y + a.h - 1, s_feedback[0] ? s_feedback : ls_tui_keyboard_mode() ? keys : "", LS_ATTR_DIM);
}

/* ------------------------------------------------------------ the screen */

static void enter(void)
{
    ls_exp_register_builtin();
    s_focus = -1;
    s_slot = 0;
    s_feedback[0] = 0;
    /* Coming back to a running experiment lands on its page. */
    const ls_experiment_t *run = ls_exp_running();
    s_open = -1;
    for (int i = 0; run && i < ls_exp_count(); i++)
        if (ls_exp_at(i) == run) s_open = i;
}

static void draw(tui_surface *sf, tui_rect a)
{
    const ls_experiment_t *e = opened();
    snprintf(s_hint, sizeof(s_hint), "%s", e ? "S start/stop  R radio  O options  ESC back"
                                             : "ENTER open  X stop  ESC home");
    if (a.w < 24 || a.h < 12) {
        s_bar_n = 0;
        s_rows_hit = tui_rect_make(0, 0, 0, 0);
        ls_panel_notice(sf, a, "EXPERIMENTS", "Enlarge the pane", "A running one keeps going");
        return;
    }
    if (e) draw_page(sf, a, e);
    else draw_list(sf, a);
}

static bool key(ls_tk_t k, char ch)
{
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    const ls_experiment_t *e = opened();
    if (k == LS_TK_TAB || k == LS_TK_LEFT || k == LS_TK_RIGHT)
        return ls_btn_navigate(k, &s_slot, &s_focus, false);
    if (k == LS_TK_ENTER && s_focus >= 0 && s_focus < s_bar_n) {
        action(s_bar_keys[s_focus]);
        return true;
    }
    if (e) {
        if (k == LS_TK_ESC || k == LS_TK_BACKSPACE) { close_page(); return true; }
        if (k == LS_TK_ENTER) { action('s'); return true; }
        if (k != LS_TK_CHAR || !ch) return false;
        if (ls_opt_key(options(e), ch)) return true;
        if (strchr("bsr", ch)) { action(ch); return true; }
        return false;
    }
    const int count = ls_exp_count();
    if (k == LS_TK_UP || k == LS_TK_DOWN) s_focus = -1;
    if (k == LS_TK_UP)   { if (s_sel > 0) s_sel--; return true; }
    if (k == LS_TK_DOWN) { if (s_sel + 1 < count) s_sel++; return true; }
    if (k == LS_TK_ENTER) { open_page(s_sel); return true; }
    if (k != LS_TK_CHAR || !ch) return false;
    if (ch == 'b' || ch == 'x') { action(ch); return true; }
    return false;
}

static bool touch(int x, int y)
{
    const int i = ls_btn_hit(x, y);
    if (i >= 0 && i < s_bar_n) { action(s_bar_keys[i]); return true; }
    if (s_open < 0 && s_rows_hit.w > 0 && tui_rect_contains(s_rows_hit, x, y)) {
        const int row = s_rows_first + (y - s_rows_hit.y) / 2;
        /* First tap selects, a tap on the selected one opens it. */
        if (row < ls_exp_count()) {
            if (row == s_sel) open_page(row);
            else s_sel = row;
        }
    }
    return true;
}

const ls_tui_screen_t ls_scr_experiments = {
    .name = "EXPERIMENTS", .hint = s_hint, .enter = enter, .draw = draw, .key = key, .touch = touch,
};
