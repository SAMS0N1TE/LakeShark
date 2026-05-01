/* adsb_render.c - TUI for the ADS-B app.
 *
 * Three pages:
 *   PAGE_MAIN  -> aircraft table with arrow-key selection cursor.
 *   PAGE_SIGNAL -> per-aircraft detail (TRACK page). Always shows the
 *                  currently-selected aircraft from the main table.
 *                  Up/Down switches contacts here too.
 *   PAGE_DIAG  -> RF & decoder diagnostics. Real-time level meter,
 *                  60s sparklines for bursts/CRC/magnitude, last-event
 *                  timestamps, USB byte-rate health.
 *
 * Compiled only when CONFIG_ENABLE_TUI is set.
 */

#include "tui.h"
#include "app_registry.h"
#include "adsb_state.h"
#include "perf.h"
#include "settings.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#ifndef CONFIG_ADSB_RX_LAT_UDEG
#define CONFIG_ADSB_RX_LAT_UDEG 0
#endif
#ifndef CONFIG_ADSB_RX_LON_UDEG
#define CONFIG_ADSB_RX_LON_UDEG 0
#endif

void adsb_on_enter_tui(void)
{
    tui_waterfall_reset();
}

/* ───────────────────────────────────────────────────────────────────
 * Local TUI helpers (mirror the patterns P25 uses; small enough to
 * duplicate rather than promote to a shared header for now).
 * ─────────────────────────────────────────────────────────────────── */

static void draw_rule(int row, int col, const char *label, int width)
{
    tui_goto(row, col);
    fputs(C_BORDER, stdout);
    fputs(HL HL " ", stdout);
    if (label) printf(C_LABEL "%s " RESET C_BORDER, label);
    int used = (label ? (int)strlen(label) + 4 : 3);
    for (int i = used; i < width; i++) fputs(HL, stdout);
    fputs(RESET, stdout);
    fputs(EL, stdout);
}

/* Format an elapsed microsecond span as a compact string.
 * Outputs "<1s", "12s", "3m42s", or "1h05m" depending on magnitude. */
static void fmt_elapsed(char *out, size_t n, int64_t us)
{
    if (us < 0) us = 0;
    /* Cap to int range — we only care about magnitudes well under
     * a year and using int silences GCC 14's format-truncation
     * analysis on the snprintfs below. */
    int64_t s64 = us / 1000000LL;
    if (s64 > 0x7FFFFFFFLL) s64 = 0x7FFFFFFFLL;
    int s = (int)s64;
    if      (s < 1)    snprintf(out, n, "<1s");
    else if (s < 60)   snprintf(out, n, "%ds", s);
    else if (s < 3600) snprintf(out, n, "%dm%02ds", s / 60, s % 60);
    else               snprintf(out, n, "%dh%02dm", s / 3600, (s / 60) % 60);
}

/* Compass octant for a bearing in degrees [0..360). */
static const char *bearing_octant(double deg)
{
    while (deg <  0)   deg += 360;
    while (deg >= 360) deg -= 360;
    static const char *oct[8] = {"N","NE","E","SE","S","SW","W","NW"};
    int idx = ((int)((deg + 22.5) / 45.0)) % 8;
    return oct[idx];
}

/* Great-circle distance in nautical miles between two lat/lon pairs. */
static double haversine_nm(double lat1, double lon1, double lat2, double lon2)
{
    const double R_NM = 3440.065;
    double t1 = lat1 * M_PI / 180.0;
    double t2 = lat2 * M_PI / 180.0;
    double dt = (lat2 - lat1) * M_PI / 180.0;
    double dl = (lon2 - lon1) * M_PI / 180.0;
    double a  = sin(dt / 2) * sin(dt / 2) +
                cos(t1) * cos(t2) * sin(dl / 2) * sin(dl / 2);
    return 2 * R_NM * atan2(sqrt(a), sqrt(1 - a));
}

/* Initial bearing from (lat1,lon1) to (lat2,lon2) in degrees [0,360). */
static double bearing_deg(double lat1, double lon1, double lat2, double lon2)
{
    double t1 = lat1 * M_PI / 180.0;
    double t2 = lat2 * M_PI / 180.0;
    double dl = (lon2 - lon1) * M_PI / 180.0;
    double y  = sin(dl) * cos(t2);
    double x  = cos(t1) * sin(t2) - sin(t1) * cos(t2) * cos(dl);
    double brg = atan2(y, x) * 180.0 / M_PI;
    if (brg < 0) brg += 360;
    return brg;
}

/* ───────────────────────────────────────────────────────────────────
 * PAGE_MAIN: aircraft table with selection chevron.
 * ─────────────────────────────────────────────────────────────────── */

void adsb_draw_main(int top, int rows, int cols)
{
    tui_goto(top, 3);
    printf(C_LABEL "AIRCRAFT " RESET C_BRIGHT "%d" RESET
           C_DIM "   " RESET
           C_LABEL "MSGS " RESET C_BRIGHT "%d" RESET
           C_DIM " (%d/s)   " RESET
           C_LABEL "CRC " RESET C_GREEN "%d" RESET C_DIM "/" RESET C_RED "%d" RESET EL,
           perf_get_active_count(), perf_get_msgs_total(), perf_get_msgs_per_sec(),
           perf_get_crc_good(), perf_get_crc_err());

    /* Column header: leading space accounts for the selection chevron
     * column (one char + space) so the data rows align correctly. */
    tui_goto(top + 2, 3);
    printf(C_LABEL "  %-8s %-9s %8s %6s %5s %9s %9s %6s %5s" RESET EL,
           "ICAO", "CALLSIGN", "ALT ft", "KT", "HDG",
           "LAT", "LON", "V/S", "MSGS");

    tui_hline(top + 3, 3, cols - 2);

    for (int r = top + 4; r < top + rows - 1; r++) {
        tui_goto(r, 3);
        fputs(EL, stdout);
    }

    int body_row = top + 4;
    int body_max = top + rows - 1;
    int shown    = 0;
    uint32_t selected = adsb_select_get_icao();

    for (int i = 0; i < ADSB_MAX_TRACKED && body_row < body_max; i++) {
        const adsb_aircraft_t *a = adsb_state_get(i);
        if (!a || !a->active) continue;

        const char *alt_col = C_TEXT;
        const char *vs_col  = C_DIM;
        char vs_buf[12] = "  --";
        if (a->vert_rate > 200) {
            alt_col = C_GREEN;
            vs_col  = C_GREEN;
            snprintf(vs_buf, sizeof(vs_buf), "+%d", a->vert_rate);
        } else if (a->vert_rate < -200) {
            alt_col = C_AMBER;
            vs_col  = C_AMBER;
            snprintf(vs_buf, sizeof(vs_buf), "%d", a->vert_rate);
        }

        char lat_s[12] = "   ------";
        char lon_s[12] = "   ------";
        if (a->pos_valid) {
            snprintf(lat_s, sizeof(lat_s), "%+9.4f", a->lat);
            snprintf(lon_s, sizeof(lon_s), "%+9.4f", a->lon);
        }

        const char *cs = a->callsign[0] ? a->callsign : "--------";
        bool is_sel = (selected != 0 && a->icao == selected);
        const char *chev = is_sel ? "\xe2\x96\xb6" : " ";  /* ▶ or space */
        const char *chev_col = is_sel ? C_BRIGHT : C_DIM;

        tui_goto(body_row, 3);
        printf("%s%s" RESET " "
               C_CYAN  "%-8lX" RESET " "
               C_GOLD  "%-9s" RESET " "
               "%s%8d" RESET " "
               C_TEXT  "%6d" RESET " "
               C_MAGENTA "%5d" RESET " "
               C_DIM   "%9s" RESET " "
               C_DIM   "%9s" RESET " "
               "%s%6s" RESET " "
               C_DIM   "%5d" RESET EL,
               chev_col, chev,
               (unsigned long)a->icao,
               cs, alt_col, a->altitude,
               a->velocity, a->heading,
               lat_s, lon_s,
               vs_col, vs_buf, a->msg_count);
        body_row++;
        shown++;
    }

    while (body_row < body_max) {
        tui_goto(body_row, 3);
        printf(C_DIM "  \xc2\xb7" RESET EL);
        body_row++;
    }

    if (shown == 0) {
        tui_goto(top + rows / 2, (cols - 30) / 2 + 3);
        printf(C_DIM "(no aircraft in range)" RESET EL);
    } else {
        /* Hint line showing the selection controls — only when there's
         * something to select. Lives on the last body row. */
        tui_goto(body_max, 3);
        printf(C_DIM "[\xe2\x86\x91/\xe2\x86\x93] select  [Enter] detail" RESET EL);
    }
}

/* ───────────────────────────────────────────────────────────────────
 * PAGE_SIGNAL (TRACK): per-aircraft detail.
 * ─────────────────────────────────────────────────────────────────── */

static void draw_no_selection(int top, int rows, int cols)
{
    /* Pre-clear the body so leftover content from the main page
     * doesn't show through gap rows. */
    for (int r = top; r < top + rows; r++) {
        tui_goto(r, 3); fputs(EL, stdout);
    }
    int msg_row = top + rows / 2;
    const char *msg = "No aircraft selected.";
    const char *hint = "Return to MAIN (1) and use \xe2\x86\x91/\xe2\x86\x93 to pick a contact.";
    tui_goto(msg_row, (cols - (int)strlen(msg)) / 2 + 3);
    printf(C_DIM "%s" RESET, msg);
    tui_goto(msg_row + 1, (cols - 50) / 2 + 3);
    printf(C_DIM "%s" RESET, hint);
}

/* Render a single sparkline row using 8-level block glyphs. The data
 * window is alt_history[] in decode order (head points at the next
 * write slot, so the oldest sample is at head and the newest is at
 * head-1 mod N). Empty slots (-1) render as a dim baseline tick. */
static void draw_alt_sparkline(int row, int col, int width,
                               const int16_t *hist, int hist_len, int hist_head)
{
    static const char *BARS[9] = {
        " ",
        "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
    };

    /* First pass: find min/max of valid samples to scale. */
    int vmin = INT_MAX, vmax = INT_MIN, n_valid = 0;
    for (int i = 0; i < hist_len; i++) {
        int v = hist[i];
        if (v < 0) continue;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
        n_valid++;
    }

    tui_goto(row, col);
    if (n_valid == 0) {
        fputs(C_DIM, stdout);
        for (int i = 0; i < width; i++) fputs(BARS[1], stdout);
        fputs(RESET, stdout);
        return;
    }
    int span = vmax - vmin;
    if (span < 1) span = 1;

    /* Walk samples oldest-to-newest, mapping each to one or more chars
     * if the sparkline is wider than the history. */
    int slots = hist_len < width ? hist_len : width;
    int extra = width - slots;        /* dim padding on the left */

    fputs(C_DIM, stdout);
    for (int i = 0; i < extra; i++) fputc(' ', stdout);

    for (int s = 0; s < slots; s++) {
        int idx = (hist_head + (hist_len - slots) + s) % hist_len;
        int v = hist[idx];
        if (v < 0) {
            fputs(C_DIM, stdout);
            fputs(BARS[1], stdout);
        } else {
            int level = ((v - vmin) * 8) / span;
            if (level < 0) level = 0;
            if (level > 8) level = 8;
            fputs(C_GREEN, stdout);
            fputs(BARS[level], stdout);
        }
    }
    fputs(RESET, stdout);
}

void adsb_draw_signal(int top, int rows, int cols)
{
    const adsb_aircraft_t *a = adsb_select_get();
    if (!a) {
        draw_no_selection(top, rows, cols);
        return;
    }

    int width = cols - 2;
    if (width < 60)  width = 60;
    if (width > 124) width = 124;

    /* Pre-clear body. */
    for (int r = top; r < top + rows; r++) {
        tui_goto(r, 3); fputs(EL, stdout);
    }

    int sel_idx = -1, sel_total = 0;
    adsb_select_index(&sel_idx, &sel_total);

    int64_t now = esp_timer_get_time();
    char first_seen[16], last_seen[16];
    fmt_elapsed(first_seen, sizeof(first_seen), now - a->first_seen_us);
    fmt_elapsed(last_seen,  sizeof(last_seen),  now - a->last_seen_us);

    /* ── Header ───────────────────────────────────────────────── */
    tui_goto(top, 3);
    const char *cs = a->callsign[0] ? a->callsign : "--------";
    printf(C_LABEL "TRACK  " RESET
           C_LABEL "ICAO " RESET C_CYAN BOLD "%06lX" RESET
           "   " C_LABEL "[ " RESET C_GOLD BOLD "%s" RESET C_LABEL " ] " RESET
           "   " C_LABEL "seen " RESET C_TEXT "%s" RESET
           "   " C_LABEL "msgs " RESET C_BRIGHT "%d" RESET,
           (unsigned long)a->icao, cs, first_seen, a->msg_count);
    if (sel_total > 1) {
        printf("   " C_DIM "\xe2\x97\x80 %d/%d \xe2\x96\xb6" RESET,
               sel_idx + 1, sel_total);
    }
    printf(EL);

    /* ── Two-column body: POSITION / MOTION on left+right ──────── */
    int row = top + 2;
    int colL = 3;
    int colR = (cols / 2) + 2;

    draw_rule(row, colL, "POSITION", (cols / 2) - 4);
    draw_rule(row, colR, "MOTION",   (cols - cols/2) - 4);
    row++;

    /* POSITION column: lat/lon, distance/bearing from RX, RX hint. */
    tui_goto(row, colL + 2);
    if (a->pos_valid) {
        printf(C_LABEL "LAT  " RESET "%+10.5f", a->lat);
    } else {
        printf(C_LABEL "LAT  " RESET C_DIM "    ------" RESET);
    }

    /* MOTION column row 1: ALT + V/S */
    tui_goto(row, colR + 2);
    printf(C_LABEL "ALT     " RESET);
    if (a->altitude != 0) {
        const char *alt_col = a->vert_rate >  200 ? C_GREEN :
                              a->vert_rate < -200 ? C_AMBER : C_TEXT;
        printf("%s%6d ft" RESET, alt_col, a->altitude);
        if      (a->vert_rate >  200) printf(C_GREEN " \xe2\x96\xb2 +%d fpm" RESET, a->vert_rate);
        else if (a->vert_rate < -200) printf(C_AMBER " \xe2\x96\xbc %d fpm"  RESET, a->vert_rate);
        else                          printf(C_DIM   " level"                RESET);
    } else {
        printf(C_DIM "    -- ft" RESET);
    }
    row++;

    tui_goto(row, colL + 2);
    if (a->pos_valid) {
        printf(C_LABEL "LON  " RESET "%+10.5f", a->lon);
    } else {
        printf(C_LABEL "LON  " RESET C_DIM "    ------" RESET);
    }
    tui_goto(row, colR + 2);
    printf(C_LABEL "SPEED   " RESET);
    if (a->velocity > 0) printf(C_TEXT "%6d kt" RESET, a->velocity);
    else                  printf(C_DIM   "    -- kt" RESET);
    row++;

    /* Distance/bearing line (left col) — only when both aircraft pos
     * and receiver location are known. */
    tui_goto(row, colL + 2);
    bool have_rx = (CONFIG_ADSB_RX_LAT_UDEG != 0 || CONFIG_ADSB_RX_LON_UDEG != 0);
    if (a->pos_valid && have_rx) {
        double rx_lat = CONFIG_ADSB_RX_LAT_UDEG / 1000000.0;
        double rx_lon = CONFIG_ADSB_RX_LON_UDEG / 1000000.0;
        double d  = haversine_nm(rx_lat, rx_lon, a->lat, a->lon);
        double br = bearing_deg(rx_lat, rx_lon, a->lat, a->lon);
        printf(C_LABEL "DIST " RESET "%6.1f NM " C_DIM "@" RESET " %3d\xc2\xb0 %s",
               d, (int)br, bearing_octant(br));
    } else if (a->pos_valid) {
        printf(C_DIM "DIST  (set ADSB_RX_LAT/LON to enable)" RESET);
    } else {
        printf(C_DIM "DIST  (waiting on position)" RESET);
    }
    tui_goto(row, colR + 2);
    printf(C_LABEL "HEADING " RESET);
    if (a->heading >= 0 && a->heading <= 359 &&
        (a->heading != 0 || a->ew_velocity != 0 || a->ns_velocity != 0)) {
        printf(C_MAGENTA "%6d\xc2\xb0" RESET " %s",
               a->heading, bearing_octant(a->heading));
    } else {
        printf(C_DIM "    --" RESET);
    }
    row++;

    /* E/W and N/S velocity components (right column only). */
    tui_goto(row, colR + 2);
    printf(C_LABEL "E/W " RESET);
    if (a->ew_velocity != 0) printf("%+5d kt", a->ew_velocity);
    else                      printf(C_DIM "   -- kt" RESET);
    printf("  " C_LABEL "N/S " RESET);
    if (a->ns_velocity != 0) printf("%+5d kt", a->ns_velocity);
    else                      printf(C_DIM "   -- kt" RESET);
    row++;

    /* ── QUALITY / MSG-TYPES section ──────────────────────────── */
    row++;
    draw_rule(row, colL, "QUALITY",   (cols / 2) - 4);
    draw_rule(row, colR, "MSG TYPES", (cols - cols/2) - 4);
    row++;

    /* QUALITY column. */
    tui_goto(row, colL + 2);
    printf(C_LABEL "FIRST SEEN " RESET C_TEXT "%s" RESET, first_seen);
    /* MSG TYPES column. */
    tui_goto(row, colR + 2);
    printf(C_LABEL "DF11  ALL-CALL          " RESET C_TEXT "%4u" RESET, a->mt_df11);
    row++;

    tui_goto(row, colL + 2);
    printf(C_LABEL "LAST SEEN  " RESET C_TEXT "%s" RESET, last_seen);
    tui_goto(row, colR + 2);
    printf(C_LABEL "DF17  POSITION (ME 9-18) " RESET C_TEXT "%4u" RESET, a->mt_df17_pos);
    row++;

    tui_goto(row, colL + 2);
    printf(C_LABEL "CRC GOOD   " RESET C_GREEN "%4d" RESET, a->good_msg_count);
    tui_goto(row, colR + 2);
    printf(C_LABEL "DF17  VELOCITY (ME 19-22)" RESET C_TEXT "%4u" RESET, a->mt_df17_vel);
    row++;

    tui_goto(row, colL + 2);
    const char *st_col = a->announced ? C_GREEN : C_AMBER;
    const char *st     = a->announced ? "CONFIRMED" : "GATING...";
    printf(C_LABEL "STATUS     " RESET "%s%s" RESET, st_col, st);
    tui_goto(row, colR + 2);
    printf(C_LABEL "DF17  IDENT    (ME 1-4)  " RESET C_TEXT "%4u" RESET, a->mt_df17_id);
    row++;

    tui_goto(row, colR + 2);
    printf(C_LABEL "DF4/5/20/21  SURVEILLANCE " RESET C_TEXT "%4u" RESET, a->mt_surv);
    row++;

    tui_goto(row, colR + 2);
    printf(C_LABEL "OTHER                    " RESET C_DIM "%4u" RESET, a->mt_other);
    row += 2;

    /* ── Altitude sparkline ───────────────────────────────────── */
    if (row + 2 < top + rows - 1) {
        draw_rule(row, colL, "ALTITUDE  recent commits", width - 4);
        row++;
        int spark_w = width - 8;
        if (spark_w > 64) spark_w = 64;
        int hist_len = sizeof(a->alt_history) / sizeof(a->alt_history[0]);
        draw_alt_sparkline(row, colL + 2, spark_w,
                           a->alt_history, hist_len, a->alt_history_head);
        printf("  " C_BRIGHT "%d ft" RESET, a->altitude);
        row++;
    }

    /* ── Footer hint ─────────────────────────────────────────── */
    int foot = top + rows - 1;
    tui_goto(foot, 3);
    printf(C_DIM "[\xe2\x86\x91/\xe2\x86\x93] prev/next contact   [1] back to MAIN   [t] inject test track" RESET EL);
}

/* ───────────────────────────────────────────────────────────────────
 * PAGE_DIAG: RF and decoder diagnostics.
 *
 * Designed to answer "is the radio working" at a glance, with enough
 * trend history to spot intermittent issues. Three sparklines (bursts,
 * CRC OK, magnitude) share a 60-second window so spikes can be
 * correlated visually — bursts going up while CRC stays flat is a
 * different problem from both rising together.
 * ─────────────────────────────────────────────────────────────────── */

/* Section rule with inset label. Mirrors P25's draw_rule. Named
 * differently from the track-page draw_rule to avoid collision in this
 * translation unit. */
static void diag_rule(int row, int col, const char *label, int width)
{
    tui_goto(row, col);
    fputs(C_BORDER, stdout);
    fputs(HL HL " ", stdout);
    if (label) printf(C_LABEL "%s " RESET C_BORDER, label);
    int used = (label ? (int)strlen(label) + 4 : 3);
    for (int i = used; i < width; i++) fputs(HL, stdout);
    fputs(RESET, stdout);
    fputs(EL, stdout);
}

/* Format an elapsed microsecond span as a compact "Nh NNm", "Nm NNs",
 * "NNs", or "<1s" string. Returns "never" when event_us==0 (meaning the
 * underlying event has never occurred). */
static void diag_fmt_elapsed(char *out, size_t n, int64_t event_us, int64_t now_us)
{
    if (event_us == 0) { snprintf(out, n, "never"); return; }
    int64_t us = now_us - event_us;
    if (us < 0) us = 0;
    int64_t s64 = us / 1000000LL;
    if (s64 > 0x7FFFFFFFLL) s64 = 0x7FFFFFFFLL;
    int s = (int)s64;
    if      (s < 1)    snprintf(out, n, "<1s");
    else if (s < 60)   snprintf(out, n, "%ds", s);
    else if (s < 3600) snprintf(out, n, "%dm %02ds", s / 60, s % 60);
    else               snprintf(out, n, "%dh %02dm",  s / 3600, (s / 60) % 60);
}

/* Color for a magnitude value 0-255 based on RF health bands.
 * <60   = under-driven / antenna problem  (dim)
 * 60-180 = normal operating window         (green)
 * 180-220 = hot, AGC will clip soon        (amber)
 * >220   = saturated, demod is blind       (red) */
static const char *diag_mag_color(int v)
{
    if (v < 60)   return C_DIM;
    if (v < 180)  return C_GREEN;
    if (v < 220)  return C_AMBER;
    return C_RED;
}

static const char *diag_mag_state(int v)
{
    if (v < 60)   return "LOW";
    if (v < 180)  return "NOMINAL";
    if (v < 220)  return "HOT";
    return "SATURATED";
}

/* Horizontal level bar: filled cells colored by the mag-state palette,
 * unfilled cells dim, with a single peak marker glyph (▼) above-and-
 * within the bar where the peak sits.
 *
 * Rendered as runs of identical glyph+color so we emit at most ~6 ANSI
 * escapes per call instead of ~80. Per-cell escapes turn out to push
 * the page over the UART's effective frame budget at 115200 baud and
 * cause visible row-by-row paint on the diag page (which has 4 bars
 * total). */
static void diag_level_bar(int avg, int peak, int width)
{
    if (avg < 0)   avg = 0;
    if (avg > 255) avg = 255;
    if (peak < avg) peak = avg;
    if (peak > 255) peak = 255;

    int filled   = avg  * width / 255;
    int peak_col = peak * width / 255;
    if (filled   > width)     filled   = width;
    if (peak_col >= width)    peak_col = width - 1;

    const char *fill_col = diag_mag_color(avg);

    /* Three runs: [0..min(filled, peak_col))   filled glyphs in fill color
     *             [peak_col..peak_col+1)       single peak marker
     *             [filled..width)              dim glyphs (after peak)
     * Plus a possible "filled but past peak_col" run if peak < avg, which
     * we already coerced to peak >= avg above so that case is impossible.
     *
     * Order of cells:
     *   if peak_col < filled (peak inside the filled region):
     *     fill_col × peak_col  +  peak_col × 1  +  fill_col × (filled - peak_col - 1)  +  dim × (width - filled)
     *   else (peak_col >= filled):
     *     fill_col × filled  +  dim × (peak_col - filled)  +  peak_col × 1  +  dim × (width - peak_col - 1)
     */
    if (peak_col < filled) {
        /* Peak inside filled region: one color the whole way through, peak
         * is just bolded. */
        fputs(fill_col, stdout);
        for (int i = 0; i < peak_col; i++)         fputs("\xe2\x96\x88", stdout);
        fputs(BOLD "\xe2\x96\x88" RESET, stdout);
        fputs(fill_col, stdout);
        for (int i = peak_col + 1; i < filled; i++) fputs("\xe2\x96\x88", stdout);
        fputs(C_DIM, stdout);
        for (int i = filled; i < width; i++)        fputs("\xe2\x96\x91", stdout);
        fputs(RESET, stdout);
    } else {
        /* Peak past the filled region: split run. */
        fputs(fill_col, stdout);
        for (int i = 0; i < filled; i++)            fputs("\xe2\x96\x88", stdout);
        fputs(C_DIM, stdout);
        for (int i = filled; i < peak_col; i++)     fputs("\xe2\x96\x91", stdout);
        fputs(diag_mag_color(peak), stdout);
        fputs(BOLD "\xe2\x96\x88" RESET, stdout);
        fputs(C_DIM, stdout);
        for (int i = peak_col + 1; i < width; i++)  fputs("\xe2\x96\x91", stdout);
        fputs(RESET, stdout);
    }
}

/* Sparkline of an array, colored by intensity. Each cell renders as
 * one of 8 block-glyph heights and is colored green→amber→red as the
 * value approaches the configured ceiling. Empty / zero cells render
 * as a dim baseline.
 *
 * Renders by tracking the "current color" and only emitting a new ANSI
 * color escape when the color changes — typical RF traffic produces
 * long runs of the same color so this drops the per-frame byte count
 * a lot at low traffic. */
static void diag_sparkline_u16(const uint16_t *data, int len, int ceiling)
{
    static const char *BARS[9] = {
        " ",
        "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
    };
    if (ceiling < 1) ceiling = 1;

    const char *cur_col = NULL;
    for (int i = 0; i < len; i++) {
        int v = data[i];
        const char *col;
        const char *glyph;
        if (v <= 0) {
            col   = C_DIM;
            glyph = "\xe2\x96\x81";  /* ▁ baseline */
        } else {
            int level = (v * 8 + ceiling - 1) / ceiling;
            if (level < 1) level = 1;
            if (level > 8) level = 8;
            col = level <= 2 ? C_GREEN :
                  level <= 5 ? C_AMBER : C_RED;
            glyph = BARS[level];
        }
        if (col != cur_col) {
            fputs(col, stdout);
            cur_col = col;
        }
        fputs(glyph, stdout);
    }
    fputs(RESET, stdout);
}

/* Magnitude sparkline: ceiling is fixed at 255 and color follows the
 * same band logic as the level meter so saturation events stay visible
 * in the trend even after they've passed. Same color-run batching as
 * the u16 sparkline above. */
static void diag_sparkline_mag(const uint8_t *data, int len)
{
    static const char *BARS[9] = {
        " ",
        "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
    };
    const char *cur_col = NULL;
    for (int i = 0; i < len; i++) {
        int v = data[i];
        int level = v * 8 / 255;
        if (level < 0) level = 0;
        if (level > 8) level = 8;
        const char *col = diag_mag_color(v);
        if (col != cur_col) {
            fputs(col, stdout);
            cur_col = col;
        }
        fputs(BARS[level], stdout);
    }
    fputs(RESET, stdout);
}

void adsb_draw_diag(int top, int rows, int cols)
{
    /* Pre-clear body. */
    for (int r = top; r < top + rows; r++) {
        tui_goto(r, 3); fputs(EL, stdout);
    }

    int width = cols - 2;
    if (width < 60)  width = 60;
    if (width > 124) width = 124;

    int row = top;

    /* ── Header line ──────────────────────────────────────────── */
    const app_t *app  = app_current();
    uint32_t     freq = app ? settings_get_freq(app) : 0;
    int          gain = app ? settings_get_gain(app) : 0;
    int64_t      now  = esp_timer_get_time();
    int64_t      uptime_s = now / 1000000LL;

    char freqbuf[24];
    snprintf(freqbuf, sizeof(freqbuf), "%lu.%03lu MHz",
             (unsigned long)(freq / 1000000UL),
             (unsigned long)((freq / 1000UL) % 1000UL));

    int up_s = (uptime_s > 0x7FFFFFFFLL) ? 0x7FFFFFFF : (int)uptime_s;
    char upbuf[16];
    if (up_s < 3600) {
        snprintf(upbuf, sizeof(upbuf), "%dm %02ds", up_s / 60, up_s % 60);
    } else {
        snprintf(upbuf, sizeof(upbuf), "%dh %02dm", up_s / 3600, (up_s / 60) % 60);
    }

    tui_goto(row, 3);
    printf(C_LABEL "RF DIAGNOSTICS" RESET
           "        " C_LABEL "GAIN " RESET C_BRIGHT "%d.%d dB" RESET
           "    " C_LABEL "FREQ " RESET C_TEXT "%s" RESET
           "    " C_LABEL "UPTIME " RESET C_TEXT "%s" RESET,
           gain / 10, gain % 10, freqbuf, upbuf);
    row += 2;

    /* ── INPUT LEVEL ──────────────────────────────────────────── */
    /* Magnitude as reported by perf is the raw lib value (0..~65211).
     * Scale to 0..255 to match the history ring's range and the
     * thresholds in diag_mag_color/diag_mag_state. */
    int avg  = perf_get_mag_avg()  >> 8;
    int peak = perf_get_mag_peak() >> 8;
    if (avg  < 0)   avg  = 0;
    if (avg  > 255) avg  = 255;
    if (peak < 0)   peak = 0;
    if (peak > 255) peak = 255;

    diag_rule(row, 3, "INPUT LEVEL", width - 4);
    row++;

    int bar_w = width - 30;
    if (bar_w > 80) bar_w = 80;
    if (bar_w < 30) bar_w = 30;

    /* Single row: "AVG NNN  [bar............]  PEAK NNN  STATE" */
    tui_goto(row, 3);
    printf(C_LABEL "AVG %3d  " RESET, avg);
    diag_level_bar(avg, peak, bar_w);
    printf("  " C_LABEL "PEAK %3d  " RESET "%s%s" RESET,
           peak, diag_mag_color(avg), diag_mag_state(avg));
    row += 2;

    /* ── DEMOD ACTIVITY ──────────────────────────────────────── */
    diag_rule(row, 3, "DEMOD ACTIVITY  60s history", width - 4);
    row++;

    const uint16_t *h_bursts = perf_history_bursts();
    const uint16_t *h_good   = perf_history_good();
    const uint8_t  *h_mag    = perf_history_mag_avg();

    int spark_w = PERF_HISTORY_LEN;            /* 60 chars = 1 char/sec */
    if (spark_w + 16 > width - 8) spark_w = width - 24;

    /* Adaptive ceiling for both data sparklines: max value seen in the
     * window, with a floor of 4 so a small spike doesn't fill the bar.
     * Computed jointly so both rows share the same scale and visual
     * comparison is valid. */
    int ceiling = 4;
    for (int i = 0; i < PERF_HISTORY_LEN; i++) {
        if (h_bursts[i] > ceiling) ceiling = h_bursts[i];
        if (h_good[i]   > ceiling) ceiling = h_good[i];
    }

    int bps = perf_get_bursts_per_sec();
    int gps = perf_get_msgs_per_sec();

    tui_goto(row, 3);
    printf(C_LABEL "BURSTS/s " RESET C_BRIGHT "%4d" RESET "  ", bps);
    diag_sparkline_u16(h_bursts, spark_w, ceiling);
    row++;

    tui_goto(row, 3);
    printf(C_LABEL "CRC OK/s " RESET C_GREEN "%4d" RESET "  ", gps);
    diag_sparkline_u16(h_good, spark_w, ceiling);
    row++;

    tui_goto(row, 3);
    printf(C_LABEL "MAG AVG  " RESET "%s%4d" RESET "  ",
           diag_mag_color(avg), avg);
    diag_sparkline_mag(h_mag, spark_w);
    row++;

    /* Time-axis label under the sparklines. The sparkline starts after
     * the 15-char label (e.g. "BURSTS/s   42  "). */
    tui_goto(row, 3 + 15);
    printf(C_DIM "60s ago" RESET);
    tui_goto(row, 3 + 15 + spark_w - 3);
    printf(C_DIM "now" RESET);
    row += 2;

    /* ── CUMULATIVE TOTALS ───────────────────────────────────── */
    diag_rule(row, 3, "CUMULATIVE", width - 4);
    row++;

    int crc_g = perf_get_crc_good();
    int crc_b = perf_get_crc_err();
    int bursts_total = perf_get_burst_total();
    int crc_pct = (crc_g + crc_b > 0) ? (100 * crc_g) / (crc_g + crc_b) : 0;

    tui_goto(row, 3);
    printf(C_LABEL "PREAMBLES " RESET C_TEXT "%6d" RESET "    "
           C_LABEL "CRC GOOD " RESET C_GREEN "%6d" RESET "    "
           C_LABEL "CRC BAD " RESET C_RED "%6d" RESET "    "
           C_LABEL "CRC OK%% " RESET "%s%3d%%" RESET,
           bursts_total, crc_g, crc_b,
           crc_pct >= 80 ? C_GREEN :
           crc_pct >= 40 ? C_AMBER : C_RED,
           crc_pct);
    row++;

    char e_burst[16], e_good[16], e_pos[16];
    diag_fmt_elapsed(e_burst, sizeof(e_burst), perf_get_last_burst_us(),    now);
    diag_fmt_elapsed(e_good,  sizeof(e_good),  perf_get_last_good_us(),     now);
    diag_fmt_elapsed(e_pos,   sizeof(e_pos),   perf_get_last_position_us(), now);

    tui_goto(row, 3);
    printf(C_LABEL "LAST BURST " RESET C_TEXT "%-10s" RESET
           C_LABEL "LAST GOOD MSG " RESET C_TEXT "%-10s" RESET
           C_LABEL "LAST POSITION " RESET C_TEXT "%-10s" RESET,
           e_burst, e_good, e_pos);
    row += 2;

    /* ── DATA PATH ────────────────────────────────────────────── */
    diag_rule(row, 3, "DATA PATH", width - 4);
    row++;

    uint32_t bps_usb = perf_get_bytes_per_sec();
    /* Expected ~3.9 MB/s at 2 MSps × 2 bytes = 4 MB/s. */
    int usb_pct = (int)(bps_usb / 40000UL);
    if (usb_pct > 100) usb_pct = 100;
    int usb_bar_w = 30;
    int usb_filled = usb_pct * usb_bar_w / 100;

    tui_goto(row, 3);
    printf(C_LABEL "USB BYTES/s " RESET C_BRIGHT "%lu.%02lu MB/s" RESET "  ",
           (unsigned long)(bps_usb / 1000000UL),
           (unsigned long)((bps_usb / 10000UL) % 100UL));
    fputs(usb_pct >= 90 ? C_GREEN : usb_pct >= 50 ? C_AMBER : C_RED, stdout);
    for (int i = 0; i < usb_filled;  i++) fputs("\xe2\x96\x88", stdout);
    fputs(C_DIM, stdout);
    for (int i = usb_filled; i < usb_bar_w; i++) fputs("\xe2\x96\x91", stdout);
    fputs(RESET, stdout);
    printf("  %s%s" RESET,
           usb_pct >= 90 ? C_GREEN  : usb_pct >= 50 ? C_AMBER : C_RED,
           usb_pct >= 90 ? "HEALTHY" : usb_pct >= 50 ? "DEGRADED" : "STALLED");

    /* Footer hint pinned to bottom of the body. */
    int foot = top + rows - 1;
    tui_goto(foot, 3);
    printf(C_DIM "sparklines colored by intensity: dim=quiet  green=light  amber=medium  red=heavy" RESET EL);
}
