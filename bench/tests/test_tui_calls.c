#include "ls_test.h"
#include "ls_calls.h"
#include "call_archive.h"
#include "ls_picker.h"
#include <stdlib.h>

static bool wide;
static int tracks = 2, deletes, focus;
static int option[CALL_OPTIONS] = {1, 1, 500, 0, 0};
static ls_picker_done_t confirmed;
static char deleted[CALL_PATH_MAX], labels[2][32];
static int added;
static const ls_opt_ctx_t *opened_options;
bool ls_tui_is_wide(void) { return wide; }
bool ls_tui_keyboard_mode(void) { return false; }
#ifndef CALLS_RENDER_TEST
int ls_tui_corner_pad(int row) { (void)row; return 0; }
void ls_tui_geometry(int *cols, int *rows, int *cw, int *ch)
{ if (cols) *cols = wide ? 100 : 40; if (rows) *rows = wide ? 26 : 65; if (cw) *cw = 8; if (ch) *ch = 16; }
#endif
const char *ls_tui_radio_claimed(void) { return "P25"; }
void call_archive_init(void) {}
void call_archive_refresh(void) {}
bool call_archive_busy(void) { return true; }
unsigned call_archive_peak(void) { return 22000; }
unsigned call_archive_drops(void) { return 13; }
unsigned call_archive_errors(void) { return 0; }
int call_archive_count(void) { return tracks; }
bool call_archive_entry(int index, call_entry_t *entry)
{
    if (index < 0 || index >= tracks) return false;
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->path, sizeof(entry->path), "/sdcard/lakeshark/calls/20261007/12000%d_%d.wav", index, 42 + index);
    entry->meta.time = 1791374400 - index;
    entry->meta.hz = 851000000; entry->meta.talkgroup = 42 + index; entry->duration_ms = 1500;
    return true;
}
bool call_archive_delete(const char *path) { deletes++; snprintf(deleted, sizeof(deleted), "%s", path); return true; }
int call_archive_option(call_option_t o) { return option[o]; }
void call_archive_set_option(call_option_t o, int v) { option[o] = v; }
const char *call_archive_talkgroups(void) { return "42,43"; }
bool call_archive_set_talkgroups(const char *text) { return true; }
void ls_opt_open(const ls_opt_ctx_t *ctx) { opened_options = ctx; }
ls_btn_t ls_opt_button(const ls_opt_ctx_t *ctx) { (void)ctx; return (ls_btn_t){.label = "OPTIONS", .key = 'o'}; }
void ls_picker_open(const char *title, ls_picker_done_t done) { (void)title; confirmed = done; added = 0; }
bool ls_picker_add(const char *label, const char *detail)
{ (void)detail; if (added < 2) snprintf(labels[added++], 32, "%s", label); return true; }
void ls_picker_select(int index) { focus = index; }

static bool locate(tui_cell *grid, int w, int h, const char *text, int *x, int *y)
{
    size_t n = strlen(text);
    for (int row = 0; row < h; ++row) for (int col = 0; col <= w - (int)n; ++col) {
        size_t i = 0; while (i < n && grid[row * w + col + i].ch == text[i]) ++i;
        if (i == n) { *x = col; *y = row; return true; }
    }
    return false;
}
static void exercise(int w, int h)
{
    size_t n = (size_t)w * h;
    tui_cell *back = calloc(n + 2, sizeof(*back)), *front = calloc(n, sizeof(*front));
    LS_CHECK(back && front); if (!back || !front) return;
    back[0].ch = 'A'; back[n + 1].ch = 'Z'; wide = w > h;
    tui_surface sf; tui_surface_setup(&sf, back + 1, front, w, h);
#ifdef CALLS_RENDER_TEST
    LS_CHECK(ls_tui_begin(w * 10, h * 17));
#endif
    ls_calls_open("P25"); tui_frame_begin(&sf); ls_calls_draw(&sf, tui_rect_make(0, 0, w, h));
    const char *buttons[] = {"PLAY", "STOP", "DELETE", "OPTIONS", "BACK"};
    int x, y;
    for (unsigned i = 0; i < 5; ++i) LS_CHECK_MSG(locate(back + 1, w, h, buttons[i], &x, &y), "missing %s at %dx%d", buttons[i], w, h);
    LS_CHECK(locate(back + 1, w, h, "TG 42", &x, &y));
    deletes = 0; ls_calls_key(LS_TK_CHAR, 'd');
    LS_EQ_STR(labels[0], "CANCEL"); LS_EQ_STR(labels[1], "DELETE"); LS_EQ_INT(focus, 0);
    confirmed(0); LS_EQ_INT(deletes, 0);
    ls_calls_key(LS_TK_CHAR, 'd');
    /* Selection can change behind the dialog; confirmation still names its call. */
    ls_calls_key(LS_TK_DOWN, 0); confirmed(1); LS_EQ_INT(deletes, 1);
    LS_CHECK(strstr(deleted, "120000_42.wav"));
    opened_options = NULL; ls_calls_key(LS_TK_CHAR, 'o'); LS_CHECK(opened_options == &ls_calls_options);
    if (locate(back + 1, w, h, "BACK", &x, &y)) ls_calls_touch(x, y);
    LS_CHECK(!ls_calls_active());
    LS_EQ_INT(back[0].ch, 'A'); LS_EQ_INT(back[n + 1].ch, 'Z'); free(back); free(front);
#ifdef CALLS_RENDER_TEST
    ls_tui_end();
#endif
}
LS_CASE(portrait_landscape_and_compact_touch_controls)
{
    exercise(40, 65); exercise(52, 70); exercise(100, 26); exercise(65, 20);
}
LS_CASE(options_use_shared_toggles_and_both_way_levels)
{
    const ls_opt_t *o = ls_calls_options.opt;
    LS_EQ_INT(o[1].kind, LS_OPT_TOGGLE); LS_EQ_INT(o[2].kind, LS_OPT_TOGGLE);
    LS_EQ_INT(o[3].kind, LS_OPT_LEVEL); LS_EQ_INT(o[4].kind, LS_OPT_LEVEL);
    LS_CHECK(o[3].step > 0 && o[4].step > 0);
    o[3].set_num(&o[3], 1000); LS_EQ_INT(o[3].num(&o[3]), 1000);
    o[3].set_num(&o[3], 900); LS_EQ_INT(o[3].num(&o[3]), 900);
}
LS_CASE(empty_archive_cannot_delete_a_stale_selection)
{
    tracks = 0; ls_calls_open("FM");
    tui_cell back[40 * 20], front[40 * 20]; tui_surface sf;
    tui_surface_setup(&sf, back, front, 40, 20); tui_frame_begin(&sf);
    ls_calls_draw(&sf, tui_rect_make(0, 0, 40, 20));
    confirmed = NULL; ls_calls_key(LS_TK_CHAR, 'd'); LS_CHECK(!confirmed);
    ls_calls_leave(); tracks = 2;
}

#ifdef CALLS_RENDER_TEST
#include "ls_panel.h"
static uint16_t pixels[568 * 1232];
bool ls_panel_fb(ls_panel_fb_t *out)
{
    memset(out, 0, sizeof(*out)); out->pixels = pixels; out->width = 568; out->height = 1232; return true;
}
void ls_panel_fb_present(void) {}
void ls_panel_fb_present_rows(int y0, int y1) { (void)y0; (void)y1; }
LS_CASE(render_calls_with_production_font_palette_and_blitter)
{
    for (int landscape = 0; landscape < 2; ++landscape) {
        int w = landscape ? 1232 : 568, h = landscape ? 568 : 1232;
        wide = landscape; LS_CHECK(ls_tui_begin(w, h));
        int cols, rows; ls_tui_geometry(&cols, &rows, NULL, NULL);
        ls_calls_open("P25"); tui_surface *sf = ls_tui_surface(); tui_frame_begin(sf);
        ls_calls_draw(sf, tui_rect_make(0, 0, cols, rows)); LS_CHECK(ls_tui_present() > 0);
        const char *directory = getenv("LS_CALLS_RENDER_DIR");
        char path[256]; snprintf(path, sizeof(path), "%s/calls-%s.ppm", directory ? directory : ".", landscape ? "landscape" : "portrait");
        FILE *f = directory ? fopen(path, "wb") : NULL;
        if (directory) LS_CHECK(f);
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                uint16_t p = landscape ? pixels[(1231 - x) * 568 + y] : pixels[y * 568 + x];
                uint8_t rgb[3] = {(uint8_t)(((p >> 11) & 31) * 255 / 31),
                    (uint8_t)(((p >> 5) & 63) * 255 / 63), (uint8_t)((p & 31) * 255 / 31)};
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
        ls_calls_leave(); ls_tui_end();
    }
}
#endif
