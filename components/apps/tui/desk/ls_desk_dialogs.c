/* The shell's small windows: a message, a line of text asked for (Rename,
   Run), New Shortcut's list of apps, the folder window, the Recycle Bin,
   the Task Manager and an item's Properties. One of each at most; each is
   drawn by the desk, so it is always live.

   Lists scroll with the arrows, the wheel and Page Up/Down; Enter does the
   default button, Esc closes. Nothing here moves unless the thing it shows
   changed. */

#include "ls_desk_priv.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "../ls_braille.h"
#include "../ls_tui_screen.h"

/* A picture's colour and a row's hue on the face, made to read in the palette in use. */
static uint8_t on_face(uint8_t fg) { return dk_fit(fg, dk_face_bg(), 30); }
static uint8_t hue_on_face(uint8_t hue) { return dk_hue(hue); }

/* Buttons sit in one row bottom right, numbered from the right edge: a row
   high, as wide as a mouse wants. Tab walks them (focus 0 is the window's
   list or field, 1.. the buttons left to right), Alt+letter presses one. */
#define BTN_W 12
static tui_rect btn(tui_rect c, int from_right) { return tui_rect_make(c.x + c.w - 1 - (BTN_W + 1) * (from_right + 1), c.y + c.h - 2, BTN_W, 1); }

enum { BC_CLOSE = 1, BC_OK, BC_PICK_ADD, BC_FOLD_OPEN, BC_FOLD_UP, BC_FOLD_DESK, BC_BIN_RESTORE, BC_BIN_DELETE,
       BC_BIN_EMPTY, BC_TASK_SWITCH, BC_TASK_END, BC_ITEM_RENAME, BC_ITEM_OPEN };
typedef struct { const char *label; int cmd; } dbtn_t;
#define DBTN_MAX 4
static int s_focus[DK_SHELL_ITEM + 1];           /* per kind of window */
static int s_sb_drag;                            /* the kind whose scrollbar thumb is held, or 0 */
static int buttons(const dk_win_t *w, dbtn_t *b, int *def);
static bool can(int cmd);
static void do_btn(dk_win_t *w, int cmd);

static int *focus_of(const dk_win_t *w) { return &s_focus[w->shell <= DK_SHELL_ITEM ? w->shell : 0]; }

static void draw_buttons(tui_surface *sf, dk_win_t *w, tui_rect c, tui_rect clip)
{
    dbtn_t b[DBTN_MAX];
    int def = -1;
    const int n = buttons(w, b, &def);
    const int f = *focus_of(w);
    const bool front = w == dk_front_win();
    for (int i = 0; i < n; i++) {
        const tui_rect r = btn(c, n - 1 - i);
        /* the ring's button is the one Enter presses; with the ring on the list, the default one */
        const bool ring = front && f == i + 1, dflt = f > 0 && f <= n ? ring : i == def;
        dk_pushbtn(sf, clip, r, b[i].label, (dflt ? DK_BS_DEFAULT : 0) | (ring ? DK_BS_FOCUS : 0) |
                   (can(b[i].cmd) ? 0 : DK_BS_DISABLED) | (dk_pressed(r) ? DK_BS_PRESSED : 0) |
                   (dk_hover_in(w, r) ? DK_BS_HOVER : 0));
    }
}

static bool tap_buttons(dk_win_t *w, tui_rect c, int col, int row)
{
    dbtn_t b[DBTN_MAX];
    int def = -1;
    const int n = buttons(w, b, &def);
    for (int i = 0; i < n; i++)
        if (tui_rect_contains(btn(c, n - 1 - i), col, row)) { if (can(b[i].cmd)) do_btn(w, b[i].cmd); return true; }
    return false;
}

/* Tab, Shift+Tab, Alt+letter, and Enter or Space on the button with the ring. */
static bool key_buttons(dk_win_t *w, ls_tk_t k, char ch)
{
    dbtn_t b[DBTN_MAX];
    int def = -1;
    const int n = buttons(w, b, &def);
    int *f = focus_of(w);
    if (*f > n) *f = 0;
    if (k == LS_TK_TAB) {
        *f = (dk_mods() & LS_IMOD_SHIFT) ? (*f + n) % (n + 1) : (*f + 1) % (n + 1);
        return true;
    }
    if (k == LS_TK_CHAR && (dk_mods() & LS_IMOD_ALT)) {
        const char want = (char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
        for (int i = 0; i < n; i++)
            if (dk_label_key(b[i].label) == want) { *f = i + 1; if (can(b[i].cmd)) do_btn(w, b[i].cmd); return true; }
        return true;
    }
    if (*f > 0 && (k == LS_TK_ENTER || (k == LS_TK_CHAR && ch == ' '))) {
        if (can(b[*f - 1].cmd)) do_btn(w, b[*f - 1].cmd);
        return true;
    }
    if (*f > 0 && (k == LS_TK_LEFT || k == LS_TK_RIGHT)) {
        *f = k == LS_TK_LEFT ? (*f > 1 ? *f - 1 : n) : (*f < n ? *f + 1 : 1);
        return true;
    }
    return false;
}

static void close_shell(int shell)
{
    dk_win_t *w = dk_win_shell(shell);
    if (w) ls_desk_close(w->id);
}

/* ----------------------------------------------------------------- list -- */

void dk_list_keep(int n, int rows, int sel, int *top)
{
    if (rows < 1) rows = 1;
    if (sel >= 0 && sel < *top) *top = sel;
    if (sel >= *top + rows) *top = sel - rows + 1;
    if (*top > n - rows) *top = n - rows;
    if (*top < 0) *top = 0;
}

void dk_list_draw(tui_surface *sf, tui_rect clip, tui_rect r, int n, int top, int sel,
                  void (*row)(int i, dk_row_t *out, void *ctx), void *ctx)
{
    dk_sunken(sf, clip, r);
    const tui_rect in = tui_rect_intersect(clip, dk_inner(r));
    dk_fill(sf, in, dk_attr(R_WELL));            /* a list box: the well's ground */
    const int rows = r.h - 2, x = r.x + 1, w = r.w - 2;
    const bool bar = n > rows;
    const int tw = bar ? w - 1 : w;
    for (int k = 0; k < rows; k++) {
        const int i = top + k, y = r.y + 1 + k;
        if (i >= n) break;
        dk_row_t d = {0};
        row(i, &d, ctx);
        const bool s = i == sel;
        const uint8_t face = s ? dk_attr(R_SELECT) : dk_attr(R_WELL);
        const tui_rect rr = tui_rect_intersect(in, tui_rect_make(x, y, tw, 1));
        if (s) dk_fill(sf, rr, face);
        if (d.glyph) {
            const uint8_t ga = s ? face : dk_well_hue(d.hue);
            if (d.glyph < 0x80) tui_put_char(sf, rr, x + 1, y, (char)d.glyph, ga);
            else ls_box_put(sf, rr, x + 1, y, d.glyph, ga);
        }
        int rx = x + tw - 1;
        if (d.right && d.right[0]) {
            const int len = dk_text_len(d.right);
            dk_text(sf, rr, rx - len, y, d.right, s ? face : dk_attr(R_WELL));
            rx -= len + 1;
        }
        dk_text(sf, tui_rect_intersect(rr, tui_rect_make(x, y, rx - x, 1)), x + 3, y, d.text ? d.text : "", face);
    }
    if (bar) dk_vscroll(sf, in, r.x + r.w - 2, r.y + 1, rows, n, rows, top);
}

/* A list view of one of these windows: a press selects the row, or works
   the scrollbar (a held thumb is followed while the window draws). */
static int lv_press(const dk_win_t *w, tui_rect r, int n, int *top, int *sel, int col, int row)
{
    const int hit = dk_listview_hit(r, n, *top, col, row);
    if (hit == DK_LV_BAR) {
        const tui_rect b = dk_listview_bar(r);
        if (dk_vscroll_hit(b.y, b.h, n, dk_listview_rows(r), *top, row) == DK_SB_THUMB) s_sb_drag = w->shell;
        else dk_vscroll_click(b.x, b.y, b.h, n, dk_listview_rows(r), top, col, row);
    } else if (hit >= 0) {
        *sel = hit;
        *focus_of(w) = 0;
    }
    return hit;
}

static void lv_follow(const dk_win_t *w, tui_rect r, int n, int *top)
{
    const int rows = dk_listview_rows(r);
    if (s_sb_drag == w->shell) {
        if (dk_held(w)) {
            int pc = 0, pr = 0;
            const tui_rect b = dk_listview_bar(r);
            dk_pointer(&pc, &pr);
            *top = dk_vscroll_drag(b.y, b.h, n, rows, pr);
        } else s_sb_drag = 0;
    }
    if (*top > n - rows) *top = n - rows;
    if (*top < 0) *top = 0;
}

static int list_hit(tui_rect r, int n, int top, int col, int row)
{
    if (col <= r.x || col >= r.x + r.w - 1 || row <= r.y || row >= r.y + r.h - 1) return -1;
    const int i = top + row - r.y - 1;
    return i < n ? i : -1;
}

/* Arrows, paging and the wheel on a list of n with `rows` in view. */
static bool list_key(ls_tk_t k, int n, int rows, int *sel, int *top)
{
    int s = *sel;
    switch (k) {
    case LS_TK_WHEEL_UP: case LS_TK_WHEEL_DOWN:
        /* the wheel moves the view, three rows a notch; the selection stays */
        *top += k == LS_TK_WHEEL_UP ? -3 : 3;
        if (*top > n - rows) *top = n - rows;
        if (*top < 0) *top = 0;
        return true;
    case LS_TK_UP: s = s <= 0 ? 0 : s - 1; break;
    case LS_TK_DOWN: s = s + 1; break;
    case LS_TK_PGUP: s -= rows; break;
    case LS_TK_PGDN: s += rows; break;
    case LS_TK_HOME: s = 0; break;
    case LS_TK_END: s = n - 1; break;
    default: return false;
    }
    if (s >= n) s = n - 1;
    if (s < 0) s = n ? 0 : -1;
    *sel = s;
    dk_list_keep(n, rows, s, top);
    return true;
}

/* -------------------------------------------------------------- message -- */

EXT_RAM_BSS_ATTR static char s_msg[2][72];

void dk_msg(const char *title, const char *l1, const char *l2)
{
    close_shell(DK_SHELL_MSG);
    snprintf(s_msg[0], sizeof(s_msg[0]), "%s", l1 ? l1 : "");
    snprintf(s_msg[1], sizeof(s_msg[1]), "%s", l2 ? l2 : "");
    dk_open_shell(DK_SHELL_MSG, title ? title : "LakeShark Desktop", 60, 8);
}

static void msg_draw(tui_surface *sf, tui_rect c, tui_rect clip)
{
    /* a warning sign: three strokes and its mark */
    const uint8_t wa = on_face(TUI_ATTR_FG(dk_attr(R_WARN)));
    const int x = c.x + 3, y = c.y + 1;
    ls_box_put(sf, clip, x + 2, y, LS_BOX_DIAG_R, wa);
    ls_box_put(sf, clip, x + 3, y, LS_BOX_DIAG_L, wa);
    ls_box_put(sf, clip, x + 1, y + 1, LS_BOX_DIAG_R, wa);
    tui_put_char(sf, clip, x + 2, y + 1, '!', dk_attr(R_STRONG));
    ls_box_put(sf, clip, x + 4, y + 1, LS_BOX_DIAG_L, wa);
    ls_box_put(sf, clip, x, y + 2, LS_BOX_DIAG_R, wa);
    dk_hline(sf, clip, x + 1, y + 2, 4, LS_EDGE_B, wa);
    ls_box_put(sf, clip, x + 5, y + 2, LS_BOX_DIAG_L, wa);
    const tui_rect t = tui_rect_intersect(clip, tui_rect_make(c.x + 11, c.y, c.w - 12, c.h));
    dk_text(sf, t, c.x + 11, c.y + 1, s_msg[0], dk_attr(R_STRONG));
    dk_text(sf, t, c.x + 11, c.y + 2, s_msg[1], dk_attr(R_FACE));
}

/* --------------------------------------------------------- prompt / Run -- */

EXT_RAM_BSS_ATTR static struct {
    char label[48], text[48];
    int max, ctx, sel;
    bool run;
    void (*done)(const char *text, int ctx);
} s_p;

void dk_prompt(const char *title, const char *label, const char *initial, int max,
               void (*done)(const char *text, int ctx), int ctx)
{
    close_shell(DK_SHELL_PROMPT);
    memset(&s_p, 0, sizeof(s_p));
    snprintf(s_p.label, sizeof(s_p.label), "%s", label ? label : "");
    snprintf(s_p.text, sizeof(s_p.text), "%s", initial ? initial : "");
    s_p.max = max > 0 && max < (int)sizeof(s_p.text) ? max : (int)sizeof(s_p.text) - 1;
    s_p.text[s_p.max] = 0;
    s_p.done = done;
    s_p.ctx = ctx;
    dk_open_shell(DK_SHELL_PROMPT, title, 54, 8);
}

/* Run: what the typed letters could mean. An app whose name or id holds
   them, a window of the shell's, and last the line itself as a console
   word for TERMINAL. */
static const struct { const char *word, *name; } RUN_SHELL[] = {
    {"control", "Control Panel"}, {"taskmgr", "Task Manager"}, {"display", "Display Properties"},
    {"folder", "SD card folder"}, {"bin", "Recycle Bin"}, {"studio", "LakeShark Studio"}, {"about", "About"},
};
#define RUN_SHELLS ((int)(sizeof(RUN_SHELL) / sizeof(RUN_SHELL[0])))
#define RUN_MAX 9
typedef struct { const ls_app_t *app; int shell; } run_hit_t;      /* neither: the console row */

static bool holds(const char *hay, const char *needle)
{
    if (!hay) return false;
    const size_t n = strlen(needle);
    for (; *hay; hay++) if (!strncasecmp(hay, needle, n)) return true;
    return false;
}

static int run_hits(run_hit_t *out)
{
    int n = 0;
    if (!s_p.text[0]) return 0;
    /* names that start with the letters first, then names that hold them */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < ls_app_count() && n < RUN_MAX - 1; i++) {
            const ls_app_t *a = ls_app_at(i);
            if (!a || !a->screen || !a->id || !strcmp(a->id, "home") || strstr(a->id, "-design")) continue;
            const size_t len = strlen(s_p.text);
            const bool starts = (a->name && !strncasecmp(a->name, s_p.text, len)) || !strncasecmp(a->id, s_p.text, len);
            const bool has = holds(a->name, s_p.text) || holds(a->id, s_p.text);
            if (pass == 0 ? starts : has && !starts) out[n++] = (run_hit_t){.app = a, .shell = -1};
        }
    for (int i = 0; i < RUN_SHELLS && n < RUN_MAX - 1; i++)
        if (holds(RUN_SHELL[i].word, s_p.text) || holds(RUN_SHELL[i].name, s_p.text)) out[n++] = (run_hit_t){.shell = i};
    out[n++] = (run_hit_t){.shell = -1};
    return n;
}

void dk_run_open_with(char first)
{
    dk_run_open();
    if (first > 32 && first < 127) { s_p.text[0] = first; s_p.text[1] = 0; }
}

void dk_run_open(void)
{
    close_shell(DK_SHELL_PROMPT);
    memset(&s_p, 0, sizeof(s_p));
    snprintf(s_p.label, sizeof(s_p.label), "Type the name of an app, or a console word:");
    s_p.max = 40;
    s_p.run = true;
    dk_open_shell(DK_SHELL_PROMPT, "Run", 58, 18);
}

static void run_row(int i, dk_row_t *o, void *ctx)
{
    const run_hit_t *h = &((const run_hit_t *)ctx)[i];
    EXT_RAM_BSS_ATTR static char line[56];
    if (h->app) {
        o->text = h->app->name ? h->app->name : h->app->id;
        o->right = h->app->id;
        o->hue = h->app->hue;
        o->glyph = LS_SHAPE_DOT;
    } else if (h->shell >= 0) {
        o->text = RUN_SHELL[h->shell].name;
        o->right = RUN_SHELL[h->shell].word;
        o->hue = TUI_YELLOW;
        o->glyph = LS_SHAPE_SQUARE;
    } else {
        snprintf(line, sizeof(line), "%s", s_p.text);
        o->text = line;
        o->right = "in TERMINAL";
        o->hue = TUI_GREEN;
        o->glyph = '>';
    }
}

static tui_rect prompt_field(tui_rect c) { return tui_rect_make(c.x + 2, c.y + 2, c.w - 4, 3); }
static tui_rect run_list(tui_rect c) { return tui_rect_make(c.x + 2, c.y + 5, c.w - 4, c.h - 8); }

static void prompt_draw(tui_surface *sf, tui_rect c, tui_rect clip)
{
    dk_text(sf, clip, c.x + 2, c.y + 1, s_p.label, dk_attr(R_FACE));
    const tui_rect f = prompt_field(c);
    dk_sunken(sf, clip, f);
    const tui_rect in = tui_rect_intersect(clip, dk_inner(f));
    dk_fill(sf, in, dk_attr(R_WELL));            /* an edit: the well's ground */
    const int n = dk_text(sf, in, f.x + 2, f.y + 1, s_p.text, dk_attr(R_WELL));
    ls_box_put(sf, in, f.x + 2 + n, f.y + 1, LS_EDGE_B, dk_attr(R_WELL));        /* where the next letter goes */
    if (s_p.run) {
        run_hit_t hits[RUN_MAX];
        const int hn = run_hits(hits);
        if (s_p.sel >= hn) s_p.sel = hn - 1;
        if (hn) dk_list_draw(sf, clip, run_list(c), hn, 0, s_p.sel, run_row, hits);
        else {
            dk_sunken(sf, clip, run_list(c));
            dk_fill(sf, tui_rect_intersect(clip, dk_inner(run_list(c))), dk_attr(R_WELL));
            dk_text(sf, clip, c.x + 5, c.y + 7, "fm   map   control   taskmgr   folder", dk_attr(R_WELL_DIM));
            dk_text(sf, clip, c.x + 5, c.y + 8, "or any console word: it runs in TERMINAL", dk_attr(R_WELL_DIM));
        }
    }
}

static void run_go(void)
{
    run_hit_t hits[RUN_MAX];
    const int hn = run_hits(hits);
    if (!hn) return;
    const run_hit_t h = hits[s_p.sel >= 0 && s_p.sel < hn ? s_p.sel : 0];
    char line[sizeof(s_p.text)];
    snprintf(line, sizeof(line), "%s", s_p.text);
    close_shell(DK_SHELL_PROMPT);
    if (h.app) { dk_open_app_win(h.app); return; }
    switch (h.shell) {
    case 0: dk_cpl_open(-1); break;
    case 1: dk_tasks_open(); break;
    case 2: dk_do(A_DISPLAY, 0); break;
    case 3: dk_folder_open(NULL, -1, -1); break;
    case 4: dk_bin_open(); break;
    case 5: dk_open_studio(); break;
    case 6: ls_desk_open_about(); break;
    default:
        if (line[0] == '/') { dk_folder_open(line, -1, -1); break; }
        if (!dk_sys_console(line)) dk_msg("Run", line, "TERMINAL is busy or not in this build.");
        break;
    }
}

static void prompt_ok(void)
{
    if (s_p.run) { run_go(); return; }
    void (*done)(const char *, int) = s_p.done;
    char text[sizeof(s_p.text)];
    snprintf(text, sizeof(text), "%s", s_p.text);
    const int ctx = s_p.ctx;
    close_shell(DK_SHELL_PROMPT);
    if (done) done(text, ctx);
}

static bool prompt_key(ls_tk_t k, char ch)
{
    const size_t n = strlen(s_p.text);
    if (k == LS_TK_CHAR && ch >= 32 && ch < 127) {
        if ((int)n < s_p.max) { s_p.text[n] = ch; s_p.text[n + 1] = 0; s_p.sel = 0; }
        s_focus[DK_SHELL_PROMPT] = 0;
        return true;
    }
    if (k == LS_TK_BACKSPACE || k == LS_TK_DELETE) { if (n) s_p.text[n - 1] = 0; s_p.sel = 0; return true; }
    if (k == LS_TK_ENTER) { prompt_ok(); return true; }
    if (k == LS_TK_ESC) { close_shell(DK_SHELL_PROMPT); return true; }
    if (s_p.run && (k == LS_TK_UP || k == LS_TK_WHEEL_UP)) { if (s_p.sel > 0) s_p.sel--; return true; }
    if (s_p.run && (k == LS_TK_DOWN || k == LS_TK_WHEEL_DOWN)) { s_p.sel++; return true; }
    return true;
}

/* -------------------------------------------------- New shortcut (apps) -- */

EXT_RAM_BSS_ATTR static struct { const ls_app_t *app[LS_APP_MAX]; uint8_t group[LS_APP_MAX]; int n, sel, top, x, y; } s_pick;

void dk_pick_open(int x, int y)
{
    s_pick.n = 0;
    for (int g = 0; g < 5; g++) {
        /* Off the stack: sized for the whole directory. */
        EXT_RAM_BSS_ATTR static const ls_app_t *list[LS_APP_MAX];
        const int n = dk_group_apps(g, list, LS_APP_MAX);
        for (int i = 0; i < n && s_pick.n < LS_APP_MAX; i++) {
            s_pick.app[s_pick.n] = list[i];
            s_pick.group[s_pick.n++] = (uint8_t)g;
        }
    }
    s_pick.sel = s_pick.n ? 0 : -1;
    s_pick.top = 0;
    s_pick.x = x;
    s_pick.y = y;
    dk_open_shell(DK_SHELL_PICK, "New Shortcut", 54, 22);
}

static const dk_col_t PICK_COLS[] = { {"App", 0, false}, {"Group", 14, false} };
static void pick_cell(int i, int col, char *buf, size_t n, uint8_t *hue, int *glyph, void *ctx)
{
    (void)ctx;
    if (col == 0) { snprintf(buf, n, "%s", s_pick.app[i]->name); *hue = s_pick.app[i]->hue; *glyph = LS_SHAPE_DOT; }
    else snprintf(buf, n, "%s", dk_group_name(s_pick.group[i]));
}

static tui_rect pick_list(tui_rect c) { return tui_rect_make(c.x + 1, c.y + 2, c.w - 2, c.h - 5); }

static void pick_add(void)
{
    if (s_pick.sel < 0 || s_pick.sel >= s_pick.n) return;
    const ls_app_t *a = s_pick.app[s_pick.sel];
    const int x = s_pick.x, y = s_pick.y;
    close_shell(DK_SHELL_PICK);
    if (dk_items_add(DKI_APP, 0, a->id, "", x, y) < 0) dk_msg("New Shortcut", "The desktop is full.", "Delete an icon, or empty the Recycle Bin.");
}

/* --------------------------------------------------------------- folder -- */

#define FOLD_MAX 128
typedef struct { char name[40]; bool dir, sized; long size; } fold_ent_t;
EXT_RAM_BSS_ATTR static struct {
    char path[40];                       /* under the card's root: "", "/music" */
    fold_ent_t *ent;                     /* PSRAM, while the window is open */
    int n, sel, top, press;
    bool ok, more;
    long size;                           /* the selected file's, -1 unknown */
    int size_of;
} s_f;

static void fold_full(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s%s%s%s", dk_sys_card_root(), s_f.path, name ? "/" : "", name ? name : "");
}

static void fold_read(void)
{
    s_f.n = 0;
    s_f.sel = -1;
    s_f.top = 0;
    s_f.press = -1;
    s_f.more = false;
    s_f.size_of = -1;
    if (!s_f.ent) s_f.ent = dk_psram(sizeof(fold_ent_t) * FOLD_MAX);
    char full[128];
    fold_full(full, sizeof(full), NULL);
    DIR *d = s_f.ent ? opendir(full) : NULL;
    s_f.ok = d != NULL;
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(s_f.ent[0].name)) continue;
        if (s_f.n >= FOLD_MAX) { s_f.more = true; break; }
        fold_ent_t *t = &s_f.ent[s_f.n++];
        snprintf(t->name, sizeof(t->name), "%.*s", (int)sizeof(t->name) - 1, e->d_name);
        t->sized = false;
        t->size = -1;
#ifdef DT_DIR
        t->dir = e->d_type == DT_DIR;
#else
        char p[400];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", full, e->d_name);
        t->dir = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
    }
    closedir(d);
    for (int a = 1; a < s_f.n; a++)                      /* folders first, then by name */
        for (int b = a; b > 0; b--) {
            const fold_ent_t *p = &s_f.ent[b - 1], *q = &s_f.ent[b];
            if (p->dir != q->dir ? p->dir : strcasecmp(p->name, q->name) <= 0) break;
            const fold_ent_t t = s_f.ent[b]; s_f.ent[b] = s_f.ent[b - 1]; s_f.ent[b - 1] = t;
        }
    if (s_f.n) s_f.sel = 0;
}

void dk_folder_open(const char *path, int x, int y)
{
    (void)x; (void)y;
    snprintf(s_f.path, sizeof(s_f.path), "%s", path && strcmp(path, "/") ? path : "");
    fold_read();
    dk_open_shell(DK_SHELL_FOLDER, "SD Card", 66, 22);
}

static void size_text(char *buf, size_t n, long size)
{
    if (size < 0) buf[0] = 0;
    else if (size < 10240) snprintf(buf, n, "%ld B", size);
    else if (size < 10485760) snprintf(buf, n, "%ld KB", size / 1024);
    else snprintf(buf, n, "%ld MB", size / 1048576);
}

/* What a name's ending says it is: the Type column. */
static const char *fold_type(const fold_ent_t *e)
{
    if (e->dir) return "Folder";
    const char *dot = strrchr(e->name, '.');
    static const struct { const char *ext, *what; } T[] = {
        {"txt", "Text"}, {"csv", "Table"}, {"log", "Log"}, {"json", "Data"}, {"wav", "Sound"}, {"mp3", "Music"},
        {"flac", "Music"}, {"ogg", "Music"}, {"bmp", "Picture"}, {"png", "Picture"}, {"jpg", "Picture"},
        {"bin", "Binary"}, {"iq", "I/Q capture"}, {"cu8", "I/Q capture"}, {"pmtiles", "Map"}, {"lap", "App"},
    };
    if (dot) for (unsigned i = 0; i < sizeof(T) / sizeof(T[0]); i++) if (!strcasecmp(dot + 1, T[i].ext)) return T[i].what;
    return "File";
}

static const dk_col_t FOLD_COLS[] = { {"Name", 0, false}, {"Type", 13, false}, {"Size", 10, true} };
static void fold_cell(int i, int col, char *buf, size_t n, uint8_t *hue, int *glyph, void *ctx)
{
    (void)ctx;
    fold_ent_t *e = &s_f.ent[i];
    if (col == 0) {
        snprintf(buf, n, "%s", e->name);
        *hue = e->dir ? TUI_YELLOW : TUI_CYAN;
        *glyph = e->dir ? LS_SHAPE_RIGHT : LS_SHAPE_DOT;
    } else if (col == 1) snprintf(buf, n, "%s", fold_type(e));
    else if (!e->dir) {
        if (!e->sized) {
            /* asked of the card once, the first time the row comes into view */
            char full[176];
            struct stat sb;
            snprintf(full, sizeof(full), "%s%s/%s", dk_sys_card_root(), s_f.path, e->name);
            e->size = stat(full, &sb) == 0 ? (long)sb.st_size : -1;
            e->sized = true;
        }
        size_text(buf, n, e->size);
    }
}

static tui_rect fold_list(tui_rect c) { return tui_rect_make(c.x + 1, c.y + 1, c.w - 2, c.h - 5); }

static void fold_up(void)
{
    char *s = strrchr(s_f.path, '/');
    if (!s) return;
    char was[40];
    snprintf(was, sizeof(was), "%s", s + 1);
    *s = 0;
    fold_read();
    for (int i = 0; i < s_f.n; i++) if (!strcmp(s_f.ent[i].name, was)) s_f.sel = i;
    dk_list_keep(s_f.n, 12, s_f.sel, &s_f.top);
}

static void fold_enter(void)
{
    if (s_f.sel < 0 || s_f.sel >= s_f.n) return;
    const fold_ent_t *e = &s_f.ent[s_f.sel];
    if (e->dir) {
        if (strlen(s_f.path) + strlen(e->name) + 2 > sizeof(s_f.path)) {
            dk_msg("SD Card", "That folder is too deep for this window.", "The SD CARD app opens it.");
            return;
        }
        char next[84];
        snprintf(next, sizeof(next), "%s/%s", s_f.path, e->name);
        snprintf(s_f.path, sizeof(s_f.path), "%.*s", (int)sizeof(s_f.path) - 1, next);
        fold_read();
        return;
    }
    char full[176];
    fold_full(full, sizeof(full), e->name);
    if (!dk_sys_open_file(full)) dk_msg("Open", e->name, "Nothing on this board opens that file.");
}

/* The selected row (or this folder) as a shortcut at col,row; -1: next place. */
static void fold_shortcut(int col, int row, int which)
{
    char target[96];
    int kind = DKI_FOLDER;
    if (which >= 0 && which < s_f.n) {
        snprintf(target, sizeof(target), "%s/%s", s_f.path, s_f.ent[which].name);
        kind = s_f.ent[which].dir ? DKI_FOLDER : DKI_FILE;
    } else {
        snprintf(target, sizeof(target), "%s", s_f.path[0] ? s_f.path : "/");
    }
    if (strlen(target) >= sizeof(((dk_icon_t *)0)->target)) {
        dk_msg("New Shortcut", "That path is too long for a shortcut.", "Make one to a folder nearer the top.");
        return;
    }
    if (dk_items_add(kind, 0, target, "", col, row) < 0)
        dk_msg("New Shortcut", "The desktop is full.", "Delete an icon, or empty the Recycle Bin.");
}

bool dk_folder_drag_item(char *label, size_t n)
{
    if (!dk_win_shell(DK_SHELL_FOLDER) || s_f.press < 0 || s_f.press >= s_f.n) return false;
    if (label) snprintf(label, n, "%s", s_f.ent[s_f.press].name);
    return true;
}

void dk_folder_drop(int col, int row)
{
    if (dk_folder_drag_item(NULL, 0)) fold_shortcut(col, row, s_f.press);
    s_f.press = -1;
}

static void fold_draw(tui_surface *sf, dk_win_t *w, tui_rect c, tui_rect clip)
{
    /* the address: the card, then each folder down to this one */
    char addr[64];
    snprintf(addr, sizeof(addr), "SD card:%s", s_f.path[0] ? s_f.path : "/");
    dk_text(sf, clip, c.x + 2, c.y, "Address:", dk_attr(R_FACE));
    dk_field(sf, clip, c.x + 10, c.y, c.w - 12, addr, -1, false);
    const tui_rect l = fold_list(c);
    if (!s_f.ok) {
        dk_sunken(sf, clip, l);
        dk_fill(sf, tui_rect_intersect(clip, dk_inner(l)), dk_attr(R_WELL));
        dk_pic(sf, clip, l.x + 3, l.y + 2, DK_PIC_CARD, dk_well_hue(TUI_CYAN));
        dk_text(sf, clip, l.x + 16, l.y + 3, "No card, or no such folder.", dk_well_hue(TUI_ATTR_FG(dk_attr(R_WARN))));
        dk_text(sf, clip, l.x + 16, l.y + 4, "Put a card in and open this again.", dk_attr(R_WELL));
    } else {
        lv_follow(w, l, s_f.n, &s_f.top);
        dk_listview(sf, clip, l, FOLD_COLS, 3, s_f.n, s_f.top, s_f.sel, w == dk_front_win() && !*focus_of(w), fold_cell, NULL);
        if (!s_f.n) dk_text(sf, clip, l.x + 3, l.y + 3, "This folder is empty.", dk_attr(R_WELL_DIM));
    }
    /* the status line: how many, and what the selection is */
    char st[80];
    int folders = 0;
    for (int k = 0; k < s_f.n; k++) folders += s_f.ent[k].dir;
    snprintf(st, sizeof(st), "%d folder%s, %d file%s%s", folders, folders == 1 ? "" : "s", s_f.n - folders,
             s_f.n - folders == 1 ? "" : "s", s_f.more ? " (the first of more)" : "");
    const int sy = c.y + c.h - 4;
    dk_text(sf, clip, c.x + 2, sy, st, dk_attr(R_FACE));
    const char *hint = "drag a row onto the desktop";
    dk_text(sf, clip, c.x + c.w - 2 - dk_text_len(hint), sy, hint, dk_attr(R_DISABLED));
}

/* ------------------------------------------------------------------ bin -- */

EXT_RAM_BSS_ATTR static struct { dk_icon_t *it[DK_ICON_MAX]; int n, sel, top; bool armed; } s_bin;

void dk_bin_open(void)
{
    s_bin.sel = 0;
    s_bin.top = 0;
    s_bin.armed = false;
    dk_open_shell(DK_SHELL_BIN, "Recycle Bin", 62, 17);
}

static const dk_col_t BIN_COLS[] = { {"Name", 18, false}, {"Was", 0, false} };
static void bin_cell(int i, int col, char *buf, size_t n, uint8_t *hue, int *glyph, void *ctx)
{
    (void)ctx;
    const dk_icon_t *it = s_bin.it[i];
    if (col == 0) {
        snprintf(buf, n, "%s", dk_items_label(it));
        *hue = it->kind == DKI_FOLDER ? TUI_YELLOW : it->kind == DKI_APP ? TUI_CYAN : TUI_WHITE;
        *glyph = it->kind == DKI_FOLDER ? LS_SHAPE_RIGHT : LS_SHAPE_DOT;
    } else dk_items_what(it, buf, n);
}

static tui_rect bin_list(tui_rect c) { return tui_rect_make(c.x + 1, c.y + 1, c.w - 2, c.h - 4); }

static void bin_draw(tui_surface *sf, dk_win_t *w, tui_rect c, tui_rect clip)
{
    s_bin.n = dk_items_binned(s_bin.it, DK_ICON_MAX);
    if (s_bin.n > DK_ICON_MAX) s_bin.n = DK_ICON_MAX;
    if (s_bin.sel >= s_bin.n) s_bin.sel = s_bin.n - 1;
    if (s_bin.sel < 0 && s_bin.n) s_bin.sel = 0;
    char head[64];
    snprintf(head, sizeof(head), s_bin.n ? "%d deleted shortcut%s. Their files are untouched." : "The bin is empty.",
             s_bin.n, s_bin.n == 1 ? "" : "s");
    dk_text(sf, clip, c.x + 2, c.y, head, dk_attr(R_FACE));
    const tui_rect l = bin_list(c);
    lv_follow(w, l, s_bin.n, &s_bin.top);
    dk_listview(sf, clip, l, BIN_COLS, 2, s_bin.n, s_bin.top, s_bin.sel, w == dk_front_win() && !*focus_of(w), bin_cell, NULL);
    if (!s_bin.n) dk_pic(sf, clip, l.x + (l.w - 10) / 2, l.y + 3, DK_PIC_BIN, on_face(TUI_WHITE));
}

static void bin_do(int what)
{
    dk_icon_t *it = s_bin.sel >= 0 && s_bin.sel < s_bin.n ? s_bin.it[s_bin.sel] : NULL;
    if (what == 0 && it) dk_items_restore(it);
    if (what == 1 && it) dk_items_purge(it);
    if (what == 2) {
        /* one press arms, the next empties: never one stray click */
        if (!s_bin.armed) { s_bin.armed = s_bin.n > 0; return; }
        dk_items_purge(NULL);
    }
    s_bin.armed = false;
}

/* --------------------------------------------------------- Task Manager -- */

#define PERF_N 48
EXT_RAM_BSS_ATTR static struct {
    int ids[DK_WIN_SLOTS], n, sel, top;
    int hist[PERF_N], psram[PERF_N], hn;
    int64_t last_us;
} s_t;

void dk_tasks_open(void)
{
    s_t.sel = 0;
    s_t.top = 0;
    dk_open_shell(DK_SHELL_TASKS, "Task Manager", 72, 23);
}

static const dk_col_t TASK_COLS[] = { {"Task", 0, false}, {"Kind", 8, false}, {"State", 24, false}, {"Window", 11, false} };
static void tasks_cell(int i, int col, char *buf, size_t n, uint8_t *hue, int *glyph, void *ctx)
{
    (void)ctx;
    const dk_win_t *w = dk_win(s_t.ids[i]);
    if (!w) { snprintf(buf, n, "?"); return; }
    /* the first window is the one in front, unless this window is: then the next */
    const char *radio = w->app && w->app->screen ? w->app->screen->radio : NULL;
    const char *claimed = ls_tui_radio_claimed();
    const bool rx = radio && claimed && !strcmp(radio, claimed);
    switch (col) {
    case 0:
        snprintf(buf, n, "%s", w->title);
        *hue = w->app ? w->app->hue : TUI_YELLOW;
        *glyph = w->kind == DK_APP ? LS_SHAPE_DOT : LS_SHAPE_SQUARE;
        break;
    case 1: snprintf(buf, n, "%s", w->kind == DK_SHELL ? "Desk" : "App"); break;
    case 2:
        if (w->kind == DK_SHELL) snprintf(buf, n, "Live");
        else if (w == dk_front_win()) { snprintf(buf, n, "Running"); *hue = TUI_GREEN; }
        else if (rx) { snprintf(buf, n, "Receiving, view paused"); *hue = TUI_CYAN; }
        else snprintf(buf, n, "Paused");
        break;
    default: snprintf(buf, n, "%s", w->state == LS_DESK_WIN_MIN ? "Minimized" : w->state == LS_DESK_WIN_MAX ? "Maximized" : "Normal"); break;
    }
}

static tui_rect tasks_list(tui_rect c) { return tui_rect_make(c.x + 1, c.y + 1, c.w - 2, c.h - 11 > 5 ? c.h - 11 : 5); }

static void tasks_draw(tui_surface *sf, dk_win_t *win, tui_rect c, tui_rect clip)
{
    s_t.n = 0;
    ls_desk_win_info_t wi;
    for (int z = ls_desk_window_count() - 1; z >= 0; z--)       /* front first */
        if (ls_desk_window_at(z, &wi) && s_t.n < DK_WIN_SLOTS) s_t.ids[s_t.n++] = wi.id;
    if (s_t.sel >= s_t.n) s_t.sel = s_t.n - 1;
    char line[80];
    snprintf(line, sizeof(line), "%d of %d windows. One app runs live; the others hold a frame.", s_t.n, LS_DESK_MAX_WIN);
    dk_text(sf, clip, c.x + 2, c.y, line, dk_attr(R_FACE));
    const tui_rect l = tasks_list(c);
    lv_follow(win, l, s_t.n, &s_t.top);
    dk_listview(sf, clip, l, TASK_COLS, 4, s_t.n, s_t.top, s_t.sel, win == dk_front_win() && !*focus_of(win), tasks_cell, NULL);

    /* Memory, as the allocator has it now; a sample a second for the traces. */
    dk_perf_t p;
    dk_sys_perf(&p);
    const int64_t now = esp_timer_get_time();
    if (now - s_t.last_us >= 1000000 || !s_t.hn) {
        s_t.last_us = now;
        if (s_t.hn == PERF_N) {
            memmove(s_t.hist, s_t.hist + 1, sizeof(int) * (PERF_N - 1));
            memmove(s_t.psram, s_t.psram + 1, sizeof(int) * (PERF_N - 1));
            s_t.hn--;
        }
        s_t.hist[s_t.hn] = (int)p.internal_kb;
        s_t.psram[s_t.hn++] = (int)p.psram_kb;
    }
    const int py = l.y + l.h;
    dk_group(sf, clip, tui_rect_make(c.x + 1, py, c.w - 2, 6), "Memory", 0);
    const int vx = c.x + 20, tx = c.x + c.w - 2 - PERF_N / 2 - 1;
    static const char *const NAME[2] = {"Internal RAM", "PSRAM"};
    for (int k = 0; k < 2; k++) {
        const int y = py + 1 + k * 2;
        const long free_kb = k ? p.psram_kb : p.internal_kb, low = k ? p.psram_low_kb : p.internal_low_kb;
        dk_text(sf, clip, c.x + 3, y, NAME[k], hue_on_face(k ? TUI_MAGENTA : TUI_CYAN));
        snprintf(line, sizeof(line), "%ld KB free", free_kb);
        dk_text(sf, clip, vx, y, line, k == 0 && free_kb < 8 ? dk_attr(R_BAD) : dk_attr(R_STRONG));
        if (low > 0) { snprintf(line, sizeof(line), "lowest since boot %ld KB", low); dk_text(sf, clip, vx, y + 1, line, dk_attr(R_FACE)); }
        if (k == 0 && p.largest_kb > 0) { snprintf(line, sizeof(line), "largest block %ld KB", p.largest_kb); dk_text(sf, clip, c.x + 3, y + 1, line, dk_attr(R_FACE)); }
        /* the trace scales to its own span: it shows which way, not how much */
        const int *h = k ? s_t.psram : s_t.hist;
        int mn = h[0], mx = h[0];
        for (int i = 1; i < s_t.hn; i++) { if (h[i] < mn) mn = h[i]; if (h[i] > mx) mx = h[i]; }
        int rel[PERF_N];
        for (int i = 0; i < s_t.hn; i++) rel[i] = h[i] - mn + 1;
        for (int i = 0; i < s_t.hn; i += 2)
            dk_spark(sf, clip, tx + i / 2, y, rel + i, i + 1 < s_t.hn ? 2 : 1, mx - mn + 1, hue_on_face(k ? TUI_MAGENTA : TUI_CYAN));
        ls_box_put(sf, clip, tx - 1, y, LS_BOX_V, dk_attr(R_SHADOW));
    }
    const int uy = py + 6;
    snprintf(line, sizeof(line), "Up %ldd %02ld:%02ld:%02ld", p.uptime_s / 86400, p.uptime_s / 3600 % 24, p.uptime_s / 60 % 60, p.uptime_s % 60);
    dk_text(sf, clip, c.x + 3, uy, line, dk_attr(R_FACE));
    if (p.frame_us > 0) {
        snprintf(line, sizeof(line), "last frame %d.%d ms, %d cells sent", p.frame_us / 1000, p.frame_us / 100 % 10, p.cells);
        dk_text(sf, clip, vx, uy, line, dk_attr(R_FACE));
    }
}

static void tasks_do(int what)
{
    if (s_t.sel < 0 || s_t.sel >= s_t.n) return;
    const int id = s_t.ids[s_t.sel];
    if (what == 0) ls_desk_raise(id);
    else ls_desk_close(id);
}

/* ------------------------------------------------------ item properties -- */

static int s_item = -1;

void dk_item_props_open(int item)
{
    s_item = item;
    close_shell(DK_SHELL_ITEM);
    dk_open_shell(DK_SHELL_ITEM, "Shortcut Properties", 56, 12);
}

static void rename_done(const char *text, int ctx)
{
    if (text[0]) dk_items_rename(ctx, text);
}

static void item_draw(tui_surface *sf, tui_rect c, tui_rect clip)
{
    const dk_icon_t *it = dk_items_at(s_item);
    if (!it) { dk_text(sf, clip, c.x + 2, c.y + 1, "That shortcut is gone.", dk_attr(R_DISABLED)); return; }
    dk_items_draw_one(sf, clip, c.x + 3, c.y + 1, it, dk_face_bg());
    char what[64];
    dk_items_what(it, what, sizeof(what));
    const int x = c.x + 17;
    const tui_rect t = tui_rect_intersect(clip, tui_rect_make(x, c.y, c.w - 18, c.h));
    dk_text(sf, t, x, c.y + 1, dk_items_label(it), dk_attr(R_STRONG));
    dk_text(sf, t, x, c.y + 3, it->kind == DKI_SYS ? "Part of the desktop" : "Shortcut", dk_attr(R_FACE));
    dk_text(sf, t, x, c.y + 4, what, dk_attr(R_INFO));
    const tui_rect r = dk_items_rect(s_item);
    char pos[48];
    snprintf(pos, sizeof(pos), (it->flags & DKI_F_MOVED) ? "Placed at column %d, row %d" : "In the columns (not dragged)", r.x, r.y);
    dk_text(sf, t, x, c.y + 6, pos, dk_attr(R_FACE));
}

/* --------------------------------------------------------------- common -- */

/* Each window's buttons, left to right, and which one Enter presses. */
static int buttons(const dk_win_t *w, dbtn_t *b, int *def)
{
    int n = 0;
    #define B(l, c) do { b[n].label = (l); b[n].cmd = (c); n++; } while (0)
    *def = 0;
    switch (w->shell) {
    case DK_SHELL_MSG: B("OK", BC_CLOSE); break;
    case DK_SHELL_PROMPT: B(s_p.run ? "&Run" : "OK", BC_OK); B("Cancel", BC_CLOSE); break;
    case DK_SHELL_PICK: B("&Add", BC_PICK_ADD); B("Cancel", BC_CLOSE); break;
    case DK_SHELL_FOLDER: B("&Open", BC_FOLD_OPEN); B("&Up", BC_FOLD_UP); B("To &desktop", BC_FOLD_DESK); B("&Close", BC_CLOSE); break;
    case DK_SHELL_BIN:
        B("&Restore", BC_BIN_RESTORE); B("&Delete", BC_BIN_DELETE); B(s_bin.armed ? "Sure?" : "&Empty bin", BC_BIN_EMPTY);
        B("&Close", BC_CLOSE);
        if (!s_bin.n) *def = 3;
        break;
    case DK_SHELL_TASKS: B("&Switch to", BC_TASK_SWITCH); B("&End task", BC_TASK_END); B("&Close", BC_CLOSE); break;
    case DK_SHELL_ITEM:
        if (dk_items_at(s_item)) { B("&Rename...", BC_ITEM_RENAME); B("&Open", BC_ITEM_OPEN); *def = 2; }
        B("OK", BC_CLOSE);
        break;
    default: break;
    }
    #undef B
    return n;
}

static bool can(int cmd)
{
    switch (cmd) {
    case BC_PICK_ADD: return s_pick.sel >= 0 && s_pick.sel < s_pick.n;
    case BC_FOLD_OPEN: return s_f.ok && s_f.sel >= 0 && s_f.sel < s_f.n;
    case BC_FOLD_UP: return s_f.path[0] != 0;
    case BC_FOLD_DESK: return s_f.ok;
    case BC_BIN_RESTORE: case BC_BIN_DELETE: return s_bin.sel >= 0 && s_bin.sel < s_bin.n;
    case BC_BIN_EMPTY: return s_bin.n > 0;
    case BC_TASK_SWITCH: case BC_TASK_END: return s_t.sel >= 0 && s_t.sel < s_t.n;
    default: return true;
    }
}

static void do_btn(dk_win_t *w, int cmd)
{
    switch (cmd) {
    case BC_CLOSE: ls_desk_close(w->id); break;
    case BC_OK: prompt_ok(); break;
    case BC_PICK_ADD: pick_add(); break;
    case BC_FOLD_OPEN: fold_enter(); break;
    case BC_FOLD_UP: fold_up(); break;
    case BC_FOLD_DESK: fold_shortcut(-1, -1, s_f.sel); break;
    case BC_BIN_RESTORE: bin_do(0); break;
    case BC_BIN_DELETE: bin_do(1); break;
    case BC_BIN_EMPTY: bin_do(2); break;
    case BC_TASK_SWITCH: tasks_do(0); break;
    case BC_TASK_END: tasks_do(1); break;
    case BC_ITEM_RENAME: {
        const dk_icon_t *it = dk_items_at(s_item);
        const int item = s_item;
        ls_desk_close(w->id);
        if (it) dk_prompt("Rename", "A new name for this icon:", dk_items_label(it), 15, rename_done, item);
        break;
    }
    case BC_ITEM_OPEN: { const int item = s_item; ls_desk_close(w->id); dk_items_open(item); break; }
    default: break;
    }
}

/* The menu bar's File rows, as the buttons and keys do them. */
static int cmd_btn(int cmd)
{
    switch (cmd) {
    case DKC_FOLD_OPEN: return BC_FOLD_OPEN;
    case DKC_FOLD_UP: return BC_FOLD_UP;
    case DKC_FOLD_SHORTCUT: return BC_FOLD_DESK;
    case DKC_BIN_RESTORE: return BC_BIN_RESTORE;
    case DKC_BIN_DELETE: return BC_BIN_DELETE;
    case DKC_BIN_EMPTY: return BC_BIN_EMPTY;
    case DKC_TASK_SWITCH: return BC_TASK_SWITCH;
    case DKC_TASK_END: return BC_TASK_END;
    default: return 0;
    }
}

bool dk_dialogs_can(int cmd) { return cmd == DKC_FOLD_REFRESH ? true : can(cmd_btn(cmd)); }

void dk_dialogs_cmd(int cmd)
{
    dk_win_t *w = dk_front_win();
    if (cmd == DKC_FOLD_REFRESH) { if (dk_win_shell(DK_SHELL_FOLDER)) fold_read(); return; }
    if (w && w->kind == DK_SHELL && can(cmd_btn(cmd))) do_btn(w, cmd_btn(cmd));
}

void dk_dialogs_draw(tui_surface *sf, dk_win_t *w, tui_rect c, tui_rect clip)
{
    switch (w->shell) {
    case DK_SHELL_MSG: msg_draw(sf, c, clip); break;
    case DK_SHELL_PROMPT: prompt_draw(sf, c, clip); break;
    case DK_SHELL_PICK:
        dk_text(sf, clip, c.x + 2, c.y + 1, "Put a shortcut to which app on the desktop?", dk_attr(R_FACE));
        lv_follow(w, pick_list(c), s_pick.n, &s_pick.top);
        dk_listview(sf, clip, pick_list(c), PICK_COLS, 2, s_pick.n, s_pick.top, s_pick.sel,
                    w == dk_front_win() && !*focus_of(w), pick_cell, NULL);
        break;
    case DK_SHELL_FOLDER: fold_draw(sf, w, c, clip); break;
    case DK_SHELL_BIN: bin_draw(sf, w, c, clip); break;
    case DK_SHELL_TASKS: tasks_draw(sf, w, c, clip); break;
    case DK_SHELL_ITEM: item_draw(sf, c, clip); break;
    default: break;
    }
    draw_buttons(sf, w, c, clip);
}

/* A press, before it is known to be a click or a drag: a list row under it
   is selected at once, a scrollbar answers at once, and the folder window
   remembers which row for a drag. */
void dk_dialogs_press(dk_win_t *w, tui_rect c, int col, int row)
{
    int i;
    s_sb_drag = 0;
    switch (w->shell) {
    case DK_SHELL_FOLDER:
        i = s_f.ok ? lv_press(w, fold_list(c), s_f.n, &s_f.top, &s_f.sel, col, row) : -1;
        s_f.press = i >= 0 ? i : -1;
        break;
    case DK_SHELL_PICK: lv_press(w, pick_list(c), s_pick.n, &s_pick.top, &s_pick.sel, col, row); break;
    case DK_SHELL_BIN: if (lv_press(w, bin_list(c), s_bin.n, &s_bin.top, &s_bin.sel, col, row) >= 0) s_bin.armed = false; break;
    case DK_SHELL_TASKS: lv_press(w, tasks_list(c), s_t.n, &s_t.top, &s_t.sel, col, row); break;
    case DK_SHELL_PROMPT:
        if (s_p.run) {
            run_hit_t hits[RUN_MAX];
            const int hn = run_hits(hits);
            if ((i = list_hit(run_list(c), hn, 0, col, row)) >= 0) s_p.sel = i;
        }
        break;
    default: break;
    }
}

bool dk_dialogs_tap(dk_win_t *w, tui_rect c, int col, int row)
{
    const bool dbl = dk_click_double();
    if (w->shell < DK_SHELL_PROMPT || w->shell > DK_SHELL_ITEM || w->shell == DK_SHELL_CPL) return false;
    if (tap_buttons(w, c, col, row)) { if (w->shell == DK_SHELL_FOLDER) s_f.press = -1; return true; }
    switch (w->shell) {
    case DK_SHELL_PROMPT:
        if (s_p.run && dbl && tui_rect_contains(run_list(c), col, row)) run_go();
        return true;
    case DK_SHELL_PICK:
        if (dbl && dk_listview_hit(pick_list(c), s_pick.n, s_pick.top, col, row) >= 0) pick_add();
        return true;
    case DK_SHELL_FOLDER:
        if (dbl && dk_listview_hit(fold_list(c), s_f.n, s_f.top, col, row) >= 0) fold_enter();
        s_f.press = -1;
        return true;
    case DK_SHELL_BIN:
        if (dbl && dk_listview_hit(bin_list(c), s_bin.n, s_bin.top, col, row) >= 0) bin_do(0);
        return true;
    case DK_SHELL_TASKS:
        if (dbl && dk_listview_hit(tasks_list(c), s_t.n, s_t.top, col, row) >= 0) tasks_do(0);
        return true;
    default: return true;
    }
}

bool dk_dialogs_key(dk_win_t *w, ls_tk_t k, char ch)
{
    const bool alt = (dk_mods() & LS_IMOD_ALT) != 0;
    if (w->shell < DK_SHELL_PROMPT || w->shell > DK_SHELL_ITEM || w->shell == DK_SHELL_CPL) return false;
    /* a text field takes plain letters; everything else goes to the ring first */
    if ((k == LS_TK_TAB || alt || *focus_of(w) > 0) && key_buttons(w, k, ch)) return true;
    if (w->shell == DK_SHELL_PROMPT) return prompt_key(k, ch);
    if (k == LS_TK_ESC) { ls_desk_close(w->id); return true; }
    tui_rect c;
    dk_win_rects(w, NULL, &c);
    switch (w->shell) {
    case DK_SHELL_MSG:
        if (k == LS_TK_ENTER || (k == LS_TK_CHAR && ch == ' ')) ls_desk_close(w->id);
        return true;
    case DK_SHELL_PICK:
        if (list_key(k, s_pick.n, dk_listview_rows(pick_list(c)), &s_pick.sel, &s_pick.top)) return true;
        if (k == LS_TK_ENTER) pick_add();
        else if (k == LS_TK_CHAR) {
            /* a letter jumps to the next app that starts with it */
            for (int d = 1; d <= s_pick.n; d++) {
                const int i = (s_pick.sel + d) % s_pick.n;
                if (s_pick.app[i]->name && toupper((unsigned char)s_pick.app[i]->name[0]) == toupper((unsigned char)ch)) { s_pick.sel = i; break; }
            }
            dk_list_keep(s_pick.n, dk_listview_rows(pick_list(c)), s_pick.sel, &s_pick.top);
        }
        return true;
    case DK_SHELL_FOLDER:
        if (list_key(k, s_f.n, dk_listview_rows(fold_list(c)), &s_f.sel, &s_f.top)) return true;
        if (k == LS_TK_ENTER || k == LS_TK_RIGHT) fold_enter();
        else if (k == LS_TK_BACKSPACE || k == LS_TK_LEFT) fold_up();
        else if (k == LS_TK_F5) fold_read();
        else if (k == LS_TK_CHAR && (ch == 'd' || ch == 'D')) fold_shortcut(-1, -1, s_f.sel);
        else if (k == LS_TK_CHAR && s_f.n) {
            /* any other letter: the next name that starts with it */
            for (int d = 1; d <= s_f.n; d++) {
                const int i = ((s_f.sel < 0 ? 0 : s_f.sel) + d) % s_f.n;
                if (toupper((unsigned char)s_f.ent[i].name[0]) == toupper((unsigned char)ch)) { s_f.sel = i; break; }
            }
            dk_list_keep(s_f.n, dk_listview_rows(fold_list(c)), s_f.sel, &s_f.top);
        }
        return true;
    case DK_SHELL_BIN:
        if (list_key(k, s_bin.n, dk_listview_rows(bin_list(c)), &s_bin.sel, &s_bin.top)) { s_bin.armed = false; return true; }
        if (k == LS_TK_ENTER) bin_do(0);
        else if (k == LS_TK_DELETE) bin_do(1);
        return true;
    case DK_SHELL_TASKS:
        if (list_key(k, s_t.n, dk_listview_rows(tasks_list(c)), &s_t.sel, &s_t.top)) return true;
        if (k == LS_TK_ENTER) tasks_do(0);
        else if (k == LS_TK_DELETE) tasks_do(1);
        return true;
    case DK_SHELL_ITEM:
        if (k == LS_TK_ENTER) ls_desk_close(w->id);
        return true;
    default: return false;
    }
}

void dk_dialogs_closed(dk_win_t *w)
{
    *focus_of(w) = 0;
    if (w->shell == DK_SHELL_FOLDER) {
        dk_psram_free(s_f.ent);
        s_f.ent = NULL;
        s_f.n = 0;
        s_f.press = -1;
    }
}
