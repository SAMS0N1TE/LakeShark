/* See ls_lap_kit.h and app-sdk/include/lakeshark_ui.h.
 *
 * Every wrapper here is a few lines over a widget the board already has:
 * it reads the app's arguments once (pointers checked with ls_lap_api_owns,
 * strings copied byte by byte while they are the app's), turns the app's
 * cells into absolute ones clipped to its area, and draws with the board's
 * code. Callbacks back into the app go through trampolines that check the
 * pointer is the app's code and enter the loader's guard; ls_lap_kit_end
 * cuts them all off before the app's code can go away. */
#include "ls_lap_kit.h"
#include "../../../app-sdk/include/lakeshark_ui.h"
#include "ls_lap_api.h"
#include "ls_lap_service.h"
#include "ls_braille.h"
#include "ls_numpad.h"
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
} k;

/* ---- reading the app's arguments --------------------------------------------- */

/* The app's string, cut to cap; false when any byte of it is not the app's. */
static bool text(char *dst, size_t cap, const char *src) {
    dst[0] = 0;
    if (!src) return true;
    size_t i = 0;
    for (; i + 1 < cap; i++) {
        if (!ls_lap_api_owns(src + i, 1)) { dst[0] = 0; return false; }
        if (!(dst[i] = src[i])) return true;
    }
    dst[i] = 0;
    return true;
}
static bool canvas(tui_surface **sf, tui_rect *area) {
    if (!ls_lap_api_canvas(sf, area)) return false;
    k.area = *area;
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
    if (!canvas(&sf, &a) || n < 1 || n > LAP_KEYS_MAX || !ls_lap_api_owns(keys, sizeof(*keys) * (size_t)n)) return;
    const tui_rect r = box(a, x, y, w, h);
    if (empty(r)) return;
    for (int i = 0; i < n; i++) {
        const lap_btn_t b = keys[i];
        text(k.btn_text[i][0], TEXT, b.label);
        text(k.btn_text[i][1], TEXT, b.value);
        k.btn[i] = (ls_btn_t){k.btn_text[i][0], b.value ? k.btn_text[i][1] : NULL,
                              b.key >= 32 && b.key < 127 ? b.key : 0, b.on, b.dim, b.badge};
    }
    ls_btn_bar_raised_slot(sf, r, k.btn, n, focus >= 0 && focus < n ? focus : -1, LS_BTN_SLOT_SCREEN);
}
int lap_ui_key_hit(int col, int row) {
    if (!ls_lap_api_inside()) return -1;
    return ls_btn_hit_slot(k.area.x + col, k.area.y + row, LS_BTN_SLOT_SCREEN);
}
int lap_ui_key_find(char ch) {
    return ls_lap_api_inside() ? ls_btn_shortcut(ch, LS_BTN_SLOT_SCREEN) : -1;
}

/* ---- OPTIONS ------------------------------------------------------------------- */

static const opt_row_t *row_of(const ls_opt_t *o) {
    return k.live && k.opts && o && o->arg >= 0 && o->arg < k.opts->nrows ? &k.opts->meta[o->arg] : NULL;
}
/* While a row's callback runs, ls_options is still using the table, so the
   app may close the list from it but not replace or free it. */
static int in_row;
static double call_get(const ls_opt_t *o) {
    const opt_row_t *m = row_of(o);
    if (!m || !ls_lap_code_ok((const void *)m->get)) return 0;
    in_row++;
    ls_lap_call_begin();
    const double v = m->get(m->id);
    ls_lap_call_end();
    in_row--;
    return v;
}
static void call_set(const ls_opt_t *o, double v) {
    const opt_row_t *m = row_of(o);
    if (!m || !ls_lap_code_ok((const void *)m->set)) return;
    in_row++;
    ls_lap_call_begin();
    m->set(m->id, v);
    ls_lap_call_end();
    in_row--;
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
    if (depth > 3 || t->nctx >= OPT_LISTS || !ls_lap_api_owns(l, sizeof(*l))) return -1;
    const lap_opt_list_t L = *l;
    if (L.n < 0 || L.n > OPT_ROWS - t->nrows || (L.n && !ls_lap_api_owns(L.rows, sizeof(*L.rows) * (size_t)L.n)))
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
            if (n < 0 || n > OPT_NAMES || (n && !ls_lap_api_owns(R.names, sizeof(*R.names) * (size_t)n)) ||
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
    if (!k.opts) return;
    ls_opt_close();
    TABLE_FREE(k.opts);
    k.opts = NULL;
}
static bool options_up(void) { return k.opts && (ls_opt_covers_screen() || ls_opt_returning()); }
bool lap_options_open(const lap_opt_list_t *root) {
    if (!ls_lap_api_inside() || !k.live || in_row) return false;
    options_free();
    opt_tree_t *t = TABLE_ALLOC(sizeof(*t));
    if (!t) return false;
    if (add_list(t, root, NULL, NULL, 0) != 0 || ls_opt_count(&t->ctx[0]) <= 0) {
        TABLE_FREE(t);
        return false;
    }
    k.opts = t;
    ls_opt_open(&t->ctx[0]);
    return true;
}
bool lap_options_showing(void) { return ls_lap_api_inside() && options_up(); }
void lap_options_close(void) {
    if (!ls_lap_api_inside() || !k.opts) return;
    if (in_row) ls_opt_close();          /* the table goes at the next open or when the app closes */
    else options_free();
}

/* ---- lists and keypads ----------------------------------------------------------- */

static void picked(int index) {
    void (*done)(int) = k.picker_done;
    k.picker_done = NULL;
    if (!k.live || !ls_lap_code_ok((const void *)done)) return;
    ls_lap_call_begin();
    done(index);
    ls_lap_call_end();
}
bool lap_picker_open(const char *title, void (*done)(int index)) {
    char t[32];
    if (!ls_lap_api_inside() || !k.live || !ls_lap_code_ok((const void *)done) || !text(t, sizeof(t), title))
        return false;
    k.picker_done = done;
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
    void (*done)(double) = k.numpad_done;
    k.numpad_done = NULL;
    k.numpad_ours = false;
    if (!k.live || !ls_lap_code_ok((const void *)done)) return;
    ls_lap_call_begin();
    done(v);
    ls_lap_call_end();
}
bool lap_numpad_open(const char *title, const char *unit, double initial, void (*done)(double value)) {
    char t[32], u[16];
    if (!ls_lap_api_inside() || !k.live || !ls_lap_code_ok((const void *)done) || !text(t, sizeof(t), title) ||
        !text(u, sizeof(u), unit))
        return false;
    k.numpad_done = done;
    k.numpad_ours = true;
    ls_numpad_open(t, u, initial, entered);
    return true;
}
bool lap_ui_overlay(void) {
    return ls_lap_api_inside() && (ls_picker_is(picked) || (k.numpad_ours && ls_numpad_active()) || options_up());
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
    ls_freq_pad_draw(sf, r, e, invalid, blink, k.pad_keys, k.pad_actions);
    k.pad = true;
}
char lap_ui_freq_hit(int col, int row) {
    if (!ls_lap_api_inside() || !k.pad) return 0;
    return ls_freq_pad_hit(k.pad_keys, k.pad_actions, k.area.x + col, k.area.y + row);
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
    if (!ls_lap_api_owns(in, sizeof(*in) * (size_t)n)) return 0;
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
    return ls_radio_glass_frequency(&k.glass, sf, r, r.y, f, entry ? e : NULL, 0, ls_tui_is_wide()) - a.y;
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
    memset(k.vfo[slot], 0, sizeof(k.vfo[slot]));
    const tui_rect r = box(a, x, y, w, h);
    if (r.w < 16 || r.h < 3) return;
    ls_radio_card_t c = {.label = l, .value = v, .frac = frac, .hue = (uint8_t)(hue & 15), .live = true,
                         .settable = true, .thumb_keys = true};
    ls_radio_vfo_card_draw(sf, r, &c);
    k.vfo[slot][0] = c.down;
    k.vfo[slot][1] = c.up;
    k.vfo[slot][2] = c.value_rect;
    k.vfo[slot][3] = c.bar;
}
int lap_ui_vfo_hit(int slot, int col, int row, float *frac) {
    if (!ls_lap_api_inside() || slot < 0 || slot >= VFO_SLOTS) return -1;
    const tui_rect *v = k.vfo[slot];
    const int ax = k.area.x + col, ay = k.area.y + row;
    const int part = ls_radio_vfo_card_hit(v[0], v[1], v[2], v[3], ax, ay);
    if (part == LS_RADIO_PART_BAR && frac && ls_lap_api_owns(frac, sizeof(*frac)))
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
    if (!canvas(&sf, &a) || n < 1 || n > 512 || max < 1 || !ls_lap_api_owns(level, sizeof(*level) * (size_t)n))
        return;
    const tui_rect r = box(a, x, y, w, h);
    if (!empty(r)) tui_sparkline(sf, r, level, n, max, marker, tui_theme_get());
}
/* SWEEP's strips: each cell two samples wide and four dots high, filled from
   the bottom (ls_braille_column), newest on the right. */
void lap_ui_history(int x, int y, int w, int h, const float *v, int n, float lo, float hi, uint8_t hue) {
    tui_surface *sf;
    tui_rect a;
    if (!canvas(&sf, &a) || n < 1 || n > 512 || !(hi > lo) || !ls_lap_api_owns(v, sizeof(*v) * (size_t)n)) return;
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
    if (!ls_lap_api_inside() || !k.live || !ls_lap_service_shown() || bins < 1 || bins > LS_WF_BINS_MAX ||
        !ls_lap_api_owns(db, sizeof(*db) * (size_t)bins) || !ls_lap_api_owns(line, sizeof(*line)))
        return false;
    const lap_wf_line_t L = *line;
    if (!(L.top_db > L.floor_db)) return false;
    for (int i = 0; i < bins; i++) {
        float f = (db[i] - L.floor_db) / (L.top_db - L.floor_db);
        k.wf[i] = !(f > 0) ? 0 : f > 1 ? 1 : f;
    }
    ls_wf_claim(LS_WF_OWNER_USER, "APP");
    k.wf_claimed = true;
    const ls_wf_feed_t feed = {.center_hz = L.center_hz > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)L.center_hz,
                               .span_hz = L.span_hz, .floor_db = L.floor_db, .top_db = L.top_db, .live = true,
                               .scaled = true, .gap = L.gap};
    ls_wf_push(LS_WF_OWNER_USER, k.wf, bins, &feed);
    return true;
}
void lap_wf_draw(int x, int y, int w, int h, int style) {
    tui_surface *sf;
    tui_rect a;
    if (!canvas(&sf, &a) || !k.wf_claimed || ls_wf_owner() != LS_WF_OWNER_USER) return;
    const tui_rect r = box(a, x, y, w, h);
    if (r.w < 8 || r.h < 3) return;
    if (style == LAP_WF_FULL) ls_wf_draw(sf, r);
    else ls_wf_draw_mini(sf, r);
}
bool lap_wf_key(lap_key_t key, char ch) {
    if (!ls_lap_api_inside() || !k.wf_claimed || key < LAP_KEY_CHAR || key > LAP_KEY_WHEEL_DOWN) return false;
    return ls_wf_key((ls_tk_t)key, ch);     /* the vocabularies agree through WHEEL_DOWN (ls_lap.c) */
}
bool lap_wf_touch(int col, int row) {
    if (!ls_lap_api_inside() || !k.wf_claimed) return false;
    return ls_wf_touch(k.area.x + col, k.area.y + row);
}
uint64_t lap_wf_marker_hz(void) { return ls_lap_api_inside() && k.wf_claimed ? ls_wf_marker_hz() : 0; }

/* ---- lifetime ---------------------------------------------------------------------------- */

void ls_lap_kit_begin(void) {
    ls_lap_kit_end();
    memset(&k, 0, sizeof(k));
    k.live = true;
}
void ls_lap_kit_end(void) {
    k.live = false;                      /* before closing: no callback reaches the app */
    if (ls_picker_is(picked)) ls_picker_close();
    k.picker_done = NULL;
    if (k.numpad_ours && ls_numpad_active()) ls_numpad_close();
    k.numpad_ours = false;
    k.numpad_done = NULL;
    options_free();
    if (k.wf_claimed && ls_wf_owner() == LS_WF_OWNER_USER) ls_wf_claim(LS_WF_OWNER_NONE, NULL);
    k.wf_claimed = false;
    k.pad = false;
    memset(k.vfo, 0, sizeof(k.vfo));
}
