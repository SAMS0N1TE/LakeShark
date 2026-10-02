/* See ls_options.h. The list, the rows that belong in it, and bringing it
   back after a choice, a keypad or a list of the app's own. */
#include "ls_options.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "ls_keyboard.h"
#include "ls_numpad.h"
#include "ls_picker.h"
#include "ls_radio_select.h"

/* The context the list was opened on, and which table row each list row is.
   The tables are the apps' and const; this is only where the list is. */
static const ls_opt_ctx_t *s_ctx;
static EXT_RAM_BSS_ATTR uint8_t s_row_opt[LS_PICKER_MAX];
static int s_rows;
/* The list row a keypad, keyboard or list of the app's own was opened from,
   for the list to come back on when that closes; -1 when nothing waits. */
static int s_back = -1;
/* The screen it was opened on: a list does not come back over another app. */
static int s_screen = -1;
/* The row a keypad or keyboard is writing to. */
static int s_edit = -1;

/* ------------------------------------------------------------ the rows -- */

ls_rsel_radio_t ls_opt_radio(const ls_opt_ctx_t *ctx)
{
    if (!ctx) return LS_RSEL_NONE;
    if (ctx->job >= 0 && ctx->job < LS_RSEL_JOBS) return ls_rsel_effective((ls_rsel_job_t)ctx->job);
    return ctx->radio;
}

static bool row_for(const ls_opt_t *o, ls_rsel_radio_t radio)
{
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
    case LS_OPT_NUMBER: {
        const double v = o->num ? o->num(o) : 0;
        if (v == (double)(long)v) snprintf(out, n, "%ld", (long)v);
        else snprintf(out, n, "%.1f", v);
        break;
    }
    case LS_OPT_TEXT: {
        const char *t = o->text ? o->text(o) : NULL;
        snprintf(out, n, "%s", t ? t : "");
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

static void fill(const ls_opt_ctx_t *ctx)
{
    char title[LS_PICKER_DETAIL];
    const ls_rsel_radio_t radio = ls_opt_radio(ctx);
    if (radio < LS_RSEL_RADIOS)
        snprintf(title, sizeof(title), "%s OPTIONS / %s", ctx->name, ls_rsel_name(radio));
    else
        snprintf(title, sizeof(title), "%s OPTIONS", ctx->name);
    ls_picker_open(title, picked);
    s_ctx = ctx;
    s_rows = 0;
    char d[LS_PICKER_DETAIL];
    for (int i = 0; i < ctx->n && s_rows < LS_PICKER_MAX; i++) {
        const ls_opt_t *o = &ctx->opt[i];
        if (!row_for(o, radio)) continue;
        detail(o, d, sizeof(d));
        ls_picker_add(o->label, d);
        s_row_opt[s_rows++] = (uint8_t)i;
    }
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
        snprintf(note, sizeof(note), "%s: %s to %s", o->label, lo, hi);
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
        snprintf(note, sizeof(note), "%s: %s", o->label, why);
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
        s_edit = row;
        s_back = row;   /* a cancelled keypad brings the list back too */
        ls_numpad_open(o->label, o->unit ? o->unit : "", o->num ? o->num(o) : 0, number_done);
        return;
    case LS_OPT_TEXT: {
        s_edit = row;
        s_back = row;
        const char *t = o->text ? o->text(o) : "";
        ls_keyboard_open(o->label, t ? t : "", o->max > 0 ? o->max : 32, text_done);
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
    s_screen = ls_tui_screen_current();
    fill(ctx);
}

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
    s_ctx = NULL;
}

void ls_opt_poll(void)
{
    if (!s_ctx) return;
    if (ls_picker_is(picked)) {
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
