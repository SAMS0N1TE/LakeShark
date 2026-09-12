/* See ls_anim.h. Bounded, skippable, decorative. */
#include "ls_anim.h"

#include <stdio.h>
#include <string.h>

#include "ls_icons.h"

#define FRAMES 12

static int      s_frame = -1;
static int      s_icon;
static uint8_t  s_hue;
static char     s_name[16];

/* A quarter-period integer sine, 0..64, so the wave openings need no float
   and no table lookup beyond this. */
static const int8_t SIN64[17] = {
    0, 12, 24, 35, 45, 53, 59, 63, 64, 63, 59, 53, 45, 35, 24, 12, 0
};
static int isin(int deg)          /* deg 0..359 -> -64..64 */
{
    int q = ((deg % 360) + 360) % 360;
    if (q < 180) return SIN64[(q * 16) / 180];
    return (int8_t)-SIN64[((q - 180) * 16) / 180];
}

void ls_anim_start(int icon, const char *name, uint8_t hue)
{
    s_frame = 0;
    s_icon  = icon;
    s_hue   = hue ? hue : (uint8_t)TUI_CYAN;
    snprintf(s_name, sizeof(s_name), "%s", name ? name : "");
}

bool ls_anim_active(void) { return s_frame >= 0; }
/* A skipped opening hands over exactly like a finished one, so it
   owes the screen the same full repaint. Only when there was something to
   skip: cancelling nothing must not cost a repaint. */
void ls_anim_cancel(void)
{
    if (s_frame < 0) return;
    s_frame = -1;
    ls_tui_invalidate();
}

/* Rings expanding from a point: the tower, and the default for anything
   without an opening of its own. */
static void rings(tui_surface *sf, tui_rect a, int cx, int cy, int f)
{
    for (int ring = 0; ring < 3; ring++) {
        const int r = f * 2 - ring * 4;
        if (r < 1) continue;
        const uint8_t c = TUI_ATTR(ring == 0 ? (s_hue | TUI_BRIGHT) : s_hue,
                                   TUI_BLACK);
        for (int deg = 0; deg < 360; deg += 12) {
            const int x = cx + (isin(deg + 90) * r) / 64;
            const int y = cy + (isin(deg) * r) / 128;   /* cells are tall   */
            if (x < a.x || x >= a.x + a.w || y < a.y || y >= a.y + a.h) continue;
            tui_put_char(sf, a, x, y, LS_TUI_QUAD(1, 1, 0, 0), c);
        }
    }
}

/* A travelling sine: FM and the analogue family. */
static void wave(tui_surface *sf, tui_rect a, int cy, int f)
{
    for (int x = 0; x < a.w; x++) {
        const int amp = 3 + (f < 6 ? f : 11 - f);
        const int y = cy + (isin(x * 12 + f * 40) * amp) / 64;
        if (y < a.y || y >= a.y + a.h) continue;
        const uint8_t c = TUI_ATTR((x + f) % 3 ? (s_hue | TUI_BRIGHT) : s_hue,
                                   TUI_BLACK);
        tui_put_char(sf, a, a.x + x, y, LS_TUI_BLOCK_FULL, c);
    }
}

/* A sweep line crossing the pane: ADS-B, and anything that scans. */
static void sweep(tui_surface *sf, tui_rect a, int f)
{
    const int x = a.x + (a.w - 1) * f / (FRAMES - 1);
    for (int y = 0; y < a.h; y++) {
        const int fade = (y * 3 / (a.h ? a.h : 1));
        tui_put_char(sf, a, x, a.y + y, LS_TUI_BLOCK_LEFT,
                     TUI_ATTR(fade ? s_hue : (s_hue | TUI_BRIGHT), TUI_BLACK));
        if (x > a.x)
            tui_put_char(sf, a, x - 1, a.y + y, LS_TUI_SHADE_25,
                         TUI_ATTR(s_hue, TUI_BLACK));
    }
}

/* Bands falling from the top: the waterfall's own opening, and REC's. */
static void cascade(tui_surface *sf, tui_rect a, int f)
{
    for (int y = 0; y < a.h; y++) {
        if (y > f * a.h / FRAMES) break;
        for (int x = 0; x < a.w; x++) {
            /* Cheap deterministic hash - it only has to look like noise. */
            const unsigned h = (unsigned)(x * 2654435761u + y * 40503u + (unsigned)f);
            const int v = (int)((h >> 13) & 0x0F);
            if (v < 6) continue;
            const char g = v > 13 ? LS_TUI_SHADE_FULL
                         : v > 10 ? LS_TUI_SHADE_75 : LS_TUI_SHADE_50;
            tui_put_char(sf, a, a.x + x, a.y + y, g,
                         TUI_ATTR(v > 12 ? (s_hue | TUI_BRIGHT) : s_hue,
                                  TUI_BLACK));
        }
    }
}

/* The mesh opening: a constellation assembling itself. */

#define MESH_NODES 5

static void mesh_node_pos(tui_rect a, int cx, int cy, int i, int *x, int *y)
{
    const int deg = 90 + i * (360 / MESH_NODES);
    const int rx = (a.w < 30 ? a.w : 30) / 3;
    const int ry = (a.h < 14 ? a.h : 14) / 3;
    *x = cx + (isin(deg + 90) * rx) / 64;
    *y = cy + (isin(deg) * ry) / 64;
}

static void constellation(tui_surface *sf, tui_rect a, int cx, int cy, int f)
{
    const uint8_t link_c = TUI_ATTR(s_hue, TUI_BLACK);
    const uint8_t node_c = TUI_ATTR(s_hue | TUI_BRIGHT, TUI_BLACK);
    const uint8_t hot_c  = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);

    for (int i = 0; i < MESH_NODES; i++) {
        int nx, ny;
        mesh_node_pos(a, cx, cy, i, &nx, &ny);

        /* The link, drawn from the centre outward as the frames advance.
           Integer interpolation along the segment: no line routine here and
           no reason to add one for five short spokes. */
        const int grow = f - 3 - i;            /* each link starts a frame later */
        if (grow > 0) {
            const int steps = 8;
            const int upto = grow * steps / 5 > steps ? steps : grow * steps / 5;
            for (int k = 1; k <= upto; k++) {
                const int lx = cx + (nx - cx) * k / steps;
                const int ly = cy + (ny - cy) * k / steps;
                if (lx < a.x || lx >= a.x + a.w || ly < a.y || ly >= a.y + a.h) continue;
                tui_put_char(sf, a, lx, ly, '.', link_c);
            }
            /* The pulse: one bright cell running out along the link. */
            const int pulse = f - 7 - i / 2;
            if (pulse >= 0 && pulse <= steps) {
                const int px = cx + (nx - cx) * pulse / steps;
                const int py = cy + (ny - cy) * pulse / steps;
                if (px >= a.x && px < a.x + a.w && py >= a.y && py < a.y + a.h)
                    tui_put_char(sf, a, px, py, LS_TUI_BLOCK_FULL, hot_c);
            }
        }

        /* The node itself, outermost first so the ring fills inward. */
        if (f >= i && nx >= a.x && nx < a.x + a.w && ny >= a.y && ny < a.y + a.h)
            tui_put_char(sf, a, nx, ny,
                         f >= 7 + i / 2 ? LS_TUI_BLOCK_FULL : LS_TUI_QUAD(1, 1, 0, 0),
                         f >= 7 + i / 2 ? node_c : link_c);
    }
}

bool ls_anim_draw(tui_surface *sf, tui_rect area)
{
    if (s_frame < 0) return false;
    if (!sf || area.w < 12 || area.h < 6) { s_frame = -1; return false; }

    const int f = s_frame;
    const int cx = area.x + area.w / 2;
    const int cy = area.y + area.h / 2;

    tui_fill(sf, area, ' ', TUI_ATTR(TUI_WHITE, TUI_BLACK));

    switch (s_icon) {
    case LS_ICON_WAVE:
    case LS_ICON_PAGER:  wave(sf, area, cy, f);    break;
    case LS_ICON_PLANE:
    case LS_ICON_MAP:    sweep(sf, area, f);       break;
    case LS_ICON_FALLS:
    case LS_ICON_RECORD: cascade(sf, area, f);     break;
    case LS_ICON_MESH:   constellation(sf, area, cx, cy, f); break;
    default:             rings(sf, area, cx, cy, f); break;
    }

    if (f >= 4 && area.h > LS_ICON_ROWS + 2)
        ls_icon_draw(sf, area, cx - LS_ICON_COLS / 2, cy - LS_ICON_ROWS / 2,
                     s_icon, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));

    /* The name wipes in one letter at a time. */
    if (s_name[0]) {
        const int n = (int)strlen(s_name);
        const int show = f * n / (FRAMES - 1) + 1;
        char buf[16];
        snprintf(buf, sizeof(buf), "%.*s", show > n ? n : show, s_name);
        const int y = cy + LS_ICON_ROWS / 2 + 1;
        if (y < area.y + area.h)
            tui_put_str(sf, area, cx - n / 2, y, buf,
                        TUI_ATTR(s_hue | TUI_BRIGHT, TUI_BLACK));
    }

    /* The handover back to the screen is a full repaint, not a diff. */

    if (++s_frame >= FRAMES) {
        s_frame = -1;
        ls_tui_invalidate();
    }
    return true;
}
