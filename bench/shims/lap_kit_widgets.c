/* Stand-ins for the board widgets ls_lap_kit.c wraps, for the bench targets
 * that link the app API without the whole TUI (lssim links the real ones).
 * Each records what it was handed so test_lap_kit can check that a wrapper
 * clipped, copied and translated before the board's code saw anything. */
#include "lap_kit_widgets.h"
#include "ls_numpad.h"
#include "ls_options.h"
#include "ls_picker.h"
#include "ls_radio_panel.h"
#include "ls_tui_ui.h"
#include "ls_waterfall.h"
#include "tuilib/tui_controls.h"
#include <string.h>

lap_kit_seen_t lap_kit_seen;
static void saw(const char *what, tui_rect r) {
    lap_kit_seen.calls++;
    lap_kit_seen.last = r;
    snprintf(lap_kit_seen.what, sizeof(lap_kit_seen.what), "%s", what);
}

int ls_btn_raised_height(tui_rect area, int n) { return n > 0 && area.h > 0 ? 3 : 0; }
void ls_btn_bar_raised_slot(tui_surface *sf, tui_rect bar, const ls_btn_t *btn, int n, int focus, int slot) {
    (void)sf; (void)focus; (void)slot;
    saw("keys", bar);
    lap_kit_seen.n = n;
    for (int i = 0; i < n && i < 12; i++) {
        snprintf(lap_kit_seen.label[i], sizeof(lap_kit_seen.label[i]), "%s", btn[i].label ? btn[i].label : "");
        lap_kit_seen.label_ptr[i] = btn[i].label;
        lap_kit_seen.hit[i] = tui_rect_make(bar.x + i * (bar.w / n), bar.y,
                                            i == n - 1 ? bar.w - i * (bar.w / n) : bar.w / n, bar.h);
    }
}
int ls_btn_hit_slot(int col, int row, int slot) {
    (void)slot;
    for (int i = 0; i < lap_kit_seen.n; i++)
        if (tui_rect_contains(lap_kit_seen.hit[i], col, row)) return i;
    return -1;
}
int ls_btn_shortcut(char ch, int slot) { (void)slot; return ch == 'o' ? 0 : -1; }

static const ls_opt_ctx_t *opt_open;
void ls_opt_open(const ls_opt_ctx_t *ctx) { opt_open = ctx; lap_kit_seen.opt = ctx; }
void ls_opt_close(void) { opt_open = NULL; lap_kit_seen.opt_closes++; }
int ls_opt_count(const ls_opt_ctx_t *ctx) { return ctx ? ctx->n : 0; }
bool ls_opt_covers_screen(void) { return opt_open != NULL; }
bool ls_opt_returning(void) { return false; }

static ls_picker_done_t picker;
void ls_picker_open(const char *title, ls_picker_done_t done) {
    picker = done;
    lap_kit_seen.picker_rows = 0;
    snprintf(lap_kit_seen.what, sizeof(lap_kit_seen.what), "%s", title);
}
bool ls_picker_add(const char *label, const char *detail) { (void)label; (void)detail; lap_kit_seen.picker_rows++; return true; }
void ls_picker_select(int index) { (void)index; }
void ls_picker_close(void) { picker = NULL; lap_kit_seen.picker_closes++; }
bool ls_picker_is(ls_picker_done_t done) { return picker && picker == done; }
void lap_kit_pick(int index) { ls_picker_done_t d = picker; picker = NULL; if (d) d(index); }

static ls_numpad_done_t numpad;
void ls_numpad_open(const char *title, const char *unit, double initial, ls_numpad_done_t done) {
    (void)title; (void)unit; (void)initial; numpad = done;
}
void ls_numpad_close(void) { numpad = NULL; }
bool ls_numpad_active(void) { return numpad != NULL; }
void lap_kit_enter(double v) { ls_numpad_done_t d = numpad; numpad = NULL; if (d) d(v); }

void ls_freq_pad_draw(tui_surface *sf, tui_rect area, const char *entry, bool invalid, uint32_t blink,
                      tui_rect keys[12], tui_rect actions[3]) {
    (void)sf; (void)entry; (void)invalid; (void)blink;
    saw("pad", area);
    for (int i = 0; i < 12; i++) keys[i] = tui_rect_make(area.x + i, area.y, 1, 1);
    for (int i = 0; i < 3; i++) actions[i] = tui_rect_make(area.x + i, area.y + 1, 1, 1);
}
char ls_freq_pad_hit(const tui_rect keys[12], const tui_rect actions[3], int col, int row) {
    for (int i = 0; i < 10; i++) if (tui_rect_contains(keys[i], col, row)) return (char)('0' + i);
    (void)actions;
    return 0;
}
void ls_radio_bezel(tui_surface *sf, tui_rect r, const char *title) { (void)sf; (void)title; saw("bezel", r); }
int ls_radio_chip(tui_surface *sf, tui_rect box, int x, int y, const char *text, bool on, uint8_t hue) {
    (void)sf; (void)on; (void)hue;
    saw("chip", tui_rect_make(x, y, (int)strlen(text) + 2, 1));
    (void)box;
    return x + (int)strlen(text) + 3;
}
void ls_radio_glass_rail(tui_surface *sf, tui_rect r, int x, int y, int width, const ls_radio_chip_t *left, int ln,
                         const ls_radio_chip_t *right, int rn) {
    (void)sf; (void)x; (void)y; (void)width; (void)left; (void)right;
    saw("rail", r);
    lap_kit_seen.n = ln + rn;
}
int ls_radio_glass_frequency(ls_radio_glass_t *g, tui_surface *sf, tui_rect r, int y, uint32_t hz, const char *entry,
                             uint32_t blink, bool wide) {
    (void)g; (void)sf; (void)entry; (void)blink; (void)wide;
    saw("frequency", r);
    lap_kit_seen.hz = hz;
    return y + 5;
}
void ls_radio_bargraph(tui_surface *sf, tui_rect box, int x, int y, int w, int rows, float frac, float peak) {
    (void)sf; (void)x; (void)y; (void)w; (void)rows; (void)frac; (void)peak;
    saw("bargraph", box);
}
void ls_radio_vfo_card_draw(tui_surface *sf, tui_rect box, ls_radio_card_t *card) {
    (void)sf;
    saw("vfo", box);
    card->down = tui_rect_make(box.x, box.y + 1, 3, 2);
    card->up = tui_rect_make(box.x + box.w - 3, box.y + 1, 3, 2);
    card->value_rect = tui_rect_make(box.x, box.y, box.w, 1);
    card->bar = tui_rect_make(box.x + 3, box.y + 1, box.w - 6, 2);
}
int ls_radio_vfo_card_hit(tui_rect down, tui_rect up, tui_rect value, tui_rect bar, int col, int row) {
    if (tui_rect_contains(down, col, row)) return LS_RADIO_PART_DOWN;
    if (tui_rect_contains(up, col, row)) return LS_RADIO_PART_UP;
    if (tui_rect_contains(value, col, row)) return LS_RADIO_PART_VALUE;
    if (tui_rect_contains(bar, col, row)) return LS_RADIO_PART_BAR;
    return -1;
}
float ls_radio_vfo_card_bar_frac(tui_rect bar, int col) { return bar.w > 1 ? (float)(col - bar.x) / (float)(bar.w - 1) : 0; }
void ls_meter_ascii(tui_surface *sf, tui_rect clip, int x, int y, int w, float fraction, uint8_t hue) {
    (void)sf; (void)x; (void)y; (void)w; (void)fraction; (void)hue;
    saw("meter", clip);
}
void tui_sparkline(tui_surface *s, tui_rect r, const int level[], int n, int max, int marker, const tui_theme *th) {
    (void)s; (void)level; (void)max; (void)marker; (void)th;
    saw("sparkline", r);
    lap_kit_seen.n = n;
}
const tui_theme *tui_theme_get(void) { return NULL; }
bool ls_tui_is_wide(void) { return false; }

static ls_wf_owner_t wf_owner;
static const ls_opt_ctx_t wf_ctx = {.name = "WATERFALL"};
const struct ls_opt_ctx_s *ls_wf_options(void) { return &wf_ctx; }
void ls_wf_claim(ls_wf_owner_t owner, const char *label) { (void)label; wf_owner = owner; }
ls_wf_owner_t ls_wf_owner(void) { return wf_owner; }
void ls_wf_push(ls_wf_owner_t owner, const float *bins, int n, const ls_wf_feed_t *feed) {
    if (owner != wf_owner) return;
    lap_kit_seen.wf_pushes++;
    lap_kit_seen.wf_bins = n;
    lap_kit_seen.wf_first = n ? bins[0] : -1;
    lap_kit_seen.wf_last = n ? bins[n - 1] : -1;
    lap_kit_seen.hz = feed->center_hz;
}
void ls_wf_draw(tui_surface *sf, tui_rect area) { (void)sf; saw("wf", area); }
void ls_wf_draw_mini(tui_surface *sf, tui_rect area) { (void)sf; saw("wf-mini", area); }
bool ls_wf_key(ls_tk_t key, char ch) { (void)key; (void)ch; return false; }
bool ls_wf_touch(int col, int row) { lap_kit_seen.last = tui_rect_make(col, row, 1, 1); return true; }
uint32_t ls_wf_marker_hz(void) { return 0; }
