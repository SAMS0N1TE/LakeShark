/* MAP: vector tiles off the card, drawn in pixels the grid lends it. */

#include "../../ls_tui_screen.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "../../ls_map.h"
#include "../../ls_motion.h"
#include "../../ls_picker.h"
#include "../../ls_quick.h"
#include "../../ls_tui.h"
#include "../../ls_tui_ui.h"
#include "../../ls_stroke.h"
/* The nodes drawn on the map are the mesh's own peers. */
#include "esp_attr.h"
#include "ls_mesh.h"
/* And the aircraft are ADS-B's own table - the one its list and radar
   read, not a copy of it. */
#include "apps/adsb/adsb_state.h"

/* Where a map archive lives. The loader takes the first one it finds rather
   than a fixed name: somebody who has cut their own county has no reason to
   have called it what this file expects. */
#define MAP_DIR "/sdcard/maps"

static bool     s_opened;
static tui_rect s_map_cells;      /* the rectangle the grid lent us */
static tui_rect s_quick_rect;
static tui_rect s_pad_rect;
static char     s_note[64];

/* Pan by a third of the frame, which is far enough to be worth the tap and
   short enough to keep your place. */
#define PAN_FRACTION 3

static const ls_quick_t QUICK[] = {
    { .label = "ZOOM+", .kind = LS_QUICK_ACTION, .action = "map.zoom",
      .choices = (const char *const[]){ "1" }, .nchoices = 1, .key = '=' },
    { .label = "ZOOM-", .kind = LS_QUICK_ACTION, .action = "map.zoom",
      .choices = (const char *const[]){ "-1" }, .nchoices = 1, .key = '-' },
    { .label = "HERE", .kind = LS_QUICK_ACTION, .action = "map.here",
      .key = 'h' },
    { .label = "VIEW", .kind = LS_QUICK_ACTION, .action = "map.view",
      .key = 'v' },
    /* FIND has RELOAD's slot, and RELOAD is not replaced by another row. */

    { .label = "FIND", .kind = LS_QUICK_ACTION, .action = "map.find",
      .key = 'f' },
};
#define N_QUICK ((int)(sizeof(QUICK) / sizeof(QUICK[0])))

/* ---------------------------------------------------------------- entry -- */

static bool find_archive(char *out, size_t cap)
{
    DIR *d = opendir(MAP_DIR);
    if (!d) return false;

    bool found = false;
    const struct dirent *e;
    while (!found && (e = readdir(d)) != NULL) {
        const size_t n = strlen(e->d_name);
        if (n < 9) continue;
        if (strcasecmp(e->d_name + n - 8, ".pmtiles") != 0) continue;
        snprintf(out, cap, "%s/%s", MAP_DIR, e->d_name);
        found = true;
    }
    closedir(d);
    return found;
}

/* Defined with the action it registers, further down; declared here because
   this is where it is called. */
static void register_view_action(void);
static void rescan(void);

/* Likewise: the cell cache is defined with the pass that fills it, and
   leave() is the one place that gives it back. */
static void cells_free(void);

static bool s_have_archive;

static void enter(void)
{
    register_view_action();
    /* The archive is opened once and kept: pmtiles_open reads and parses the
       root directory, and doing that on every entry would stall the first
       frame of a screen somebody just tapped into. */
    if (!s_opened) {
        s_opened = true;
        char path[128];
        if (find_archive(path, sizeof(path)))
            s_have_archive = ls_map_open(path);
        return;
    }
    /* Nothing open means look again, every time this screen is
       entered. The card is usually put in after boot, and a screen that only
       ever looked once needed a reboot to notice - which reads as the card
       not working. This is what RELOAD was for; doing it here costs a
       directory listing on a screen that has no map anyway, and it happens
       whether or not anybody thinks to press a button. */
    if (!s_have_archive) rescan();
}

/* Look again. The card is usually inserted after boot, and a screen that
   only ever looked once would need a reboot to notice - which reads as the
   card not working. */
static void rescan(void)
{
    char path[128];
    if (find_archive(path, sizeof(path))) s_have_archive = ls_map_open(path);
}

static void leave(void)
{
    /* Hand the pixels back, or the map stays on the glass under the next
       screen: the cell renderer only pushes cells that changed, and none of
       the cells under a borrowed rectangle did. */
    ls_tui_reserve(tui_rect_make(0, 0, 0, 0));
    s_map_cells = tui_rect_make(0, -1, 0, 0);
    s_pad_rect = tui_rect_make(0, -1, 0, 0);
    cells_free();
}

/* --------------------------------------------------------------- draw --- */

static void draw_placeholder(tui_surface *sf, tui_rect area, const char *why)
{
    const uint8_t dim = LS_ATTR_DIM;
    ls_panel_box(sf, area, "MAP", TUI_CYAN);
    tui_put_str(sf, area, area.x + 2, area.y + 2, why, dim);
    tui_put_str(sf, area, area.x + 2, area.y + 4,
                "put a .pmtiles archive at " MAP_DIR, dim);
}

/* The map, drawn as characters. */

/* Sub-pixels per cell, and why they are not the same in both axes. */

#define SUB_X 3

static int sub_y(void)
{
    int cw = 10, ch = 17;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    if (cw < 1) cw = 10;
    int sy = (SUB_X * ch + cw / 2) / cw;
    if (sy < 3) sy = 3;
    if (sy > 8) sy = 8;
    return sy;
}

/* The palette a class is drawn in. Water reads as water, parks as green,
   roads as the brightest thing on the screen because they are what the eye
   follows, and buildings as the dim grey that everything structural in this
   interface uses. */
static uint8_t ink_attr(ls_map_ink_t k)
{
    switch (k) {
    case LS_MAP_WATER:    return TUI_ATTR(TUI_BLUE | TUI_BRIGHT, TUI_BLACK);
    case LS_MAP_PARK:     return TUI_ATTR(TUI_GREEN, TUI_BLACK);
    case LS_MAP_BUILDING: return TUI_ATTR(TUI_BLACK | TUI_BRIGHT, TUI_BLACK);
    case LS_MAP_ROAD:     return TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    default:              return TUI_ATTR(TUI_BLACK, TUI_BLACK);
    }
}

/* Two ways to draw it, and mono is not just colour turned off. */

typedef enum { MAP_VIEW_COLOUR = 0, MAP_VIEW_MONO, MAP_VIEW__COUNT } map_view_t;
static map_view_t s_view;

static const char *const VIEW_NAME[MAP_VIEW__COUNT] = { "colour", "lines" };

static ls_act_status_t a_map_view(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    s_view = (map_view_t)((s_view + 1) % MAP_VIEW__COUNT);
    out->kind = LS_VAL_TEXT;
    out->s = VIEW_NAME[s_view];
    return LS_ACT_OK;
}

static ls_act_status_t a_map_find(const ls_args_t *in, ls_val_t *out);

static void register_view_action(void)
{
    static bool done;
    if (done) return;
    done = ls_action_register("map.view", "", LS_CAP_UI, a_map_view,
                              "filled colour, or one-bit line art");
    ls_action_register("map.find", "", LS_CAP_UI, a_map_find,
                       "the named places in view, nearest first, to go to");
}

/* The cells the picture became, kept until the picture changes. */

#define CELL_COLS_MAX 160

static char    *s_cc_glyph;        /* 0 means "nothing here, leave it black" */
static uint8_t *s_cc_attr;
static int      s_cc_w, s_cc_h;
static uint32_t s_cc_serial;
static int      s_cc_view = -1;
static bool     s_cc_valid;
static uint32_t s_cells_us;        /* what the last rebuild cost */

static uint8_t s_acc[CELL_COLS_MAX * 4];
static uint8_t s_ink[CELL_COLS_MAX * 4];
static uint8_t s_prev[CELL_COLS_MAX * SUB_X];

static uint8_t s_cur[CELL_COLS_MAX * SUB_X];

/* The one ink mono draws in. White because the point of a single-colour
   mode is contrast; the Flipper's orange is its screen, not its design. */
#define MONO_ATTR TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK)

static bool cells_fit(int w, int h)
{
    if (w <= 0 || h <= 0) return false;
    if (s_cc_glyph && s_cc_w == w && s_cc_h == h) return true;

    /* PSRAM, not the internal heap. */

    heap_caps_free(s_cc_glyph);
    heap_caps_free(s_cc_attr);
    const size_t need = (size_t)w * (size_t)h;
    s_cc_glyph = (char *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_cc_attr = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cc_glyph || !s_cc_attr) {
        heap_caps_free(s_cc_glyph);  s_cc_glyph = NULL;
        heap_caps_free(s_cc_attr);   s_cc_attr = NULL;
        s_cc_w = s_cc_h = 0;
        return false;
    }
    s_cc_w = w;
    s_cc_h = h;
    s_cc_valid = false;
    return true;
}

static void cells_free(void)
{
    heap_caps_free(s_cc_glyph);  s_cc_glyph = NULL;
    heap_caps_free(s_cc_attr);   s_cc_attr = NULL;
    s_cc_w = s_cc_h = 0;
    s_cc_valid = false;
}

/* A finished row of accumulators becomes a finished row of cells. */
static void flush_row(int cy, int w, bool mono)
{
    if (cy < 0 || cy >= s_cc_h) return;
    char    *g  = s_cc_glyph + (size_t)cy * s_cc_w;
    uint8_t *at = s_cc_attr  + (size_t)cy * s_cc_w;

    for (int cx = 0; cx < w; cx++) {
        if (mono) {
            /* A STROKE, not a block, and this is the line the two previous attempts never reached. */

            const uint8_t *b = &s_ink[cx * 4];
            const char st = ls_stroke_glyph(b[0], b[1], b[2], b[3]);
            if (!st) { g[cx] = 0; continue; }
            g[cx]  = st;
            at[cx] = MONO_ATTR;
            continue;
        }

        const uint8_t *q = &s_acc[cx * 4];
        uint8_t top = q[0];
        for (int i = 1; i < 4; i++) if (q[i] > top) top = q[i];
        if (top == LS_MAP_GROUND) { g[cx] = 0; continue; }

        /* The glyph says WHICH quarters had that class in them. */
        g[cx]  = LS_TUI_QUAD(q[0] == top, q[1] == top,
                             q[2] == top, q[3] == top);
        at[cx] = ink_attr((ls_map_ink_t)top);
    }
}

/* The framebuffer, walked once, in the order it is stored. */

static void build_cells(tui_rect a, const uint16_t *px, int pw, int ph)
{
    const bool mono = (s_view == MAP_VIEW_MONO);
    const int  sy_n = sub_y();
    const int  qx = (SUB_X + 1) / 2;      /* first sub-column of the right half */
    const int  qy = (sy_n + 1) / 2;

    int w = a.w;
    if (w > CELL_COLS_MAX) w = CELL_COLS_MAX;

    int xmax = w * SUB_X;
    if (xmax > pw) xmax = pw;

    memset(s_cc_glyph, 0, (size_t)s_cc_w * (size_t)s_cc_h);
    memset(s_acc, 0, sizeof s_acc);
    memset(s_ink, 0, sizeof s_ink);
    memset(s_prev, 0, sizeof s_prev);

    int cy = 0;
    for (int y = 0; y < ph; y++) {
        const int ncy = y / sy_n;
        if (ncy != cy) {
            flush_row(cy, w, mono);
            memset(s_acc, 0, sizeof s_acc);
            memset(s_ink, 0, sizeof s_ink);
            cy = ncy;
            if (cy >= a.h) break;
        }
        const int qh = ((y % sy_n) >= qy) ? 2 : 0;
        const uint16_t *row = px + (size_t)y * (size_t)pw;

        /* One call for the row. See ls_map_classify_row. */
        ls_map_classify_row(row, s_cur, xmax);

        uint8_t left = LS_MAP_GROUND;
        for (int x = 0; x < xmax; x++) {
            const uint8_t k = s_cur[x];
            const int cx = x / SUB_X;
            const int q  = qh + (((x % SUB_X) >= qx) ? 1 : 0);

            uint8_t *slot = &s_acc[cx * 4 + q];
            if (k > *slot) *slot = k;

            if (mono) {
                /* Mono is a LINE DRAWING, which is not colour turned off and is not shading either. */

                const bool edge = (x > 0 && k != left) ||
                                  (y > 0 && k != s_prev[x]);
                if (k == LS_MAP_ROAD || edge) s_ink[cx * 4 + q] = 1;
            }

            left = k;
        }
        /* The row just walked becomes the row above. A copy, rather
           than swapping two pointers, because both are fixed arrays and the
           copy is a few hundred bytes against the thousands of comparisons
           it feeds. */
        memcpy(s_prev, s_cur, (size_t)xmax);
    }
    flush_row(cy, w, mono);
}

static void draw_cells(tui_surface *sf, tui_rect a,
                       const uint16_t *px, int pw, int ph)
{
    if (a.w <= 0 || a.h <= 0 || !px) return;
    if (!cells_fit(a.w, a.h)) {

        ls_panel_notice(sf, a, "MAP", "no memory to lay the map out in cells",
                        "close an app and come back");
        return;
    }

    const uint32_t serial = ls_map_render_serial();
    if (!s_cc_valid || s_cc_serial != serial || s_cc_view != (int)s_view) {
        const int64_t t0 = esp_timer_get_time();
        build_cells(a, px, pw, ph);
        s_cells_us  = (uint32_t)(esp_timer_get_time() - t0);
        s_cc_serial = serial;
        s_cc_view   = (int)s_view;
        s_cc_valid  = true;
    }

    for (int cy = 0; cy < a.h; cy++) {
        const char    *g  = s_cc_glyph + (size_t)cy * s_cc_w;
        const uint8_t *at = s_cc_attr  + (size_t)cy * s_cc_w;
        for (int cx = 0; cx < a.w; cx++)
            if (g[cx]) tui_put_char(sf, a, a.x + cx, a.y + cy, g[cx], at[cx]);
    }
}

/* The names, over the picture, in cells. */

#define LABELS_DRAWN_MAX 24

typedef struct { int x0, x1, y; } lbox;

/* ONE reservation list for the whole overlay pass, not one per layer. */

/* Room for every aircraft on top of that. The node and place-name
   loops still stop at LABELS_DRAWN_MAX, so a map with no aircraft on it
   places exactly what it did before; aircraft may use the extra, and what
   they take below the old limit comes out of the place names - live traffic
   before a hamlet, the order already gives the nodes. In PSRAM, like
   the node table beside it: only the draw path touches it. */
#define OVERLAY_BOXES_MAX (LABELS_DRAWN_MAX + ADSB_MAX_TRACKED)
EXT_RAM_BSS_ATTR static lbox s_taken[OVERLAY_BOXES_MAX];
static int  s_ntaken;

static bool box_free(int x0, int x1, int y)
{
    for (int i = 0; i < s_ntaken; i++) {
        if (s_taken[i].y != y) continue;
        /* One cell of air either side: two names that merely touch read as
           one longer name, which is worse than one name and a gap. */
        if (x0 <= s_taken[i].x1 + 1 && x1 >= s_taken[i].x0 - 1) return false;
    }
    return true;
}

static void box_take(int x0, int x1, int y)
{
    if (s_ntaken >= OVERLAY_BOXES_MAX) return;
    s_taken[s_ntaken].x0 = x0;
    s_taken[s_ntaken].x1 = x1;
    s_taken[s_ntaken].y  = y;
    s_ntaken++;
}

/* The pad's box is already-taken ground, the same as a label's. Called once
   at the start of the pass; everything drawn after it steps around it. */
static void overlay_reset(tui_rect a, tui_rect avoid)
{
    s_ntaken = 0;
    if (avoid.h <= 0) return;
    const int x0 = avoid.x - a.x, x1 = x0 + avoid.w - 1;
    const int y0 = avoid.y - a.y, y1 = y0 + avoid.h - 1;
    for (int ry = y0; ry <= y1; ry++) box_take(x0, x1, ry);
}

/* Bigger is more important. min_zoom is the cartographer's own answer and
   comes first; rank breaks ties and covers tiles that omit min_zoom. */
static int importance(const carto_label *L)
{
    return (L->min_zoom ? (32 - L->min_zoom) * 8 : 0) + L->rank;
}

static void draw_labels(tui_surface *sf, tui_rect a, int sx, int sy,
                        tui_rect avoid)
{
    const carto_label *L = NULL;
    const int n = ls_map_labels(&L);
    if (!L || n <= 0) return;

    const int zoom = ls_map_zoom();

    /* Most important first, by insertion into a small ordered list - n is
       at most sixty-four and this runs once per view change, not per frame. */
    int order[64];
    int m = 0;
    for (int i = 0; i < n && i < 64; i++) {
        if (L[i].min_zoom && zoom < (int)L[i].min_zoom) continue;
        int j = m++;
        while (j > 0 && importance(&L[order[j - 1]]) < importance(&L[i])) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }

    /*/The reservations are the pass's, not this layer's:
       see overlay_reset. A name that would land under the pad, or on a node
       marker drawn before it, is skipped by the existing crowd rule instead
       of being drawn and then painted over a letter at a time. */
    const int avoid_x0 = avoid.x - a.x, avoid_x1 = avoid_x0 + avoid.w - 1;
    const int avoid_y0 = avoid.y - a.y, avoid_y1 = avoid_y0 + avoid.h - 1;

    const uint8_t attr = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dot  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);

    for (int k = 0; k < m && s_ntaken < LABELS_DRAWN_MAX; k++) {
        const carto_label *lb = &L[order[k]];

        /* Frame pixels to cells: the render is sx by sy sub-pixels per cell. */
        const int cx = lb->x / sx;
        const int cy = lb->y / sy;
        if (cx < 0 || cy < 0 || cx >= a.w || cy >= a.h) continue;
        /* The point's own dot, not only its name - a dot under the pad is
           still a dot the pad then draws over. */
        if (avoid.h > 0 && cx >= avoid_x0 && cx <= avoid_x1 &&
            cy >= avoid_y0 && cy <= avoid_y1) continue;

        int len = (int)strlen(lb->text);
        const int room = a.w / 3;
        if (len > room) {
            len = room;
            while (len > 4 && lb->text[len] != ' ' && lb->text[len - 1] != ' ')
                len--;
            while (len > 1 && lb->text[len - 1] == ' ') len--;
        }

        int x0 = cx - len / 2;
        if (x0 < 0) x0 = 0;
        if (x0 + len > a.w) x0 = a.w - len;
        if (x0 < 0) continue;

        /* Under the dot when there is room, over it when there is not. */
        int y = (cy + 1 < a.h) ? cy + 1 : cy - 1;
        if (y < 0 || y >= a.h) continue;
        if (!box_free(x0, x0 + len - 1, y)) continue;

        tui_put_char(sf, a, a.x + cx, a.y + cy, LS_TUI_SHADE_FULL, dot);
        for (int i = 0; i < len; i++)
            tui_put_char(sf, a, a.x + x0 + i, a.y + y, lb->text[i], attr);

        box_take(x0, x0 + len - 1, y);
    }
}

/* ------------------------------------------------------------ mesh nodes */

/* The nodes, on the map. */

static bool s_nodes_on = true;

/* Latitude to the web mercator y the tiles are cut in, as a fraction of the
   world. Not the equirectangular approximation ls_geo uses: that is fine for
   a range and a bearing between two nearby points and wrong for placing a
   pixel on a projected tile. */
static double merc_y(double lat)
{
    if (lat >  85.05) lat =  85.05;
    if (lat < -85.05) lat = -85.05;
    const double r = lat * M_PI / 180.0;
    return (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0;
}

/* Where a coordinate lands in the rendered frame, in cells of `a`. False when
   it is off the pane. */
static bool map_cell_of(double lat, double lon, tui_rect a, int pw, int ph,
                        int *cx, int *cy)
{
    double clat = 0, clon = 0;
    ls_map_get_center(&clat, &clon);

    const int tp = ls_map_tile_px() > 0 ? ls_map_tile_px() : 256;
    const double world = (double)tp * ldexp(1.0, ls_map_zoom());

    const double dx = ((lon - clon) / 360.0) * world;
    const double dy = (merc_y(lat) - merc_y(clat)) * world;

    const double fx = pw / 2.0 + dx;
    const double fy = ph / 2.0 + dy;
    if (fx < 0 || fy < 0 || fx >= pw || fy >= ph) return false;

    *cx = (int)(fx / SUB_X);
    *cy = (int)(fy / sub_y());
    return (*cx >= 0 && *cy >= 0 && *cx < a.w && *cy < a.h);
}

/* The peers, and they are NOT on the stack: twelve of them is 480 bytes and
   the TUI task's stack is under 4 KB. The same contract scr_mesh.c records
   under - only the draw path touches this. */
EXT_RAM_BSS_ATTR static ls_mesh_peer_t s_map_peers[LS_MESH_MAX_PEERS];

static void draw_mesh_nodes(tui_surface *sf, tui_rect a, int pw, int ph,
                            tui_rect avoid)
{
    if (!s_nodes_on) return;
    const int n = ls_mesh_peers(s_map_peers, LS_MESH_MAX_PEERS);
    if (n <= 0) return;

    const int avoid_x0 = avoid.x - a.x, avoid_x1 = avoid_x0 + avoid.w - 1;
    const int avoid_y0 = avoid.y - a.y, avoid_y1 = avoid_y0 + avoid.h - 1;

    for (int i = 0; i < n && s_ntaken < LABELS_DRAWN_MAX; i++) {
        if (!s_map_peers[i].has_loc) continue;
        int cx, cy;
        if (!map_cell_of(s_map_peers[i].lat_e6 / 1e6,
                         s_map_peers[i].lon_e6 / 1e6, a, pw, ph, &cx, &cy))
            continue;
        if (avoid.h > 0 && cx >= avoid_x0 && cx <= avoid_x1 &&
            cy >= avoid_y0 && cy <= avoid_y1) continue;

        /* Colour is age, not signal. Signal is a property of the
           path between two radios and changes with a step sideways; how long
           ago a node was heard is a property of the node, and it is the one
           that answers "is that thing still there". */
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
        const uint32_t age = (now > s_map_peers[i].last_heard)
                             ? now - s_map_peers[i].last_heard : 0;
        const uint8_t hue = age < 300  ? (TUI_MAGENTA | TUI_BRIGHT)
                          : age < 1800 ? TUI_MAGENTA
                                       : LS_DIM_FG;   /**/

        tui_put_char(sf, a, a.x + cx, a.y + cy, LS_TUI_SHADE_FULL,
                     TUI_ATTR(hue, TUI_BLACK));

        char who[LS_MESH_PEER_NAME > 9 ? LS_MESH_PEER_NAME : 9];
        if (s_map_peers[i].name[0])
            snprintf(who, sizeof(who), "%s", s_map_peers[i].name);
        else
            snprintf(who, sizeof(who), "%.8s", s_map_peers[i].id);

        int len = (int)strlen(who);
        const int room = a.w / 3;
        if (len > room) len = room;
        int x0 = cx + 2;
        if (x0 + len > a.w) x0 = cx - 1 - len;
        if (x0 < 0) continue;
        const int y = cy;
        if (!box_free(x0, x0 + len - 1, y)) continue;

        for (int k = 0; k < len; k++)
            tui_put_char(sf, a, a.x + x0 + k, a.y + y, who[k],
                         TUI_ATTR(hue, TUI_BLACK));

        /* The marker cell as well as the name: a place name drawn through
           a node's dot loses the dot, and the dot is the part that says
           where the node is. */
        box_take(x0 < cx ? x0 : cx, (x0 + len - 1) > cx ? x0 + len - 1 : cx, y);
    }
}

/* ------------------------------------------------------------- aircraft */

/* The aircraft, on the map. */

#define AIRCRAFT_SHOW_US   (120 * 1000000LL)
#define AIRCRAFT_FRESH_US   (30 * 1000000LL)

static char track_glyph(const adsb_aircraft_t *ac)
{
    if (ac->velocity <= 0) return '+';
    const int h = ((ac->heading % 360) + 360) % 360;
    if (h < 45 || h >= 315) return '^';
    if (h < 135) return '>';
    if (h < 225) return 'v';
    return '<';
}

static void draw_aircraft(tui_surface *sf, tui_rect a, int pw, int ph,
                          tui_rect avoid)
{
    const int avoid_x0 = avoid.x - a.x, avoid_x1 = avoid_x0 + avoid.w - 1;
    const int avoid_y0 = avoid.y - a.y, avoid_y1 = avoid_y0 + avoid.h - 1;
    const int64_t  now = esp_timer_get_time();
    const uint32_t sel_icao = adsb_select_get_icao();

    for (int slot = 0; slot < ADSB_MAX_TRACKED && s_ntaken < OVERLAY_BOXES_MAX;
         slot++) {
        const adsb_aircraft_t *ac = adsb_state_get(slot);
        if (!ac || !ac->active || !ac->pos_valid) continue;
        const int64_t age = now - ac->last_seen_us;
        if (age > AIRCRAFT_SHOW_US) continue;

        int cx, cy;
        if (!map_cell_of(ac->lat, ac->lon, a, pw, ph, &cx, &cy)) continue;
        if (avoid.h > 0 && cx >= avoid_x0 && cx <= avoid_x1 &&
            cy >= avoid_y0 && cy <= avoid_y1) continue;

        const uint8_t band = ac->altitude < 5000  ? TUI_YELLOW
                           : ac->altitude < 20000 ? TUI_GREEN
                                                  : TUI_CYAN;
        const uint8_t hue = age <= AIRCRAFT_FRESH_US
                            ? (uint8_t)(band | TUI_BRIGHT) : band;
        tui_put_char(sf, a, a.x + cx, a.y + cy, track_glyph(ac),
                     ac->icao == sel_icao ? TUI_ATTR(TUI_BLACK, hue)
                                          : TUI_ATTR(hue, TUI_BLACK));

        char who[10];
        if (ac->callsign[0])
            snprintf(who, sizeof(who), "%.8s", ac->callsign);
        else
            snprintf(who, sizeof(who), "%06lX", (unsigned long)ac->icao);
        int len = (int)strlen(who);
        const int room = a.w / 3;
        if (len > room) len = room;
        int x0 = cx + 2;
        if (x0 + len > a.w) x0 = cx - 1 - len;

        if (len > 0 && x0 >= 0 && box_free(x0, x0 + len - 1, cy)) {
            for (int k = 0; k < len; k++)
                tui_put_char(sf, a, a.x + x0 + k, a.y + cy, who[k],
                             TUI_ATTR(hue, TUI_BLACK));
            box_take(x0 < cx ? x0 : cx,
                     (x0 + len - 1) > cx ? x0 + len - 1 : cx, cy);
        } else {
            /* The mark alone still claims its cell: a place name placed
               after this must step around the aircraft, not through it. */
            box_take(cx, cx, cy);
        }
    }
}

/* Search, on a device whose only keypad is numeric. */

static int s_find_x[LS_PICKER_MAX];
static int s_find_y[LS_PICKER_MAX];
static int s_find_n;
static int s_find_pw, s_find_ph;

static void on_pick(int i)
{
    if (i < 0 || i >= s_find_n) return;
    /* The centre of the frame is where we are, so moving the place to the
       centre is the difference between the two. */
    ls_map_pan(s_find_x[i] - s_find_pw / 2, s_find_y[i] - s_find_ph / 2);
}

/* How far, and which way, in words. */
static void place_detail(char *out, size_t cap, int dx_px, int dy_px)
{
    double lat = 0, lon = 0;
    ls_map_get_center(&lat, &lon);

    /* Web-mercator metres per pixel: the equatorial figure, shrunk by the
       cosine of the latitude, scaled for a tile drawn at something other
       than its natural 256. */
    const int tp = ls_map_tile_px();
    const double mpp = 156543.03392 * cos(lat * M_PI / 180.0)
                     / ldexp(1.0, ls_map_zoom())
                     * (256.0 / (double)(tp > 0 ? tp : 256));

    const double east = dx_px * mpp;
    const double south = dy_px * mpp;
    const double miles = sqrt(east * east + south * south) / 1609.344;

    static const char *const ROSE[8] = { "N", "NE", "E", "SE",
                                         "S", "SW", "W", "NW" };
    double brg = atan2(east, -south) * 180.0 / M_PI;
    if (brg < 0) brg += 360.0;
    const int oct = ((int)((brg + 22.5) / 45.0)) % 8;

    if (miles < 0.05)     snprintf(out, cap, "here");
    else if (miles < 10)  snprintf(out, cap, "%.1fmi %s", miles, ROSE[oct]);
    else                  snprintf(out, cap, "%.0fmi %s", miles, ROSE[oct]);
}

static ls_act_status_t a_map_find(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    s_find_n = 0;
    ls_picker_open("PLACES", on_pick);

    const carto_label *L = NULL;
    const int n = ls_map_labels(&L);

    const char *why = ls_map_status();
    if (why || !L || n <= 0) {

        ls_picker_empty_reason(why ? why : "no named places in this view");
        out->kind = LS_VAL_TEXT;
        out->s = why ? why : "nothing named in view - try zooming out";
        return LS_ACT_OK;
    }

    ls_map_render(&s_find_pw, &s_find_ph);

    /* Most important first, by insertion into a small ordered list. Same
       ordering as the drawn labels, so the list and the map agree. */
    int order[LS_PICKER_MAX];
    int m = 0;
    for (int i = 0; i < n && m < LS_PICKER_MAX; i++) {
        int j = m++;
        while (j > 0 && importance(&L[order[j - 1]]) < importance(&L[i])) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }

    for (int k = 0; k < m; k++) {
        const carto_label *lb = &L[order[k]];
        char detail[LS_PICKER_DETAIL];
        place_detail(detail, sizeof(detail),
                     lb->x - s_find_pw / 2, lb->y - s_find_ph / 2);
        if (!ls_picker_add(lb->text, detail)) break;
        s_find_x[s_find_n] = lb->x;
        s_find_y[s_find_n] = lb->y;
        s_find_n++;
    }

    static char note[40];
    snprintf(note, sizeof(note), "%d place%s in view", s_find_n,
             s_find_n == 1 ? "" : "s");
    out->kind = LS_VAL_TEXT;
    out->s = note;
    return LS_ACT_OK;
}

/* A pad, because a drag is not reachable here. */

static tui_rect pad_rect_for(tui_rect body)
{
    /* Below this the box would crowd whatever else is in the corner, and a
       pane this small has nowhere to put a control that does not sit on top
       of something else anyway. */
    if (body.w < 16 || body.h < 8) return tui_rect_make(0, -1, 0, 0);
    return tui_rect_make(body.x + body.w - 3, body.y + body.h - 3, 3, 3);
}

static void draw_pan_pad(tui_surface *sf, tui_rect r)
{
    if (r.h <= 0) return;
    const int cx = r.x + 1, cy = r.y + 1;
    const uint8_t attr = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    tui_put_char(sf, r, cx,     cy - 1, '^', attr);
    tui_put_char(sf, r, cx - 1, cy,     '<', attr);
    tui_put_char(sf, r, cx + 1, cy,     '>', attr);
    tui_put_char(sf, r, cx,     cy + 1, 'v', attr);
}

static void draw(tui_surface *sf, tui_rect area)
{

    /* The controls sit under the map in cells, so the map gets what is left.
       Three rows of buttons plus a row of air. */
    const int want = ls_quick_rows(QUICK, N_QUICK, area.w, ls_tui_is_wide());
    const int ctl_h = (area.h > want + 10) ? want : 0;
    tui_rect body = tui_rect_make(area.x, area.y, area.w, area.h - ctl_h);

    s_quick_rect = ctl_h
        ? tui_rect_make(area.x, area.y + body.h, area.w, ctl_h)
        : tui_rect_make(0, -1, 0, 0);

    /* Begin FIRST, then ask what is wrong. The other order asks a map that
       has not been started why it is not drawable, gets "it has no
       framebuffer", and never calls the one function that would give it one.
       That cost a flash to find. */
    /* The tile shrinks with the buffer, or the same rectangle covers
       a fifth of the ground and the map is silently five zoom steps in. */
    int cw = 10;
    ls_tui_geometry(NULL, NULL, &cw, NULL);
    ls_map_set_tile_px(256 * SUB_X / (cw > 0 ? cw : 10));
    ls_map_begin(body.w * SUB_X, body.h * sub_y());

    /* Render FIRST, then ask what is wrong. */

    int pw = 0, ph = 0;
    const uint16_t *px = ls_map_render(&pw, &ph);
    const char *why = ls_map_status();

    if (!px) {
        draw_placeholder(sf, body, why ? why : "the map has not been started");
        /* Nothing to pan to, so nothing claims the corner - a stale box left
           over from the last open map would eat taps meant for the
           placeholder underneath it. */
        s_pad_rect = tui_rect_make(0, -1, 0, 0);
    } else {
        {
            /* A part drawn frame is not an empty one. While tiles
               are still arriving the picture is worth showing, gaps and
               all - that IS the progress indicator - and the "nothing here"
               message must wait until there is nothing left to arrive. */
            if (why && !ls_map_render_busy()) {
                draw_placeholder(sf, body, why);
                s_pad_rect = tui_rect_make(0, -1, 0, 0);
                if (ctl_h) ls_quick_draw_posture(sf, s_quick_rect, ls_tui_is_wide(), QUICK, N_QUICK);
                return;
            }
            s_pad_rect = pad_rect_for(body);
            draw_cells(sf, body, px, pw, ph);
            /* Nodes before names, so the names step around them:
               a node is live data about the network you are standing in and
               a hamlet is not. See overlay_reset. */
            overlay_reset(body, s_pad_rect);
            draw_mesh_nodes(sf, body, pw, ph, s_pad_rect);
            /* Aircraft after the nodes and before the names: live
               traffic steps around the mesh, and a town steps around both. */
            draw_aircraft(sf, body, pw, ph, s_pad_rect);
            draw_labels(sf, body, SUB_X, sub_y(), s_pad_rect);

            s_map_cells = body;

            double lat = 0, lon = 0;
            ls_map_get_center(&lat, &lon);
            ls_map_stats_t st;
            ls_map_stats(&st);

            /* A turning mark while tiles are still arriving.

               The render is spread over frames now, so for about half a
               second after a pan the map on the glass is a real picture with
               real gaps in it. Gaps look like a fault. This is the
               difference between "still drawing" and "that is all there
               is", and it is one cell that stops turning when the render
               finishes - so it also answers "did it get stuck". */
            const bool busy = ls_map_render_busy();
            const char pip = busy ? ls_motion_pip(true) : ' ';

            /* Which view this is. */

            const char view = (s_view == MAP_VIEW_MONO) ? 'L' : 'C';

            if (ls_tui_is_wide())
                snprintf(s_note, sizeof(s_note),
                         "%c z%d%c %.4f %.4f %d/%dt r%lums c%lums",
                         pip, ls_map_zoom(), view, lat, lon,
                         st.tiles_drawn, st.tiles_wanted,
                         (unsigned long)(st.render_us / 1000),
                         (unsigned long)(s_cells_us / 1000));
            else
                snprintf(s_note, sizeof(s_note), "%c z%d%c %d/%dt r%lu c%lu",
                         pip, ls_map_zoom(), view,
                         st.tiles_drawn, st.tiles_wanted,
                         (unsigned long)(st.render_us / 1000),
                         (unsigned long)(s_cells_us / 1000));
            ls_tui_status_set(s_note, NULL);

            /* Drawn last so the glyphs sit on top of the picture - the
               reservation that keeps them off any label already happened,
               above, before draw_labels() ran. */
            draw_pan_pad(sf, s_pad_rect);
        }
    }

    if (ctl_h) ls_quick_draw_posture(sf, s_quick_rect, ls_tui_is_wide(), QUICK, N_QUICK);
}

/* --------------------------------------------------------------- input -- */

static int pan_step(void)
{
    const int px = (s_map_cells.w * SUB_X) / PAN_FRACTION;
    return px > 0 ? px : 32;
}

static bool key(ls_tk_t k, char ch)
{
    if (k == LS_TK_CHAR &&
        ls_quick_key(ch, QUICK, N_QUICK, ls_quick_grant_builtin(), NULL))
        return true;

    if (k == LS_TK_CHAR && (ch == 'n' || ch == 'N')) {
        s_nodes_on = !s_nodes_on;
        ls_tui_invalidate();
        return true;
    }

    switch (k) {
    case LS_TK_LEFT:  ls_map_pan(-pan_step(), 0); return true;
    case LS_TK_RIGHT: ls_map_pan( pan_step(), 0); return true;
    case LS_TK_UP:    ls_map_pan(0, -pan_step()); return true;
    case LS_TK_DOWN:  ls_map_pan(0,  pan_step()); return true;
    default: return false;
    }
}

/* A tap inside the map recentres on where it landed, which is the one
   gesture every map has and the only one reachable without a drag: the
   debounce layer reports a completed tap and nothing between. */
static bool touch(int col, int row)
{
    if (s_quick_rect.h > 0 && row >= s_quick_rect.y &&
        row < s_quick_rect.y + s_quick_rect.h &&
        ls_quick_touch(col, row, QUICK, N_QUICK,
                       ls_quick_grant_builtin(), NULL))
        return true;

    /* Ahead of the recentre test, since the pad sits inside the same
       rectangle the recentre logic hit-tests against. Whole box claimed, its
       blank corners included - the box reads as one control at arm's length,
       and a tap a pixel off a glyph should not fall through to recentring on
       a spot the thumb was not aiming for. */
    if (s_pad_rect.h > 0 &&
        col >= s_pad_rect.x && col < s_pad_rect.x + s_pad_rect.w &&
        row >= s_pad_rect.y && row < s_pad_rect.y + s_pad_rect.h) {
        const int dx = col - (s_pad_rect.x + 1);
        const int dy = row - (s_pad_rect.y + 1);
        if      (dx == 0 && dy == -1) ls_map_pan(0, -pan_step());
        else if (dx == 0 && dy ==  1) ls_map_pan(0,  pan_step());
        else if (dx == -1 && dy == 0) ls_map_pan(-pan_step(), 0);
        else if (dx ==  1 && dy == 0) ls_map_pan( pan_step(), 0);
        return true;
    }

    if (s_map_cells.h <= 0) return false;
    if (col < s_map_cells.x || col >= s_map_cells.x + s_map_cells.w)
        return false;
    if (row < s_map_cells.y || row >= s_map_cells.y + s_map_cells.h)
        return false;

    const int dx = (col - s_map_cells.x - s_map_cells.w / 2) * SUB_X;
    const int dy = (row - s_map_cells.y - s_map_cells.h / 2) * sub_y();
    ls_map_pan(dx, dy);
    return true;
}

/* Called by the map.reload action, which is how the console and a card app
   reach the same rescan the button does. */
void ls_scr_map_reload(void) { rescan(); }

const ls_tui_screen_t ls_scr_map = {
    .name = "MAP",
    .hint = "TAP centre  ARROWS pan  F find  N nodes  = in  - out",
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .key = key,
    .touch = touch,
};
