/* See ls_pixel_wf.h. */
#include "ls_pixel_wf.h"
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

static void *ps_calloc(size_t n, size_t size)
{
#ifdef ESP_PLATFORM
    return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return calloc(n, size);
#endif
}
static void ps_free(void *q)
{
#ifdef ESP_PLATFORM
    heap_caps_free(q);
#else
    free(q);
#endif
}

void ls_pwf_release(ls_pwf_t *p)
{
    const uint16_t *src = NULL;
    if (p && p->img && ls_tui_image_now(NULL, &src, NULL, NULL, NULL) && src == p->img)
        ls_tui_image(tui_rect_make(0, 0, 0, 0), NULL, 0, 0, 0);
}

void ls_pwf_free(ls_pwf_t *p)
{
    if (!p) return;
    ls_pwf_release(p);
    ps_free(p->hist);
    ps_free(p->img);
    ps_free(p->first);
    ps_free(p->last);
    p->hist = NULL;
    p->img = NULL;
    p->first = p->last = NULL;
    p->w = p->h = p->bins = p->lines = p->freq_px = 0;
}

void ls_pwf_clear(ls_pwf_t *p)
{
    if (!p) return;
    p->head = p->filled = p->fresh = p->pos = p->moved = 0;
    p->redraw = true;
}

bool ls_pwf_size(ls_pwf_t *p, ls_pwf_flow_t flow, int w, int h, int bins)
{
    if (!p || w < 1 || h < 1 || bins < 1) return false;
    if (p->img && p->flow == flow && p->w == w && p->h == h && p->bins == bins) return true;
    ls_pwf_free(p);
    p->flow = flow;
    const int lines = flow == LS_PWF_LEFT ? w : h, freq = flow == LS_PWF_LEFT ? h : w;
    p->hist = ps_calloc((size_t)lines, (size_t)bins);
    p->img = ps_calloc((size_t)w * h, sizeof(uint16_t));
    p->first = ps_calloc((size_t)freq, sizeof(int16_t));
    p->last = ps_calloc((size_t)freq, sizeof(int16_t));
    if (!p->hist || !p->img || !p->first || !p->last) {
        ls_pwf_free(p);
        return false;
    }
    p->w = w;
    p->h = h;
    p->bins = bins;
    p->lines = lines;
    p->freq_px = freq;
    ls_pwf_axis_even(p, 0, bins - 1);
    ls_pwf_clear(p);
    return true;
}

void ls_pwf_axis_changed(ls_pwf_t *p)
{
    if (p) p->redraw = true;
}

void ls_pwf_axis_even(ls_pwf_t *p, int lo, int hi)
{
    if (!p || !p->first || hi < lo) return;
    const int n = hi - lo + 1, f = p->freq_px;
    for (int i = 0; i < f; i++) {
        /* Pixel i from the low end. */
        int a = lo + i * n / f, b = lo + (i + 1) * n / f - 1;
        if (b < a) b = a;
        const int at = p->flow == LS_PWF_LEFT ? f - 1 - i : i;
        p->first[at] = (int16_t)a;
        p->last[at] = (int16_t)b;
    }
    p->redraw = true;
}

static uint16_t stop_colour(const ls_pwf_stop_t *s, int n, int at)
{
    int i = 0;
    while (i + 1 < n && at > s[i + 1].at) i++;
    const ls_pwf_stop_t *a = &s[i], *b = i + 1 < n ? &s[i + 1] : a;
    const int span = b->at - a->at;
    int k = span > 0 ? at - a->at : 0;
    if (k < 0) k = 0;
    if (k > span) k = span;
    const int r = span ? a->r + (b->r - a->r) * k / span : a->r;
    const int g = span ? a->g + (b->g - a->g) * k / span : a->g;
    const int bl = span ? a->b + (b->b - a->b) * k / span : a->b;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (bl >> 3));
}

void ls_pwf_colours(ls_pwf_t *p, const ls_pwf_stop_t *stops, int n, float lo, float hi)
{
    if (!p || !stops || n < 1) return;
    uint16_t lut[256];
    const float range = hi - lo > 1e-3f ? hi - lo : 1e-3f;
    for (int c = 0; c < 256; c++) {
        float t = (c - lo) / range;
        t = c == 0 || t < 0 ? 0 : t > 1 ? 1 : t;
        lut[c] = stop_colour(stops, n, (int)(t * 255.0f + 0.5f));
    }
    if (memcmp(lut, p->lut, sizeof(lut))) {
        memcpy(p->lut, lut, sizeof(lut));
        p->redraw = true;
    }
}

uint8_t *ls_pwf_push(ls_pwf_t *p)
{
    if (!p || !p->hist) return NULL;
    p->head = (p->head + 1) % p->lines;
    if (p->filled < p->lines) p->filled++;
    p->fresh++;
    return p->hist + (size_t)p->head * p->bins;
}

/* The line `age` lines old into picture column / row `at` (ring index). */
static void draw_line(ls_pwf_t *p, int at, int age)
{
    const uint8_t *line = age < p->filled
        ? p->hist + (size_t)((p->head - age + p->lines) % p->lines) * p->bins : NULL;
    const int f = p->freq_px;
    uint16_t *px;
    int step;
    if (p->flow == LS_PWF_LEFT) { px = p->img + at; step = p->w; }
    else                        { px = p->img + (size_t)at * p->w; step = 1; }
    for (int i = 0; i < f; i++) {
        int m = 0;
        if (line)
            for (int k = p->first[i]; k <= p->last[i]; k++)
                if (line[k] > m) m = line[k];
        px[(size_t)i * step] = p->lut[m];
    }
    /* The marker, dashed along time: two lit, two dark, by ring position so
       the dashes travel with the picture. */
    const int mk = p->mark1 - 1;
    if (line && mk >= 0 && !((at >> 1) & 1))
        for (int i = mk; i < mk + 2 && i < f; i++) px[(size_t)i * step] = p->mark_colour;
    p->stat_px += (uint32_t)f;
}

void ls_pwf_mark(ls_pwf_t *p, int px, uint16_t colour)
{
    if (!p) return;
    const int m = px >= 0 && px < p->freq_px ? px + 1 : 0;
    if (m != p->mark1 || (m && colour != p->mark_colour)) {
        p->mark1 = m;
        p->mark_colour = colour;
        p->redraw = true;
    }
}

void ls_pwf_compose(ls_pwf_t *p, int64_t now_us, int64_t min_us)
{
    if (!p || !p->img) return;
    p->stat_px = 0;
    const int lines = p->lines;
    if (p->redraw || p->fresh >= lines) {
        /* Everything again, the ring started where it stands. */
        if (p->flow == LS_PWF_LEFT) {
            p->pos = (p->pos + (p->fresh % lines)) % lines;
            for (int a = 0; a < lines; a++) draw_line(p, (p->pos - 1 - a + 2 * lines) % lines, a);
        } else {
            p->pos = (p->pos - (p->fresh % lines) + lines) % lines;
            for (int a = 0; a < lines; a++) draw_line(p, (p->pos + a) % lines, a);
        }
        p->moved = -1; /* the panel draws every cell */
    } else if (p->fresh && now_us - p->composed_us >= min_us) {
        for (int a = p->fresh - 1; a >= 0; a--) {
            if (p->flow == LS_PWF_LEFT) {
                draw_line(p, p->pos, a);
                p->pos = (p->pos + 1) % lines;
            } else {
                p->pos = (p->pos - 1 + lines) % lines;
                draw_line(p, p->pos, a);
            }
        }
        if (p->moved >= 0) p->moved += p->fresh;
    } else
        return;
    p->fresh = 0;
    p->redraw = false;
    p->composed_us = now_us;
    p->serial++;
}

void ls_pwf_show(ls_pwf_t *p, tui_surface *sf, tui_rect cells)
{
    if (!p || !p->img) return;
    if (p->flow == LS_PWF_LEFT)
        ls_tui_image_ring(cells, p->img, p->w, p->h, p->pos, 0, p->moved, 0, p->serial);
    else
        ls_tui_image_ring(cells, p->img, p->w, p->h, 0, p->pos, 0, p->moved, p->serial);
    p->moved = 0;
    for (int y = cells.y; y < cells.y + cells.h; y++)
        for (int x = cells.x; x < cells.x + cells.w; x++)
            tui_put_char(sf, cells, x, y, LS_TUI_IMAGE_CELL, 0);
}
