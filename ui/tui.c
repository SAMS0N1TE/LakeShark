/*
 * tui.c - shared TUI: header, page bar, content pages, footer.
 *
 * Render order each frame:
 *   1. Cursor home (\033[H) - no CLS to avoid flicker.
 *   2. Header (app name, frequency, rate, uptime, stats).
 *   3. Page tab bar (MAIN / SIGNAL / LOG / SETTINGS).
 *      The SIGNAL label is supplied per-app via app_t::signal_label,
 *      so it reads MODE-S on ADS-B and DEMOD on P25, etc.
 *   4. Page body - dispatches to:
 *        PAGE_MAIN     Ã¢â€ â€™ active app's draw_main()
 *        PAGE_SIGNAL   -> active apps draw_signal() if set, else built-in
 *        PAGE_LOG      Ã¢â€ â€™ draw_log()
 *        PAGE_SETTINGS Ã¢â€ â€™ draw_settings() (with key handling inline)
 *   5. Footer - key hints and audio modes.
 *
 * Every visible line ends with EL so stale glyphs from the previous
 * frame get wiped without a full CLS.
 */
#include "tui.h"
#include "app_registry.h"
#include "sam_tts.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "settings.h"

#define TUI_REFRESH_MS  120

/* -- logging ring buffer --------------------------------------------- */
typedef struct {
    char text[96];
    uint8_t color;
} tui_log_entry_t;

static tui_log_entry_t s_log[TUI_LOG_LINES];
static int s_log_head = 0;
static int s_log_count = 0;

/* Log colour palette - index via the color_index param. */
static const char *const LOG_COL[] = {
    C_CYAN, C_GREEN, C_AMBER, C_MAGENTA, C_RED, C_DIM
};
#define LOG_COL_N (sizeof(LOG_COL)/sizeof(LOG_COL[0]))

static bool s_dirty = true;
static int64_t s_last_draw = 0;
static int64_t s_boot_us = 0;

void tui_mark_dirty(void) { s_dirty = true; }

void tui_log(int color_index, const char *fmt, ...)
{
    if (color_index < 0 || color_index >= (int)LOG_COL_N) color_index = 0;
    tui_log_entry_t *e = &s_log[s_log_head];
    e->color = (uint8_t)color_index;
    va_list ap; va_start(ap, fmt);
    vsnprintf(e->text, sizeof(e->text), fmt, ap);
    va_end(ap);
    s_log_head = (s_log_head + 1) % TUI_LOG_LINES;
    if (s_log_count < TUI_LOG_LINES) s_log_count++;
    s_dirty = true;
}

/* -- waterfall ------------------------------------------------------- */
/* Waterfall state: circular buffer of rows, each row is a histogram of
 * magnitude across frequency bins. Auto-gain tracks a slow-decaying
 * peak so the display stays vivid regardless of input level. */
#define WF_BINS  120
#define WF_ROWS  48             /* 48 data rows rendered as 24 terminal
                                 * lines via upper-half-block Ã¢â‚¬â€ fills
                                 * the body region. Frame cost managed
                                 * by fg+bg colour caching so we stay
                                 * well under the UART TX budget. */
static uint8_t  s_wf[WF_ROWS][WF_BINS];
static int      s_wf_row = 0;
static uint32_t s_wf_peak = 4096;     /* slow-IIR peak for auto-gain     */

void tui_waterfall_reset(void)
{
    memset(s_wf, 0, sizeof(s_wf));
    s_wf_row  = 0;
    s_wf_peak = 4096;
}

void tui_waterfall_push(const uint16_t *mag, int mag_len)
{
    if (!mag || mag_len < WF_BINS) return;
    int bin = mag_len / WF_BINS;
    int row = s_wf_row % WF_ROWS;
    uint32_t frame_peak = 1;

    /* First pass: compute bin averages + track frame peak. */
    uint32_t bins[WF_BINS];
    for (int b = 0; b < WF_BINS; b++) {
        int start = b * bin;
        int end   = start + bin;
        if (end > mag_len) end = mag_len;
        uint32_t sum = 0;
        for (int j = start; j < end; j++) sum += mag[j];
        bins[b] = sum / (uint32_t)(end - start);
        if (bins[b] > frame_peak) frame_peak = bins[b];
    }

    /* Slow IIR peak track: fast attack, slow decay. This fixes the
     * "everything is white" problem - the old code normalised to a
     * fixed divisor, so a weak band scaled up to full white. */
    if (frame_peak > s_wf_peak) {
        s_wf_peak = frame_peak;
    } else {
        s_wf_peak = s_wf_peak - (s_wf_peak >> 6) + (frame_peak >> 6);
    }
    if (s_wf_peak < 512) s_wf_peak = 512;

    /* Second pass: normalise 0..255 against tracked peak. */
    for (int b = 0; b < WF_BINS; b++) {
        uint32_t v = (bins[b] * 255) / s_wf_peak;
        if (v > 255) v = 255;
        s_wf[row][b] = (uint8_t)v;
    }
    s_wf_row++;
}

/* -- helper primitives ----------------------------------------------- */
void tui_goto(int row, int col) { printf("\033[%d;%dH", row, col); }

void tui_hline(int row, int col, int n)
{
    tui_goto(row, col);
    printf(C_BORDER);
    for (int i = 0; i < n; i++) printf(HL);
    printf(RESET);
}

void tui_pad(const char *s, int width)
{
    int len = 0;
    if (s) {
        /* byte length approximation - OK for ASCII content we control */
        len = (int)strlen(s);
        if (len > width) len = width;
        fwrite(s, 1, len, stdout);
    }
    for (int i = len; i < width; i++) fputc(' ', stdout);
}

static void fmt_uptime(char *buf, size_t sz)
{
    int64_t s = (esp_timer_get_time() - s_boot_us) / 1000000LL;
    int h = (int)(s / 3600);
    int m = (int)((s / 60) % 60);
    int sec = (int)(s % 60);
    snprintf(buf, sz, "%02d:%02d:%02d", h, m, sec);
}

static void fmt_hz(char *buf, size_t sz, uint32_t hz)
{
    /* 1090000000 Ã¢â€ â€™ "1090.000 MHz", 162025000 Ã¢â€ â€™ "162.025 MHz" */
    uint32_t mhz = hz / 1000000;
    uint32_t khz_frac = (hz % 1000000) / 1000;
    snprintf(buf, sz, "%lu.%03lu MHz",
             (unsigned long)mhz, (unsigned long)khz_frac);
}

/* -- header row ------------------------------------------------------ */
static void draw_header(void)
{
    const app_t *a = app_current();
    char freq[24], uptime[16];
    fmt_hz(freq, sizeof(freq), a ? settings_get_freq(a) : 0);
    fmt_uptime(uptime, sizeof(uptime));

    tui_goto(1, 1);
    printf(C_BORDER TL);
    for (int i = 0; i < TUI_COLS - 2; i++) printf(HL);
    printf(TR EL RESET "\n");

    tui_goto(2, 1);
    printf(C_BORDER VL RESET);
    /* Banner is per-app: ADS-B is "ATC TERMINAL", P25 is "TRUNK MONITOR",
     * etc. Falls back to a neutral string if the app didn't set one. */
    const char *banner = (a && a->banner) ? a->banner : "SDR TERMINAL";
    printf(" " C_GOLD BOLD "%s" RESET, banner);
    printf(C_DIM " // " RESET C_BRIGHT "%s" RESET, a ? a->name : "?");
    printf(C_DIM " // " RESET C_CYAN "%s" RESET, freq);
    printf(C_DIM " // " RESET C_BLUE "%lu kSps" RESET,
           (unsigned long)((a ? a->default_rate : 0) / 1000));
    printf(C_DIM "    " RESET);
    printf(C_LABEL "UPTIME " RESET C_TEXT "%s" RESET, uptime);
    /* Right-justify filler */
    printf(EL);
    /* Redraw right border at column TUI_COLS. */
    tui_goto(2, TUI_COLS);
    printf(C_BORDER VL RESET);
}

/* -- page tab bar at row 3 ------------------------------------------- */
static void draw_page_tabs(void)
{
    /* Page names: tabs 2 and 5 are app-relabelable via signal_label /
     * diag_label. Other tabs are framework-owned. */
    const app_t *cur_app = app_current();
    const char *sig_label =
        (cur_app && cur_app->signal_label) ? cur_app->signal_label : "SIGNAL";
    const char *diag_label =
        (cur_app && cur_app->diag_label)   ? cur_app->diag_label   : "DIAG";
    const char *page_names[PAGE_COUNT] = {
        "MAIN", sig_label, "LOG", "SETTINGS", diag_label
    };
    page_t cur = page_current();

    tui_goto(3, 1);
    printf(C_BORDER TR_ RESET);
    printf(C_BORDER);
    for (int i = 0; i < TUI_COLS - 2; i++) printf(HL);
    printf(TL_ EL RESET);

    tui_goto(4, 1);
    printf(C_BORDER VL RESET);
    for (int i = 0; i < PAGE_COUNT; i++) {
        printf("  ");
        if (i == (int)cur) {
            printf(C_BG_ACT C_BRIGHT BOLD " %d:%s " RESET,
                   i + 1, page_names[i]);
        } else {
            printf(C_DIM " %d:%s " RESET, i + 1, page_names[i]);
        }
    }
    /* Apps list on the right side of the tab row. Now actually
     * enumerates registered apps via app_at()/app_count() instead of
     * just printing the active one's name. */
    printf("  " C_LABEL "APP: " RESET);
    int n = app_count();
    int active = app_current_index();
    for (int i = 0; i < n; i++) {
        const app_t *ai = app_at(i);
        if (!ai || !ai->name) continue;
        if (i == active) printf(C_GOLD BOLD " %s " RESET, ai->name);
        else             printf(C_DIM     " %s " RESET, ai->name);
    }
    printf(C_DIM "[A] next" RESET);
    if (app_switch_in_progress()) {
        printf(C_AMBER " (switching..)" RESET);
    }
    printf(EL);
    tui_goto(4, TUI_COLS);
    printf(C_BORDER VL RESET);

    tui_goto(5, 1);
    printf(C_BORDER TR_);
    for (int i = 0; i < TUI_COLS - 2; i++) printf(HL);
    printf(TL_ EL RESET);
}

/* -- page: main (delegates to app) ----------------------------------- */
static void draw_page_main(int top_row, int rows, int cols)
{
    const app_t *a = app_current();
    if (a && a->draw_main) {
        a->draw_main(top_row, rows, cols);
    } else {
        tui_goto(top_row, 3);
        printf(C_DIM "(no app active)" RESET EL);
    }
}

/* -- page: waterfall ------------------------------------------------- */
/* Return an xterm-256 colour index (16..255) for a given intensity.
 *
 * 24-bit truecolour escapes (\e[38;2;R;G;Bm) are ~22 bytes each and
 * not reliably supported on ESP-IDF's serial monitor. The xterm-256
 * palette has a 216-colour 6x6x6 RGB cube (indices 16..231) and a
 * 24-level grayscale ramp (232..255), addressed with shorter escapes
 * (\e[38;5;Nm = ~11 bytes). We map the same blackÃ¢â€ â€™blueÃ¢â€ â€™cyanÃ¢â€ â€™yellowÃ¢â€ â€™
 * white heatmap into the 256-colour palette. Works on every terminal
 * made in the last 20 years and cuts frame size nearly in half. */
static uint8_t wf_256(uint8_t v)
{
    /* Black floor */
    if (v < 16) return 16;
    /* Blue ramp (16..80): 17, 18, 19 (dark blue Ã¢â€ â€™ blue) */
    if (v < 48) return 17;
    if (v < 80) return 19;      /* brighter blue */
    /* Cyan (80..160): 37, 51 */
    if (v < 112) return 38;     /* blue-cyan */
    if (v < 144) return 45;     /* cyan-green */
    if (v < 160) return 51;     /* cyan */
    /* Yellow (160..220): 190, 226 */
    if (v < 180) return 82;     /* green-yellow */
    if (v < 200) return 190;    /* yellow-green */
    if (v < 220) return 226;    /* yellow */
    /* White hot (220+): 231 */
    return 231;
}

/* Legacy 24-bit RGB helper removed Ã¢â‚¬â€ waterfall switched to xterm-256
 * via wf_256() above, and the spectrum strip uses named C_GREEN/
 * C_AMBER/C_RED colours directly, so nothing else needs RGB. */

/* -- page: signal analyzer (labelled WATERFALL in the tab bar) -------
 *
 * The historical waterfall approach (scrolling 48 rows of 120 colour
 * cells per frame) was ~15 KB of ANSI escapes per paint, which at
 * 115200 baud was slower than the phenomenon it was trying to display.
 * It looked pretty but told you very little.
 *
 * This rewrite keeps the colour aesthetic but reorganises the layout
 * around diagnostic usefulness:
 *
 *   1. Big live spectrum bar graph Ã¢â‚¬â€ wide bars (2 chars per bin)
 *      coloured green/amber/red by level. Instant "is signal present"
 *      check. Shows peak-hold (thin dim line) behind the live bars so
 *      you can see recent transients.
 *   2. SNR readout Ã¢â‚¬â€ peak/avg ratio as a number with interpretation.
 *      Mode-S preambles need SNR > 10x to decode reliably.
 *   3. Level meters Ã¢â‚¬â€ current avg and peak magnitude as horizontal
 *      bars with scale markers, so you immediately see if the
 *      receiver is saturated (red=BAD) or starved (dim=BAD).
 *   4. Burst counter Ã¢â‚¬â€ number of bins above 30000 mag in the last
 *      second. Non-zero means real ADS-B preambles are in the air.
 *   5. Compact 6-row history strip at the bottom Ã¢â‚¬â€ just enough context
 *      to see if the energy is bursty (aircraft) or continuous (noise
 *      floor), without wasting bandwidth on 48 rows.
 */

/* Peak-hold that decays slowly, for drawing the translucent peak line. */
static uint8_t s_peak_hold[WF_BINS];
static int64_t s_peak_hold_last_decay = 0;

static void update_peak_hold(const uint8_t *row)
{
    /* Fast attack: take max immediately. Slow decay: subtract 2 every
     * 100 ms. This creates a ghosting peak line that shows the recent
     * envelope behind the live bars. */
    int64_t now = esp_timer_get_time();
    bool decay = (now - s_peak_hold_last_decay) > 100000LL;
    for (int b = 0; b < WF_BINS; b++) {
        if (row[b] > s_peak_hold[b]) s_peak_hold[b] = row[b];
        else if (decay && s_peak_hold[b] >= 2) s_peak_hold[b] -= 2;
    }
    if (decay) s_peak_hold_last_decay = now;
}

/* Burst counter Ã¢â‚¬â€ count cells above preamble threshold in last frame. */
static int count_bursts(const uint8_t *row, int len, uint8_t threshold)
{
    int n = 0;
    for (int b = 0; b < len; b++) if (row[b] >= threshold) n++;
    return n;
}

static void draw_page_waterfall(int top_row, int rows, int cols)
{
    int usable_cols = cols - 4;
    if (usable_cols > WF_BINS) usable_cols = WF_BINS;

    const app_t *a = app_current();
    char hz[24]; fmt_hz(hz, sizeof(hz), a ? settings_get_freq(a) : 0);

    int newest = (s_wf_row - 1 + WF_ROWS) % WF_ROWS;
    update_peak_hold(s_wf[newest]);

    /* Stats on the current row. */
    uint32_t sum = 0, pk = 0;
    for (int b = 0; b < WF_BINS; b++) {
        sum += s_wf[newest][b];
        if (s_wf[newest][b] > pk) pk = s_wf[newest][b];
    }
    uint32_t avg = sum / WF_BINS;
    /* SNR as integer x10 for one decimal place without floats. */
    int snr_x10 = avg ? (int)((pk * 10) / avg) : 0;
    int bursts = count_bursts(s_wf[newest], WF_BINS, 180);

    /* Ã¢â€â‚¬Ã¢â€â‚¬ Header Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ */
    tui_goto(top_row, 3);
    printf(C_LABEL "SIGNAL  " RESET C_CYAN "%s" RESET
           C_DIM "  (%d bins)" RESET EL, hz, WF_BINS);

    /* Ã¢â€â‚¬Ã¢â€â‚¬ Stat bar: AVG | PEAK | SNR | BURSTS Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ */
    tui_goto(top_row + 2, 3);
    /* AVG with colour by band. */
    const char *avg_col = avg < 40 ? C_DIM :
                          avg < 120 ? C_GREEN :
                          avg < 200 ? C_AMBER : C_RED;
    const char *avg_desc = avg < 40 ? "(weak)" :
                           avg < 120 ? "(good)" :
                           avg < 200 ? "(hot) " : "(SAT!)";
    printf(C_LABEL "AVG " RESET "%s%3lu %s" RESET "  ",
           avg_col, (unsigned long)avg, avg_desc);

    /* PEAK */
    const char *pk_col = pk < 80 ? C_DIM :
                         pk < 180 ? C_GREEN :
                         pk < 240 ? C_AMBER : C_RED;
    printf(C_LABEL "PEAK " RESET "%s%3lu" RESET "  ",
           pk_col, (unsigned long)pk);

    /* SNR Ã¢â‚¬â€ Mode-S preambles decode reliably at >10:1. */
    const char *snr_col = snr_x10 < 30 ? C_DIM :
                          snr_x10 < 80 ? C_AMBER :
                          snr_x10 < 150 ? C_GREEN : C_CYAN;
    const char *snr_desc = snr_x10 < 30 ? "(flat) " :
                           snr_x10 < 80 ? "(marg) " :
                           snr_x10 < 150 ? "(good) " : "(sharp)";
    printf(C_LABEL "SNR " RESET "%s%d.%d %s" RESET "  ",
           snr_col, snr_x10 / 10, snr_x10 % 10, snr_desc);

    /* BURSTS in this frame */
    const char *brst_col = bursts == 0 ? C_DIM :
                           bursts < 3 ? C_AMBER : C_GREEN;
    printf(C_LABEL "BURSTS " RESET "%s%d" RESET EL, brst_col, bursts);

    /* Ã¢â€â‚¬Ã¢â€â‚¬ Big spectrum bar graph Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ */
    /* 8-level vertical bars per bin, 2 chars wide for visual weight.
     * Peak-hold overlaid via dim colour on the frames above live. */
    static const char *BAR_GLYPHS[9] = {
        " ",
        "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
    };

    int spec_bars = usable_cols / 2;      /* 2 chars per bar for weight */
    if (spec_bars > WF_BINS) spec_bars = WF_BINS;
    int spec_height = 10;                 /* 10 rows tall */
    int avail_rows = rows - 4;            /* after header + stat row   */
    if (spec_height > avail_rows - 4) spec_height = avail_rows - 4;
    if (spec_height < 3) spec_height = 3;

    /* Draw top to bottom. Each cell decides: empty, peak-hold glyph,
     * or live bar glyph. */
    for (int h = 0; h < spec_height; h++) {
        tui_goto(top_row + 4 + h, 3);
        /* The level this row represents Ã¢â‚¬â€ row 0 is top, row spec_height-1
         * is bottom. Bars grow upward, so we're asking "is the value at
         * or above the threshold for this row?". */
        int threshold = 255 - (h * 255) / spec_height;
        int last_col = -1;
        for (int b = 0; b < spec_bars; b++) {
            uint8_t v  = s_wf[newest][b];
            uint8_t ph = s_peak_hold[b];
            const char *glyph;
            int cc;
            if (v >= threshold) {
                /* Live bar. */
                int overshoot = v - threshold;
                int sub = (overshoot * 8) / (255 / spec_height + 1);
                if (sub < 0) sub = 0;
                if (sub > 8) sub = 8;
                glyph = (h == 0 && v > 250) ? BAR_GLYPHS[8] :
                        (sub > 0 ? BAR_GLYPHS[sub] : BAR_GLYPHS[8]);
                /* Colour live by absolute magnitude, not sub-row. */
                cc = v < 80 ? 1 : v < 180 ? 2 : v < 240 ? 3 : 4;
                                        /* 1=dim 2=green 3=amber 4=red */
            } else if (ph >= threshold) {
                /* Peak-hold ghost Ã¢â‚¬â€ faint line. */
                glyph = BAR_GLYPHS[1];
                cc = 5;                 /* 5=peak-hold dim cyan */
            } else {
                glyph = " ";
                cc = 0;
            }
            if (cc != last_col) {
                fputs(cc == 0 ? RESET :
                      cc == 1 ? C_DIM :
                      cc == 2 ? C_GREEN :
                      cc == 3 ? C_AMBER :
                      cc == 4 ? C_RED :
                                "\033[38;5;51m",    /* peak-hold cyan */
                      stdout);
                last_col = cc;
            }
            /* Print the glyph twice for 2-char-wide bars. */
            fputs(glyph, stdout);
            fputs(glyph, stdout);
        }
        fputs(RESET EL, stdout);
    }

    /* Ã¢â€â‚¬Ã¢â€â‚¬ Axis label under the bars Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ */
    int axis_row = top_row + 4 + spec_height;
    tui_goto(axis_row, 3);
    /* Print tick marks at left, center, right */
    int bar_cols = spec_bars * 2;
    printf(C_DIM "|");
    for (int i = 1; i < bar_cols / 2 - 4; i++) fputc('-', stdout);
    fputs("| CENTER |", stdout);
    for (int i = 1; i < bar_cols - bar_cols/2 - 8; i++) fputc('-', stdout);
    fputs("|" RESET EL, stdout);

    /* Ã¢â€â‚¬Ã¢â€â‚¬ Compact history strip Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ */
    /* 6 rows max of recent history, tight, just to show burst patterns.
     * Uses 256-colour palette like before, but only 6 rows means under
     * 2 KB of ANSI per frame. */
    int hist_top = axis_row + 1;
    int hist_rows = rows - (hist_top - top_row);
    if (hist_rows > 6) hist_rows = 6;
    if (hist_rows < 1) hist_rows = 0;

    if (hist_rows > 0) {
        tui_goto(hist_top, 3);
        printf(C_LABEL "HISTORY " RESET C_DIM
               "(newest top, %d rows)" RESET EL, hist_rows * 2);

        for (int line = 0; line < hist_rows; line++) {
            int top_pix = (s_wf_row - 1 - 2*line     + WF_ROWS) % WF_ROWS;
            int bot_pix = (s_wf_row - 1 - 2*line - 1 + WF_ROWS) % WF_ROWS;
            tui_goto(hist_top + 1 + line, 3);
            uint16_t last_key = 0xFFFF;
            /* Use 2-char-wide cells so the history strip aligns with
             * the spectrum bars above. */
            int hist_bars = usable_cols / 2;
            if (hist_bars > WF_BINS) hist_bars = WF_BINS;
            for (int b = 0; b < hist_bars; b++) {
                uint8_t vt = s_wf[top_pix][b];
                uint8_t vb = s_wf[bot_pix][b];
                uint8_t cf = wf_256(vt);
                uint8_t cb = wf_256(vb);
                uint16_t key = ((uint16_t)cf << 8) | cb;
                if (key != last_key) {
                    printf("\033[38;5;%u;48;5;%um", cf, cb);
                    last_key = key;
                }
                fputs("\xe2\x96\x80\xe2\x96\x80", stdout);
            }
            fputs(RESET EL, stdout);
        }
    }

    /* Clear any leftover body area below the history strip. */
    int last_drawn = hist_top - top_row + 1 + hist_rows;
    for (int i = last_drawn; i < rows; i++) {
        tui_goto(top_row + i, 3);
        fputs(EL, stdout);
    }
}

/* -- page: log ------------------------------------------------------- */
static void draw_page_log(int top_row, int rows, int cols)
{
    (void)cols;
    /* Pre-clear the body region. Without this, leftover rows from the
     * previous page leak through any row this page doesn't write to
     * (e.g. the gap row between the title and the first log line). */
    for (int r = top_row; r < top_row + rows; r++) {
        tui_goto(r, 3);
        fputs(EL, stdout);
    }

    tui_goto(top_row, 3);
    printf(C_LABEL "EVENT LOG" RESET C_DIM
           "  (newest first, %d entries buffered)" RESET EL, s_log_count);

    int visible = rows - 2;
    if (visible > s_log_count) visible = s_log_count;
    for (int i = 0; i < visible; i++) {
        int idx = (s_log_head - 1 - i + TUI_LOG_LINES) % TUI_LOG_LINES;
        tui_log_entry_t *e = &s_log[idx];
        tui_goto(top_row + 2 + i, 3);
        printf("%s%s" RESET EL, LOG_COL[e->color], e->text);
    }
}

/* -- page: settings -------------------------------------------------- */
typedef enum {
    S_FREQ = 0,
    S_GAIN,
    S_FAV_LIST,
    S_APP_SELECT,
    S_VOICE_PRESET,     /* cycle SAM presets: DEFAULT/ELVIS/DEEP/SOFT/STUFFY */
    S_VOICE_LOWPASS,    /* cycle LP mode:    OFF/SOFT/FIRM                   */
    S_VOICE_SHELF,      /* cycle shelf mode: OFF/WARM/WARMER                 */
    S_VOICE_TEST,       /* ENTER: play canned test phrase                    */
    S_COUNT
} settings_item_t;

/* Settings page state. */
static int             s_sel = 0;
static int             s_fav_sel = 0;       /* which fav slot selected     */
static bool            s_edit_freq = false;
static char            s_edit_buf[16];
static int             s_edit_len = 0;

/* Public probe: true when the settings page is mid-edit (digits should
 * route to the edit buffer, not to page-switch shortcuts). The TUI key
 * dispatcher checks this BEFORE its '1' '2' '3' '4' page shortcut so a
 * user typing a frequency can include digits 1-4 in the value. */
bool settings_is_editing(void)
{
    return s_edit_freq;
}

static void settings_enter_edit(void)
{
    const app_t *a = app_current();
    if (!a) return;
    uint32_t hz = settings_get_freq(a);
    snprintf(s_edit_buf, sizeof(s_edit_buf), "%lu", (unsigned long)hz);
    s_edit_len = (int)strlen(s_edit_buf);
    s_edit_freq = true;
}
static void settings_commit_edit(void)
{
    const app_t *a = app_current();
    if (!a) { s_edit_freq = false; return; }
    uint32_t hz = (uint32_t)strtoul(s_edit_buf, NULL, 10);
    if (hz >= 1000000 && hz <= 2000000000) {
        settings_set_freq(a, hz);
        tui_log(1, "SETTING   freq set to %lu Hz", (unsigned long)hz);

        /* Apply the new freq to the live radio so the user doesn't
         * have to switch apps to make it take effect. settings_set_freq
         * above persists to NVS; rtlsdr_dev_set_freq retunes the running
         * RTL-SDR. The active app's RX task will keep running on the
         * new freq immediately; for P25, the on-screen freq display
         * pulls from settings_get_freq() so it'll catch up on the next
         * frame. */
        extern uint32_t rtlsdr_dev_set_freq(uint32_t hz);
        rtlsdr_dev_set_freq(hz);
    } else {
        tui_log(4, "SETTING   %lu Hz out of range, ignored",
                (unsigned long)hz);
    }
    s_edit_freq = false;
}

/* The settings page handles its own key presses - called from the main
 * key dispatcher in app_main's UART task. */
bool settings_handle_key(tui_key_t k)
{
    if (page_current() != PAGE_SETTINGS) return false;
    const app_t *a = app_current();
    if (!a) return false;

    if (s_edit_freq) {
        if (k >= '0' && k <= '9') {
            if (s_edit_len < (int)sizeof(s_edit_buf) - 1) {
                s_edit_buf[s_edit_len++] = (char)k;
                s_edit_buf[s_edit_len]   = 0;
            }
        } else if (k == TK_BKSP) {
            if (s_edit_len > 0) s_edit_buf[--s_edit_len] = 0;
        } else if (k == TK_ENTER) {
            settings_commit_edit();
        } else if (k == TK_ESC) {
            s_edit_freq = false;
        }
        tui_mark_dirty();
        return true;
    }

    switch (k) {
    case TK_UP:   if (s_sel > 0) s_sel--; break;
    case TK_DOWN: if (s_sel < S_COUNT - 1) s_sel++; break;
    case TK_LEFT:
        if (s_sel == S_GAIN) {
            int g = settings_get_gain(a) - 10;
            if (g < 0) g = 0;
            settings_set_gain(a, g);
            extern int rtlsdr_dev_set_gain(int tenths_db);
            rtlsdr_dev_set_gain(g);
            tui_log(1, "SETTING   gain %d.%d dB", g / 10, g % 10);
        } else if (s_sel == S_FAV_LIST) {
            if (s_fav_sel > 0) s_fav_sel--;
        } else if (s_sel == S_APP_SELECT) {
            /* previous app */
            int idx = app_current_index() - 1;
            if (idx < 0) idx = 0;
            app_switch_to(idx);
        } else if (s_sel == S_VOICE_PRESET) {
            /* Cycle backwards through SAM voice presets. Wraps at 0. */
            int p = (int)sam_tts_get_preset() - 1;
            if (p < 0) p = SAM_PRESET_COUNT - 1;
            sam_tts_set_preset((sam_tts_voice_preset_t)p);
            settings_voice_preset_set(p);
            tui_log(1, "VOICE     preset %s", sam_tts_preset_name(p));
        } else if (s_sel == S_VOICE_LOWPASS) {
            /* 3-state cycle: OFF <- SOFT <- FIRM */
            int m = sam_tts_get_lowpass() - 1;
            if (m < 0) m = 2;
            sam_tts_set_lowpass(m);
            settings_voice_lowpass_set(m);
            tui_log(1, "VOICE     lowpass %s", sam_tts_lowpass_name(m));
        } else if (s_sel == S_VOICE_SHELF) {
            int m = sam_tts_get_lowshelf() - 1;
            if (m < 0) m = 2;
            sam_tts_set_lowshelf(m);
            settings_voice_lowshelf_set(m);
            tui_log(1, "VOICE     shelf %s", sam_tts_lowshelf_name(m));
        }
        break;
    case TK_RIGHT:
        if (s_sel == S_GAIN) {
            int g = settings_get_gain(a) + 10;
            if (g > 600) g = 600;
            settings_set_gain(a, g);
            extern int rtlsdr_dev_set_gain(int tenths_db);
            rtlsdr_dev_set_gain(g);
            tui_log(1, "SETTING   gain %d.%d dB", g / 10, g % 10);
        } else if (s_sel == S_FAV_LIST) {
            if (s_fav_sel < MAX_FAVOURITES - 1) s_fav_sel++;
        } else if (s_sel == S_APP_SELECT) {
            app_cycle_next();
        } else if (s_sel == S_VOICE_PRESET) {
            int p = ((int)sam_tts_get_preset() + 1) % SAM_PRESET_COUNT;
            sam_tts_set_preset((sam_tts_voice_preset_t)p);
            settings_voice_preset_set(p);
            tui_log(1, "VOICE     preset %s", sam_tts_preset_name(p));
        } else if (s_sel == S_VOICE_LOWPASS) {
            int m = (sam_tts_get_lowpass() + 1) % 3;
            sam_tts_set_lowpass(m);
            settings_voice_lowpass_set(m);
            tui_log(1, "VOICE     lowpass %s", sam_tts_lowpass_name(m));
        } else if (s_sel == S_VOICE_SHELF) {
            int m = (sam_tts_get_lowshelf() + 1) % 3;
            sam_tts_set_lowshelf(m);
            settings_voice_lowshelf_set(m);
            tui_log(1, "VOICE     shelf %s", sam_tts_lowshelf_name(m));
        }
        break;
    case TK_ENTER:
        if (s_sel == S_FREQ) {
            settings_enter_edit();
        } else if (s_sel == S_FAV_LIST) {
            /* ENTER on a favourite: tune to it, or if empty, save current */
            uint32_t cur = settings_fav_get(a, s_fav_sel);
            if (cur == 0) {
                settings_fav_set(a, s_fav_sel, settings_get_freq(a));
                tui_log(1, "SETTING   saved fav[%d] = %lu Hz",
                        s_fav_sel, (unsigned long)settings_get_freq(a));
            } else {
                settings_set_freq(a, cur);
                tui_log(1, "SETTING   tuned to fav[%d] = %lu Hz",
                        s_fav_sel, (unsigned long)cur);
            }
        } else if (s_sel == S_VOICE_TEST) {
            /* Fire the test phrase using current preset + filters. */
            sam_tts_test_speak();
            tui_log(1, "VOICE     test phrase");
        }
        break;
    default: return false;
    }
    tui_mark_dirty();
    return true;
}

static void draw_page_settings(int top_row, int rows, int cols)
{
    (void)cols;
    const app_t *a = app_current();
    if (!a) return;

    /* Pre-clear the body region. Without this, leftover rows from the
     * previous page leak through any row this page doesn't write to. */
    for (int rr = top_row; rr < top_row + rows; rr++) {
        tui_goto(rr, 3);
        fputs(EL, stdout);
    }

    tui_goto(top_row, 3);
    printf(C_LABEL "SETTINGS " RESET C_GOLD BOLD "%s" RESET C_DIM
           "  (arrows nav, left/right change, enter edit/save)" RESET EL,
           a->name);

    int r = top_row + 2;

    /* Frequency */
    tui_goto(r++, 3);
    if (s_sel == S_FREQ) printf(C_BG_ACT);
    printf(C_LABEL " %-14s " RESET, "FREQUENCY");
    if (s_edit_freq) {
        printf(C_AMBER "[EDIT] " C_BRIGHT "%s" C_AMBER "_" RESET
               C_DIM "  Hz   ([0-9] type, [Bksp] del, [Enter] save, [Esc] cancel)" RESET,
               s_edit_buf);
    } else {
        char hz[24]; fmt_hz(hz, sizeof(hz), settings_get_freq(a));
        printf(C_CYAN "%s" RESET C_DIM "   ([Enter] to edit)" RESET, hz);
    }
    printf(EL);

    /* Gain */
    tui_goto(r++, 3);
    if (s_sel == S_GAIN) printf(C_BG_ACT);
    int g = settings_get_gain(a);
    printf(C_LABEL " %-14s " RESET C_GREEN "%d.%d dB" RESET
           C_DIM "  (0 = auto)" RESET EL, "TUNER GAIN", g / 10, g % 10);

    /* Favourites */
    tui_goto(r++, 3);
    if (s_sel == S_FAV_LIST) printf(C_BG_ACT);
    printf(C_LABEL " %-14s " RESET, "FAVOURITES");
    printf(C_DIM "[enter: save current / tune to saved]" RESET EL);
    for (int i = 0; i < MAX_FAVOURITES; i++) {
        tui_goto(r++, 5);
        uint32_t fh = settings_fav_get(a, i);
        bool is_sel = (s_sel == S_FAV_LIST && s_fav_sel == i);
        if (is_sel) printf(C_BG_ACT);
        printf(" %d: ", i);
        if (fh == 0) {
            printf(C_DIM "(empty)" RESET);
        } else {
            char hz[24]; fmt_hz(hz, sizeof(hz), fh);
            printf(C_CYAN "%s" RESET, hz);
        }
        printf(EL);
    }

    /* App selector */
    tui_goto(r++, 3);
    if (s_sel == S_APP_SELECT) printf(C_BG_ACT);
    printf(C_LABEL " %-14s " RESET C_GOLD "%s" RESET
           C_DIM "   (left/right to change)" RESET EL,
           "ACTIVE APP", a->name);

    /* Voice rows Ã¢â‚¬â€ preset, lowpass, low shelf, test.
     * Kept compact; each fits on one line with a dim hint. */
    tui_goto(r++, 3);
    if (s_sel == S_VOICE_PRESET) printf(C_BG_ACT);
    printf(C_LABEL " %-14s " RESET C_GOLD "%s" RESET
           C_DIM "   (left/right to cycle)" RESET EL,
           "VOICE PRESET", sam_tts_preset_name(sam_tts_get_preset()));

    tui_goto(r++, 3);
    if (s_sel == S_VOICE_LOWPASS) printf(C_BG_ACT);
    printf(C_LABEL " %-14s " RESET C_GREEN "%s" RESET
           C_DIM "   (smooths 4-bit hash)" RESET EL,
           "LOWPASS", sam_tts_lowpass_name(sam_tts_get_lowpass()));

    tui_goto(r++, 3);
    if (s_sel == S_VOICE_SHELF) printf(C_BG_ACT);
    printf(C_LABEL " %-14s " RESET C_GREEN "%s" RESET
           C_DIM "   (adds low-end body)" RESET EL,
           "LOW SHELF", sam_tts_lowshelf_name(sam_tts_get_lowshelf()));

    tui_goto(r++, 3);
    if (s_sel == S_VOICE_TEST) printf(C_BG_ACT);
    printf(C_LABEL " %-14s " RESET C_AMBER "[ENTER to speak]" RESET EL,
           "TEST VOICE");
}

/* -- side-border painter --------------------------------------------- */
static void paint_side_borders(int first_row, int last_row)
{
    for (int r = first_row; r <= last_row; r++) {
        tui_goto(r, 1);
        printf(C_BORDER VL RESET);
        tui_goto(r, TUI_COLS);
        printf(C_BORDER VL RESET);
    }
}

/* -- footer ---------------------------------------------------------- */
static void draw_footer(void)
{
    int row = TUI_ROWS;
    tui_goto(row, 1);
    printf(C_BORDER BL);
    for (int i = 0; i < TUI_COLS - 2; i++) printf(HL);
    printf(BR EL RESET);

    /* Help line just above. */
    tui_goto(row - 1, 1);
    printf(C_BORDER VL RESET);
    printf(C_DIM " [1-5] page  [Tab] cycle  [A] app" RESET
           C_DIM "  |  [M]ute [+/-] vol  [V]oice [C][L][P][B]" RESET
           C_DIM "  |  [ctrl-]] exit" RESET EL);
    tui_goto(row - 1, TUI_COLS);
    printf(C_BORDER VL RESET);
}

/* -- main draw ------------------------------------------------------- */
void tui_init(void)
{
    s_boot_us = esp_timer_get_time();
    /* Scrub EVERYTHING: full clear + clear scrollback + reset all attrs
     * + reset scroll region + home cursor + hide cursor. This wipes any
     * pre-TUI boot logs (USBH enumeration spam, ES8311 init, etc.) that
     * would otherwise linger below our border if the user's terminal
     * window is taller than TUI_ROWS. \033[3J clears the scrollback
     * buffer itself so nothing stays visible by scrolling back.
     *
     * The cursor-hide (\033[?25l) is also re-emitted in tui_draw() each
     * frame. Some terminals (PuTTY notably) un-hide the cursor when
     * they see certain sequences like scroll-region resets or after a
     * burst of SGR colour changes, so a one-shot hide at init is not
     * enough Ã¢â‚¬â€ the cursor comes back and ends up tracing the draw path
     * across the screen, which looks like flicker. Re-asserting hide
     * every frame pins it down reliably.
     *
     * Previously we minimised per-frame escape traffic because the
     * ISR-level USB-host `esp_rom_printf` could interleave with our
     * output and corrupt mid-escape. That's no longer a concern now
     * that the ESP-IDF console has been moved off UART0 entirely (see
     * sdkconfig: CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y). UART0 is ours
     * alone, so we can afford the extra defensive escapes. */
    printf("\033[0m"        /* reset all attributes          */
           "\033[2J"        /* clear visible screen          */
           "\033[3J"        /* clear scrollback buffer       */
           "\033[r"         /* reset scroll region           */
           "\033[H"         /* home cursor                   */
           "\033[?7l"       /* disable auto-wrap at margin    */
           "\033[?25l");    /* hide cursor (DECTCEM)         */
    fflush(stdout);
    s_dirty = true;
}

void tui_draw(void)
{
    int64_t now = esp_timer_get_time();
    if (!s_dirty && (now - s_last_draw) < (TUI_REFRESH_MS * 1000LL)) return;
    s_last_draw = now;
    s_dirty     = false;

    /* Re-assert cursor hide at start of every frame, then home. See
     * tui_init() for the rationale Ã¢â‚¬â€ some terminals un-hide on various
     * escape sequences, so we need to keep saying "stay hidden".
     *
     * No \033[2J full-clear here Ã¢â‚¬â€ that would flicker every frame
     * because every paint goes blank before redrawing. Instead, every
     * line we draw ends with EL (\033[K, erase-to-end-of-line) and we
     * explicitly clear rows below TUI_ROWS after draw_footer(). */
    printf("\033[?25l\033[H");
    draw_header();
    draw_page_tabs();

    /* Body region is rows 6 .. TUI_ROWS-2 (last content row). */
    int body_top = 6;
    int body_bot = TUI_ROWS - 2;
    int body_rows = body_bot - body_top + 1;

    /* Clear body + paint sides. Clearing happens inside each draw_*. */
    switch (page_current()) {
    case PAGE_MAIN:      draw_page_main(body_top, body_rows, TUI_COLS - 4); break;
    case PAGE_SIGNAL: {
        /* Each app can supply its own signal-page renderer. ADS-B leaves
         * draw_signal NULL and gets the framework's Mode-S analyzer
         * (which is what its old "WATERFALL" tab did). P25 supplies
         * p25_draw_signal which renders a P25-relevant view. */
        const app_t *a = app_current();
        if (a && a->draw_signal) {
            a->draw_signal(body_top, body_rows, TUI_COLS - 4);
        } else {
            draw_page_waterfall(body_top, body_rows, TUI_COLS - 4);
        }
        break;
    }
    case PAGE_LOG:       draw_page_log(body_top, body_rows, TUI_COLS - 4); break;
    case PAGE_SETTINGS:  draw_page_settings(body_top, body_rows, TUI_COLS - 4); break;
    case PAGE_DIAG: {
        /* Per-app diagnostic view. Apps that don't provide one get a
         * placeholder rather than nothing — easier to understand than
         * a blank page when switching apps. */
        const app_t *a = app_current();
        if (a && a->draw_diag) {
            a->draw_diag(body_top, body_rows, TUI_COLS - 4);
        } else {
            for (int r = body_top; r < body_top + body_rows; r++) {
                tui_goto(r, 3); fputs(EL, stdout);
            }
            tui_goto(body_top + body_rows / 2, (TUI_COLS - 30) / 2);
            printf(C_DIM "(no diagnostics for this app)" RESET);
        }
        break;
    }
    default: break;
    }

    paint_side_borders(6, TUI_ROWS - 2);
    draw_footer();

    /* Clear any rows that might exist below our TUI area Ã¢â‚¬â€ common when
     * the user's terminal window is taller than TUI_ROWS. Without this,
     * boot-time log text or earlier-frame footer ghosting below the
     * border stays visible and the screen looks broken. \033[J erases
     * from cursor to end of screen. */
    tui_goto(TUI_ROWS + 1, 1);
    printf("\033[J");

    /* Park cursor on top of the bottom-right border character and
     * re-assert hide one last time. Two layers of defence:
     *   1. If the terminal honours \033[?25l, the cursor is invisible.
     *   2. If it doesn't (some PuTTY builds, some Windows terminals),
     *      the cursor sits stationary on top of the 'Ã¢â€â€š' border glyph
     *      where it's visually indistinguishable from the border.
     * Either way, the "cursor flying across the screen" effect is gone
     * because the cursor ends every frame in the same spot. */
    /* Auto-wrap suppression: \033[?7l tells the terminal not to wrap
     * the cursor to the next line when output (or local-echo of typed
     * characters) would push past the right margin. Without this,
     * keystrokes typed between renders get echoed at the parked
     * position (TUI_ROWS, TUI_COLS) and then the terminal wraps them
     * onto the next line at column 1 - which is exactly the "leak
     * character on the inner left border" that has been visible since
     * the keymap was added. Re-asserted every frame in case some
     * other code path changed it. */
    tui_goto(TUI_ROWS, TUI_COLS);
    printf("\033[?7l\033[?25l");

    fflush(stdout);
}
