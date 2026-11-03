/* See ls_map_ink.h. */
#include "ls_map_ink.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"

#include "ls_tui.h"

#define INK_COLS 160
#define INK_ROWS 100

/* PSRAM: four bytes a cell for the largest map pane, touched only by the
   UI task's draw. */
EXT_RAM_BSS_ATTR static uint8_t s_bits[INK_COLS * INK_ROWS];
EXT_RAM_BSS_ATTR static uint8_t s_attr[INK_COLS * INK_ROWS];
EXT_RAM_BSS_ATTR static uint8_t s_prio[INK_COLS * INK_ROWS];
EXT_RAM_BSS_ATTR static uint8_t s_used[INK_COLS * INK_ROWS]; /* a dot that is not a pulse */
static tui_rect s_rect;
static bool     s_on, s_pulse;
/* Braille: two dots across and four down a cell instead of sextants. */
static bool     s_braille;
static int      s_dy = 3;
static uint8_t  s_tint;

void ls_ink_set_tint(uint8_t base) { s_tint = (uint8_t)(base & 7); }
void ls_ink_pulse(bool on) { s_pulse = on; }

static uint8_t tint_nibble(uint8_t n)
{
    const uint8_t c = n & 7, bright = n & 8;
    /* The ground stays the ground; the dim grey of furniture becomes the
       dim tint; every colour takes the tint at its own brightness. */
    if (c == 0) return bright ? s_tint : 0;
    return (uint8_t)(s_tint | bright);
}

uint8_t ls_ink_tinted(uint8_t attr)
{
    if (!s_tint) return attr;
    return TUI_ATTR(tint_nibble(TUI_ATTR_FG(attr)), tint_nibble(TUI_ATTR_BG(attr)));
}

bool ls_ink_begin(tui_rect cells)
{
    s_on = cells.w > 0 && cells.h > 0 && cells.w <= INK_COLS && cells.h <= INK_ROWS;
    s_rect = cells;
    s_braille = false; s_dy = 3;
    if (!s_on) return false;
    for (int r = 0; r < cells.h; r++) {
        memset(s_bits + r * INK_COLS, 0, (size_t)cells.w);
        memset(s_prio + r * INK_COLS, 0, (size_t)cells.w);
        memset(s_used + r * INK_COLS, 0, (size_t)cells.w);
    }
    return true;
}

bool ls_ink_begin_braille(tui_rect cells)
{
    const bool ok = ls_ink_begin(cells);
    s_braille = true; s_dy = 4;
    return ok;
}

void ls_ink_dot(int x, int y, uint8_t attr, uint8_t prio)
{
    if (!s_on || x < 0 || y < 0 || x >= s_rect.w * 2 || y >= s_rect.h * s_dy) return;
    const int col = x / 2, row = y / s_dy;
    const int i = row * INK_COLS + col;
    /* Unicode braille numbers its dots down the left column, then the
       right, and the bottom pair last. */
    static const uint8_t BRAILLE[4][2] = { { 0, 3 }, { 1, 4 }, { 2, 5 }, { 6, 7 } };
    s_bits[i] |= (uint8_t)(1u << (s_braille ? BRAILLE[y % 4][x % 2] : (y % 3) * 2 + (x % 2)));
    if (!s_pulse) s_used[i] = 1;
    if (prio >= s_prio[i]) { s_prio[i] = prio ? prio : 1; s_attr[i] = attr; }
}

static bool on_dash(int dash, int k)
{
    if (!dash) return true;
    if (dash > 0) return (k / dash) % 2 == 0;
    return k % (-dash) == 0;
}

/* One contiguous, actually rasterized run. Preserve the original Bresenham
   error while marking ownership, so dashed/clipped lines hide exactly their
   own dots. The recorded path has the same endpoints as that board stroke. */
static void trace_run(int ax,int ay,int bx,int by,int error,int count,
                      int dx,int dy,int sx,int sy,uint8_t attr,uint8_t prio)
{
    tui_stroke_hook_fn hook=__atomic_load_n(&tui_stroke_hook,__ATOMIC_ACQUIRE);
    if(!hook || !count) return;
    uint8_t ink=ls_ink_tinted(attr);
    static const uint8_t bits[4][2]={{1,8},{2,16},{4,32},{64,128}};
    bool braille=s_braille || prio<=3;
    int rows=braille?4:3,ty=ay/s_dy*rows+ay%s_dy,uy=by/s_dy*rows+by%s_dy;
    if(!hook(1,NULL,s_rect,2,rows,ax,ty,bx,uy,ink)) return;
    for(int i=0;i<count;i++) {
        int mask=braille?bits[ay%s_dy][ax%2]:1u<<((ay%3)*2+ax%2);
        ty=ay/s_dy*rows+ay%s_dy;
        hook(2,NULL,s_rect,2,rows,ax,ty,mask,braille?0x2800:0xC0,ink);
        int e2=2*error;
        if(e2>=dy) {error+=dy;ax+=sx;}
        if(e2<=dx) {error+=dx;ay+=sy;}
    }
}

void ls_ink_line(int x0, int y0, int x1, int y1, uint8_t attr, uint8_t prio, int dash)
{
    if (!s_on) return;
    /* A line wholly off one side of the canvas draws nothing; long ones
       from far off the pane are clipped to a sane length first. */
    const int w = s_rect.w * 2, h = s_rect.h * s_dy;
    if ((x0 < 0 && x1 < 0) || (y0 < 0 && y1 < 0) || (x0 >= w && x1 >= w) || (y0 >= h && y1 >= h)) return;
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    if (dx > 4 * w || -dy > 4 * h) return;
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, k = 0, ax=0,ay=0,bx=0,by=0,run_error=0,run_count=0;
    bool trace=__atomic_load_n(&tui_stroke_hook,__ATOMIC_ACQUIRE)!=NULL;
    for (;;) {
        bool drawn=on_dash(dash,k) && x0>=0 && y0>=0 && x0<w && y0<h;
        if (drawn) {
            ls_ink_dot(x0, y0, attr, prio);
            if(trace) {
                if(!run_count) {ax=x0;ay=y0;run_error=err;}
                bx=x0;by=y0;run_count++;
            }
        } else if(run_count) {
            trace_run(ax,ay,bx,by,run_error,run_count,dx,dy,sx,sy,attr,prio);run_count=0;
        }
        k++;
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    trace_run(ax,ay,bx,by,run_error,run_count,dx,dy,sx,sy,attr,prio);
}

void ls_ink_ellipse(int cx, int cy, int rx, int ry, uint8_t attr, uint8_t prio, int dash)
{
    if (!s_on || rx < 1 || ry < 1) return;
    /* Enough steps that neighbouring dots touch, capped for a ring far
       larger than the pane. */
    int steps = (int)(2.0 * M_PI * (rx > ry ? rx : ry));
    if (steps < 12) steps = 12;
    if (steps > 1200) steps = 1200;
    /* A unit vector turned a step at a time: two multiplies and adds per
       dot instead of a sine and a cosine. Single precision, which is all
       this part has an FPU for. */
    const float c = cosf(6.2831853f / steps), s = sinf(6.2831853f / steps);
    float ux = 0.0f, uy = 1.0f;
    const int w = s_rect.w * 2, h = s_rect.h * s_dy;
    int px = 0x7FFFFFFF, py = 0, k = 0;
    bool previous=false;
    tui_stroke_hook_fn hook=__atomic_load_n(&tui_stroke_hook,__ATOMIC_ACQUIRE);
    for (int i = 0; i < steps; i++) {
        const int x = cx + (int)lrintf(rx * ux);
        const int y = cy - (int)lrintf(ry * uy);
        const float nx = ux * c + uy * s;
        uy = uy * c - ux * s;
        ux = nx;
        if (x == px && y == py) continue;
        bool drawn=on_dash(dash,k) && x>=0 && y>=0 && x<w && y<h;
        if(drawn) {
            ls_ink_dot(x,y,attr,prio);
            if(hook) {
                bool braille=s_braille || prio<=3;int rows=braille?4:3;
                int ax=previous?px:x,ay=previous?py:y;
                int a=ay/s_dy*rows+ay%s_dy,b=y/s_dy*rows+y%s_dy;
                uint8_t ink=ls_ink_tinted(attr);
                if(hook(1,NULL,s_rect,2,rows,ax,a,x,b,ink)) {
                    static const uint8_t bits[4][2]={{1,8},{2,16},{4,32},{64,128}};
                    int mask=braille?bits[y%s_dy][x%2]:1u<<((y%3)*2+x%2);
                    hook(2,NULL,s_rect,2,rows,x,b,mask,braille?0x2800:0xC0,ink);
                    mask=braille?bits[ay%s_dy][ax%2]:1u<<((ay%3)*2+ax%2);
                    hook(2,NULL,s_rect,2,rows,ax,a,mask,braille?0x2800:0xC0,ink);
                }
            }
        }
        previous=drawn;px=x;py=y;
        k++;
    }
}

bool ls_ink_cell_used(int col, int row)
{
    if (!s_on || col < 0 || row < 0 || col >= s_rect.w || row >= s_rect.h) return false;
    return s_used[row * INK_COLS + col] != 0;
}

int ls_ink_cell_prio(int col, int row)
{
    if (!s_on || col < 0 || row < 0 || col >= s_rect.w || row >= s_rect.h) return 0;
    return s_bits[row * INK_COLS + col] ? s_prio[row * INK_COLS + col] : 0;
}

void ls_ink_flush(tui_surface *sf, tui_rect skip)
{
    if (!s_on) return;
    for (int r = 0; r < s_rect.h; r++) {
        const int y = s_rect.y + r;
        for (int c = 0; c < s_rect.w; c++) {
            const uint8_t b = s_bits[r * INK_COLS + c];
            if (!b) continue;
            const int x = s_rect.x + c;
            if (skip.h > 0 && x >= skip.x && x < skip.x + skip.w &&
                y >= skip.y && y < skip.y + skip.h) continue;
            uint16_t glyph=LS_TUI_SEXT(b);
            if (s_braille) glyph = (uint16_t)(0x2800 | b);
            else if(s_prio[r*INK_COLS+c]<=3) {
                static const uint8_t dots[6]={1,8,2,16,4,32};
                glyph=0x2800;for(int k=0;k<6;k++) if(b&(1u<<k)) glyph|=dots[k];
            }
            ls_tui_put_glass(sf, s_rect, x, y, glyph, ls_ink_tinted(s_attr[r * INK_COLS + c]));
        }
    }
}

void ls_ink_text(tui_surface *sf, tui_rect clip, int x, int y, const char *s, int n, uint8_t attr)
{
    attr = ls_ink_tinted(attr);
    /* `n` is bytes of UTF-8; each character takes one cell, folded to ASCII. */
    const char *p = s, *end = s + n;
    for (int i = 0; p < end && *p; i++) {
        /* A space is see-through: the cell is left to the picture, which
           also clears anything the canvas put there. */
        ls_tui_put_glass(sf, clip, x + i, y, tui_utf8_fold(&p), attr);
    }
}
