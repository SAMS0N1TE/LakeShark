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

static uint8_t on_face(uint8_t fg) { return (uint8_t)((dk_attr(R_FACE) & 0xF0) | (fg & 0x0F)); }
static uint8_t hue_on_face(uint8_t hue)
{
    /* an app's colour as text on the face: readable on silver too */
    uint8_t c = hue & 7;
    if (!dk_light_face()) return on_face((uint8_t)(c == TUI_BLACK ? TUI_WHITE : c | TUI_BRIGHT));
    if (c == TUI_YELLOW) c = TUI_RED;
    if (c == TUI_CYAN) c = TUI_BLUE;
    if (c == TUI_WHITE) c = TUI_BLACK;
    return on_face(c);
}

/* Buttons sit bottom right, numbered from the right edge. */
#define BTN_W 13
static tui_rect btn(tui_rect c, int from_right) { return tui_rect_make(c.x + c.w - (BTN_W + 1) * (from_right + 1), c.y + c.h - 3, BTN_W, 3); }

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
    dk_frame(sf, clip, r, false);
    const tui_rect in = tui_rect_intersect(clip, dk_inner(r));
    const int rows = r.h - 2, x = r.x + 1, w = r.w - 2;
    const bool bar = n > rows;
    const int tw = bar ? w - 1 : w;
    for (int k = 0; k < rows; k++) {
        const int i = top + k, y = r.y + 1 + k;
        if (i >= n) break;
        dk_row_t d = {0};
        row(i, &d, ctx);
        const bool s = i == sel;
        const uint8_t face = s ? dk_attr(R_SELECT) : dk_attr(R_FACE);
        const tui_rect rr = tui_rect_intersect(in, tui_rect_make(x, y, tw, 1));
        if (s) dk_fill(sf, rr, face);
        if (d.glyph) {
            const uint8_t ga = s ? face : hue_on_face(d.hue);
            if (d.glyph < 0x80) tui_put_char(sf, rr, x + 1, y, (char)d.glyph, ga);
            else ls_box_put(sf, rr, x + 1, y, d.glyph, ga);
        }
        int rx = x + tw - 1;
        if (d.right && d.right[0]) {
            const int len = dk_text_len(d.right);
            dk_text(sf, rr, rx - len, y, d.right, s ? face : dk_attr(R_STRONG));
            rx -= len + 1;
        }
        dk_text(sf, tui_rect_intersect(rr, tui_rect_make(x, y, rx - x, 1)), x + 3, y, d.text ? d.text : "", face);
    }
    if (bar) {
        /* the bar: a thin track, and a thumb as long as the part in view */
        const int bx = r.x + r.w - 2;
        int th = rows * rows / n, ty = top * rows / n;
        if (th < 1) th = 1;
        if (ty + th > rows) ty = rows - th;
        for (int k = 0; k < rows; k++)
            ls_box_put(sf, in, bx, r.y + 1 + k, k >= ty && k < ty + th ? LS_BOX_DBL_V : LS_BOX_V,
                       k >= ty && k < ty + th ? dk_attr(R_STRONG) : dk_attr(R_SHADOW));
    }
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
    case LS_TK_UP: case LS_TK_WHEEL_UP: s = s <= 0 ? 0 : s - 1; break;
    case LS_TK_DOWN: case LS_TK_WHEEL_DOWN: s = s + 1; break;
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
    dk_button(sf, clip, btn(c, 0), "OK", true, dk_pressed(btn(c, 0)));
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
static tui_rect run_list(tui_rect c) { return tui_rect_make(c.x + 2, c.y + 5, c.w - 4, c.h - 9); }

static void prompt_draw(tui_surface *sf, tui_rect c, tui_rect clip)
{
    dk_text(sf, clip, c.x + 2, c.y + 1, s_p.label, dk_attr(R_FACE));
    const tui_rect f = prompt_field(c);
    dk_frame(sf, clip, f, false);
    const tui_rect in = tui_rect_intersect(clip, dk_inner(f));
    const int n = dk_text(sf, in, f.x + 2, f.y + 1, s_p.text, dk_attr(R_STRONG));
    ls_box_put(sf, in, f.x + 2 + n, f.y + 1, LS_EDGE_B, dk_attr(R_ACCEL));       /* where the next letter goes */
    if (s_p.run) {
        run_hit_t hits[RUN_MAX];
        const int hn = run_hits(hits);
        if (s_p.sel >= hn) s_p.sel = hn - 1;
        if (hn) dk_list_draw(sf, clip, run_list(c), hn, 0, s_p.sel, run_row, hits);
        else {
            dk_frame(sf, clip, run_list(c), false);
            dk_text(sf, clip, c.x + 5, c.y + 7, "fm   map   control   taskmgr   folder", dk_attr(R_DISABLED));
            dk_text(sf, clip, c.x + 5, c.y + 8, "or any console word: it runs in TERMINAL", dk_attr(R_DISABLED));
        }
    }
    dk_button(sf, clip, btn(c, 1), s_p.run ? "Run" : "OK", true, dk_pressed(btn(c, 1)));
    dk_button(sf, clip, btn(c, 0), "Cancel", false, dk_pressed(btn(c, 0)));
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
        return true;
    }
    if (k == LS_TK_BACKSPACE || k == LS_TK_DELETE) { if (n) s_p.text[n - 1] = 0; s_p.sel = 0; return true; }
    if (k == LS_TK_ENTER) { prompt_ok(); return true; }
    if (k == LS_TK_ESC) { close_shell(DK_SHELL_PROMPT); return true; }
    if (s_p.run && (k == LS_TK_UP || k == LS_TK_WHEEL_UP)) { if (s_p.sel > 0) s_p.sel--; return true; }
    if (s_p.run && (k == LS_TK_DOWN || k == LS_TK_TAB || k == LS_TK_WHEEL_DOWN)) { s_p.sel++; return true; }
    return true;
}

/* -------------------------------------------------- New shortcut (apps) -- */

EXT_RAM_BSS_ATTR static struct { const ls_app_t *app[LS_TUI_MAX_SCREENS]; uint8_t group[LS_TUI_MAX_SCREENS]; int n, sel, top, x, y; } s_pick;

void dk_pick_open(int x, int y)
{
    s_pick.n = 0;
    for (int g = 0; g < 5; g++) {
        const ls_app_t *list[LS_TUI_MAX_SCREENS];
        const int n = dk_group_apps(g, list, LS_TUI_MAX_SCREENS);
        for (int i = 0; i < n && s_pick.n < LS_TUI_MAX_SCREENS; i++) {
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

static void pick_row(int i, dk_row_t *o, void *ctx)
{
    (void)ctx;
    o->text = s_pick.app[i]->name;
    o->right = dk_group_name(s_pick.group[i]);
    o->hue = s_pick.app[i]->hue;
    o->glyph = LS_SHAPE_DOT;
}

static tui_rect pick_list(tui_rect c) { return tui_rect_make(c.x + 2, c.y + 2, c.w - 4, c.h - 6); }

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
typedef struct { char name[40]; bool dir; } fold_ent_t;
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
    dk_open_shell(DK_SHELL_FOLDER, "SD Card", 64, 24);
}

static void fold_row(int i, dk_row_t *o, void *ctx)
{
    (void)ctx;
    const fold_ent_t *e = &s_f.ent[i];
    o->text = e->name;
    o->right = e->dir ? "folder" : "";
    o->hue = e->dir ? TUI_YELLOW : TUI_WHITE;
    o->glyph = e->dir ? LS_SHAPE_RIGHT : LS_SHAPE_DOT;
}

static tui_rect fold_list(tui_rect c) { return tui_rect_make(c.x + 2, c.y + 2, c.w - 4, c.h - 7); }

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

static void fold_draw(tui_surface *sf, tui_rect c, tui_rect clip)
{
    /* the path, as a band: a card, then each folder down to this one */
    const tui_rect band = tui_rect_intersect(clip, tui_rect_make(c.x + 2, c.y + 1, c.w - 4, 1));
    int x = c.x + 2;
    x += dk_text(sf, band, x, c.y + 1, "SD card  ", hue_on_face(TUI_CYAN));
    x += dk_text(sf, band, x, c.y + 1, s_f.path[0] ? s_f.path : "/", dk_attr(R_STRONG));
    const tui_rect l = fold_list(c);
    if (!s_f.ok) {
        dk_frame(sf, clip, l, false);
        dk_pic(sf, clip, l.x + 3, l.y + 2, DK_PIC_CARD, on_face(TUI_CYAN));
        dk_text(sf, clip, l.x + 16, l.y + 3, "No card, or no such folder.", dk_attr(R_WARN));
        dk_text(sf, clip, l.x + 16, l.y + 4, "Put a card in and open this again.", dk_attr(R_FACE));
    } else if (!s_f.n) {
        dk_frame(sf, clip, l, false);
        dk_text(sf, clip, l.x + 3, l.y + 2, "This folder is empty.", dk_attr(R_DISABLED));
    } else {
        dk_list_keep(s_f.n, l.h - 2, s_f.sel, &s_f.top);
        dk_list_draw(sf, clip, l, s_f.n, s_f.top, s_f.sel, fold_row, NULL);
    }
    /* what is selected, in a line: read when the selection changes, not every frame */
    char st[80];
    if (s_f.sel >= 0 && s_f.sel < s_f.n && !s_f.ent[s_f.sel].dir) {
        if (s_f.size_of != s_f.sel) {
            char full[176];
            struct stat sb;
            fold_full(full, sizeof(full), s_f.ent[s_f.sel].name);
            s_f.size = stat(full, &sb) == 0 ? (long)sb.st_size : -1;
            s_f.size_of = s_f.sel;
        }
        if (s_f.size < 0) snprintf(st, sizeof(st), "%d items", s_f.n);
        else if (s_f.size < 10240) snprintf(st, sizeof(st), "%d items   %ld bytes", s_f.n, s_f.size);
        else if (s_f.size < 10485760) snprintf(st, sizeof(st), "%d items   %ld KB", s_f.n, s_f.size / 1024);
        else snprintf(st, sizeof(st), "%d items   %ld MB", s_f.n, s_f.size / 1048576);
    } else {
        snprintf(st, sizeof(st), "%d item%s%s", s_f.n, s_f.n == 1 ? "" : "s", s_f.more ? " (the first of more)" : "");
    }
    const int sy = c.y + c.h - 5;
    dk_text(sf, clip, c.x + 2, sy, st, dk_attr(R_FACE));
    const char *hint = "drag a row onto the desktop";
    dk_text(sf, clip, c.x + c.w - 2 - dk_text_len(hint), sy, hint, dk_attr(R_DISABLED));
    dk_button(sf, clip, btn(c, 3), "Open", true, dk_pressed(btn(c, 3)));
    dk_button(sf, clip, btn(c, 2), "Up", false, dk_pressed(btn(c, 2)));
    dk_button(sf, clip, btn(c, 1), "To desktop", false, dk_pressed(btn(c, 1)));
    dk_button(sf, clip, btn(c, 0), "Close", false, dk_pressed(btn(c, 0)));
}

/* ------------------------------------------------------------------ bin -- */

EXT_RAM_BSS_ATTR static struct { dk_icon_t *it[DK_ICON_MAX]; int n, sel, top; bool armed; } s_bin;

void dk_bin_open(void)
{
    s_bin.sel = 0;
    s_bin.top = 0;
    s_bin.armed = false;
    dk_open_shell(DK_SHELL_BIN, "Recycle Bin", 58, 18);
}

static void bin_row(int i, dk_row_t *o, void *ctx)
{
    (void)ctx;
    EXT_RAM_BSS_ATTR static char what[56];
    const dk_icon_t *it = s_bin.it[i];
    dk_items_what(it, what, sizeof(what));
    o->text = dk_items_label(it);
    o->right = what;
    o->hue = it->kind == DKI_FOLDER ? TUI_YELLOW : it->kind == DKI_APP ? TUI_CYAN : TUI_WHITE;
    o->glyph = it->kind == DKI_FOLDER ? LS_SHAPE_RIGHT : LS_SHAPE_DOT;
}

static tui_rect bin_list(tui_rect c) { return tui_rect_make(c.x + 2, c.y + 2, c.w - 4, c.h - 6); }

static void bin_draw(tui_surface *sf, tui_rect c, tui_rect clip)
{
    s_bin.n = dk_items_binned(s_bin.it, DK_ICON_MAX);
    if (s_bin.n > DK_ICON_MAX) s_bin.n = DK_ICON_MAX;
    if (s_bin.sel >= s_bin.n) s_bin.sel = s_bin.n - 1;
    if (s_bin.sel < 0 && s_bin.n) s_bin.sel = 0;
    char head[64];
    snprintf(head, sizeof(head), s_bin.n ? "%d deleted shortcut%s. Their files are untouched." : "The bin is empty.",
             s_bin.n, s_bin.n == 1 ? "" : "s");
    dk_text(sf, clip, c.x + 2, c.y + 1, head, dk_attr(R_FACE));
    const tui_rect l = bin_list(c);
    if (s_bin.n) {
        dk_list_keep(s_bin.n, l.h - 2, s_bin.sel, &s_bin.top);
        dk_list_draw(sf, clip, l, s_bin.n, s_bin.top, s_bin.sel, bin_row, NULL);
    } else {
        dk_frame(sf, clip, l, false);
        dk_pic(sf, clip, l.x + (l.w - 10) / 2, l.y + 2, DK_PIC_BIN, on_face(TUI_WHITE));
    }
    dk_button(sf, clip, btn(c, 3), "Restore", s_bin.n > 0, dk_pressed(btn(c, 3)));
    dk_button(sf, clip, btn(c, 2), "Delete", false, dk_pressed(btn(c, 2)));
    dk_button(sf, clip, btn(c, 1), s_bin.armed ? "Sure?" : "Empty bin", false, s_bin.armed);
    dk_button(sf, clip, btn(c, 0), "Close", s_bin.n == 0, dk_pressed(btn(c, 0)));
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
    dk_open_shell(DK_SHELL_TASKS, "Task Manager", 68, 24);
}

static void tasks_row(int i, dk_row_t *o, void *ctx)
{
    (void)ctx;
    EXT_RAM_BSS_ATTR static char right[40];
    const dk_win_t *w = dk_win(s_t.ids[i]);
    if (!w) { o->text = "?"; return; }
    const dk_win_t *front = dk_front_win();
    const char *radio = w->app && w->app->screen ? w->app->screen->radio : NULL;
    const char *claimed = ls_tui_radio_claimed();
    const bool rx = radio && claimed && !strcmp(radio, claimed);
    snprintf(right, sizeof(right), "%s%s", w->kind == DK_SHELL ? "desk window" : w == front ? "running" : rx ? "receiving, view paused"
             : "paused", w->state == LS_DESK_WIN_MIN ? ", minimized" : "");
    o->text = w->title;
    o->right = right;
    o->hue = w->app ? w->app->hue : TUI_YELLOW;
    o->glyph = w->kind == DK_APP ? LS_SHAPE_DOT : LS_SHAPE_SQUARE;
}

static tui_rect tasks_list(tui_rect c) { return tui_rect_make(c.x + 2, c.y + 2, c.w - 4, c.h - 14 > 4 ? c.h - 14 : 4); }

static void tasks_draw(tui_surface *sf, tui_rect c, tui_rect clip)
{
    s_t.n = 0;
    ls_desk_win_info_t wi;
    for (int z = ls_desk_window_count() - 1; z >= 0; z--)       /* front first */
        if (ls_desk_window_at(z, &wi) && s_t.n < DK_WIN_SLOTS) s_t.ids[s_t.n++] = wi.id;
    if (s_t.sel >= s_t.n) s_t.sel = s_t.n - 1;
    char line[80];
    snprintf(line, sizeof(line), "%d of %d windows. One app runs live; the others hold a frame.", s_t.n, LS_DESK_MAX_WIN);
    dk_text(sf, clip, c.x + 2, c.y + 1, line, dk_attr(R_FACE));
    const tui_rect l = tasks_list(c);
    dk_list_keep(s_t.n, l.h - 2, s_t.sel, &s_t.top);
    dk_list_draw(sf, clip, l, s_t.n, s_t.top, s_t.sel, tasks_row, NULL);

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
    dk_hline(sf, clip, c.x + 2, py, c.w - 4, LS_BOX_H, dk_attr(R_SHADOW));
    dk_text(sf, clip, c.x + 4, py, " Memory ", dk_attr(R_STRONG));
    const int vx = c.x + 20, tx = c.x + c.w - 2 - PERF_N / 2 - 1;
    static const char *const NAME[2] = {"Internal RAM", "PSRAM"};
    for (int k = 0; k < 2; k++) {
        const int y = py + 1 + k * 3;
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
    const int uy = py + 7;
    snprintf(line, sizeof(line), "Up %ldd %02ld:%02ld:%02ld", p.uptime_s / 86400, p.uptime_s / 3600 % 24, p.uptime_s / 60 % 60, p.uptime_s % 60);
    dk_text(sf, clip, c.x + 3, uy, line, dk_attr(R_FACE));
    if (p.frame_us > 0) {
        snprintf(line, sizeof(line), "last frame %d.%d ms, %d cells sent", p.frame_us / 1000, p.frame_us / 100 % 10, p.cells);
        dk_text(sf, clip, vx, uy, line, dk_attr(R_FACE));
    }
    dk_button(sf, clip, btn(c, 2), "Switch to", true, dk_pressed(btn(c, 2)));
    dk_button(sf, clip, btn(c, 1), "End task", false, dk_pressed(btn(c, 1)));
    dk_button(sf, clip, btn(c, 0), "Close", false, dk_pressed(btn(c, 0)));
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
    if (!it) { dk_text(sf, clip, c.x + 2, c.y + 1, "That shortcut is gone.", dk_attr(R_DISABLED)); dk_button(sf, clip, btn(c, 0), "OK", true, dk_pressed(btn(c, 0))); return; }
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
    dk_button(sf, clip, btn(c, 2), "Rename...", false, dk_pressed(btn(c, 2)));
    dk_button(sf, clip, btn(c, 1), "Open", false, dk_pressed(btn(c, 1)));
    dk_button(sf, clip, btn(c, 0), "OK", true, dk_pressed(btn(c, 0)));
}

/* --------------------------------------------------------------- common -- */

void dk_dialogs_draw(tui_surface *sf, dk_win_t *w, tui_rect c, tui_rect clip)
{
    switch (w->shell) {
    case DK_SHELL_MSG: msg_draw(sf, c, clip); break;
    case DK_SHELL_PROMPT: prompt_draw(sf, c, clip); break;
    case DK_SHELL_PICK:
        dk_text(sf, clip, c.x + 2, c.y + 1, "Put a shortcut to which app on the desktop?", dk_attr(R_FACE));
        dk_list_keep(s_pick.n, pick_list(c).h - 2, s_pick.sel, &s_pick.top);
        dk_list_draw(sf, clip, pick_list(c), s_pick.n, s_pick.top, s_pick.sel, pick_row, NULL);
        dk_button(sf, clip, btn(c, 1), "Add", true, dk_pressed(btn(c, 1)));
        dk_button(sf, clip, btn(c, 0), "Cancel", false, dk_pressed(btn(c, 0)));
        break;
    case DK_SHELL_FOLDER: fold_draw(sf, c, clip); break;
    case DK_SHELL_BIN: bin_draw(sf, c, clip); break;
    case DK_SHELL_TASKS: tasks_draw(sf, c, clip); break;
    case DK_SHELL_ITEM: item_draw(sf, c, clip); break;
    default: break;
    }
}

/* A press, before it is known to be a click or a drag: a list row under it
   is selected at once, and the folder window remembers which for a drag. */
void dk_dialogs_press(dk_win_t *w, tui_rect c, int col, int row)
{
    int i;
    switch (w->shell) {
    case DK_SHELL_FOLDER:
        s_f.press = s_f.ok ? list_hit(fold_list(c), s_f.n, s_f.top, col, row) : -1;
        if (s_f.press >= 0) s_f.sel = s_f.press;
        break;
    case DK_SHELL_PICK: if ((i = list_hit(pick_list(c), s_pick.n, s_pick.top, col, row)) >= 0) s_pick.sel = i; break;
    case DK_SHELL_BIN: if ((i = list_hit(bin_list(c), s_bin.n, s_bin.top, col, row)) >= 0) { s_bin.sel = i; s_bin.armed = false; } break;
    case DK_SHELL_TASKS: if ((i = list_hit(tasks_list(c), s_t.n, s_t.top, col, row)) >= 0) s_t.sel = i; break;
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
    #define HIT(k) tui_rect_contains(btn(c, (k)), col, row)
    switch (w->shell) {
    case DK_SHELL_MSG:
        if (HIT(0)) ls_desk_close(w->id);
        return true;
    case DK_SHELL_PROMPT:
        if (HIT(1)) prompt_ok();
        else if (HIT(0)) ls_desk_close(w->id);
        else if (s_p.run && dbl && tui_rect_contains(run_list(c), col, row)) run_go();
        return true;
    case DK_SHELL_PICK:
        if (HIT(1) || (dbl && list_hit(pick_list(c), s_pick.n, s_pick.top, col, row) >= 0)) pick_add();
        else if (HIT(0)) ls_desk_close(w->id);
        return true;
    case DK_SHELL_FOLDER:
        if (HIT(3) || (dbl && list_hit(fold_list(c), s_f.n, s_f.top, col, row) >= 0)) fold_enter();
        else if (HIT(2)) fold_up();
        else if (HIT(1)) fold_shortcut(-1, -1, s_f.sel);
        else if (HIT(0)) ls_desk_close(w->id);
        s_f.press = -1;
        return true;
    case DK_SHELL_BIN:
        if (HIT(3) || (dbl && list_hit(bin_list(c), s_bin.n, s_bin.top, col, row) >= 0)) bin_do(0);
        else if (HIT(2)) bin_do(1);
        else if (HIT(1)) bin_do(2);
        else if (HIT(0)) ls_desk_close(w->id);
        return true;
    case DK_SHELL_TASKS:
        if (HIT(2) || (dbl && list_hit(tasks_list(c), s_t.n, s_t.top, col, row) >= 0)) tasks_do(0);
        else if (HIT(1)) tasks_do(1);
        else if (HIT(0)) ls_desk_close(w->id);
        return true;
    case DK_SHELL_ITEM:
        if (HIT(2)) {
            const dk_icon_t *it = dk_items_at(s_item);
            const int item = s_item;
            ls_desk_close(w->id);
            if (it) dk_prompt("Rename", "A new name for this icon:", dk_items_label(it), 15, rename_done, item);
        } else if (HIT(1)) { const int item = s_item; ls_desk_close(w->id); dk_items_open(item); }
        else if (HIT(0)) ls_desk_close(w->id);
        return true;
    default: return false;
    }
    #undef HIT
}

bool dk_dialogs_key(dk_win_t *w, ls_tk_t k, char ch)
{
    if (w->shell == DK_SHELL_PROMPT) return prompt_key(k, ch);
    if (k == LS_TK_ESC) { ls_desk_close(w->id); return true; }
    tui_rect c;
    dk_win_rects(w, NULL, &c);
    switch (w->shell) {
    case DK_SHELL_MSG:
        if (k == LS_TK_ENTER || (k == LS_TK_CHAR && ch == ' ')) ls_desk_close(w->id);
        return true;
    case DK_SHELL_PICK:
        if (list_key(k, s_pick.n, pick_list(c).h - 2, &s_pick.sel, &s_pick.top)) return true;
        if (k == LS_TK_ENTER) pick_add();
        else if (k == LS_TK_CHAR) {
            /* a letter jumps to the next app that starts with it */
            for (int d = 1; d <= s_pick.n; d++) {
                const int i = (s_pick.sel + d) % s_pick.n;
                if (s_pick.app[i]->name && toupper((unsigned char)s_pick.app[i]->name[0]) == toupper((unsigned char)ch)) { s_pick.sel = i; break; }
            }
        }
        return true;
    case DK_SHELL_FOLDER:
        if (list_key(k, s_f.n, fold_list(c).h - 2, &s_f.sel, &s_f.top)) return true;
        if (k == LS_TK_ENTER || k == LS_TK_RIGHT) fold_enter();
        else if (k == LS_TK_BACKSPACE || k == LS_TK_LEFT) fold_up();
        else if (k == LS_TK_CHAR && (ch == 'd' || ch == 'D')) fold_shortcut(-1, -1, s_f.sel);
        return true;
    case DK_SHELL_BIN:
        if (list_key(k, s_bin.n, bin_list(c).h - 2, &s_bin.sel, &s_bin.top)) { s_bin.armed = false; return true; }
        if (k == LS_TK_ENTER) bin_do(0);
        else if (k == LS_TK_DELETE) bin_do(1);
        return true;
    case DK_SHELL_TASKS:
        if (list_key(k, s_t.n, tasks_list(c).h - 2, &s_t.sel, &s_t.top)) return true;
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
    if (w->shell == DK_SHELL_FOLDER) {
        dk_psram_free(s_f.ent);
        s_f.ent = NULL;
        s_f.n = 0;
        s_f.press = -1;
    }
}
