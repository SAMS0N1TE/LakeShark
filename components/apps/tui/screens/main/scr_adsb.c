/* ADS-B screen: the aircraft table.

   Selection already lives in the model - `adsb_select_next/_prev/
   _index` are in adsb_state.h, not in the LVGL app - so this screen holds no
   selection state of its own. That is worth preserving: the moment a view
   owns the selection, two views disagree about it. */
#include "../../ls_tui_screen.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"

#include "../../ls_geo.h"
#include "../../ls_tui_ui.h"
#include "apps/adsb/adsb_state.h"
#include "core/perf.h"
#include "core/settings.h"
#include "ls_gps.h"

static int  s_top;      /* first visible row of the list                  */
static bool s_detail;   /* the list, or the selected aircraft's own page   */

/* Where draw() last put the list, the radar and the contacts on the
   radar, so a tap is tested against what is on the glass - the contract every
   screen with a touch handler keeps; scr_map.c records its pan pad the same
   way. None of it is state about the aircraft: the selection still lives in
   the model (). The plot table is in PSRAM, like the map's node table,
   because only the TUI task's draw and touch paths ever reach it. */
static tui_rect s_list_rect  = { 0, -1, 0, 0 };
static tui_rect s_radar_rect = { 0, -1, 0, 0 };
typedef struct { int16_t x, y; uint32_t icao; } radar_plot_t;
EXT_RAM_BSS_ATTR static radar_plot_t s_plot[ADSB_MAX_TRACKED];
static int s_nplot;
static tui_rect s_nav[3];
static uint32_t s_row_icao[ADSB_MAX_TRACKED];
static int s_rows;

/* The width the wide row needs: 48 characters, two columns of margin and
   two of border. Below it the narrow set is the only one that fits. */
#define WIDE_COLS 52

/* The rows draw_list spends on something other than a contact: the
   box's top border, the column header and its bottom border, and the
   three-row TRAFFIC strip under it. ADSB_MAX_TRACKED plus these is the whole
   table with nothing scrolled off. */
#define LIST_CHROME_ROWS 6
/* Below this the radar is a frame and a scale legend with no room to plot. */
#define RADAR_MIN_ROWS   14

/* Plot the model's 32 altitude samples using eighth-cell bars. */
#define ALT_SENTINEL (-1)   /* adsb_state_init's "never written" mark */

static void draw_altitude_trend(tui_surface *sf, tui_rect r,
                                const adsb_aircraft_t *a)
{
    if (r.h < 1 || r.w < 8) return;

    int lo = 1000000, hi = -1000000;
    bool any = false;
    for (int i = 0; i < 32; i++) {
        if (a->alt_history[i] == ALT_SENTINEL) continue;
        any = true;
        if (a->alt_history[i] < lo) lo = a->alt_history[i];
        if (a->alt_history[i] > hi) hi = a->alt_history[i];
    }
    if (!any) {
        tui_put_str(sf, r, r.x, r.y, "no altitude history yet",
                    LS_ATTR_DIM);
        return;
    }

    if (hi - lo < 200) { const int mid = (hi + lo) / 2; lo = mid - 100; hi = mid + 100; }

    const int n = r.w < 32 ? r.w : 32;
    const int span = hi - lo;
    for (int x = 0; x < n; x++) {
        /* Oldest to newest, left to right. `head` is where the NEXT sample
           lands, which makes it the OLDEST one still in the ring - see
           adsb_state_push_altitude. */
        const int idx = (a->alt_history_head + (32 - n) + x) % 32;
        const int16_t v = a->alt_history[idx];
        if (v == ALT_SENTINEL) continue;

        const int eighths = ((int)(v - lo) * (r.h * 8)) / span;
        const int whole = eighths / 8, rem = eighths % 8;
        for (int y = 0; y < whole && y < r.h; y++)
            tui_put_char(sf, r, r.x + x, r.y + r.h - 1 - y, LS_TUI_TRACE(8),
                        TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        if (rem && whole < r.h)
            tui_put_char(sf, r, r.x + x, r.y + r.h - 1 - whole,
                        LS_TUI_TRACE(rem), TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    }
}

/* DO-260B emitter categories: type codes 4/3/2 select A/B/C.
   Category zero and reserved entries do not identify an aircraft type. */
static const char *emitter_word(int tc, int ca)
{
    static const char *const SET_A[8] = {
        NULL, "light", "small", "large", "high vortex", "heavy",
        "high performance", "rotorcraft" };
    static const char *const SET_B[8] = {
        NULL, "glider", "lighter than air", "parachutist", "ultralight",
        NULL, "unmanned", "space vehicle" };
    static const char *const SET_C[8] = {
        NULL, "emergency vehicle", "service vehicle", "obstacle",
        "obstacle", "obstacle", NULL, NULL };
    if (ca < 0 || ca > 7) return NULL;
    return tc == 4 ? SET_A[ca] : tc == 3 ? SET_B[ca]
         : tc == 2 ? SET_C[ca] : NULL;
}

/* ENTER opens the selected aircraft's details. */
static void draw_detail(tui_surface *sf, tui_rect area,
                        const adsb_aircraft_t *a, int64_t now)
{
    const uint8_t frame = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t val   = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t good  = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t warn  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim = LS_ATTR_DIM;
    char buf[48];

    char title[16];
    snprintf(title, sizeof(title), "%06lX", (unsigned long)a->icao);
    tui_box(sf, area, title, frame);
    /* Nine fields now, rows 1 to 9, so the box needs eleven rows to
       keep them inside its borders. The old floor of eight let the last two
       of the eight it had land on the bottom border and past it. */
    if (area.w < 20 || area.h < 11) return;

    int row = 1;
    snprintf(buf, sizeof(buf), "%.8s", a->callsign[0] ? a->callsign : "unknown");
    ls_kv(sf, area, row++, "CALLSIGN", buf, val);

    /* Right under the name, because "what is it" is the next
       question. Dim until an identification has said. */
    if (a->emitter_tc) {
        const char *word = emitter_word(a->emitter_tc, a->emitter_ca);
        snprintf(buf, sizeof(buf), "%c%d %s", 'A' + (4 - a->emitter_tc),
                 a->emitter_ca, word ? word : "not given");
    } else {
        snprintf(buf, sizeof(buf), "not sent yet");
    }
    ls_kv(sf, area, row++, "CATEGORY", buf, a->emitter_tc ? val : dim);

    snprintf(buf, sizeof(buf), "%d ft", a->altitude);
    ls_kv(sf, area, row++, "ALTITUDE", buf, val);

    const uint8_t vs_at = a->vert_rate > 100 ? good
                        : a->vert_rate < -100 ? warn : val;
    snprintf(buf, sizeof(buf), "%+d ft/min", a->vert_rate);
    ls_kv(sf, area, row++, "V/S", buf, vs_at);

    snprintf(buf, sizeof(buf), "%d kt", a->velocity);
    ls_kv(sf, area, row++, "SPEED", buf, val);

    snprintf(buf, sizeof(buf), "%03d deg", a->heading);
    ls_kv(sf, area, row++, "HEADING", buf, val);

    if (a->pos_valid)
        snprintf(buf, sizeof(buf), "%.4f, %.4f", (double)a->lat, (double)a->lon);
    else
        snprintf(buf, sizeof(buf), "not reported yet");
    ls_kv(sf, area, row++, "POSITION", buf, a->pos_valid ? val : dim);

    int age = (int)((now - a->last_seen_us) / 1000000);
    if (age < 0) age = 0;
    int tracked = (int)((now - a->first_seen_us) / 1000000);
    if (tracked < 0) tracked = 0;
    snprintf(buf, sizeof(buf), "%ds ago, tracked %ds", age, tracked);
    ls_kv(sf, area, row++, "LAST SEEN", buf, age > 15 ? warn : val);

    snprintf(buf, sizeof(buf), "%d ok, %d crc err",
             a->good_msg_count, a->crc_err_count);
    ls_kv(sf, area, row++, "MESSAGES", buf, a->crc_err_count ? warn : val);

    row += 1;
    if (row < area.h - 4) {
        tui_put_str(sf, area, area.x + 2, area.y + row, "ALTITUDE, RECENT REPORTS",
                    TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        row += 1;
        const int trend_h = area.h - 1 - row;
        if (trend_h > 0)
            draw_altitude_trend(sf,
                tui_rect_make(area.x + 2, area.y + row, area.w - 4, trend_h),
                a);
    }
}

/* ------------------------------------------------------------- radar --- */

/* Bearing and range, recomputed fresh every frame. */

static void bearing_range_nm(double lat0, double lon0, double lat1, double lon1,
                             double *bearing_deg, double *range_nm)
{
    double m = 0;
    ls_geo_bearing_range(lat0, lon0, lat1, lon1, bearing_deg, &m);
    *range_nm = m / LS_GEO_M_PER_NM;
}

static bool radar_center(double *lat, double *lon, bool *live)
{
    ls_gps_state_t g;
    ls_gps_get(&g);
    if (g.fix) {
        *lat = g.lat_deg; *lon = g.lon_deg;
        if (live) *live = true;
        return true;
    }
    float hlat, hlon;
    if (settings_get_home(&hlat, &hlon)) {
        *lat = (double)hlat; *lon = (double)hlon;
        if (live) *live = false;
        return true;
    }
    return false;
}

static int ring_scale_nm(double max_range_nm)
{
    static const int STEPS[] = { 5, 10, 20, 40, 80, 160, 320 };
    for (unsigned i = 0; i < sizeof(STEPS) / sizeof(STEPS[0]); i++)
        if (max_range_nm <= STEPS[i]) return STEPS[i];
    return STEPS[sizeof(STEPS) / sizeof(STEPS[0]) - 1];
}

static void draw_radar(tui_surface *sf, tui_rect area)
{
    const uint8_t frame = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    tui_box(sf, area, "RADAR", frame);
    if (area.w < 14 || area.h < 10) return;

    double clat, clon;
    bool live;
    if (!radar_center(&clat, &clon, &live)) {

        ls_panel_notice(sf, area, "RADAR", "no position to centre on",
                        "wait for a GPS fix, or 'home <lat> <lon>'");
        return;
    }

    /* One row of air below the title, one row reserved above the bottom
       border for the scale legend - the scope itself is what is left. */
    const tui_rect body = tui_rect_make(area.x + 1, area.y + 2,
                                        area.w - 2, area.h - 4);
    if (body.w < 10 || body.h < 6) return;
    const int legend_row = area.y + area.h - 2;

    /* Circle, not ellipse - corrected for the cell's own aspect
       (10x17, nowhere near square) the same way the map's sub-pixel buffer
       is: a radius equal in cells in both axes draws something one and
       seven tenths taller than it is wide. */
    int cw = 10, ch = 17;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    if (cw < 1) cw = 10;
    if (ch < 1) ch = 17;
    const double max_rx_px = (body.w / 2.0) * cw;
    const double max_ry_px = (body.h / 2.0) * ch;
    const double radius_px = max_rx_px < max_ry_px ? max_rx_px : max_ry_px;
    const double rx = radius_px / cw, ry = radius_px / ch;
    const double ccx = body.x + body.w / 2.0, ccy = body.y + body.h / 2.0;

    double max_range = 0.0;
    for (int slot = 0; slot < 16; slot++) {
        const adsb_aircraft_t *a = adsb_state_get(slot);
        if (!a || !a->active || !a->pos_valid) continue;
        double brg, rng;
        bearing_range_nm(clat, clon, (double)a->lat, (double)a->lon, &brg, &rng);
        if (rng > max_range) max_range = rng;
    }
    const int scale_nm = ring_scale_nm(max_range);

    const uint8_t ring_c = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t comp_c = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);

    /* A dotted circle, not a filled one - the same "line drawing,
       not colour turned off" reasoning the map's mono view is built on: a
       filled disc buries every contact near the centre of the screen under
       its own background. Two passes, the boundary and a half-scale ring,
       so there is a sense of distance without a number attached to every
       point on it. */
    for (int pass = 0; pass < 2; pass++) {
        const double frac = pass == 0 ? 1.0 : 0.5;
        for (int i = 0; i < 72; i++) {
            const double a_rad = (double)i / 72.0 * 2.0 * M_PI;
            const int px = (int)lround(ccx + sin(a_rad) * rx * frac);
            const int py = (int)lround(ccy - cos(a_rad) * ry * frac);
            if (px >= body.x && px < body.x + body.w &&
                py >= body.y && py < body.y + body.h)
                tui_put_char(sf, body, px, py, '.', ring_c);
        }
    }

    const int icx = (int)lround(ccx), icy = (int)lround(ccy);
    tui_put_char(sf, body, icx, body.y, 'N', comp_c);
    tui_put_char(sf, body, icx, body.y + body.h - 1, 'S', comp_c);
    tui_put_char(sf, body, body.x, icy, 'W', comp_c);
    tui_put_char(sf, body, body.x + body.w - 1, icy, 'E', comp_c);

    tui_put_char(sf, body, icx, icy, '+',
                TUI_ATTR(live ? (TUI_GREEN | TUI_BRIGHT) : (TUI_YELLOW | TUI_BRIGHT),
                         TUI_BLACK));

    typedef struct { int x0, x1, y; } rbox;
    rbox taken[16];
    int ntaken = 0;

    const uint32_t sel_icao = adsb_select_get_icao();
    for (int slot = 0; slot < 16; slot++) {
        const adsb_aircraft_t *a = adsb_state_get(slot);
        if (!a || !a->active || !a->pos_valid) continue;
        double brg, rng;
        bearing_range_nm(clat, clon, (double)a->lat, (double)a->lon, &brg, &rng);
        if (rng > scale_nm) continue;   /* off the scope, not off the sky */

        const double frac = rng / scale_nm;
        const double rad = brg * M_PI / 180.0;
        const int px = (int)lround(ccx + sin(rad) * rx * frac);
        const int py = (int)lround(ccy - cos(rad) * ry * frac);
        if (px < body.x || px >= body.x + body.w ||
            py < body.y || py >= body.y + body.h) continue;

        /* Altitude band, the three-colour read a real scope uses: low is
           not the same picture as high and transiting, and colour says
           which without spending a label on every point. */
        const uint8_t hue = a->altitude < 5000  ? TUI_YELLOW
                           : a->altitude < 20000 ? TUI_GREEN
                                                  : TUI_CYAN;
        const bool sel = (a->icao == sel_icao);
        tui_put_char(sf, body, px, py, sel ? '@' : LS_TUI_BLOCK_FULL,
                    TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK));

        /* Where it landed, for touch(): a tap is resolved against the
           contacts actually on the scope, not recomputed from a projection
           the next frame might draw differently. Recorded before the label,
           which a crowd can drop - the dot is always drawn. */
        if (s_nplot < ADSB_MAX_TRACKED) {
            s_plot[s_nplot].x = (int16_t)px;
            s_plot[s_nplot].y = (int16_t)py;
            s_plot[s_nplot].icao = a->icao;
            s_nplot++;
        }

        /* A callsign one cell clear of its dot, only when it fits inside
           the scope AND nothing else already claimed that row-span. */
        if (!a->callsign[0] || ntaken >= 16) continue;
        char cs[9];
        snprintf(cs, sizeof(cs), "%.8s", a->callsign);
        const int len = (int)strlen(cs);
        const int x0 = px + 1, x1 = x0 + len - 1;
        if (x1 >= body.x + body.w) continue;
        bool free = true;
        for (int i = 0; i < ntaken; i++) {
            if (taken[i].y != py) continue;
            if (x0 <= taken[i].x1 && x1 >= taken[i].x0) { free = false; break; }
        }
        if (!free) continue;
        tui_put_str(sf, body, x0, py, cs, TUI_ATTR(hue, TUI_BLACK));
        taken[ntaken].x0 = x0; taken[ntaken].x1 = x1; taken[ntaken].y = py;
        ntaken++;
    }

    char scale_buf[16];
    snprintf(scale_buf, sizeof(scale_buf), "%d NM", scale_nm);
    tui_put_str(sf, area, area.x + area.w - 2 - (int)strlen(scale_buf),
               legend_row, scale_buf, LS_ATTR_DIM);
    if (!live)
        tui_put_str(sf, area, area.x + 2, legend_row, "using saved home",
                   LS_ATTR_DIM);
}

/* The dashboard band: what the last two minutes looked like. */

#define TRAFFIC_N 128
static uint8_t s_traffic[TRAFFIC_N];
static int     s_traffic_head;
static int     s_traffic_len;
static int64_t s_traffic_next_us;

static void traffic_sample(int64_t now)
{
    if (s_traffic_next_us && now < s_traffic_next_us) return;
    s_traffic_next_us = now + 1000000;

    int n = adsb_state_active_count();
    if (n < 0) n = 0;
    if (n > 255) n = 255;
    s_traffic[s_traffic_head] = (uint8_t)n;
    s_traffic_head = (s_traffic_head + 1) % TRAFFIC_N;
    if (s_traffic_len < TRAFFIC_N) s_traffic_len++;
}

static void draw_dash(tui_surface *sf, tui_rect r, int64_t now)
{
    /* Not "TRAFFIC" - the list already has a strip by that name with
       the live count and the message rate on it, and two boxes with one title
       is a screen arguing with itself. This one is about TIME, and it says
       only what the other does not. */
    ls_panel_box(sf, r, "HISTORY", TUI_CYAN);
    if (r.h < 4 || r.w < 24) return;

    const uint8_t val = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim = LS_ATTR_DIM;

    int    live = 0, with_pos = 0;
    double far_nm = 0.0;
    double clat = 0, clon = 0;
    const bool have_centre = radar_center(&clat, &clon, NULL);

    for (int i = 0; i < ADSB_MAX_TRACKED; i++) {
        const adsb_aircraft_t *a = adsb_state_get(i);
        if (!a || !a->active) continue;
        live++;
        if (!a->pos_valid || !have_centre) continue;
        with_pos++;
        double b = 0, nm = 0;
        bearing_range_nm(clat, clon, a->lat, a->lon, &b, &nm);
        if (nm > far_nm) far_nm = nm;
    }

    int peak = 0;
    for (int i = 0; i < s_traffic_len; i++) if (s_traffic[i] > peak) peak = s_traffic[i];

    char buf[96];
    if (have_centre)
        snprintf(buf, sizeof(buf),
                 "with position %d   peak %d   furthest %.0f nm",
                 with_pos, peak, far_nm);
    else
        /* No centre is not a range of zero. The radar says the same thing in
           its own panel; saying "0 nm" here would be a measurement nobody
           took. */
        snprintf(buf, sizeof(buf),
                 "with position %d   peak %d   no fix, so no range",
                 with_pos, peak);
    tui_put_str(sf, r, r.x + 2, r.y + 1, buf, live ? val : dim);

    const int gy = r.y + r.h - 2;
    const int gw = r.w - 4;
    if (gw < 8 || gy <= r.y + 1) return;

    if (!s_traffic_len) {
        tui_put_str(sf, r, r.x + 2, gy, "nothing sampled yet", dim);
        return;
    }

    int top = peak < 1 ? 1 : peak;
    for (int x = 0; x < gw; x++) {
        /* Oldest on the left, newest on the right, which is the direction
           every chart of time is read in. */
        const int back = gw - 1 - x;
        if (back >= s_traffic_len) continue;
        const int idx = (s_traffic_head - 1 - back + TRAFFIC_N * 2) % TRAFFIC_N;
        const int v = s_traffic[idx];
        if (!v) continue;
        int eighths = v * 8 / top;
        if (eighths < 1) eighths = 1;
        if (eighths > 8) eighths = 8;
        tui_put_char(sf, r, r.x + 2 + x, gy, LS_TUI_TRACE(eighths),
                     TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    }

    snprintf(buf, sizeof(buf), "%d", top);
    tui_put_str(sf, r, r.x + r.w - 2 - (int)strlen(buf), r.y + r.h - 3, buf,
                dim);
    tui_put_str(sf, r, r.x + 2, r.y + r.h - 3,
                s_traffic_len >= gw ? "2 min" : "since opening", dim);
}

static void draw_list(tui_surface *sf, tui_rect area, int64_t now)
{
    const uint8_t frame  = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t head   = TUI_ATTR(TUI_BLACK, TUI_CYAN);
    const uint8_t sel    = TUI_ATTR(TUI_BLACK, TUI_CYAN | TUI_BRIGHT);
    const uint8_t body   = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    const uint8_t stale = LS_ATTR_DIM;
    char buf[128];

    /* The list and the footer tile the pane exactly, and both are clamped
       into it. Without the clamp a pane shorter than the footer put the
       footer above the pane's own top - area.h - 3 goes negative - and
       tui_box drew its border on a row belonging to whatever was above.
       The renderer only pushes changed cells, so that border stayed there. */
    int foot_h = area.h < 3 ? (area.h < 0 ? 0 : area.h) : 3;
    int list_hh = area.h - foot_h;
    if (list_hh < 0) list_hh = 0;

    int list_h = list_hh - 2;
    tui_rect list = tui_rect_make(area.x, area.y, area.w, list_hh);
    tui_box(sf, list, "AIRCRAFT", frame);

    /* Two column sets, because one of them does not fit portrait. */

    const bool narrow = list.w < WIDE_COLS;

    tui_fill(sf, tui_rect_make(list.x + 1, list.y + 1, list.w - 2, 1), ' ', head);
    tui_put_str(sf, list, list.x + 2, list.y + 1,
                narrow ? "ICAO    CALLSIGN   ALT     AGE"
                       : "ICAO    CALLSIGN   ALT     SPD  HDG   AGE  MSGS",
                head);

    int sel_index = 0, total = 0;
    adsb_select_index(&sel_index, &total);
    int active = adsb_state_active_count();

    /* Keep the selection on screen without letting the list jump around: only
       scroll when the selection has actually left the window. */
    if (sel_index < s_top) s_top = sel_index;
    if (sel_index >= s_top + list_h - 1) s_top = sel_index - list_h + 2;
    if (s_top < 0) s_top = 0;

    int row = 0, shown = 0;
    for (int slot = 0; slot < 16 && row < list_h - 1; slot++) {
        const adsb_aircraft_t *a = adsb_state_get(slot);
        if (!a || !a->active) continue;
        if (shown++ < s_top) continue;

        int age = (int)((now - a->last_seen_us) / 1000000);
        if (age < 0) age = 0;
        char call[10];
        snprintf(call, sizeof(call), "%.8s", a->callsign[0] ? a->callsign : "-");
        if (narrow)
            snprintf(buf, sizeof(buf), "%06lX  %-9s %6d  %4d",
                     (unsigned long)a->icao, call, a->altitude, age);
        else
            snprintf(buf, sizeof(buf), "%06lX  %-9s %6d  %4d  %3d  %4d  %5lu",
                     (unsigned long)a->icao, call, a->altitude, a->velocity,
                     a->heading, age, (unsigned long)a->msg_count);
        uint8_t at = (shown - 1 == sel_index) ? sel
                   : a->pos_valid            ? body : stale;
        tui_put_str(sf, list, list.x + 2, list.y + 2 + row, buf, at);
        if (s_rows < ADSB_MAX_TRACKED) s_row_icao[s_rows++] = a->icao;
        row++;
    }
    if (!active)
        tui_put_str(sf, list, list.x + 2, list.y + 3, "no aircraft heard yet",
                    stale);

    /* A sparkline of message rate, from the model's own 60-sample ring - the
       GUI kept its own copy of this and did not need to. */
    tui_rect foot = tui_rect_make(area.x, area.y + list_hh, area.w, foot_h);
    tui_box(sf, foot, "TRAFFIC", frame);
    const uint16_t *hist = perf_history_good();
    if (hist) {
        int w = foot.w - 4;
        uint16_t peak = 1;
        for (int i = 0; i < 60; i++) if (hist[i] > peak) peak = hist[i];
        for (int x = 0; x < w && x < 60; x++) {
            uint16_t v = hist[59 - (x * 60 / (w ? w : 1))];
            int eighths = (int)((uint32_t)v * 8u / peak);
            if (v && !eighths) eighths = 1;
            if (eighths)
                tui_put_char(sf, foot, foot.x + 2 + x, foot.y + 1,
                             LS_TUI_TRACE(eighths),
                             TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
        }
    }
    snprintf(buf, sizeof(buf), "%d active   %d msg/s   %d crc err",
             active, perf_get_msgs_per_sec(), perf_get_crc_err());
    tui_put_str(sf, foot, foot.x + foot.w - 2 - (int)strlen(buf), foot.y + 1,
                buf, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
}

static void draw(tui_surface *sf, tui_rect area)
{
    const int64_t now = esp_timer_get_time();
    const adsb_aircraft_t *sel = s_detail ? adsb_select_get() : NULL;

    if (s_detail && !sel) s_detail = false;

    traffic_sample(now);

    /* Nothing is claimed until a pane is drawn: a rect left over
       from the other posture, or from the list before the detail page
       opened, would take taps meant for whatever is there now. */
    s_list_rect  = tui_rect_make(0, -1, 0, 0);
    s_radar_rect = tui_rect_make(0, -1, 0, 0);
    s_nplot = 0;
    s_rows = 0;
    memset(s_nav, 0, sizeof(s_nav));
    if (area.h >= 18 && area.w >= 42) {
        const int nav_h = ls_tui_is_wide() ? 3 : 5;
        const char *labels[] = {"UP prev", s_detail ? "ENTER back" :
                               area.w >= 45 ? "ENTER details" : "ENTER info", "DOWN next"};
        const int step = (area.w - 1) / 3;
        for (int i = 0; i < 3; i++) {
            const int x = area.x + i * step;
            const int w = i == 2 ? area.x + area.w - x : step + 1;
            const tui_rect box = tui_rect_make(x, area.y + area.h - nav_h, w, nav_h);
            s_nav[i] = box;
            if (i < 2) s_nav[i].w--;
            ls_panel_box(sf, box, NULL, TUI_CYAN);
            const tui_rect face = tui_rect_make(x + 1, box.y + 1, w - 2, nav_h - 2);
            ls_fill_dither(sf, face, LS_DITHER_LIGHT, TUI_CYAN);
            ls_dither_label(sf, face, (face.h - 1) / 2, labels[i],
                            TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
        }
        area.h -= nav_h;
    }

    if (s_detail && sel) { draw_detail(sf, area, sel, now); return; }

    /* Landscape places the aircraft list beside the radar. */
    if (ls_tui_is_wide() && area.w >= 80) {
        /* The dashboard takes the bottom, and only if the table and
           the radar can still be read above it. Six rows is the trace, its
           scale and the figures; below about twenty rows left over, a list
           of aircraft stops being a list, and the table is the thing this
           screen is for. */
        tui_rect top = area;
        if (area.h >= 26) {
            top = tui_rect_make(area.x, area.y, area.w, area.h - 6);
            draw_dash(sf, tui_rect_make(area.x, area.y + top.h, area.w, 6),
                      now);
        }
        tui_rect list, radar;
        ls_tui_split_at(top, top.w - 38, &list, &radar);
        draw_list(sf, list, now);
        draw_radar(sf, radar);
        s_list_rect  = list;
        s_radar_rect = radar;
        return;
    }

    /* Stack all sixteen contacts above the radar in portrait.
   Keep the list height fixed so arriving contacts cannot move the scope. */
    const int list_rows = ADSB_MAX_TRACKED + LIST_CHROME_ROWS;
    if (area.h >= list_rows + RADAR_MIN_ROWS) {
        const tui_rect list  = tui_rect_make(area.x, area.y, area.w, list_rows);
        const tui_rect radar = tui_rect_make(area.x, area.y + list_rows,
                                             area.w, area.h - list_rows);
        draw_list(sf, list, now);
        draw_radar(sf, radar);
        s_list_rect  = list;
        s_radar_rect = radar;
        return;
    }
    draw_list(sf, area, now);
    s_list_rect = area;
}

static bool key(ls_tk_t k, char ch)
{
    (void)ch;
    /* UP/DOWN keep working in DETAIL - moving through contacts is the more
       useful reading of "next" while looking at one, and it is one selection
       state either way (), so nothing about switching pages needs to
       change to support it. */
    if (k == LS_TK_UP)   { adsb_select_prev(); return true; }
    if (k == LS_TK_DOWN) { adsb_select_next(); return true; }
    if (k == LS_TK_ENTER) {
        if (!s_detail && !adsb_select_get()) return true;  /* nothing to open */
        s_detail = !s_detail;
        return true;
    }
    if (k == LS_TK_ESC && s_detail) { s_detail = false; return true; }
    return false;
}

/* Hit-test the last drawn panes. Radar taps select the nearest
   contact; a second tap opens it. List rows open their drawn ICAO directly. */
static bool hit(tui_rect r, int col, int row)
{
    return r.h > 0 && col >= r.x && col < r.x + r.w &&
           row >= r.y && row < r.y + r.h;
}

static bool touch(int col, int row)
{
    for (int i = 0; i < 3; i++)
        if (hit(s_nav[i], col, row))
            return key(i == 0 ? LS_TK_UP : i == 1 ? LS_TK_ENTER : LS_TK_DOWN, 0);
    if (s_detail) return true;

    if (hit(s_radar_rect, col, row)) {
        int cw = 10, ch = 17;
        ls_tui_geometry(NULL, NULL, &cw, &ch);
        if (cw < 1) cw = 10;
        if (ch < 1) ch = 17;
        int  best = -1;
        long best_d = 0;
        for (int i = 0; i < s_nplot; i++) {
            const long dx = (long)(s_plot[i].x - col) * cw;
            const long dy = (long)(s_plot[i].y - row) * ch;
            const long d = dx * dx + dy * dy;
            if (best < 0 || d < best_d) { best = i; best_d = d; }
        }
        if (best < 0) return true;
        if (adsb_select_get_icao() == s_plot[best].icao) s_detail = true;
        else adsb_select_set_icao(s_plot[best].icao);
        return true;
    }

    if (hit(s_list_rect, col, row)) {
        const int index = row - s_list_rect.y - 2;
        if (index >= 0 && index < s_rows) {
            adsb_select_set_icao(s_row_icao[index]);
            s_detail = adsb_select_get() != NULL;
        }
        return true;
    }
    return false;
}

static void leave(void) { s_detail = false; }

const ls_tui_screen_t ls_scr_adsb = {
    /* an aircraft list with no receiver behind it is an empty table. */
    .radio = "ADS-B",
    .name = "ADSB",
    .hint = "UP/DOWN aircraft  ENTER details  ESC back",
    .enter = NULL,
    .leave = leave,
    .draw = draw,
    .key = key,
    .touch = touch,
};
