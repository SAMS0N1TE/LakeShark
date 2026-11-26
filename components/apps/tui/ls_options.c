/* See ls_options.h. The list, the rows that belong in it, and bringing it
   back after a choice, a keypad or a list of the app's own. */
#include "ls_options.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "ls_keyboard.h"
#include "ls_numpad.h"
#include "ls_picker.h"
#include "ls_radio_select.h"
#include "ls_i18n.h"

/* The context the list was opened on, and which table row each list row is.
   The tables are the apps' and const; this is only where the list is. */
static const ls_opt_ctx_t *s_ctx;
static EXT_RAM_BSS_ATTR uint8_t s_row_opt[LS_PICKER_MAX];
static int s_rows;
/* Settings are telemetry, not a frame clock. Edits still update immediately. */
static int64_t s_values_us;
/* The list row a keypad, keyboard or list of the app's own was opened from,
   for the list to come back on when that closes; -1 when nothing waits. */
static int s_back = -1;
/* The screen it was opened on: a list does not come back over another app. */
static int s_screen = -1;
/* The row a keypad or keyboard is writing to. */
static int s_edit = -1;
/* The lists above the one up now, and the row of each that opened the next:
   where BACK goes. */
#define DEPTH 4
static const ls_opt_ctx_t *s_up[DEPTH];
static int s_up_row[DEPTH];
static int s_depth;
static void (*s_root_back)(void);

/* ------------------------------------------------------------ the rows -- */

ls_rsel_radio_t ls_opt_radio(const ls_opt_ctx_t *ctx)
{
    if (!ctx) return LS_RSEL_NONE;
    if (ctx->job >= 0 && ctx->job < LS_RSEL_JOBS) return ls_rsel_effective((ls_rsel_job_t)ctx->job);
    return ctx->radio;
}

static bool row_for(const ls_opt_t *o, ls_rsel_radio_t radio)
{
    if (o->kind == LS_OPT_MENU && ls_opt_count(o->sub) <= 0) return false;
    if (!o->radios) return true;
    return radio < LS_RSEL_RADIOS && (o->radios & LS_OPT_RADIO(radio));
}

bool ls_opt_applies(const ls_opt_ctx_t *ctx, int i)
{
    if (!ctx || !ctx->opt || i < 0 || i >= ctx->n) return false;
    return row_for(&ctx->opt[i], ls_opt_radio(ctx));
}

int ls_opt_count(const ls_opt_ctx_t *ctx)
{
    if (!ctx || !ctx->opt) return 0;
    const ls_rsel_radio_t radio = ls_opt_radio(ctx);
    int n = 0;
    for (int i = 0; i < ctx->n; i++)
        if (row_for(&ctx->opt[i], radio)) n++;
    return n;
}

ls_btn_t ls_opt_button(const ls_opt_ctx_t *ctx)
{
    const char *under = ctx ? (ctx->tag ? ctx->tag : ctx->name) : NULL;
    return (ls_btn_t){ "OPTIONS", under, LS_OPT_KEY, false, false };
}

void ls_opt_value(const ls_opt_t *o, char *out, size_t n)
{
    if (!out || !n) return;
    out[0] = 0;
    if (!o) return;
    if (o->show) { o->show(o, out, n); return; }
    switch (o->kind) {
    case LS_OPT_CYCLE: {
        const int v = o->get ? o->get(o) : -1;
        snprintf(out, n, "%s", o->names && v >= 0 && v < o->n ? o->names[v] : "?");
        break;
    }
    case LS_OPT_TOGGLE: {
        const int v = o->get && o->get(o) ? 1 : 0;
        snprintf(out, n, "%s", o->names ? o->names[v] : v ? "ON" : "OFF");
        break;
    }
    case LS_OPT_NUMBER:
    case LS_OPT_LEVEL: {
        const double v = o->num ? o->num(o) : 0;
        if (v == (double)(long)v) snprintf(out, n, "%ld", (long)v);
        else snprintf(out, n, "%.1f", v);
        break;
    }
    case LS_OPT_MENU:
        snprintf(out, n, ">");
        break;
    case LS_OPT_TEXT: {
        const char *t = o->text ? o->text(o) : NULL;
        /* Empty is still a row that opens an editor: show the chevron the
           submenu rows show, not a blank. */
        snprintf(out, n, "%s", t && t[0] ? t : ">");
        break;
    }
    case LS_OPT_ACTION:
        break;
    }
}

/* What a row says beside its label: why it cannot be used, or its value. */
static void detail(const ls_opt_t *o, char *out, size_t n)
{
    const char *why = o->why_not ? o->why_not(o) : NULL;
    if (why) snprintf(out, n, "%s", why);
    else ls_opt_value(o, out, n);
}

/* ------------------------------------------------------------ the list -- */

static void picked(int row);
static void stepped(int row, int dir);
static void dlg_form_on(void);
static void dlg_begin(const ls_opt_ctx_t *ctx);
static void dlg_end(void);
static void go_up(void);
static void navigate(int row,int dir);

static void fill(const ls_opt_ctx_t *ctx)
{
    char title[LS_PICKER_DETAIL];
    const ls_rsel_radio_t radio = ls_opt_radio(ctx);
    const char *rname = radio < LS_RSEL_RADIOS ? ls_rsel_name(radio) : NULL;
    if (s_depth > 0) {
        const char *parent = ls_tr(s_up[s_depth - 1]->name);
        const char *name = ls_tr(ctx->name);
        if (rname && strlen(parent) + strlen(name) + strlen(rname) + 6 < sizeof(title))
            snprintf(title, sizeof(title), "%s > %s / %s", parent, name, rname);
        else
            snprintf(title, sizeof(title), "%s > %s", parent, name);
    } else if (rname) {
        snprintf(title, sizeof(title), ls_tr("%s OPTIONS / %s"), ls_tr(ctx->name), rname);
    } else {
        snprintf(title, sizeof(title), ls_tr("%s OPTIONS"), ls_tr(ctx->name));
    }
    ls_picker_open(title, picked);
    const ls_opt_ctx_t *root=s_depth?s_up[0]:ctx;
    ls_picker_header(root->draw_header);
    ls_picker_preview(ctx->preview ? ctx->preview : root->preview);
    if (s_depth > 0 || ctx->back_to_screen || s_root_back) ls_picker_back(go_up);
    s_ctx = ctx;
    s_values_us = esp_timer_get_time();
    if(ctx->menu_arrows) ls_picker_navigation(navigate);
    s_rows = 0;
    char d[LS_PICKER_DETAIL];
    for (int i = 0; i < ctx->n && s_rows < LS_PICKER_MAX; i++) {
        const ls_opt_t *o = &ctx->opt[i];
        if (!row_for(o, radio)) continue;
        detail(o, d, sizeof(d));
        ls_picker_add(o->label, d);
        if ((o->kind == LS_OPT_LEVEL || (o->kind == LS_OPT_CYCLE && o->step > 0)) &&
            !(o->why_not && o->why_not(o)))
            ls_picker_stepper(s_rows, stepped);
        s_row_opt[s_rows++] = (uint8_t)i;
    }
    dlg_form_on();                       /* a desk window: the standard dialog over the list */
}

/* BACK: the list above, on the row that opened this one. */
static void go_up(void)
{
    if (s_depth <= 0) {
        void (*cb)(void)=s_root_back;ls_opt_close();if(cb) cb();return;
    }
    s_depth--;
    const ls_opt_ctx_t *ctx = s_up[s_depth];
    const int row = s_up_row[s_depth];
    s_back = -1;
    fill(ctx);
    ls_picker_select(row);
}

static void navigate(int row,int dir)
{
    if(dir<0) { go_up(); return; }
    if(row>=0 && row<s_rows && s_ctx->opt[s_row_opt[row]].kind==LS_OPT_MENU)
        picked(row);
}

/* The list again, on the row that was chosen, saying `note` when there is
   one. */
static void reopen(int row, const char *note)
{
    const ls_opt_ctx_t *ctx = s_ctx;
    s_back = -1;
    if (!ctx) return;
    fill(ctx);
    ls_picker_select(row);
    if (note) ls_picker_note(note);
}

static const ls_opt_t *row_opt(int row)
{
    if (!s_ctx || row < 0 || row >= s_rows) return NULL;
    return &s_ctx->opt[s_row_opt[row]];
}

/* A level's < or >: one step, held to its range, and the list stays up. */
static void stepped(int row, int dir)
{
    const ls_opt_t *o = row_opt(row);
    if (!o) return;
    if (o->why_not && o->why_not(o)) return;
    if (o->kind == LS_OPT_CYCLE) {
        if (!o->get || !o->set || o->n <= 0) return;
        o->set(o, (o->get(o) + dir + o->n) % o->n);
        reopen(row, NULL);
        return;
    }
    if (!o->num || !o->set_num) return;
    const double step = o->step > 0 ? o->step : 1;
    double v = o->num(o) + dir * step;
    if (o->lo < o->hi) {
        if (v < o->lo) v = o->lo;
        if (v > o->hi) v = o->hi;
    }
    o->set_num(o, v);
    char d[LS_PICKER_DETAIL];
    detail(o, d, sizeof(d));
    ls_picker_set_detail(row, d);
}

static void number_done(double v)
{
    const ls_opt_t *o = row_opt(s_edit);
    const int row = s_edit;
    s_edit = -1;
    if (!o) { s_back = -1; return; }
    if (o->lo < o->hi && !(v >= o->lo && v <= o->hi)) {
        char note[LS_PICKER_DETAIL + 32], lo[16], hi[16];
        snprintf(lo, sizeof(lo), "%g", o->lo);
        snprintf(hi, sizeof(hi), "%g", o->hi);
        snprintf(note, sizeof(note), ls_tr("%s: %s to %s"), ls_tr(o->label), lo, hi);
        reopen(row, note);
        return;
    }
    if (o->set_num) o->set_num(o, v);
    reopen(row, NULL);
}

static void text_done(const char *text)
{
    const ls_opt_t *o = row_opt(s_edit);
    const int row = s_edit;
    s_edit = -1;
    if (!o) { s_back = -1; return; }
    if (o->set_text) o->set_text(o, text ? text : "");
    reopen(row, NULL);
}

static bool overlay_up(void)
{
    return ls_picker_active() || ls_numpad_active() || ls_keyboard_active();
}

static void picked(int row)
{
    const ls_opt_t *o = row_opt(row);
    if (!o) return;
    const char *why = o->why_not ? o->why_not(o) : NULL;
    if (why) {
        /* The list stays, with the reason above it, and nothing changes. */
        char note[LS_PICKER_DETAIL * 2];
        snprintf(note, sizeof(note), "%s: %s", ls_tr(o->label), ls_tr(why));
        reopen(row, note);
        return;
    }
    switch (o->kind) {
    case LS_OPT_CYCLE:
        if (o->set && o->n > 0) {
            const int v = o->get ? o->get(o) : -1;
            o->set(o, v >= 0 && v + 1 < o->n ? v + 1 : 0);
        }
        reopen(row, NULL);
        return;
    case LS_OPT_TOGGLE:
        if (o->set) o->set(o, o->get && o->get(o) ? 0 : 1);
        reopen(row, NULL);
        return;
    case LS_OPT_NUMBER:
    case LS_OPT_LEVEL:
        s_edit = row;
        s_back = row;   /* a cancelled keypad brings the list back too */
        ls_numpad_open(ls_tr(o->label), o->unit ? ls_tr(o->unit) : "", o->num ? o->num(o) : 0, number_done);
        return;
    case LS_OPT_MENU:
        if (o->sub && s_depth < DEPTH) {
            s_up[s_depth] = s_ctx;
            s_up_row[s_depth] = row;
            s_depth++;
            s_back = -1;
            fill(o->sub);
            return;
        }
        reopen(row, NULL);
        return;
    case LS_OPT_TEXT: {
        s_edit = row;
        s_back = row;
        const char *t = o->text ? o->text(o) : "";
        ls_keyboard_open(ls_tr(o->label), t ? t : "", o->max > 0 ? o->max : 32, text_done);
        return;
    }
    case LS_OPT_ACTION:
        if (o->act) o->act(o);
        if (o->leaves) { s_back = -1; return; }
        /* A list or keypad of the app's own: the list waits for it. */
        if (overlay_up()) { s_back = row; return; }
        reopen(row, NULL);
        return;
    }
}

void ls_opt_open(const ls_opt_ctx_t *ctx)
{
    if (ls_opt_count(ctx) <= 0) return;
    s_back = -1;
    s_edit = -1;
    s_depth = 0;s_root_back=NULL;
    s_screen = ls_tui_screen_current();
    fill(ctx);
    if (ls_tui_compact()) dlg_begin(ctx);
}

void ls_opt_back(void (*on_back)(void)) { s_root_back=on_back;if(s_ctx) fill(s_ctx); }

bool ls_opt_key(const ls_opt_ctx_t *ctx, char ch)
{
    if (ch != LS_OPT_KEY && ch != 'O') return false;
    if (ls_opt_count(ctx) <= 0) return false;
    ls_opt_open(ctx);
    return true;
}

bool ls_opt_returning(void)
{
    return s_back >= 0 && s_ctx != NULL;
}

void ls_opt_close(void)
{
    if (s_ctx && ls_picker_is(picked)) ls_picker_close();
    s_back = -1;
    s_edit = -1;
    s_depth = 0;
    s_ctx = NULL;s_root_back=NULL;
    dlg_end();
}

bool ls_opt_covers_screen(void)
{
    const ls_opt_ctx_t *root = s_depth > 0 ? s_up[0] : s_ctx;
    return root && root->covers_screen && ls_picker_is(picked);
}

void ls_opt_poll(void)
{
    if (!s_ctx) return;
    if (ls_picker_is(picked)) {
        const int64_t now = esp_timer_get_time();
        const ls_opt_ctx_t *root = s_depth > 0 ? s_up[0] : s_ctx;
        if (now >= s_values_us && now - s_values_us < (int64_t)root->refresh_ms * 1000) return;
        s_values_us = now;
        /* The values follow the settings while the list is up: most of these
           are requests a worker takes a moment to carry out. */
        char d[LS_PICKER_DETAIL];
        for (int r = 0; r < s_rows; r++) {
            detail(&s_ctx->opt[s_row_opt[r]], d, sizeof(d));
            ls_picker_set_detail(r, d);
        }
        return;
    }
    if (s_back < 0) return;
    if (overlay_up()) return;
    if (ls_tui_screen_current() != s_screen) { s_back = -1; return; }
    reopen(s_back, NULL);
}

/* ====================================================== THE DESK DIALOG ==

   In a desk window OPTIONS is one standard dialog, the same for every app:
   "<APP> Options" in the title bar, the categories (the list's MENU rows)
   in a list box at the left, the settings of the one chosen in a group box
   as the controls a desktop has - a check box for an on/off, a drop-down
   for a choice, a trackbar with its value for a level, an edit with [...]
   for a number or words, a push button for an action - and OK, Cancel and
   Apply at the bottom right. A change is made as it is chosen (a colour, a
   theme, a layer shows at once) and the dialog stays; Cancel puts back
   every value it changed, OK keeps them, Apply keeps them and stays.

   It is the list underneath (ls_picker with this form over it), so the
   keypad and keyboard a number or a name opens, and coming back after
   them, are the list's as before. */
#if LS_TUI_HAS_COMPACT

#include "ls_deskui.h"
#include "ls_box.h"

#define DLG_SNAP 64
#define DLG_CATS 12
EXT_RAM_BSS_ATTR static struct { const ls_opt_t *o; double v; } s_snap[DLG_SNAP];
EXT_RAM_BSS_ATTR static int s_nsnap;
EXT_RAM_BSS_ATTR static const ls_opt_ctx_t *s_dlg_root;          /* the context OPTIONS was opened on */
EXT_RAM_BSS_ATTR static int s_dlg_focus;                         /* a row; DLG_F_OK.. a button; DLG_F_CATS the list */
EXT_RAM_BSS_ATTR static int s_dlg_top;                           /* the first row in view */
enum { DLG_F_CATS = -1, DLG_F_OK = 1000, DLG_F_CANCEL, DLG_F_APPLY };
typedef struct { tui_rect row, a, b, bar, val; } dlg_hit_t;
EXT_RAM_BSS_ATTR static dlg_hit_t s_dlg_hit[LS_PICKER_MAX];
EXT_RAM_BSS_ATTR static tui_rect s_dlg_cat_hit[DLG_CATS];
EXT_RAM_BSS_ATTR static tui_rect s_dlg_ok, s_dlg_cancel, s_dlg_apply, s_dlg_box, s_dlg_backkey;
EXT_RAM_BSS_ATTR static int s_dlg_rows_shown;
EXT_RAM_BSS_ATTR static ls_dui_popup_t s_drop;
EXT_RAM_BSS_ATTR static ls_dui_item_t s_drop_item[24];
static int s_drop_row = -1;

static double opt_now(const ls_opt_t *o)
{
    if ((o->kind == LS_OPT_CYCLE || o->kind == LS_OPT_TOGGLE) && o->get) return o->get(o);
    if ((o->kind == LS_OPT_LEVEL || o->kind == LS_OPT_NUMBER) && o->num) return o->num(o);
    return 0;
}

static void snap_ctx(const ls_opt_ctx_t *ctx, int depth)
{
    if (!ctx || depth > 3) return;
    const ls_rsel_radio_t radio = ls_opt_radio(ctx);
    for (int i = 0; i < ctx->n; i++) {
        const ls_opt_t *o = &ctx->opt[i];
        if (!row_for(o, radio)) continue;
        if (o->kind == LS_OPT_MENU) { snap_ctx(o->sub, depth + 1); continue; }
        const bool has = ((o->kind == LS_OPT_CYCLE || o->kind == LS_OPT_TOGGLE) && o->get && o->set)
                      || ((o->kind == LS_OPT_LEVEL || o->kind == LS_OPT_NUMBER) && o->num && o->set_num);
        if (!has || s_nsnap >= DLG_SNAP) continue;
        s_snap[s_nsnap].o = o;
        s_snap[s_nsnap++].v = opt_now(o);
    }
}

static void dlg_snapshot(void) { s_nsnap = 0; snap_ctx(s_dlg_root, 0); }

static bool dlg_changed(void)
{
    for (int i = 0; i < s_nsnap; i++) if (opt_now(s_snap[i].o) != s_snap[i].v) return true;
    return false;
}

static void dlg_revert(void)
{
    for (int i = 0; i < s_nsnap; i++) {
        const ls_opt_t *o = s_snap[i].o;
        if (opt_now(o) == s_snap[i].v) continue;
        if (o->kind == LS_OPT_CYCLE || o->kind == LS_OPT_TOGGLE) o->set(o, (int)s_snap[i].v);
        else o->set_num(o, s_snap[i].v);
    }
}

/* The categories: the root itself when it has settings of its own, then
   each of its MENU rows. *row gets the root's list row of category i. */
static int dlg_cats(const char **name, int *row)
{
    const ls_opt_ctx_t *root = s_dlg_root;
    if (!root) return 0;
    const ls_rsel_radio_t radio = ls_opt_radio(root);
    int n = 0, pr = 0, own = 0;
    for (int i = 0; i < root->n; i++)
        if (row_for(&root->opt[i], radio) && root->opt[i].kind != LS_OPT_MENU) own++;
    if (own) { if (name) name[n] = "General"; if (row) row[n] = -1; n++; }
    for (int i = 0; i < root->n && n < DLG_CATS; i++) {
        const ls_opt_t *o = &root->opt[i];
        if (!row_for(o, radio)) continue;
        if (o->kind == LS_OPT_MENU) { if (name) name[n] = o->label; if (row) row[n] = pr; n++; }
        pr++;
    }
    return n;
}

/* Which category is up: the root, or the MENU row the path starts from. */
static int dlg_cat_now(void)
{
    int row[DLG_CATS];
    const int n = dlg_cats(NULL, row);
    const int at = s_depth > 0 ? s_up_row[0] : -1;
    for (int i = 0; i < n; i++) if (row[i] == at) return i;
    return 0;
}

/* A MENU row on the root page whose page is a category already: the list
   at the left has it, the page does not repeat it. */
static bool dlg_hidden(int r)
{
    const ls_opt_t *o = row_opt(r);
    return o && s_depth == 0 && o->kind == LS_OPT_MENU && dlg_cats(NULL, NULL) > 1;
}

static int dlg_first(void)
{
    for (int r = 0; r < s_rows; r++) if (!dlg_hidden(r)) return r;
    return DLG_F_OK;
}

static int dlg_cat_depth(void)
{
    int row[DLG_CATS];
    const int n = dlg_cats(NULL, row), c = dlg_cat_now();
    return n > 0 && c < n && row[c] >= 0 ? 1 : 0;
}

static void dlg_go_cat(int i)
{
    int row[DLG_CATS];
    const int n = dlg_cats(NULL, row);
    if (i < 0 || i >= n || !s_dlg_root) return;
    s_depth = 0;
    s_back = -1;
    s_dlg_top = 0;
    if (row[i] < 0) { fill(s_dlg_root); s_dlg_focus = dlg_first(); return; }
    s_ctx = s_dlg_root;
    fill(s_dlg_root);                 /* s_row_opt of the root, for the row below */
    if (row[i] >= s_rows) return;
    s_up[0] = s_dlg_root;
    s_up_row[0] = row[i];
    s_depth = 1;
    fill(s_dlg_root->opt[s_row_opt[row[i]]].sub);
    s_dlg_focus = dlg_first();
}

static void dlg_begin(const ls_opt_ctx_t *ctx)
{
    s_dlg_root = ctx;
    s_dlg_focus = 0;
    s_dlg_top = 0;
    s_drop.open = false;
    dlg_snapshot();
    int row[DLG_CATS];
    if (dlg_cats(NULL, row) > 0 && row[0] >= 0) dlg_go_cat(0);   /* no settings of its own: the first category */
    else s_dlg_focus = dlg_first();
}

static bool dlg_check(const ls_opt_t *o)
{
    if (o->kind != LS_OPT_TOGGLE) return false;
    if (!o->names) return true;
    const char *n0 = o->names[0];
    return n0 && (!strcmp(n0, "OFF") || !strcmp(n0, "NO") || !strcmp(n0, "off") || !strcmp(n0, "Off"));
}

/* Words in the caption's case: "SCAN OPTIONS" reads "Scan options" on a title. */
static void dlg_case(char *s)
{
    bool up = true;
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z' && !up) *s = (char)(*s + 32);
        up = *s == ' ' || *s == '/' || *s == '>' || *s == '(';
    }
}

static void dlg_cancel(void) { dlg_revert(); ls_opt_close(); }

static void dlg_drop_open(int row, const ls_opt_t *o, tui_rect at)
{
    const int n = o->kind == LS_OPT_TOGGLE ? 2 : o->n < 24 ? o->n : 24;
    if (!o->names || n <= 0) { stepped(row, 1); return; }
    const int cur = o->get ? o->get(o) : -1;
    for (int i = 0; i < n; i++) {
        s_drop_item[i] = (ls_dui_item_t){.label = ls_tr(o->names[i]), .id = (int16_t)i,
                                         .flags = (uint8_t)(i == cur ? LS_DUI_CHECK : 0)};
    }
    ls_dui_popup_open(&s_drop, s_drop_item, n, at.x, at.y + 1, -1, TUI_CYAN);
    s_drop.sel = cur >= 0 && cur < n ? cur : 0;
    s_drop_row = row;
}

static void dlg_drop_pick(int v)
{
    const ls_opt_t *o = row_opt(s_drop_row);
    s_drop.open = false;
    if (o && o->set && v >= 0) { o->set(o, v); reopen(s_drop_row, NULL); }
}

/* Enter or a click on row r's control: what the list's row did, in place. */
static void dlg_activate(int r)
{
    const ls_opt_t *o = row_opt(r);
    if (!o) return;
    if (o->kind == LS_OPT_CYCLE || (o->kind == LS_OPT_TOGGLE && !dlg_check(o))) {
        if (!(o->why_not && o->why_not(o))) dlg_drop_open(r, o, s_dlg_hit[r].val);
        return;
    }
    picked(r);
}

static void dlg_draw(tui_surface *sf, tui_rect area)
{
    const ls_opt_ctx_t *root = s_dlg_root ? s_dlg_root : (s_depth ? s_up[0] : s_ctx);
    if (!root || !s_ctx) return;
    const char *cname[DLG_CATS];
    const int ncat = dlg_cats(cname, NULL), cat = dlg_cat_now();
    char title[64];
    const ls_rsel_radio_t radio = ls_opt_radio(root);
    if (radio < LS_RSEL_RADIOS) snprintf(title, sizeof(title), "%s Options - %s", ls_tr(root->name), ls_rsel_name(radio));
    else snprintf(title, sizeof(title), "%s Options", ls_tr(root->name));
    /* Label column and controls: as wide as the longest words ask. */
    int lw = 8, vw = 10;
    char v[LS_PICKER_DETAIL];
    for (int r = 0; r < s_rows; r++) {
        const ls_opt_t *o = row_opt(r);
        const int n = (int)strlen(ls_tr(o->label)) + 2;
        if (n > lw) lw = n;
        ls_opt_value(o, v, sizeof(v));
        if ((int)strlen(v) + 2 > vw) vw = (int)strlen(v) + 2;
        for (int k = 0; o->kind == LS_OPT_CYCLE && o->names && k < o->n; k++)
            if ((int)strlen(ls_tr(o->names[k])) + 2 > vw) vw = (int)strlen(ls_tr(o->names[k])) + 2;
    }
    if (lw > 26) lw = 26;
    if (vw > 24) vw = 24;
    int cw = 0;
    for (int i = 0; ncat > 1 && i < ncat; i++) if ((int)strlen(ls_tr(cname[i])) + 4 > cw) cw = (int)strlen(ls_tr(cname[i])) + 4;
    if (cw > 22) cw = 22;
    int w = (cw ? cw + 1 : 0) + lw + vw + 34;
    if (w < 56) w = 56;
    if (w > area.w - 2) w = area.w - 2;
    int h = s_rows + 8;
    if (ncat + 6 > h) h = ncat + 6;
    if (h < 12) h = 12;
    if (h > area.h - 1) h = area.h - 1;
    const tui_rect box = tui_rect_make(area.x + (area.w - w) / 2, area.y + (area.h - h) / 3, w, h);
    s_dlg_box = box;
    ls_dialog1_frame(sf, box, title, TUI_CYAN);
    const tui_rect in = tui_rect_make(box.x + 2, box.y + 2, box.w - 4, box.h - 5);
    /* The categories: a list box at the left. */
    int gx = in.x;
    for (int i = 0; i < DLG_CATS; i++) s_dlg_cat_hit[i] = tui_rect_make(0, -1, 0, 0);
    if (ncat > 1 && cw) {
        const tui_rect lb = ls_dui_well(sf, tui_rect_make(in.x, in.y, cw, in.h), NULL);
        for (int i = 0; i < ncat && i < lb.h; i++) {
            const tui_rect rr = tui_rect_make(lb.x, lb.y + i, lb.w, 1);
            const bool on = i == cat;
            if (on) tui_fill(sf, rr, ' ', ls_ink(s_dlg_focus == DLG_F_CATS ? LS_INK_SELECT : LS_INK_SELECT_DIM, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLUE)));
            char t[32];
            snprintf(t, sizeof(t), "%s", ls_tr(cname[i]));
            dlg_case(t);
            tui_put_str(sf, rr, rr.x + 1, rr.y, t, on ? ls_ink(s_dlg_focus == DLG_F_CATS ? LS_INK_SELECT : LS_INK_SELECT_DIM, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLUE))
                                                 : TUI_ATTR(TUI_WHITE, TUI_BLACK));
            s_dlg_cat_hit[i] = rr;
        }
        gx += cw + 1;
    }
    /* The settings of the one chosen, in a group box. */
    char cap[48];
    snprintf(cap, sizeof(cap), "%s", s_depth > 0 ? ls_tr(s_ctx->name) : ncat > 1 ? "General" : ls_tr(root->name));
    dlg_case(cap);
    const tui_rect g = ls_dui_group(sf, tui_rect_make(gx, in.y, in.x + in.w - gx, in.h), cap);
    int y = g.y + 1, x0 = g.x + 1;
    s_dlg_backkey = tui_rect_make(0, -1, 0, 0);
    if (s_depth > dlg_cat_depth()) {
        /* A page under a category: Back to the one above. */
        const ls_btn_t b = {.label = "< BACK"};
        const int kw = ls_key1_draw(sf, g, x0, y, &b, false);
        s_dlg_backkey = tui_rect_make(x0, y, kw, 1);
        y += 2;
    }
    const int room = g.y + g.h - y;
    s_dlg_rows_shown = room > 0 ? room : 0;
    if (s_dlg_focus >= 0 && s_dlg_focus < s_rows) {
        if (s_dlg_focus < s_dlg_top) s_dlg_top = s_dlg_focus;
        if (s_dlg_focus >= s_dlg_top + s_dlg_rows_shown) s_dlg_top = s_dlg_focus - s_dlg_rows_shown + 1;
    }
    if (s_dlg_top > s_rows - s_dlg_rows_shown) s_dlg_top = s_rows - s_dlg_rows_shown;
    if (s_dlg_top < 0) s_dlg_top = 0;
    const uint8_t text = TUI_ATTR(TUI_WHITE, TUI_BLACK), dim = LS_ATTR_FAINT;
    const uint8_t sel = ls_ink(LS_INK_SELECT, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLUE));
    for (int r = 0; r < LS_PICKER_MAX; r++) s_dlg_hit[r] = (dlg_hit_t){0};
    for (int r = s_dlg_top; r < s_rows && y < g.y + g.h; r++) {
        if (dlg_hidden(r)) continue;
        const ls_opt_t *o = row_opt(r);
        const char *why = o->why_not ? o->why_not(o) : NULL;
        const bool foc = r == s_dlg_focus;
        dlg_hit_t *hit = &s_dlg_hit[r];
        hit->row = tui_rect_make(x0, y, g.x + g.w - x0, 1);
        const char *label = ls_tr(o->label);
        ls_opt_value(o, v, sizeof(v));
        const int fx = x0 + lw;
        if (dlg_check(o)) {
            /* [x] Label: the box, then its words; the focus selects the words. */
            const tui_rect bx = ls_dui_field(sf, g, x0, y, 3);
            if (o->get && o->get(o)) ls_box_put(sf, g, bx.x, y, LS_SHAPE_CHECK, why ? dim : TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
            const int n = (int)strlen(label);
            if (foc) tui_fill(sf, tui_rect_intersect(g, tui_rect_make(x0 + 3, y, n + 2, 1)), ' ', sel);
            tui_put_str(sf, g, x0 + 4, y, label, foc ? sel : why ? dim : text);
            hit->val = tui_rect_make(x0, y, n + 6, 1);
        } else if (o->kind == LS_OPT_ACTION || o->kind == LS_OPT_MENU) {
            /* A push button with the row's words; what it shows beside it. */
            char lab[48];
            snprintf(lab, sizeof(lab), o->kind == LS_OPT_MENU ? "%s >>" : "%s", label);
            const ls_btn_t b = {.label = lab, .dim = why != NULL};
            const int kw = ls_key1_draw(sf, g, x0, y, &b, foc);
            hit->val = tui_rect_make(x0, y, kw, 1);
            if (o->kind == LS_OPT_ACTION && v[0]) tui_put_str(sf, tui_rect_intersect(g, tui_rect_make(x0 + kw + 1, y, g.x + g.w - x0 - kw - 2, 1)), x0 + kw + 2, y, v, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        } else {
            tui_put_str(sf, tui_rect_intersect(g, tui_rect_make(x0, y, lw - 1, 1)), x0, y, label, why ? dim : text);
            tui_put_char(sf, g, x0 + (int)strlen(label) < fx - 1 ? x0 + (int)strlen(label) : fx - 2, y, ':', why ? dim : text);
            if (o->kind == LS_OPT_LEVEL && o->num && o->hi > o->lo) {
                const double now = o->num(o);
                const float frac = (float)((now - o->lo) / (o->hi - o->lo));
                tui_rect dn, up, vr, bar;
                ls_slider1_draw(sf, tui_rect_make(fx, y, g.x + g.w - fx - 1, 1), "", v, frac, TUI_CYAN, !why, &dn, &up, &vr, &bar);
                hit->a = dn; hit->b = up; hit->val = vr; hit->bar = bar;
                if (foc && vr.w > 2) tui_put_str(sf, g, vr.x + 1, y, v, sel);
            } else {
                /* A drop-down for a choice, an edit with [...] for a number or words. */
                const bool drop = o->kind == LS_OPT_CYCLE || o->kind == LS_OPT_TOGGLE;
                const int fw = vw + 2;
                const tui_rect fi = ls_dui_field(sf, g, fx, y, fw);
                if (foc) tui_fill(sf, fi, ' ', sel);
                tui_put_str(sf, tui_rect_intersect(g, fi), fi.x + 1, y, v, foc ? sel : why ? dim : TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
                hit->val = tui_rect_make(fx, y, fw, 1);
                const ls_btn_t k = {.label = drop ? "v" : "...", .dim = why != NULL};
                const int kw = ls_key1_draw(sf, g, fx + fw, y, &k, false);
                hit->a = tui_rect_make(fx + fw, y, kw, 1);
                if (why) tui_put_str(sf, tui_rect_intersect(g, tui_rect_make(fx + fw + kw + 1, y, g.x + g.w - fx - fw - kw - 2, 1)),
                                     fx + fw + kw + 2, y, ls_tr(why), dim);
            }
        }
        y++;
    }
    if (s_rows > s_dlg_rows_shown) {
        char pos[24];
        snprintf(pos, sizeof(pos), " %d-%d of %d ", s_dlg_top + 1, s_dlg_top + s_dlg_rows_shown, s_rows);
        tui_put_str(sf, box, g.x + g.w - (int)strlen(pos) - 1, g.y + g.h, pos, text);
    }
    /* OK, Cancel, Apply at the bottom right; Apply greyed until something changed. */
    const int by = box.y + box.h - 2;
    const ls_btn_t ok = {.label = "OK"}, ca = {.label = "Cancel"}, ap = {.label = "Apply", .dim = !dlg_changed()};
    const int wa = ls_key1_width(&ap), wc = ls_key1_width(&ca), wo = ls_key1_width(&ok) + 4;
    int bx = box.x + box.w - 2 - wa;
    s_dlg_apply = tui_rect_make(bx, by, wa, 1);
    ls_key1_draw(sf, box, bx, by, &ap, s_dlg_focus == DLG_F_APPLY);
    bx -= wc + 1;
    s_dlg_cancel = tui_rect_make(bx, by, wc, 1);
    ls_key1_draw(sf, box, bx, by, &ca, s_dlg_focus == DLG_F_CANCEL);
    bx -= wo + 1;
    s_dlg_ok = tui_rect_make(bx, by, wo, 1);
    const ls_btn_t okw = {.label = "  OK  "};
    ls_key1_draw(sf, box, bx, by, &okw, s_dlg_focus == DLG_F_OK);
    tui_put_str(sf, box, box.x + 2, by, ls_tr("Changes show at once"), LS_ATTR_FAINT);
    if (s_drop.open) ls_dui_popup_draw(sf, area, &s_drop);
}

static void dlg_focus_step(int d)
{
    /* rows, then OK, Cancel, Apply, then the categories */
    int order[LS_PICKER_MAX + 4], n = 0;
    for (int r = 0; r < s_rows && n < LS_PICKER_MAX; r++) if (!dlg_hidden(r)) order[n++] = r;
    order[n++] = DLG_F_OK; order[n++] = DLG_F_CANCEL; order[n++] = DLG_F_APPLY;
    if (dlg_cats(NULL, NULL) > 1) order[n++] = DLG_F_CATS;
    int at = 0;
    for (int i = 0; i < n; i++) if (order[i] == s_dlg_focus) at = i;
    s_dlg_focus = order[(at + d + n) % n];
}

static bool dlg_key(ls_tk_t k, char ch)
{
    if (s_drop.open) {
        const int c = ls_dui_popup_key(&s_drop, k, ch);
        if (c >= 0) dlg_drop_pick(c);
        else if (c == LS_DUI_CLOSED) s_drop.open = false;
        return true;
    }
    const int ncat = dlg_cats(NULL, NULL);
    const bool row = s_dlg_focus >= 0 && s_dlg_focus < s_rows;
    switch (k) {
    case LS_TK_ESC:
        if (s_depth > dlg_cat_depth()) { go_up(); return true; }
        dlg_cancel();
        return true;
    case LS_TK_TAB: dlg_focus_step(1); return true;
    case LS_TK_UP:
        if (s_dlg_focus == DLG_F_CATS) { dlg_go_cat((dlg_cat_now() + ncat - 1) % (ncat ? ncat : 1)); s_dlg_focus = DLG_F_CATS; }
        else dlg_focus_step(-1);
        return true;
    case LS_TK_DOWN:
        if (s_dlg_focus == DLG_F_CATS) { dlg_go_cat((dlg_cat_now() + 1) % (ncat ? ncat : 1)); s_dlg_focus = DLG_F_CATS; }
        else dlg_focus_step(1);
        return true;
    case LS_TK_PGUP: if (ncat > 1) dlg_go_cat((dlg_cat_now() + ncat - 1) % ncat); return true;
    case LS_TK_PGDN: if (ncat > 1) dlg_go_cat((dlg_cat_now() + 1) % ncat); return true;
    case LS_TK_WHEEL_UP: if (s_dlg_top > 0) s_dlg_top--; return true;
    case LS_TK_WHEEL_DOWN: if (s_dlg_top + s_dlg_rows_shown < s_rows) s_dlg_top++; return true;
    case LS_TK_LEFT:
    case LS_TK_RIGHT:
        if (row) {
            const ls_opt_t *o = row_opt(s_dlg_focus);
            const int d = k == LS_TK_LEFT ? -1 : 1;
            if (o->kind == LS_OPT_LEVEL) stepped(s_dlg_focus, d);
            else if (o->kind == LS_OPT_CYCLE && o->get && o->set && o->n > 0 && !(o->why_not && o->why_not(o))) {
                o->set(o, (o->get(o) + d + o->n) % o->n);
                reopen(s_dlg_focus, NULL);
            }
        } else if (s_dlg_focus >= DLG_F_OK) dlg_focus_step(k == LS_TK_LEFT ? -1 : 1);
        return true;
    case LS_TK_ENTER:
        if (s_dlg_focus == DLG_F_CANCEL) dlg_cancel();
        else if (s_dlg_focus == DLG_F_APPLY) dlg_snapshot();
        else if (s_dlg_focus == DLG_F_OK) ls_opt_close();
        else if (row) dlg_activate(s_dlg_focus);
        return true;
    case LS_TK_CHAR:
        if (ch == ' ' && row) { dlg_activate(s_dlg_focus); return true; }
        return true;
    default:
        return true;
    }
}

static bool dlg_touch(int col, int row)
{
    if (s_drop.open) {
        const int c = ls_dui_popup_tap(&s_drop, col, row);
        if (c >= 0) dlg_drop_pick(c);
        else if (c == LS_DUI_CLOSED) s_drop.open = false;
        return true;
    }
    if (tui_rect_contains(s_dlg_ok, col, row)) { ls_opt_close(); return true; }
    if (tui_rect_contains(s_dlg_cancel, col, row)) { dlg_cancel(); return true; }
    if (tui_rect_contains(s_dlg_apply, col, row)) { dlg_snapshot(); s_dlg_focus = DLG_F_APPLY; return true; }
    if (tui_rect_contains(s_dlg_backkey, col, row)) { go_up(); return true; }
    for (int i = 0; i < DLG_CATS; i++)
        if (s_dlg_cat_hit[i].w && tui_rect_contains(s_dlg_cat_hit[i], col, row)) { dlg_go_cat(i); s_dlg_focus = DLG_F_CATS; return true; }
    for (int r = 0; r < s_rows; r++) {
        const dlg_hit_t *h = &s_dlg_hit[r];
        if (!h->row.w || !tui_rect_contains(h->row, col, row)) continue;
        s_dlg_focus = r;
        const ls_opt_t *o = row_opt(r);
        if (o->why_not && o->why_not(o)) return true;
        if (h->a.w && tui_rect_contains(h->a, col, row)) {
            if (o->kind == LS_OPT_LEVEL) stepped(r, -1);
            else dlg_activate(r);
        } else if (h->b.w && tui_rect_contains(h->b, col, row)) {
            stepped(r, 1);
        } else if (h->bar.w && tui_rect_contains(h->bar, col, row) && o->set_num && o->hi > o->lo) {
            /* A click on the track sets the level there, on its step. */
            double v = o->lo + (o->hi - o->lo) * (double)(col - h->bar.x) / (double)(h->bar.w > 1 ? h->bar.w - 1 : 1);
            if (o->step > 0) v = o->lo + (double)(long)((v - o->lo) / o->step + 0.5) * o->step;
            o->set_num(o, v);
        } else if (h->val.w && tui_rect_contains(h->val, col, row)) {
            dlg_activate(r);
        }
        return true;
    }
    return true;                       /* a dialog: a click outside it does nothing */
}

static const ls_picker_form_t DLG_FORM = {dlg_draw, dlg_key, dlg_touch};

static void dlg_form_on(void) { if (ls_tui_compact()) ls_picker_form(&DLG_FORM); }
static void dlg_end(void) { s_dlg_root = NULL; s_drop.open = false; s_nsnap = 0; }

#else
static void dlg_end(void) {}
static void dlg_form_on(void) {}
static void dlg_begin(const ls_opt_ctx_t *ctx) { (void)ctx; }
#endif
