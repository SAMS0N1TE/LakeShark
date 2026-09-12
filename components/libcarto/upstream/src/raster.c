#include "carto/raster.h"

uint16_t carto_rgb565(carto_rgb c) {
    return (uint16_t)(((c.r & 0xF8) << 8) | ((c.g & 0xFC) << 3) | (c.b >> 3));
}

void carto_put_px(carto_framebuffer *fb, int x, int y, carto_rgb c) {
    if (!fb || x < 0 || y < 0 || x >= fb->width || y >= fb->height) return;
    uint8_t *row = fb->pixels + (size_t)y * fb->stride;
    switch (fb->format) {
        case CARTO_FMT_RGB565:
            ((uint16_t *)row)[x] = carto_rgb565(c);
            return;
        case CARTO_FMT_RGBA8888: {
            uint8_t *p = row + (size_t)x * 4;
            p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = 0xFF;
            return;
        }
        case CARTO_FMT_INDEXED8:
            row[x] = (uint8_t)((c.r * 30 + c.g * 59 + c.b * 11) / 100);
            return;
        case CARTO_FMT_MONO1: {
            int lum = (c.r * 30 + c.g * 59 + c.b * 11) / 100;
            uint8_t mask = (uint8_t)(0x80 >> (x & 7));
            if (lum >= 128) row[x >> 3] |= mask;
            else            row[x >> 3] &= (uint8_t)~mask;
            return;
        }
    }
}

void carto_fill_rect(carto_framebuffer *fb, int x, int y, int w, int h, carto_rgb c) {
    if (!fb) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > fb->width)  x1 = fb->width;
    int y1 = y + h; if (y1 > fb->height) y1 = fb->height;
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx)
            carto_put_px(fb, xx, yy, c);
}

void carto_draw_line(carto_framebuffer *fb, int x0, int y0, int x1, int y1,
                     int width, carto_rgb c) {
    if (!fb) return;
    if (width < 1) width = 1;
    int half = width / 2;

    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        if (width == 1) carto_put_px(fb, x0, y0, c);
        else            carto_fill_rect(fb, x0 - half, y0 - half, width, width, c);
        if (x0 == x1 && y0 == y1) break;
        /*LS-1085  Multiply, do not shift. `err` starts at dx - dy and is
           negative for any line steeper than 45 degrees, which is half of
           them - and left-shifting a negative int is undefined behaviour in
           C, not merely implementation-defined.

           It has always produced the right answer on the compilers this has
           been built with, which is exactly why it survived: nothing fails
           until an optimiser decides a negative shift cannot happen and
           reasons from there. Found by `bench/verify.ps1 -Sanitize`, which
           runs UBSan in trap mode on this toolchain - the whole test binary
           died with an illegal instruction on this line, in the middle of
           drawing a water line.

           `* 2` is the same arithmetic with defined behaviour and the same
           code generated. */
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void carto_polyline(carto_framebuffer *fb, const carto_ipt *pts, int n, int width, carto_rgb c) {
    if (!fb || !pts || n < 2) return;
    for (int i = 0; i + 1 < n; ++i)
        carto_draw_line(fb, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, width, c);
}

/* LS DEVIATION 2: one horizontal run, with the format decided once.

   carto_put_px switches on the pixel format and bounds-checks for every
   pixel, and the polygon fill called it per pixel across a whole scanline.
   That is the renderer's hot loop: a filled landuse polygon on a 720 wide
   map is hundreds of thousands of switches a frame. RGB565 is the only
   format this board has, so it gets a straight run of stores and everything
   else falls back to the general path. */
static void carto_fill_span(carto_framebuffer *fb, int xa, int xb, int y,
                            carto_rgb c) {
    if (!fb || y < 0 || y >= fb->height) return;
    if (xa < 0) xa = 0;
    if (xb >= fb->width) xb = fb->width - 1;
    if (xa > xb) return;

    if (fb->format == CARTO_FMT_RGB565) {
        uint16_t *row = (uint16_t *)(fb->pixels + (size_t)y * fb->stride);
        const uint16_t v = carto_rgb565(c);
        for (int x = xa; x <= xb; ++x) row[x] = v;
        return;
    }
    for (int x = xa; x <= xb; ++x) carto_put_px(fb, x, y, c);
}

void carto_fill_polygon(carto_framebuffer *fb, const carto_ipt *pts, int n, carto_rgb c) {
    if (!fb || !pts || n < 3) return;

    int miny = pts[0].y, maxy = pts[0].y;
    for (int i = 1; i < n; ++i) {
        if (pts[i].y < miny) miny = pts[i].y;
        if (pts[i].y > maxy) maxy = pts[i].y;
    }
    if (miny < 0) miny = 0;
    if (maxy >= fb->height) maxy = fb->height - 1;

    /* LS DEVIATION 1: this was `int xints[2048]`, an 8 KB automatic array.
       FreeRTOS task stacks on this board are 4 to 8 KB, so the first polygon
       with a scanline to fill would have run off the end of the stack. Static
       rather than arena because the fill has no arena parameter and adding
       one would change a public signature; the renderer runs on one task, and
       that is written down in the component's DEVIATIONS.md. */
    static int xints[CARTO_MAX_CROSSINGS];
    /*LS-1073  Float, not double: a divide per crossing, in software.

       This is the inner loop of every filled polygon - once per edge per
       scanline - and the divide in it was double precision on a chip whose
       FPU only does single. Water and landuse are most of what a map of New
       Hampshire draws, so this ran millions of times a pan.

       The inputs are integer pixel coordinates, exact in a float well past
       any value a frame can hold, and `t` is a ratio in nought to one. The
       result is truncated to an int. There is nothing here that needed
       fifteen significant digits. */
    for (int y = miny; y <= maxy; ++y) {
        float yc = (float)y + 0.5f;
        int cnt = 0;
        for (int i = 0; i < n; ++i) {
            int j = (i + 1) % n;
            float y0 = (float)pts[i].y, y1 = (float)pts[j].y;
            float x0 = (float)pts[i].x, x1 = (float)pts[j].x;
            if ((y0 <= yc && y1 > yc) || (y1 <= yc && y0 > yc)) {
                float t = (yc - y0) / (y1 - y0);
                int xi = (int)(x0 + t * (x1 - x0));
                if (cnt < CARTO_MAX_CROSSINGS) xints[cnt++] = xi;
            }
        }
        for (int a = 1; a < cnt; ++a) {
            int key = xints[a], b = a - 1;
            while (b >= 0 && xints[b] > key) { xints[b + 1] = xints[b]; --b; }
            xints[b + 1] = key;
        }
        for (int k = 0; k + 1 < cnt; k += 2)
            carto_fill_span(fb, xints[k], xints[k + 1], y, c);
    }
}

static int carto_edge(int ax, int ay, int bx, int by, int px, int py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

void carto_fill_triangle(carto_framebuffer *fb, int x0, int y0, int x1, int y1,
                         int x2, int y2, carto_rgb c) {
    if (!fb) return;
    int minx = x0; if (x1 < minx) minx = x1; if (x2 < minx) minx = x2;
    int miny = y0; if (y1 < miny) miny = y1; if (y2 < miny) miny = y2;
    int maxx = x0; if (x1 > maxx) maxx = x1; if (x2 > maxx) maxx = x2;
    int maxy = y0; if (y1 > maxy) maxy = y1; if (y2 > maxy) maxy = y2;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= fb->width)  maxx = fb->width - 1;
    if (maxy >= fb->height) maxy = fb->height - 1;

    int area = carto_edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) return;

    for (int py = miny; py <= maxy; ++py) {
        for (int px = minx; px <= maxx; ++px) {
            int w0 = carto_edge(x1, y1, x2, y2, px, py);
            int w1 = carto_edge(x2, y2, x0, y0, px, py);
            int w2 = carto_edge(x0, y0, x1, y1, px, py);
            int inside = area > 0
                ? (w0 >= 0 && w1 >= 0 && w2 >= 0)
                : (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (inside) carto_put_px(fb, px, py, c);
        }
    }
}
