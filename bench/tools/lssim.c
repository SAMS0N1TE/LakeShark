/* The panel, on this machine. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ls_shim_time_live: see below. */
#include "esp_timer.h"

#include "ls_tui.h"
#include "ls_tui_screen.h"
#include "ls_app.h"
#include "ls_notify.h"
#include "ls_icons.h"
#include "ls_theme.h"
#include "ls_value.h"
#include "ls_action.h"
#include "ls_panel.h"
#include "ls_waterfall.h"
#include "ls_wf_source.h"
#include "ls_map.h"
#include "apps/adsb/adsb_state.h"
#include "apps/fm/fm_state.h"

/* ---------------------------------------------------------- the panel -- */

/* The real one. Landscape is the blitter's transpose of this, not a second
   buffer, exactly as on the board. */
#define NATIVE_W 568
#define NATIVE_H 1232

#define CORNER_R 72

static uint16_t g_fb[NATIVE_W * NATIVE_H];

bool ls_panel_fb(ls_panel_fb_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->pixels = g_fb;
    out->width = NATIVE_W;
    out->height = NATIVE_H;
    return true;
}
void ls_panel_fb_present(void) { }

/* ------------------------------------------------------------ writing -- */

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static bool off_glass(int x, int y)
{
    const int r = CORNER_R;
    int cx, cy;
    if (x < r && y < r)                       { cx = r; cy = r; }
    else if (x >= NATIVE_W - r && y < r)      { cx = NATIVE_W - r - 1; cy = r; }
    else if (x < r && y >= NATIVE_H - r)      { cx = r; cy = NATIVE_H - r - 1; }
    else if (x >= NATIVE_W - r && y >= NATIVE_H - r)
                                              { cx = NATIVE_W - r - 1; cy = NATIVE_H - r - 1; }
    else return false;

    const int dx = x - cx, dy = y - cy;
    return (dx * dx + dy * dy) > (r * r);
}

static int write_bmp(const char *path)
{
    const int w = NATIVE_W, h = NATIVE_H;
    const int stride = w * 3 + ((4 - (w * 3 & 3)) & 3);
    const uint32_t data = (uint32_t)stride * (uint32_t)h;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    put_u32(hdr + 2, 54u + data);
    put_u32(hdr + 10, 54);
    put_u32(hdr + 14, 40);
    put_u32(hdr + 18, (uint32_t)w);
    put_u32(hdr + 22, (uint32_t)h);
    put_u16(hdr + 26, 1);
    put_u16(hdr + 28, 24);
    put_u32(hdr + 34, data);
    fwrite(hdr, 1, 54, f);

    uint8_t *row = calloc(1, (size_t)stride);
    if (!row) { fclose(f); return -1; }

    /* BMP rows run bottom to top. */
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint8_t r, g, b;
            if (off_glass(x, y)) {
                /* Bright magenta, which nothing in any theme draws. It was
                   dim at first and that was useless: on an image whose
                   background is black, a dark corner is exactly what an
                   unlit corner looks like, and telling those two apart is
                   the reason the mask is here. */
                r = 200; g = 0; b = 160;
            } else {
                const uint16_t p = g_fb[(size_t)y * w + x];
                r = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
                g = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
                b = (uint8_t)((p & 0x1F) * 255 / 31);
            }
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        fwrite(row, 1, (size_t)stride, f);
    }
    free(row);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------- the grid -- */

static void dump_grid(FILE *f)
{
    const tui_surface *sf = ls_tui_surface();
    if (!sf) return;

    fprintf(f, "    ");
    for (int x = 0; x < sf->w; x++) fprintf(f, "%d", (x / 10) % 10);
    fprintf(f, "\n    ");
    for (int x = 0; x < sf->w; x++) fprintf(f, "%d", x % 10);
    fprintf(f, "\n");

    for (int y = 0; y < sf->h; y++) {
        fprintf(f, "%3d ", y);
        for (int x = 0; x < sf->w; x++) {
            const char c = sf->back[y * sf->w + x].ch;
            /* The shade and block characters are outside ASCII and print as
               mojibake; they are the ones a layout question is usually
               about, so they get names a reader can count. */
            unsigned char u = (unsigned char)c;
            if (u == (unsigned char)LS_TUI_SHADE_25)       fputc('.', f);
            else if (u == (unsigned char)LS_TUI_SHADE_50)  fputc(':', f);
            else if (u == (unsigned char)LS_TUI_SHADE_75)  fputc('*', f);
            else if (u == (unsigned char)LS_TUI_SHADE_FULL) fputc('#', f);
            else if (u < 32 || u > 126)                    fputc('?', f);
            else                                           fputc(c, f);
        }
        fprintf(f, "\n");
    }
}

/* ------------------------------------------------------------ the apps -- */

extern const ls_tui_screen_t ls_scr_home, ls_scr_p25, ls_scr_fm, ls_scr_adsb,
                             ls_scr_rec, ls_scr_diag, ls_scr_settings,
                             ls_scr_gps, ls_scr_map, ls_scr_falls,
                             ls_scr_mesh, ls_scr_radios;

/* The same table compact_ui.cpp registers, minus the ones whose screens pull
   a radio stack this tool has no use for. Kept in the same order so a screen
   index here means what it means on the board. */
static const ls_app_t APPS[] = {
    { "home", "HOME", "directory", LS_ICON_SHARK, TUI_CYAN,
      LS_APP_MAIN, &ls_scr_home, NULL },
    { "p25",  "P25",  "trunking",  LS_ICON_TOWER, TUI_GREEN,
      LS_APP_MAIN, &ls_scr_p25, NULL },
    { "fm",   "FM",   "analogue",  LS_ICON_WAVE,  TUI_YELLOW,
      LS_APP_MAIN, &ls_scr_fm, NULL },
    { "adsb", "ADSB", "aircraft",  LS_ICON_PLANE, TUI_MAGENTA,
      LS_APP_MAIN, &ls_scr_adsb, NULL },
    { "rec",  "REC",  "capture",   LS_ICON_RECORD, TUI_RED,
      LS_APP_EXTRA, &ls_scr_rec, NULL },
    { "diag", "DIAG", "health",    LS_ICON_CHIP,  TUI_WHITE,
      LS_APP_EXTRA, &ls_scr_diag, NULL },
    { "set",  "SET",  "display",   LS_ICON_GEAR,  TUI_BLUE,
      LS_APP_EXTRA, &ls_scr_settings, NULL },
    { "gps",  "GPS",  "position",  LS_ICON_SAT,   TUI_YELLOW,
      LS_APP_EXTRA, &ls_scr_gps, NULL },

    { "map",  "MAP",  "charts",    LS_ICON_MAP,   TUI_GREEN,
      LS_APP_EXTRA, &ls_scr_map, NULL },
    { "falls","FALLS","spectrum",  LS_ICON_FALLS, TUI_CYAN,
      LS_APP_EXTRA, &ls_scr_falls, NULL },
    { "mesh", "MESH", "lora",      LS_ICON_MESH,  TUI_MAGENTA,
      LS_APP_EXTRA, &ls_scr_mesh, NULL },
    /* what is powered, and how to stop it. */
    { "radios", "RADIOS", "power",  LS_ICON_POWER, TUI_RED,
      LS_APP_EXTRA, &ls_scr_radios, NULL },
};
#define N_APPS ((int)(sizeof(APPS) / sizeof(APPS[0])))

/* ------------------------------------------------------------- feeding -- */

/* The two screens nobody has ever seen work. */

static void feed_waterfall(void)
{
    for (int i = 0; i < LS_WF_ROWS_MAX + 8; i++)
        ls_wf_source_pump();
}

/* The archive the host tests use. Franklin, New Hampshire, at z12 - the same
   place lssim_state.c puts the GPS fix, so the two screens agree about where
   this imaginary radio is standing. */
#ifndef LSSIM_PMTILES
#define LSSIM_PMTILES "fixtures/carto/franklin_z12.pmtiles"
#endif

/* The archive is an argument now, so a freshly extracted one can be
   opened with the board's own reader before it is written to a card. That
   reader refuses compressed directories, oversized roots and fat leaves, and
   finding any of that out on the board costs a card swap. */
static const char *s_map_archive = LSSIM_PMTILES;

static void feed_map(void)
{
    if (ls_map_open(s_map_archive))
        ls_map_center(43.4445, -71.6473);
    else
        printf("lssim: no map archive at %s\n", s_map_archive);
}

static void feed_pages(void)
{
    static const struct {
        uint32_t ric; uint8_t func; char type; uint16_t baud; const char *text;
    } SEED[] = {
        { 1234568, 0, 'A', 1200, "From: <clinician> Re: <ward 4> please call switchboard on 2211 when free, no rush" },
        { 1180224, 3, 'A', 1200, "CODE STROKE B HALLWAY 08" },
        {  921604, 1, 'N', 1200, "5551234" },
        { 1234568, 0, '?',  512, "649(UU-*301(6  )4UU3-3)478--)3094U" },
        { 2201100, 2, 'T', 2400, "(tone)" },
        { 1180224, 3, 'A', 1200, "NET CONTROL 1900 LOCAL USUAL CHANNEL BRING A HANDHELD" },
    };
    const int64_t now = esp_timer_get_time();
    const int n = (int)(sizeof(SEED) / sizeof(SEED[0]));
    for (int i = 0; i < n; i++) {
        fm_page_t *pg = &FM.pages[FM.page_head];
        memset(pg, 0, sizeof(*pg));
        pg->ts_us    = now - (int64_t)(n - i) * 30 * 1000000LL;
        pg->address  = SEED[i].ric;
        pg->function = SEED[i].func;
        pg->type     = SEED[i].type;
        pg->baud     = SEED[i].baud;
        pg->protocol = FM_PAGE_PROTOCOL_POCSAG;
        snprintf(pg->text, sizeof(pg->text), "%s", SEED[i].text);
        FM.page_head = (FM.page_head + 1) % FM_PAGE_LOG_MAX;
        if (FM.page_count < FM_PAGE_LOG_MAX) FM.page_count++;
    }
}

static void feed_adsb(void)
{
    static const struct {
        uint32_t icao;
        const char *callsign;
        int alt, vel, hdg, vs;
        float lat, lon;
        bool pos_valid;
        int age_s;
    } SEED[] = {
        { 0xA1B2C3u, "UAL442",  34000, 460,  270,   0,  43.60f, -71.30f, true,   0 },
        { 0xA4C5D6u, "N914QT",   5500, 140,  185, -600,  43.55f, -71.42f, true,  12 },
        { 0xAABBCCu, "DAL118",  28000, 480,   95, 1200,  43.70f, -71.10f, true,  40 },
        { 0xA00777u, "",        1800,  90,   30,    0,   0.0f,    0.0f, false,   3 },
    };
    adsb_state_init();
    const int64_t now = esp_timer_get_time();
    for (unsigned i = 0; i < sizeof(SEED) / sizeof(SEED[0]); i++) {
        adsb_aircraft_t *a = adsb_state_find_or_create(SEED[i].icao);
        if (!a) continue;
        snprintf(a->callsign, sizeof(a->callsign), "%s", SEED[i].callsign);
        a->altitude   = SEED[i].alt;
        a->velocity   = SEED[i].vel;
        a->heading    = SEED[i].hdg;
        a->vert_rate  = SEED[i].vs;
        a->lat        = SEED[i].lat;
        a->lon        = SEED[i].lon;
        a->pos_valid  = SEED[i].pos_valid;
        a->msg_count  = 40 + (int)i * 7;
        a->good_msg_count = a->msg_count - (int)i;
        a->crc_err_count  = (int)i;
        a->last_seen_us   = now - (int64_t)SEED[i].age_s * 1000000LL;
        a->first_seen_us  = a->last_seen_us - 120 * 1000000LL;
        for (int k = 0; k < 32; k++)
            a->alt_history[k] = (int16_t)(SEED[i].alt - 400 + k * 25);
    }
    adsb_select_set_icao(SEED[0].icao);
}

/* -------------------------------------------------------------- driving -- */

/* State advances between frames, not only at seed time.

   Several things on this interface are only meaningful as a rate or a
   delta - the capture charts, the POCSAG tape - and a fixture that sets a
   counter once and leaves it draws them all as "nothing has happened
   since". That is a real layout, but it is the empty one, and -e already
   exists for looking at those. */
void lssim_tick_state(void);

static void frame(int n)
{
    tui_surface *sf = ls_tui_surface();
    for (int i = 0; i < n; i++) {
        lssim_tick_state();
        tui_frame_begin(sf);
        ls_tui_router_draw(sf);
        ls_tui_present();
    }
}

static void time_frames(int n, bool moving)
{
    tui_surface *sf = ls_tui_surface();

    /* One outside the timing: the first draw of a screen fills caches that
       every later frame hits, and averaging it in reports a steady state
       nobody is ever in. */
    lssim_tick_state();
    tui_frame_begin(sf);
    ls_tui_router_draw(sf);
    ls_tui_present();

    /* The whole batch, not each frame. clock() on Windows advances in steps
       of about sixteen milliseconds, so a per-frame reading of anything
       under that is either zero or one tick - which reports a frame as free
       or as forty times its cost, and never as what it is. */
    const clock_t t0 = clock();
    for (int i = 0; i < n; i++) {

        if (moving) ls_tui_router_key((i & 1) ? LS_TK_RIGHT : LS_TK_LEFT, 0);
        lssim_tick_state();
        tui_frame_begin(sf);
        ls_tui_router_draw(sf);
        ls_tui_present();
    }
    const double us = (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC;
    printf("lssim: %d %s frames  %.0f us/frame  (%.1f ms total)\n",
           n, moving ? "moving" : "still", us / (n > 0 ? n : 1), us / 1000.0);

    /* And where the map's share of it went, when there is a map. */
    ls_map_stats_t ms;
    ls_map_stats(&ms);
    if (ms.tiles_wanted > 0)
        printf("lssim: last map render  %d/%d tiles  fetch %lu us  "
               "raster %lu us  total %lu us\n",
               ms.tiles_drawn, ms.tiles_wanted,
               (unsigned long)ms.fetch_us, (unsigned long)ms.raster_us,
               (unsigned long)ms.render_us);
    if (ms.tiles_wanted > 0)
        printf("lssim: geometry scratch peak %d points of 65536\n",
               ms.scratch_peak);
}

static int app_by_id(const char *id)
{
    for (int i = 0; i < N_APPS; i++)
        if (strcmp(APPS[i].id, id) == 0) return i;
    return -1;
}

/* Named keys, because half this interface is not reachable without them. */

static bool named_key(const char *name, ls_tk_t *out)
{
    static const struct { const char *name; ls_tk_t key; } NAMES[] = {
        { "left",  LS_TK_LEFT  }, { "right", LS_TK_RIGHT },
        { "up",    LS_TK_UP    }, { "down",  LS_TK_DOWN  },
        { "enter", LS_TK_ENTER }, { "esc",   LS_TK_ESC   },
        { "tab",   LS_TK_TAB   }, { "mic",   LS_TK_MIC   },
    };
    for (unsigned i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++)
        if (!strcmp(name, NAMES[i].name)) { *out = NAMES[i].key; return true; }
    return false;
}

static void usage(void)
{
    printf("lssim - render the real TUI to an image\n\n");
    printf("  lssim <app> [options]\n\n");
    printf("  apps      ");
    for (int i = 0; i < N_APPS; i++) printf("%s ", APPS[i].id);
    printf("\n");
    printf("  -o FILE   output BMP (default sim.bmp)\n");
    printf("  -l        landscape (default portrait)\n");
    printf("  -t NAME   theme by name\n");
    printf("  -D        Daylight: black on white over the theme\n");
    printf("  -k KEYS   feed characters to the screen, one per frame\n");
    printf("  -K NAMES  feed named keys, comma separated, one per frame:\n");
    printf("            left,right,up,down,enter,esc,tab,mic  (before -k)\n");
    printf("  -x SCRIPT comma separated, in order, after -K and -k:\n");
    printf("            COL:ROW taps a cell, +text types it (_ is space),\n");
    printf("            @name presses a named key\n");
    printf("  -f N      frames to settle (default 3)\n");
    printf("  -d        also print the cell grid as text\n");
    printf("  -e        empty: no radio, no map - the idle branches\n");
    printf("  -n TEXT   post a notification banner, to look at it\n");
    printf("  -F N      font index: 0 is 10x17, 1 is 9x16\n");
    printf("  -m FILE   a .pmtiles archive instead of the fixture\n");
    printf("  -T N      time N frames of this screen and print us/frame\n");
    printf("  -P N      the same, panning between frames\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 1; }

    const char *want = argv[1];
    const char *out = "sim.bmp";
    const char *theme = NULL;
    const char *keys = NULL;
    const char *navkeys = NULL;
    const char *taps = NULL;
    const char *notice = NULL;
    bool landscape = false;
    bool dump = false;
    bool empty = false;
    bool daylight = false;              /* */
    int font = 0;
    const char *maparc = NULL;
    int settle = 3;
    int timed = 0;
    bool moving = false;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) theme = argv[++i];
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) keys = argv[++i];
        else if (!strcmp(argv[i], "-K") && i + 1 < argc) navkeys = argv[++i];
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) taps = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) notice = argv[++i];
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) settle = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-l")) landscape = true;
        else if (!strcmp(argv[i], "-d")) dump = true;
        else if (!strcmp(argv[i], "-e")) empty = true;
        else if (!strcmp(argv[i], "-D")) daylight = true;
        else if (!strcmp(argv[i], "-F") && i + 1 < argc)
            font = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) maparc = argv[++i];
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) timed = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-P") && i + 1 < argc) {
            timed = atoi(argv[++i]);
            moving = true;
        }
        else { usage(); return 1; }
    }

    const int idx = app_by_id(want);
    if (idx < 0) { printf("lssim: no app '%s'\n", want); usage(); return 1; }

    /* Before begin, which is where the cell size becomes the grid.

       Every layout in this program sizes itself from the rect it is handed,
       so a smaller cell should mean more of them and nothing else. "Should"
       is why this option exists: a font change moves every column count in
       the interface at once, and the cheapest place to find out what that
       breaks is here. */

    if (timed > 0) ls_shim_time_live(1);
    else           ls_shim_time_set(1500000);

    ls_tui_set_font_index(font);

    /* The grid the board has, either way round. */
    if (!ls_tui_begin(landscape ? NATIVE_H : NATIVE_W,
                      landscape ? NATIVE_W : NATIVE_H)) {
        printf("lssim: ls_tui_begin failed\n");
        return 1;
    }
    ls_tui_set_rotation_cw(true);

    if (theme) {
        const ls_tui_theme_t *t = ls_tui_theme_by_name(theme);
        if (!t) { printf("lssim: no theme '%s'\n", theme); return 1; }
        ls_tui_set_theme(t);
    }
    /* Daylight laid over whichever theme -t chose, the way the SET
       row lays it over the chosen one on the board. */
    if (daylight) ls_tui_set_daylight(true);

    void lssim_seed_state(void);
    lssim_seed_state();

    ls_value_publish_builtin();
    ls_action_register_builtin();
    for (int i = 0; i < N_APPS; i++) ls_app_register(&APPS[i]);

    /* The same four the firmware puts on the strip, by the same ids.
       A simulator whose navigation differs from the board's is a simulator
       that answers a different question than the one being asked of it. */
    {
        static const char *const TABS[] = { "home", "mesh", "radios", "set" };
        int idx[4], n = 0;
        for (unsigned t = 0; t < sizeof(TABS) / sizeof(TABS[0]); t++) {
            const ls_app_t *a = ls_app_by_id(TABS[t]);
            if (!a || !a->screen) continue;
            const int si = ls_tui_screen_index_of(a->screen);
            if (si >= 0) idx[n++] = si;
        }
        if (n) ls_tui_screen_set_tabs(idx, n);
    }

    {
        extern bool ls_scr_mesh_notice(ls_notice_t *out);
        ls_notify_add_probe(ls_scr_mesh_notice);
    }

    void lssim_set_empty(bool on);
    lssim_set_empty(empty);
    if (maparc) s_map_archive = maparc;
    if (!empty) {
        feed_waterfall();
        feed_map();
        feed_adsb();
        feed_pages();
    }

    ls_tui_screen_show(idx);
    ls_tui_invalidate();
    frame(settle);

    if (timed > 0) time_frames(timed, moving);

    /* Keys after the screen has settled, so a control's before and after are
       both reachable by rendering twice with different -k. */

    if (navkeys) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s", navkeys);
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            ls_tk_t k;
            if (!named_key(tok, &k)) {
                printf("lssim: no key '%s'\n", tok);
                return 1;
            }
            ls_tui_router_key(k, 0);
            frame(1);
        }
        frame(settle);
    }

    if (keys) {
        for (const char *k = keys; *k; k++) {
            ls_tui_router_key(LS_TK_CHAR, *k);
            frame(1);
        }
        frame(settle);
    }

    /* An ordered script, because -K and -k each run as a block and the thing
       worth checking is usually a tap, then some typing, then another tap:
       open the keyboard, write a message, press OK. Two blocks cannot say
       that in either order. A token is a tap, `+text` to type, or `@name`
       for one of the named keys. */
    if (taps) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", taps);
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            if (tok[0] == '+') {
                for (const char *c = tok + 1; *c; c++) {
                    ls_tui_router_key(LS_TK_CHAR, *c == '_' ? ' ' : *c);
                    frame(1);
                }
                continue;
            }
            if (tok[0] == '@') {
                ls_tk_t k;
                if (!named_key(tok + 1, &k)) {
                    printf("lssim: no key '%s'\n", tok + 1);
                    return 1;
                }
                ls_tui_router_key(k, 0);
                frame(1);
                continue;
            }
            int tc = -1, tr = -1;
            if (sscanf(tok, "%d:%d", &tc, &tr) != 2 || tc < 0 || tr < 0) {
                printf("lssim: bad tap '%s', want COL:ROW\n", tok);
                return 1;
            }
            ls_tui_router_touch(tc, tr);
            frame(1);
        }
        frame(settle);
    }

    if (notice) {
        ls_notice_t n;
        memset(&n, 0, sizeof(n));
        snprintf(n.title, sizeof(n.title), "%s", "DIRECT MESSAGE");
        snprintf(n.body, sizeof(n.body), "%s", notice);
        n.hue = TUI_CYAN;
        n.screen = -1;
        ls_notify_post(&n);
        frame(1);
    }

    if (write_bmp(out) != 0) {
        printf("lssim: cannot write %s\n", out);
        return 1;
    }

    if (dump) dump_grid(stdout);

    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    printf("lssim: %s %s %dx%d cells -> %s\n", want,
           landscape ? "landscape" : "portrait", cols, rows, out);
    return 0;
}
