#include "ls_lap_context.h"
/* See ls_lap_kit.h and app-sdk/include/lakeshark_ui.h.
 *
 * Every wrapper here is a few lines over a widget the board already has:
 * it reads the app's arguments once (synchronous pointers checked with ls_lap_api_readable,
 * strings copied byte by byte while they are the app's), turns the app's
 * cells into absolute ones clipped to its area, and draws with the board's
 * code. Callbacks back into the app go through trampolines that check the
 * pointer is the app's code and enter the loader's guard; ls_lap_kit_end
 * cuts them all off before the app's code can go away. */
#include "ls_lap_kit.h"
#include "../../../app-sdk/include/lakeshark_ui.h"
#include "ls_lap_api.h"
#include "ls_lap_system.h"
#include "ls_lap_service.h"
#include "ls_braille.h"
#include "ls_numpad.h"
#include "ls_keyboard.h"
#include "ls_options.h"
#include "ls_picker.h"
#include "ls_radio_panel.h"
#include "ls_tui_ui.h"
#include "ls_waterfall.h"
#include "tuilib/tui_controls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#define TABLE_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define TABLE_FREE(p) heap_caps_free(p)
#else
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif
#define TABLE_ALLOC(n) calloc(1, (n))
#define TABLE_FREE(p) free(p)
#endif

#define TEXT 24                          /* a label or a value as the board shows it */
#define OPT_LISTS 6
#define OPT_ROWS 48
#define OPT_NAMES 8
#define VFO_SLOTS 4

typedef struct {
    char label[TEXT], unit[12];
    char names[OPT_NAMES][14];
    const char *name_ptr[OPT_NAMES];
    double (*get)(int);
    void (*set)(int, double);
    int id;
} opt_row_t;
typedef struct {                         /* one app's OPTIONS tree, PSRAM, while it is open */
    ls_opt_ctx_t ctx[OPT_LISTS];
    char title[OPT_LISTS][16];
    ls_opt_t rows[OPT_ROWS];
    opt_row_t meta[OPT_ROWS];
    int nctx, nrows;
} opt_tree_t;

EXT_RAM_BSS_ATTR static struct {
    bool live;                           /* callbacks may reach the app */
    tui_rect area;                       /* the app's area at its last draw */
    ls_btn_t btn[LAP_KEYS_MAX];
    char btn_text[LAP_KEYS_MAX][2][TEXT];
    tui_rect pad_keys[12], pad_actions[3];
    bool pad;
    tui_rect vfo[VFO_SLOTS][4];          /* down, up, value, bar */
    ls_radio_glass_t glass;
    void (*picker_done)(int);
    void (*numpad_done)(double);
    bool numpad_ours;
    opt_tree_t *opts;
    float wf[LS_WF_BINS_MAX];
    bool wf_claimed;
} p4_k_slots[LS_LAP_CONTEXTS];
#define p4_k (p4_k_slots[ls_lap_context_index()])


/* Host targets with recording widgets may omit the keyboard. */
#if !defined(ESP_PLATFORM) || defined(LS_LAP_HOST_TEST)
extern void ls_keyboard_open(const char *, const char *, int, ls_keyboard_done_t) __attribute__((weak));
extern void ls_keyboard_open_lower(const char *, const char *, int, ls_keyboard_done_t) __attribute__((weak));
extern void ls_keyboard_close(void) __attribute__((weak));
extern bool ls_keyboard_active(void) __attribute__((weak));
extern void ls_picker_note(const char *) __attribute__((weak));
extern bool ls_picker_active(void) __attribute__((weak));
#define WIDGET_AVAILABLE(fn) ((fn) != NULL)
#else
/* Firmware always links these widgets; only host fixtures may omit them. */
#define WIDGET_AVAILABLE(fn) true
#endif
static void picked(int index);
static bool options_up(void);
static LS_LAP_POOL struct {
    int id, kind, status, value, max;
    char result[LS_KEYBOARD_MAX + 1];
} p4_dialog_slots[LS_LAP_CONTEXTS];
#define p4_dialog (p4_dialog_slots[ls_lap_context_index()])

static unsigned dialog_serial;
static void dialog_text_done(const char *value) {
    if (!p4_k.live || !p4_dialog.id || p4_dialog.status || p4_dialog.kind != 1) return;
    snprintf(p4_dialog.result, sizeof(p4_dialog.result), "%.*s", p4_dialog.max, value);
    p4_dialog.status = 1;
    ls_lap_lifecycle(LAP_INPUT_FOCUS_GAINED, 0, 0);
}
static void dialog_picked(int index) {
    if (!p4_k.live || !p4_dialog.id || p4_dialog.status || p4_dialog.kind == 1) return;
    p4_dialog.value = index; p4_dialog.status = 1;
    ls_lap_lifecycle(LAP_INPUT_FOCUS_GAINED, 0, 0);
}
void ls_lap_dialog_forget(void) { memset(&p4_dialog, 0, sizeof(p4_dialog)); }
bool ls_lap_dialog_active(void) { return p4_dialog.id && !p4_dialog.status; }
static void dialog_close(void) {
    if (!ls_lap_dialog_active()) return;
    if (p4_dialog.kind == 1) { if (WIDGET_AVAILABLE(ls_keyboard_close)) ls_keyboard_close(); }
    else if (ls_picker_is(dialog_picked)) ls_picker_close();
    p4_dialog.status = 2;
    ls_lap_lifecycle(LAP_INPUT_FOCUS_GAINED, 0, 0);
    memset(p4_dialog.result, 0, sizeof(p4_dialog.result));
}
/* Strict bounded copy: stack strings are synchronous input too. */
static bool dialog_string(char *out, size_t cap, const char *src) {
    if (!src) { out[0] = 0; return true; }
    for (size_t i = 0; i < cap; i++) {
        if (!ls_lap_api_readable(src + i, 1)) return false;
        out[i] = src[i]; if (!out[i]) return true;
    }
    return false;
}
static int dialog_begin(int kind) {
    if (ls_lap_system_picker_active()) return LAP_RX_EBUSY;
    if (!ls_lap_api_inside() || !p4_k.live) return LAP_RX_EINVAL;
    if (p4_dialog.id || ls_picker_is(picked) || (WIDGET_AVAILABLE(ls_picker_active) && ls_picker_active()) ||
        (WIDGET_AVAILABLE(ls_keyboard_active) && ls_keyboard_active()) || p4_k.numpad_ours || options_up()) return LAP_RX_EBUSY;
    memset(&p4_dialog, 0, sizeof(p4_dialog));
    p4_dialog.id = (int)(dialog_serial = dialog_serial % 0x3fffffff + 1);
    p4_dialog.kind = kind;
    ls_lap_lifecycle(LAP_INPUT_FOCUS_LOST, 0, 0);
    return p4_dialog.id;
}
int lap_text_input(const char *prompt, const char *initial, int max, unsigned flags) {
    char title[96], value[LS_KEYBOARD_MAX + 1];
    if (max < 1 || max > LS_KEYBOARD_MAX || (flags & ~LAP_TEXT_LOWER) ||
        !dialog_string(title, sizeof(title), prompt) || !dialog_string(value, (size_t)max + 1, initial)) return LAP_RX_EINVAL;
    if (!WIDGET_AVAILABLE(ls_keyboard_open) || !WIDGET_AVAILABLE(ls_keyboard_open_lower) ||
        !WIDGET_AVAILABLE(ls_keyboard_active) || !WIDGET_AVAILABLE(ls_keyboard_close)) return LAP_RX_EUNSUPPORTED;
    if (ls_keyboard_active()) return LAP_RX_EBUSY;
    int id = dialog_begin(1); if (id < 0) return id;
    p4_dialog.max = max;
    if (flags & LAP_TEXT_LOWER) ls_keyboard_open_lower(title, value, max, dialog_text_done);
    else ls_keyboard_open(title, value, max, dialog_text_done);
    return id;
}
int lap_confirm(const char *message) {
    char text[160];
    if (!dialog_string(text, sizeof(text), message)) return LAP_RX_EINVAL;
    int id = dialog_begin(2); if (id < 0) return id;
    p4_dialog.max = 2;
    ls_picker_open("CONFIRM", dialog_picked);
    if (WIDGET_AVAILABLE(ls_picker_note)) ls_picker_note(text);
    ls_picker_add("NO", NULL); ls_picker_add("YES", NULL);
    ls_picker_select(0);
    return id;
}
int lap_choose(const char *const *list, int count) {
    char rows[LS_PICKER_MAX][LS_PICKER_TEXT];
    if (!ls_lap_api_inside() || count < 1 || count > LS_PICKER_MAX ||
        !ls_lap_api_readable(list, (size_t)count * sizeof(*list))) return LAP_RX_EINVAL;
    for (int i = 0; i < count; i++) if (!dialog_string(rows[i], sizeof(rows[i]), list[i])) return LAP_RX_EINVAL;
    int id = dialog_begin(3); if (id < 0) return id;
    p4_dialog.max = count;
    ls_picker_open("CHOOSE", dialog_picked);
    for (int i = 0; i < count; i++) ls_picker_add(rows[i], NULL);
    return id;
}
int lap_dialog_poll(int id, char *out, size_t cap, int *value) {
    if (!ls_lap_api_inside() || id <= 0 || id != p4_dialog.id) return LAP_RX_EINVAL;
    if (!p4_dialog.status && (p4_dialog.kind == 1 ? !ls_keyboard_active() : !ls_picker_is(dialog_picked))) {
        p4_dialog.status = 2; ls_lap_lifecycle(LAP_INPUT_FOCUS_GAINED, 0, 0);
    }
    if (!p4_dialog.status) return 0;
    if (p4_dialog.status == 1) {
        if (p4_dialog.kind == 1) {
            size_t n = strlen(p4_dialog.result) + 1;
            if (cap < n || !ls_lap_api_readable(out, n)) return LAP_RX_EINVAL;
            memcpy(out, p4_dialog.result, n);
        } else {
            if (!ls_lap_api_readable(value, sizeof(*value))) return LAP_RX_EINVAL;
            *value = p4_dialog.value;
        }
    }
    int status = p4_dialog.status; memset(&p4_dialog, 0, sizeof(p4_dialog)); return status;
}
void lap_dialog_cancel(int id) {
    if (ls_lap_api_inside() && id > 0 && id == p4_dialog.id) dialog_close();
}

#if !defined(ESP_PLATFORM) || defined(LS_LAP_HOST_TEST)
bool ls_lap_dialog_answer(const char *text, int index, bool cancel) {
    if (!ls_lap_dialog_active()) return false;
    if (cancel) { dialog_close(); return true; }
    if (p4_dialog.kind == 1) {
        if (!text) return false;
        if (ls_keyboard_close) ls_keyboard_close();
        dialog_text_done(text);
    } else {
        if (index < 0 || index >= p4_dialog.max) return false;
        ls_picker_close(); dialog_picked(index);
    }
    return true;
}
#endif

/* ---- reading the app's arguments --------------------------------------------- */

/* The app's string, cut to cap; false when any byte of it is not the app's. */
static bool text(char *dst, size_t cap, const char *src) {
    dst[0] = 0;
    if (!src) return true;
    size_t i = 0;
    for (; i + 1 < cap; i++) {
        if (!ls_lap_api_readable(src + i, 1)) { dst[0] = 0; return false; }
        if (!(dst[i] = src[i])) return true;
    }
    dst[i] = 0;
    return true;
}
static bool canvas(tui_surface **sf, tui_rect *area) {
    if (!ls_lap_api_canvas(sf, area)) return false;
    p4_k.area = *area;
    return true;
}
/* The app's rectangle, made absolute and held inside its area. */
static tui_rect box(tui_rect area, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0 || x > 4096 || y > 4096 || x < -4096 || y < -4096 || w > 4096 || h > 4096)
        return tui_rect_make(0, 0, 0, 0);
    return tui_rect_intersect(tui_rect_make(area.x + x, area.y + y, w, h), area);
}
static bool empty(tui_rect r) { return r.w <= 0 || r.h <= 0; }

/* ---- keys -------------------------------------------------------------------- */

int lap_ui_keys_rows(int n) {
    if (!ls_lap_api_inside() || n < 1) return 0;
    tui_surface *sf;
    tui_rect a;
    if (!canvas(&sf, &a)) return 0;
    return ls_btn_raised_height(a, n > LAP_KEYS_MAX ? LAP_KEYS_MAX : n);
}
void lap_ui_keys(int x, int y, int w, int h, const lap_btn_t *keys, int n, int focus) {
    tui_surface *sf;
    tui_rect a;
    if (!canvas(&sf, &a) || n < 1 || n > LAP_KEYS_MAX || !ls_lap_api_readable(keys, sizeof(*keys) * (size_t)n)) return;
    const tui_rect r = box(a, x, y, w, h);
    if (empty(r)) return;
    for (int i = 0; i < n; i++) {
        const lap_btn_t b = keys[i];
        text(p4_k.btn_text[i][0], TEXT, b.label);
        text(p4_k.btn_text[i][1], TEXT, b.value);
        p4_k.btn[i] = (ls_btn_t){p4_k.btn_text[i][0], b.value ? p4_k.btn_text[i][1] : NULL,
                              b.key >= 32 && b.key < 127 ? b.key : 0, b.on, b.dim, b.badge};
    }
    ls_btn_bar_raised_slot(sf, r, p4_k.btn, n, focus >= 0 && focus < n ? focus : -1, LS_BTN_SLOT_SCREEN);
}
int lap_ui_key_hit(int col, int row) {
    if (!ls_lap_api_inside()) return -1;
    return ls_btn_hit_slot(p4_k.area.x + col, p4_k.area.y + row, LS_BTN_SLOT_SCREEN);
}
int lap_ui_key_find(char ch) {
    return ls_lap_api_inside() ? ls_btn_shortcut(ch, LS_BTN_SLOT_SCREEN) : -1;
}

/* ---- OPTIONS ------------------------------------------------------------------- */

static const opt_row_t *row_of(const ls_opt_t *o) {
    return p4_k.live && p4_k.opts && o && o->arg >= 0 && o->arg < p4_k.opts->nrows ? &p4_k.opts->meta[o->arg] : NULL;
}
/* While a row's callback runs, ls_options is still using the table, so the
   app may close the list from it but not replace or free it. */
static LS_LAP_POOL int p4_in_row_slots[LS_LAP_CONTEXTS];
#define p4_in_row (p4_in_row_slots[ls_lap_context_index()])

static double call_get(const ls_opt_t *o) {
    const opt_row_t *m = row_of(o);
    if (!m || !ls_lap_code_ok((const void *)m->get)) return 0;
    p4_in_row++;
    ls_lap_call_begin();
    const double v = m->get(m->id);
    ls_lap_call_end();
    p4_in_row--;
    return v;
}
static void call_set(const ls_opt_t *o, double v) {
    const opt_row_t *m = row_of(o);
    if (!m || !ls_lap_code_ok((const void *)m->set)) return;
    p4_in_row++;
    ls_lap_call_begin();
    m->set(m->id, v);
    ls_lap_call_end();
    p4_in_row--;
}
static int tr_get(const ls_opt_t *o) { return (int)call_get(o); }
static void tr_set(const ls_opt_t *o, int v) { call_set(o, v); }
static double tr_num(const ls_opt_t *o) { return call_get(o); }
static void tr_set_num(const ls_opt_t *o, double v) { call_set(o, v); }
static void tr_act(const ls_opt_t *o) { call_set(o, 0); }

/* One list of the app's tree into ctx[index]; false when the table is not
   the app's or does not fit. Depth and the list count bound a loop. */
static int add_list(opt_tree_t *t, const lap_opt_list_t *l, double (*pget)(int), void (*pset)(int, double),
                    int depth) {
    if (depth > 3 || t->nctx >= OPT_LISTS || !ls_lap_api_readable(l, sizeof(*l))) return -1;
    const lap_opt_list_t L = *l;
    if (L.n < 0 || L.n > OPT_ROWS - t->nrows || (L.n && !ls_lap_api_readable(L.rows, sizeof(*L.rows) * (size_t)L.n)))
        return -1;
    double (*get)(int) = L.get ? L.get : pget;
    void (*set)(int, double) = L.set ? L.set : pset;
    if ((get && !ls_lap_code_ok((const void *)get)) || (set && !ls_lap_code_ok((const void *)set))) return -1;
    const int c = t->nctx++;
    text(t->title[c], sizeof(t->title[c]), L.name);
    const int first = t->nrows;
    t->nrows += L.n;
    for (int i = 0; i < L.n; i++) {
        const lap_opt_t R = L.rows[i];
        ls_opt_t *o = &t->rows[first + i];
        opt_row_t *m = &t->meta[first + i];
        if (!text(m->label, sizeof(m->label), R.label)) return -1;
        text(m->unit, sizeof(m->unit), R.unit);
        m->get = get;
        m->set = set;
        m->id = R.id;
        *o = (ls_opt_t){.label = m->label, .arg = first + i, .unit = m->unit, .lo = R.lo, .hi = R.hi, .step = R.step};
        switch (R.kind) {
        case LAP_OPT_CYCLE:
        case LAP_OPT_TOGGLE: {
            const int n = R.kind == LAP_OPT_TOGGLE ? (R.names ? 2 : 0) : R.n;
            if (n < 0 || n > OPT_NAMES || (n && !ls_lap_api_readable(R.names, sizeof(*R.names) * (size_t)n)) ||
                (R.kind == LAP_OPT_CYCLE && n < 1))
                return -1;
            for (int j = 0; j < n; j++) {
                text(m->names[j], sizeof(m->names[j]), R.names[j]);
                m->name_ptr[j] = m->names[j];
            }
            o->kind = R.kind == LAP_OPT_CYCLE ? LS_OPT_CYCLE : LS_OPT_TOGGLE;
            o->names = n ? m->name_ptr : NULL;
            o->n = n;
            o->get = tr_get;
            o->set = tr_set;
            o->step = R.kind == LAP_OPT_CYCLE ? 1 : 0;   /* < and > step a choice too */
            break;
        }
        case LAP_OPT_NUMBER:
        case LAP_OPT_LEVEL:
            o->kind = R.kind == LAP_OPT_NUMBER ? LS_OPT_NUMBER : LS_OPT_LEVEL;
            o->num = tr_num;
            o->set_num = tr_set_num;
            break;
        case LAP_OPT_ACTION:
            o->kind = LS_OPT_ACTION;
            o->act = tr_act;
            break;
        case LAP_OPT_MENU: {
            const int sub = add_list(t, R.sub, get, set, depth + 1);
            if (sub < 0) return -1;
            o->kind = LS_OPT_MENU;
            o->sub = &t->ctx[sub];
            break;
        }
        case LAP_OPT_DISPLAY:            /* the waterfall's own rows, as FM shows them */
            o->kind = LS_OPT_MENU;
            o->sub = ls_wf_options();
            break;
        default:
            return -1;
        }
    }
    t->ctx[c] = (ls_opt_ctx_t){.name = t->title[c], .job = -1, .radio = LS_RSEL_NONE, .opt = &t->rows[first],
                               .n = L.n, .covers_screen = true};
    return c;
}
static void options_free(void) {
    if (!p4_k.opts) return;
    ls_opt_close();
    TABLE_FREE(p4_k.opts);
    p4_k.opts = NULL;
}
static bool options_up(void) { return p4_k.opts && (ls_opt_covers_screen() || ls_opt_returning()); }
bool lap_options_open(const lap_opt_list_t *root) {
    if (!ls_lap_api_inside() || !p4_k.live || p4_in_row) return false;
    options_free();
    opt_tree_t *t = TABLE_ALLOC(sizeof(*t));
    if (!t) return false;
    if (add_list(t, root, NULL, NULL, 0) != 0 || ls_opt_count(&t->ctx[0]) <= 0) {
        TABLE_FREE(t);
        return false;
    }
    p4_k.opts = t;
    ls_opt_open(&t->ctx[0]);
    return true;
}
bool lap_options_showing(void) { return ls_lap_api_inside() && options_up(); }
void lap_options_close(void) {
    if (!ls_lap_api_inside() || !p4_k.opts) return;
    if (p4_in_row) ls_opt_close();          /* the table goes at the next open or when the app closes */
    else options_free();
}

/* ---- lists and keypads ----------------------------------------------------------- */

static void picked(int index) {
    void (*done)(int) = p4_k.picker_done;
    p4_k.picker_done = NULL;
    if (!p4_k.live || !ls_lap_code_ok((const void *)done)) return;
    ls_lap_call_begin();
    done(index);
    ls_lap_call_end();
}
bool lap_picker_open(const char *title, void (*done)(int index)) {
    if (ls_lap_system_picker_active()) return false;
    char t[32];
    if (ls_lap_dialog_active() || !ls_lap_api_inside() || !p4_k.live || !ls_lap_code_ok((const void *)done) || !text(t, sizeof(t), title))
        return false;
    p4_k.picker_done = done;
    ls_picker_open(t, picked);
    return true;
}
bool lap_picker_add(const char *label, const char *detail) {
    char l[LS_PICKER_TEXT], d[LS_PICKER_DETAIL];
    if (!ls_lap_api_inside() || !ls_picker_is(picked) || !text(l, sizeof(l), label) || !text(d, sizeof(d), detail))
        return false;
    return ls_picker_add(l, detail ? d : NULL);
}
void lap_picker_select(int index) {
    if (ls_lap_api_inside() && ls_picker_is(picked)) ls_picker_select(index);
}
static void entered(double v) {
    void (*done)(double) = p4_k.numpad_done;
    p4_k.numpad_done = NULL;
    p4_k.numpad_ours = false;
    if (!p4_k.live || !ls_lap_code_ok((const void *)done)) return;
    ls_lap_call_begin();
    done(v);
    ls_lap_call_end();
}
bool lap_numpad_open(const char *title, const char *unit, double initial, void (*done)(double value)) {
    if (ls_lap_system_picker_active()) return false;
    char t[32], u[16];
    if (ls_lap_dialog_active() || !ls_lap_api_inside() || !p4_k.live || !ls_lap_code_ok((const void *)done) || !text(t, sizeof(t), title) ||
        !text(u, sizeof(u), unit))
        return false;
    p4_k.numpad_done = done;
    p4_k.numpad_ours = true;
    ls_numpad_open(t, u, initial, entered);
    return true;
}
bool lap_ui_overlay(void) {
    return ls_lap_api_inside() && (ls_lap_system_picker_active() || ls_lap_dialog_active() || ls_picker_is(picked) || (p4_k.numpad_ours && ls_numpad_active()) || options_up());
}

void lap_ui_freq_pad(int x, int y, int w, int h, const char *entry, bool invalid) {
    tui_surface *sf;
    tui_rect a;
    char e[16];
    if (!canvas(&sf, &a) || !text(e, sizeof(e), entry)) return;
    const tui_rect r = box(a, x, y, w, h);
    if (empty(r)) return;
#if defined(ESP_PLATFORM)
    const uint32_t blink = (uint32_t)(esp_timer_get_time() / 1000);
#else
    const uint32_t blink = 0;
#endif
    ls_freq_pad_draw(sf, r, e, invalid, blink, p4_k.pad_keys, p4_k.pad_actions);
    p4_k.pad = true;
}
char lap_ui_freq_hit(int col, int row) {
    if (!ls_lap_api_inside() || !p4_k.pad) return 0;
    return ls_freq_pad_hit(p4_k.pad_keys, p4_k.pad_actions, p4_k.area.x + col, p4_k.area.y + row);
}

/* ---- the radio glass ---------------------------------------------------------------- */

void lap_ui_bezel(int x, int y, int w, int h, const char *title) {
    tui_surface *sf;
    tui_rect a;
    char t[TEXT];
    if (!canvas(&sf, &a) || !text(t, sizeof(t), title)) return;
    const tui_rect r = box(a, x, y, w, h);
    if (r.w >= 2 && r.h >= 2) ls_radio_bezel(sf, r, title ? t : NULL);
}
int lap_ui_chip(int x, int y, const char *s, bool on, uint8_t hue) {
    tui_surface *sf;
    tui_rect a;
    char t[TEXT];
    if (!canvas(&sf, &a) || !text(t, sizeof(t), s) || y < 0 || y >= a.h) return x;
    return ls_radio_chip(sf, a, a.x + x, a.y + y, t, on, hue & 15) - a.x;
}
static int chips(ls_radio_chip_t *out, char (*words)[12], const lap_chip_t *in, int n) {
    if (n <= 0 || !in) return 0;
    if (n > 6) n = 6;
    if (!ls_lap_api_readable(in, sizeof(*in) * (size_t)n)) return 0;
    for (int i = 0; i < n; i++) {
        const lap_chip_t c = in[i];
        text(words[i], 12, c.text);
        out[i] = (ls_radio_chip_t){words[i], c.on, (uint8_t)(c.hue & 15)};
    }
    return n;
}
void lap_ui_rail(int x, int y, int w, const lap_chip_t *left, int left_n, const lap_chip_t *right, int right_n) {
    tui_surface *sf;
    tui_rect a;
    if (!canvas(&sf, &a) || y < 0 || y >= a.h) return;
    ls_radio_chip_t l[6], r[6];
    char lw[6][12], rw[6][12];
    const int ln = chips(l, lw, left, left_n), rn = chips(r, rw, right, right_n);
    const tui_rect row = box(a, x, y, w, 1);
    if (!empty(row)) ls_radio_glass_rail(sf, row, row.x, row.y, row.w, l, ln, r, rn);
}
int lap_ui_frequency(int x, int y, int w, int h, uint64_t hz, const char *entry) {
    tui_surface *sf;
    tui_rect a;
    char e[16];
    if (!canvas(&sf, &a) || !text(e, sizeof(e), entry)) return y;
    const tui_rect r = box(a, x, y, w, h);
    if (r.w < 8 || r.h < 2) return y;
    const uint32_t f = hz > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)hz;
    return ls_radio_glass_frequency(&p4_k.glass, sf, r, r.y, f, entry ? e : NULL, 0, ls_tui_is_wide()) - a.y;
}
void lap_ui_bargraph(int x, int y, int w, int rows, float frac, float peak) {
    tui_surface *sf;
    tui_rect a;
    if (!canvas(&sf, &a)) return;
    const tui_rect r = box(a, x, y, w, rows);
    if (!empty(r)) ls_radio_bargraph(sf, r, r.x, r.y, r.w, r.h, frac, peak);
}
void lap_ui_vfo(int slot, int x, int y, int w, int h, const char *label, const char *value, float frac, uint8_t hue) {
    tui_surface *sf;
    tui_rect a;
    char l[TEXT], v[TEXT];
    if (slot < 0 || slot >= VFO_SLOTS || !canvas(&sf, &a) || !text(l, sizeof(l), label) || !text(v, sizeof(v), value))
        return;
    memset(p4_k.vfo[slot], 0, sizeof(p4_k.vfo[slot]));
    const tui_rect r = box(a, x, y, w, h);
    if (r.w < 16 || r.h < 3) return;
    ls_radio_card_t c = {.label = l, .value = v, .frac = frac, .hue = (uint8_t)(hue & 15), .live = true,
                         .settable = true, .thumb_keys = true};
    ls_radio_vfo_card_draw(sf, r, &c);
    p4_k.vfo[slot][0] = c.down;
    p4_k.vfo[slot][1] = c.up;
    p4_k.vfo[slot][2] = c.value_rect;
    p4_k.vfo[slot][3] = c.bar;
}
int lap_ui_vfo_hit(int slot, int col, int row, float *frac) {
    if (!ls_lap_api_inside() || slot < 0 || slot >= VFO_SLOTS) return -1;
    const tui_rect *v = p4_k.vfo[slot];
    const int ax = p4_k.area.x + col, ay = p4_k.area.y + row;
    const int part = ls_radio_vfo_card_hit(v[0], v[1], v[2], v[3], ax, ay);
    if (part == LS_RADIO_PART_BAR && frac && ls_lap_api_readable(frac, sizeof(*frac)))
        *frac = ls_radio_vfo_card_bar_frac(v[3], ax);
    _Static_assert((int)LAP_UI_PART_DOWN == (int)LS_RADIO_PART_DOWN && (int)LAP_UI_PART_UP == (int)LS_RADIO_PART_UP &&
                       (int)LAP_UI_PART_VALUE == (int)LS_RADIO_PART_VALUE && (int)LAP_UI_PART_BAR == (int)LS_RADIO_PART_BAR,
                   "the SDK's parts are the panel's");
    return part;
}

/* ---- meters and history -------------------------------------------------------------- */

void lap_ui_meter(int x, int y, int w, float frac, uint8_t hue) {
    tui_surface *sf;
    tui_rect a;
    if (!canvas(&sf, &a)) return;
    const tui_rect r = box(a, x, y, w, 1);
    if (r.w >= 3) ls_meter_ascii(sf, r, r.x, r.y, r.w, frac, hue & 15);
}
void lap_ui_sparkline(int x, int y, int w, int h, const int *level, int n, int max, int marker) {
    tui_surface *sf;
    tui_rect a;
    if (!canvas(&sf, &a) || n < 1 || n > 512 || max < 1 || !ls_lap_api_readable(level, sizeof(*level) * (size_t)n))
        return;
    const tui_rect r = box(a, x, y, w, h);
    if (!empty(r)) tui_sparkline(sf, r, level, n, max, marker, tui_theme_get());
}
/* SWEEP's strips: each cell two samples wide and four dots high, filled from
   the bottom (ls_braille_column), newest on the right. */
void lap_ui_history(int x, int y, int w, int h, const float *v, int n, float lo, float hi, uint8_t hue) {
    tui_surface *sf;
    tui_rect a;
    if (!canvas(&sf, &a) || n < 1 || n > 512 || !(hi > lo) || !ls_lap_api_readable(v, sizeof(*v) * (size_t)n)) return;
    const tui_rect r = box(a, x, y, w, h);
    if (empty(r)) return;
    const uint8_t at = TUI_ATTR(hue & 15, TUI_BLACK);
    const int dots = r.h * 4;
    for (int c = 0; c < r.w; c++) {
        int height[2];
        for (int s = 0; s < 2; s++) {
            const int i = n - (r.w - c) * 2 + s;      /* the newest sample is the last dot column */
            float f = i >= 0 ? (v[i] - lo) / (hi - lo) : 0;
            if (!(f > 0)) f = 0;
            if (f > 1) f = 1;
            height[s] = i >= 0 ? (int)(f * (float)dots + 0.5f) : 0;
        }
        for (int row = 0; row < r.h; row++) {
            const int base = (r.h - 1 - row) * 4;     /* dots below this cell */
            uint8_t bits = 0;
            for (int s = 0; s < 2; s++) {
                const int in = height[s] - base;
                if (in > 0) bits |= ls_braille_column((unsigned)(in > 4 ? 4 : in), s);
            }
            ls_braille_put(sf, r, r.x + c, r.y + row, bits ? (int16_t)(0x2800 + bits) : ' ', at);
        }
    }
}

/* ---- the waterfall ----------------------------------------------------------------------- */

bool lap_wf_push(const float *db, int bins, const lap_wf_line_t *line) {
    if (!ls_lap_api_inside() || !p4_k.live || !ls_lap_service_shown() || bins < 1 || bins > LS_WF_BINS_MAX ||
        !ls_lap_api_readable(db, sizeof(*db) * (size_t)bins) || !ls_lap_api_readable(line, sizeof(*line)))
        return false;
    const lap_wf_line_t L = *line;
    if (!(L.top_db > L.floor_db)) return false;
    for (int i = 0; i < bins; i++) {
        float f = (db[i] - L.floor_db) / (L.top_db - L.floor_db);
        p4_k.wf[i] = !(f > 0) ? 0 : f > 1 ? 1 : f;
    }
    ls_wf_claim(LS_WF_OWNER_USER, "APP");
    p4_k.wf_claimed = true;
    const ls_wf_feed_t feed = {.center_hz = L.center_hz > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)L.center_hz,
                               .span_hz = L.span_hz, .floor_db = L.floor_db, .top_db = L.top_db, .live = true,
                               .scaled = true, .gap = L.gap};
    ls_wf_push(LS_WF_OWNER_USER, p4_k.wf, bins, &feed);
    return true;
}
void lap_wf_draw(int x, int y, int w, int h, int style) {
    tui_surface *sf;
    tui_rect a;
    if (!canvas(&sf, &a) || !p4_k.wf_claimed || ls_wf_owner() != LS_WF_OWNER_USER) return;
    const tui_rect r = box(a, x, y, w, h);
    if (r.w < 8 || r.h < 3) return;
    if (style == LAP_WF_FULL) ls_wf_draw(sf, r);
    else ls_wf_draw_mini(sf, r);
}
bool lap_wf_key(lap_key_t key, char ch) {
    if(!ls_lap_radio_control_allowed()) return false;
    if (!ls_lap_api_inside() || !p4_k.wf_claimed || key < LAP_KEY_CHAR || key > LAP_KEY_WHEEL_DOWN) return false;
    return ls_wf_key((ls_tk_t)key, ch);     /* the vocabularies agree through WHEEL_DOWN (ls_lap.c) */
}
bool lap_wf_touch(int col, int row) {
    if(!ls_lap_radio_control_allowed()) return false;
    if (!ls_lap_api_inside() || !p4_k.wf_claimed) return false;
    return ls_wf_touch(p4_k.area.x + col, p4_k.area.y + row);
}
uint64_t lap_wf_marker_hz(void) { return ls_lap_api_inside() && p4_k.wf_claimed ? ls_wf_marker_hz() : 0; }

/* ---- lifetime ---------------------------------------------------------------------------- */

void ls_lap_kit_begin(void) {
    ls_lap_kit_end();
    memset(&p4_k, 0, sizeof(p4_k));
    p4_k.live = true;
}
void ls_lap_kit_end(void) {
    if(!p4_k.live) return; /* a hidden resident cannot close another app's UI */
    dialog_close();
    p4_k.live = false;                      /* before closing: no callback reaches the app */
    if (ls_picker_is(picked)) ls_picker_close();
    p4_k.picker_done = NULL;
    if (p4_k.numpad_ours && ls_numpad_active()) ls_numpad_close();
    p4_k.numpad_ours = false;
    p4_k.numpad_done = NULL;
    options_free();
    if (p4_k.wf_claimed && ls_wf_owner() == LS_WF_OWNER_USER) ls_wf_claim(LS_WF_OWNER_NONE, NULL);
    p4_k.wf_claimed = false;
    p4_k.pad = false;
    memset(p4_k.vfo, 0, sizeof(p4_k.vfo));
}
