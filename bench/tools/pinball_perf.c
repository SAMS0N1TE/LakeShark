/* PINBALL frame cost on the host, stage by stage, with a model of the
   firmware present (ls_tui.c blit_image_cell / cell diff) so the cells and
   pixels a frame pushes to the panel are counted. Deterministic play: the
   same launches and flips every run.

     pinball_perf [frames] [present-mode]
       present-mode 0: every image cell on a new serial (lap_image ring mode)
       present-mode 1: only cells whose source pixels changed (compare mode)
       default: the mode the firmware's lap_image uses (LS_PERF_PRESENT). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__declspec(dllimport) int __stdcall QueryPerformanceCounter(long long *);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long *);
static double pb_sum[8];
static long long pb_last;
static void pb_mark(int i)
{
    long long t;
    QueryPerformanceCounter(&t);
    if (i) pb_sum[i] += (double)(t - pb_last);
    pb_last = t;
}
#define PB_MARK(i) pb_mark(i)
#include "../../app-sdk/apps/pinball/pinball.c"
#include "../../components/apps/tui/lap3d/l3_private.h"

#ifndef LS_PERF_PRESENT
#define LS_PERF_PRESENT 1
#endif

static uint32_t clock_now = 1000;
static int cols = 52, rows = 70;
static const int CW = 10, CH = 17;
static int present_mode = LS_PERF_PRESENT;

int lap_cols(void) { return cols; }
int lap_rows(void) { return rows; }
void lap_cell_px(int *w, int *h) { if (w) *w = CW; if (h) *h = CH; }
uint16_t lap_palette(int c)
{
    static const uint16_t p[16] = {0x0000, 0xB249, 0x4C6B, 0xB447, 0x3B74, 0x8AD4, 0x3C73, 0xAD97,
                                   0x3A09, 0xFB4D, 0x6F0F, 0xFE4A, 0x6DBF, 0xDC5F, 0x5F1C, 0xF7BF};
    return p[c & 15];
}
uint32_t lap_millis(void) { return clock_now; }
uint32_t lap_random(void) { static uint32_t r = 42; r = r * 1664525u + 1013904223u; return r; }
void lap_fullscreen(bool b) { (void)b; }
void lap_immersive(bool b) { (void)b; }
void lap_exit(void) {}
void lap_hold_rotation(bool b) { (void)b; }
void lap_keep_awake(bool b) { (void)b; }
static lap_touch_t finger;
bool lap_touch(lap_touch_t *t) { *t = finger; return finger.down; }
bool lap_motion(lap_motion_t *m) { (void)m; return false; }
bool lap_tone(int h, int ms, int v, int w) { (void)h; (void)ms; (void)v; (void)w; return true; }
void lap_tone_stop(void) {}
bool lap_haptic(int n) { (void)n; return true; }
bool lap_vibrate(int m, int s) { (void)m; (void)s; return true; }
int lap_file_open(const char *n, const char *mode) { (void)n; (void)mode; return -1; }
int lap_file_read(int fd, void *b, size_t n) { (void)fd; (void)b; (void)n; return -1; }
int lap_file_write(int fd, const void *b, size_t n) { (void)fd; (void)b; return (int)n; }
void lap_file_close(int fd) { (void)fd; }

/* ---- the panel model ------------------------------------------------------ */

static uint16_t *fb;
static int fbw, fbh;
static uint16_t *prev_img;
static size_t prev_cap;
static bool prev_ok;
static uint32_t last_serial;
static uint32_t *cell_sig, *cell_prev;      /* text cells: packed glyph + attr */
static long cells_blit, rows_dirty;
static int dirty_lo, dirty_hi;

static void fb_setup(void)
{
    free(fb);
    fbw = cols * CW;
    fbh = rows * CH;
    fb = calloc((size_t)fbw * fbh, 2);
    free(cell_sig); free(cell_prev);
    cell_sig = calloc((size_t)cols * rows, 4);
    cell_prev = calloc((size_t)cols * rows, 4);
    prev_ok = false;
    last_serial = 0;
}

static void touch_rows(int row)
{
    if (row < dirty_lo) dirty_lo = row;
    if (row > dirty_hi) dirty_hi = row;
}

/* ls_tui.c blit_image_cell, the scaled portrait branch. */
static void image_cell(const uint16_t *p, int pw, int ph, int col, int row)
{
    const int iw = cols * CW, ih = rows * CH;
    uint16_t sxs[32], sys[32];
    for (int x = 0; x < CW; x++) sxs[x] = (uint16_t)(((col * CW + x) * pw / iw) % pw);
    for (int y = 0; y < CH; y++) sys[y] = (uint16_t)(((row * CH + y) * ph / ih) % ph);
    const int x0 = col * CW, y0 = row * CH;
    for (int y = 0; y < CH; y++) {
        uint16_t *run = fb + (size_t)(y0 + y) * fbw + x0;
        const uint16_t *src = p + (size_t)sys[y] * pw;
        for (int x = 0; x < CW; x++) run[x] = src[sxs[x]];
    }
    cells_blit++;
    touch_rows(row);
}

void lap_image(int x, int y, int w, int h, const uint16_t *p, int pw, int ph, uint32_t serial)
{
    (void)x; (void)y;
    if (serial == last_serial) return;
    last_serial = serial;
    const size_t n = (size_t)pw * ph;
    if (n > prev_cap) { free(prev_img); prev_img = malloc(n * 2); prev_cap = n; prev_ok = false; }
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
            if (present_mode == 1 && prev_ok) {
                const int sx0 = c * pw / w, sx1 = ((c + 1) * pw + w - 1) / w;
                const int sy0 = r * ph / h, sy1 = ((r + 1) * ph + h - 1) / h;
                bool same = true;
                for (int yy = sy0; same && yy < sy1; yy++)
                    same = !memcmp(p + (size_t)yy * pw + sx0, prev_img + (size_t)yy * pw + sx0,
                                   (size_t)(sx1 - sx0) * 2);
                if (same) continue;
            }
            image_cell(p, pw, ph, c, r);
        }
    if (present_mode == 1) { memcpy(prev_img, p, n * 2); prev_ok = true; }
}

/* lap_dots packs each cell to a glyph and two colours; the TUI then redraws
   only cells whose glyph or colours changed. Model: the six dots as the
   cell's signature, a changed cell costs a text-cell blit (170 px fill). */
void lap_dots(int x, int y, int w, int h, const uint8_t *dots)
{
    (void)x; (void)y;
    const int dw = w * 2;
    for (int r = 0; r < h && r < rows; r++)
        for (int c = 0; c < w && c < cols; c++) {
            uint32_t sig = 2166136261u;
            for (int k = 0; k < 6; k++) sig = (sig ^ dots[(size_t)(3 * r + k / 2) * dw + 2 * c + (k & 1)]) * 16777619u;
            cell_sig[r * cols + c] = sig;
        }
    for (int i = 0; i < cols * rows; i++)
        if (cell_sig[i] != cell_prev[i]) {
            cell_prev[i] = cell_sig[i];
            const int col = i % cols, row = i / cols;
            for (int yy = 0; yy < CH; yy++) {
                uint16_t *run = fb + (size_t)(row * CH + yy) * fbw + col * CW;
                for (int xx = 0; xx < CW; xx++) run[xx] = (uint16_t)cell_sig[i];
            }
            cells_blit++;
            touch_rows(row);
        }
}
void lap_clear(uint8_t a) { (void)a; }
void lap_put_str(int x, int y, const char *s, uint8_t a) { (void)x; (void)y; (void)s; (void)a; }

/* ---- the runs ------------------------------------------------------------- */

typedef struct {
    double stage[8], worst, total;
    double shadow_us, raster_us, shade_us, resolve_us, l3_us;
    double dots, cells, flush_rows;
    long frames;
} result_t;

static double qpc_ms;

static void frame(result_t *R, bool count, int f, bool play)
{
    clock_now += 33;
    if (play) {
        /* Flip in a fixed rhythm; launch whenever the ball waits. */
        if (f % 17 == 0) key_l = clock_now + 160;
        if (f % 23 == 5) key_r = clock_now + 160;
        if (state == ST_PLAY && in_lane && f % 9 == 0) launch(0.85f);
        if (state == ST_OVER) start();
    }
    memset(pb_sum, 0, sizeof(pb_sum));
    cells_blit = 0;
    dirty_lo = 1 << 30; dirty_hi = -1;
    long long t0, t1;
    QueryPerformanceCounter(&t0);
    draw();
    QueryPerformanceCounter(&t1);
    if (!count) return;
    const double ms = (double)(t1 - t0) * qpc_ms;
    R->total += ms;
    if (ms > R->worst) R->worst = ms;
    for (int i = 1; i < 8; i++) R->stage[i] += pb_sum[i] * qpc_ms;
    if (camera_mode != 2 && scene) {
        l3_stats_t st;
        l3_stats(scene, &st);
        R->shadow_us += st.us_shadow;
        R->raster_us += st.us_raster;
        R->shade_us += st.us_shade;
        R->resolve_us += st.us_resolve;
        R->l3_us += st.us_total;
        R->dots += st.dots_shaded;
    }
    R->cells += (double)cells_blit;
    if (dirty_hi >= dirty_lo) R->flush_rows += (double)(dirty_hi - dirty_lo + 1);
    R->frames++;
    /* PB_DEBUG=1: which dynamic commands changed (engine internals). */
    if (getenv("PB_DEBUG") && camera_mode != 2 && scene && R->frames < 6) {
        const l3_dyn *d = scene->dyn[!scene->dyn_flip];
        printf("frame %ld changed:", R->frames);
        for (int k = 0; k < scene->ndyn; k++) if (d[k].changed) printf(" %d(kind %d cmd %d)", k, scene->cmd[d[k].cmd].kind, d[k].cmd);
        printf("\n");
    }
}

static void run(const char *label, int c, int r, int mach, int cam, bool play, int frames)
{
    cols = c; rows = r;
    fb_setup();
    machine = mach;
    camera_mode = cam;
    enter();
    build_table();
    view_dirty = true;
    state = ST_TITLE;
    level = NORMAL;
    if (play) { start(); }
    result_t R;
    memset(&R, 0, sizeof(R));
    for (int f = 0; f < frames + 20; f++) frame(&R, f >= 20, f, play);
    leave();
    const double n = (double)R.frames;
    printf("%-26s %6.3f %6.3f | %6.3f %6.3f %6.3f %6.3f %6.3f %6.3f | %5.0f %5.0f %5.0f %5.0f | %6.0f %5.0f %4.0f\n",
           label, R.total / n, R.worst, R.stage[1] / n, R.stage[2] / n, R.stage[3] / n, R.stage[4] / n,
           R.stage[5] / n, (R.stage[6] + R.stage[7]) / n, R.shadow_us / n, R.raster_us / n, R.shade_us / n,
           R.resolve_us / n, R.dots / n, R.cells / n, R.flush_rows / n);
}

int main(int argc, char **argv)
{
    long long f;
    QueryPerformanceFrequency(&f);
    qpc_ms = 1000.0 / (double)f;
    const int frames = argc > 1 ? atoi(argv[1]) : 600;
    if (argc > 2) present_mode = atoi(argv[2]);
    printf("present mode %d, %d frames each, 33 ms clock steps\n", present_mode, frames);
    printf("%-26s %6s %6s | %6s %6s %6s %6s %6s %6s | %5s %5s %5s %5s | %6s %5s %4s\n", "scene", "ms", "worst",
           "game", "record", "l3_end", "2d", "rgb", "show", "shdw", "rast", "shade", "rslv", "dots", "cells",
           "rows");
    static const struct { const char *name; int c, r; } grid[] = { {"52x70", 52, 70}, {"118x31", 118, 31} };
    for (int g = 0; g < 2; g++) {
        char label[64];
        for (int m = 0; m < MACHINES; m++) {
            for (int cam = 0; cam < 3; cam++) {
                snprintf(label, sizeof label, "%s %s %s play", grid[g].name, m ? "ION" : "REEF",
                         cam == 0 ? "3D" : cam == 1 ? "TOP" : "2D");
                run(label, grid[g].c, grid[g].r, m, cam, true, frames);
            }
            snprintf(label, sizeof label, "%s %s 3D title", grid[g].name, m ? "ION" : "REEF");
            run(label, grid[g].c, grid[g].r, m, 0, false, frames / 3);
        }
    }
    return 0;
}
