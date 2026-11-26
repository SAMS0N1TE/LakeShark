#include "ls_lap.h"
#include "ls_input_ev.h"
#include "ls_map_share_ui.h"
#include "apps/example_app.h"
#include "ls_sweep_ui.h"
#include "ls_meters.h"
extern const ls_tui_screen_t ls_scr_meters;
#include "ls_sweep_app.h"
#include "ls_rid_ui.h"
#include "ls_rid.h"
#include "ls_splash.h"
#include "ls_rec_replay.h"
#include "ls_calls.h"
#include "call_archive.h"
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#define _mkdir(p) mkdir((p), 0755)
#endif
#include "ls_tui_density.h"
#include "ls_i18n.h"
/* The panel, on this machine. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ls_shim_time_live: see below. */
#include "esp_timer.h"

#include "ls_tui.h"
#include "ls_tui_idle.h"
#include "ls_tui_touch.h"
#include "ls_tui_screen.h"
#include "ls_gauge.h"
#include "ls_gauge_soc.h"
#include "ls_lock.h"
#include "ls_vt.h"
#include "ls_box.h"
#include "desk/ls_desk.h"
#include "ls_app.h"
#include "ls_app_docs.h"
#include "ls_notify.h"
#include "ls_route_live.h"
#include "ls_icons.h"
#include "ls_theme.h"
#include "ls_value.h"
#include "ls_action.h"
#include "ls_panel.h"
#include "ls_waterfall.h"
#include "ls_wf_source.h"
#include "lr_iq_spectrum.h"
#include "ls_map.h"
#include "ls_map_marks.h"
#include "ls_extnodes.h"
#include <math.h>
#include "ls_notes.h"
#include "ls_experiments.h"
#include "apps/adsb/adsb_state.h"
#include "apps/adsb/uat_stream.h"
#include "apps/adsb/uat_weather.h"
#include "apps/fm/fm_state.h"
#include "ls_radio_select.h"
#include "apps/acars/acars_app.h"
#include "apps/fm/same_store.h"
#include "apps/fm/aprs_store.h"
#include "apps/fm/ais_store.h"
#include "dmr_watch.h"
#include "dmr_gen.h"
#include "ls_tmach.h"
#include "ls_decoded.h"

/* ---------------------------------------------------------- the panel -- */

/* The real one. Landscape is the blitter's transpose of this, not a second
   buffer, exactly as on the board. */
/* Read TFT geometry from the firmware variant. The default remains AMOLED. */
#define CONFIG_LS_TDP4_PANEL_HI8561 1
#include "variants/t_display_p4.h"
#undef CONFIG_LS_TDP4_PANEL_HI8561

static int NATIVE_W = 568, NATIVE_H = 1232, CORNER_R = 72;
static bool s_tft;
static uint16_t g_fb[384 * 15 * 112 * 26];     /* the biggest LSSIM_GRID, 384x112; the T-Display is 568 x 1232 */

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
void ls_panel_fb_present_rows(int y0, int y1) { (void)y0; (void)y1; }

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

static void capture_frame(void);
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
            const int dx = x - LS_PANEL_CUTOUT_CX, dy = y - LS_PANEL_CUTOUT_CY;
            const int rr = LS_PANEL_CUTOUT_R * LS_PANEL_CUTOUT_R;
            if (s_tft && dx * dx + dy * dy <= rr) {
                /* Black, with a grey rim so the hole shows on black too. */
                r = g = b = dx * dx + dy * dy > rr - 2 * LS_PANEL_CUTOUT_R ? 110 : 0;
            } else if (off_glass(x, y)) {
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

static FILE *s_i18n_misses;
static void i18n_miss(const char *en)
{
    fputs(en, s_i18n_misses);
    fputc(0, s_i18n_misses);
}

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
            const int16_t cp=sf->back[y*sf->w+x].ch;
            const char c=cp>=0x2800 && cp<=0x28ff ? '#' : cp>=0x2000 && ls_box_is(cp) ? ls_box_ascii(cp) : (char)cp;
            /* The shade and block characters are outside ASCII and print as
               mojibake; they are the ones a layout question is usually
               about, so they get names a reader can count. */
            unsigned char u = (unsigned char)c;
            /* A Cyrillic letter (tui_cyr.h) as its UTF-8, one column. */
            const uint32_t cy = tui_cyr_slot_codepoint(tui_cyr_slot(cp));
            if (cy) { fputc(0xC0 | (int)(cy >> 6), f); fputc(0x80 | (int)(cy & 0x3F), f); }
            else if (u == (unsigned char)LS_TUI_SHADE_25)  fputc('.', f);
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
                             ls_scr_mesh, ls_scr_radios, ls_scr_labs, ls_scr_journal, ls_scr_subghz, ls_scr_mixrf,
                             ls_scr_notes, ls_scr_compass, ls_scr_music, ls_scr_experiments,
                             ls_scr_terminal, ls_scr_update, ls_scr_packs, ls_scr_tiles, ls_scr_rfsurvey, ls_scr_fox,
                             ls_scr_bootlog, ls_scr_store, ls_scr_locker, ls_scr_chat;
extern const ls_app_doc_t ls_doc_tiles;
extern const ls_tui_screen_t ls_scr_digi;
extern const ls_app_doc_t ls_doc_digi;
extern const ls_tui_screen_t ls_scr_radio;
extern const ls_app_doc_t ls_doc_radio;

/* The same table compact_ui.cpp registers, minus the ones whose screens pull
   a radio stack this tool has no use for. Kept in the same order so a screen
   index here means what it means on the board. */
extern const ls_tui_screen_t lssim_p25_preview, lssim_fm_preview, ls_scr_wireless;
extern const ls_tui_screen_t ls_lap_screen;
extern const ls_tui_screen_t ls_scr_tmach;
extern const ls_tui_screen_t ls_scr_freqs;
extern const ls_tui_screen_t ls_scr_wardrive;
bool ls_wardrive_running(void);
extern const ls_tui_screen_t ls_scr_party;
bool ls_party_running(void);
#include "ls_freqs.h"
/* The two design previews exist only in the simulator, so they carry a
   simulator doc rather than one of the real ones. */
static const ls_app_doc_t sim_preview_doc = {
    .purpose = "A layout preview that exists only in the simulator, for "
               "looking at a design without a board.",
    .records = LS_APP_RECORDS_NOTHING,
    .gps     = LS_APP_GPS_UNUSED,
};

static const ls_app_t APPS[] = {
    { "home", "HOME", "directory", LS_ICON_SHARK, TUI_CYAN,
      LS_APP_MAIN, &ls_scr_home, NULL, &ls_doc_home },
    { "p25",  "P25",  "trunking",  LS_ICON_TOWER, TUI_GREEN,
      LS_APP_MAIN, &ls_scr_p25, NULL, &ls_doc_p25 },
    { "fm",   "FM",   "analogue",  LS_ICON_WAVE,  TUI_YELLOW,
      LS_APP_MAIN, &ls_scr_fm, NULL, &ls_doc_fm },
    { "adsb", "ADSB", "aircraft",  LS_ICON_PLANE, TUI_MAGENTA,
      LS_APP_MAIN, &ls_scr_adsb, NULL, &ls_doc_adsb },
    { "freqs", "FREQS", "frequency library", LS_ICON_FREQS, TUI_YELLOW,
      LS_APP_EXTRA, &ls_scr_freqs, NULL, &ls_doc_freqs },
    { "diag", "DIAG", "health",    LS_ICON_CHIP,  TUI_WHITE,
      LS_APP_EXTRA, &ls_scr_diag, NULL, &ls_doc_diag },
    { "terminal", "TERMINAL", "console", LS_ICON_TERMINAL, TUI_GREEN,
      LS_APP_EXTRA, &ls_scr_terminal, NULL, &ls_doc_terminal },
    { "bootlog", "BOOT LOG", "startup log", LS_ICON_BOOTLOG, TUI_YELLOW,
      LS_APP_EXTRA, &ls_scr_bootlog, NULL, &ls_doc_bootlog },
    { "store", "STORE", "apps", LS_ICON_STORE, TUI_CYAN,
      LS_APP_EXTRA, &ls_scr_store, NULL, &ls_doc_store },
    { "locker", "LOCKER", "files", LS_ICON_LOCKER, TUI_CYAN,
      LS_APP_EXTRA, &ls_scr_locker, NULL, &ls_doc_locker },
    { "chat", "CHAT", "talk", LS_ICON_CHAT, TUI_MAGENTA,
      LS_APP_EXTRA, &ls_scr_chat, NULL, &ls_doc_chat },
    { "update", "UPDATE", "firmware", LS_ICON_UPDATE, TUI_CYAN,
      LS_APP_EXTRA, &ls_scr_update, NULL, &ls_doc_update },
    { "packs", "PACKS", "frequency data", LS_ICON_PACKS, TUI_CYAN,
      LS_APP_EXTRA, &ls_scr_packs, NULL, &ls_doc_packs },
    { "set",  "SET",  "display",   LS_ICON_GEAR,  TUI_BLUE,
      LS_APP_EXTRA, &ls_scr_settings, NULL, &ls_doc_settings },
    { "gps",  "GPS",  "position",  LS_ICON_SAT,   TUI_YELLOW,
      LS_APP_EXTRA, &ls_scr_gps, NULL, &ls_doc_gps },

    { "map",  "MAP",  "charts",    LS_ICON_MAP,   TUI_GREEN,
      LS_APP_EXTRA, &ls_scr_map, NULL, &ls_doc_map },
    { "tiles", "TILES", "map regions", LS_ICON_TILES, TUI_CYAN,
      LS_APP_EXTRA, &ls_scr_tiles, NULL, &ls_doc_tiles },
    { "falls","FALLS","spectrum",  LS_ICON_FALLS, TUI_CYAN,
      LS_APP_EXTRA, &ls_scr_falls, NULL, &ls_doc_falls },
    { "mesh", "MESH", "lora",      LS_ICON_MESH,  TUI_MAGENTA,
      LS_APP_EXTRA, &ls_scr_mesh, NULL, &ls_doc_mesh },
    /* what is powered, and how to stop it. */
    { "radios", "RADIOS", "power",  LS_ICON_POWER, TUI_RED,
      LS_APP_EXTRA, &ls_scr_radios, NULL, &ls_doc_radios },
    { "labs", "LORA LABS", "experiments", LS_ICON_LABS, TUI_CYAN, LS_APP_EXTRA, &ls_scr_labs, NULL, &ls_doc_labs },
    { "experiments", "EXPERIMENTS", "LR2021 and friends", LS_ICON_EXPERIMENT, TUI_MAGENTA, LS_APP_EXTRA, &ls_scr_experiments, NULL, &ls_doc_experiments },
    { "journal", "JOURNAL", "field notes", LS_ICON_JOURNAL, TUI_GREEN, LS_APP_EXTRA, &ls_scr_journal, NULL, &ls_doc_journal },
    { "notes", "NOTES", "field notes", LS_ICON_JOURNAL, TUI_GREEN, LS_APP_EXTRA, &ls_scr_notes, NULL, &ls_doc_notes },
    { "compass", "COMPASS", "bearings", LS_ICON_COMPASS, TUI_YELLOW, LS_APP_EXTRA, &ls_scr_compass, NULL, &ls_doc_compass },
    { "rfsurvey", "RF SURVEY", "heat map", LS_ICON_HEAT, TUI_MAGENTA, LS_APP_EXTRA, &ls_scr_rfsurvey, NULL, &ls_doc_rfsurvey },
    { "fox", "FOX HUNT", "find a TX", LS_ICON_FOX, TUI_YELLOW, LS_APP_EXTRA, &ls_scr_fox, NULL, &ls_doc_fox },
    { "subghz", "SUB-GHZ", "rec + replay", LS_ICON_SUBGHZ, TUI_GREEN, LS_APP_EXTRA, &ls_scr_subghz, NULL, &ls_doc_subghz },
    { "music", "MUSIC", "player", LS_ICON_MUSIC, TUI_CYAN, LS_APP_EXTRA, &ls_scr_music, NULL, &ls_doc_music },
    { "mixrf", "MIX-RF", "keyboard radios", LS_ICON_CHIP, TUI_CYAN, LS_APP_EXTRA, &ls_scr_mixrf, NULL, &ls_doc_mixrf },
    { "p25-design", "P25 DESIGN", "preview", LS_ICON_TOWER, TUI_CYAN, LS_APP_EXTRA, &lssim_p25_preview, NULL, &sim_preview_doc },
    { "fm-design", "FM DESIGN", "preview", LS_ICON_WAVE, TUI_CYAN, LS_APP_EXTRA, &lssim_fm_preview, NULL, &sim_preview_doc },
    { "link", "LINK", "wireless", LS_ICON_WIRELESS, TUI_CYAN,
      LS_APP_EXTRA, &ls_scr_wireless, NULL, &ls_doc_link },

    { "drones", "DRONES", "BLE Remote ID", LS_ICON_DRONE, TUI_CYAN,
      LS_APP_EXTRA, &ls_scr_drones, NULL, &ls_doc_drones },
    { "wardrive", "WARDRIVE", "wifi bt cell gps", LS_ICON_WARDRIVE, TUI_YELLOW,
      LS_APP_EXTRA, &ls_scr_wardrive, ls_wardrive_running, &ls_doc_wardrive, NULL, 0, 88, 0 },   /* its toolbar spelled out */
    { "party", "PARTY", "sync finds + map", LS_ICON_PARTY, TUI_MAGENTA,
      LS_APP_EXTRA, &ls_scr_party, ls_party_running, &ls_doc_party },
    { "meters", "METERS", "utility meters", LS_ICON_METER, TUI_YELLOW,
      LS_APP_EXTRA, &ls_scr_meters, NULL, &ls_doc_meters },
    { "sweep", "SWEEP", "passive nearby signals", LS_ICON_SWEEP, TUI_GREEN,
      LS_APP_EXTRA, &ls_scr_sweep, NULL, &ls_doc_sweep },
    { "digi", "DIGI", "FT8 FT4 WSPR", LS_ICON_DIGI, TUI_GREEN,
      LS_APP_EXTRA, &ls_scr_digi, NULL, &ls_doc_digi },
    { "tmach", "TIME MACHINE", "rewind the air", LS_ICON_TMACH, TUI_MAGENTA,
      LS_APP_EXTRA, &ls_scr_tmach, NULL, &ls_doc_tmach, NULL, 0, 84, 0 },   /* its toolbar spelled out */
#if defined(CONFIG_LS_RX_HEAD) && CONFIG_LS_RX_HEAD
    { "radio", "RADIO", "receiver + decoders", LS_ICON_WAVE, TUI_GREEN,
      LS_APP_EXTRA, &ls_scr_radio, NULL, &ls_doc_radio },
#endif
#if defined(CONFIG_LS_EXAMPLE_APP) && CONFIG_LS_EXAMPLE_APP
    LS_EXAMPLE_APP_ENTRY,
#endif
    { "lap", "APP", "native backend", LS_ICON_CHIP, TUI_GREEN,
      LS_APP_USER, &ls_lap_screen, NULL, &sim_preview_doc },
};
#define N_APPS ((int)(sizeof(APPS) / sizeof(APPS[0])))

/* ------------------------------------------------------------- feeding -- */

/* TIME MACHINE: 30 s of a pager channel at FM's 256k CU8 through the real
   taps, stage, ring and tape - a noise floor, three bursts (two decoded,
   one not), a retune - then what the decoders said, and one clip saved
   into TEMP the way WHAT WAS THAT? saves it. LSSIM_TMACH=saving stops the
   clip half written. */
static void seed_tmach(void)
{
    char dir[200];
    snprintf(dir, sizeof(dir), "%s/lssim_tmach", getenv("TEMP") ? getenv("TEMP") : ".");
    _mkdir(dir);
    for (int n = 1; n < 100; n++) {            /* the same clip number every run */
        static const char *const EXT[] = { "cs8", "cs16", "json" };
        for (int e = 0; e < 3; e++) {
            char p[240];
            snprintf(p, sizeof(p), "%s/tm%04d.%s", dir, n, EXT[e]);
            remove(p);
        }
    }
    ls_tm_set_clip_dir(dir);
    ls_tm_init();
    const int64_t now = esp_timer_get_time();
    ls_tm_service(now);
    static uint8_t buf[16384];
    const uint32_t rate = 256000;
    const int pairs = (int)sizeof(buf) / 2;
    const struct { double from, to, amp; } burst[] = { {-25.9, -25.1, 70}, {-18.6, -18.1, 40}, {-7.8, -6.9, 90} };
    uint32_t noise = 12345;
    double phase = 0;
    int64_t t = now - 30000000;
    while (t < now) {
        const double ts = (t - now) / 1e6;
        const ls_tm_src_t src = { .center_hz = ts < -12.0 ? 152600000 : 152840000, .rate_hz = rate,
                                  .gain_tenths = 400, .fmt = LS_TM_FMT_CU8, .radio = LS_TM_RADIO_RTL };
        for (int k = 0; k < pairs; k++) {
            const double tk = ts + (double)k / rate;
            double amp = 2.5;
            for (unsigned b = 0; b < sizeof(burst) / sizeof(burst[0]); b++)
                if (tk >= burst[b].from && tk < burst[b].to) amp = burst[b].amp * (0.8 + 0.2 * sin(tk * 9));
            phase += 6.283185307 * (((k / 213) & 1) ? 4500.0 : -4500.0) / rate;
            noise = noise * 1664525u + 1013904223u;
            const double ni = ((int)(noise >> 24) - 128) / 32.0, nq = ((int)((noise >> 16) & 255) - 128) / 32.0;
            buf[2 * k] = (uint8_t)lround(128 + amp * cos(phase) + ni);
            buf[2 * k + 1] = (uint8_t)lround(128 + amp * sin(phase) + nq);
        }
        t += (int64_t)pairs * 1000000 / rate;
        ls_tm_tap(LS_TM_Q_SDR, &src, buf, sizeof(buf), t);
        ls_tm_service(t);
    }
    struct { uint16_t kind; double ago; uint32_t address, hz; uint8_t radio; const char *from, *text; } ev[] = {
        { LS_DECODED_DETECT, 610, 0, 0, 0, "BLE", "tracker moving with you, 6 min" },
        { LS_DECODED_ADSB, 190, 0xA1B2C3, 1090000000, LS_TM_RADIO_RTL, "UAL442", "" },
        { LS_DECODED_AIS, 125, 366123456, 162025000, LS_TM_RADIO_LR2021, "SEA TEST", "under way, 7.5 kn" },
        { LS_DECODED_REC, 52, 0, 433920000, LS_TM_RADIO_LR2021, "LR2021", "OOK24 A1B2C3, 5 frames" },
        { LS_DECODED_APRS, 41, 0, 144390000, LS_TM_RADIO_LR2021, "N0CALL-9", "wx 12C 4 kt NW" },
        { LS_DECODED_PAGE, 25.4, 1234568, 0, 0, "POCSAG 1200", "UNIT 4 RESPOND STATION 2" },
        { LS_DECODED_P25, 15, 4512, 851012500, LS_TM_RADIO_HACKRF, "TG 4512", "unit 1704" },
        { LS_DECODED_PAGE, 7.3, 1234568, 0, 0, "POCSAG 1200", "CALL 555 0142 RE TEST" },
    };
    for (unsigned i = 0; i < sizeof(ev) / sizeof(ev[0]); i++) {
        ls_decoded_t d;
        memset(&d, 0, sizeof(d));
        d.kind = ev[i].kind; d.address = ev[i].address; d.freq_hz = ev[i].hz; d.radio = ev[i].radio;
        d.at_us = now - (int64_t)(ev[i].ago * 1e6);
        snprintf(d.from, sizeof(d.from), "%s", ev[i].from);
        snprintf(d.text, sizeof(d.text), "%s", ev[i].text);
        ls_decoded_publish(&d);
        if (d.kind == LS_DECODED_ADSB) for (int k = 1; k < 14; k++) { d.at_us += 10000000; ls_decoded_publish(&d); }
    }
    /* LSSIM_TMACH=many: a timeline longer than the page, to scroll. */
    if (!strcmp(getenv("LSSIM_TMACH"), "many"))
        for (int i = 0; i < 30; i++) {
            ls_decoded_t d;
            memset(&d, 0, sizeof(d));
            d.kind = LS_DECODED_APRS; d.freq_hz = 144390000; d.radio = LS_TM_RADIO_LR2021;
            d.at_us = now - (int64_t)(700 + 60 * i) * 1000000;
            snprintf(d.from, sizeof(d.from), "N%dTST-%d", i % 10, i);
            snprintf(d.text, sizeof(d.text), "beacon %d", i);
            ls_decoded_publish(&d);
        }
    ls_tm_service(now);
    /* WHAT WAS THAT?, as the key does it. */
    const bool half = !strcmp(getenv("LSSIM_TMACH"), "saving");
    if (ls_tm_what_was_that())
        for (int k = 0; k < (half ? 2 : 200); k++) ls_tm_service(now);
    ls_tm_status_t st;
    if (ls_tm_status(&st)) printf("lssim: tmach %.1f s held, %lu ticks, %lu rows, %s\n", st.held_s,
                                  (unsigned long)st.tick_count, (unsigned long)st.tl_count, st.note);
}

/* The two screens nobody has ever seen work. */

static void feed_waterfall(void)
{
    if (getenv("LSSIM_LR_IQ")) {
        /* Synthetic IQ passes through the real FFT and source, including a gap. */
        uint8_t raw[1280]; uint64_t pair = 0; uint32_t noise = 1;
        uint32_t center = 433920000;
        if (getenv("LSSIM_FM_LR")) {
            center = 162550000;
            FM.mode = FM_MODE_LISTEN; FM.freq_hz = center;
            FM.lr_iq_active = true; FM.lr_channel_hz = 25000;
            FM.lr_lost_pairs = 2000; FM.lr_gaps = 1; FM.lr_audio_drops = 3;
            FM.squelch_open = true;
            ls_rsel_set(LS_RSEL_FM, LS_RSEL_LORA);
            ls_wf_source_select(LS_WF_SRC_FM);
        }
        lr_spec_enable(true); lr_spec_begin(center);
        /* LSSIM_LR_SPAN: the wide route as well, 576k around an LO 144 kHz
           under the channel: the LO's leak in the middle, the channel's
           carrier wandering at +144k, a steady neighbour at +60k and a
           pager-like burst at -190k every few rows. */
        const bool span = getenv("LSSIM_LR_SPAN") != NULL;
        static int16_t wide[2 * 512];
        uint64_t wide_n = 0;
        if (span) {
            if (!getenv("LSSIM_FM_LR")) {
                ls_rsel_set(LS_RSEL_WATERFALL, LS_RSEL_LORA);
                ls_wf_source_select(LS_WF_SRC_LORA);
            }
            ls_wf_source_lr_span(true); lr_span_enable(true);
        }
        for (int row = 0; row < 100; ++row) {
            if (span) {
                for (int block = 0; block < 45; ++block) {
                    for (int k = 0; k < 512; ++k, ++wide_n) {
                        noise = noise * 1664525u + 1013904223u;
                        const double t = (double)wide_n / 576000;
                        double i = 900 + (int)(noise & 511) - 256, q = -500 + (int)((noise >> 9) & 511) - 256;
                        const double ch = 6.283185307179586 * (144000 + 2500 * sin(6.283185307179586 * 0.7 * row / 10)) * t;
                        i += 6000 * cos(ch); q += 6000 * sin(ch);
                        const double nb = 6.283185307179586 * 60000 * t;
                        i += 1500 * cos(nb); q += 1500 * sin(nb);
                        if (row % 12 < 4) {
                            const double pg = 6.283185307179586 * (-190000 + ((wide_n >> 8) & 1 ? 4500 : -4500)) * t;
                            i += 9000 * cos(pg); q += 9000 * sin(pg);
                        }
                        wide[2 * k] = (int16_t)i; wide[2 * k + 1] = (int16_t)q;
                    }
                    lr_span_feed(wide, 512, center - 144000, center, 576000,
                                 (uint32_t)(esp_timer_get_time() / 1000), true);
                    ls_shim_time_advance(3000);
                }
                ls_wf_source_pump();
                continue;
            }
            if (row == 75) pair += 2000;
            for (int block = 0; block < 45; ++block) {
                for (int k = 0; k < 320; ++k) {
                    noise = noise * 1664525u + 1013904223u;
                    double a = 6.283185307179586 * (40 + row/5) * (double)(pair+k)/512;
                    int16_t i = (int16_t)(12000*cos(a) + (int)(noise & 255)-128);
                    int16_t q = (int16_t)(12000*sin(a) + (int)((noise>>8)&255)-128);
                    raw[4*k]=i; raw[4*k+1]=(uint16_t)i>>8;
                    raw[4*k+2]=q; raw[4*k+3]=(uint16_t)q>>8;
                }
                lr_spec_feed(raw, sizeof(raw), pair, 1,
                             (uint32_t)(esp_timer_get_time()/1000), true);
                pair += 320; ls_shim_time_advance(3000);
            }
            ls_wf_source_pump();
        }
        return;
    }
    for (int i = 0; i < LS_WF_ROWS_MAX + 8; i++)
        ls_wf_source_pump();
}

/* The archive the host tests use. Franklin, New Hampshire, at z12 - the same
   place lssim_state.c puts the GPS fix, so the two screens agree about where
   this imaginary radio is standing. */
#ifndef LSSIM_PMTILES
#define LSSIM_PMTILES "fixtures/carto/franklin_z12.pmtiles"
#endif

/* LSSIM_MAP_STRESS=N: MAP with every store full around the view, then N
   frames of zooming through every level, panning, style and layer changes.
   Run under the sanitizer build; a clean exit prints "map stress: ok". */
static void frame(int n);
static void map_stress(int frames)
{
    double lat0, lon0;
    ls_map_get_center(&lat0, &lon0);
    const int64_t now = esp_timer_get_time();
    unsigned seed = 12345;
#define RND() (seed = seed * 1103515245u + 12345u, (seed >> 16) & 0x7fff)
#define JIT(span) (((double)RND() / 16383.5 - 1.0) * (span))
    for (int i = 0; i < ADSB_MAX_TRACKED + 4; i++) {
        adsb_aircraft_t *a = adsb_state_find_or_create(0xC00000u + (uint32_t)i);
        if (!a) break;
        snprintf(a->callsign, sizeof(a->callsign), "STR%03d", i);
        a->altitude = 1000 + i * 2500; a->velocity = 120 + i * 20; a->heading = (i * 47) % 360;
        a->altitude_valid = a->velocity_valid = a->heading_valid = a->pos_valid = true;
        a->msg_count = 50; a->last_seen_us = now; a->first_seen_us = now - 300 * 1000000LL;
        a->altitude_ts_us = a->velocity_ts_us = a->heading_ts_us = now;
        for (int k = ADSB_TRAIL_N + 8; k >= 0; k--) {
            a->lat = (float)(lat0 + JIT(0.2)); a->lon = (float)(lon0 + JIT(0.2));
            a->pos_ts_us = now - (int64_t)k * 5000000LL;
            adsb_state_push_position(a);
        }
        if (i == 3) { adsb_state_set_squawk(a, 7700); adsb_state_set_squawk(a, 7700); }
    }
    for (int i = 0; i < LS_MARKS_MAX + 4; i++) {
        char name[24];
        snprintf(name, sizeof(name), "STRESS MARK %d", i);
        ls_marks_add(lat0 + JIT(0.15), lon0 + JIT(0.15), i % 6, name);
    }
    for (int s = 0; s < LS_SKETCH_MAX + 2; s++) {
        ls_sketch_begin();
        for (int k = 0; k < LS_SKETCH_PTS + 6; k++) ls_sketch_add_point(lat0 + JIT(0.3), lon0 + JIT(0.3));
        ls_sketch_finish();
    }
    static ais_vessel_t v;
    for (int i = 0; i < AIS_VESSELS + 8; i++) {
        memset(&v, 0, sizeof(v)); v.mmsi = 366000000u + (unsigned)i;
        v.fields = AIS_POSITION | AIS_MOTION | AIS_NAME; v.message = 1; v.position = true;
        v.lat = lat0 + JIT(0.2); v.lon = lon0 + JIT(0.2); v.sog = 50 + i; v.cog = (i * 370) % 3600; v.heading = v.cog / 10;
        snprintf(v.name, sizeof(v.name), "STRESS VESSEL %d", i);
        ais_store_receive(&v, NULL);
    }
    static aprs_packet_t p;
    for (int i = 0; i < APRS_STATIONS + 8; i++) {
        char call[12], pkt[64];
        const double la = lat0 + JIT(0.2), lo = lon0 + JIT(0.2);
        const double ala = fabs(la), alo = fabs(lo);
        snprintf(call, sizeof(call), "S%dTR-%d", i % 10, i % 15);
        snprintf(pkt, sizeof(pkt), "!%02d%05.2f%c/%03d%05.2f%c>stress %d", (int)ala, (ala - (int)ala) * 60.0, la < 0 ? 'S' : 'N',
                 (int)alo, (alo - (int)alo) * 60.0, lo < 0 ? 'W' : 'E', i);
        call[1] = (char)('0' + i % 10); call[0] = (char)('A' + i % 26);
        aprs_parse(call, "APRS", (const uint8_t *)pkt, strlen(pkt), &p);
        aprs_store_receive(&p, NULL);
    }
    for (int i = 0; i < LS_EXTNODES_MAX + 6; i++) {
        ls_extnode_t n; memset(&n, 0, sizeof(n));
        memcpy(n.src, "MT", 3); n.id = 0x1000u + (unsigned)i;
        snprintf(n.name, sizeof(n.name), "Stress node %d", i); snprintf(n.short_name, sizeof(n.short_name), "S%03d", i);
        n.lat = lat0 + JIT(0.2); n.lon = lon0 + JIT(0.2); n.heard_us = now; n.has_pos = true;
        ls_extnode_put(&n);
    }
    frame(3);
    static const char keys[] = "++--+-vtnrix[]jb";
    for (int f = 0; f < frames; f++) {
        const unsigned r = RND();
        if (f % 97 == 0) {                      /* every zoom, both ways, with and without a frame between */
            for (int z = 0; z < 26; z++) { ls_tui_router_key(LS_TK_WHEEL_UP, 0); if (f & 1) frame(1); }
            for (int z = 0; z < 26; z++) { ls_tui_router_key(LS_TK_WHEEL_DOWN, 0); if (f & 1) frame(1); }
            ls_map_zoom_by(12 - ls_map_zoom());
        } else if (f % 211 == 0) {              /* off to an edge of the world and back */
            ls_map_center(r & 1 ? 84.9 : -84.9, r & 2 ? 179.99 : -179.99);
            frame(2);
            ls_map_zoom_by((int)(r % 23) - ls_map_zoom());
            frame(2);
            ls_map_center(lat0, lon0);
        } else if (r % 5 == 0) ls_tui_router_key((ls_tk_t)(LS_TK_UP + r % 4), 0);
        else if (r % 5 == 1) ls_tui_router_key(r & 8 ? LS_TK_WHEEL_UP : LS_TK_WHEEL_DOWN, 0);
        else if (r % 5 == 2) ls_tui_router_key(LS_TK_CHAR, keys[r % (sizeof(keys) - 1)]);
        else if (r % 5 == 3) ls_map_center(lat0 + JIT(0.1), lon0 + JIT(0.1));
        if (f % 40 == 39) ls_tui_router_key(LS_TK_ESC, 0);
        frame(1);
    }
#undef RND
#undef JIT
    printf("map stress: ok, %d frames\n", frames);
}

#ifdef LS_CARTOCORE_HOST
void lssim_cartocore_pump(void);
void lssim_carto_faults(unsigned per_thousand);
unsigned lssim_carto_fault_count(void);
unsigned lssim_carto_picture(unsigned *z, uint32_t *raster_us, uint32_t *paint_us, uint32_t *reads, uint32_t *read_bytes);
unsigned lssim_carto_frame_serial(void);
bool lssim_carto_idle(void);
extern void ls_scr_map_force_view(int);
extern void ls_carto_map_leave(void);
static void carto_look(int look)
{
    extern uint32_t settings_get_map_layers(uint32_t);
    extern void settings_set_map_layers(uint32_t);
    const uint32_t bits = settings_get_map_layers(0x7ff7) & 0x0fffffffU;
    settings_set_map_layers(bits | (1u << 23) | ((uint32_t)look << 28));
}
/* LSSIM_CARTO_STRESS=N: the CartoCore views over real cells (LSSIM_CELLS)
   for N frames: every zoom both ways, pans along and across z8 cell edges
   and a four-cell corner, off the downloaded cells and back, every look,
   the keys strip hidden and shown (a new picture size), MAP left and
   entered again, and for the middle third a card that fails reads and
   opens. Run under the sanitizer build; a clean exit prints
   "carto stress: ok". */
static void carto_stress(int frames)
{
    double lat0, lon0;
    ls_map_get_center(&lat0, &lon0);
    const double pi = 3.14159265358979323846;
    /* the corner of four z8 cells nearest the start, and the middle of an edge */
    const double cx = floor((lon0 + 180.0) / 360.0 * 256.0 + 0.5), my = (1.0 - asinh(tan(lat0 * pi / 180.0)) / pi) / 2.0 * 256.0;
    const double cy = floor(my + 0.5);
    const double corner_lon = cx / 256.0 * 360.0 - 180.0, corner_lat = atan(sinh(pi * (1.0 - 2.0 * cy / 256.0))) * 180.0 / pi;
    unsigned seed = 4321;
#define RND() (seed = seed * 1103515245u + 12345u, (seed >> 16) & 0x7fff)
#define JIT(span) (((double)RND() / 16383.5 - 1.0) * (span))
    static const int looks[] = { 4, 0, 1, 2, 3 };
    static const char keys[] = "+-+-x";
    extern bool lssim_carto_frame_info(tui_rect *, unsigned *, unsigned *, bool *);
    frame(4);
    for (int f = 0; f < frames; f++) {
        const unsigned r = RND();
        lssim_carto_faults(f >= frames / 3 && f < 2 * frames / 3 ? 25 + (unsigned)(f % 4) * 25 : 0);
        if (f % 67 == 0) {                       /* every zoom, both ways */
            for (int z = 0; z < 24; z++) { ls_tui_router_key(LS_TK_WHEEL_UP, 0); if (f & 1) frame(1 + (int)(r % 3)); }
            for (int z = 0; z < 24; z++) { ls_tui_router_key(LS_TK_WHEEL_DOWN, 0); if (f & 1) frame(1 + (int)(r % 3)); }
            ls_map_zoom_by(8 + (int)(r % 9) - ls_map_zoom());
        } else if (f % 83 == 0) {                /* the corner four cells share, then around it */
            ls_map_center(corner_lat, corner_lon);
            for (int k = 0; k < 12; k++) { ls_tui_router_key((ls_tk_t)(LS_TK_UP + (k * 5 + (int)r) % 4), 0); frame(1 + k % 2); }
        } else if (f % 101 == 0) {               /* along an edge between two cells */
            ls_map_center(lat0 + JIT(0.2), corner_lon + JIT(0.0005));
            for (int k = 0; k < 10; k++) { ls_tui_router_key(k & 1 ? LS_TK_UP : LS_TK_DOWN, 0); frame(1); }
        } else if (f % 127 == 0) {               /* off the cells: nothing under the view, then back */
            ls_map_center(lat0 + 6.0 + JIT(2.0), lon0 + 9.0 + JIT(2.0));
            frame(3);
            ls_map_zoom_by((int)(r % 19) - ls_map_zoom());
            frame(3);
            ls_map_center(lat0, lon0);
        } else if (f % 149 == 0) {               /* an edge of the world */
            ls_map_center(r & 1 ? 84.9 : -84.9, r & 2 ? 179.99 : -179.99);
            frame(2);
            ls_map_center(lat0, lon0);
        } else if (f % 43 == 0) carto_look(looks[(f / 43) % 5]);
        else if (f % 181 == 0) { ls_carto_map_leave(); frame(1 + (int)(r % 4)); }
        else if (f % 59 == 0 && !getenv("LSSIM_MAP_LOOK")) ls_scr_map_force_view((f / 59) % 2 ? 3 + (int)(r % 3) : -1);
        else if (r % 6 == 0) ls_tui_router_key((ls_tk_t)(LS_TK_UP + r % 4), 0);
        else if (r % 6 == 1) ls_tui_router_key(r & 8 ? LS_TK_WHEEL_UP : LS_TK_WHEEL_DOWN, 0);
        else if (r % 6 == 2) ls_tui_router_key(LS_TK_CHAR, keys[r % (sizeof(keys) - 1)]);
        else if (r % 6 == 3) ls_map_center(lat0 + JIT(0.6), lon0 + JIT(0.6));
        /* the retry timers (5 s, 60 s) run out now and then */
        ls_shim_time_advance(f % 37 == 0 ? 6000000 : f % 331 == 0 ? 61000000 : 40000);
        frame(1);
    }
    /* The card is good again: within 30 s the map has to be back, fresh, at
       the place and zoom asked for. Anything else is a hang. */
    lssim_carto_faults(0);
    if (!getenv("LSSIM_MAP_LOOK")) ls_scr_map_force_view(-1);
    carto_look(4);
    ls_map_center(lat0, lon0);
    ls_map_zoom_by(13 - ls_map_zoom());
    bool fresh = false;
    for (int i = 0; i < 600 && !fresh; i++) {
        tui_rect area; unsigned z, gen;
        ls_shim_time_advance(50000);
        frame(1);
        fresh = lssim_carto_frame_info(&area, &z, &gen, &fresh) && fresh;
    }
    if (!fresh) { printf("carto stress: STUCK, no fresh map 30 s after the card recovered\n"); fflush(stdout); exit(3); }
#undef RND
#undef JIT
    printf("carto stress: ok, %d frames, %u card faults\n", frames, lssim_carto_fault_count());
}

/* LSSIM_CARTO_PERF=1: what a CartoCore picture costs, per zoom, in host
   microseconds: the worker (decode + raster, then paint + publish), the UI
   frame that takes a new picture and an idle UI frame; cold (a zoom just
   arrived at) and after a one-step pan. */
static int64_t perf_now_us(void);
static int cmp_u32(const void *a, const void *b) { const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return (x > y) - (x < y); }
static uint32_t median(uint32_t *v, int n) { if (n < 1) return 0; qsort(v, (size_t)n, sizeof(*v), cmp_u32); return v[n / 2]; }
static uint32_t s_cp_ui_us;
void lssim_tick_state(void);
static int64_t s_step_us;
static bool carto_perf_wait(unsigned *serial, uint32_t *worker_us)
{
    tui_surface *sf = ls_tui_surface();
    unsigned z; uint32_t a, b, c, d;
    *worker_us = 0;
    for (int i = 0; i < 400; i++) {
        if (s_step_us) ls_shim_time_advance(s_step_us);
        const int64_t t0 = perf_now_us();
        lssim_cartocore_pump();
        const int64_t t1 = perf_now_us();
        lssim_tick_state();
        ls_input_drain();
        const unsigned now = lssim_carto_picture(&z, &a, &b, &c, &d) + lssim_carto_frame_serial();
        const int64_t t2 = perf_now_us();
        tui_frame_begin(sf);
        ls_tui_router_draw(sf);
        ls_tui_present();
        s_cp_ui_us = (uint32_t)(perf_now_us() - t2);
        if (now != *serial) { *serial = now; *worker_us = (uint32_t)(t1 - t0); return true; }
    }
    return false;
}
static void carto_perf(void)
{
    static const int zooms[] = { 6, 8, 10, 11, 12, 13, 14, 15, 16 };
    const bool picture = !getenv("LSSIM_CARTOCORE") || !strcmp(getenv("LSSIM_CARTOCORE"), "picture");
    unsigned serial = 0, z; uint32_t raster, paint, reads, bytes, worker;
    frame(6);
    serial = lssim_carto_picture(&z, &raster, &paint, &reads, &bytes) + lssim_carto_frame_serial();
    printf("carto perf (%s): host us\n zoom | cold worker raster paint reads kB  ui_take | pan worker raster paint reads  ui_take ui_idle\n", picture ? "picture" : "cells");
    for (size_t k = 0; k < sizeof(zooms) / sizeof(zooms[0]); k++) {
        ls_map_zoom_by(zooms[k] - ls_map_zoom());
        if (!carto_perf_wait(&serial, &worker)) { printf(" z%-3d | no picture\n", zooms[k]); continue; }
        lssim_carto_picture(&z, &raster, &paint, &reads, &bytes);
        const uint32_t cold_worker = worker, cold_raster = raster, cold_paint = paint, cold_reads = reads, cold_kb = bytes / 1024u, cold_ui = s_cp_ui_us;
        for (int i = 0; i < 80 && !lssim_carto_idle(); i++) frame(1);  /* let prefetch settle */
        serial = lssim_carto_picture(&z, &raster, &paint, &reads, &bytes) + lssim_carto_frame_serial();
        uint32_t w[16], ra[16], pa[16], ui[16], idle[16], rd = 0; int n = 0;
        for (int i = 0; i < 16; i++) {
            ls_tui_router_key(i & 1 ? LS_TK_DOWN : LS_TK_RIGHT, 0);
            if (!carto_perf_wait(&serial, &worker)) break;
            lssim_carto_picture(&z, &raster, &paint, &reads, &bytes);
            w[n] = worker; ra[n] = raster; pa[n] = paint; ui[n] = s_cp_ui_us; rd += reads;
            uint32_t dummy; unsigned same = serial;
            (void)dummy;
            /* an idle frame: nothing asked, nothing new */
            { tui_surface *sf = ls_tui_surface(); const int64_t t = perf_now_us(); tui_frame_begin(sf); ls_tui_router_draw(sf); ls_tui_present(); idle[n] = (uint32_t)(perf_now_us() - t); }
            (void)same;
            n++;
            for (int j = 0; j < 30 && !lssim_carto_idle(); j++) frame(1);
            serial = lssim_carto_picture(&z, &raster, &paint, &reads, &bytes) + lssim_carto_frame_serial();
        }
        printf(" z%-3d | %6u %6u %6u %4u %5u %6u | %6u %6u %6u %4u %6u %6u  (n=%d)\n", zooms[k], cold_worker, cold_raster, cold_paint,
               cold_reads, cold_kb, cold_ui, median(w, n), median(ra, n), median(pa, n), n ? rd / (unsigned)n : 0, median(ui, n), median(idle, n), n);
    }
}
#endif

/* The archive is an argument now, so a freshly extracted one can be
   opened with the board's own reader before it is written to a card. That
   reader refuses compressed directories, oversized roots and fat leaves, and
   finding any of that out on the board costs a card swap. */
static const char *s_map_archive = LSSIM_PMTILES;

/* Fixed synthetic fixture, scaled only for the requested still-image scene. */
double lssim_map_scene_coord(double v,bool latitude) {
    const char *z=getenv("LSSIM_MAP_ZOOM");
    if(!z || !getenv("LSSIM_MAP_BUSY")) return v;
    double centre=latitude?43.4445:-71.6473;
    double scale=ldexp(1.0,(getenv("LSSIM_CARTOCORE")?11:12)-atoi(z));
    return centre+(v-centre)*scale;
}
static void feed_map(void)
{
    /* LSSIM_MAP_START: leave the map unplaced, as after a boot */
    const bool placed = !getenv("LSSIM_MAP_START");
    if (getenv("LSSIM_CARTOCORE")) { if (placed) ls_map_center(43.4445,-71.6473); }
    else if (ls_map_open(s_map_archive))
        { if (placed) ls_map_center(43.4445, -71.6473); }
    else
        printf("lssim: no map archive at %s\n", s_map_archive);
    /* Markers and a drawn line, in a scratch file so a render never writes
       into the fixtures. */
    char marks[512];
    const char *tmp = getenv("TEMP");
    snprintf(marks, sizeof(marks), "%s/lssim_marks.txt", tmp ? tmp : ".");
    remove(marks);
    ls_marks_use_file(marks);
    ls_marks_add(lssim_map_scene_coord(43.4520,true), lssim_map_scene_coord(-71.6620,false), LS_MARK_CAMP, "BASE CAMP");
    ls_marks_add(lssim_map_scene_coord(43.4380,true), lssim_map_scene_coord(-71.6300,false), LS_MARK_HAZARD, "WASHOUT");
    ls_marks_add(lssim_map_scene_coord(43.4300,true), lssim_map_scene_coord(-71.6750,false), LS_MARK_WATER, NULL);
    ls_sketch_begin();
    ls_sketch_add_point(lssim_map_scene_coord(43.4520,true), lssim_map_scene_coord(-71.6620,false));
    ls_sketch_add_point(lssim_map_scene_coord(43.4470,true), lssim_map_scene_coord(-71.6500,false));
    ls_sketch_add_point(lssim_map_scene_coord(43.4410,true), lssim_map_scene_coord(-71.6420,false));
    ls_sketch_add_point(lssim_map_scene_coord(43.4380,true), lssim_map_scene_coord(-71.6300,false));
    ls_sketch_finish();
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

/* Read the same wire IQ as the standalone replay, so weather screenshots
   exercise CPFSK, sync, RS and APDU parsing before they reach the screen. */
static uat_stream_t s_uat_replay;
static void replay_uat_down(const uat_fix_t *fix, void *user)
{
    (void)user;
    adsb_state_update_uat(fix, esp_timer_get_time());
}
static void replay_uat_up(const uint8_t *p, void *user)
{
    (void)user;
    uat_weather_uplink(p, esp_timer_get_time());
}
static void replay_uat(void)
{
    const char *path = getenv("LSSIM_UAT_IQ");
    if (!path || !path[0]) return;
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); exit(1); }
    uint8_t buffer[8192];
    uat_weather_reset();
    uat_stream_init(&s_uat_replay, replay_uat_down, replay_uat_up, NULL);
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), fp))) uat_stream_feed(&s_uat_replay, buffer, n);
    bool failed = ferror(fp) || s_uat_replay.have_i;
    fclose(fp);
    if (failed) { fprintf(stderr, "UAT IQ replay truncated or unreadable\n"); exit(1); }
    fprintf(stderr, "UAT replay: %lu down %lu up\n",
        (unsigned long)(s_uat_replay.stats.short_frames + s_uat_replay.stats.long_frames),
        (unsigned long)s_uat_replay.stats.uplink_frames);
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
        { 0xA1B2C3u, "UAL442",  34000, 460,  270,   0,  43.49f, -71.67f, true,   0 },
        { 0xA4C5D6u, "N914QT",   5500, 140,  185, -600,  43.41f, -71.69f, true,  12 },
        { 0xAABBCCu, "DAL118",  28000, 480,   95, 1200,  43.40f, -71.60f, true,  40 },
        { 0xA00777u, "",        1800,  90,   30,    0,   0.0f,    0.0f, false,   3 },
        { 0xA7E001u, "LIFE1",   1200, 110,  320,    0,  43.455f, -71.705f, true,  1 },
        { 0xA5FD03u, "FDX903",  21000, 390,  40, 1800,  43.425f, -71.585f, true,  2 },
        { 0xA77000u, "N55EM",   3500, 120,  140, -900,  43.465f, -71.625f, true,  1 },
    };
    /* Emitter categories, for the symbols: rotorcraft, heavy, light. */
    static const struct { uint32_t icao; uint8_t tc, ca; } CAT[] = {
        { 0xA1B2C3u, 4, 3 }, { 0xA7E001u, 4, 7 }, { 0xA5FD03u, 4, 5 }, { 0xA77000u, 4, 1 },
        { 0xA4C5D6u, 4, 1 },
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
        a->lat        = lssim_map_scene_coord(SEED[i].lat,true);
        a->lon        = lssim_map_scene_coord(SEED[i].lon,false);
        a->pos_valid  = SEED[i].pos_valid;
        a->msg_count  = 40 + (int)i * 7;
        a->good_msg_count = a->msg_count - (int)i;
        a->crc_err_count  = (int)i;
        a->last_seen_us   = now - (int64_t)SEED[i].age_s * 1000000LL;
        a->pos_ts_us      = a->pos_valid ? a->last_seen_us : 0;
        a->first_seen_us  = a->last_seen_us - 120 * 1000000LL;
        a->altitude_valid = a->velocity_valid = a->heading_valid = a->vert_rate_valid = true;
        a->heading_is_track = true;
        a->altitude_ts_us = a->velocity_ts_us = a->heading_ts_us =
            a->vert_rate_ts_us = a->last_seen_us;
        for (int k = 0; k < 32; k++)
            a->alt_history[k] = SEED[i].alt - 400 + k * 25;
        for (unsigned c = 0; c < sizeof(CAT) / sizeof(CAT[0]); c++)
            if (CAT[c].icao == a->icao) { a->emitter_tc = CAT[c].tc; a->emitter_ca = CAT[c].ca; }
        /* Where it has been: back along a gently turning track, a report
           every five seconds for the last hundred. */
        if (a->pos_valid) {
            const float lat = a->lat, lon = a->lon;
            const int64_t ts = a->pos_ts_us;
            for (int k = 20; k >= 1; k--) {
                const double hdg = (SEED[i].hdg + k * 2.5) * M_PI / 180.0;
                const double nm = SEED[i].vel * (k * 5.0) / 3600.0;
                a->lat = lat - (float)(nm * cos(hdg) / 60.0);
                a->lon = lon - (float)(nm * sin(hdg) / 60.0 / cos(lat * M_PI / 180.0));
                a->pos_ts_us = ts - (int64_t)k * 5000000LL;
                adsb_state_push_position(a);
            }
            a->lat = lat; a->lon = lon; a->pos_ts_us = ts;
            adsb_state_push_position(a);
        }
        if (a->icao == 0xA77000u) { adsb_state_set_squawk(a, 7700); adsb_state_set_squawk(a, 7700); }
        else if (a->pos_valid) { adsb_state_set_squawk(a, 1200 + (int)i); adsb_state_set_squawk(a, 1200 + (int)i); }
    }
    /* LSSIM_ADSB_FILL: every slot tracked, for a list that has to scroll. */
    if (getenv("LSSIM_ADSB_FILL")) {
        for (int i = 0; i < ADSB_MAX_TRACKED; i++) {
            adsb_aircraft_t *a = adsb_state_find_or_create(0xB00000u + (uint32_t)i);
            if (!a) break;
            if (a->callsign[0]) continue;
            snprintf(a->callsign, sizeof(a->callsign), "FILL%02d", i);
            a->altitude = 2000 + i * 1500; a->altitude_valid = true;
            a->msg_count = 10 + i;
            a->last_seen_us = now - (int64_t)i * 1000000LL;
            a->first_seen_us = a->last_seen_us - 60 * 1000000LL;
            a->altitude_ts_us = a->last_seen_us;
        }
    }
    adsb_select_set_icao(SEED[0].icao);
}

/* Receive metadata fixtures reach the same symbol observer as firmware. */
static void seed_dmr(void)
{
    static const uint8_t sync[6] = {0xDF,0xF5,0x7D,0x75,0xDF,0x5D};
    static const uint8_t cach[2][3] = {{0x80,0x02,0x02},{0x88,0x00,0x20}};
    dmr_watch_reset();
    for (unsigned slot = 0; slot < 2; ++slot) {
        uint8_t frame[36];
        memcpy(frame, cach[slot], 3);
        dmr_lc_t lc = {.source = 1234567 + slot, .destination = slot ? 16777215 : 2051,
                       .service_options = slot ? 0x40 : 0};
        dmr_gen_burst(frame + 3, sync, 7, 1, &lc);
        for (unsigned i = 0; i < 288; i += 2) {
            unsigned hi = (frame[i/8] >> (7-i%8)) & 1;
            unsigned lo = (frame[i/8] >> (6-i%8)) & 1;
            dmr_watch_symbol(hi ? (lo ? -3600 : -1200) : (lo ? 3600 : 1200), 0, 2400, -2400);
        }
    }
}

/* -------------------------------------------------------------- driving -- */

/* State advances between frames, not only at seed time.

   Several things on this interface are only meaningful as a rate or a
   delta - the capture charts, the POCSAG tape - and a fixture that sets a
   counter once and leaves it draws them all as "nothing has happened
   since". That is a real layout, but it is the empty one, and -e already
   exists for looking at those. */
void lssim_tick_state(void);
#ifdef LS_CARTOCORE_HOST
void lssim_cartocore_pump(void);
#else
static void lssim_cartocore_pump(void) {}
#endif

/* How far the frozen clock moves between frames, in microseconds.

   Zero keeps the old behaviour exactly: the clock sits at 120 s and never
   moves, so every screenshot is reproducible.  That is also why anything
   driven by ls_motion_phase(), ls_fresh() or ls_map_motion renders as a
   single static instant here - those read esp_timer_get_time() rather than
   counting frames, so without this they cannot be seen at all except under
   -T, whose live clock is not reproducible.  Frame-counted animation
   (ls_anim, ls_notify, the keyboard cursor) never needed it. */
static int64_t s_step_us;

/* Reproducible 1 Hz moving-label regression, using the production ADS-B
 * store, projection, symbol/trail renderer and both map screens. */
static void label_track_tick(void) {
    if(!getenv("LSSIM_LABEL_TRACK")) return;
    static int64_t last;
    int64_t now=esp_timer_get_time();
    if(last && now-last<1000000) return;
    static unsigned step;
    if(!last) { adsb_state_init();adsb_select_set_icao(0); }
    last=now;
    adsb_aircraft_t *a=adsb_state_find_or_create(0xABC123);
    snprintf(a->callsign,sizeof(a->callsign),"STICKY1");
    a->lat=43.449f;a->lon=-71.638f-step*0.0005f;
    a->heading=260+(step%21);a->velocity=80;a->altitude=3000;
    a->pos_valid=true;a->pos_ts_us=a->last_seen_us=now;
    adsb_state_push_position(a);
    step++;
}

/* NOTES and FREQS work on scratch copies of their fixtures, so whatever a
   run types, imports, saves or trashes never reaches the fixtures themselves. */
static void sim_copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL;
    char buf[4096]; size_t n;
    while (a && b && (n = fread(buf, 1, sizeof(buf), a)) > 0) fwrite(buf, 1, n, b);
    if (a) fclose(a);
    if (b) fclose(b);
}

static const char *sim_copy(const char *from_dir, const char *dir)
{
#ifdef _WIN32
    mkdir("build"); mkdir("build/lssim_sdcard"); mkdir(dir);
#else
    mkdir("build", 0775); mkdir("build/lssim_sdcard", 0775); mkdir(dir, 0775);
#endif
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' && (!e->d_name[1] || e->d_name[1] == '.')) continue;
            char path[512]; snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            remove(path);
        }
        closedir(d);
    }
    d = opendir(from_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' && (!e->d_name[1] || e->d_name[1] == '.')) continue;
            char from[512], to[512];
            snprintf(from, sizeof(from), "%s/%s", from_dir, e->d_name);
            snprintf(to, sizeof(to), "%s/%s", dir, e->d_name);
            sim_copy_file(from, to);
        }
        closedir(d);
    }
    return dir;
}

static bool calls_fixture;
static const char *calls_root="bench/build/calls-fixture";
bool call_archive_card_space(uint64_t *total,uint64_t *free) {
    *total=8000000000ULL;*free=getenv("LSSIM_CALLS_LOW")?100000000ULL:3900000000ULL;return true;
}
static void seed_calls(bool empty) {
    /* Optional fixed epoch for reproducible before/after pixel comparisons. */
    const char *epoch=getenv("LSSIM_CALLS_TIME");
    const time_t fixture_time=epoch?(time_t)strtoll(epoch,NULL,10):time(NULL);
    if(empty)calls_root="bench/build/calls-empty";
    _mkdir(calls_root);char dir[256];snprintf(dir,sizeof(dir),"%s/20261008",calls_root);_mkdir(dir);
    call_archive_init();call_archive_set_option(CALL_OPT_DAYS,0);
    /* A small trunked system's last hour and a half, newest first, with one
       NOAA weather recording from FM among them. */
    static const struct { unsigned tg, unit, ms, ago_s; } CALLS[] = {
        {2104,3301118, 9400,  64}, {1041, 220158,14200, 190}, {1041, 220311, 6100, 251},
        {   0,      0,30000, 540}, {3301,4410027,21500, 731}, {2110,3302240, 4800, 905},
        {1043, 220158,33000,1322}, {2104,3301118, 7700,1861}, {4005,6602001,12300,2655},
        {1041, 220402,18900,3480}, {2110,3302240, 3200,4511}, {5120,9900120,41000,5790},
    };
    for(int i=0;i<(empty?0:12);i++) {
        char path[320];snprintf(path,sizeof(path),"%s/1200%02d_%d.wav",dir,i,i+42);
        FILE *f=fopen(path,"wb");if(!f)continue;
        const bool fm=!CALLS[i].tg;
        uint8_t h[44];unsigned rate=fm?16000:8000;unsigned bytes=rate*2*CALLS[i].ms/1000;call_wav_header(h,rate,bytes);fwrite(h,1,44,f);
        for(unsigned n=0;n<bytes;n++)fputc(0,f);
        fclose(f);
        char meta[328];snprintf(meta,sizeof(meta),"%s.meta",path);f=fopen(meta,"w");
        if(f){fprintf(f,"%lld,%u,%u,%u,%u,%u,0,0,0",(long long)fixture_time-CALLS[i].ago_s,CALLS[i].ms,fm?162550000:851262500,CALLS[i].tg,CALLS[i].unit,rate);fclose(f);}
    }
    for(int i=0;i<3;i++)call_archive_test_pump(calls_root);
    void lssim_seed_p25_calls(void);
    if(!empty)lssim_seed_p25_calls();
    ls_calls_open("P25");calls_fixture=true;
}
/* Capture every presented frame, including worker stalls and failed renders. */
static void capture_frame(void) {
    /* Every presented frame as a picture, for looking at animation. */
    {
        const char *d = getenv("LSSIM_FRAMES");
        static unsigned n;
        char p[1024];
        if (d) { snprintf(p, sizeof(p), "%s/%04u.bmp", d, n++); write_bmp(p); }
    }
    /* Opt-in invariant for every presented frame, including app openings and
       OPTIONS. Spaces are painted cells too: check background, not text. */
    if (getenv("LSSIM_STATUS_AUDIT")) {
        tui_surface *sf = ls_tui_surface();
        const int pad = ls_tui_corner_pad(0);
        for (int x = pad; x < sf->w - pad; x++) {
            if (!sf->front[x].ch || TUI_ATTR_BG(sf->front[x].attr) != TUI_CYAN) {
                fprintf(stderr, "unpainted status cell %d on %s\n", x,
                        ls_tui_screen_name(ls_tui_screen_current()));
                exit(2);
            }
        }
    }
#ifdef LS_CARTOCORE_HOST
    const char *dir=getenv("LSSIM_CAPTURE_FRAMES");if(!dir) return;
    const char *every=getenv("LSSIM_CAPTURE_EVERY");
    static unsigned presented;
    unsigned period=every?(unsigned)atoi(every):1;
    if(period<1) period=1;
    if(++presented%period) return;
    extern bool lssim_carto_frame_info(tui_rect *,unsigned *,unsigned *,bool *);
    static unsigned number;char path[1024];tui_rect area;unsigned z,generation;bool fresh;
    bool ready=lssim_carto_frame_info(&area,&z,&generation,&fresh);
    if(getenv("LSSIM_LABEL_TRACK")) {
        extern bool ls_map_air_label_trace(uint32_t,int [6]);
        int trace[6];
        if(ls_map_air_label_trace(0xABC123,trace)) {
            snprintf(path,sizeof(path),"%s/%04u.track",dir,number);
            FILE *t=fopen(path,"w");assert(t);
            fprintf(t,"%d %d %d %d %d %d\n",trace[0],trace[1],trace[2],trace[3],trace[4],trace[5]);
            fclose(t);
        }
    }
    snprintf(path,sizeof(path),"%s/%04u.bmp",dir,number);assert(!write_bmp(path));
    snprintf(path,sizeof(path),"%s/%04u.cells",dir,number++);
    FILE *f=fopen(path,"wb");assert(f);
    fprintf(f,"%d %d %u %u %d %d %d %d\n",ready,fresh,z,generation,area.x,area.y,area.w,area.h);
    tui_surface *sf=ls_tui_surface();
    if(ready) for(int y=area.y;y<area.y+area.h;y++) for(int x=area.x;x<area.x+area.w;x++) {
        tui_cell c=sf->front[y*sf->w+x];
        fputc(c.ch&255,f);fputc((c.ch>>8)&255,f);fputc(c.attr,f);
    }
    fclose(f);
#endif
}
/* LSSIM_PERF=FILE: every frame's cost and its shown cells, for the desktop
   feed benchmark (what a frame costs to build, how much of it changed, what
   the board's encoder then sends). A '#name' token in -x labels what follows.
   Records: "LSPF", u16 cols, u16 rows, u32 draw us, u32 present us, u8 the
   changed rows are known, 64 bytes of them (ls_tui_front_rows_take), the
   picture's rect (4 x i16) and serial (u32), then the cells; "LSPL" + 32
   bytes of label. LSSIM_ROWS_AUDIT=1 alone only checks, on every frame, that
   no row changed without ls_tui saying so, and exits 2 when one did. */
static FILE *s_perf_file;
static bool s_perf_input;
static bool s_perf_on, s_rows_audit;
static int64_t perf_now_us(void)
{
    struct timespec ts;
#ifdef TIME_UTC
    timespec_get(&ts, TIME_UTC);
#else
    /* MinGW's C library has no C11 timespec_get: without this lssim did not
       build on Windows, and every SDK test that runs an app in it failed. */
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static void perf_label(const char *name)
{
    char label[32] = {0};
    if (!s_perf_file) return;
    snprintf(label, sizeof(label), "%s", name);
    fwrite("LSPL", 1, 4, s_perf_file);
    fwrite(label, 1, sizeof(label), s_perf_file);
}
/* "LSPI", before a frame's LSPF: what the picture layer of the feed reads.
   u32 flags (1 a picture, 2 ring, 4 pan, 8 its pixels follow, 16 the frame
   answers input), i32 w, h, x0,
   y0, u32 serial, ring epoch, columns, rows, pan epoch, i32 pan left, top,
   u32 ground, u32 cell w, h, u32 glass bytes + the bits, u32 n + n x (u16
   glyph, u8 halo, w*h coverage) for glass glyphs not written before, then
   the pixels when they differ from the last ones written. */
static void perf_picture(const tui_surface *sf)
{
    static uint16_t *last;
    static size_t last_n;
    static uint8_t seen[8192];
    tui_rect ir = {0};
    const uint16_t *src = NULL;
    int32_t g[4] = {0};
    uint32_t u[8] = {0}, flags = s_perf_input ? 16 : 0, cell[2];
    int w = 0, h = 0, x0 = 0, y0 = 0, cw = 0, ch = 0;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    cell[0] = (uint32_t)cw; cell[1] = (uint32_t)ch;
    if (ls_tui_image_now(&ir, &src, &w, &h, &u[0]) && src) {
        flags |= 1;
        ls_tui_image_ring_offset(&x0, &y0);
        if (ls_tui_image_ring_state(&u[1], &u[2], &u[3])) flags |= 2;
        uint16_t ground = 0;
        int32_t pl = 0, pt = 0;
        if (ls_tui_image_pan_state(&u[4], &pl, &pt, &ground)) flags |= 4;
        u[5] = (uint32_t)pl; u[6] = (uint32_t)pt; u[7] = ground;
        g[0] = w; g[1] = h; g[2] = x0; g[3] = y0;
        const size_t n = (size_t)w * h;
        if (n != last_n || !last || memcmp(last, src, n * 2)) {
            free(last); last = malloc(n * 2); last_n = n;
            memcpy(last, src, n * 2);
            flags |= 8;
        }
    }
    fwrite("LSPI", 1, 4, s_perf_file);
    fwrite(&flags, 4, 1, s_perf_file);
    fwrite(g, 4, 4, s_perf_file);
    fwrite(u, 4, 8, s_perf_file);
    fwrite(cell, 4, 2, s_perf_file);
    size_t gn = 0;
    const uint8_t *glass = ls_tui_glass_now(&gn);
    if (!glass) gn = 0;
    const uint32_t gn32 = (uint32_t)gn;
    fwrite(&gn32, 4, 1, s_perf_file);
    if (gn) fwrite(glass, 1, gn, s_perf_file);
    uint32_t fresh = 0;
    const long at = ftell(s_perf_file);
    fwrite(&fresh, 4, 1, s_perf_file);
    for (size_t i = 0; gn && i < (size_t)sf->w * sf->h; i++) {
        const uint16_t glyph = (uint16_t)sf->front[i].ch;
        if (!(glass[i >> 3] >> (i & 7) & 1) || (seen[glyph >> 3] >> (glyph & 7) & 1)) continue;
        seen[glyph >> 3] |= (uint8_t)(1 << (glyph & 7));
        uint8_t mask[1024] = {0};
        bool halo = false;
        ls_tui_glass_mask(glyph, mask, &halo);
        fwrite(&glyph, 2, 1, s_perf_file); fputc(halo, s_perf_file);
        fwrite(mask, 1, (size_t)cw * ch, s_perf_file);
        fresh++;
    }
    if (fresh) {
        fseek(s_perf_file, at, SEEK_SET);
        fwrite(&fresh, 4, 1, s_perf_file);
        fseek(s_perf_file, 0, SEEK_END);
    }
    if (flags & 8) fwrite(src, 2, (size_t)w * h, s_perf_file);
}
static void perf_frame(int64_t draw_us, int64_t present_us)
{
    static tui_cell *prev;
    static int pw, ph;
    tui_surface *sf = ls_tui_surface();
    uint8_t rows[64];
    const bool known = ls_tui_front_rows_take(rows, sizeof(rows));
    const size_t n = (size_t)sf->w * sf->h;
    if (known && prev && pw == sf->w && ph == sf->h) {
        for (int r = 0; r < sf->h; r++)
            if (!(rows[r >> 3] >> (r & 7) & 1) &&
                memcmp(prev + (size_t)r * sf->w, sf->front + (size_t)r * sf->w, (size_t)sf->w * sizeof(tui_cell))) {
                fprintf(stderr, "lssim: row %d changed and ls_tui_front_rows_take did not say so\n", r);
                exit(2);
            }
    }
    if (!prev || pw != sf->w || ph != sf->h) {
        free(prev);
        prev = malloc(n * sizeof(tui_cell));
        pw = sf->w; ph = sf->h;
    }
    const bool rows_ok = known && prev != NULL;
    if (prev) memcpy(prev, sf->front, n * sizeof(tui_cell));
    if (!s_perf_file) return;
    tui_rect ir = {0};
    const uint16_t *isrc = NULL;
    int iw = 0, ih = 0;
    uint32_t iser = 0;
    if (!ls_tui_image_now(&ir, &isrc, &iw, &ih, &iser)) { ir = (tui_rect){0}; iser = 0; }
    perf_picture(sf);
    const uint16_t dim[2] = {(uint16_t)sf->w, (uint16_t)sf->h};
    const uint32_t us[2] = {(uint32_t)draw_us, (uint32_t)present_us};
    const uint8_t k = rows_ok;
    const int16_t rect[4] = {(int16_t)ir.x, (int16_t)ir.y, (int16_t)ir.w, (int16_t)ir.h};
    fwrite("LSPF", 1, 4, s_perf_file);
    fwrite(dim, 2, 2, s_perf_file);
    fwrite(us, 4, 2, s_perf_file);
    fwrite(&k, 1, 1, s_perf_file);
    fwrite(rows, 1, sizeof(rows), s_perf_file);
    fwrite(rect, 2, 4, s_perf_file);
    fwrite(&iser, 4, 1, s_perf_file);
    fwrite(sf->front, sizeof(tui_cell), n, s_perf_file);
}
static void frame(int n)
{
    tui_surface *sf = ls_tui_surface();
    static bool perf_asked;
    if (!perf_asked) {
        perf_asked = true;
        s_rows_audit = getenv("LSSIM_ROWS_AUDIT") != NULL;
        if (getenv("LSSIM_PERF")) s_perf_file = fopen(getenv("LSSIM_PERF"), "wb");
        s_perf_on = s_rows_audit || s_perf_file;
    }
    if (s_perf_on) {
        for (int i = 0; i < n; i++) {
            if (s_step_us) ls_shim_time_advance(s_step_us);
            lssim_cartocore_pump();
            lssim_tick_state();
            label_track_tick();
            for (int k = 0; k < 4; k++) ls_notes_io_step();
            ls_exp_service();
            ls_lap_poll();
            ls_input_drain();
            const int64_t t0 = perf_now_us();
            tui_frame_begin(sf);
            if (ls_desk_enabled()) ls_desk_draw(sf);
            else ls_tui_router_draw(sf);
            const int64_t t1 = perf_now_us();
            ls_tui_present();
            const int64_t t2 = perf_now_us();
            capture_frame();
            perf_frame(t1 - t0, t2 - t1);
        }
        return;
    }
    for (int i = 0; i < n; i++) {
        if (s_step_us) ls_shim_time_advance(s_step_us);
        lssim_cartocore_pump();
        lssim_tick_state();
        label_track_tick();
        /* The notes store's worker, which on the board is the field I/O task. */
        for (int k = 0; k < 4; k++) ls_notes_io_step();
        /* The experiments worker, which on the board is its own task. */
        ls_exp_service();
        ls_lap_poll();
        ls_input_drain();
        tui_frame_begin(sf);
        if (ls_desk_enabled()) ls_desk_draw(sf);
        else ls_tui_router_draw(sf);
        ls_tui_present();capture_frame();
    }
}


#ifdef LSSIM_PROF
/* Build with -DLSSIM_PROF -finstrument-functions (bench/build-pg): per
   function self time over the timed frames, by address; resolve with nm.
   LSSIM_PROF=FILE names the dump. */
#define NIF __attribute__((no_instrument_function))
typedef struct { void *fn; double self; double incl; unsigned calls; } prof_ent;
static prof_ent s_pe[8192];
static struct { void *fn, *site; unsigned n; } s_pc[16384];
static struct { void *fn; double t0, child; } s_ps[512];
static int s_pd;
static bool s_prof_on;
int main(int, char **);
static double s_tsc_us;
static double prof_now(void) NIF;
static double prof_now(void) { return (double)__builtin_ia32_rdtsc(); }
void __cyg_profile_func_enter(void *fn, void *site) NIF;
void __cyg_profile_func_enter(void *fn, void *site)
{
    if (s_prof_on) {
        unsigned h = (unsigned)((((size_t)fn >> 2) ^ ((size_t)site * 40503u)) * 2654435761u) & 16383;
        while (s_pc[h].fn && (s_pc[h].fn != fn || s_pc[h].site != site)) h = (h + 1) & 16383;
        s_pc[h].fn = fn; s_pc[h].site = site; s_pc[h].n++;
    }
    if (!s_prof_on || s_pd >= 511) { if (s_prof_on) s_pd++; return; }
    s_ps[s_pd].fn = fn; s_ps[s_pd].child = 0; s_ps[s_pd].t0 = prof_now();
    s_pd++;
}
void __cyg_profile_func_exit(void *fn, void *site) NIF;
void __cyg_profile_func_exit(void *fn, void *site)
{
    (void)site; (void)fn;
    if (!s_prof_on) return;
    if (s_pd > 511) { s_pd--; return; }
    if (s_pd <= 0) return;
    s_pd--;
    const double dt = prof_now() - s_ps[s_pd].t0;
    void *f = s_ps[s_pd].fn;
    unsigned h = (unsigned)(((size_t)f >> 2) * 2654435761u) & 8191;
    while (s_pe[h].fn && s_pe[h].fn != f) h = (h + 1) & 8191;
    s_pe[h].fn = f; s_pe[h].calls++; s_pe[h].incl += dt; s_pe[h].self += dt - s_ps[s_pd].child;
    if (s_pd > 0) s_ps[s_pd - 1].child += dt;
}
static double s_c0, s_u0, s_cal;
static void prof_toggle(bool on) NIF;
static void prof_toggle(bool on)
{
    if (on && !s_c0) { s_c0 = prof_now(); s_u0 = (double)perf_now_us(); }
    s_prof_on = on;
}
static void prof_dump(int frames)
{
    s_cal = (prof_now() - s_c0) / ((double)perf_now_us() - s_u0);   /* ticks per us, wall incl. everything */
    const char *p = getenv("LSSIM_PROF");
    FILE *f = fopen(p ? p : "prof.txt", "w");
    if (!f) return;
    fprintf(f, "main %p frames %d\n", (void *)&main, frames);
    for (int i = 0; i < 8192; i++)
        if (s_pe[i].fn) fprintf(f, "%p %.3f %.3f %u\n", s_pe[i].fn, s_pe[i].self / frames / s_cal, s_pe[i].incl / frames / s_cal, s_pe[i].calls);
    for (int i = 0; i < 16384; i++)
        if (s_pc[i].fn) fprintf(f, "C %p %p %u\n", s_pc[i].fn, s_pc[i].site, s_pc[i].n);
    fclose(f);
}
#define PROF_ON(b) prof_toggle(b)
#else
#define PROF_ON(b) ((void)0)
#define prof_dump(n) ((void)0)
#endif

static void time_frames(int n, bool moving)
{
    tui_surface *sf = ls_tui_surface();

    /* One outside the timing: the first draw of a screen fills caches that
       every later frame hits, and averaging it in reports a steady state
       nobody is ever in. */
    lssim_tick_state();
    ls_input_drain();
    tui_frame_begin(sf);
    ls_tui_router_draw(sf);
    ls_tui_present();capture_frame();

    /* The whole batch, not each frame. clock() on Windows advances in steps
       of about sixteen milliseconds, so a per-frame reading of anything
       under that is either zero or one tick - which reports a frame as free
       or as forty times its cost, and never as what it is. */
    /* LSSIM_COSTROW=name: per-frame timing, one line "COST name ..." with the
       median and p95 of draw and present (us), cells changed per frame, and
       how many frames changed nothing at all (work the screen could skip). */
    const char *costrow = getenv("LSSIM_COSTROW");
    double *cd = costrow ? calloc((size_t)n, sizeof(double)) : NULL;
    double *cp = costrow ? calloc((size_t)n, sizeof(double)) : NULL;
    double *cc = costrow ? calloc((size_t)n, sizeof(double)) : NULL;
    tui_cell *cprev = costrow ? malloc((size_t)sf->w * sf->h * sizeof(tui_cell)) : NULL;
    if (cprev) memcpy(cprev, sf->front, (size_t)sf->w * sf->h * sizeof(tui_cell));
    int c_same = 0;
    double c_same_us = 0, c_all_us = 0;
    const clock_t t0 = clock();
    for (int i = 0; i < n; i++) {

        if (s_step_us) ls_shim_time_advance(s_step_us);
        if (moving) ls_tui_router_key((i & 1) ? LS_TK_RIGHT : LS_TK_LEFT, 0);
        lssim_cartocore_pump();
        lssim_tick_state();
        ls_lap_poll();
        ls_input_drain();
        const int64_t q0 = costrow ? perf_now_us() : 0;
        PROF_ON(true);
        tui_frame_begin(sf);
        ls_tui_router_draw(sf);
        PROF_ON(false);
        const int64_t q1 = costrow ? perf_now_us() : 0;
        ls_tui_present();
        const int64_t q2 = costrow ? perf_now_us() : 0;
        capture_frame();
        if (cprev) {
            const size_t nc = (size_t)sf->w * sf->h;
            int ch = 0;
            for (size_t k = 0; k < nc; k++) if (memcmp(&cprev[k], &sf->front[k], sizeof(tui_cell))) ch++;
            memcpy(cprev, sf->front, nc * sizeof(tui_cell));
            cd[i] = (double)(q1 - q0); cp[i] = (double)(q2 - q1); cc[i] = ch;
            c_all_us += cd[i] + cp[i];
            if (!ch) { c_same++; c_same_us += cd[i] + cp[i]; }
        }
    }
    prof_dump(n);
    /* LSSIM_IDLE=0|1: the UI loop's pacing over 30 simulated seconds, with the
       idle gate (1) or the plain 40 ms period (0). One line "IDLE name ..." with
       loops, presents that changed the glass, identical frames and the host cpu
       time of draw + present per simulated second. */
    if (getenv("LSSIM_IDLE")) {
        const int gate = atoi(getenv("LSSIM_IDLE"));
        ls_idle_t g; memset(&g, 0, sizeof(g));
        int64_t simt = 0, period = 40000;
        double cpu = 0; long loops = 0, changed = 0;
        while (simt < 30000000) {
            ls_shim_time_advance(period);
            simt += period;
            lssim_cartocore_pump(); lssim_tick_state(); ls_lap_poll(); ls_input_drain();
            const int64_t q0 = perf_now_us();
            tui_frame_begin(sf); ls_tui_router_draw(sf); ls_tui_present();
            cpu += (double)(perf_now_us() - q0);
            loops++;
            const bool clean = ls_tui_present_clean();
            if (!clean) changed++;
            period = (gate ? ls_idle_period(&g, clean, false, 40) : 40) * 1000;
        }
        printf("IDLE %s gate %d loops/s %.1f changed/s %.1f identical_pct %.0f cpu_us_per_s %.0f\n",
               getenv("LSSIM_COSTROW") ? getenv("LSSIM_COSTROW") : "?", gate, loops / 30.0,
               changed / 30.0, 100.0 * (loops - changed) / loops, cpu / 30.0);
        if (getenv("LSSIM_WHY")) ls_tui_changes_print();
    }
    if (cprev) {
        double *a[3] = {cd, cp, cc};
        double med[3], p95[3];
        for (int j = 0; j < 3; j++) {
            for (int x = 1; x < n; x++) { double v = a[j][x]; int y = x - 1; while (y >= 0 && a[j][y] > v) { a[j][y + 1] = a[j][y]; y--; } a[j][y + 1] = v; }
            med[j] = a[j][n / 2]; p95[j] = a[j][(int)(n * 0.95)];
        }
        uint32_t h = 2166136261u;
        for (size_t k = 0; k < (size_t)sf->w * sf->h; k++) { h = (h ^ sf->front[k].ch) * 16777619u; h = (h ^ sf->front[k].attr) * 16777619u; }
        printf("COST %s grid %dx%d draw_med %.0f draw_p95 %.0f pres_med %.0f pres_p95 %.0f chg_med %.0f chg_p95 %.0f same_pct %.0f wasted_pct %.0f hash %08x\n",
               costrow, sf->w, sf->h, med[0], p95[0], med[1], p95[1], med[2], p95[2],
               100.0 * c_same / n, c_all_us > 0 ? 100.0 * c_same_us / c_all_us : 0.0, h);
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
        { "backspace", LS_TK_BACKSPACE },
        { "f1", LS_TK_F1 }, { "f2", LS_TK_F2 }, { "f3", LS_TK_F3 }, { "f4", LS_TK_F4 }, { "f5", LS_TK_F5 },
        { "home", LS_TK_HOME }, { "end", LS_TK_END }, { "pgup", LS_TK_PGUP }, { "pgdn", LS_TK_PGDN },
        { "ins", LS_TK_INSERT }, { "del", LS_TK_DELETE },
    };
    for (unsigned i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++)
        if (!strcmp(name, NAMES[i].name)) { *out = NAMES[i].key; return true; }
    return false;
}

/* The desk's input as the board gets it: queued and drained, so a screen can
   ask what is being delivered (ls_input_current). M<n> in a -x script holds
   modifiers down for what follows (1 Shift, 2 Alt, 4 Ctrl). */
static uint8_t s_sim_mods;
static void desk_in(const ls_input_ev_t *ev)
{
    ls_input_ev_t e = *ev;
    e.mods = s_sim_mods;
    ls_input_post(&e);
    ls_input_drain();
}
static void desk_key_in(ls_tk_t k, char ch)
{
    const ls_input_ev_t e = {.kind = LS_IEV_KEY_DOWN, .source = LS_ISRC_LOCAL, .clicks = 1, .key = (uint16_t)k, .ch = ch};
    desk_in(&e);
}

static void usage(void)
{
    printf("lssim - render the real TUI to an image\n\n");
    printf("  lssim <app> [options]\n\n");
    printf("  calls     embedded CALLS with sample recordings (LSSIM_CALLS_LOW=1 for guard)\n");
    printf("  apps      ");
    for (int i = 0; i < N_APPS; i++) printf("%s ", APPS[i].id);
    printf("\n");
    printf("  -o FILE   output BMP (default sim.bmp)\n");
    printf("  -r FILE   load a .sub into RECORD replay without transmitting\n");
    printf("  -l        clockwise landscape (default portrait)\n");
    printf("  --ccw     counter-clockwise landscape\n");
    printf("  --tft     HI8561 panel with camera hole\n");
    printf("  -t NAME   theme by name\n");
    printf("  -D        Daylight: black on white over the theme\n");
    printf("  --ev HEX one term ev batch (24-byte records), delivered before -k\n");
    printf("  -k KEYS   feed characters to the screen, one per frame\n");
    printf("  -K NAMES  feed named keys, comma separated, one per frame:\n");
    printf("            left,right,up,down,enter,esc,tab,mic  (before -k)\n");
    printf("  -x SCRIPT comma separated, in order, after -K and -k:\n");
    printf("            COL:ROW taps a cell, +text types it (_ is space),\n");
    printf("            @name presses a named key, ~N waits N frames\n");
    printf("  -f N      frames to settle (default 3)\n");
    printf("  -d        also print the cell grid as text\n");
    printf("  --term    live in this terminal (VT); --feed: the cell feed on stdout,\n");
    printf("            console words on stdin (LakeShark Studio's run panel)\n");
    printf("  -e        empty: no radio, no map - the idle branches\n");
    printf("  -n TEXT   post a notification banner, to look at it;\n");
    printf("            -f then counts frames into the banner's own life\n");
    printf("  -F N      font index: 0=10x17, 1=9x16, 2=15x26, 3=8x14, 4=7x13\n");
    printf("  -m FILE   a .pmtiles archive instead of the fixture\n");
    printf("  -g FILE   load a GPX route into MAP and COMPASS\n");
    printf("  -T N      time N frames of this screen and print us/frame\n");
    printf("  -P N      the same, panning between frames\n");
    printf("  -A MS     advance the clock MS per frame, so time-based\n");
    printf("            animation moves (default 0: one frozen instant)\n");
    printf("  -a ACTION call one board action as an SD app would, e.g. \"store.open dice\"\n");
    printf("  -C        census: enter every screen, count the value and\n");
    printf("            action tables, exit 1 on a refusal or too little room\n");
    printf("  LSSIM_LANG=uk draws the interface in Ukrainian\n");
    printf("  LSSIM_ACARS=1 seeds FM with decoded ACARS messages\n");
    printf("  LSSIM_SAME=1 seeds FM with a quiet test and a tornado warning\n");
    printf("  LSSIM_DMR=1 seeds clear/encrypted repeater calls (P25 key 6)\n");
    printf("  LSSIM_JOURNAL=N seeds JOURNAL with N entries\n");
    printf("  LSSIM_TONE=1..258 seeds a detected receive tone in FM\n");
    printf("  LSSIM_HF=usb|lsb|am|sam|cw seeds shortwave in FM (AM mode on the HF chain)\n");
    printf("  LSSIM_UAT_IQ=FILE.cu8 decodes recorded/synthetic 978 IQ before rendering\n");
    printf("  LSSIM_RS41=1 seeds EXPERIMENTS and MAP with sonde flight tracks\n");
    printf("  LSSIM_AIS=1 seeds FM and MAP with receive-only vessels\n");
    printf("  LSSIM_APRS=1 seeds FM and MAP with stations, weather and messages\n");
}

/* lssim play REC OUT: a recording from the board ('tui rec', gathered by
   tools/ls_record.py) drawn frame by frame through this same renderer, as
   raw RGB24 at the panel's native size, for ffmpeg. The file is a text
   line "LSREC1 cols rows landscape font theme daylight", then each frame as
   cols*rows pairs of (character, attribute) bytes; LSREC2 (see
   tools/ls_record.py) adds the map pictures and each frame's glass bits.
   Off the glass is black. */
static int play(const char *in, const char *out)
{
    FILE *f = fopen(in, "rb");
    if (!f) { printf("lssim: cannot read %s\n", in); return 1; }
    char head[160];
    if (!fgets(head, sizeof(head), f)) { fclose(f); return 1; }
    int cols, rows, landscape, font, daylight, version = 0, nimages = 0;
    char theme[40];
    if (sscanf(head, "LSREC%d %d %d %d %d %39s %d", &version, &cols, &rows, &landscape, &font, theme,
               &daylight) != 7 || (version != 1 && version != 2)) {
        printf("lssim: %s is not a recording\n", in);
        fclose(f);
        return 1;
    }
    /* LSREC2 adds the pictures behind image cells (the map), then each
       frame's glass bits and which picture it shows. */
    typedef struct { tui_rect r; int w, h; uint16_t *px; } rec_image_t;
    rec_image_t *images = NULL;
    const char *ni = strstr(head, " images ");
    if (version == 2 && ni) nimages = atoi(ni + 8);
    if (nimages > 0) images = calloc((size_t)nimages, sizeof(*images));
    for (int i = 0; i < nimages && images; i++) {
        int16_t d[6];
        if (fread(d, sizeof(d), 1, f) != 1) { nimages = i; break; }
        images[i].r = tui_rect_make(d[0], d[1], d[2], d[3]);
        images[i].w = d[4]; images[i].h = d[5];
        images[i].px = malloc((size_t)d[4] * d[5] * 2);
        if (!images[i].px || fread(images[i].px, 2, (size_t)d[4] * d[5], f) != (size_t)d[4] * d[5]) {
            nimages = i; break;
        }
    }
    ls_shim_time_set(120000000);
    ls_tui_set_font_index(font);
    ls_tui_set_corner_radius(CORNER_R);
    if (!ls_tui_begin(landscape ? NATIVE_H : NATIVE_W, landscape ? NATIVE_W : NATIVE_H)) {
        fclose(f);
        return 1;
    }
    ls_tui_set_rotation_cw(true);
    for (char *c = theme; *c; c++) if (*c == '_') *c = ' ';     /* names with spaces travel with underscores */
    const ls_tui_theme_t *t = ls_tui_theme_by_name(theme);
    if (t) ls_tui_set_theme(t);
    if (daylight) ls_tui_set_daylight(true);
    int gc, gr;
    ls_tui_geometry(&gc, &gr, NULL, NULL);
    if (gc != cols || gr != rows) {
        printf("lssim: the board's grid is %dx%d, this one %dx%d\n", cols, rows, gc, gr);
        fclose(f);
        return 1;
    }
    FILE *o = fopen(out, "wb");
    if (!o) { fclose(f); return 1; }
    const size_t cells = (size_t)cols * rows;
    uint8_t *raw = malloc(cells * 2);
    uint8_t *rgb = malloc((size_t)NATIVE_W * NATIVE_H * 3);
    tui_surface *sf = ls_tui_surface();
    const size_t gbytes = (cells + 7) / 8;
    uint8_t *glass = malloc(gbytes);
    int frames = 0;
    while (raw && rgb && glass && fread(raw, 1, cells * 2, f) == cells * 2) {
        for (size_t i = 0; i < cells; i++) { sf->back[i].ch = (char)raw[2 * i]; sf->back[i].attr = raw[2 * i + 1]; }
        if (version == 2) {
            int32_t pic = -1;
            if (fread(glass, 1, gbytes, f) != gbytes || fread(&pic, sizeof(pic), 1, f) != 1) break;
            if (pic >= 0 && pic < nimages) {
                ls_tui_image(images[pic].r, images[pic].px, images[pic].w, images[pic].h, (uint32_t)pic + 1);
                for (size_t i = 0; i < cells; i++)
                    if (glass[i >> 3] >> (i & 7) & 1) ls_tui_glass((int)(i % (size_t)cols), (int)(i / (size_t)cols));
            } else {
                ls_tui_image(tui_rect_make(0, 0, 0, 0), NULL, 0, 0, 0);
            }
        }
        ls_tui_present();capture_frame();
        for (int y = 0; y < NATIVE_H; y++)
            for (int x = 0; x < NATIVE_W; x++) {
                uint8_t *px = rgb + ((size_t)y * NATIVE_W + x) * 3;
                if (off_glass(x, y)) { px[0] = px[1] = px[2] = 0; continue; }
                const uint16_t p = g_fb[(size_t)y * NATIVE_W + x];
                px[0] = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
                px[1] = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
                px[2] = (uint8_t)((p & 0x1F) * 255 / 31);
            }
        fwrite(rgb, 1, (size_t)NATIVE_W * NATIVE_H * 3, o);
        frames++;
    }
    free(raw); free(rgb); free(glass);
    for (int i = 0; i < nimages && images; i++) free(images[i].px);
    free(images);
    fclose(o); fclose(f);
    printf("lssim: %d frames, %dx%d, %s\n", frames, NATIVE_W, NATIVE_H, landscape ? "landscape" : "portrait");
    return frames ? 0 : 1;
}

/* LSSIM_FEED_OUT=FILE: the screen as one full update of the cell feed, the
   records ls_rterm.c's cells_render sends ('G', 'P', then 'C' and 'X' runs),
   raw and unframed, for driving web/screen.js headless
   (bench/tools/desk_font_shots.py). Cells are keyed as cell_key does. */
typedef struct { uint8_t *b; size_t n, cap; } feed_buf;
static void feed_put(feed_buf *k, int c)
{
    if (k->n == k->cap) {
        const size_t cap = k->cap ? k->cap * 2 : 65536;
        uint8_t *nb = realloc(k->b, cap);
        if (!nb) return;
        k->b = nb;
        k->cap = cap;
    }
    k->b[k->n++] = (uint8_t)c;
}
static void feed_u16(feed_buf *k, unsigned v) { feed_put(k, (int)(v & 0xFF)); feed_put(k, (int)(v >> 8 & 0xFF)); }
static unsigned feed_565(uint32_t c) { return (c >> 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 3 & 0x1F); }
static bool feed_records(feed_buf *k)
{
    tui_surface *sf = ls_tui_surface();
    const int cols = sf->w, n = sf->w * sf->h;
    uint32_t *top = calloc((size_t)n, sizeof(uint32_t)), *bot = calloc((size_t)n, sizeof(uint32_t));
    uint8_t *kind = calloc((size_t)n, 1);
    if (!top || !bot || !kind) { free(top); free(bot); free(kind); return false; }
    size_t gn = 0;
    const uint8_t *glass = ls_tui_glass_now(&gn);
    tui_rect r = {0};
    const uint16_t *src = NULL;
    int w = 0, h = 0, x0 = 0, y0 = 0;
    uint32_t serial = 0;
    const bool img = ls_tui_image_now(&r, &src, &w, &h, &serial) && src && r.x >= 0 && r.y >= 0 &&
                     r.x + r.w <= cols && r.y + r.h <= sf->h;
    if (img) { ls_tui_image_ring_offset(&x0, &y0); ls_vt_sample_image(r, cols, src, w, h, x0, y0, top, bot); }
    for (int i = 0; i < n; i++) {
        const int ch = sf->front[i].ch < 0 ? sf->front[i].ch & 0xFF : sf->front[i].ch;
        if (!img || !tui_rect_contains(r, i % cols, i / cols)) continue;
        if (ch == 0xB0) kind[i] = 1;
        else if (glass && (size_t)(i >> 3) < gn && (glass[i >> 3] >> (i & 7) & 1)) kind[i] = 2;
    }
    const ls_tui_theme_t *th = ls_tui_theme_effective(ls_tui_get_theme(), ls_tui_daylight());
    int fi = 0, cw = 0, chh = 0;
    for (; fi < ls_tui_font_count() - 1; fi++) if (ls_tui_font_at(fi) == ls_tui_font()) break;
    ls_tui_geometry(NULL, NULL, &cw, &chh);
    feed_put(k, 'G'); feed_u16(k, (unsigned)cols); feed_u16(k, (unsigned)sf->h);
    feed_put(k, fi); feed_put(k, cw); feed_put(k, chh);
    feed_put(k, 'P');
    for (int i = 0; i < 16; i++) feed_u16(k, th->palette[i]);
    for (int i = 0; i < n;) {
        const bool special = kind[i] != 0;
        int j = i;
        while (j < n && (kind[j] != 0) == special && j - i < 4096) j++;
        feed_put(k, special ? 'X' : 'C'); feed_u16(k, (unsigned)i); feed_u16(k, (unsigned)(j - i));
        for (int t = i; t < j; t++) {
            const int ch = sf->front[t].ch < 0 ? sf->front[t].ch & 0xFF : sf->front[t].ch;
            feed_u16(k, (unsigned)(uint16_t)ch); feed_put(k, sf->front[t].attr);
            if (!special) continue;
            feed_put(k, kind[t]);
            if (kind[t] == 1) { feed_u16(k, feed_565(top[t])); feed_u16(k, feed_565(bot[t])); }
            else {
                const uint32_t a = top[t], b = bot[t];
                const uint32_t m = ((a >> 16 & 255) + (b >> 16 & 255)) / 2 << 16 |
                                   ((a >> 8 & 255) + (b >> 8 & 255)) / 2 << 8 | ((a & 255) + (b & 255)) / 2;
                feed_u16(k, feed_565(m)); feed_u16(k, 0);
            }
        }
        i = j;
    }
    free(top); free(bot); free(kind);
    return true;
}
static int write_feed(const char *path)
{
    feed_buf k = {0};
    FILE *f = fopen(path, "wb");
    if (!f || !feed_records(&k)) { printf("lssim: cannot write %s\n", path); if (f) fclose(f); free(k.b); return 1; }
    fwrite(k.b, 1, k.n, f);
    fclose(f);
    free(k.b);
    tui_surface *sf = ls_tui_surface();
    int fi = 0;
    for (; fi < ls_tui_font_count() - 1; fi++) if (ls_tui_font_at(fi) == ls_tui_font()) break;
    printf("lssim: feed %dx%d font %d -> %s\n", sf->w, sf->h, fi, path);
    return 0;
}
/* lssim --feed: the same records for every frame, for lssim_feed_run. */
static size_t feed_now(const uint8_t **out)
{
    static feed_buf k;
    k.n = 0;
    if (!feed_records(&k)) return 0;
    *out = k.b;
    return k.n;
}

/* `lssim atlas -o lsfont.bin`: every glyph of every font, drawn by the
   board's own renderer and read back as coverage, for a viewer that draws
   cells itself (the cell feed, web/screen.js). Layout: "LSF2", a font count,
   a u16 glyph count, then per font its cell width and height and the glyphs
   of w*h bytes (0 = background, 255 = ink): codes 0-255, braille
   U+2800-28FF, box lines U+2500-257F, then the shapes (ls_box.h). */
static int font_atlas(const char *out)
{
    FILE *f = fopen(out, "wb");
    if (!f) { printf("lssim: cannot write %s\n", out); return 1; }
    const int nf = ls_tui_font_count();
    const int NG = LS_BOX_ATLAS_GLYPHS;
    fwrite("LSF2", 1, 4, f);
    fputc(nf, f);
    fputc(NG & 0xFF, f);
    fputc(NG >> 8, f);
    for (int fi = 0; fi < nf; fi++) {
        ls_tui_set_font_index(fi);
        NATIVE_W = 2000;           /* 129 columns of the widest font; g_fb holds 2000 x 1020 */
        NATIVE_H = 1000;
        CORNER_R = 0;
        ls_tui_set_corner_radius(0);
        if (!ls_tui_begin(NATIVE_W, NATIVE_H)) { printf("lssim: begin failed\n"); fclose(f); return 1; }
        int cols = 0, rows = 0, cw = 0, ch = 0;
        ls_tui_geometry(&cols, &rows, &cw, &ch);
        const ls_tui_theme_t *th = ls_tui_theme_effective(ls_tui_get_theme(), ls_tui_daylight());
        int ink = 15, ground = 0;                         /* the most and least green: the widest ramp */
        for (int i = 0; i < 16; i++) {
            if (((th->palette[i] >> 5) & 63) > ((th->palette[ink] >> 5) & 63)) ink = i;
            if (((th->palette[i] >> 5) & 63) < ((th->palette[ground] >> 5) & 63)) ground = i;
        }
        const int gi = (th->palette[ink] >> 5) & 63, gb = (th->palette[ground] >> 5) & 63;
        tui_surface *sf = ls_tui_surface();
        for (int i = 0; i < cols * rows; i++) sf->back[i] = (tui_cell){' ', (uint8_t)(ground << 4 | ground)};
        if (cols < 130 || rows < 2 + (NG / 64 + 1) * 2) { printf("lssim: grid %dx%d too small\n", cols, rows); fclose(f); return 1; }
        for (int g = 0; g < NG; g++) {
            const int code = g < 256 ? g : g < 512 ? 0x2800 + g - 256 : g < 640 ? 0x2500 + g - 512
                           : g - 640 < LS_BOX_SHAPE_N ? LS_BOX_SHAPES[g - 640] : ' ';
            sf->back[(2 + (g / 64) * 2) * cols + 1 + (g % 64) * 2] = (tui_cell){(int16_t)code, (uint8_t)(ground << 4 | ink)};
        }
        ls_tui_invalidate();
        ls_tui_present();
        fputc(cw, f);
        fputc(ch, f);
        uint8_t *cov = malloc((size_t)cw * ch);
        for (int g = 0; g < NG; g++) {
            const int col = 1 + (g % 64) * 2, row = 2 + (g / 64) * 2;
            int x0 = -1, y0 = -1, c = 0, r = 0;      /* the cell's first pixel, found the way a tap is */
            for (int y = 0; y < NATIVE_H && y0 < 0; y++)
                if (ls_tui_pixel_to_cell(col * cw + cw / 2, y, &c, &r) && r == row) y0 = y;
            for (int x = 0; x < NATIVE_W && x0 < 0; x++)
                if (ls_tui_pixel_to_cell(x, y0 + 1, &c, &r) && c == col) x0 = x;
            for (int y = 0; y < ch; y++)
                for (int x = 0; x < cw; x++) {
                    const int gp = (g_fb[(size_t)(y0 + y) * NATIVE_W + x0 + x] >> 5) & 63;
                    int a = gi != gb ? (gp - gb) * 255 / (gi - gb) : 0;
                    cov[y * cw + x] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
                }
            fwrite(cov, 1, (size_t)cw * ch, f);
        }
        free(cov);
        printf("lssim: font %d %dx%d, ink %d ground %d\n", fi, cw, ch, ink, ground);
    }
    fclose(f);
    printf("lssim: atlas -> %s\n", out);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 1; }
    if (!strcmp(argv[1], "play")) {
        if (argc < 4) { printf("lssim play REC OUT.rgb\n"); return 1; }
        return play(argv[2], argv[3]);
    }

    const char *want = argv[1];
    const char *out = "sim.bmp";
    const char *theme = NULL;
    const char *keys = NULL;
    const char *events = NULL;
    const char *navkeys = NULL;
    const char *taps = NULL;
    const char *notice = NULL;
    const char *replay_path = NULL;
    bool landscape = false;
    bool clockwise = true;
    bool dump = false;
    bool term = false;
    bool feed = false;
    bool empty = false;
    bool daylight = false;              /* */
    int font = 0;
    const char *maparc = NULL, *route_path = NULL;
    int settle = 3;
    int timed = 0;
    bool moving = false;
    bool census = false;
    const char *act_line = NULL;        /* -a "NAME [ARG]": one board action, as an app calls it */

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) theme = argv[++i];
        else if (!strcmp(argv[i], "--ev") && i + 1 < argc) events = argv[++i];
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) keys = argv[++i];
        else if (!strcmp(argv[i], "-K") && i + 1 < argc) navkeys = argv[++i];
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) taps = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) notice = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) replay_path = argv[++i];
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) settle = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-l")) landscape = true;
        else if (!strcmp(argv[i], "--ccw")) { landscape = true; clockwise = false; }
        else if (!strcmp(argv[i], "--tft")) s_tft = true;
        else if (!strcmp(argv[i], "-d")) dump = true;
        else if (!strcmp(argv[i], "--term")) term = true;
        else if (!strcmp(argv[i], "--feed")) feed = true;
        else if (!strcmp(argv[i], "-e")) empty = true;
        else if (!strcmp(argv[i], "-C")) census = true;
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) act_line = argv[++i];
        else if (!strcmp(argv[i], "-D")) daylight = true;
        else if (!strcmp(argv[i], "-F") && i + 1 < argc)
            font = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) maparc = argv[++i];
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) route_path = argv[++i];
        else if (!strcmp(argv[i], "-A") && i + 1 < argc)
            s_step_us = (int64_t)atoi(argv[++i]) * 1000;
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) timed = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-P") && i + 1 < argc) {
            timed = atoi(argv[++i]);
            moving = true;
        }
        else { usage(); return 1; }
    }

    if (!strcmp(want, "atlas")) return font_atlas(strcmp(out, "sim.bmp") ? out : "lsfont.bin");
    const bool splash = !strcmp(want, "splash");
    /* lssim desk: the desktop shell over every app, as a Nano's 'desk on'. */
    const bool desk = !strcmp(want, "desk");
    const int idx = app_by_id(splash || desk || (!strncmp(want, "lock", 4) && strcmp(want, "locker")) ? "home" : !strcmp(want,"calls")?"p25":want);
    if (idx < 0) { printf("lssim: no app '%s'\n", want); usage(); return 1; }

    /* Before begin, which is where the cell size becomes the grid.

       Every layout in this program sizes itself from the rect it is handed,
       so a smaller cell should mean more of them and nothing else. "Should"
       is why this option exists: a font change moves every column count in
       the interface at once, and the cheapest place to find out what that
       breaks is here. */

    if ((timed > 0 || term || feed) && !s_step_us) ls_shim_time_live(1);
    else           ls_shim_time_set(120000000);

    ls_tui_set_font_index(ls_tui_font_for_view(font, APPS[idx].screen->name, false));

    /* The grid the board has, either way round. */
    if (s_tft) {
        NATIVE_W = LS_BOARD_LCD_H_RES;
        NATIVE_H = LS_BOARD_LCD_V_RES;
        CORNER_R = LS_BOARD_LCD_CORNER_R;
        ls_tui_set_cutout(LS_PANEL_CUTOUT_CX, LS_PANEL_CUTOUT_CY, LS_PANEL_CUTOUT_R);
    }
    /* LSSIM_GRID=160x45: a grid like a Nano's terminal panel ('panel term',
       40-384 x 12-112), for a desktop window. */
    const char *grid = getenv("LSSIM_GRID");
    int gc = 0, gr = 0;
    if (grid && sscanf(grid, "%dx%d", &gc, &gr) == 2 && gc >= 40 && gr >= 12 && gc <= 384 && gr <= 112) {
        NATIVE_W = gc * ls_tui_font_at(font)->cell_w;
        NATIVE_H = gr * ls_tui_font_at(font)->cell_h;
        CORNER_R = 0;
        landscape = false;
    }
    ls_tui_set_rotation_cw(clockwise);
    ls_tui_set_corner_radius(CORNER_R);
    if (!ls_tui_begin(landscape ? NATIVE_H : NATIVE_W,
                      landscape ? NATIVE_W : NATIVE_H)) {
        printf("lssim: ls_tui_begin failed\n");
        return 1;
    }
    {
        tui_rect hole;
        if (ls_tui_cutout(&hole))
            printf("lssim: camera covers cells %d,%d %dx%d\n",
                   hole.x, hole.y, hole.w, hole.h);
    }

    if (theme) {
        char name[48];
        snprintf(name, sizeof(name), "%s", theme);
        for (char *c = name; *c; c++) if (*c == '_') *c = ' ';     /* names with spaces travel with underscores */
        const ls_tui_theme_t *t = ls_tui_theme_by_name(name);
        if (!t) { printf("lssim: no theme '%s'\n", theme); return 1; }
        ls_tui_set_theme(t);
    }
    /* Daylight laid over whichever theme -t chose, the way the SET
       row lays it over the chosen one on the board. */
    if (daylight) ls_tui_set_daylight(true);

    void lssim_seed_state(void);
    lssim_seed_state();
    { void lssim_rfs_seed(void); lssim_rfs_seed(); }
    /* WARDRIVE: five minutes of a drive before the first frame (LSSIM_WD=idle: none). */
    if (!strcmp(want, "wardrive")) { void lssim_wardrive_seed(void); lssim_wardrive_seed(); }
    /* PARTY: six minutes of a party with three others (LSSIM_PARTY=new|lobby: none). */
    if (!strcmp(want, "party")) { void lssim_party_seed(void); lssim_party_seed(); }
    /* LSSIM_LANG=uk draws the interface in Ukrainian, as Settings > Device
       > Language does on the board. */
    if (getenv("LSSIM_LANG") && !strcmp(getenv("LSSIM_LANG"), "uk")) ls_i18n_load(LS_LANG_UK);
    /* LSSIM_I18N_MISSES=FILE lists every string drawn with no translation. */
    if (getenv("LSSIM_I18N_MISSES")) {
        s_i18n_misses = fopen(getenv("LSSIM_I18N_MISSES"), "ab");
        if (s_i18n_misses) ls_i18n_set_miss_hook(i18n_miss);
    }
    if (getenv("LSSIM_DMR")) seed_dmr();
    if (getenv("LSSIM_HF")) {
        /* A band with a voice in the passband, in the demod named. */
        void lssim_seed_hf(const char *demod);
        lssim_seed_hf(getenv("LSSIM_HF"));
    }
    if (getenv("LSSIM_TMACH")) seed_tmach();
    if (getenv("LSSIM_TONE")) {
        unsigned tone = (unsigned)atoi(getenv("LSSIM_TONE"));
        FM.mode = FM_MODE_LISTEN;
        FM.tone_show = true;
        FM.tone_required = tone < FM_TONE_CHOICES ? tone : 0;
        FM.tone_detected = (fm_tone_result_t){FM.tone_required, 0.95f};
    }
    if (getenv("LSSIM_AIS")) {
        ais_store_clear(); FM.mode = FM_MODE_AIS; FM.freq_hz = AIS_CENTER_HZ;
        static ais_vessel_t v;
        for (int i = 0; i < 4; ++i) {
            memset(&v, 0, sizeof(v)); v.mmsi = 366123450+i;
            v.fields = AIS_POSITION | AIS_MOTION | AIS_NAME | AIS_STATIC | AIS_VOYAGE;
            v.message = i&1 ? 18 : 1; v.channel = i&1; v.position = true;
            v.lat = 43.448+0.002*i; v.lon = -71.656+0.004*i;
            v.sog = 75+30*i; v.cog = 450+700*i; v.heading = v.cog/10;
            v.ship_type = i&1 ? 37 : 70; v.nav_status = i ? 0 : 5;
            snprintf(v.name, sizeof(v.name), "%s", i&1 ? "SEA TEST" : "LAKESHARK");
            snprintf(v.callsign, sizeof(v.callsign), "WDC123%d", i);
            snprintf(v.destination, sizeof(v.destination), "BOSTON");
            ais_store_receive(&v, NULL);
        }
    }
    /* POCSAG with pages in the ring, and HEAR CHANNEL on: the RX HEAD's
       pager + voice preset. */
    if (getenv("LSSIM_PAGER")) {
        static const struct { uint32_t ric; char type; uint16_t baud; const char *text; } P[] = {
            { 1234560, 'A', 1200, "ENGINE 7 RESPOND 114 MAPLE ST SMOKE SHOWING" },
            { 1847203, 'N', 1200, "555-0142" },
            { 1234567, 'A', 512,  "MEDIC 3 TRANSPORT COMPLETE, BACK IN SERVICE" },
            { 2044161, 'T', 2400, "(tone)" },
            { 1500002, 'A', 1200, "DPW CREW 2 WATER MAIN BREAK RT 3 AND ELM" },
        };
        FM.mode = FM_MODE_POCSAG;
        FM.freq_hz = FM_FREQ_POCSAG;
        FM.pocsag_auto = true;
        FM.pager_voice = true;
        FM.pocsag_frames = 420; FM.pocsag_addr = 38; FM.pocsag_msg = 121;
        FM.pocsag_cw_errs = 7;
        FM.audio_level = 0.38f; FM.noise = 0.31f;
        for (int i = 0; i < (int)(sizeof(P) / sizeof(P[0])); i++) {
            fm_page_t *pg = &FM.pages[FM.page_head];
            memset(pg, 0, sizeof(*pg));
            pg->ts_us = 1000000LL * (i + 1); pg->address = P[i].ric; pg->type = P[i].type;
            pg->baud = P[i].baud; pg->function = P[i].type == 'N' ? 0 : 3;
            snprintf(pg->text, sizeof(pg->text), "%s", P[i].text);
            FM.page_head = (FM.page_head + 1) % FM_PAGE_LOG_MAX;
            if (FM.page_count < FM_PAGE_LOG_MAX) FM.page_count++;
        }
        FM.pocsag_pages = (uint32_t)FM.page_count;
    }
    if (getenv("LSSIM_ACARS")) {
        FM.mode = FM_MODE_ACARS;
        FM.freq_hz = FM_FREQ_ACARS;
        acars_app_clear();
        acars_app_inject("N123ABC", "H1", "FIRST message from the ACARS decoder log");
        acars_app_inject("N456DEF", "Q0", "Flight AB1234 reports position and weather. This retained message exercises the full detail text beyond the pager's eighty character preview. The final words remain readable at both orientations: END OF MESSAGE.");
        acars_state_t *state = acars_app_state_mut();
        state->n_synced = 5;
        state->n_bad_crc = 3;
    }
    if (getenv("LSSIM_APRS")) {
        static aprs_packet_t p;
        static const char *const packets[] = {
            "!4326.80N/07139.50W>Mobile / receive only",
            "!4327.10N/07139.80W_180/010g020t072h50b10132",
            ";TRAIL    *071234z4327.30N/07139.10W>Trailhead",
            ":ANYONE   :Meet at the trailhead{12",
            ">Listening on 144.390 MHz"
        };
        static const char *const calls[] = { "N0CALL-7", "WX1BOX", "N1OBJ", "N2MSG", "N3STAT" };
        aprs_store_clear();
        FM.mode = FM_MODE_APRS; FM.freq_hz = 144390000;
        for (int i = 0; i < 5; ++i) {
            aprs_parse(calls[i], "APRS", (const uint8_t *)packets[i], strlen(packets[i]), &p);
            aprs_store_receive(&p, NULL);
        }
    }
    /* Exercise the production alert view, including quiet tests and EOM. */
    if (getenv("LSSIM_SAME")) {
        static same_alert_t a;
        FM.mode = FM_MODE_SAME;
        same_options_t o = {.include_tests = true};
        same_options_set(&o);
        same_parse("ZCZC-WXR-RWT-025017+0015-2801400-KBOX/NWS-", &a);
        same_store_receive(&a, NULL);
        a.ended = true; same_store_receive(&a, NULL);
        same_parse("ZCZC-WXR-TOR-025017-025021+0030-2801430-KBOX/NWS-", &a);
        same_store_receive(&a, NULL);
        /* Fixtures start with the list visible, without a banner obscuring it. */
        same_store_notice(&a);
    }

    /* LSSIM_WFM, LSSIM_AIR, LSSIM_SSTV, LSSIM_WX: the broadcast extras. */
    extern void lssim_bcast_seed(void);
    lssim_bcast_seed();

    ls_value_publish_builtin();
    ls_action_register_builtin();
    ls_freqs_register();
    void lssim_ota_publish(void);
    if (getenv("LSSIM_RID")) {
        for(int i=0;i<5;i++) {
            uint8_t mac[6]={1,2,3,4,5,(uint8_t)i};
            uint8_t ad[31]={30,0x16,0xfa,0xff,0x0d,0,0x02,0x12};
            snprintf((char *)ad+8,20,"LS-DEMO-%03d",i+1);
            ls_rid_advert(mac,0,-42-i*9,ad,sizeof(ad),esp_timer_get_time());
            memset(ad+6,0,25); ad[6]=0x12; ad[7]=0x20; ad[8]=40+i*20; ad[9]=20+i*10;
            put_u32(ad+11,(uint32_t)(432005000+i*9000));
            put_u32(ad+15,(uint32_t)(-716499000+i*13000));
            put_u16(ad+19,2200+i*30); put_u16(ad+21,2220+i*30); put_u16(ad+23,2100+i*30);
            ls_rid_advert(mac,0,-42-i*9,ad,sizeof(ad),esp_timer_get_time()-(getenv("LSSIM_RID_STALE")?61000000:0));
            memset(ad+6,0,25); ad[6]=getenv("LSSIM_RID_V0")?0x40:0x42; ad[7]=5;
            put_u32(ad+8,432000000); put_u32(ad+12,(uint32_t)-716500000);
            put_u16(ad+16,1); ad[18]=10; put_u16(ad+19,2400); put_u16(ad+21,2000); ad[23]=0x12;
            put_u16(ad+24,2050);
            ls_rid_advert(mac,0,-42-i*9,ad,sizeof(ad),esp_timer_get_time()-(getenv("LSSIM_RID_STALE")?61000000:0));
            if(getenv("LSSIM_RID_STALE")) {
                memset(ad+6,0,25);ad[6]=2;ad[7]=0x12;
                snprintf((char *)ad+8,20,"LS-DEMO-%03d",i+1);
                ls_rid_advert(mac,0,-42-i*9,ad,sizeof(ad),esp_timer_get_time()-11000000);
                ls_rid_advert(mac,0,-42-i*9,ad,sizeof(ad),esp_timer_get_time());
            }
        }
    }
    lssim_ota_publish();          /* what main/ls_ota.c publishes on the board */
    _Static_assert(N_APPS <= LS_TUI_MAX_SCREENS, "Simulator apps must fit the registry");
    void ls_lap_sim_tile(ls_app_t *tile);
    for (int i = 0; i < N_APPS; i++) {
        ls_app_t app = APPS[i];
        if (!strcmp(app.id, "lap")) ls_lap_sim_tile(&app);
        /* LSSIM_SD_GROUP=1..4: the card's app on that HOME tab (LS_APP_GROUP_*). */
        if (app.cat == LS_APP_USER && getenv("LSSIM_SD_GROUP")) app.group = (uint8_t)atoi(getenv("LSSIM_SD_GROUP"));
        int rc = ls_app_register(&app);
        if (rc < 0) { fprintf(stderr,"lssim: app %s refused: %s\n",APPS[i].id,ls_app_register_why(rc)); return 1; }
    }
    ls_notify_add_probe(ls_rid_notice);
    ls_sweep_init();
    void lssim_sweep_seed(void);
    lssim_sweep_seed();
    ls_meters_begin();
    void lssim_meters_seed(void);
    lssim_meters_seed();
    void lssim_digi_seed(void);
    lssim_digi_seed();

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
    ls_notes_use_directory(sim_copy("fixtures/notes", "build/lssim_notes"));
    { extern void ls_mesh_desk_export_dir(const char *dir); ls_mesh_desk_export_dir("build/lssim_mesh");
      extern void ls_adsb_desk_export_dir(const char *dir); ls_adsb_desk_export_dir("build/lssim_adsb");
      extern void ls_p25_desk_export_dir(const char *dir); ls_p25_desk_export_dir("build/lssim_p25");
      extern void ls_journal_desk_export_dir(const char *dir); ls_journal_desk_export_dir("build/lssim_journal"); }
    /* FREQS: a card with the country bank the units carry and a mine.csv.
       LSSIM_FREQS_EMPTY: no card, so the built-in set. LSSIM_FREQS_FROM:
       the banks of another directory copied onto the card instead. */
    if (getenv("LSSIM_FREQS_EMPTY")) ls_freqs_set_root("build/lssim_no_card");
    else {
        const char *from = getenv("LSSIM_FREQS_FROM");
        ls_freqs_set_root(sim_copy(from && *from ? from : "fixtures/freqs", "build/lssim_sdcard/freqs"));
        sim_copy_file("../tools/freqs/argentina.csv", "build/lssim_sdcard/freqs/argentina.csv");
    }
    if (!empty) {
        feed_waterfall();
        feed_map();
        feed_adsb();
        replay_uat();
        feed_pages();
    }

#ifdef LS_CARTOCORE_HOST
    const char *cc_fill=getenv("LSSIM_CARTOCORE");
    if(cc_fill) {
        extern uint32_t settings_get_map_layers(uint32_t);
        extern void settings_set_map_layers(uint32_t);
        extern void ls_scr_map_force_view(int);
        (void)settings_get_map_layers;(void)settings_set_map_layers;
        ls_scr_map_force_view(!strcmp(cc_fill,"braille")?4:!strcmp(cc_fill,"picture")?5:3);
        double lat=43.4445,lon=-71.6473;
        const char *center=getenv("LSSIM_CARTO_CENTER");if(center) sscanf(center,"%lf,%lf",&lat,&lon);
        if(center || !getenv("LSSIM_MAP_START")) ls_map_center(lat,lon);
        const char *zoom=getenv("LSSIM_CARTO_ZOOM");
        ls_map_zoom_by((zoom?atoi(zoom):14)-ls_map_zoom());
    }
#endif
    /* Explicit host visual matrix controls; no device settings are changed. */
    const char *mv=getenv("LSSIM_MAP_VIEW"), *mz=getenv("LSSIM_MAP_ZOOM");
    /* The product default (Auto) draws the world line map until the card has a map; the fixture
       archive is what these renders are of, so pin the file view unless a run picks its own. */
    if(!mv && !getenv("LSSIM_CARTOCORE") && !getenv("LSSIM_MAP_START") && !getenv("LSSIM_MAP_SOURCE") && !getenv("LSSIM_MAP_LOOK")) mv="0";
    if(mv) {
        extern uint32_t settings_get_map_layers(uint32_t);
        extern void settings_set_map_layers(uint32_t);
        extern void ls_scr_map_force_view(int);
        (void)settings_get_map_layers;(void)settings_set_map_layers;
        ls_scr_map_force_view(atoi(mv));
    }
    /* SOURCE (0 auto, 1 local, 2 online) and the look (0 lines, 1 filled,
       2 blocks, 3 dots, 4 detailed), as MAP saves them. */
    if(getenv("LSSIM_MAP_SOURCE") || getenv("LSSIM_MAP_LOOK")) {
        extern uint32_t settings_get_map_layers(uint32_t);
        extern void settings_set_map_layers(uint32_t);
        const char *src=getenv("LSSIM_MAP_SOURCE"), *look=getenv("LSSIM_MAP_LOOK");
        uint32_t bits=settings_get_map_layers(0x7ff7)&0x0f1fffffU;
        /* LSSIM_MAP_PALETTE: the STYLE colours as saved (0 theme, 8 NIGHT, 1..7 the others). */
        const char *pal=getenv("LSSIM_MAP_PALETTE");
        settings_set_map_layers(bits|(1u<<23)|((uint32_t)(src?atoi(src):0)<<21)|((uint32_t)(look?atoi(look):0)<<28)|
                                ((uint32_t)(pal?atoi(pal)&15:0)<<24));
    }
    { const char *at=getenv("LSSIM_MAP_AT"); double la,lo; if(at && sscanf(at,"%lf,%lf",&la,&lo)==2) ls_map_center(la,lo); }
    if(mz) ls_map_zoom_by(atoi(mz)-ls_map_zoom());
    if(getenv("LSSIM_MAP_FRAMES")) { extern void ls_map_frames_command(const char *); ls_map_frames_command("on"); }
    /* -C: what the board registers, counted. The built-in sets went in above;
       MAP and P25 add theirs when entered, so every screen is entered once.
       CELL is not linked here and adds 3 values and 2 actions on the board,
       so a table that only just fits here is one that overflows there. */
    if (census) {
        enum { SPARE = 8 };
        for (int i = 0; i < ls_tui_screen_count(); i++) ls_tui_screen_show(i);
        const int nv = ls_value_count(), na = ls_action_count();
        printf("lssim: values %d/%d, actions %d/%d, refused %d and %d\n",
               nv, LS_VALUE_MAX, na, LS_ACTION_MAX,
               ls_value_refused(), ls_action_refused());
        return (ls_value_refused() || ls_action_refused() ||
                LS_VALUE_MAX - nv < SPARE || LS_ACTION_MAX - na < SPARE) ? 1 : 0;
    }

    /* -a: what an SD app's lap_action does (READ granted, one text argument),
       before the first frame, e.g. -a "store.open dice". */
    if (act_line) {
        char nm[48];
        const char *sp = strchr(act_line, ' ');
        snprintf(nm, sizeof(nm), "%.*s", sp ? (int)(sp - act_line) : (int)strlen(act_line), act_line);
        ls_args_t in;
        ls_val_t v;
        memset(&in, 0, sizeof(in));
        memset(&v, 0, sizeof(v));
        if (sp) { in.n = 1; in.v[0].kind = LS_VAL_TEXT; in.v[0].s = sp + 1; }
        const ls_act_status_t st = ls_action_call(nm, &in, &v, LS_CAP_READ);
        printf("lssim: action %s -> %d %s\n", nm, (int)st, v.kind == LS_VAL_TEXT && v.s ? v.s : "");
    }
    if (route_path && !ls_route_live_load(route_path,false)) {
        fprintf(stderr,"lssim: cannot load route %s\n",route_path);return 1;
    }
    if (getenv("LSSIM_RS41")) {
        extern void lssim_seed_rs41(void);
        lssim_seed_rs41();
        if (!strcmp(want,"experiments")) {
            extern const ls_experiment_t exp_rs41;
            ls_exp_register_builtin(); ls_exp_start(&exp_rs41); ls_exp_service();
        }
    }
    lssim_cartocore_pump();
    ls_tui_screen_show(ls_tui_screen_index_of(APPS[idx].screen));
    /* LSSIM_BATT=<pct>[c|u|f]: a battery gauge, discharging, or charging,
       on USB idle, or on USB full. LSSIM_CLOCK=<text>: the status clock. */
    if (getenv("LSSIM_BATT")) {
        extern void ls_shim_gauge(const ls_gauge_t *g);
        const char *b = getenv("LSSIM_BATT");
        const char st = b[strspn(b, "0123456789")];
        ls_gauge_t g = { .present = true, .percent = (uint8_t)atoi(b), .millivolts = 3900 };
        g.charging = st == 'c';
        g.full = st == 'f';
        g.plugged = st == 'c' || st == 'u' || st == 'f';
        g.state = g.full ? LS_BATT_FULL : g.charging ? LS_BATT_CHARGING : g.plugged ? LS_BATT_PLUGGED : LS_BATT_DISCHARGING;
        g.milliamps = g.charging ? 450 : g.plugged ? 0 : -380;
        ls_shim_gauge(&g);
    }
    if (getenv("LSSIM_CLOCK")) ls_tui_status_set_clock(getenv("LSSIM_CLOCK"));
    /* lssim lock: the lock page with a PIN of 1234 set through its own pad;
       lock0: with no PIN; lockset: setting a new PIN. */
    if (!strncmp(want, "lock", 4) && strcmp(want, "locker")) {
        ls_tui_status_set_clock("14:07Z");
        ls_lock_init();
        if (!strcmp(want, "lock")) {
            ls_lock_pin_edit();
            for (int r = 0; r < 2; r++) {
                for (const char *p = "1234"; *p; p++) ls_tui_router_key(LS_TK_CHAR, *p);
                if (!r) ls_tui_router_key(LS_TK_ENTER, 0);
            }
            ls_tui_set_locked(true);
        } else if (!strcmp(want, "lockset")) ls_lock_pin_edit();
        else ls_tui_set_locked(true);
    }
    if(!strcmp(want,"map") && getenv("LSSIM_MAP_RECEIVED")) {
        ls_mesh_msg_t msg={.direct=true};ls_notice_t notice={0};
        snprintf(msg.peer,sizeof(msg.peer),"1234567890abcdef");
        snprintf(msg.sender,sizeof(msg.sender),"KB1QWE");
        snprintf(msg.text,sizeof(msg.text),"WPT RECEIVED CAMP [LS1 1234abcd 1/1] 43.44450,-71.64730");
        if(ls_scr_map_receive(&msg,&notice) && notice.open) notice.open(notice.item);
    }
    if(!strcmp(want,"calls"))seed_calls(empty);
    /* LSSIM_P25_TGS: the P25 page with heard talkgroups in ACTIVITY. */
    if((!strcmp(want,"p25") || desk) && getenv("LSSIM_P25_TGS")) {
        void lssim_seed_p25_calls(void);
        lssim_seed_p25_calls();
    }
    if (replay_path) {
        subghz_file_t file;
        int32_t edges[4096];
        char line[6144];
        if (!subghz_file_load(replay_path,&file,edges,4096,line,sizeof(line)) ||
            !ls_scr_rec_replay_file(replay_path,&file,edges)) {
            fprintf(stderr,"lssim: unsupported replay file %s\n",replay_path);
            return 1;
        }
    }
    if (desk) {
        /* The tray's clock: this computer's, in UTC, as the board's GPS would give it. */
        const time_t now = time(NULL);
        const struct tm *g = gmtime(&now);
        char clk[12];
        snprintf(clk, sizeof(clk), "%02d:%02dZ", g ? g->tm_hour : 0, g ? g->tm_min : 0);
        ls_tui_status_set_clock(clk);
        /* LSSIM_DESK_WALL=N: a built-in wallpaper, as if saved last run. */
        if (getenv("LSSIM_DESK_WALL")) {
            extern void settings_set_desk_wallpaper(uint32_t id);
            settings_set_desk_wallpaper((uint32_t)atoi(getenv("LSSIM_DESK_WALL")));
        }
        ls_desk_set_enabled(true);
        /* LSSIM_DESK_THEME=classic, or a scheme number (0 Bay 95 .. 5 Night). */
        if (getenv("LSSIM_DESK_THEME")) {
            const char *t = getenv("LSSIM_DESK_THEME");
            ls_desk_set_theme(!strcmp(t, "classic") ? LS_DESK_THEME_CLASSIC95 : atoi(t));
        }
        /* LSSIM_DESK_OPEN=id: that app in a window; LSSIM_DESK_MAX=1 maximizes it,
           LSSIM_DESK_SIZE=WxH sizes it instead (an app window: the smallest preset
           at least that big), LSSIM_DESK_PRESET=0..3 Small, Medium, Large, Maximized. */
        if (getenv("LSSIM_DESK_OPEN")) {
            const int id = ls_desk_open_app(getenv("LSSIM_DESK_OPEN"));
            int ww = 0, wh = 0;
            ls_shim_time_advance(1000000);              /* the opening zoom has ended by the shot */
            frame(8);
            if (id >= 0 && getenv("LSSIM_DESK_MAX")) ls_desk_maximize(id);
            else if (id >= 0 && getenv("LSSIM_DESK_PRESET")) ls_desk_set_size(id, atoi(getenv("LSSIM_DESK_PRESET")));
            else if (id >= 0 && getenv("LSSIM_DESK_SIZE") && sscanf(getenv("LSSIM_DESK_SIZE"), "%dx%d", &ww, &wh) == 2) {
                ls_desk_resize(id, ww, wh);
                ls_desk_move(id, 2, 1);
            }
            ls_shim_time_advance(1000000);              /* and so has the zoom of a size change */
            frame(1);
            ls_desk_win_info_t wi;
            for (int z = 0; z < ls_desk_window_count(); z++)
                if (ls_desk_window_at(z, &wi))
                    printf("lssim: window %d at %d,%d %dx%d client %d,%d %dx%d\n", wi.id, wi.x, wi.y, wi.w, wi.h,
                           wi.cx, wi.cy, wi.cw, wi.ch);
        }
        /* LSSIM_DESK_LAT=18,22,95,...: round trips as the page reports them
           ('term lat'), one a second, for the tray's latency line. */
        const char *lat = getenv("LSSIM_DESK_LAT");
        for (const char *p = lat; p && *p;) {
            const int ms = atoi(p);
            ls_input_latency_report(ms, ms + ms / 3);
            ls_shim_time_advance(1100000);
            frame(1);
            p = strchr(p, ',');
            if (p) p++;
        }
    }
    ls_tui_invalidate();
    frame(settle);
    /* --term: from here the person at the terminal drives it. */
    if (term) {
        int lssim_term_run(void (*frame)(int));
        return lssim_term_run(frame);
    }
    /* --feed: the board's cell feed on stdout and its console words on stdin,
       for LakeShark Studio's run panel (app-sdk/studio). */
    if (feed) {
        int lssim_feed_run(void (*frame)(int), size_t (*records)(const uint8_t **));
        return lssim_feed_run(frame, feed_now);
    }
    if (getenv("LSSIM_FM_LR") && getenv("LSSIM_LR_IQ")) {
        /* Feed after the real panel has allocated its pixel history too. */
        feed_waterfall(); frame(1);
    }

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

    if (events) {
        if (!ls_input_decode_hex(events)) { fprintf(stderr, "lssim: invalid --ev hex\n"); return 1; }
        frame(1);
    }

    if (getenv("LSSIM_MAP_STRESS")) map_stress(atoi(getenv("LSSIM_MAP_STRESS")));
#ifdef LS_CARTOCORE_HOST
    if (getenv("LSSIM_CARTO_STRESS")) carto_stress(atoi(getenv("LSSIM_CARTO_STRESS")));
    if (getenv("LSSIM_CARTO_PERF")) carto_perf();
#endif
    if (keys) {
        for (const char *k = keys; *k; k++) {
            ls_tui_router_key(LS_TK_CHAR, *k);
            frame(1);
        }
        frame(settle);
    }

    /* The signal page may have enabled the tap only after navigation. */
    if (!empty && getenv("LSSIM_LR_IQ")) {
        feed_waterfall();
        frame(1);
    }

    /* An ordered script, because -K and -k each run as a block and the thing
       worth checking is usually a tap, then some typing, then another tap:
       open the keyboard, write a message, press OK. Two blocks cannot say
       that in either order. A token is a tap, `+text` to type, or `@name`
       for one of the named keys. */
    if (taps) {
        char buf[4096];
        snprintf(buf, sizeof(buf), "%s", taps);
        bool desk_clock_set = false;
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            if (tok[0] == '+') {
                for (const char *c = tok + 1; *c; c++) {
                    if (desk) desk_key_in(LS_TK_CHAR, *c == '_' ? ' ' : *c);
                    else ls_tui_router_key(LS_TK_CHAR, *c == '_' ? ' ' : *c);
                    frame(1);
                }
                continue;
            }
            if (desk && tok[0] == 'M' && tok[1] >= '0' && tok[1] <= '9') { s_sim_mods = (uint8_t)atoi(tok + 1); continue; }
            /* W c:r:n turns the wheel n notches (negative: up) over a cell. */
            if (desk && tok[0] == 'W' && tok[1] >= '0' && tok[1] <= '9') {
                int a = -1, bb = -1, n = 1;
                if (sscanf(tok + 1, "%d:%d:%d", &a, &bb, &n) < 2) { printf("lssim: bad wheel '%s'\n", tok); return 1; }
                const ls_input_ev_t e = {.kind = LS_IEV_WHEEL, .source = LS_ISRC_LOCAL, .col = (int16_t)a, .row = (int16_t)bb,
                                         .wheel = (int16_t)n};
                desk_in(&e); frame(1);
                continue;
            }
            if (tok[0] == '@') {
                ls_tk_t k;
                if (!named_key(tok + 1, &k)) {
                    printf("lssim: no key '%s'\n", tok + 1);
                    return 1;
                }
                if (desk) desk_key_in(k, 0);
                else ls_tui_router_key(k, 0);
                frame(1);
                continue;
            }
            if (tok[0] == '#') { perf_label(tok + 1); continue; }   /* LSSIM_PERF: names what follows */
            s_perf_input = tok[0] != '~' && tok[0] != '=';   /* frames this token draws answer input */
            if (tok[0] == '~') {                 /* let something finish: a map drawing */
                frame(atoi(tok + 1) > 0 ? atoi(tok + 1) : 1);
                continue;
            }
            /* The desk: !app opens an app's window, R c:r right-clicks,
               H c:r hovers, D c:r>c:r drags with the left button. */
            /* !display:N opens Display Properties on tab N, !shutdown the Shut Down dialog. */
            /* K[cas]:key a key with Ctrl/Alt/Shift held (a name, or one character);
               U[ca] lets the modifier go. */
            if (desk && tok[0] == 'K' && strchr(tok, ':')) {
                ls_input_ev_t e = {.source = LS_ISRC_LOCAL, .kind = LS_IEV_KEY_DOWN, .clicks = 1};
                const char *key = strchr(tok, ':') + 1;
                for (const char *m = tok + 1; *m != ':'; m++)
                    e.mods |= *m == 'c' ? LS_IMOD_CTRL : *m == 'a' ? LS_IMOD_ALT : *m == 's' ? LS_IMOD_SHIFT : 0;
                ls_tk_t k = LS_TK_CHAR;
                static const struct { const char *n; ls_tk_t k; } X[] = {
                    {"f1", LS_TK_F1}, {"f2", LS_TK_F2}, {"f4", LS_TK_F4}, {"f6", LS_TK_F6}, {"f10", LS_TK_F10},
                    {"f9", LS_TK_F9}, {"f11", LS_TK_F11},
                    {"del", LS_TK_DELETE}, {"home", LS_TK_HOME}, {"end", LS_TK_END}, {"bksp", LS_TK_BACKSPACE},
                    {"pgdn", LS_TK_PGDN}, {"pgup", LS_TK_PGUP},
                };
                bool found = false;
                for (unsigned i = 0; i < sizeof(X) / sizeof(X[0]) && !found; i++) if (!strcmp(key, X[i].n)) { k = X[i].k; found = true; }
                if (!found && key[1] && !named_key(key, &k)) { printf("lssim: no key '%s'\n", key); return 1; }
                e.key = (uint16_t)k;
                if (k == LS_TK_CHAR) e.ch = key[0] == '_' ? ' ' : key[0];
                ls_desk_input(&e); frame(1);
                continue;
            }
            if (desk && tok[0] == 'U' && (tok[1] == 'c' || tok[1] == 'a') && !tok[2]) {
                ls_input_ev_t e = {.source = LS_ISRC_LOCAL, .kind = LS_IEV_KEY_UP, .key = tok[1] == 'c' ? LS_TK_CTRL : LS_TK_ALT};
                ls_desk_input(&e); frame(1);
                continue;
            }
            if (tok[0] == '!') {
                if (!strcmp(tok + 1, "about")) ls_desk_open_about();
                else if (!strcmp(tok + 1, "run")) ls_desk_shell_open("run", -1);
                else if (!strcmp(tok + 1, "tasks")) ls_desk_shell_open("tasks", -1);
                else if (!strcmp(tok + 1, "bin")) ls_desk_shell_open("bin", -1);
                else if (!strcmp(tok + 1, "folder")) ls_desk_shell_open("folder", -1);
                else if (!strncmp(tok + 1, "control", 7)) ls_desk_shell_open("control", tok[8] == ':' ? atoi(tok + 9) : -1);
                else if (!strncmp(tok + 1, "display", 7)) ls_desk_open_display(tok[8] == ':' ? atoi(tok + 9) : 0);
                else if (!strcmp(tok + 1, "shutdown")) ls_desk_open_shutdown();
                else if (!strcmp(tok + 1, "welcome")) ls_desk_welcome();
                else ls_desk_open_app(tok + 1);
                frame(1);
                continue;
            }
            /* =MS moves the clock on by MS, one frame drawn: a mid-animation
               shot stops the clock there (the last frames do not move it). */
            if (tok[0] == '=') {
                ls_shim_time_advance((int64_t)atoi(tok + 1) * 1000);
                desk_clock_set = true;
                frame(1);
                continue;
            }
            /* ^ maximizes the front window (and ^ again restores it). */
            if (tok[0] == '^' && !tok[1]) { ls_desk_maximize(ls_desk_front()); frame(1); continue; }
            /* &volume, &clock...: a tray popup; &&volume its right-button menu. */
            if (tok[0] == '&') { ls_desk_tray_popup(tok + (tok[1] == '&' ? 2 : 1), tok[1] == '&'); frame(1); continue; }
            if (tok[0] == 'C' && tok[1] >= '0' && tok[1] <= '9') {      /* C c:r a left click */
                int a = -1, bb = -1;
                if (sscanf(tok + 1, "%d:%d", &a, &bb) != 2) { printf("lssim: bad click '%s'\n", tok); return 1; }
                ls_input_ev_t e = {.source = LS_ISRC_LOCAL, .clicks = 1, .col = (int16_t)a, .row = (int16_t)bb,
                                   .kind = LS_IEV_PTR_DOWN, .button = LS_IBTN_LEFT, .buttons = 1};
                desk_in(&e); frame(1);
                e.kind = LS_IEV_PTR_UP; e.buttons = 0; desk_in(&e); frame(1);
                continue;
            }
            /* S c:r>c:r: a finger on an app screen goes down, moves in four
               frames and lifts (a trailing ? keeps it down). */
            if (tok[0] == 'S' && tok[1] >= '0' && tok[1] <= '9') {
                int a = -1, b = -1, c = -1, d = -1, cw = 10, chh = 17;
                if (sscanf(tok + 1, "%d:%d>%d:%d", &a, &b, &c, &d) != 4) { printf("lssim: a swipe wants S c:r>c:r\n"); return 1; }
                ls_tui_geometry(NULL, NULL, &cw, &chh);
                for (int k = 0; k <= 4; k++) {
                    const int col = a + (c - a) * k / 4, row = b + (d - b) * k / 4;
                    ls_tui_touch_remote(col, row, col * cw + cw / 2, row * chh + chh / 2, true);
                    frame(1);
                }
                if (tok[strlen(tok) - 1] != '?') {
                    ls_tui_touch_remote(c, d, c * cw + cw / 2, d * chh + chh / 2, false);
                    frame(1);
                }
                continue;
            }
            if (tok[0] == 'R' || tok[0] == 'H' || tok[0] == 'D') {
                int a = -1, b = -1, c = -1, d = -1;
                const int got = sscanf(tok + 1, "%d:%d>%d:%d", &a, &b, &c, &d);
                if (got < 2) { printf("lssim: bad desk token '%s'\n", tok); return 1; }
                ls_input_ev_t e = {.source = LS_ISRC_LOCAL, .clicks = 1, .col = (int16_t)a, .row = (int16_t)b};
                if (tok[0] == 'H') { e.kind = LS_IEV_PTR_MOVE; desk_in(&e); frame(1); continue; }
                if (tok[0] == 'R') {
                    e.kind = LS_IEV_PTR_DOWN; e.button = LS_IBTN_RIGHT; e.buttons = 1 << LS_IBTN_RIGHT;
                    desk_in(&e); frame(1);
                    e.kind = LS_IEV_PTR_UP; e.buttons = 0; desk_in(&e); frame(1);
                    continue;
                }
                if (got < 4) { printf("lssim: a drag wants D c:r>c:r\n"); return 1; }
                e.kind = LS_IEV_PTR_DOWN; e.button = LS_IBTN_LEFT; e.buttons = 1;
                desk_in(&e); frame(1);
                for (int k = 1; k <= 4; k++) {
                    e.kind = LS_IEV_PTR_MOVE;
                    e.col = (int16_t)(a + (c - a) * k / 4); e.row = (int16_t)(b + (d - b) * k / 4);
                    desk_in(&e); frame(1);
                }
                if (tok[strlen(tok) - 1] != '?') { e.kind = LS_IEV_PTR_UP; e.buttons = 0; desk_in(&e); frame(1); }
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
        /* Without a =MS in the script, whatever the clicks started has finished
           by the shot. */
        if (desk && !desk_clock_set) ls_shim_time_advance(3000000);
        frame(settle);
    }

    {
        void lssim_ota_notice(void);
        lssim_ota_notice();
        if (getenv("LSSIM_OTA_NOTICE")) frame(settle);
    }

    if (notice) {
        ls_notice_t n;
        memset(&n, 0, sizeof(n));
        snprintf(n.title, sizeof(n.title), "%s", "DIRECT MESSAGE");
        snprintf(n.body, sizeof(n.body), "%s", notice);
        n.hue = TUI_CYAN;
        n.screen = -1;
        if (getenv("LSSIM_NOTICE_DETECTOR")) {
            /* A detector's notice, as lap_alert posts one. */
            snprintf(n.title, sizeof(n.title), "%s", "DETECTOR");
            n.hue = n.accent = TUI_BLUE | TUI_BRIGHT;
            n.quiet = n.steady = true;
        }
        ls_notify_post(&n);
        /* The banner's own frames, not one.

           This used to draw a single frame after posting, which pinned every
           -n render to slide age 1 - a two-row box that has not grown enough
           to draw its text yet (ls_notify.c only puts the words in once the
           box is tall enough). So the banner's slide, its flash and its
           expiry could not be looked at from here at all, which is awkward
           for the one piece of animation the code comments actually ask to
           improve. With -n, -f now means how many frames into the banner's
           life to render: 1 is the first sliver, 4 has it open, 150 is the
           last frame before it goes. */
        frame(settle);
    }

    if (timed > 0) time_frames(timed, moving);

    if (splash) {
        int cols, rows;
        ls_tui_geometry(&cols, &rows, NULL, NULL);
        /* LSSIM_SPLASH_FRAME=N draws frame N of 93, the board's count;
           LSSIM_SPLASH_CAPTION is the version line under it. */
        const char *at = getenv("LSSIM_SPLASH_FRAME"), *caption = getenv("LSSIM_SPLASH_CAPTION");
        ls_splash_set_caption(caption ? caption : "2.18.0-g0000000");
        ls_splash_draw(ls_tui_surface(), cols, rows, at ? atoi(at) : 93, 93);
        ls_tui_present();capture_frame();
    }
    /* A bench-only snapshot for measuring the production cells feed. */
    if (getenv("LSSIM_VISUAL_OUT")) {
        /* Host-only fixture: real image, ring origin, glass coverage and the
           rendered RGB565 reference. Requires LSSIM_GRID (no panel inset). */
        FILE *f=fopen(getenv("LSSIM_VISUAL_OUT"),"wb");assert(f);
        tui_surface *sf=ls_tui_surface();tui_rect rect={0};
        const uint16_t *src=NULL;int sw=0,sh=0,x0=0,y0=0,cw=0,ch=0;uint32_t serial=0;
        ls_tui_geometry(NULL,NULL,&cw,&ch);
        ls_tui_image_now(&rect,&src,&sw,&sh,&serial);ls_tui_image_ring_offset(&x0,&y0);
        size_t gn=0;const uint8_t *glass=ls_tui_glass_now(&gn);unsigned count=0;
        for(int i=0;i<sf->w*sf->h;i++) if(glass && (glass[i>>3]>>(i&7)&1)) count++;
        uint32_t meta[]={sf->w,sf->h,font,cw,ch,rect.x,rect.y,rect.w,rect.h,sw,sh,x0,y0,serial,count};
        fwrite("LSP2",1,4,f);fwrite(meta,sizeof(meta),1,f);
        const ls_tui_theme_t *th=ls_tui_theme_effective(ls_tui_get_theme(),ls_tui_daylight());
        fwrite(th->palette,2,16,f);fwrite(sf->front,sizeof(tui_cell),(size_t)sf->w*sf->h,f);
        fwrite(glass,1,gn,f);if(src) fwrite(src,2,(size_t)sw*sh,f);
        for(int i=0;i<sf->w*sf->h;i++) if(glass && (glass[i>>3]>>(i&7)&1)) {
            uint16_t head[]={i,(uint16_t)sf->front[i].ch};uint8_t mask[1024];bool halo=false;
            ls_tui_glass_mask(head[1],mask,&halo);fwrite(head,2,2,f);fputc(halo,f);fwrite(mask,1,(size_t)cw*ch,f);
        }
        uint32_t ring[4]={0};ring[0]=ls_tui_image_ring_state(&ring[1],&ring[2],&ring[3]);
        fwrite(ring,sizeof(ring),1,f);
        fwrite(g_fb,2,(size_t)sf->w*cw*sf->h*ch,f);fclose(f);
    }
    if (desk && getenv("LSSIM_DESK_COST")) { char *av[] = {"desk", "cost"}; ls_desk_console(2, av); }
    if (getenv("LSSIM_CELLS_OUT")) {
        FILE *f = fopen(getenv("LSSIM_CELLS_OUT"), "wb");
        tui_surface *sf = ls_tui_surface();
        if (!f || fwrite(sf->back, sizeof(tui_cell), (size_t)sf->w * sf->h, f) != (size_t)sf->w * sf->h) return 1;
        fclose(f);
    }
    if (getenv("LSSIM_FEED_OUT") && write_feed(getenv("LSSIM_FEED_OUT")) != 0) return 1;
    /* LSSIM_MAP_FRAMES=1: MAP's frame trace ('carto frames'), printed at the end. */
    if (getenv("LSSIM_MAP_FRAMES")) { extern void ls_map_frames_command(const char *); ls_map_frames_command(""); }
    if (write_bmp(out) != 0) {
        printf("lssim: cannot write %s\n", out);
        return 1;
    }

    if (getenv("LSSIM_C6_AUDIT")) {
        for (int i = 0; i < ls_btn_count_slot(LS_BTN_SLOT_SCREEN); ++i) {
            int x, y, w, h;
            if (ls_btn_rect_slot(LS_BTN_SLOT_SCREEN, i, &x, &y, &w, &h)) {
                printf("C6 KEY %d %d %d %d %d\n", i, x, y, w, h);
                if (w < 8 || h < 3) return 2;
            }
        }
    }
    if (dump) {
        dump_grid(stdout);
#if defined(CONFIG_LS_EXAMPLE_APP) && CONFIG_LS_EXAMPLE_APP
        if (!strcmp(want, "example")) {
            for (int i = 0; i < ls_btn_count_slot(LS_BTN_SLOT_SCREEN); ++i) {
                int x, y, w, h;
                if (ls_btn_rect_slot(LS_BTN_SLOT_SCREEN, i, &x, &y, &w, &h))
                    printf("example-button %d %d %d %d %d\n", i, x, y, w, h);
            }
        }
#endif

#ifdef LS_MAP_AUDIT
        if (!strcmp(want, "map")) {
            extern int ls_map_label_audit(int *labels);
            int labels = 0;
            const int bad = ls_map_label_audit(&labels);
            printf("label audit: %d labels, %d overlaps\n", labels, bad);
            extern int ls_map_carto_label_audit(void);
            int broken=ls_map_carto_label_audit();
            printf("CartoCore label audit: %d broken cells/plates\n",broken);
            extern int ls_map_carto_low_masked(void);
            printf("CartoCore label priority: %d low-priority cells masked\n",ls_map_carto_low_masked());
            if(broken) return 1;
        }
#endif
        if (getenv("LSSIM_RS41")) {
            extern void lssim_report_rs41_target(void);
            lssim_report_rs41_target();
        }
    }

    int cols, rows;
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    printf("lssim: %s %s %dx%d cells -> %s\n", want,
           landscape ? "landscape" : "portrait", cols, rows, out);
    return 0;
}
