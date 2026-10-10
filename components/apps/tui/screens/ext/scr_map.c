#include "../../ls_cells.h"
#include "../../ls_cartocore_map.h"
#ifdef LS_MAP_AUDIT
static size_t s_carto_audit_count;
static int s_carto_audit_bad,s_carto_low_masked;
#endif
/* MAP: vector tiles off the card, drawn in pixels the grid lends it, with
   the live picture over them: aircraft, mesh nodes, this receiver, and the
   markers and lines placed by hand. */

#include "../../ls_tui_screen.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "../../ls_app.h"
#include "../../ls_compass_live.h"
#include "../../ls_geo.h"
#include "../../ls_keyboard.h"
#include "../../ls_map.h"
#include "../../ls_route_live.h"
#include "../../ls_map_ink.h"
#include "../../ls_map_label_policy.h"
#include "../../ls_map_marks.h"
#include "../../ls_motion.h"
#include "../../ls_map_motion.h"
#include "../../ls_notes.h"
#include "../../ls_picker.h"
#include "../../ls_quick.h"
#include "../../ls_theme.h"
#include "../../ls_tui.h"
#include "../../ls_tui_ui.h"
#include "../../ls_stroke.h"
#include "carto/style.h"
#include "carto/raster.h"
#include "core/settings.h"
/* The nodes drawn on the map are the mesh's own peers. */
#include "esp_attr.h"
#include "ls_mesh.h"
#include "ls_gps.h"
#include "apps/fm/aprs_store.h"
#include "apps/fm/ais_store.h"
#include "../../experiments/rs41_store.h"
/* And the aircraft are ADS-B's own table - the one its list and radar
   read, not a copy of it. */
#include "apps/adsb/adsb_state.h"

#define MAP_DIR "/sdcard/maps"

static bool     s_opened;
static tui_rect s_map_cells;      /* the rectangle the grid lent us */
static tui_rect s_quick_rect;
static tui_rect s_pad_rect;
static tui_rect s_card_rect;
static int s_header_h;
static bool s_controls_hidden;
static tui_rect s_controls_hit;
static char     s_note[96];
static char     s_toast[64];
static int64_t  s_toast_us;
EXT_RAM_BSS_ATTR static ls_gps_state_t s_receiver;
static bool s_receiver_fresh;

/* Pan by a third of the frame, which is far enough to be worth the tap and
   short enough to keep your place. */
#define PAN_FRACTION 3

/* ------------------------------------------------------------- layers -- */

/* What is drawn over the tiles, one bit each, remembered in NVS. */
enum {
    L_AIR     = 1u << 0,
    L_TRAILS  = 1u << 1,
    L_BLOCKS  = 1u << 2,    /* two line data blocks; otherwise callsign only */
    L_NOLABEL = 1u << 3,    /* no aircraft labels at all                     */
    L_MESH    = 1u << 4,
    L_MARKS   = 1u << 5,
    L_RINGS   = 1u << 6,
    L_COVER   = 1u << 7,
    L_PLACES  = 1u << 8,
    L_LINKS   = 1u << 9,
    L_VECTORS = 1u << 10,
    L_APRS    = 1u << 11,
    L_ROUTE   = 1u << 12,
    L_AIS     = 1u << 13,
    L_SONDE   = 1u << 14,
};
#define LAYERS_DEFAULT (L_AIR | L_TRAILS | L_MESH | L_MARKS | L_RINGS | \
                        L_PLACES | L_LINKS | L_VECTORS | L_APRS | L_ROUTE | L_AIS | L_SONDE)

static uint32_t s_layers = LAYERS_DEFAULT;
static bool     s_layers_loaded;

static bool layer(uint32_t bit) { return (s_layers & bit) != 0; }

#define LAYER_BITS 0x00FFFFFFu

static void palette_load(void);
static uint32_t style_bits(void);

static void layers_set(uint32_t bits)
{
    s_layers = bits & LAYER_BITS;
    settings_set_map_layers(s_layers | style_bits());
}

static void say(const char *text)
{
    snprintf(s_toast, sizeof(s_toast), "%s", text);
    s_toast_us = esp_timer_get_time();
}

/* --------------------------------------------------------------- quick -- */

static const ls_quick_t QUICK[] = {
    { .label = "ZOOM+", .kind = LS_QUICK_ACTION, .action = "map.zoom",
      .choices = (const char *const[]){ "1" }, .nchoices = 1, .key = '=' },
    { .label = "ZOOM-", .kind = LS_QUICK_ACTION, .action = "map.zoom",
      .choices = (const char *const[]){ "-1" }, .nchoices = 1, .key = '-' },
    { .label = "GO TO", .kind = LS_QUICK_ACTION, .action = "map.find", .key = 'f' },
    { .label = "ROUTE", .kind = LS_QUICK_ACTION, .action = "map.route", .key = 'u' },
    { .label = "OPTIONS", .kind = LS_QUICK_ACTION, .action = "map.route_options", .key = 'o' },
    { .label = "FOLLOW", .kind = LS_QUICK_ACTION, .action = "map.track", .key = 'g' },
    { .label = "MARK", .kind = LS_QUICK_ACTION, .action = "map.mark", .key = 'k' },
    { .label = "DRAW", .kind = LS_QUICK_ACTION, .action = "map.draw", .key = 'd' },
    { .label = "LAYERS", .kind = LS_QUICK_ACTION, .action = "map.layers", .key = 'l' },
    { .label = "STYLE", .kind = LS_QUICK_ACTION, .action = "map.style", .key = 's' },
    { .label = "SET HOME", .kind = LS_QUICK_ACTION, .action = "map.home", .key = 'h' },
};
#define N_QUICK ((int)(sizeof(QUICK) / sizeof(QUICK[0])))

/* While a line is being drawn the row is about the line. */
static const ls_quick_t QUICK_DRAW[] = {
    { .label = "ADD POINT", .kind = LS_QUICK_ACTION, .action = "map.draw_add", .key = 'a' },
    { .label = "UNDO", .kind = LS_QUICK_ACTION, .action = "map.draw_undo", .key = 'u' },
    { .label = "DONE", .kind = LS_QUICK_ACTION, .action = "map.draw_done", .key = 'o' },
    { .label = "CANCEL", .kind = LS_QUICK_ACTION, .action = "map.draw_cancel", .key = 'c' },
    { .label = "ZOOM+", .kind = LS_QUICK_ACTION, .action = "map.zoom",
      .choices = (const char *const[]){ "1" }, .nchoices = 1, .key = '=' },
    { .label = "ZOOM-", .kind = LS_QUICK_ACTION, .action = "map.zoom",
      .choices = (const char *const[]){ "-1" }, .nchoices = 1, .key = '-' },
};
#define N_QUICK_DRAW ((int)(sizeof(QUICK_DRAW) / sizeof(QUICK_DRAW[0])))

static const ls_quick_t *quick_table(int *n)
{
    if (ls_sketch_drawing()) { *n = N_QUICK_DRAW; return QUICK_DRAW; }
    *n = N_QUICK;
    return QUICK;
}

/* ---------------------------------------------------------------- entry -- */

static bool find_archive(char *out, size_t cap)
{
    DIR *d = opendir(MAP_DIR);
    if (!d) return false;

    bool found = false;
    const struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const size_t n = strlen(e->d_name);
        if (n < 9) continue;
        if (strcasecmp(e->d_name + n - 8, ".pmtiles") != 0) continue;
        char path[128];
        if (n + sizeof(MAP_DIR) + 1 > sizeof(path)) continue;
        snprintf(path, sizeof(path), "%s/%.*s", MAP_DIR, (int)n, e->d_name);
        if (strlen(path) >= cap || ls_map_check_archive(path)) continue;
        if (!found || strcasecmp(path, out) < 0) {
            snprintf(out, cap, "%s", path);
            found = true;
        }
    }
    closedir(d);
    return found;
}

/* Defined with the action it registers, further down; declared here because
   this is where it is called. */
static void register_view_action(void);
static void rescan(void);
static bool cartocore_view(void);

/* Likewise: the cell cache is defined with the pass that fills it, and
   leave() is the one place that gives it back. */
static void cells_free(void);

static bool s_have_archive;
EXT_RAM_BSS_ATTR static char s_archives[LS_PICKER_MAX][128];
static int s_archive_n;
static const char *s_file_error;

static int archive_order(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

static void pick_archive(int i)
{
    if (i < 0 || i >= s_archive_n) return;
    if(cartocore_view()) {
        s_file_error=ls_carto_map_select(s_archives[i])?NULL:"cannot open .ctile map";
        return;
    }
    if (!ls_map_open(s_archives[i])) {
        s_file_error = ls_map_open_error();
        return;
    }
    s_file_error = NULL;
    s_have_archive = true;
    double lat, lon;
    ls_map_get_center(&lat, &lon);
    if (ls_map_zoom_covering(lat, lon) < 0) {
        FILE *f = fopen(s_archives[i], "rb");
        uint8_t h[127];
        if (f) {
            if (fread(h, 1, sizeof(h), f) == sizeof(h)) {
                const uint32_t x = (uint32_t)h[119] | (uint32_t)h[120] << 8 |
                    (uint32_t)h[121] << 16 | (uint32_t)h[122] << 24;
                const uint32_t y = (uint32_t)h[123] | (uint32_t)h[124] << 8 |
                    (uint32_t)h[125] << 16 | (uint32_t)h[126] << 24;
                ls_map_center((int32_t)y / 1e7, (int32_t)x / 1e7);
                ls_map_zoom_by((int)h[118] - ls_map_zoom());
            }
            fclose(f);
        }
    }
}

static ls_act_status_t a_map_files(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    if(cartocore_view()) {
        s_archive_n=(int)ls_carto_map_files(s_archives,LS_PICKER_MAX);
        qsort(s_archives,s_archive_n,sizeof(s_archives[0]),archive_order);
        ls_picker_open("CTILE MAPS",pick_archive);
        for(int i=0;i<s_archive_n;i++) ls_picker_add(s_archives[i]+sizeof(MAP_DIR),"open map");
        if(!s_archive_n) ls_picker_empty_reason("No cached .ctile list; reopen after map loads");
        out->kind=LS_VAL_TEXT;out->s="choose a map";return LS_ACT_OK;
    }
    s_archive_n = 0;
    DIR *d = opendir(MAP_DIR);
    const struct dirent *e;
    if (d) {
        while ((e = readdir(d)) != NULL && s_archive_n < LS_PICKER_MAX) {
            const size_t n = strlen(e->d_name);
            const char *suffix=cartocore_view()?".ctile":".pmtiles";
            size_t ext=strlen(suffix);
            if (n<=ext || strcasecmp(e->d_name+n-ext,suffix)) continue;
            if (n + sizeof(MAP_DIR) + 1 > sizeof(s_archives[0])) continue;
            snprintf(s_archives[s_archive_n++], sizeof(s_archives[0]), "%s/%.*s", MAP_DIR, (int)n, e->d_name);
        }
        closedir(d);
    }
    qsort(s_archives, s_archive_n, sizeof(s_archives[0]), archive_order);
    ls_picker_open("SD MAPS", pick_archive);
    for (int i = 0; i < s_archive_n; i++) {
        const char *error = cartocore_view()?NULL:ls_map_check_archive(s_archives[i]);
        const char *current = ls_map_archive();
        ls_picker_add(s_archives[i] + sizeof(MAP_DIR), error ? "unsupported" :
            current && !strcmp(current, s_archives[i]) ? "current" : "open map");
    }
    if (!s_archive_n) ls_picker_empty_reason(cartocore_view()?"put .ctile files in SD /maps":"put .pmtiles files in SD /maps");
    out->kind = LS_VAL_TEXT;
    out->s = "choose a map";
    return LS_ACT_OK;
}

static void enter(void)
{
    register_view_action();
    if (!s_layers_loaded) {
        s_layers_loaded = true;
        s_layers = settings_get_map_layers(LAYERS_DEFAULT) & LAYER_BITS;
        palette_load();
    }
    if(cartocore_view()) {
        ls_map_external_view(0,0);cells_free();s_have_archive=false;s_opened=false;s_file_error=NULL;
        return;
    }
    ls_marks_load();
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
    /* Nothing open means look again, every time this screen is entered: the
       card is usually put in after boot, and a screen that only ever looked
       once needed a reboot to notice. */
    if (!s_have_archive) rescan();
}

/* Look again. The card is usually inserted after boot, and a screen that
   only ever looked once would need a reboot to notice - which reads as the
   card not working. */
static void rescan(void)
{
    if(cartocore_view()) {
        ls_map_external_view(0,0);s_have_archive=false;s_opened=false;s_file_error=NULL;
        ls_carto_map_leave();return;
    }
    char path[128];
    if (!find_archive(path, sizeof(path))) {
        s_file_error = "no compatible map found; choose MAPS";
        return;
    }
    s_file_error = ls_map_open(path) ? NULL : ls_map_open_error();
    s_have_archive = ls_map_archive() != NULL;
}

static void leave(void)
{
    ls_place_search_close();
#ifdef LS_MAP_AUDIT
    s_carto_audit_count=0;s_carto_audit_bad=0;s_carto_low_masked=0;
#endif
    ls_tui_image(tui_rect_make(0, 0, 0, 0), NULL, 0, 0, 0);
    /* Hand the pixels back, or the map stays on the glass under the next
       screen: the cell renderer only pushes cells that changed, and none of
       the cells under a borrowed rectangle did. */
    ls_tui_reserve(tui_rect_make(0, 0, 0, 0));
    s_map_cells = tui_rect_make(0, -1, 0, 0);
    s_pad_rect = tui_rect_make(0, -1, 0, 0);
    s_card_rect = tui_rect_make(0, -1, 0, 0);
    cells_free();
    ls_carto_map_leave();
    ls_tui_basemap(NULL,tui_rect_make(0,0,0,0),NULL);
}

/* --------------------------------------------------------------- draw --- */

static void draw_placeholder(tui_surface *sf, tui_rect area, const char *why)
{
    const uint8_t dim = LS_ATTR_DIM;
    ls_panel_box(sf, area, "MAP", TUI_CYAN);
    tui_put_str(sf, area, area.x + 2, area.y + 2, why, dim);
    if(!cartocore_view() || !strcmp(why,"No map installed"))
        tui_put_str(sf, area, area.x + 2, area.y + 4,
                cartocore_view()?"Download a region in TILES > SEARCH":"put a .pmtiles archive at " MAP_DIR, dim);
}

/* Sub-pixels per cell, and why they are not the same in both axes. */

#define SUB_X (cartocore_view() ? 2 : 3)
static bool cartocore_view(void);

static int sub_y(void)
{
    if(cartocore_view()) return 4;
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

typedef enum { MAP_VIEW_FIELD = 0, MAP_VIEW_COLOUR, MAP_VIEW_MONO, MAP_VIEW_CC_SMOOTH, MAP_VIEW_CC_BRAILLE, MAP_VIEW__COUNT } map_view_t;
static map_view_t s_view;
static bool cartocore_view(void) { return s_view>=MAP_VIEW_CC_SMOOTH; }

static const char *const VIEW_NAME[MAP_VIEW__COUNT] = { "field", "blocks", "lines", "CartoCore smooth", "CartoCore braille" };

/* The map's colours. Each names what every class libcarto draws becomes,
   whether water and parks are filled or only outlined, which roads are
   worth drawing at all, and the one colour, if any, that everything over
   the map is drawn in. The vector palettes are the look of a classic radar
   terminal: black, the coast and the water as thin lines, major roads only. */
typedef struct {
    const char *name, *what;
    carto_rgb ground, water, park, building, road_lo, road_hi, major, edge;
    uint8_t   fill;           /* PF_* */
    uint8_t   road_min;       /* roads below this priority are not drawn */
    uint8_t   tint;           /* 0, or the TUI colour everything over the map takes */
} map_palette_t;

enum { PF_WATER = 1, PF_PARK = 2, PF_BUILDING = 4, PF_EDGE_WATER = 8, PF_EDGE_PARK = 16 };

static const map_palette_t PALETTES[] = {
    { "NIGHT", "dark ground, full colour",
      {8, 16, 21}, {29, 81, 110}, {36, 65, 49}, {75, 75, 80}, {110, 110, 110}, {245, 245, 245},
      {220, 206, 159}, {0, 0, 0}, PF_WATER | PF_PARK | PF_BUILDING, 1, 0 },
    { "PAPER", "a light chart for the sun",
      {236, 233, 224}, {150, 190, 214}, {196, 219, 178}, {205, 198, 188}, {150, 146, 140}, {62, 58, 52},
      {214, 120, 46}, {0, 0, 0}, PF_WATER | PF_PARK | PF_BUILDING, 1, 0 },
    { "RED", "night vision: nothing but red",
      {0, 0, 0}, {48, 0, 0}, {24, 0, 0}, {34, 4, 4}, {70, 10, 10}, {170, 25, 20},
      {235, 45, 30}, {0, 0, 0}, PF_WATER | PF_PARK | PF_BUILDING, 1, TUI_RED },
    { "GREEN", "green phosphor",
      {0, 6, 0}, {0, 44, 14}, {0, 24, 6}, {8, 30, 10}, {20, 80, 30}, {60, 170, 70},
      {130, 255, 130}, {0, 0, 0}, PF_WATER | PF_PARK | PF_BUILDING, 1, TUI_GREEN },
    { "AMBER", "amber terminal",
      {8, 4, 0}, {54, 28, 0}, {28, 14, 0}, {40, 22, 4}, {90, 50, 0}, {190, 110, 10},
      {255, 175, 45}, {0, 0, 0}, PF_WATER | PF_PARK | PF_BUILDING, 1, TUI_YELLOW },
    { "BLUE", "cold blue",
      {0, 4, 12}, {10, 40, 82}, {6, 22, 36}, {20, 30, 45}, {40, 70, 110}, {90, 150, 210},
      {170, 220, 255}, {0, 0, 0}, PF_WATER | PF_PARK | PF_BUILDING, 1, TUI_CYAN },
    /* The two scopes keep the map well under the targets: dim outlines,
       main roads only, so an aircraft is the brightest thing on the glass. */
    { "VECTOR", "radar terminal: outlines, full colour targets",
      {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {34, 42, 46}, {58, 72, 78},
      {90, 104, 112}, {28, 64, 88}, PF_EDGE_WATER, 7, 0 },
    { "SCOPE", "radar scope: green outlines, green targets",
      {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 40, 16}, {0, 62, 24},
      {0, 105, 40}, {0, 66, 26}, PF_EDGE_WATER, 7, TUI_GREEN },
};
#define N_PALETTES ((int)(sizeof(PALETTES) / sizeof(PALETTES[0])))
enum { PAL_NIGHT = 0, PAL_PAPER = 1 };

static int s_palette;

/* Class colours derive from every active TUI theme, including Daylight.
 * Map style choices affect fills/road filtering, never override a live theme. */
static bool s_basemap_busy, s_map_crowded;
static int s_palette_key;
static uint16_t s_map_colours[16];
static carto_rgb rgb565(uint16_t c) {
    return (carto_rgb){((c>>11)&31)*255/31,((c>>5)&63)*255/63,(c&31)*255/31};
}
static carto_rgb rgb_mix(carto_rgb a,carto_rgb b,int n,int d) {
    return (carto_rgb){a.r+(b.r-a.r)*n/d,a.g+(b.g-a.g)*n/d,a.b+(b.b-a.b)*n/d};
}
static const map_palette_t *palette(void)
{
    static map_palette_t p;
    static const ls_tui_theme_t *previous;
    static int key=-1;
    const ls_tui_theme_t *t=ls_tui_active_theme();
    const int next=s_palette*2+(s_basemap_busy?1:0);
    if(previous==t && key==next) return &p;
    previous=t;key=next;s_palette_key++;
    p=PALETTES[s_palette>=0 && s_palette<N_PALETTES?s_palette:0];
    p.name=t->name;p.tint=0;
    carto_rgb ground=rgb565(t->palette[0]), blue=rgb565(t->palette[TUI_BLUE]);
    carto_rgb green=rgb565(t->palette[TUI_GREEN]), white=rgb565(t->palette[TUI_WHITE]);
    p.ground=rgb_mix(ground,blue,1,16);
    p.water=rgb_mix(p.ground,blue,5,8);
    p.park=rgb_mix(p.ground,green,2,8);
    p.building=rgb_mix(p.ground,white,2,8);
    p.road_lo=rgb_mix(p.ground,white,3,8);
    p.road_hi=rgb_mix(p.ground,white,6,8);
    p.major=rgb_mix(p.ground,white,7,8);
    p.edge=p.water;
    if(s_basemap_busy) {
        p.water=rgb_mix(p.ground,p.water,3,4);p.park=rgb_mix(p.ground,p.park,3,4);
        p.building=rgb_mix(p.ground,p.building,3,4);
        p.road_lo=rgb_mix(p.ground,p.road_lo,3,4);p.road_hi=rgb_mix(p.ground,p.road_hi,3,4);
        p.major=rgb_mix(p.ground,p.major,3,4);
    }
    for(int i=0;i<16;i++) s_map_colours[i]=carto_rgb565(p.ground);
    s_map_colours[2]=s_map_colours[10]=carto_rgb565(p.park);
    s_map_colours[4]=s_map_colours[12]=carto_rgb565(p.water);
    s_map_colours[6]=s_map_colours[14]=carto_rgb565(p.water);
    s_map_colours[8]=carto_rgb565(p.building);
    s_map_colours[7]=carto_rgb565(p.road_lo);
    s_map_colours[15]=carto_rgb565(p.road_hi);
    s_map_colours[3]=s_map_colours[11]=carto_rgb565(p.major);
    s_map_colours[5]=s_map_colours[13]=carto_rgb565(p.building);
    s_map_colours[1]=s_map_colours[9]=carto_rgb565(p.building);
    return &p;
}
/* Warm accent is reserved for data strokes; basemap roads use neutral ink.
 * In monochrome themes its lightness still clears the muted class range. */
static uint8_t radar_attr(void) { return TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK); }

static void glass(tui_surface *sf, tui_rect clip, int x, int y, char ch, uint8_t attr)
{
    ls_tui_put_glass(sf, clip, x, y, ch, ls_ink_tinted(attr));
}

static ls_act_status_t a_map_view(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    s_view = (map_view_t)((s_view + 1) % MAP_VIEW__COUNT);
    if(!cartocore_view()) ls_carto_map_leave();
    layers_set(s_layers);
    out->kind = LS_VAL_TEXT;
    out->s = VIEW_NAME[s_view];
    return LS_ACT_OK;
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
static uint16_t *s_field_pixels;
static int s_field_w, s_field_h;
static uint32_t s_field_serial;
static bool s_field_valid;

static uint8_t s_acc[CELL_COLS_MAX * 4];
static uint8_t s_ink[CELL_COLS_MAX * 4];
static EXT_RAM_BSS_ATTR uint8_t s_prev[CELL_COLS_MAX * 3];

static uint8_t s_cur[CELL_COLS_MAX * 3];

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
    heap_caps_free(s_field_pixels); s_field_pixels = NULL;
    s_field_valid = false;
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
            const uint8_t *b = &s_ink[cx * 4];
            const char st = ls_stroke_glyph(b[0], b[1], b[2], b[3]);
            if (!st) { g[cx] = 0; continue; }
            g[cx]  = st;
            uint8_t top=s_acc[cx*4];
            for(int i=1;i<4;i++) if(s_acc[cx*4+i]>top) top=s_acc[cx*4+i];
            at[cx] = ls_ink_tinted(top==LS_MAP_ROAD?MONO_ATTR:ink_attr((ls_map_ink_t)top));
            continue;
        }

        const uint8_t *q = &s_acc[cx * 4];
        uint8_t top = q[0];
        for (int i = 1; i < 4; i++) if (q[i] > top) top = q[i];
        if (top == LS_MAP_GROUND) { g[cx] = 0; continue; }

        /* The glyph says WHICH quarters had that class in them. */
        g[cx]  = LS_TUI_QUAD(q[0] == top, q[1] == top,
                             q[2] == top, q[3] == top);
        at[cx] = ls_ink_tinted(ink_attr((ls_map_ink_t)top));
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
        /* The row just walked becomes the row above. */
        memcpy(s_prev, s_cur, (size_t)xmax);
    }
    flush_row(cy, w, mono);
}

/* The field picture, through the palette. Each pixel is first named by
   the colour libcarto drew it in - ground, water, park, building, a road
   of some priority, a major road - and then painted as the palette says.
   Outlines need the neighbours' names, so three rows of names are kept. */
enum { K_GROUND = 0, K_WATER, K_PARK, K_BUILDING, K_ROAD = 4, K_MAJOR = 4 + CARTO_ROAD_PRIO_MAX, K_OTHER };

typedef struct { uint16_t rgb; uint8_t k; } field_key_t;
static field_key_t s_keys[5 + CARTO_ROAD_PRIO_MAX];
static int s_nkeys;

static void field_keys(void)
{
    if (s_nkeys) return;
    carto_style st;
    carto_style_default(&st);
    s_keys[s_nkeys++] = (field_key_t){ carto_rgb565(st.bg), K_GROUND };
    s_keys[s_nkeys++] = (field_key_t){ carto_rgb565(st.water), K_WATER };
    s_keys[s_nkeys++] = (field_key_t){ carto_rgb565(st.park), K_PARK };
    s_keys[s_nkeys++] = (field_key_t){ carto_rgb565(st.road_color), K_MAJOR };
    s_keys[s_nkeys++] = (field_key_t){ carto_rgb565(st.building), K_BUILDING };
    for (int p = CARTO_ROAD_PRIO_MIN; p < CARTO_ROAD_PRIO_MAX; p++)
        s_keys[s_nkeys++] = (field_key_t){ carto_rgb565(st.road_color_by_prio[p]), (uint8_t)(K_ROAD + p) };
}

static uint8_t field_kind(uint16_t v)
{
    for (int k = 0; k < s_nkeys; k++) if (s_keys[k].rgb == v) return s_keys[k].k;
    return K_OTHER;
}

static uint16_t mix565(carto_rgb a, carto_rgb b, int num, int den)
{
    const carto_rgb c = { (uint8_t)(a.r + (b.r - a.r) * num / den), (uint8_t)(a.g + (b.g - a.g) * num / den),
                          (uint8_t)(a.b + (b.b - a.b) * num / den) };
    return carto_rgb565(c);
}

EXT_RAM_BSS_ATTR static uint8_t s_krow[3][CELL_COLS_MAX * 3];

static void field_paint(const map_palette_t *pal, const uint16_t *px, uint16_t *out, int pw, int ph)
{
    field_keys();
    uint16_t ink[K_OTHER + 1];
    const uint16_t ground = carto_rgb565(pal->ground);
    ink[K_GROUND] = ground;
    ink[K_ROAD] = ground;
    ink[K_WATER] = (pal->fill & PF_WATER) ? carto_rgb565(pal->water) : ground;
    ink[K_PARK] = (pal->fill & PF_PARK) ? carto_rgb565(pal->park) : ground;
    ink[K_BUILDING] = (pal->fill & PF_BUILDING) ? carto_rgb565(pal->building) : ground;
    for (int p = CARTO_ROAD_PRIO_MIN; p < CARTO_ROAD_PRIO_MAX; p++)
        ink[K_ROAD + p] = p < pal->road_min ? ground
                        : mix565(pal->road_lo, pal->road_hi, p - CARTO_ROAD_PRIO_MIN,
                                 CARTO_ROAD_PRIO_MAX - 1 - CARTO_ROAD_PRIO_MIN);
    ink[K_MAJOR] = carto_rgb565(pal->major);
    const uint16_t edge = carto_rgb565(pal->edge);
    const bool edges = (pal->fill & (PF_EDGE_WATER | PF_EDGE_PARK)) != 0;
    if (pw > CELL_COLS_MAX * 3) pw = CELL_COLS_MAX * 3;

    /* Row y's names live in s_krow[y % 3]; the row below is named before
       the row is painted, over the one two above that is done with. */
    for (int x = 0; x < pw; x++) s_krow[0][x] = field_kind(px[x]);
    for (int y = 0; y < ph; y++) {
        const uint8_t *up = y > 0 ? s_krow[(y + 2) % 3] : NULL;
        const uint8_t *cur = s_krow[y % 3];
        uint8_t *down = s_krow[(y + 1) % 3];
        if (y + 1 < ph) for (int x = 0; x < pw; x++) down[x] = field_kind(px[(size_t)(y + 1) * pw + x]);
        uint16_t *o = out + (size_t)y * pw;
        for (int x = 0; x < pw; x++) {
            const uint8_t k = cur[x];
            uint16_t c = k == K_OTHER ? ink[K_BUILDING] : ink[k];
            if (edges && ((k == K_WATER && (pal->fill & PF_EDGE_WATER)) ||
                          (k == K_PARK && (pal->fill & PF_EDGE_PARK)))) {
                const bool rim = (x > 0 && cur[x - 1] != k) || (x + 1 < pw && cur[x + 1] != k) ||
                                 (up && up[x] != k) || (y + 1 < ph && down[x] != k);
                if (rim) c = k == K_WATER ? edge : mix565(pal->ground, pal->edge, 1, 2);
            }
            o[x] = c;
        }
    }
}

static int s_field_pal = -1;

static void draw_cells(tui_surface *sf, tui_rect a,
                       const uint16_t *px, int pw, int ph)
{
    if (a.w <= 0 || a.h <= 0 || !px) return;
    if (s_view == MAP_VIEW_FIELD) {
        const uint32_t serial = ls_map_render_serial();
        const map_palette_t *pal = palette();
        const int pal_i = s_palette_key;
        if (!s_field_pixels || s_field_w != pw || s_field_h != ph) {
            heap_caps_free(s_field_pixels);
            s_field_pixels = heap_caps_malloc((size_t)pw * ph * sizeof(uint16_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            s_field_w = pw; s_field_h = ph; s_field_valid = false;
        }
        if (s_field_pixels && (!s_field_valid || s_field_serial != serial || s_field_pal != pal_i)) {
            field_paint(pal, px, s_field_pixels, pw, ph);
            s_field_serial = serial; s_field_valid = true; s_field_pal = pal_i;
        }
        ls_tui_image(a, s_field_pixels ? s_field_pixels : px, pw, ph, serial ^ ((uint32_t)pal_i << 24));
        for (int y = a.y; y < a.y + a.h; y++)
            for (int x = a.x; x < a.x + a.w; x++)
                tui_put_char(sf, a, x, y, LS_TUI_IMAGE_CELL, 0);
        return;
    }
    ls_tui_image(tui_rect_make(0, 0, 0, 0), NULL, 0, 0, 0);
    if (!cells_fit(a.w, a.h)) {

        ls_panel_notice(sf, a, "MAP", "no memory to lay the map out in cells",
                        "close an app and come back");
        return;
    }

    const uint32_t serial = ls_map_render_serial();
    palette();
    const int cc_key = (int)s_view * 65536 + s_palette_key;
    if (!s_cc_valid || s_cc_serial != serial || s_cc_view != cc_key) {
        const int64_t t0 = esp_timer_get_time();
        build_cells(a, px, pw, ph);
        s_cells_us  = (uint32_t)(esp_timer_get_time() - t0);
        s_cc_serial = serial;
        s_cc_view   = cc_key;
        s_cc_valid  = true;
    }

    for (int cy = 0; cy < a.h; cy++) {
        const char    *g  = s_cc_glyph + (size_t)cy * s_cc_w;
        const uint8_t *at = s_cc_attr  + (size_t)cy * s_cc_w;
        for (int cx = 0; cx < a.w; cx++)
            if (g[cx]) tui_put_char(sf, a, a.x + cx, a.y + cy, g[cx], at[cx]);
    }
}

/* ------------------------------------------------------- reservations -- */

/* ONE reservation list for the whole overlay pass: symbols claim their
   cells first, then every label steps around every symbol and every other
   label. Cells are relative to the pane. PSRAM: only the draw path uses it. */
typedef struct { int16_t x0, x1, y; uint8_t hair, vair; } lbox;
#define OVERLAY_BOXES_MAX 1024
EXT_RAM_BSS_ATTR static lbox s_taken[OVERLAY_BOXES_MAX];
static int  s_ntaken;

/* How much room a name asks for. 0: a clear cell either side and a clear row
   above and below, so two names never read as one. 1: no clear row above and
   below. 2: only never on top of anything. A label tries every spot at one
   tier before it relaxes to the next, so it is dropped only when there is
   truly no free cell for it. */
static int s_tier;

static bool box_free(int x0, int x1, int y)
{
    for (int i = 0; i < s_ntaken; i++) {
        const int dy = s_taken[i].y - y;
        if (dy < -1 || dy > 1) continue;
        /* A name is not read next to another name, nor a symbol, without a
           cell of air; the row above and below counts for names only. */
        if (dy != 0 && (!s_taken[i].vair || s_tier > 0)) continue;
        const int hair = s_tier >= 2 ? 0 : s_taken[i].hair;
        if (x0 <= s_taken[i].x1 + hair && x1 >= s_taken[i].x0 - hair) return false;
    }
    return true;
}

static void box_take_air(int x0, int x1, int y, int hair, int vair)
{
    if (s_ntaken >= OVERLAY_BOXES_MAX) return;
    s_taken[s_ntaken].x0 = (int16_t)x0;
    s_taken[s_ntaken].x1 = (int16_t)x1;
    s_taken[s_ntaken].y  = (int16_t)y;
    s_taken[s_ntaken].hair = (uint8_t)hair;
    s_taken[s_ntaken].vair = (uint8_t)vair;
    s_ntaken++;
}

static void box_take(int x0, int x1, int y) { box_take_air(x0, x1, y, 1, 1); }

static void box_take_rect(tui_rect a, tui_rect r)
{
    if (r.h <= 0 || r.w <= 0) return;
    const int x0 = r.x - a.x, x1 = x0 + r.w - 1;
    for (int y = r.y - a.y; y < r.y - a.y + r.h; y++) box_take(x0, x1, y);
}

static bool in_rect(tui_rect r, int x, int y)
{
    return r.h > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

#ifdef LS_MAP_AUDIT
/* Host check: where every label went and which cells a symbol covers, so a
   test can say that no label sits on a symbol or on another label. */
#define AUDIT_LABELS_MAX 160
static struct { int16_t x, y, w; } s_audit_lab[AUDIT_LABELS_MAX];
static int s_audit_n;
static uint8_t s_audit_sym[160 * 100 / 8 + 1];
static int s_audit_w, s_audit_h;
static void audit_label(int x, int y, int w)
{
    if (s_audit_n < AUDIT_LABELS_MAX) {
        s_audit_lab[s_audit_n].x = (int16_t)x;
        s_audit_lab[s_audit_n].y = (int16_t)y;
        s_audit_lab[s_audit_n].w = (int16_t)w;
        s_audit_n++;
    }
}
int ls_map_label_audit(int *labels)
{
    int bad = 0;
    for (int i = 0; i < s_audit_n; i++) {
        for (int j = i + 1; j < s_audit_n; j++)
            if (s_audit_lab[i].y == s_audit_lab[j].y &&
                s_audit_lab[i].x < s_audit_lab[j].x + s_audit_lab[j].w &&
                s_audit_lab[j].x < s_audit_lab[i].x + s_audit_lab[i].w) bad++;
        for (int x = s_audit_lab[i].x; x < s_audit_lab[i].x + s_audit_lab[i].w; x++) {
            if (x < 0 || x >= s_audit_w || s_audit_lab[i].y < 0 || s_audit_lab[i].y >= s_audit_h) continue;
            const int k = s_audit_lab[i].y * s_audit_w + x;
            if (s_audit_sym[k >> 3] & (1u << (k & 7))) bad++;
        }
    }
    if (labels) *labels = s_audit_n;
    return bad;
}
#endif

/* Every cell the canvas holds a symbol in (marks, nodes, aircraft, the
   receiver and above: lines, trails and rings sit lower) is taken, exactly
   as drawn, whatever drew it. A cell of air either side, none above and
   below: a label may sit right over a symbol's top or bottom row. */
static void claim_ink(tui_rect a)
{
#ifdef LS_MAP_AUDIT
    memset(s_audit_sym, 0, sizeof(s_audit_sym));
    s_audit_w = a.w <= 160 ? a.w : 160;
    s_audit_h = a.h <= 100 ? a.h : 100;
#endif
    for (int y = 0; y < a.h; y++) {
        int run = -1;
        for (int x = 0; x <= a.w; x++) {
            const bool on = x < a.w && ls_ink_cell_prio(x, y) >= 5;
#ifdef LS_MAP_AUDIT
            if (on && x < s_audit_w && y < s_audit_h) {
                const int k = y * s_audit_w + x;
                s_audit_sym[k >> 3] |= (uint8_t)(1u << (k & 7));
            }
#endif
            if (on && run < 0) run = x;
            if (!on && run >= 0) { box_take_air(run, x - 1, y, 1, 0); run = -1; }
        }
    }
}

/* The pad and the card are already-taken ground. */
static void overlay_reset(tui_rect a, tui_rect pad, tui_rect card)
{
    s_ntaken = 0;
#ifdef LS_MAP_AUDIT
    s_audit_n = 0;
#endif
    box_take_rect(a, pad);
    box_take_rect(a, card);
}

static EXT_RAM_BSS_ATTR map_label_history s_label_history;
static bool label_slot_free(void *context,int x,int y,int w,int rows) {
    const tui_rect *a=context;
    if(x<0 || y<0 || x+w>a->w || y+rows>a->h) return false;
    for(int r=0;r<rows;r++) if(!box_free(x,x+w-1,y+r)) return false;
    return true;
}

/* Live labels may relax padding, but never overlap an occupied cell.
 * Keep hysteresis in one choose call so a failed padded slot does not start
 * the hold timer before the unpadded fallback has been considered. */
static bool live_label_slot_free(void *context,int x,int y,int w,int rows) {
    s_tier=1;
    if(label_slot_free(context,x,y,w,rows)) return true;
    s_tier=2;
    bool free=label_slot_free(context,x,y,w,rows);
    s_tier=1;
    return free;
}

static bool basemap_label_slot_free(void *context,int x,int y,int w,int rows) {
    if(!label_slot_free(context,x,y,w,rows)) return false;
    for(int r=0;r<rows;r++) for(int c=0;c<w;c++)
        if(ls_ink_cell_used(x+c,y+r)) return false;
    return true;
}

/* ---------------------------------------------------------- place names -- */

#define LABELS_DRAWN_MAX 16
static int map_label_cap(tui_rect a) {
    int z=ls_map_zoom();int cap=z<=10?6:z<=13?10:16;
    int area_cap=a.w*a.h/(s_basemap_busy?180:120);
    if(cap>area_cap) cap=area_cap;
    return cap<2?2:cap;
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

    const uint8_t attr = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    const uint8_t dot  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);

    int drawn = 0;
    for (int k = 0; k < m && drawn < map_label_cap(a); k++) {
        const carto_label *lb = &L[order[k]];

        const int cx = lb->x / sx;
        const int cy = lb->y / sy;
        if (cx < 0 || cy < 0 || cx >= a.w || cy >= a.h) continue;
        if (in_rect(avoid, a.x + cx, a.y + cy)) continue;
        s_tier = 1;
        const bool dot_free = box_free(cx, cx, cy) && !ls_ink_cell_used(cx,cy);
        s_tier = 0;
        if (!dot_free) continue;

        /* Names in the tiles are UTF-8; everything below measures and cuts
           by bytes, so it works on the name folded to one byte a cell. */
        char text[CARTO_LABEL_MAX_TEXT];
        snprintf(text, sizeof(text), "%s", lb->text);
        tui_utf8_fold_str(text);
        int len = (int)strlen(text);
        const int room = a.w / (s_basemap_busy?4:3);
        if (len > room) {
            len = room;
            while (len > 4 && text[len] != ' ' && text[len - 1] != ' ')
                len--;
            while (len > 1 && text[len - 1] == ' ') len--;
        }
        if (len > a.w) continue;

        /* Under the dot when there is room, then over it, then beside it. */
        int top = cx - len / 2;
        if (top < 0) top = 0;
        if (top + len > a.w) top = a.w - len;
        const int spot[6][2] = { { top, cy + 1 }, { top, cy - 1 }, { cx + 2, cy },
                                 { cx - 1 - len, cy }, { top, cy + 2 }, { top, cy - 2 } };
        int x0=0,y=0;s_tier=1;
        bool placed=map_label_choose(&s_label_history,map_label_key(text,1+s_view),
            cx,cy,len,1,spot,2,6,esp_timer_get_time(),basemap_label_slot_free,&a,&x0,&y);
        s_tier=0;if(!placed) continue;

        glass(sf, a, a.x + cx, a.y + cy, '.', dot);
        box_take(cx, cx, cy);
        ls_ink_text(sf, a, a.x + x0, a.y + y, text, len, attr);
        box_take(x0, x0 + len - 1, y);
#ifdef LS_MAP_AUDIT
        audit_label(x0, y, len);
#endif
        drawn++;
    }
}

/* CartoCore publishes names separately from its clean terrain frame. Keep the
 * marker/text reservations, relocate whole names, then protect them from ring
 * scales and the centre reticle. No glass: spaces also get a solid dark plate. */
static EXT_RAM_BSS_ATTR ls_carto_label s_carto_labels[LS_CARTO_LABEL_MAX];
#ifdef LS_MAP_AUDIT
int ls_map_carto_low_masked(void) { return s_carto_low_masked; }
int ls_map_carto_label_audit(void) { return s_carto_audit_bad; }
static int carto_audit_cells(const tui_surface *sf) {
    int bad=0;
    for(size_t i=0;i<s_carto_audit_count;i++) {
        const ls_carto_label *l=&s_carto_labels[i];
        for(int k=0;l->text[k];k++) {
            tui_cell c=sf->back[l->y*sf->w+l->x+k];
            if(c.ch!=l->text[k] || c.attr!=l->attr || TUI_ATTR_BG(c.attr)!=TUI_BLACK ||
               !(TUI_ATTR_FG(c.attr)&TUI_BRIGHT)) bad++;
        }
    }
    return bad;
}
#endif
static void draw_carto_labels(tui_surface *sf,tui_rect a) {
    size_t n=ls_carto_map_labels(s_carto_labels,LS_CARTO_LABEL_MAX,a.w,a.h);
#ifdef LS_MAP_AUDIT
    s_carto_audit_count=n;
#endif
    /* Engine classes 1/2 are places, 3 water, 4/5 roads. Stable ordering
     * keeps place names ahead of minor labels regardless of scanline order. */
    int order[LS_CARTO_LABEL_MAX];
    for(size_t i=0;i<n;i++) {
        int j=i;while(j>0 && s_carto_labels[order[j-1]].label_class>s_carto_labels[i].label_class) {
            order[j]=order[j-1];j--;
        }
        order[j]=i;
    }
    int drawn=0,minor=0,cap=map_label_cap(a);
    for(size_t i=0;i<n;i++) {
        ls_carto_label *l=&s_carto_labels[order[i]];
        int len=(int)strlen(l->text),x=0,y=0;
        if(drawn>=cap || (l->label_class>=3 && minor>=(s_basemap_busy?2:4))) { l->text[0]=0;continue; }
        int room=a.w/(s_basemap_busy?3:2);
        if(len>room) { len=room;while(len>4 && l->text[len]!=' ') len--;l->text[len]=0; }
        int spots[9][2]={{l->x,l->y},{l->x,l->y+1},{l->x,l->y-1},
            {l->x,l->y+2},{l->x,l->y-2},{l->x+2,l->y},{l->x-2,l->y},
            {l->x+1,l->y+1},{l->x-1,l->y-1}};
        s_tier=1;
        bool placed=map_label_choose(&s_label_history,map_label_key(l->text,1+s_view),
            l->x,l->y,len,1,spots,2,9,esp_timer_get_time(),basemap_label_slot_free,&a,&x,&y);
        s_tier=0;if(!placed) { l->text[0]=0;continue; }
        l->attr=TUI_ATTR(TUI_WHITE|TUI_BRIGHT,TUI_BLACK);
        for(int k=0;k<len;k++) {
#ifdef LS_MAP_AUDIT
            int prio=ls_ink_cell_prio(x+k,y);if(prio>0 && prio<5) s_carto_low_masked++;
#endif
            tui_put_char(sf,a,a.x+x+k,a.y+y,l->text[k],l->attr);
        }
        box_take(x,x+len-1,y);drawn++;if(l->label_class>=3) minor++;
#ifdef LS_MAP_AUDIT
        audit_label(x,y,len);l->x=a.x+x;l->y=a.y+y;
#endif
    }
}

/* ----------------------------------------------------------- projection -- */

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

static double world_px(void)
{
    const int tp = ls_map_tile_px() > 0 ? ls_map_tile_px() : 256;
    return (double)(cartocore_view()?256:tp) * ldexp(1.0, ls_map_zoom());
}

/* Where a coordinate lands in the rendered frame, in frame pixels, on or
   off the frame. False only for a coordinate that is not on the map. */
static bool frame_px(double lat, double lon, int pw, int ph, double *fx, double *fy)
{
    if (!isfinite(lat) || !isfinite(lon) || fabs(lat) > 85 || fabs(lon) > 180) return false;
    double clat = 0, clon = 0;
    ls_map_get_center(&clat, &clon);
    const double world = world_px();
    double delta = (lon - clon) / 360.0;
    if (delta > 0.5) delta -= 1;
    if (delta < -0.5) delta += 1;
    *fx = pw / 2.0 + delta * world;
    *fy = ph / 2.0 + (merc_y(lat) - merc_y(clat)) * world;
    return true;
}

/* The other way: a frame pixel back to a coordinate. */
static void frame_ll(double fx, double fy, int pw, int ph, double *lat, double *lon)
{
    double clat = 0, clon = 0;
    ls_map_get_center(&clat, &clon);
    const double world = world_px();
    double nx = (clon + 180.0) / 360.0 + (fx - pw / 2.0) / world;
    double ny = merc_y(clat) + (fy - ph / 2.0) / world;
    nx -= floor(nx);
    if (ny < 0) ny = 0;
    if (ny > 1) ny = 1;
    *lon = nx * 360.0 - 180.0;
    *lat = atan(sinh(M_PI * (1.0 - 2.0 * ny))) * 180.0 / M_PI;
}

/* Metres of ground per frame pixel at the centre. */
static double frame_mpp(void)
{
    double lat = 0, lon = 0;
    ls_map_get_center(&lat, &lon);
    const int tp = cartocore_view()?256:ls_map_tile_px();
    return 156543.03392 * cos(lat * M_PI / 180.0) / ldexp(1.0, ls_map_zoom())
           * (256.0 / (double)(tp > 0 ? tp : 256));
}

/* Frame pixels to canvas dots: two across and three down per cell. */
static void px_dot(double fx, double fy, int *dx, int *dy)
{
    *dx = (int)floor(fx * 2.0 / SUB_X);
    *dy = (int)floor(fy * 3.0 / sub_y());
}

/* Where a coordinate lands in the rendered frame, in cells of `a`. False when
   it is off the pane. */
static bool map_cell_of(double lat, double lon, tui_rect a, int pw, int ph,
                        int *cx, int *cy)
{
    double fx, fy;
    if (!frame_px(lat, lon, pw, ph, &fx, &fy)) return false;
    if (fx < 0 || fy < 0 || fx >= pw || fy >= ph) return false;
    *cx = (int)(fx / SUB_X);
    *cy = (int)(fy / sub_y());
    return (*cx >= 0 && *cy >= 0 && *cx < a.w && *cy < a.h);
}

/* Where this receiver is: a fresh fix, or the saved home. */
static bool receiver_at(double *lat, double *lon, bool *live)
{
    if (s_receiver_fresh) {
        *lat = s_receiver.lat_deg; *lon = s_receiver.lon_deg;
        if (live) *live = true;
        return true;
    }
    float hl, ho;
    if (settings_get_home(&hl, &ho) && fabs(hl) <= 85 && fabs(ho) <= 180) {
        *lat = hl; *lon = ho;
        if (live) *live = false;
        return true;
    }
    return false;
}

static void refresh_receiver(int64_t now)
{
    ls_gps_get(&s_receiver);
    s_receiver_fresh = s_receiver.fix && s_receiver.last_fix_us > 0 &&
        now >= s_receiver.last_fix_us && now - s_receiver.last_fix_us <= 10000000 &&
        isfinite(s_receiver.lat_deg) && fabs(s_receiver.lat_deg) <= 85 &&
        isfinite(s_receiver.lon_deg) && fabs(s_receiver.lon_deg) <= 180;
}

/* Range and bearing in the words the card and the lists use. */
static void range_words(char *out, size_t cap, double lat0, double lon0, double lat1, double lon1)
{
    double brg = 0, m = 0;
    ls_geo_bearing_range(lat0, lon0, lat1, lon1, &brg, &m);
    const double nm = m / LS_GEO_M_PER_NM;
    if (m < 50) snprintf(out, cap, "here");
    else if (nm < 10) snprintf(out, cap, "%.1fnm %s %03d", nm, ls_geo_compass(brg), (int)lround(brg) % 360);
    else snprintf(out, cap, "%.0fnm %s %03d", nm, ls_geo_compass(brg), (int)lround(brg) % 360);
}

/* ------------------------------------------------------------ selection -- */

typedef enum { SEL_NONE = 0, SEL_AIR, SEL_NODE, SEL_MARK, SEL_LINE } sel_kind_t;

static sel_kind_t s_sel;
static uint32_t   s_sel_icao;
static char       s_sel_node[17];
static int        s_sel_index;          /* marker or line */
static int64_t    s_lock_us;            /* when the lock-on began */
static bool       s_track;              /* keep the selection in view */
static bool       s_card_hidden;

static void select_none(void)
{
    s_sel = SEL_NONE;
    s_track = false;
}

static void select_air(uint32_t icao)
{
    s_sel = SEL_AIR;
    s_sel_icao = icao;
    adsb_select_set_icao(icao);
    s_lock_us = esp_timer_get_time();
}

static void select_node(const char *id)
{
    s_sel = SEL_NODE;
    snprintf(s_sel_node, sizeof(s_sel_node), "%s", id);
    s_lock_us = esp_timer_get_time();
}

static void select_mark(sel_kind_t kind, int index)
{
    s_sel = kind;
    s_sel_index = index;
    s_lock_us = esp_timer_get_time();
}

/* Go there: centre on it, and start the lock-on so the eye finds it. */
static void jump_to(double lat, double lon)
{
    ls_map_follow_set(false);
    ls_map_center(lat, lon);
    s_lock_us = esp_timer_get_time();
}

/* What the last frame drew and where, so a tap selects what is on the
   glass. Cells are absolute. */
typedef enum { HIT_AIR = 1, HIT_NODE, HIT_MARK, HIT_LINE, HIT_EDGE = 0x80 } hit_kind_t;
typedef struct { int16_t x, y, x1, y1; uint8_t kind; uint32_t id; double lat, lon; } hit_t;
#define HITS_MAX 96
EXT_RAM_BSS_ATTR static hit_t s_hits[HITS_MAX];
static int s_nhits;

static void hit_add(int x, int y, uint8_t kind, uint32_t id, double lat, double lon)
{
    if (s_nhits >= HITS_MAX) return;
    s_hits[s_nhits++] = (hit_t){ (int16_t)x, (int16_t)y, (int16_t)x, (int16_t)y, kind, id, lat, lon };
}

/* Where the last label went, absolute cells, so the label itself can be
   tapped: a name is a bigger target than the shape beside it. */
static tui_rect s_label_rect;

static void hit_label(uint8_t kind, uint32_t id, double lat, double lon)
{
    if (s_label_rect.h <= 0 || s_nhits >= HITS_MAX) return;
    const tui_rect r = s_label_rect;
    s_hits[s_nhits++] = (hit_t){ (int16_t)r.x, (int16_t)r.y, (int16_t)(r.x + r.w - 1),
                                 (int16_t)(r.y + r.h - 1), kind, id, lat, lon };
}

/* ------------------------------------------------------------- symbols -- */

/* Aircraft are plain shapes: an arrowhead along the track for anything
   with wings, and a ring, a diamond or a square for the few that are not.
   A shape is a filled outline in dot units, nose to the north, turned to
   the heading and sampled back onto the dot grid four times a dot. Only the
   right half is written; the left is its mirror. */
typedef struct { float x, y; } pt_t;

static const pt_t SHAPE_ARROW[] = {
    { 0.0f, -3.4f }, { 2.8f, 2.8f }, { 0.0f, 1.2f },
};
static const pt_t SHAPE_DIAMOND[] = {
    { 0.0f, -2.6f }, { 2.0f, 0.0f }, { 0.0f, 2.6f },
};
static const pt_t SHAPE_SQUARE[] = {
    { 0.0f, -2.0f }, { 2.0f, -2.0f }, { 2.0f, 2.0f }, { 0.0f, 2.0f },
};

static void silhouette(int cx, int cy, const pt_t *half, int n, double deg, double scale,
                       uint8_t attr, uint8_t prio)
{
    int cw = 10, ch = 17;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    /* A dot is cw/2 wide and ch/3 tall; the outline is in dot widths.
       Single precision throughout: this part has no double FPU. */
    const float asp = (ch > 0 && cw > 0) ? (ch / 3.0f) / (cw / 2.0f) : 1.1f;
    const float r = (float)deg * 0.017453293f, c = cosf(r), sn = sinf(r);
    const float sc = (float)scale;
    float ox[16], oy[16];
    const int total = 2 * n < 16 ? 2 * n : 16;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int i = 0; i < total; i++) {
        const pt_t p = i < n ? half[i] : (pt_t){ -half[total - 1 - i].x, half[total - 1 - i].y };
        const float mx = p.x * sc, my = p.y * sc;
        ox[i] = mx * c - my * sn;
        oy[i] = (mx * sn + my * c) / asp;
        if (ox[i] < x0) x0 = ox[i];
        if (ox[i] > x1) x1 = ox[i];
        if (oy[i] < y0) y0 = oy[i];
        if (oy[i] > y1) y1 = oy[i];
    }
    for (int j = (int)floorf(y0); j <= (int)ceilf(y1); j++)
        for (int i = (int)floorf(x0); i <= (int)ceilf(x1); i++) {
            int hits = 0;
            for (int k = 0; k < 4; k++) {
                const float x = i + ((k & 1) ? 0.25f : -0.25f);
                const float y = j + ((k & 2) ? 0.25f : -0.25f);
                bool in = false;
                for (int a = 0, b = total - 1; a < total; b = a++)
                    if ((oy[a] > y) != (oy[b] > y) &&
                        x < (ox[b] - ox[a]) * (y - oy[a]) / (oy[b] - oy[a]) + ox[a]) in = !in;
                if (in) hits++;
            }
            if (hits >= 2) ls_ink_dot(cx + i, cy + j, attr, prio);
        }
}

typedef enum { SYM_KIND_JET, SYM_KIND_HEAVY, SYM_KIND_LIGHT, SYM_KIND_ROTOR,
               SYM_KIND_BALLOON, SYM_KIND_UAV, SYM_KIND_VEHICLE } sym_kind_t;

static sym_kind_t sym_for(const adsb_aircraft_t *ac)
{
    const int tc = ac->emitter_tc, ca = ac->emitter_ca;
    if (tc == 4) {
        if (ca == 1 || ca == 2) return SYM_KIND_LIGHT;
        if (ca == 4 || ca == 5) return SYM_KIND_HEAVY;
        if (ca == 7) return SYM_KIND_ROTOR;
        return SYM_KIND_JET;
    }
    if (tc == 3) {
        if (ca == 1 || ca == 4) return SYM_KIND_LIGHT;
        if (ca == 2 || ca == 3) return SYM_KIND_BALLOON;
        if (ca == 6) return SYM_KIND_UAV;
        return SYM_KIND_JET;
    }
    if (tc == 2 && ca >= 1 && ca <= 3) return SYM_KIND_VEHICLE;
    return SYM_KIND_JET;
}

#define SIL(a) (a), (int)(sizeof(a) / sizeof((a)[0]))

/* One aircraft centred on dot (cx, cy); `size` 1 is the normal arrowhead,
   smaller for a wide or crowded view. */
static void air_symbol(int cx, int cy, sym_kind_t k, int heading, uint8_t attr, uint8_t prio,
                       double size)
{
    switch (k) {
    case SYM_KIND_HEAVY: silhouette(cx, cy, SIL(SHAPE_ARROW), heading, 1.25 * size, attr, prio); break;
    case SYM_KIND_LIGHT: silhouette(cx, cy, SIL(SHAPE_ARROW), heading, 0.8 * size, attr, prio); break;
    case SYM_KIND_UAV:   silhouette(cx, cy, SIL(SHAPE_DIAMOND), 0, size, attr, prio); break;
    case SYM_KIND_VEHICLE: silhouette(cx, cy, SIL(SHAPE_SQUARE), 0, size, attr, prio); break;
    case SYM_KIND_BALLOON:
        ls_ink_ellipse(cx, cy, 2, 2, attr, prio, 0);
        break;
    case SYM_KIND_ROTOR: {
        /* A ring with a tick for the way it is going. */
        const float r = heading * 0.017453293f;
        ls_ink_ellipse(cx, cy, 2, 2, attr, prio, 0);
        ls_ink_dot(cx, cy, attr, prio);
        ls_ink_line(cx, cy, cx + (int)lrintf(4.0f * sinf(r)), cy - (int)lrintf(3.6f * cosf(r)), attr, prio, 0);
        break;
    }
    default: silhouette(cx, cy, SIL(SHAPE_ARROW), heading, size, attr, prio); break;
    }
}

/* Claim the cells a symbol of radius r dots covers, so no label lands on it. */
static void claim_dots(int cx, int cy, int r, int aw, int ah)
{
    int x0 = (cx - r) / 2, x1 = (cx + r) / 2;
    int y0 = (cy - r) / 3, y1 = (cy + r) / 3;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= aw) x1 = aw - 1;
    if (y1 >= ah) y1 = ah - 1;
    for (int y = y0; y <= y1; y++) box_take_air(x0, x1, y, 1, 0);
}

/* A ring that grows outward from a fresh report, then starts again. A
   report, not an accuracy: it says "just heard" and nothing about where. */
static void ping_ring(int cx, int cy, uint8_t attr, int period_ms)
{
    int cw = 10, ch = 17;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    const int step = ls_motion_phase(5, period_ms);
    const double r_px = (1.5 + step) * (cw > 0 ? cw : 10);
    const int rx = (int)lround(r_px / ((cw > 0 ? cw : 10) / 2.0));
    const int ry = (int)lround(r_px / ((ch > 0 ? ch : 17) / 3.0));
    ls_ink_ellipse(cx, cy, rx, ry, attr, 1, step < 2 ? -2 : -3);
}

/* Corner brackets closing in on a target: wide and fast at the moment of
   selection, then a steady frame. */
static void lock_brackets(int cx, int cy, uint8_t attr, int64_t now)
{
    double t = (double)(now - s_lock_us) / 700000.0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    const double ease = 1.0 - (1.0 - t) * (1.0 - t);
    const int rx = (int)lround(5 + 12 * (1.0 - ease));
    const int ry = (int)lround(5 + 12 * (1.0 - ease));
    const int arm = 3;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2) {
            const int x = cx + sx * rx, y = cy + sy * ry;
            ls_ink_line(x, y, x - sx * arm, y, attr, 9, 0);
            ls_ink_line(x, y, x, y - sy * arm, attr, 9, 0);
        }
}

/* Where full size symbols already are, this frame: aircraft and nodes
   both, so one never sits full size on the other. */
#define PLACED_MAX (ADSB_MAX_TRACKED + LS_MESH_MAX_PEERS)
static int16_t s_placed_x[PLACED_MAX], s_placed_y[PLACED_MAX];
static int     s_nplaced;

static bool crowded_at(int dx, int dy)
{
    for (int k = 0; k < s_nplaced; k++)
        if (abs(s_placed_x[k] - dx) < 8 && abs(s_placed_y[k] - dy) < 7) return true;
    if (s_nplaced < PLACED_MAX) { s_placed_x[s_nplaced] = (int16_t)dx; s_placed_y[s_nplaced] = (int16_t)dy; s_nplaced++; }
    return false;
}

/* ------------------------------------------------------------- aircraft -- */

#define AIRCRAFT_SHOW_US   LS_MAP_AIR_SHOW_US
#define AIRCRAFT_FRESH_US   (30 * 1000000LL)
#define AIRCRAFT_RECKON_US LS_MAP_AIR_RECKON_US

typedef struct {
    bool     on;             /* has a position worth drawing */
    bool     inside;         /* on the pane */
    int      slot;
    double   lat, lon;       /* where it is drawn, reckoned forward */
    double   fx, fy;         /* frame pixels */
    int      dx, dy;         /* canvas dots */
    uint8_t  hue;
    bool     fresh, emergency;
    bool     compact;        /* drawn as the small arrowhead */
    int      squawk;
} air_view_t;

EXT_RAM_BSS_ATTR static air_view_t     s_air[ADSB_MAX_TRACKED];
EXT_RAM_BSS_ATTR static ls_map_glide_t s_air_glide[ADSB_MAX_TRACKED];
EXT_RAM_BSS_ATTR static adsb_trail_pt_t s_trail[ADSB_TRAIL_N];

/* A trail's dots, kept until the trail or the view changes: projecting
   every point every frame was most of a frame's time on this part. */
typedef struct {
    uint32_t icao;
    int      n, pw, ph, zoom;
    int64_t  newest;
    double   clat, clon;
    int16_t  x[ADSB_TRAIL_N], y[ADSB_TRAIL_N];
    int64_t  ts[ADSB_TRAIL_N];
} trail_dots_t;
EXT_RAM_BSS_ATTR static trail_dots_t s_trail_dots[ADSB_MAX_TRACKED];
EXT_RAM_BSS_ATTR static int s_trail_kept[ADSB_MAX_TRACKED];

static const trail_dots_t *trail_dots(int slot, uint32_t icao, int pw, int ph)
{
    trail_dots_t *t = &s_trail_dots[slot];
    const int n = adsb_state_trail(slot, s_trail, ADSB_TRAIL_N);
    double clat, clon;
    ls_map_get_center(&clat, &clon);
    const int64_t newest = n ? s_trail[n - 1].ts_us : 0;
    if (t->icao == icao && t->n == n && t->newest == newest && t->pw == pw && t->ph == ph &&
        t->zoom == ls_map_zoom() && t->clat == clat && t->clon == clon)
        return t;
    t->icao = icao; t->n = 0; t->newest = newest; t->pw = pw; t->ph = ph;
    t->zoom = ls_map_zoom(); t->clat = clat; t->clon = clon;
    for (int i = 0; i < n; i++) {
        double fx, fy;
        if (!frame_px(s_trail[i].lat, s_trail[i].lon, pw, ph, &fx, &fy)) continue;
        int qx, qy;
        px_dot(fx, fy, &qx, &qy);
        if (qx < -32000 || qx > 32000 || qy < -32000 || qy > 32000) continue;
        t->x[t->n] = (int16_t)qx; t->y[t->n] = (int16_t)qy; t->ts[t->n] = s_trail[i].ts_us;
        t->n++;
    }
    /* Keyed on the input count, so an unchanged trail is recognised even
       when a point fell off the pane. */
    const int kept = t->n;
    t->n = n;
    t->x[0] = kept > 0 ? t->x[0] : 0;
    t->newest = newest;
    s_trail_kept[slot] = kept;
    return t;
}

static uint8_t air_band(int altitude)
{
    return altitude < 5000 ? TUI_YELLOW : altitude < 20000 ? TUI_GREEN : TUI_CYAN;
}

static bool squawk_emergency(int sq) { return sq == 7500 || sq == 7600 || sq == 7700; }

/* The speed leader's reach, the same for every aircraft in view so their
   lengths compare: the time a 300 kt aircraft takes to cross about six
   cells, rounded to a time worth reading. */
static int s_vector_s;

static int vector_seconds(void)
{
    const double mpp = frame_mpp();
    if (!(mpp > 0)) return 60;
    const double px_per_s = 300.0 * LS_GEO_M_PER_NM / 3600.0 / mpp;
    const double want = 6.0 * SUB_X / px_per_s;
    static const int NICE[] = { 10, 15, 30, 60, 120, 180, 300, 600, 900 };
    int best = NICE[0];
    for (unsigned i = 0; i < sizeof(NICE) / sizeof(NICE[0]); i++)
        if (fabs(NICE[i] - want) < fabs(best - want)) best = NICE[i];
    return best;
}

static void air_prepare(tui_rect a, int pw, int ph, int64_t now)
{
    const uint32_t sel = s_sel == SEL_AIR ? s_sel_icao : 0;
    for (int slot = 0; slot < ADSB_MAX_TRACKED; slot++) {
        air_view_t *v = &s_air[slot];
        v->on = v->inside = false;
        const adsb_aircraft_t *ac = adsb_state_get(slot);
        if (!ac || !ac->active || !ac->pos_valid || ac->pos_ts_us <= 0) continue;
        const int64_t age = now - ac->pos_ts_us;
        if (age < 0 || age > AIRCRAFT_SHOW_US) continue;

        double lat = ac->lat, lon = ac->lon;
        int ns = ac->ns_velocity, ew = ac->ew_velocity;
        if (!ns && !ew && ac->velocity > 0) {
            ns = (int)lround(ac->velocity * cos(ac->heading * M_PI / 180.0));
            ew = (int)lround(ac->velocity * sin(ac->heading * M_PI / 180.0));
        }
        double rlat, rlon;
        ls_map_dead_reckon(lat, lon, ns, ew, ac->pos_ts_us, now, AIRCRAFT_RECKON_US, &rlat, &rlon);
        ls_map_glide(&s_air_glide[slot], ac->icao, ac->pos_ts_us, rlat, rlon, now, &v->lat, &v->lon);

        if (!frame_px(v->lat, v->lon, pw, ph, &v->fx, &v->fy)) continue;
        v->on = true;
        v->slot = slot;
        v->inside = v->fx >= 0 && v->fy >= 0 && v->fx < pw && v->fy < ph;
        px_dot(v->fx, v->fy, &v->dx, &v->dy);
        v->fresh = age <= AIRCRAFT_FRESH_US;
        v->squawk = adsb_state_squawk(slot);
        v->emergency = squawk_emergency(v->squawk);
        const uint8_t band = air_band(ac->altitude);
        v->hue = v->emergency ? (uint8_t)(TUI_RED | TUI_BRIGHT)
               : ac->icao == sel ? (uint8_t)(TUI_WHITE | TUI_BRIGHT)
               : v->fresh ? (uint8_t)(band | TUI_BRIGHT) : LS_DIM_FG;
    }
    (void)a;
}

static void air_canvas(tui_rect a, int pw, int ph, int64_t now)
{
    if (!layer(L_AIR)) return;
    const uint32_t sel = s_sel == SEL_AIR ? s_sel_icao : 0;
    const int vec_s = s_vector_s;

    /* Trails and leaders first, so the symbols sit on them. */
    for (int slot = 0; slot < ADSB_MAX_TRACKED; slot++) {
        const air_view_t *v = &s_air[slot];
        if (!v->on) continue;
        const adsb_aircraft_t *ac = adsb_state_get(slot);
        if (layer(L_TRAILS) || ac->icao == sel) {
            const trail_dots_t *t = trail_dots(slot, ac->icao, pw, ph);
            const int n = s_trail_kept[slot];
            int px = v->dx, py = v->dy;
            /* Newest to oldest, from the symbol backwards; the last minute
               bright, older track dim. */
            for (int i = n - 1; i >= 0; i--) {
                const int qx = t->x[i], qy = t->y[i];
                const bool recent = now - t->ts[i] < 60000000LL;
                const uint8_t hue = v->emergency ? TUI_RED|TUI_BRIGHT : TUI_YELLOW|TUI_BRIGHT;
                ls_ink_line(px, py, qx, qy, TUI_ATTR(hue, TUI_BLACK), 3, recent ? 0 : 1);
                px = qx; py = qy;
            }
        }
        if (layer(L_VECTORS) && v->fresh && ac->velocity > 30 && vec_s > 0) {
            int ns = ac->ns_velocity, ew = ac->ew_velocity;
            if (!ns && !ew) {
                ns = (int)lround(ac->velocity * cos(ac->heading * M_PI / 180.0));
                ew = (int)lround(ac->velocity * sin(ac->heading * M_PI / 180.0));
            }
            double lat2, lon2, fx, fy;
            ls_map_dead_reckon(v->lat, v->lon, ns, ew, 0, (int64_t)vec_s * 1000000LL,
                               (int64_t)vec_s * 1000000LL, &lat2, &lon2);
            if (frame_px(lat2, lon2, pw, ph, &fx, &fy)) {
                int qx, qy;
                px_dot(fx, fy, &qx, &qy);
                /* Never longer than twelve cells, whatever the speed says. */
                const double len = hypot(qx - v->dx, qy - v->dy), cap = 24.0;
                if (len > cap) {
                    qx = v->dx + (int)lround((qx - v->dx) * cap / len);
                    qy = v->dy + (int)lround((qy - v->dy) * cap / len);
                }
                ls_ink_line(v->dx, v->dy, qx, qy, radar_attr(), 3, 2);
            }
        }
    }

    /* Full symbols only where there is room for them: zoomed in far enough
       that aircraft are apart, and not on top of one already drawn. The
       selection is always full size. */
    const bool wide = ls_map_zoom() < 10;
    for (int pass = 0; pass < 2; pass++) {
        for (int slot = 0; slot < ADSB_MAX_TRACKED; slot++) {
            air_view_t *v = &s_air[slot];
            if (!v->on || !v->inside) continue;
            const adsb_aircraft_t *ac = adsb_state_get(slot);
            /* The selection last, so it is on top of anything it crosses. */
            if ((ac->icao == sel) != (pass == 1)) continue;
            v->compact = ac->icao != sel && (wide || crowded_at(v->dx, v->dy));
            const bool blink_off = v->emergency && ls_motion_phase(2, 500) == 1;
            uint8_t hue = blink_off ? (uint8_t)TUI_RED : v->hue;
            /* A ring for a contact just heard for the first time, and for an
               emergency; a report every half second is not news. */
            if (!v->emergency && now - ac->first_seen_us < 10000000)
                ping_ring(v->dx, v->dy, TUI_ATTR(air_band(ac->altitude), TUI_BLACK), 1500);
            if (v->emergency) ping_ring(v->dx, v->dy, TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK), 900);
            air_symbol(v->dx, v->dy, sym_for(ac), ac->heading, TUI_ATTR(hue, TUI_BLACK),
                       ac->icao == sel ? 8 : 7, v->compact ? 0.75 : 1.0);
            claim_dots(v->dx, v->dy, v->compact ? 2 : 4, a.w, a.h);
            if (ac->icao == sel) lock_brackets(v->dx, v->dy, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK), now);
            hit_add(a.x + v->dx / 2, a.y + v->dy / 3, HIT_AIR, ac->icao, v->lat, v->lon);
        }
    }
}

/* The data block beside an aircraft: callsign, then altitude in hundreds
   of feet with a climb mark and speed in knots - "340^460" - the way a
   controller's screen writes it. */
static void air_block_lines(const adsb_aircraft_t *ac, const air_view_t *v, char *l1, char *l2, size_t cap)
{
    if (ac->callsign[0]) snprintf(l1, cap, "%.8s", ac->callsign);
    else snprintf(l1, cap, "%06lX", (unsigned long)ac->icao);
    if (v->emergency) { snprintf(l2, cap, "SQ%04d", v->squawk); return; }
    const char climb = ac->vert_rate > 300 ? '^' : ac->vert_rate < -300 ? 'v' : '-';
    int hundreds = (ac->altitude + 50) / 100;
    if (hundreds < 0) hundreds = 0;
    if (hundreds > 999) hundreds = 999;
    if (ac->velocity > 0) snprintf(l2, cap, "%03d%c%d", hundreds, climb, ac->velocity);
    else snprintf(l2, cap, "%03d%c", hundreds, climb);
}

/* Where a one or two line label of w cells could go around a symbol at cell
   (cx, cy): right, left, above, below, nudged a row up or down, and each a
   few cells further out. spot[i][2] is how far the label's nearest cell is
   from the symbol, a cell being 10 and a row 17, and the list is sorted so
   the nearest comes first. */
#define LABEL_SPOTS_MAX 64
static int label_spots(int cx, int cy, int gap, int w, int rows, int a_w, int spot[][3])
{
    int n = 0;
    int top = cx - w / 2;
    if (top + w > a_w) top = a_w - w;
    if (top < 0) top = 0;
    int cand[LABEL_SPOTS_MAX][2];
    for (int k = 0; k < 5 && n + 10 <= LABEL_SPOTS_MAX; k++) {
        const int g = gap + k;
        const int pos[10][2] = {
            { cx + g, cy - (rows - 1) }, { cx - g - w + 1, cy - (rows - 1) },
            { top, cy - g - (rows - 1) }, { top, cy + g },
            { cx + g, cy }, { cx - g - w + 1, cy },
            { cx + g, cy - (rows - 1) - 1 }, { cx - g - w + 1, cy - (rows - 1) - 1 },
            { cx + g, cy - (rows - 1) + 1 }, { cx - g - w + 1, cy - (rows - 1) + 1 } };
        for (int i = 0; i < 10; i++) {
            if (rows == 1 && (i == 4 || i == 5)) continue;
            cand[n][0] = pos[i][0];
            cand[n][1] = pos[i][1];
            n++;
        }
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        const int x0 = cand[i][0], y0 = cand[i][1];
        const int dxn = x0 > cx ? x0 - cx : (x0 + w - 1 < cx ? cx - (x0 + w - 1) : 0);
        const int dyn = y0 > cy ? y0 - cy : (y0 + rows - 1 < cy ? cy - (y0 + rows - 1) : 0);
        const int cost = 10 * dxn + 17 * dyn;
        int j = m++;
        while (j > 0 && spot[j - 1][2] > cost) {
            spot[j][0] = spot[j - 1][0]; spot[j][1] = spot[j - 1][1]; spot[j][2] = spot[j - 1][2];
            j--;
        }
        spot[j][0] = x0; spot[j][1] = y0; spot[j][2] = cost;
    }
    return m;
}

/* Put a one or two line label next to a symbol at cell (cx, cy), the nearest
   spot that is clear with air round it, then the nearest at all. Dropped only
   when no spot is free of every symbol and every other label. */
static bool place_label_key(tui_surface *sf, tui_rect a, int cx, int cy, int gap,
                        const char *l1, const char *l2, uint8_t attr, bool solid,
                        uint32_t key)
{
    /* Node names and marks can be UTF-8; placement measures in bytes, so it
       works on the names folded to one byte a cell. */
    char b1[64], b2[64];
    snprintf(b1, sizeof(b1), "%s", l1);
    tui_utf8_fold_str(b1);
    l1 = b1;
    if (l2) {
        snprintf(b2, sizeof(b2), "%s", l2);
        tui_utf8_fold_str(b2);
        l2 = b2;
    }
    const int n1 = (int)strlen(l1), n2 = l2 ? (int)strlen(l2) : 0;
    const int w = n1 > n2 ? n1 : n2;
    const int rows = n2 ? 2 : 1;
    s_label_rect = tui_rect_make(0, -1, 0, 0);
    int spot[LABEL_SPOTS_MAX][3];
    const int ns = label_spots(cx, cy, gap, w, rows, a.w, spot);
    int x0=0,y0=0;s_tier=1;
    bool placed=map_label_choose(&s_label_history,key,
        cx,cy,w,rows,spot,3,ns,esp_timer_get_time(),live_label_slot_free,&a,&x0,&y0);
    s_tier=0;if(!placed) return false;
            for (int r = 0; r < rows; r++) {
                const char *s = r ? l2 : l1;
                const int n = r ? n2 : n1;
                if (solid) {
                    for (int i = 0; i < w; i++)
                        tui_put_char(sf, a, a.x + x0 + i, a.y + y0 + r, i < n ? s[i] : ' ', ls_ink_tinted(attr));
                } else {
                    ls_ink_text(sf, a, a.x + x0, a.y + y0 + r, s, n, attr);
                }
                box_take(x0, x0 + w - 1, y0 + r);
#ifdef LS_MAP_AUDIT
                audit_label(x0, y0 + r, w);
#endif
            }
    s_label_rect=tui_rect_make(a.x+x0,a.y+y0,w,rows);
    return true;
}

static bool place_label(tui_surface *sf, tui_rect a, int cx, int cy, int gap,
                        const char *l1, const char *l2, uint8_t attr, bool solid)
{
    return place_label_key(sf,a,cx,cy,gap,l1,l2,attr,solid,
                           map_label_key(l1,32+s_view));
}

static void air_text(tui_surface *sf, tui_rect a)
{
    if (!layer(L_AIR) || layer(L_NOLABEL)) return;
    const uint32_t sel = s_sel == SEL_AIR ? s_sel_icao : 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int slot = 0; slot < ADSB_MAX_TRACKED; slot++) {
            const air_view_t *v = &s_air[slot];
            if (!v->on || !v->inside) continue;
            const adsb_aircraft_t *ac = adsb_state_get(slot);
            /* The selection first this time, so its label gets the best spot. */
            if ((ac->icao == sel) != (pass == 0)) continue;
            char l1[12], l2[12];
            air_block_lines(ac, v, l1, l2, sizeof(l1));
            const bool full = (layer(L_BLOCKS) && !s_map_crowded) || ac->icao == sel || v->emergency;
            const int cx = v->dx / 2, cy = v->dy / 3;
            const int gap = v->compact ? 2 : 3;
            if (ac->icao == sel || v->emergency) {
                const uint8_t tag = v->emergency ? TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_RED)
                                                 : TUI_ATTR(TUI_BLACK, air_band(ac->altitude) | TUI_BRIGHT);
                place_label(sf, a, cx, cy, gap, l1, full ? l2 : NULL, tag, true);
            } else {
                place_label(sf, a, cx, cy, gap, l1, full ? l2 : NULL, TUI_ATTR(v->hue, TUI_BLACK), false);
            }
            hit_label(HIT_AIR, ac->icao, v->lat, v->lon);
        }
    }
}

/* APRS shares the map's symbol reservations, age colours and receive rings. */
static EXT_RAM_BSS_ATTR aprs_packet_t s_aprs_points[APRS_STATIONS];
static int s_aprs_count;
static void aprs_prepare_map(int64_t now)
{
    s_aprs_count = 0;
    aprs_options_t o; aprs_options_get(&o);
    if (!layer(L_APRS) || !o.map) return;
    int count = aprs_store_snapshot(s_aprs_points, APRS_STATIONS, now);
    for (int i = 0; i < count; ++i) {
        aprs_packet_t *p = &s_aprs_points[i];
        if (!p->position ||
            now < p->position_us || now-p->position_us > (int64_t)o.keep_minutes*60000000) continue;
        if (aprs_visible(p, &o, now, s_receiver_fresh, s_receiver.lat_deg, s_receiver.lon_deg,
                         s_receiver.last_fix_us, NULL, NULL)) {
            s_aprs_points[s_aprs_count++] = *p;
        }
    }
}
static uint8_t aprs_hue(const aprs_packet_t *p, int64_t now)
{
    int64_t age = now-p->position_us;
    uint8_t hue = p->weather || p->symbol == '_' ? TUI_YELLOW : TUI_CYAN;
    return age < 300000000 ? hue | TUI_BRIGHT : age < 900000000 ? hue : LS_DIM_FG;
}
static void aprs_canvas(tui_rect a, int pw, int ph, int64_t now)
{
    aprs_prepare_map(now);
    for (int i = 0; i < s_aprs_count; ++i) {
        const aprs_packet_t *p = &s_aprs_points[i]; double fx, fy;
        if (!frame_px(p->lat, p->lon, pw, ph, &fx, &fy) || fx < 0 || fy < 0 || fx >= pw || fy >= ph) continue;
        int dx, dy; px_dot(fx, fy, &dx, &dy);
        uint8_t attr = TUI_ATTR(aprs_hue(p, now), TUI_BLACK);
        if (now-p->heard_us < 2500000) ping_ring(dx, dy, attr, 2000);
        claim_dots(dx, dy, 2, a.w, a.h);
    }
}
static void aprs_text(tui_surface *sf, tui_rect a, int pw, int ph, int64_t now)
{
    for (int i = 0; i < s_aprs_count; ++i) {
        const aprs_packet_t *p = &s_aprs_points[i]; double fx, fy;
        if (!frame_px(p->lat, p->lon, pw, ph, &fx, &fy) || fx < 0 || fy < 0 || fx >= pw || fy >= ph) continue;
        int dx, dy; px_dot(fx, fy, &dx, &dy);
        uint8_t attr = TUI_ATTR(aprs_hue(p, now), TUI_BLACK);
        tui_put_char(sf, a, a.x+dx/2, a.y+dy/3, p->symbol, attr);
        place_label(sf, a, dx/2, dy/3, 2, p->call, p->name[0] ? p->name : NULL, attr, false);
    }
}

/* Sonde tracks use the same geographic projection and age palette as aircraft. */
static EXT_RAM_BSS_ATTR rs41_sonde_t s_map_sonde;
static struct { double lat, lon; char serial[9]; int64_t us; } s_sonde_labels[RS41_SONDES];
static int s_sonde_labels_n;
static void sonde_canvas(tui_rect a, int pw, int ph, int64_t now)
{
    s_sonde_labels_n = 0;
    if (!layer(L_SONDE) || !rs41_show_map()) return;
    for (int slot = 0; slot < RS41_SONDES; ++slot) {
        if (!rs41_store_copy(slot,&s_map_sonde,now) || !s_map_sonde.fix_us) continue;
        uint8_t hue = now-s_map_sonde.fix_us < 300000000 ? TUI_YELLOW|TUI_BRIGHT : LS_DIM_FG;
        uint8_t attr = TUI_ATTR(hue,TUI_BLACK);
        int px = 0, py = 0; bool previous = false;
        for (unsigned i = 0; i < s_map_sonde.count; ++i) {
            double fx, fy; int dx, dy;
            const rs41_point_t *p = &s_map_sonde.track[i];
            if (!frame_px(p->lat,p->lon,pw,ph,&fx,&fy)) { previous = false; continue; }
            px_dot(fx,fy,&dx,&dy);
            if (previous) ls_ink_line(px,py,dx,dy,attr,1,0);
            px = dx; py = dy; previous = true;
        }
        double fx,fy; int dx,dy;
        if (!frame_px(s_map_sonde.report.lat,s_map_sonde.report.lon,pw,ph,&fx,&fy) || fx < 0 || fy < 0 || fx >= pw || fy >= ph) continue;
        px_dot(fx,fy,&dx,&dy);
        ls_ink_ellipse(dx,dy,2,3,attr,3,0);
        ls_ink_line(dx,dy+3,dx,dy+5,attr,3,0);
        if (now-s_map_sonde.fix_us < 2500000) ping_ring(dx,dy,attr,2000);
        claim_dots(dx,dy,3,a.w,a.h);
        int k = s_sonde_labels_n++;
        s_sonde_labels[k].lat = s_map_sonde.report.lat; s_sonde_labels[k].lon = s_map_sonde.report.lon;
        s_sonde_labels[k].us = s_map_sonde.fix_us;
        memcpy(s_sonde_labels[k].serial,s_map_sonde.report.serial,9);
    }
}
static void sonde_text(tui_surface *sf, tui_rect a, int pw, int ph, int64_t now)
{
    if (layer(L_NOLABEL)) return;
    for (int i = 0; i < s_sonde_labels_n; ++i) {
        double fx,fy; int dx,dy;
        if (!frame_px(s_sonde_labels[i].lat,s_sonde_labels[i].lon,pw,ph,&fx,&fy)) continue;
        px_dot(fx,fy,&dx,&dy);
        place_label(sf,a,dx/2,dy/3,3,s_sonde_labels[i].serial,"RS41",
            TUI_ATTR(now-s_sonde_labels[i].us < 300000000 ? TUI_YELLOW|TUI_BRIGHT : LS_DIM_FG,TUI_BLACK),false);
    }
}

/* Vessel positions expire independently of static identity reports. */
static EXT_RAM_BSS_ATTR ais_vessel_t s_ais_points[AIS_VESSELS];
static int s_ais_count;
static const pt_t SHAPE_VESSEL[] = { { 0, -3.5f }, { 1.8f, -0.8f }, { 1.8f, 2.8f }, { 0, 2.8f } };
static uint8_t ais_hue(const ais_vessel_t *p, int64_t now)
{
    ais_options_t o; ais_options_get(&o);
    int64_t age = now-p->position_us, keep = (int64_t)o.keep_minutes*60000000;
    return age < keep/6 ? TUI_CYAN | TUI_BRIGHT : age < keep/2 ? TUI_CYAN : LS_DIM_FG;
}
static void ais_canvas(tui_rect a, int pw, int ph, int64_t now)
{
    s_ais_count = 0; ais_options_t o; ais_options_get(&o);
    if (!layer(L_AIS) || !o.map) return;
    int count = ais_store_snapshot(s_ais_points, AIS_VESSELS, now);
    for (int i = 0; i < count; ++i) {
        ais_vessel_t *p = &s_ais_points[i];
        if (!p->position || now < p->position_us || now-p->position_us > (int64_t)o.keep_minutes*60000000 ||
            !ais_visible(p, &o, now, s_receiver_fresh, s_receiver.lat_deg, s_receiver.lon_deg,
                         s_receiver.last_fix_us, NULL, NULL)) continue;
        s_ais_points[s_ais_count++] = *p;
    }
    for (int i = 0; i < s_ais_count; ++i) {
        const ais_vessel_t *p = &s_ais_points[i]; double fx, fy;
        if (!frame_px(p->lat, p->lon, pw, ph, &fx, &fy) || fx < 0 || fy < 0 || fx >= pw || fy >= ph) continue;
        int dx, dy; px_dot(fx, fy, &dx, &dy);
        uint8_t attr = TUI_ATTR(ais_hue(p, now), TUI_BLACK);
        if (layer(L_VECTORS) && p->sog < 1023 && p->sog > 0 && p->cog < 3600) {
            double lat, lon, x, y;
            double angle = p->cog*M_PI/1800;
            /* A three-minute course vector uses the same geographic projection as aircraft. */
            ls_map_dead_reckon(p->lat, p->lon, (int)lround(p->sog/10.0*cos(angle)),
                (int)lround(p->sog/10.0*sin(angle)), 0, 180000000, 180000000, &lat, &lon);
            if (frame_px(lat, lon, pw, ph, &x, &y)) {
                int qx, qy; px_dot(x, y, &qx, &qy);
                double length = hypot(qx-dx, qy-dy);
                if (length > 24) { qx = dx+(int)lround((qx-dx)*24/length); qy = dy+(int)lround((qy-dy)*24/length); }
                ls_ink_line(dx, dy, qx, qy, attr, 3, 2);
            }
        }
        double heading = p->heading < 360 ? p->heading : p->cog/10.0;
        if (heading < 360) silhouette(dx, dy, SIL(SHAPE_VESSEL), heading, 1, attr, 7);
        else silhouette(dx, dy, SIL(SHAPE_DIAMOND), 0, 1, attr, 7);
        if (now-p->heard_us < 2500000) ping_ring(dx, dy, attr, 2000);
        claim_dots(dx, dy, 4, a.w, a.h);
    }
}
static void ais_text(tui_surface *sf, tui_rect a, int pw, int ph, int64_t now)
{
    ais_options_t o; ais_options_get(&o);
    if (!o.names) return;
    for (int i = 0; i < s_ais_count; ++i) {
        const ais_vessel_t *p = &s_ais_points[i]; double fx, fy;
        if (!frame_px(p->lat, p->lon, pw, ph, &fx, &fy) || fx < 0 || fy < 0 || fx >= pw || fy >= ph) continue;
        int dx, dy; px_dot(fx, fy, &dx, &dy); char mmsi[12];
        snprintf(mmsi, sizeof(mmsi), "%09lu", (unsigned long)p->mmsi);
        /* Names are not identities: two vessels may share the same name. */
        place_label_key(sf, a, dx/2, dy/3, 3, p->name[0] ? p->name : mmsi, NULL,
                    TUI_ATTR(ais_hue(p, now), TUI_BLACK), false,
                    map_label_key(mmsi,64+s_view));
    }
}

/* --------------------------------------------------------- mesh nodes -- */

/* The peers, and they are NOT on the stack: twelve of them is 480 bytes and
   the TUI task's stack is under 4 KB. */
EXT_RAM_BSS_ATTR static ls_mesh_peer_t s_map_peers[LS_MESH_MAX_PEERS];
static int s_npeers;

/* Colour is age, not signal. Signal is a property of the path between two
   radios and changes with a step sideways; how long ago a node was heard is
   a property of the node, and it is the one that answers "is that thing
   still there". */
static uint8_t node_hue(uint32_t age, bool selected)
{
    if (selected) return TUI_WHITE | TUI_BRIGHT;
    return age < 300 ? (TUI_MAGENTA | TUI_BRIGHT) : age < 1800 ? TUI_MAGENTA : LS_DIM_FG;
}

static uint32_t node_age(const ls_mesh_peer_t *p)
{
    const uint32_t now = ls_mesh_now();
    return now > p->last_heard ? now - p->last_heard : 0;
}

static void age_words(char *out, size_t cap, uint32_t s)
{
    if (s < 90) snprintf(out, cap, "%lus", (unsigned long)s);
    else if (s < 5400) snprintf(out, cap, "%lum", (unsigned long)(s / 60));
    else if (s < 172800) snprintf(out, cap, "%luh", (unsigned long)(s / 3600));
    else snprintf(out, cap, "%lud", (unsigned long)(s / 86400));
}

/* A node is a plain shape that says what it is: a triangle for a
   repeater, a square for a room server, a ring for a sensor, a diamond for
   a person's radio. They do not turn; a node has no heading. */
static const pt_t SHAPE_TRIANGLE[] = {
    { 0.0f, -2.8f }, { 2.7f, 2.2f }, { 0.0f, 2.2f },
};

static void node_symbol(int cx, int cy, uint8_t type, uint8_t attr, uint8_t prio, double size)
{
    switch (type) {
    case LS_MESH_ROLE_REPEATER: silhouette(cx, cy, SIL(SHAPE_TRIANGLE), 0, size, attr, prio); break;
    case LS_MESH_ROLE_ROOM:     silhouette(cx, cy, SIL(SHAPE_SQUARE), 0, 0.9 * size, attr, prio); break;
    case LS_MESH_ROLE_SENSOR: {
        const int r = size < 1.0 ? 1 : 2;
        ls_ink_ellipse(cx, cy, r, r, attr, prio, 0);
        break;
    }
    default: silhouette(cx, cy, SIL(SHAPE_DIAMOND), 0, size, attr, prio); break;
    }
}

static bool s_node_compact[LS_MESH_MAX_PEERS];

/* Read once a frame, before anything that asks about a node: the card and
   FOLLOW want them even with the layer off. */
static void nodes_prepare(void)
{
    s_npeers = ls_mesh_peers(s_map_peers, LS_MESH_MAX_PEERS);
    if (s_npeers < 0) s_npeers = 0;
}

static void nodes_canvas(tui_rect a, int pw, int ph)
{
    if (!layer(L_MESH) || s_npeers <= 0) return;

    double rlat = 0, rlon = 0;
    const bool have_rx = receiver_at(&rlat, &rlon, NULL);
    int rdx = 0, rdy = 0;
    if (have_rx) {
        double fx, fy;
        if (frame_px(rlat, rlon, pw, ph, &fx, &fy)) px_dot(fx, fy, &rdx, &rdy);
    }

    for (int i = 0; i < s_npeers; i++) {
        const ls_mesh_peer_t *p = &s_map_peers[i];
        if (!p->has_loc) continue;
        double fx, fy;
        if (!frame_px(p->lat_e6 / 1e6, p->lon_e6 / 1e6, pw, ph, &fx, &fy)) continue;
        int dx, dy;
        px_dot(fx, fy, &dx, &dy);
        const uint32_t age = node_age(p);
        const bool selected = s_sel == SEL_NODE && !strcmp(p->id, s_sel_node);
        const uint8_t hue = node_hue(age, selected);
        /* Heard in the last ten minutes: a thin line from here to it. */
        if (layer(L_LINKS) && have_rx && age < 600)
            ls_ink_line(rdx, rdy, dx, dy, TUI_ATTR(TUI_MAGENTA, TUI_BLACK), 1, 1);
        if (fx < 0 || fy < 0 || fx >= pw || fy >= ph) continue;
        /* A ring only when it has just been heard. */
        if (age < 60) ping_ring(dx, dy, TUI_ATTR(TUI_MAGENTA, TUI_BLACK), 2000);
        const bool compact = !selected && (ls_map_zoom() < 10 || crowded_at(dx, dy));
        s_node_compact[i] = compact;
        node_symbol(dx, dy, p->type, TUI_ATTR(hue, TUI_BLACK), selected ? 8 : 6, compact ? 0.75 : 1.0);
        claim_dots(dx, dy, compact ? 2 : 3, a.w, a.h);
        if (selected) lock_brackets(dx, dy, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK), esp_timer_get_time());
        hit_add(a.x + dx / 2, a.y + dy / 3, HIT_NODE, (uint32_t)i, p->lat_e6 / 1e6, p->lon_e6 / 1e6);
    }
}

static void nodes_text(tui_surface *sf, tui_rect a, int pw, int ph)
{
    if (!layer(L_MESH)) return;
    for (int i = 0; i < s_npeers; i++) {
        const ls_mesh_peer_t *p = &s_map_peers[i];
        if (!p->has_loc) continue;
        double fx, fy;
        if (!frame_px(p->lat_e6 / 1e6, p->lon_e6 / 1e6, pw, ph, &fx, &fy)) continue;
        if (fx < 0 || fy < 0 || fx >= pw || fy >= ph) continue;
        int dx, dy;
        px_dot(fx, fy, &dx, &dy);
        const uint32_t age = node_age(p);
        const bool selected = s_sel == SEL_NODE && !strcmp(p->id, s_sel_node);
        char l1[LS_MESH_PEER_NAME], l2[24], ago[12];
        if (p->name[0]) snprintf(l1, sizeof(l1), "%.12s", p->name);
        else snprintf(l1, sizeof(l1), "%.8s", p->id);
        age_words(ago, sizeof(ago), age);
        snprintf(l2, sizeof(l2), "%s %.0fdB", ago, (double)p->rssi);
        const int gap = s_node_compact[i] ? 2 : 3;
        if (selected)
            place_label(sf, a, dx / 2, dy / 3, gap, l1, l2, TUI_ATTR(TUI_BLACK, TUI_MAGENTA | TUI_BRIGHT), true);
        else
            place_label(sf, a, dx / 2, dy / 3, gap, l1, layer(L_BLOCKS) ? l2 : NULL,
                        TUI_ATTR(node_hue(age, false), TUI_BLACK), false);
        hit_label(HIT_NODE, (uint32_t)i, p->lat_e6 / 1e6, p->lon_e6 / 1e6);
    }
}

/* ----------------------------------------------- receiver, rings, cover -- */

/* The saved home, when a live fix has taken the receiver's place and the
   two are more than a cell apart. */
static bool home_apart(tui_rect a, int pw, int ph, int *cx, int *cy)
{
    float hl, ho;
    if (!s_receiver_fresh || !settings_get_home(&hl, &ho) || fabs(hl) > 85 || fabs(ho) > 180) return false;
    int gx, gy;
    if (!map_cell_of(hl, ho, a, pw, ph, cx, cy)) return false;
    if (map_cell_of(s_receiver.lat_deg, s_receiver.lon_deg, a, pw, ph, &gx, &gy) &&
        abs(gx - *cx) <= 1 && abs(gy - *cy) <= 1) return false;
    return true;
}

static void receiver_canvas(tui_rect a, int pw, int ph)
{
    int hx, hy;
    if (home_apart(a, pw, ph, &hx, &hy)) {
        const int dx = hx * 2 + 1, dy = hy * 3 + 1;
        const uint8_t at = TUI_ATTR(TUI_CYAN, TUI_BLACK);
        ls_ink_line(dx - 2, dy - 2, dx + 2, dy - 2, at, 7, 0);
        ls_ink_line(dx - 2, dy + 2, dx + 2, dy + 2, at, 7, 0);
        ls_ink_line(dx - 2, dy - 2, dx - 2, dy + 2, at, 7, 0);
        ls_ink_line(dx + 2, dy - 2, dx + 2, dy + 2, at, 7, 0);
        claim_dots(dx, dy, 2, a.w, a.h);
    }
    double lat, lon;
    bool live = false;
    if (!receiver_at(&lat, &lon, &live)) return;
    double fx, fy;
    if (!frame_px(lat, lon, pw, ph, &fx, &fy) || fx < 0 || fy < 0 || fx >= pw || fy >= ph) return;
    int dx, dy;
    px_dot(fx, fy, &dx, &dy);
    const uint8_t at = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    if (live) ping_ring(dx, dy, TUI_ATTR(TUI_CYAN, TUI_BLACK), 2000);
    ls_ink_line(dx - 4, dy, dx - 2, dy, at, 8, 0);
    ls_ink_line(dx + 2, dy, dx + 4, dy, at, 8, 0);
    ls_ink_line(dx, dy - 4, dx, dy - 2, at, 8, 0);
    ls_ink_line(dx, dy + 2, dx, dy + 4, at, 8, 0);
    ls_ink_dot(dx, dy, at, 8);
    claim_dots(dx, dy, 3, a.w, a.h);
}

static void receiver_text(tui_surface *sf, tui_rect a, int pw, int ph)
{
    int hx, hy;
    if (home_apart(a, pw, ph, &hx, &hy))
        place_label(sf, a, hx, hy, 3, "HOME", NULL, TUI_ATTR(TUI_CYAN, TUI_BLACK), false);
    double lat, lon;
    bool live = false;
    if (!receiver_at(&lat, &lon, &live)) return;
    int cx, cy;
    if (!map_cell_of(lat, lon, a, pw, ph, &cx, &cy)) return;
    place_label(sf, a, cx, cy, 3, live ? "GPS" : "HOME", NULL, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK), false);
}

static double s_ring_nm;

static void rings_canvas(tui_surface *sf, tui_rect a, int pw, int ph, bool text)
{
    s_ring_nm = 0;
    if (!layer(L_RINGS)) return;
    double lat, lon;
    if (!receiver_at(&lat, &lon, NULL)) return;
    double fx, fy;
    if (!frame_px(lat, lon, pw, ph, &fx, &fy)) return;
    const double mpp = frame_mpp();
    if (!(mpp > 0)) return;
    const double half_nm = (pw > ph ? pw : ph) / 2.0 * mpp / LS_GEO_M_PER_NM;
    static const double NICE[] = { 0.25, 0.5, 1, 2, 5, 10, 20, 25, 50, 100, 200, 250 };
    double step = NICE[0];
    for (unsigned i = 0; i < sizeof(NICE) / sizeof(NICE[0]); i++)
        if (NICE[i] <= half_nm / 3.0) step = NICE[i];
    s_ring_nm = step;
    const double diag = hypot(pw, ph) + hypot(fx - pw / 2.0, fy - ph / 2.0);
    int dx, dy;
    px_dot(fx, fy, &dx, &dy);
    const uint8_t at = radar_attr();
    for (int k = 1; k <= 12; k++) {
        const double r_px = k * step * LS_GEO_M_PER_NM / mpp;
        if (r_px > diag) break;
        const int rx = (int)lround(r_px * 2.0 / SUB_X), ry = (int)lround(r_px * 3.0 / sub_y());
        if (!text) {
            ls_ink_ellipse(dx, dy, rx, ry, at, 1, -3);
            continue;
        }
        char lab[12];
        if (step < 1) snprintf(lab, sizeof(lab), "%.2gnm", k * step);
        else snprintf(lab, sizeof(lab), "%.0fnm", k * step);
        const int n = (int)strlen(lab);
        const int cx = dx / 2 - n / 2, cy = (dy - ry) / 3;
        if (cx >= 0 && cx + n <= a.w && cy >= 0 && cy < a.h && box_free(cx, cx + n - 1, cy)) {
            ls_ink_text(sf, a, a.x + cx, a.y + cy, lab, n, at);
            box_take(cx, cx + n - 1, cy);
        }
    }
}

/* How far out aircraft have been heard, by bearing, since boot: 36 sectors
   of ten degrees about the receiver, each holding its furthest position. */
#define COVER_SECTORS 36
EXT_RAM_BSS_ATTR static float s_cover_nm[COVER_SECTORS];
static double s_cover_lat = 999, s_cover_lon;
static float  s_cover_max;

static void cover_update(void)
{
    double lat, lon;
    if (!receiver_at(&lat, &lon, NULL)) return;
    /* A receiver that moved more than a mile starts a new outline. */
    double b, m;
    if (s_cover_lat > 90) m = 1e9;
    else ls_geo_bearing_range(s_cover_lat, s_cover_lon, lat, lon, &b, &m);
    if (m > LS_GEO_M_PER_MILE) {
        memset(s_cover_nm, 0, sizeof(s_cover_nm));
        s_cover_lat = lat; s_cover_lon = lon; s_cover_max = 0;
    }
    for (int slot = 0; slot < ADSB_MAX_TRACKED; slot++) {
        const adsb_aircraft_t *ac = adsb_state_get(slot);
        if (!ac || !ac->active || !ac->pos_valid) continue;
        ls_geo_bearing_range(s_cover_lat, s_cover_lon, ac->lat, ac->lon, &b, &m);
        const float nm = (float)(m / LS_GEO_M_PER_NM);
        if (nm > 400) continue;
        const int s = ((int)(b / 10.0)) % COVER_SECTORS;
        if (nm > s_cover_nm[s]) s_cover_nm[s] = nm;
        if (nm > s_cover_max) s_cover_max = nm;
    }
}

static void cover_canvas(int pw, int ph)
{
    if (!layer(L_COVER) || s_cover_lat > 90) return;
    const double mpp = frame_mpp();
    if (!(mpp > 0)) return;
    double fx0, fy0;
    if (!frame_px(s_cover_lat, s_cover_lon, pw, ph, &fx0, &fy0)) return;
    int px = 0, py = 0, first_x = 0, first_y = 0;
    bool have_prev = false, have_first = false;
    const uint8_t at = TUI_ATTR(TUI_GREEN, TUI_BLACK);
    for (int s = 0; s <= COVER_SECTORS; s++) {
        const int k = s % COVER_SECTORS;
        if (s_cover_nm[k] <= 0) { have_prev = false; continue; }
        const double brg = (k * 10 + 5) * M_PI / 180.0;
        const double r_px = s_cover_nm[k] * LS_GEO_M_PER_NM / mpp;
        int dx, dy;
        px_dot(fx0 + r_px * sin(brg), fy0 - r_px * cos(brg), &dx, &dy);
        if (s == COVER_SECTORS) { if (have_prev && have_first) ls_ink_line(px, py, first_x, first_y, at, 1, 2); break; }
        if (have_prev) ls_ink_line(px, py, dx, dy, at, 1, 2);
        if (!have_first && s == 0) { first_x = dx; first_y = dy; have_first = true; }
        px = dx; py = dy; have_prev = true;
    }
}

/* ------------------------------------------------------ marks and lines -- */

static uint8_t mark_hue(int icon)
{
    switch (icon) {
    case LS_MARK_HAZARD: case LS_MARK_TARGET: case LS_MARK_AID: return TUI_RED | TUI_BRIGHT;
    case LS_MARK_CAMP:  return TUI_GREEN | TUI_BRIGHT;
    case LS_MARK_WATER: return TUI_BLUE | TUI_BRIGHT;
    case LS_MARK_FLAG:  return TUI_MAGENTA | TUI_BRIGHT;
    case LS_MARK_CAR:   return TUI_WHITE | TUI_BRIGHT;
    default:            return TUI_YELLOW | TUI_BRIGHT;
    }
}

static void sketch_canvas(const ls_sketch_t *s, int pw, int ph, bool open)
{
    if (!s || s->n <= 0) return;
    const uint8_t at = TUI_ATTR(s->hue | TUI_BRIGHT, TUI_BLACK);
    int px = 0, py = 0;
    for (int i = 0; i < s->n; i++) {
        double fx, fy;
        if (!frame_px(s->lat[i], s->lon[i], pw, ph, &fx, &fy)) continue;
        int dx, dy;
        px_dot(fx, fy, &dx, &dy);
        if (i) {
            /* Two dots wide, so a line reads over any ground. */
            ls_ink_line(px, py, dx, dy, at, 4, 0);
            ls_ink_line(px + 1, py, dx + 1, dy, at, 4, 0);
        }
        const int r = open && i == s->n - 1 ? 2 + ls_motion_phase(2, 600) : 1;
        ls_ink_line(dx - r, dy - r, dx + r, dy - r, at, 5, 0);
        ls_ink_line(dx - r, dy + r, dx + r, dy + r, at, 5, 0);
        ls_ink_line(dx - r, dy - r, dx - r, dy + r, at, 5, 0);
        ls_ink_line(dx + r, dy - r, dx + r, dy + r, at, 5, 0);
        px = dx; py = dy;
    }
}

static void marks_canvas(tui_rect a, int pw, int ph, int64_t now)
{
    if (!layer(L_MARKS)) return;
    for (int i = 0; i < ls_sketch_count(); i++) {
        const ls_sketch_t *s = ls_sketch_at(i);
        sketch_canvas(s, pw, ph, false);
        if (s && s->n > 0) {
            double fx, fy;
            if (frame_px(s->lat[s->n - 1], s->lon[s->n - 1], pw, ph, &fx, &fy) &&
                fx >= 0 && fy >= 0 && fx < pw && fy < ph) {
                int dx, dy;
                px_dot(fx, fy, &dx, &dy);
                hit_add(a.x + dx / 2, a.y + dy / 3, HIT_LINE, (uint32_t)i, s->lat[s->n - 1], s->lon[s->n - 1]);
                if (s_sel == SEL_LINE && s_sel_index == i)
                    lock_brackets(dx, dy, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK), now);
            }
        }
    }
    for (int i = 0; i < ls_marks_count(); i++) {
        const ls_mark_t *m = ls_marks_at(i);
        double fx, fy;
        if (!frame_px(m->lat, m->lon, pw, ph, &fx, &fy) || fx < 0 || fy < 0 || fx >= pw || fy >= ph) continue;
        int dx, dy;
        px_dot(fx, fy, &dx, &dy);
        /* A pin: a ring about the cell the symbol letter sits in. */
        const int cdx = (dx / 2) * 2 + 1, cdy = (dy / 3) * 3 + 1;
        const uint8_t at = TUI_ATTR(mark_hue(m->icon), TUI_BLACK);
        ls_ink_line(cdx - 3, cdy - 2, cdx + 3, cdy - 2, at, 5, 0);
        ls_ink_line(cdx - 3, cdy + 2, cdx + 3, cdy + 2, at, 5, 0);
        ls_ink_line(cdx - 3, cdy - 2, cdx - 3, cdy + 2, at, 5, 0);
        ls_ink_line(cdx + 3, cdy - 2, cdx + 3, cdy + 2, at, 5, 0);
        ls_ink_line(cdx, cdy + 2, cdx, cdy + 4, at, 5, 0);
        claim_dots(cdx, cdy, 3, a.w, a.h);
        const bool sel = s_sel == SEL_MARK && s_sel_index == i;
        if (sel) lock_brackets(cdx, cdy, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK), now);
        hit_add(a.x + dx / 2, a.y + dy / 3, HIT_MARK, (uint32_t)i, m->lat, m->lon);
    }
    /* The compass's GO TO target, so the map and the compass agree. */
    double tlat, tlon;
    if (ls_compass_target(&tlat, &tlon, NULL, 0)) {
        double fx, fy;
        if (frame_px(tlat, tlon, pw, ph, &fx, &fy) && fx >= 0 && fy >= 0 && fx < pw && fy < ph) {
            int dx, dy;
            px_dot(fx, fy, &dx, &dy);
            const uint8_t at = TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK);
            ls_ink_line(dx, dy + 3, dx, dy - 4, at, 5, 0);
            ls_ink_line(dx, dy - 4, dx + 4, dy - 3, at, 5, 0);
            ls_ink_line(dx + 4, dy - 3, dx, dy - 2, at, 5, 0);
            claim_dots(dx, dy, 3, a.w, a.h);
        }
    }
    const ls_sketch_t *open = ls_sketch_open();
    if (open) sketch_canvas(open, pw, ph, true);
}

static void marks_text(tui_surface *sf, tui_rect a, int pw, int ph)
{
    if (!layer(L_MARKS)) return;
    for (int i = 0; i < ls_marks_count(); i++) {
        const ls_mark_t *m = ls_marks_at(i);
        int cx, cy;
        if (!map_cell_of(m->lat, m->lon, a, pw, ph, &cx, &cy)) continue;
        const uint8_t hue = mark_hue(m->icon);
        glass(sf, a, a.x + cx, a.y + cy, ls_mark_icon_char(m->icon), TUI_ATTR(hue, TUI_BLACK));
        const bool sel = s_sel == SEL_MARK && s_sel_index == i;
        place_label(sf, a, cx, cy, 3, m->name, NULL,
                    sel ? TUI_ATTR(TUI_BLACK, hue) : TUI_ATTR(hue, TUI_BLACK), sel);
        hit_label(HIT_MARK, (uint32_t)i, m->lat, m->lon);
    }
    for (int i = 0; i < ls_sketch_count(); i++) {
        const ls_sketch_t *s = ls_sketch_at(i);
        if (!s || s->n < 2) continue;
        int cx, cy;
        if (!map_cell_of(s->lat[s->n - 1], s->lon[s->n - 1], a, pw, ph, &cx, &cy)) continue;
        char len[24];
        snprintf(len, sizeof(len), "%.2fmi", ls_sketch_length_m(s) / LS_GEO_M_PER_MILE);
        const bool sel = s_sel == SEL_LINE && s_sel_index == i;
        place_label(sf, a, cx, cy, 2, s->name, len,
                    sel ? TUI_ATTR(TUI_BLACK, s->hue | TUI_BRIGHT) : TUI_ATTR(s->hue | TUI_BRIGHT, TUI_BLACK), sel);
        hit_label(HIT_LINE, (uint32_t)i, s->lat[s->n - 1], s->lon[s->n - 1]);
    }
    double tlat, tlon;
    char tname[24];
    if (ls_compass_target(&tlat, &tlon, tname, sizeof(tname))) {
        int cx, cy;
        if (map_cell_of(tlat, tlon, a, pw, ph, &cx, &cy)) {
            char l1[32];
            snprintf(l1, sizeof(l1), "GO TO %.18s", tname);
            place_label(sf, a, cx, cy, 3, l1, NULL, TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK), false);
        }
    }
}

/* -------------------------------------------------------- off the pane -- */

/* Something worth finding that is not in view gets an arrow on the edge of
   the pane pointing at it, with its name and how far. Tapping the arrow
   goes there. */
typedef struct { double fx, fy, lat, lon; uint8_t kind; uint32_t id; char name[12]; uint8_t hue; } edge_t;
#define EDGES_MAX 8
EXT_RAM_BSS_ATTR static edge_t s_edges[EDGES_MAX];
static int    s_nedges;

static void edge_add(double fx, double fy, double lat, double lon, uint8_t kind, uint32_t id,
                     const char *name, uint8_t hue)
{
    if (s_nedges >= EDGES_MAX) return;
    edge_t *e = &s_edges[s_nedges++];
    e->fx = fx; e->fy = fy; e->lat = lat; e->lon = lon; e->kind = kind; e->id = id; e->hue = hue;
    snprintf(e->name, sizeof(e->name), "%.10s", name);
}

static void edges_collect(int pw, int ph)
{
    s_nedges = 0;
    /* The selection always, then the nearest aircraft out of view. */
    if (s_sel == SEL_AIR) {
        for (int s = 0; s < ADSB_MAX_TRACKED; s++) {
            const air_view_t *v = &s_air[s];
            const adsb_aircraft_t *ac = adsb_state_get(s);
            if (v->on && !v->inside && ac->icao == s_sel_icao)
                edge_add(v->fx, v->fy, v->lat, v->lon, HIT_AIR, ac->icao,
                         ac->callsign[0] ? ac->callsign : "selected", TUI_WHITE | TUI_BRIGHT);
        }
    } else if (s_sel == SEL_MARK) {
        const ls_mark_t *m = ls_marks_at(s_sel_index);
        double fx, fy;
        if (m && frame_px(m->lat, m->lon, pw, ph, &fx, &fy) && (fx < 0 || fy < 0 || fx >= pw || fy >= ph))
            edge_add(fx, fy, m->lat, m->lon, HIT_MARK, (uint32_t)s_sel_index, m->name, mark_hue(m->icon));
    } else if (s_sel == SEL_NODE) {
        for (int i = 0; i < s_npeers; i++) {
            const ls_mesh_peer_t *p = &s_map_peers[i];
            double fx, fy;
            if (p->has_loc && !strcmp(p->id, s_sel_node) &&
                frame_px(p->lat_e6 / 1e6, p->lon_e6 / 1e6, pw, ph, &fx, &fy) &&
                (fx < 0 || fy < 0 || fx >= pw || fy >= ph))
                edge_add(fx, fy, p->lat_e6 / 1e6, p->lon_e6 / 1e6, HIT_NODE, (uint32_t)i,
                         p->name[0] ? p->name : p->id, TUI_MAGENTA | TUI_BRIGHT);
        }
    }
    if (!layer(L_AIR)) return;
    for (int k = 0; k < 4; k++) {
        int best = -1;
        double best_d = 0;
        for (int s = 0; s < ADSB_MAX_TRACKED; s++) {
            const air_view_t *v = &s_air[s];
            if (!v->on || v->inside || !v->fresh) continue;
            const adsb_aircraft_t *ac = adsb_state_get(s);
            bool dup = false;
            for (int e = 0; e < s_nedges; e++) if (s_edges[e].kind == HIT_AIR && s_edges[e].id == ac->icao) dup = true;
            if (dup) continue;
            const double d = hypot(v->fx - pw / 2.0, v->fy - ph / 2.0);
            if (best < 0 || d < best_d) { best = s; best_d = d; }
        }
        if (best < 0) break;
        const adsb_aircraft_t *ac = adsb_state_get(best);
        char name[12];
        if (ac->callsign[0]) snprintf(name, sizeof(name), "%.8s", ac->callsign);
        else snprintf(name, sizeof(name), "%06lX", (unsigned long)ac->icao);
        edge_add(s_air[best].fx, s_air[best].fy, s_air[best].lat, s_air[best].lon, HIT_AIR, ac->icao,
                 name, air_band(ac->altitude) | TUI_BRIGHT);
    }
}

/* Where the line from the centre to (fx, fy) leaves the frame, pulled in
   by a margin. */
static void edge_point(int pw, int ph, double fx, double fy, double margin, double *ex, double *ey)
{
    const double cx = pw / 2.0, cy = ph / 2.0;
    const double vx = fx - cx, vy = fy - cy;
    double t = 1e9;
    if (vx > 0) t = fmin(t, (pw - margin - cx) / vx);
    if (vx < 0) t = fmin(t, (margin - cx) / vx);
    if (vy > 0) t = fmin(t, (ph - margin - cy) / vy);
    if (vy < 0) t = fmin(t, (margin - cy) / vy);
    if (t > 1) t = 1;
    *ex = cx + vx * t;
    *ey = cy + vy * t;
}

static void edges_canvas(tui_rect a, int pw, int ph)
{
    for (int i = 0; i < s_nedges; i++) {
        const edge_t *e = &s_edges[i];
        double ex, ey;
        edge_point(pw, ph, e->fx, e->fy, 2.0 * SUB_X, &ex, &ey);
        int tx, ty;
        px_dot(ex, ey, &tx, &ty);
        const double ang = atan2(e->fy - ph / 2.0, e->fx - pw / 2.0);
        const uint8_t at = TUI_ATTR(e->hue, TUI_BLACK);
        /* An arrowhead pointing out of the pane, and its shaft. */
        for (int side = -1; side <= 1; side += 2) {
            const double b = ang + M_PI + side * 0.55;
            ls_ink_line(tx, ty, tx + (int)lround(4 * cos(b)), ty + (int)lround(4 * sin(b) * 1.1), at, 9, 0);
        }
        ls_ink_line(tx, ty, tx - (int)lround(6 * cos(ang)), ty - (int)lround(6 * sin(ang)), at, 9, 0);
        claim_dots(tx, ty, 4, a.w, a.h);
        hit_add(a.x + tx / 2, a.y + ty / 3, (uint8_t)(e->kind | HIT_EDGE), e->id, e->lat, e->lon);
    }
}

static void edges_text(tui_surface *sf, tui_rect a, int pw, int ph)
{
    double rlat = 0, rlon = 0, clat = 0, clon = 0;
    const bool have_rx = receiver_at(&rlat, &rlon, NULL);
    ls_map_get_center(&clat, &clon);
    for (int i = 0; i < s_nedges; i++) {
        const edge_t *e = &s_edges[i];
        double ex, ey;
        edge_point(pw, ph, e->fx, e->fy, 2.0 * SUB_X, &ex, &ey);
        const int cx = (int)(ex / SUB_X), cy = (int)(ey / sub_y());
        char dist[24];
        range_words(dist, sizeof(dist), have_rx ? rlat : clat, have_rx ? rlon : clon, e->lat, e->lon);
        char *sp = strchr(dist, ' ');
        if (sp && strchr(sp + 1, ' ')) *strchr(sp + 1, ' ') = 0;   /* "12nm NE" */
        place_label(sf, a, cx, cy, 3, e->name, dist, TUI_ATTR(e->hue, TUI_BLACK), false);
        hit_label((uint8_t)(e->kind | HIT_EDGE), e->id, e->lat, e->lon);
    }
}

/* ---------------------------------------------------------------- card -- */

/* What is known about the selection, in a panel over the corner of the map. */
static int card_lines(char lines[][40], int max, char *title, size_t tcap)
{
    int n = 0;
    double rlat = 0, rlon = 0;
    const bool have_rx = receiver_at(&rlat, &rlon, NULL);
    char rng[28];
    if (s_sel == SEL_AIR) {
        const adsb_aircraft_t *ac = NULL;
        int slot = -1;
        for (int s = 0; s < ADSB_MAX_TRACKED; s++) {
            const adsb_aircraft_t *c = adsb_state_get(s);
            if (c && c->active && c->icao == s_sel_icao) { ac = c; slot = s; }
        }
        if (!ac) return 0;
        snprintf(title, tcap, "%.8s", ac->callsign[0] ? ac->callsign : "AIRCRAFT");
        snprintf(lines[n++], 40, "ICAO %06lX", (unsigned long)ac->icao);
        snprintf(lines[n++], 40, "ALT  %d ft %+d", ac->altitude, ac->vert_rate);
        snprintf(lines[n++], 40, "SPD  %d kt  HDG %03d", ac->velocity, ac->heading);
        const int sq = adsb_state_squawk(slot);
        if (sq) snprintf(lines[n++], 40, "SQK  %04d%s", sq, squawk_emergency(sq) ? " EMERGENCY" : "");
        if (ac->pos_valid && have_rx) {
            range_words(rng, sizeof(rng), rlat, rlon, s_air[slot].lat, s_air[slot].lon);
            snprintf(lines[n++], 40, "RNG  %s", rng);
        }
        const int64_t now = esp_timer_get_time();
        const int age = ac->pos_valid ? (int)((now - ac->pos_ts_us) / 1000000) : -1;
        if (age >= 0) snprintf(lines[n++], 40, "POS  %ds ago%s", age, age > 20 ? " (held)" : "");
        snprintf(lines[n++], 40, "%s", s_track ? "FOLLOWING  G stops" : "ENTER go  G follow");
    } else if (s_sel == SEL_NODE) {
        const ls_mesh_peer_t *p = NULL;
        for (int i = 0; i < s_npeers; i++) if (!strcmp(s_map_peers[i].id, s_sel_node)) p = &s_map_peers[i];
        if (!p) return 0;
        snprintf(title, tcap, "%.16s", p->name[0] ? p->name : "NODE");
        static const char *const TYPES[] = { "node", "chat", "repeater", "room", "sensor" };
        snprintf(lines[n++], 40, "ID   %.8s %s", p->id, TYPES[p->type <= 4 ? p->type : 0]);
        char ago[12];
        age_words(ago, sizeof(ago), node_age(p));
        snprintf(lines[n++], 40, "HEARD %s ago", ago);
        snprintf(lines[n++], 40, "SIG  %.0f dBm  SNR %.1f", (double)p->rssi, (double)p->snr);
        if (p->has_loc && have_rx) {
            range_words(rng, sizeof(rng), rlat, rlon, p->lat_e6 / 1e6, p->lon_e6 / 1e6);
            snprintf(lines[n++], 40, "RNG  %s", rng);
        }
        snprintf(lines[n++], 40, "%s", s_track ? "FOLLOWING  G stops" : "ENTER go  G follow");
    } else if (s_sel == SEL_MARK) {
        const ls_mark_t *m = ls_marks_at(s_sel_index);
        if (!m) return 0;
        snprintf(title, tcap, "%.20s", m->name);
        snprintf(lines[n++], 40, "%c %s", ls_mark_icon_char(m->icon), ls_mark_icon_name(m->icon));
        snprintf(lines[n++], 40, "%.5f, %.5f", m->lat, m->lon);
        if (have_rx) {
            range_words(rng, sizeof(rng), rlat, rlon, m->lat, m->lon);
            snprintf(lines[n++], 40, "RNG  %s", rng);
        }
        snprintf(lines[n++], 40, "ENTER go  E edit");
    } else if (s_sel == SEL_LINE) {
        const ls_sketch_t *s = ls_sketch_at(s_sel_index);
        if (!s) return 0;
        snprintf(title, tcap, "%.20s", s->name);
        snprintf(lines[n++], 40, "%d points", s->n);
        snprintf(lines[n++], 40, "%.2f mi  %.2f km", ls_sketch_length_m(s) / LS_GEO_M_PER_MILE,
                 ls_sketch_length_m(s) / 1000.0);
        snprintf(lines[n++], 40, "ENTER go  E edit");
    }
    return n < max ? n : max;
}

EXT_RAM_BSS_ATTR static char s_card_text[8][40];
EXT_RAM_BSS_ATTR static char s_card_title[24];

static tui_rect card_rect_for(tui_rect body)
{
    if (s_sel == SEL_NONE || s_card_hidden || body.w < 30 || body.h < 14) return tui_rect_make(0, -1, 0, 0);
    char (*lines)[40] = s_card_text;
    char *title = s_card_title;
    title[0] = 0;
    const int n = card_lines(lines, 8, title, sizeof(s_card_title));
    if (n <= 0) return tui_rect_make(0, -1, 0, 0);
    int w = (int)strlen(title) + 6;
    for (int i = 0; i < n; i++) if ((int)strlen(lines[i]) + 4 > w) w = (int)strlen(lines[i]) + 4;
    if (w > body.w - 2) w = body.w - 2;
    const int h = n + 2;
    if (ls_tui_is_wide()) return tui_rect_make(body.x + 1, body.y + 1, w, h);
    return tui_rect_make(body.x + 1, body.y + body.h - h - 1, w, h);
}

static void draw_card(tui_surface *sf, tui_rect r)
{
    if (r.h <= 0) return;
    char (*lines)[40] = s_card_text;
    char *title = s_card_title;
    title[0] = 0;
    const int n = card_lines(lines, 8, title, sizeof(s_card_title));
    tui_fill(sf, r, ' ', TUI_ATTR(TUI_WHITE, TUI_BLACK));
    const uint8_t hue = s_sel == SEL_AIR ? TUI_YELLOW : s_sel == SEL_NODE ? TUI_MAGENTA
                      : s_sel == SEL_LINE ? TUI_GREEN : TUI_CYAN;
    ls_panel_box(sf, r, title, hue);
    for (int i = 0; i < n && i < r.h - 2; i++) {
        const bool hint = i == n - 1;
        tui_put_str(sf, r, r.x + 2, r.y + 1 + i, lines[i],
                    hint ? LS_ATTR_DIM : TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    }
}

/* ------------------------------------------------------------- pickers -- */

/* Everything worth going to, in one list: aircraft and nodes nearest the
   centre first, then markers, lines and the places named in view. */
typedef struct { uint8_t kind; uint32_t id; double lat, lon; char node[17]; } goto_t;
EXT_RAM_BSS_ATTR static goto_t s_goto[LS_PICKER_MAX];
static int s_ngoto;
static int s_find_pw, s_find_ph;

static void goto_pick(int i)
{
    if (i < 0 || i >= s_ngoto) return;
    const goto_t *g = &s_goto[i];
    if(g->kind==255) { ls_place_search_open(true);return; }
    s_track = false;
    switch (g->kind) {
    case HIT_AIR:  select_air(g->id); break;
    case HIT_NODE: select_node(g->node); break;
    case HIT_MARK: select_mark(SEL_MARK, (int)g->id); break;
    case HIT_LINE: select_mark(SEL_LINE, (int)g->id); break;
    default:       select_none(); break;
    }
    jump_to(g->lat, g->lon);
}

static void goto_add(uint8_t kind, uint32_t id, double lat, double lon, const char *node,
                     const char *label, const char *detail)
{
    if (s_ngoto >= LS_PICKER_MAX) return;
    if (!ls_picker_add(label, detail)) return;
    goto_t *g = &s_goto[s_ngoto++];
    g->kind = kind; g->id = id; g->lat = lat; g->lon = lon;
    snprintf(g->node, sizeof(g->node), "%s", node ? node : "");
}

static ls_act_status_t a_map_find(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    s_ngoto = 0;
    ls_picker_open("GO TO", goto_pick);
    goto_add(255,0,0,0,NULL,"SEARCH PLACES","Country, state or city");
    double clat, clon;
    ls_map_get_center(&clat, &clon);
    double rlat = clat, rlon = clon;
    receiver_at(&rlat, &rlon, NULL);
    char detail[LS_PICKER_DETAIL], label[LS_PICKER_TEXT], rng[28];

    /* Aircraft by range from here. */
    EXT_RAM_BSS_ATTR static int order[ADSB_MAX_TRACKED];
    EXT_RAM_BSS_ATTR static double dist[ADSB_MAX_TRACKED];
    int m = 0;
    const int64_t now = esp_timer_get_time();
    for (int s = 0; s < ADSB_MAX_TRACKED; s++) {
        const adsb_aircraft_t *ac = adsb_state_get(s);
        if (!ac || !ac->active || !ac->pos_valid || now - ac->pos_ts_us > AIRCRAFT_SHOW_US) continue;
        double b, d;
        ls_geo_bearing_range(rlat, rlon, ac->lat, ac->lon, &b, &d);
        int j = m++;
        while (j > 0 && dist[j - 1] > d) { dist[j] = dist[j - 1]; order[j] = order[j - 1]; j--; }
        dist[j] = d; order[j] = s;
    }
    for (int k = 0; k < m; k++) {
        const adsb_aircraft_t *ac = adsb_state_get(order[k]);
        if (ac->callsign[0]) snprintf(label, sizeof(label), "AIR %.8s", ac->callsign);
        else snprintf(label, sizeof(label), "AIR %06lX", (unsigned long)ac->icao);
        range_words(rng, sizeof(rng), rlat, rlon, ac->lat, ac->lon);
        snprintf(detail, sizeof(detail), "%s  %dft", rng, ac->altitude);
        goto_add(HIT_AIR, ac->icao, ac->lat, ac->lon, NULL, label, detail);
    }
    ls_mesh_peer_t p;
    for (int r = 0; r < LS_MESH_MAX_PEERS && ls_mesh_peer_at(r, &p); r++) {
        if (!p.has_loc) continue;
        snprintf(label, sizeof(label), "NODE %.20s", p.name[0] ? p.name : p.id);
        range_words(rng, sizeof(rng), rlat, rlon, p.lat_e6 / 1e6, p.lon_e6 / 1e6);
        char ago[12];
        age_words(ago, sizeof(ago), node_age(&p));
        snprintf(detail, sizeof(detail), "%s  %s ago", rng, ago);
        goto_add(HIT_NODE, 0, p.lat_e6 / 1e6, p.lon_e6 / 1e6, p.id, label, detail);
    }
    for (int i = 0; i < ls_marks_count(); i++) {
        const ls_mark_t *mk = ls_marks_at(i);
        snprintf(label, sizeof(label), "%c %.24s", ls_mark_icon_char(mk->icon), mk->name);
        range_words(rng, sizeof(rng), rlat, rlon, mk->lat, mk->lon);
        snprintf(detail, sizeof(detail), "marker  %s", rng);
        goto_add(HIT_MARK, (uint32_t)i, mk->lat, mk->lon, NULL, label, detail);
    }
    for (int i = 0; i < ls_sketch_count(); i++) {
        const ls_sketch_t *s = ls_sketch_at(i);
        snprintf(label, sizeof(label), "~ %.24s", s->name);
        snprintf(detail, sizeof(detail), "line  %.2f mi", ls_sketch_length_m(s) / LS_GEO_M_PER_MILE);
        goto_add(HIT_LINE, (uint32_t)i, s->lat[0], s->lon[0], NULL, label, detail);
    }
    /* Places named in the current view, most important first. */
    const carto_label *L = NULL;
    const int n = ls_map_labels(&L);
    if (L && n > 0 && !ls_map_status()) {
        ls_map_render(&s_find_pw, &s_find_ph);
        EXT_RAM_BSS_ATTR static int ord[64];
        int mm = 0;
        for (int i = 0; i < n && i < 64; i++) {
            int j = mm++;
            while (j > 0 && importance(&L[ord[j - 1]]) < importance(&L[i])) { ord[j] = ord[j - 1]; j--; }
            ord[j] = i;
        }
        for (int k = 0; k < mm; k++) {
            const carto_label *lb = &L[ord[k]];
            double lat, lon;
            frame_ll(lb->x, lb->y, s_find_pw, s_find_ph, &lat, &lon);
            range_words(rng, sizeof(rng), rlat, rlon, lat, lon);
            snprintf(detail, sizeof(detail), "place  %s", rng);
            goto_add(0, 0, lat, lon, NULL, lb->text, detail);
        }
    }
    if (!s_ngoto) ls_picker_empty_reason("nothing to go to yet: no aircraft, nodes, markers or places in view");
    static char note[40];
    snprintf(note, sizeof(note), "%d to choose from", s_ngoto);
    out->kind = LS_VAL_TEXT;
    out->s = note;
    return LS_ACT_OK;
}

/* Editing a marker or a line: one list of what can be done to it. */
static int s_edit_index;
static sel_kind_t s_edit_kind;

static void open_app(const char *id)
{
    const ls_app_t *app = ls_app_by_id(id);
    const int idx = (app && app->screen) ? ls_tui_screen_index_of(app->screen) : -1;
    if (idx >= 0) ls_tui_screen_show(idx);
}

static void save_or_say(const char *done)
{
    if (ls_marks_save()) say(done);
    else say(ls_marks_error() ? ls_marks_error() : "not saved");
}

static void rename_done(const char *text)
{
    const bool ok = s_edit_kind == SEL_MARK ? ls_marks_rename(s_edit_index, text)
                                            : ls_sketch_rename(s_edit_index, text);
    if (ok) save_or_say("renamed");
}

static void icon_done(int i)
{
    if (ls_marks_set_icon(s_edit_index, i)) save_or_say("symbol changed");
}

static void edit_done(int i)
{
    if (s_edit_kind == SEL_MARK) {
        const ls_mark_t *m = ls_marks_at(s_edit_index);
        if (!m) return;
        switch (i) {
        case 0: jump_to(m->lat, m->lon); break;
        case 1:
            ls_compass_set_target(m->lat, m->lon, m->name);
            open_app("compass");
            break;
        case 2: ls_keyboard_open("MARKER NAME", m->name, LS_MARK_NAME - 1, rename_done); break;
        case 3:
            ls_picker_open("SYMBOL", icon_done);
            for (int k = 0; k < LS_MARK__COUNT; k++) {
                char lab[24];
                snprintf(lab, sizeof(lab), "%c  %s", ls_mark_icon_char(k), ls_mark_icon_name(k));
                ls_picker_add(lab, k == m->icon ? "current" : "");
            }
            break;
        case 4: {
            char body[96];
            snprintf(body, sizeof(body), "> MAP %.6f, %.6f  z%d", m->lat, m->lon, ls_map_zoom());
            say(ls_notes_mark(m->name, body) ? "saved to NOTES" : "NOTES could not take it");
            break;
        }
        case 5:
            ls_marks_delete(s_edit_index);
            select_none();
            save_or_say("marker deleted");
            break;
        default: break;
        }
        return;
    }
    const ls_sketch_t *s = ls_sketch_at(s_edit_index);
    if (!s) return;
    switch (i) {
    case 0: jump_to(s->lat[0], s->lon[0]); break;
    case 1: ls_keyboard_open("LINE NAME", s->name, LS_MARK_NAME - 1, rename_done); break;
    case 2: {
        char body[96];
        snprintf(body, sizeof(body), "> MAP %.6f, %.6f  z%d\n%.2f mi, %d points", s->lat[0], s->lon[0],
                 ls_map_zoom(), ls_sketch_length_m(s) / LS_GEO_M_PER_MILE, s->n);
        say(ls_notes_mark(s->name, body) ? "saved to NOTES" : "NOTES could not take it");
        break;
    }
    case 3:
        ls_sketch_delete(s_edit_index);
        select_none();
        save_or_say("line deleted");
        break;
    default: break;
    }
}

static void open_edit(void)
{
    if (s_sel == SEL_MARK && ls_marks_at(s_sel_index)) {
        s_edit_kind = SEL_MARK;
        s_edit_index = s_sel_index;
        ls_picker_open(ls_marks_at(s_sel_index)->name, edit_done);
        ls_picker_add("Go to it", "centre the map");
        ls_picker_add("Navigate with COMPASS", "sets the GO TO target");
        ls_picker_add("Rename", "");
        ls_picker_add("Change symbol", ls_mark_icon_name(ls_marks_at(s_sel_index)->icon));
        ls_picker_add("Save to NOTES", "as a MAP line");
        ls_picker_add("Delete", "");
    } else if (s_sel == SEL_LINE && ls_sketch_at(s_sel_index)) {
        s_edit_kind = SEL_LINE;
        s_edit_index = s_sel_index;
        ls_picker_open(ls_sketch_at(s_sel_index)->name, edit_done);
        ls_picker_add("Go to its start", "centre the map");
        ls_picker_add("Rename", "");
        ls_picker_add("Save to NOTES", "start point and length");
        ls_picker_add("Delete", "");
    }
}

/* LAYERS: a list of switches that stays open while they are flipped. */
static ls_act_status_t a_map_layers(const ls_args_t *in, ls_val_t *out);

static const struct { uint32_t bit; const char *name; const char *what; } LAYER_ROWS[] = {
    { L_AIR,     "Aircraft",       "ADS-B positions" },
    { L_TRAILS,  "Flight trails",  "where each has been" },
    { L_VECTORS, "Speed leaders",  "where each will be" },
    { L_BLOCKS,  "Data blocks",    "altitude and speed labels" },
    { L_NOLABEL, "Hide labels",    "symbols only" },
    { L_MESH,    "Mesh nodes",     "peers with a position" },
    { L_APRS,    "APRS stations",  "stations and weather" },
    { L_AIS,     "AIS vessels",    "vessel positions, course" },
    { L_SONDE,   "SONDE",          "RS41 tracks and last fix" },
    { L_LINKS,   "Mesh links",     "lines to nodes heard lately" },
    { L_ROUTE,   "Route",          "planned and walked path" },
    { L_MARKS,   "Markers, lines", "placed by hand" },
    { L_RINGS,   "Range rings",    "distance from here" },
    { L_COVER,   "Coverage",       "furthest aircraft by bearing" },
    { L_PLACES,  "Place names",    "from the map" },
};
#define N_LAYER_ROWS ((int)(sizeof(LAYER_ROWS) / sizeof(LAYER_ROWS[0])))

static void layer_pick(int i)
{
    if (i < 0 || i >= N_LAYER_ROWS) return;
    layers_set(s_layers ^ LAYER_ROWS[i].bit);
    ls_val_t out;
    a_map_layers(NULL, &out);
}

static ls_act_status_t a_map_layers(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    ls_picker_open("LAYERS", layer_pick);
    for (int i = 0; i < N_LAYER_ROWS; i++) {
        char d[LS_PICKER_DETAIL];
        snprintf(d, sizeof(d), "%s  %s", layer(LAYER_ROWS[i].bit) ? "[ON] " : "[off]", LAYER_ROWS[i].what);
        ls_picker_add(LAYER_ROWS[i].name, d);
    }
    out->kind = LS_VAL_TEXT;
    out->s = "tap a layer to switch it";
    return LS_ACT_OK;
}

/* STYLE: the map's palette, and how the picture is filled. */
static uint32_t style_bits(void)
{
    return ((uint32_t)(s_palette & 0x0F) << 24) | ((uint32_t)(s_view & 0x07) << 28);
}

static void palette_load(void)
{
    const uint32_t v = settings_get_map_layers(LAYERS_DEFAULT);
    const int p = (int)((v >> 24) & 0x0F), w = (int)((v >> 28) & 0x07);
    s_palette = p < N_PALETTES ? p : 0;
    s_view = w < MAP_VIEW__COUNT ? (map_view_t)w : MAP_VIEW_FIELD;
}

static const char *const VIEW_WHAT[MAP_VIEW__COUNT] = {
    "the picture itself", "coloured blocks of text cells", "outlines drawn in characters", "active CTILE, smooth quadrants", "active CTILE, smooth braille dots" };

static void style_pick(int i)
{
    if (i == N_PALETTES + MAP_VIEW__COUNT) { ls_val_t out; a_map_files(NULL, &out); return; }
    if (i >= 0 && i < N_PALETTES) s_palette = i;
    else if (i >= N_PALETTES && i < N_PALETTES + MAP_VIEW__COUNT) s_view = (map_view_t)(i - N_PALETTES);
    else return;
    if(!cartocore_view()) ls_carto_map_leave();
    layers_set(s_layers);
    say(i < N_PALETTES ? PALETTES[i].what : VIEW_WHAT[s_view]);
}

static void theme_style_pick(int i) {
    if(i==0 || i==1) style_pick(i?6:0);
    else if(i>=2) style_pick(i+N_PALETTES-2);
}
static ls_act_status_t a_map_style(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    ls_picker_open("MAP STYLE", theme_style_pick);
    ls_picker_add("Theme fills",s_palette<6?"[ON] follows TUI theme":"follows TUI theme");
    ls_picker_add("Theme outlines",s_palette>=6?"[ON] PMTiles coast and major roads":"PMTiles coast and major roads");
    for (int i = 0; i < MAP_VIEW__COUNT; i++) {
        char lab[LS_PICKER_TEXT], d[LS_PICKER_DETAIL];
        snprintf(lab, sizeof(lab), "Fill: %s", VIEW_NAME[i]);
        snprintf(d, sizeof(d), "%s%s", i == (int)s_view ? "[ON] " : "", VIEW_WHAT[i]);
        ls_picker_add(lab, d);
    }
    const char *arch = ls_map_archive();
    const char *leaf = arch ? strrchr(arch, '/') : NULL;
    ls_picker_add("Map file on the card", leaf ? leaf + 1 : "none open");
    out->kind = LS_VAL_TEXT;
    out->s = "choose a fill; colours follow TUI theme";
    return LS_ACT_OK;
}

/* ------------------------------------------------------------- actions -- */

static int s_pw, s_ph;      /* the frame the last draw used */

static ls_act_status_t a_map_mark(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    double lat, lon;
    ls_map_get_center(&lat, &lon);
    const int i = ls_marks_add(lat, lon, LS_MARK_STAR, NULL);
    out->kind = LS_VAL_TEXT;
    if (i < 0) { out->s = "marker list is full"; say(out->s); return LS_ACT_UNAVAILABLE; }
    select_mark(SEL_MARK, i);
    if (!layer(L_MARKS)) layers_set(s_layers | L_MARKS);
    static char msg[48];
    if (ls_marks_save()) snprintf(msg, sizeof(msg), "%s dropped; E edits it", ls_marks_at(i)->name);
    else snprintf(msg, sizeof(msg), "%s", ls_marks_error() ? ls_marks_error() : "not saved");
    say(msg);
    out->s = msg;
    return LS_ACT_OK;
}

static ls_act_status_t a_map_draw(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    ls_sketch_begin();
    if (!layer(L_MARKS)) layers_set(s_layers | L_MARKS);
    say("DRAW: tap the map or ADD POINT; DONE keeps it");
    out->kind = LS_VAL_TEXT;
    out->s = "drawing a line";
    return LS_ACT_OK;
}

static ls_act_status_t a_map_draw_add(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    double lat, lon;
    ls_map_get_center(&lat, &lon);
    out->kind = LS_VAL_TEXT;
    out->s = ls_sketch_add_point(lat, lon) ? "point added" : "no room for another point";
    return LS_ACT_OK;
}

static ls_act_status_t a_map_draw_undo(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    out->kind = LS_VAL_TEXT;
    out->s = ls_sketch_undo() ? "last point removed" : "nothing to undo";
    return LS_ACT_OK;
}

static ls_act_status_t a_map_draw_done(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    const int i = ls_sketch_finish();
    out->kind = LS_VAL_TEXT;
    if (i < 0) { out->s = "a line needs two points"; say(out->s); return LS_ACT_OK; }
    select_mark(SEL_LINE, i);
    save_or_say("line kept");
    out->s = "line kept";
    return LS_ACT_OK;
}

static ls_act_status_t a_map_draw_cancel(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    ls_sketch_cancel();
    say("line discarded");
    out->kind = LS_VAL_TEXT;
    out->s = "line discarded";
    return LS_ACT_OK;
}

/* FOLLOW keeps the selected aircraft or node in view; with nothing
   selected it follows this receiver's GPS, as it always did. */
static ls_act_status_t a_map_track(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    out->kind = LS_VAL_TEXT;
    if (s_sel == SEL_AIR || s_sel == SEL_NODE) {
        s_track = !s_track;
        if (s_track) ls_map_follow_set(false);
        out->s = s_track ? "following the selection" : "follow off";
        say(out->s);
        return LS_ACT_OK;
    }
    s_track = false;
    const bool enable = !ls_map_following();
    if (enable && ls_gps_start() != ESP_OK) {
        out->s = "GPS receiver unavailable";
        return LS_ACT_UNAVAILABLE;
    }
    ls_map_follow_set(enable);
    out->s = enable ? "following GPS; waiting for a fresh fix" : "GPS follow off";
    say(out->s);
    return LS_ACT_OK;
}

/* ROUTE keeps file names in PSRAM; the picker lends their labels to the UI. */
#define ROUTE_DIR "/sdcard/lakeshark/routes"
EXT_RAM_BSS_ATTR static char s_routes[LS_PICKER_MAX-1][128];
static int s_route_n;
static void route_pick(int i)
{
    if (i<0 || i>s_route_n) return;
    bool ok=ls_route_live_load(i ? s_routes[i-1] : NULL,i==0);
    if (!ok) { say("route unavailable, invalid, or too large"); return; }
    layers_set(s_layers|L_ROUTE);
    const ls_route_t *r=ls_route_live();
    s_track=false;
    ls_map_center(r->point[0].lat,r->point[0].lon);
    ls_map_follow_set(true);
    say(i ? "route loaded; waiting for GPS" : "backtracking saved walk; waiting for GPS");
}
static ls_act_status_t a_map_route(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    s_route_n=0;
    DIR *d=opendir(ROUTE_DIR);
    const struct dirent *e;
    if (d) {
        while ((e=readdir(d)) && s_route_n<LS_PICKER_MAX-1) {
            size_t n=strlen(e->d_name);
            if (n<5 || strcasecmp(e->d_name+n-4,".gpx") ||
                n+sizeof(ROUTE_DIR)+1>sizeof(s_routes[0])) continue;
            snprintf(s_routes[s_route_n++],sizeof(s_routes[0]),"%s/%.*s",ROUTE_DIR,(int)n,e->d_name);
        }
        closedir(d);
    }
    qsort(s_routes,s_route_n,sizeof(s_routes[0]),archive_order);
    ls_picker_open("ROUTE",route_pick);
    ls_picker_add("BACKTRACK","reverse current / last recorded walk");
    for (int i=0;i<s_route_n;i++) ls_picker_add(s_routes[i]+sizeof(ROUTE_DIR),"follow GPX");
    out->kind=LS_VAL_TEXT;out->s="GPX: SD /lakeshark/routes; or BACKTRACK";
    return LS_ACT_OK;
}
static ls_act_status_t a_route_options(const ls_args_t *in, ls_val_t *out)
{
    (void)in;ls_opt_open(ls_route_options());
    out->kind=LS_VAL_TEXT;out->s="route options";return LS_ACT_OK;
}

static void register_view_action(void)
{
    static bool done;
    if (done) return;
    done = ls_action_register("map.view", "", LS_CAP_UI, a_map_view,
                              "field, blocks, or line art");
    ls_action_register("map.route", "", LS_CAP_UI, a_map_route, "follow GPX or backtrack a walk");
    ls_action_register("map.route_options", "", LS_CAP_UI, a_route_options, "route guidance settings");
    ls_action_register("map.files", "", LS_CAP_UI, a_map_files, "choose an SD map");
    ls_action_register("map.find", "", LS_CAP_UI, a_map_find,
                       "aircraft, nodes, markers and places to go to");
    ls_action_register("map.layers", "", LS_CAP_UI, a_map_layers, "what is drawn over the map");
    ls_action_register("map.style", "", LS_CAP_UI, a_map_style, "map fill; colours follow TUI theme");
    ls_action_register("map.mark", "", LS_CAP_UI, a_map_mark, "drop a marker at the centre");
    ls_action_register("map.draw", "", LS_CAP_UI, a_map_draw, "draw a line point by point");
    ls_action_register("map.draw_add", "", LS_CAP_UI, a_map_draw_add, "add the centre to the line");
    ls_action_register("map.draw_undo", "", LS_CAP_UI, a_map_draw_undo, "remove the last point");
    ls_action_register("map.draw_done", "", LS_CAP_UI, a_map_draw_done, "keep the line");
    ls_action_register("map.draw_cancel", "", LS_CAP_UI, a_map_draw_cancel, "discard the line");
    ls_action_register("map.track", "", LS_CAP_UI, a_map_track,
                       "follow the selection, or GPS with nothing selected");
}

/* --------------------------------------------------------- the preview -- */

void ls_map_preview_frame(tui_rect area, int *pw, int *ph, int *tile_px)
{
    int cw=10;
    ls_tui_geometry(NULL,NULL,&cw,NULL);
    if (pw) *pw=area.w*SUB_X;
    if (ph) *ph=area.h*sub_y();
    if (tile_px) *tile_px=cartocore_view()?256:256*SUB_X/(cw>0?cw:10);
}

void ls_map_preview(tui_surface *sf, tui_rect area, double lat, double lon)
{
    if (area.w<1 || area.h<1) return;
    if (!cartocore_view() && !s_opened) { s_opened=true; rescan(); }
    /* The room the labels over this take starts empty every frame, picture
       or not: with no archive there is nothing to draw, but the aircraft and
       their callsigns still come, and the last frame's list would leave them
       nowhere to go after a few frames. */
    overlay_reset(area,tui_rect_make(0,0,0,0),tui_rect_make(0,0,0,0));
    ls_map_center(lat,lon);
    int pw=0,ph=0,tile=256;
    ls_map_preview_frame(area,&pw,&ph,&tile);
    ls_map_set_tile_px(tile);
    if(cartocore_view()) {
        ls_map_external_view(pw,ph);s_opened=s_have_archive=false;
        unsigned z=ls_map_zoom();
        if(ls_carto_map_prepare(&lat,&lon,&z)) {
            ls_map_center(lat,lon);ls_map_zoom_by((int)z-ls_map_zoom());
        }
        palette();
        if(ls_carto_map_draw(sf,area,lat,lon,z,s_view==MAP_VIEW_CC_BRAILLE,layer(L_PLACES)))
            ls_tui_basemap(sf,area,s_map_colours);
        return;
    }
    if (!ls_map_begin(pw,ph)) return;
    const uint16_t *px=ls_map_render(&pw,&ph);
    if (px) { draw_cells(sf,area,px,pw,ph);ls_tui_basemap(sf,area,s_map_colours); }
}

void ls_map_preview_reserve(tui_rect area,int x,int y,int width)
{
    box_take(x-area.x,x-area.x+width-1,y-area.y);
}

void ls_map_preview_labels(tui_surface *sf,tui_rect area)
{
    if(!cartocore_view()) draw_labels(sf,area,SUB_X,sub_y(),tui_rect_make(0,0,0,0));
}

bool ls_map_preview_point(double lat,double lon,tui_rect area,int *x,int *y)
{
    int cx,cy;
    if (!map_cell_of(lat,lon,area,area.w*SUB_X,area.h*sub_y(),&cx,&cy)) return false;
    *x=area.x+cx;*y=area.y+cy;
    return true;
}

/* The ADS-B screen's mini map: the same aircraft, trails and labels as the
   full map, over the preview ls_map_preview just drew. Returns the plotted
   aircraft so the screen can hit-test them. */
int ls_map_preview_air(tui_surface *sf, tui_rect area, ls_map_plot_t *plots, int max)
{
    const int pw = area.w * SUB_X, ph = area.h * sub_y();
    const int64_t now = esp_timer_get_time();
    refresh_receiver(now);
    const uint32_t keep_icao = s_sel_icao;
    const sel_kind_t keep_sel = s_sel;
    const uint32_t keep_layers = s_layers;
    const uint32_t adsb_sel = adsb_select_get_icao();
    s_sel = adsb_sel ? SEL_AIR : SEL_NONE;
    s_sel_icao = adsb_sel;
    s_layers = (s_layers | L_AIR) & ~(uint32_t)L_BLOCKS;
    s_vector_s = vector_seconds();
    s_nhits = 0;
    ls_ink_begin(area);
    ls_ink_set_tint(palette()->tint);
    s_nplaced = 0;
    receiver_canvas(area, pw, ph);
    air_prepare(area, pw, ph, now);
    air_canvas(area, pw, ph, now);
    s_npeers = 0;
    edges_collect(pw, ph);
    if (s_nedges > 1) s_nedges = 1;      /* the selection only: the pane is small */
    edges_canvas(area, pw, ph);
    ls_ink_flush(sf, tui_rect_make(0, -1, 0, 0));
    claim_ink(area);
    receiver_text(sf, area, pw, ph);
    air_text(sf, area);
    edges_text(sf, area, pw, ph);
    int n = 0;
    for (int i = 0; i < s_nhits && n < max; i++)
        if (s_hits[i].kind == HIT_AIR) plots[n++] = (ls_map_plot_t){ s_hits[i].x, s_hits[i].y, s_hits[i].id };
    s_sel = keep_sel;
    s_sel_icao = keep_icao;
    s_layers = keep_layers;
    return n;
}

void ls_map_preview_leave(void) { leave(); }

/* ---------------------------------------------------------------- pad -- */

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

/* ---------------------------------------------------------------- draw -- */

/* Keep the followed target in view: recentre only when it leaves the middle
   third, so a render is not thrown away every frame. */
static void track_selection(int pw, int ph)
{
    if (!s_track || ls_map_render_busy()) return;
    double lat = 0, lon = 0, fx, fy;
    bool have = false;
    if (s_sel == SEL_AIR) {
        for (int s = 0; s < ADSB_MAX_TRACKED; s++)
            if (s_air[s].on && adsb_state_get(s)->icao == s_sel_icao) { lat = s_air[s].lat; lon = s_air[s].lon; have = true; }
    } else if (s_sel == SEL_NODE) {
        for (int i = 0; i < s_npeers; i++)
            if (s_map_peers[i].has_loc && !strcmp(s_map_peers[i].id, s_sel_node)) {
                lat = s_map_peers[i].lat_e6 / 1e6; lon = s_map_peers[i].lon_e6 / 1e6; have = true;
            }
    }
    if (!have || !frame_px(lat, lon, pw, ph, &fx, &fy)) return;
    if (fabs(fx - pw / 2.0) > pw / 6.0 || fabs(fy - ph / 2.0) > ph / 6.0) ls_map_center(lat, lon);
}

/* The selection can disappear under the screen: an aircraft ages out, a
   marker is deleted. */
static void check_selection(void)
{
    if (s_sel == SEL_AIR) {
        bool found = false;
        for (int s = 0; s < ADSB_MAX_TRACKED; s++) {
            const adsb_aircraft_t *ac = adsb_state_get(s);
            if (ac && ac->active && ac->icao == s_sel_icao) found = true;
        }
        if (!found) select_none();
    } else if (s_sel == SEL_MARK && !ls_marks_at(s_sel_index)) select_none();
    else if (s_sel == SEL_LINE && !ls_sketch_at(s_sel_index)) select_none();
}

static void centre_reticle(tui_surface *sf, tui_rect body)
{
    if (s_controls_hidden && !ls_sketch_drawing()) return;
    const int cx = body.w / 2, cy = body.h / 2;
    if (!box_free(cx, cx, cy)) return;
    glass(sf, body, body.x + cx, body.y + cy, '+',
                     ls_sketch_drawing() ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
}

static int count_air_positions(void)
{
    int n = 0;
    for (int s = 0; s < ADSB_MAX_TRACKED; s++) if (s_air[s].on) n++;
    return n;
}

/* Clip in floating point before converting map pixels to ink dots. A long,
   decimated leg may cross the pane with both endpoints outside it. */
static void route_line(double x0,double y0,double x1,double y1,int pw,int ph,uint8_t attr)
{
    double dx=x1-x0,dy=y1-y0,lo=0,hi=1;
    const double p[]={-dx,dx,-dy,dy}, q[]={x0,pw-1-x0,y0,ph-1-y0};
    for (int k=0;k<4;k++) {
        if (fabs(p[k])<1e-9) { if (q[k]<0) return; continue; }
        double t=q[k]/p[k];
        if (p[k]<0) lo=fmax(lo,t); else hi=fmin(hi,t);
    }
    if (lo>hi) return;
    int ax,ay,bx,by;
    px_dot(x0+lo*dx,y0+lo*dy,&ax,&ay);
    px_dot(x0+hi*dx,y0+hi*dy,&bx,&by);
    ls_ink_line(ax,ay,bx,by,attr,4,0);
}
static void route_canvas(int pw,int ph)
{
    const ls_route_t *r=ls_route_live();
    if (!r->valid || !layer(L_ROUTE) || pw<1 || ph<1) return;
    const uint8_t bright=TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK),dim=LS_ATTR_DIM;
    for (size_t i=1;i<r->n;i++) {
        const ls_route_point_t *a=&r->point[i-1],*b=&r->point[i];
        double x0,y0,x1,y1;
        if (b->start || !frame_px(a->lat,a->lon,pw,ph,&x0,&y0) ||
            !frame_px(b->lat,b->lon,pw,ph,&x1,&y1)) continue;
        if (r->arrived || b->along_m<=r->walked_m) route_line(x0,y0,x1,y1,pw,ph,dim);
        else if (a->along_m<r->walked_m && b->along_m>a->along_m) {
            double f=(r->walked_m-a->along_m)/(b->along_m-a->along_m);
            double x=x0+f*(x1-x0),y=y0+f*(y1-y0);
            route_line(x0,y0,x,y,pw,ph,dim);route_line(x,y,x1,y1,pw,ph,bright);
        } else route_line(x0,y0,x1,y1,pw,ph,bright);
    }
    if (r->located && !r->arrived) {
        double fx,fy;
        if (frame_px(r->point[r->next].lat,r->point[r->next].lon,pw,ph,&fx,&fy) &&
            fx>=0 && fx<pw && fy>=0 && fy<ph) {
            int x,y;px_dot(fx,fy,&x,&y);
            int radius=2+ls_motion_phase(2,600);
            ls_ink_ellipse(x,y,radius,radius,bright,6,0);
        }
    }
}

static void draw(tui_surface *sf, tui_rect area)
{
    if(ls_place_search_active()) { ls_place_search_draw(sf,area);return; }
    const int64_t now = esp_timer_get_time();
    refresh_receiver(now);
    ls_map_follow_fix(s_receiver_fresh, s_receiver.lat_deg, s_receiver.lon_deg,
                      s_receiver.last_fix_us, now);
    check_selection();

    int nq = 0;
    const ls_quick_t *quick = quick_table(&nq);
    const int want = ls_quick_rows(quick, nq, area.w, ls_tui_is_wide());
    const int ctl_h = (!s_controls_hidden && area.h > want + 10) ? want : 0;
    const bool guidance=ls_route_live()->valid;
    s_header_h = (ls_tui_is_wide() ? 1 : 3)+(guidance ? 1 : 0);
    tui_rect body = tui_rect_make(area.x, area.y + s_header_h, area.w, area.h - ctl_h - s_header_h);

    s_quick_rect = ctl_h
        ? tui_rect_make(area.x, body.y + body.h, area.w, ctl_h)
        : tui_rect_make(0, -1, 0, 0);

    /* The tile shrinks with the buffer, or the same rectangle covers
       a fifth of the ground and the map is silently five zoom steps in. */
    int cw = 10;
    ls_tui_geometry(NULL, NULL, &cw, NULL);
    ls_map_set_tile_px(cartocore_view()?256:256 * SUB_X / (cw > 0 ? cw : 10));
    bool cc_ready=true;
    if(cartocore_view()) {
        ls_map_external_view(body.w*2,body.h*4);
        s_have_archive=false;s_opened=false;s_file_error=NULL;cells_free();
        double lat,lon;ls_map_get_center(&lat,&lon);unsigned z=ls_map_zoom();
        cc_ready=ls_carto_map_prepare(&lat,&lon,&z);
        if(cc_ready) { ls_map_center(lat,lon);ls_map_zoom_by((int)z-ls_map_zoom()); }
    } else {
        if(!s_opened) { rescan();s_opened=true; }
        ls_map_begin(body.w*SUB_X,body.h*sub_y());
    }

    nodes_prepare();
    s_basemap_busy=(layer(L_AIR) && count_air_positions()>0) ||
        (layer(L_MESH) && s_npeers>0) || (layer(L_MARKS) && ls_marks_count()>0) ||
        s_receiver_fresh || ls_route_live()->valid;
    palette();
    /* Render FIRST, then ask what is wrong. The other order asks a map that
       has not been started why it is not drawable. */
    int pw = 0, ph = 0;
    const bool cc=cartocore_view();
    const uint16_t *px = cc?NULL:ls_map_render(&pw, &ph);
    bool cc_ok=false;
#ifdef LS_MAP_AUDIT
    s_carto_audit_count=0;s_carto_audit_bad=0;s_carto_low_masked=0;
#endif
    if(cc) {
        double lat,lon; ls_map_get_center(&lat,&lon);
        pw=body.w*2; ph=body.h*4;
        ls_tui_reserve(tui_rect_make(0,0,0,0));
        ls_tui_image(tui_rect_make(0,0,0,0),NULL,0,0,0);
        cc_ok=ls_carto_map_draw(sf,body,lat,lon,ls_map_zoom(),s_view==MAP_VIEW_CC_BRAILLE,layer(L_PLACES));
    }
    const char *why = cc?(cc_ok?NULL:ls_carto_map_status()):ls_map_status();
    s_pw = pw; s_ph = ph;

    s_vector_s = vector_seconds();
    air_prepare(body, pw, ph, now);
    int demand=1;
    if(layer(L_AIR)) for(int i=0;i<ADSB_MAX_TRACKED;i++) if(s_air[i].inside && s_air[i].on) demand++;
    if(layer(L_MESH)) demand+=s_npeers;
    if(layer(L_MARKS)) demand+=ls_marks_count();
    int capacity=body.w*body.h/160;
    if(demand>capacity+2) s_map_crowded=true;
    else if(demand<capacity-2) s_map_crowded=false;
    nodes_prepare();
    cover_update();
    track_selection(pw, ph);

    /* The header: what is on the map and what the marks mean. */
    const uint8_t head = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    s_controls_hit = tui_rect_make(area.x + area.w - 13, area.y, 13, s_header_h);
    tui_rect heading = area;
    heading.w -= 14;
    const char *gps = s_track ? "TRACK" : !s_receiver_fresh ? "WAIT" : ls_map_following() ? "FOLLOW" : "LIVE";
    char title[128];
    int nmesh = 0;
    for (int i = 0; i < s_npeers; i++) if (s_map_peers[i].has_loc) nmesh++;
    char rings[16] = "";
    if (s_ring_nm > 0) snprintf(rings, sizeof(rings), "  rings %gnm", s_ring_nm);
    if (ls_tui_is_wide()) {
        snprintf(title, sizeof(title), "[%s %s/S]  AIR %d  MESH %d  MARK %d%s  vec %ds  GPS %s",
                 palette()->name, VIEW_NAME[s_view], count_air_positions(), nmesh, ls_marks_count(),
                 rings, s_vector_s, gps);
        tui_put_str(sf, heading, area.x, area.y, title, head);
    } else {
        snprintf(title, sizeof(title), "[ %s %s ]  %s", palette()->name, VIEW_NAME[s_view],
                 ls_sketch_drawing() ? "DRAWING" : "MAP");
        tui_put_str(sf, heading, area.x, area.y, title, head);
        char b[24];
        snprintf(b, sizeof(b), "AIR %d", count_air_positions());
        tui_put_str(sf, area, area.x, area.y + 1, b, TUI_ATTR(TUI_YELLOW, TUI_BLACK));
        snprintf(b, sizeof(b), "MESH %d", nmesh);
        tui_put_str(sf, area, area.x + 8, area.y + 1, b, TUI_ATTR(TUI_MAGENTA, TUI_BLACK));
        snprintf(b, sizeof(b), "MARK %d", ls_marks_count());
        tui_put_str(sf, area, area.x + 16, area.y + 1, b, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
        snprintf(b, sizeof(b), "GPS %s", gps);
        tui_put_str(sf, area, area.x + 24, area.y + 1, b, TUI_ATTR(TUI_CYAN, TUI_BLACK));
    }
    tui_put_str(sf, area, s_controls_hit.x, area.y, s_controls_hidden ? "[SHOW KEYS]" : "[HIDE KEYS]",
                TUI_ATTR(TUI_BLACK, TUI_CYAN));

    s_nhits = 0;
    if (!px && !cc_ok) {
        ls_tui_basemap(NULL,tui_rect_make(0,0,0,0),NULL);
        draw_placeholder(sf, body, why ? why : "the map has not been started");
        s_pad_rect = tui_rect_make(0, -1, 0, 0);
        s_card_rect = tui_rect_make(0, -1, 0, 0);
    } else if (why && !ls_map_render_busy()) {
        /* A part drawn frame is not an empty one: while tiles are still
           arriving the picture is worth showing, gaps and all, and "nothing
           here" waits until there is nothing left to arrive. */
        draw_placeholder(sf, body, why);
        s_pad_rect = tui_rect_make(0, -1, 0, 0);
        s_card_rect = tui_rect_make(0, -1, 0, 0);
    } else if(!cc || ls_carto_map_fresh()) {
        s_pad_rect = s_controls_hidden ? tui_rect_make(0, -1, 0, 0) : pad_rect_for(body);
        s_card_rect = card_rect_for(body);
        if(!cc) draw_cells(sf, body, px, pw, ph);
        ls_tui_basemap(sf,body,s_map_colours);
        overlay_reset(body, s_pad_rect, s_card_rect);

        /* Underneath to on top: rings and coverage, lines and trails, then
           symbols, then every label stepping around every symbol. */
        ls_ink_begin(body);
        ls_ink_set_tint(palette()->tint);
        s_nplaced = 0;
        rings_canvas(sf, body, pw, ph, false);
        cover_canvas(pw, ph);
        route_canvas(pw,ph);
        marks_canvas(body, pw, ph, now);
        nodes_canvas(body, pw, ph);
        aprs_canvas(body, pw, ph, now);
        ais_canvas(body, pw, ph, now);
        sonde_canvas(body, pw, ph, now);
        receiver_canvas(body, pw, ph);
        air_canvas(body, pw, ph, now);
        edges_collect(pw, ph);
        edges_canvas(body, pw, ph);
        ls_ink_flush(sf, s_card_rect);
        claim_ink(body);

        receiver_text(sf, body, pw, ph);
        air_text(sf, body);
        nodes_text(sf, body, pw, ph);
        aprs_text(sf, body, pw, ph, now);
        ais_text(sf, body, pw, ph, now);
        sonde_text(sf, body, pw, ph, now);
        marks_text(sf, body, pw, ph);
        edges_text(sf, body, pw, ph);
        if(cc && layer(L_PLACES)) draw_carto_labels(sf,body);
        rings_canvas(sf, body, pw, ph, true);
        if (!cc && layer(L_PLACES)) draw_labels(sf, body, SUB_X, sub_y(), s_pad_rect);
        centre_reticle(sf, body);
        draw_card(sf, s_card_rect);

        s_map_cells = body;


        draw_pan_pad(sf, s_pad_rect);
    }

    double lat = 0, lon = 0;
    ls_map_get_center(&lat, &lon);
    /* A turning mark while tiles are still arriving, so a gap reads as
       "still drawing" and not as "that is all there is". */
    const bool busy = !cc && ls_map_render_busy();
    const char pip = busy ? ls_motion_pip(true) : ' ';
    const double width_m = 40075016.686 * cos(lat * M_PI / 180.0) * pw /
        (ldexp(1.0, ls_map_zoom()) * ls_map_tile_px());
    const ls_sketch_t *open = ls_sketch_open();
    if (open)
        snprintf(s_note, sizeof(s_note), "DRAW %d pts %.2f mi  tap adds  U undo  O done  C cancel",
                 open->n, ls_sketch_length_m(open) / LS_GEO_M_PER_MILE);
    else if (s_toast[0] && now - s_toast_us < 4000000)
        snprintf(s_note, sizeof(s_note), "%s", s_toast);
    else
        snprintf(s_note, sizeof(s_note), "%c %s z%d%s %.2f mi across", pip, busy ? "LOADING" : "OFFLINE",
                 ls_map_zoom(), !cc && ls_map_zoom() > ls_map_source_zoom() ? " MAG" : "", width_m / 1609.344);
    ls_tui_status_set(s_file_error ? s_file_error : s_note, NULL);
    if (!ls_tui_is_wide())
        tui_put_str(sf, area, area.x, area.y + 2, s_file_error ? s_file_error : s_note, LS_ATTR_DIM);


    if (guidance) {
        char strip[96];ls_route_guidance(strip,sizeof(strip));
        tui_put_str(sf,area,area.x,area.y+s_header_h-1,strip,
            TUI_ATTR(ls_route_live()->off_route ? TUI_YELLOW|TUI_BRIGHT : TUI_CYAN|TUI_BRIGHT,TUI_BLACK));
    }
    if (ctl_h) ls_quick_draw_posture(sf, s_quick_rect, ls_tui_is_wide(), quick, nq);
#ifdef LS_MAP_AUDIT
    s_carto_audit_bad=carto_audit_cells(sf); /* before an external modal covers MAP */
#endif
}

/* --------------------------------------------------------------- input -- */

static int pan_step(void)
{
    const int px = (s_map_cells.w * SUB_X) / PAN_FRACTION;
    return px > 0 ? px : 32;
}

static void pan(int dx, int dy)
{
    s_track = false;
    ls_map_pan(dx, dy);
}

/* Step through the aircraft with a position, nearest the centre first, and
   go to each one. */
static void cycle_air(int dir)
{
    int order[ADSB_MAX_TRACKED], m = 0;
    double dist[ADSB_MAX_TRACKED];
    double clat, clon;
    ls_map_get_center(&clat, &clon);
    double rlat = clat, rlon = clon;
    receiver_at(&rlat, &rlon, NULL);
    for (int s = 0; s < ADSB_MAX_TRACKED; s++) {
        if (!s_air[s].on) continue;
        double b, d;
        ls_geo_bearing_range(rlat, rlon, s_air[s].lat, s_air[s].lon, &b, &d);
        int j = m++;
        while (j > 0 && dist[j - 1] > d) { dist[j] = dist[j - 1]; order[j] = order[j - 1]; j--; }
        dist[j] = d; order[j] = s;
    }
    if (!m) { say("no aircraft with a position"); return; }
    int at = -1;
    for (int k = 0; k < m; k++) if (s_sel == SEL_AIR && adsb_state_get(order[k])->icao == s_sel_icao) at = k;
    const int next = at < 0 ? (dir > 0 ? 0 : m - 1) : (at + dir + m) % m;
    const int s = order[next];
    select_air(adsb_state_get(s)->icao);
    jump_to(s_air[s].lat, s_air[s].lon);
}

/* ENTER: go to what is selected. */
static bool go_selected(void)
{
    if (s_sel == SEL_AIR) {
        for (int s = 0; s < ADSB_MAX_TRACKED; s++)
            if (s_air[s].on && adsb_state_get(s)->icao == s_sel_icao) { jump_to(s_air[s].lat, s_air[s].lon); return true; }
    } else if (s_sel == SEL_NODE) {
        for (int i = 0; i < s_npeers; i++)
            if (s_map_peers[i].has_loc && !strcmp(s_map_peers[i].id, s_sel_node)) {
                jump_to(s_map_peers[i].lat_e6 / 1e6, s_map_peers[i].lon_e6 / 1e6);
                return true;
            }
    } else if (s_sel == SEL_MARK) {
        const ls_mark_t *m = ls_marks_at(s_sel_index);
        if (m) { jump_to(m->lat, m->lon); return true; }
    } else if (s_sel == SEL_LINE) {
        const ls_sketch_t *l = ls_sketch_at(s_sel_index);
        if (l) { jump_to(l->lat[0], l->lon[0]); return true; }
    }
    return false;
}

static bool key(ls_tk_t k, char ch)
{
    if(ls_place_search_active())return ls_place_search_key(k,ch);
    if (k == LS_TK_CHAR && (ch == 'x' || ch == 'X')) {
        s_controls_hidden = !s_controls_hidden;
        ls_tui_invalidate();
        return true;
    }
    int nq = 0;
    const ls_quick_t *quick = quick_table(&nq);
    if (k == LS_TK_CHAR && ls_quick_key(ch, quick, nq, ls_quick_grant_builtin(), NULL))
        return true;

    if (ls_sketch_drawing()) {
        ls_val_t out;
        if (k == LS_TK_ENTER) { a_map_draw_add(NULL, &out); return true; }
        if (k == LS_TK_ESC) { a_map_draw_cancel(NULL, &out); return true; }
    }

    if (k == LS_TK_CHAR && (ch == 'n' || ch == 'N')) {
        layers_set(s_layers ^ L_MESH);
        say(layer(L_MESH) ? "mesh nodes shown" : "mesh nodes hidden");
        return true;
    }
    if (k == LS_TK_CHAR && (ch == 't' || ch == 'T')) {
        layers_set(s_layers ^ L_TRAILS);
        say(layer(L_TRAILS) ? "trails shown" : "trails hidden");
        return true;
    }
    if (k == LS_TK_CHAR && (ch == 'r' || ch == 'R')) {
        layers_set(s_layers ^ L_RINGS);
        say(layer(L_RINGS) ? "range rings shown" : "range rings hidden");
        return true;
    }
    if (k == LS_TK_CHAR && (ch == 'i' || ch == 'I')) { s_card_hidden = !s_card_hidden; return true; }
    if (k == LS_TK_CHAR && (ch == 'e' || ch == 'E')) { open_edit(); return true; }
    if (k == LS_TK_CHAR && (ch == 'm' || ch == 'M')) { ls_val_t out; a_map_files(NULL, &out); return true; }
    if (k == LS_TK_CHAR && (ch == 'v' || ch == 'V')) {
        ls_val_t out;
        a_map_view(NULL, &out);
        return true;
    }
    if (k == LS_TK_TAB || (k == LS_TK_CHAR && ch == ']')) { cycle_air(+1); return true; }
    if (k == LS_TK_CHAR && ch == '[') { cycle_air(-1); return true; }
    if (k == LS_TK_ENTER) return go_selected();
    if (k == LS_TK_ESC && s_sel != SEL_NONE) { select_none(); return true; }
    switch (k) {
    case LS_TK_LEFT:  pan(-pan_step(), 0); return true;
    case LS_TK_RIGHT: pan( pan_step(), 0); return true;
    case LS_TK_UP:    pan(0, -pan_step()); return true;
    case LS_TK_DOWN:  pan(0,  pan_step()); return true;
    default: return false;
    }
}

/* The nearest thing drawn within reach of a tap, or -1. */
static int nearest_hit(int col, int row)
{
    int cw = 10, ch = 17;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    if (cw < 1) cw = 10;
    if (ch < 1) ch = 17;
    int best = -1;
    long best_d = 0;
    /* About a fingertip and a half either way: six cells across. A tap
       on a label or a shape is distance zero and always wins. */
    const long reach = (long)(6 * cw) * (long)(6 * cw);
    for (int i = 0; i < s_nhits; i++) {
        const hit_t *h = &s_hits[i];
        const long gx = col < h->x ? h->x - col : col > h->x1 ? col - h->x1 : 0;
        const long gy = row < h->y ? h->y - row : row > h->y1 ? row - h->y1 : 0;
        const long dx = gx * cw, dy = gy * ch;
        const long d = dx * dx + dy * dy;
        if (d > reach) continue;
        if (best < 0 || d < best_d) { best = i; best_d = d; }
    }
    return best;
}

static bool hit_is_selected(const hit_t *h)
{
    switch (h->kind & 0x7F) {
    case HIT_AIR:  return s_sel == SEL_AIR && s_sel_icao == h->id;
    case HIT_NODE: return s_sel == SEL_NODE && (int)h->id < s_npeers && !strcmp(s_map_peers[h->id].id, s_sel_node);
    case HIT_MARK: return s_sel == SEL_MARK && s_sel_index == (int)h->id;
    case HIT_LINE: return s_sel == SEL_LINE && s_sel_index == (int)h->id;
    default: return false;
    }
}

static void select_hit(const hit_t *h)
{
    switch (h->kind & 0x7F) {
    case HIT_AIR:  select_air(h->id); break;
    case HIT_NODE: if ((int)h->id < s_npeers) select_node(s_map_peers[h->id].id); break;
    case HIT_MARK: select_mark(SEL_MARK, (int)h->id); break;
    case HIT_LINE: select_mark(SEL_LINE, (int)h->id); break;
    default: break;
    }
}

/* A tap on something selects it, a second tap goes to it, a tap on an edge
   arrow goes straight there, and a tap on bare map recentres on that spot -
   or, while drawing, adds it to the line. */
static bool touch(int col, int row)
{
    if(ls_place_search_active())return ls_place_search_touch(col,row);
    if (in_rect(s_controls_hit, col, row)) return key(LS_TK_CHAR, 'x');
    if (s_map_cells.h > 0 && row >= s_map_cells.y - s_header_h && row < s_map_cells.y &&
        col >= s_map_cells.x && col < s_map_cells.x + 20) {
        ls_val_t out;
        a_map_style(NULL, &out);
        return true;
    }
    int nq = 0;
    const ls_quick_t *quick = quick_table(&nq);
    if (s_quick_rect.h > 0 && row >= s_quick_rect.y &&
        row < s_quick_rect.y + s_quick_rect.h &&
        ls_quick_touch(col, row, quick, nq, ls_quick_grant_builtin(), NULL))
        return true;

    /* Whole pad box claimed, its blank corners included: a tap a pixel off a
       glyph should not fall through to recentring. */
    if (in_rect(s_pad_rect, col, row)) {
        const int dx = col - (s_pad_rect.x + 1);
        const int dy = row - (s_pad_rect.y + 1);
        if      (dx == 0 && dy == -1) pan(0, -pan_step());
        else if (dx == 0 && dy ==  1) pan(0,  pan_step());
        else if (dx == -1 && dy == 0) pan(-pan_step(), 0);
        else if (dx ==  1 && dy == 0) pan( pan_step(), 0);
        return true;
    }
    /* The card: tapping it edits a marker or line, and goes to anything else. */
    if (in_rect(s_card_rect, col, row)) {
        if (s_sel == SEL_MARK || s_sel == SEL_LINE) open_edit();
        else go_selected();
        return true;
    }

    if (!in_rect(s_map_cells, col, row)) return false;

    const double fx = (col - s_map_cells.x + 0.5) * SUB_X;
    const double fy = (row - s_map_cells.y + 0.5) * sub_y();
    if (ls_sketch_drawing()) {
        double lat, lon;
        frame_ll(fx, fy, s_pw, s_ph, &lat, &lon);
        if (!ls_sketch_add_point(lat, lon)) say("no room for another point");
        return true;
    }

    const int h = nearest_hit(col, row);
    if (h >= 0) {
        const hit_t *hit = &s_hits[h];
        if (hit->kind & HIT_EDGE) {
            select_hit(hit);
            jump_to(hit->lat, hit->lon);
        } else if (hit_is_selected(hit)) {
            jump_to(hit->lat, hit->lon);
        } else {
            select_hit(hit);
        }
        return true;
    }

    const int dx = (col - s_map_cells.x - s_map_cells.w / 2) * SUB_X;
    const int dy = (row - s_map_cells.y - s_map_cells.h / 2) * sub_y();
    pan(dx, dy);
    return true;
}

/* Called by the map.reload action, which is how the console and a card app
   reach the same rescan the button does. */
const char *ls_scr_map_reload(void) { rescan(); return s_file_error; }

const ls_tui_screen_t ls_scr_map = {
    .name = "MAP",
    .hint = "TAP select/centre  TAB next plane  ENTER go  F go to  K mark  D draw  L layers  S style",
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .key = key,
    .touch = touch,
};
