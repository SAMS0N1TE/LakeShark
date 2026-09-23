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
#include "../../ls_map.h"
#include "../../ls_quick.h"
#include "../../ls_tui_ui.h"
#include "apps/adsb/adsb_state.h"
#include "core/perf.h"
#include "core/settings.h"
#include "ls_gps.h"

static bool s_radar_only;
static tui_rect s_tools;
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
    float hlat, hlon;
    if (settings_get_home(&hlat, &hlon) && fabs(hlat)<=85 && fabs(hlon)<=180) {
        *lat=hlat; *lon=hlon;
        if (live) *live=false;
        return true;
    }
    ls_gps_state_t g;
    ls_gps_get(&g);
    const int64_t now=esp_timer_get_time();
    if (g.fix && g.last_fix_us && now>=g.last_fix_us && now-g.last_fix_us<=10000000 &&
        isfinite(g.lat_deg) && isfinite(g.lon_deg) && fabs(g.lat_deg)<=85 && fabs(g.lon_deg)<=180) {
        *lat = g.lat_deg; *lon = g.lon_deg;
        if (live) *live = true;
        return true;
    }
    return false;
}

static void draw_radar(tui_surface *sf, tui_rect area)
{
    const uint8_t bright=TUI_ATTR(TUI_CYAN|TUI_BRIGHT,TUI_BLACK);
    tui_box(sf,area,"MINI MAP / RTL ADS-B",bright);
    if(area.w<20 || area.h<10) return;
    double lat,lon; bool live;
    if(!radar_center(&lat,&lon,&live)) {
        ls_panel_notice(sf,area,"SET HOME","GPS, coordinates or map","Saved for next time");
        return;
    }
    const tui_rect body=tui_rect_make(area.x+1,area.y+3,area.w-2,area.h-6);
    ls_map_preview(sf,body,lat,lon);
    char text[80];
    snprintf(text,sizeof(text),"%s %.4f, %.4f",live?"GPS":"HOME",lat,lon);
    tui_put_str(sf,area,area.x+2,area.y+1,text,bright);
    tui_put_str(sf,area,area.x+2,area.y+2,"^N  +home  >air  @selected  .stale",LS_ATTR_DIM);
    const int64_t now=esp_timer_get_time();
    const uint32_t selected=adsb_select_get_icao();
    struct {int x0,x1,y;} labels[ADSB_MAX_TRACKED]; int nlabels=0;
    for(int i=0;i<ADSB_MAX_TRACKED;i++) {
        const adsb_aircraft_t *a=adsb_state_get(i);
        if(!a || !a->active || !a->pos_valid) continue;
        int x,y;
        if(!ls_map_preview_point(a->lat,a->lon,body,&x,&y)) continue;
        bool fresh=a->pos_ts_us>0 && now>=a->pos_ts_us && now-a->pos_ts_us<=15000000;
        uint8_t ink=fresh?TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK):LS_ATTR_DIM;
        tui_put_char(sf,body,x,y,a->icao==selected?'@':fresh?'>':'.',ink);
        ls_map_preview_reserve(body,x,y,1);
        if(s_nplot<ADSB_MAX_TRACKED) s_plot[s_nplot++]=(radar_plot_t){x,y,a->icao};
        if(x+9<body.x+body.w) {
            snprintf(text,sizeof(text),"%.8s",a->callsign[0]?a->callsign:"");
            const int end=x+(int)strlen(text);
            bool clear=true;
            for(int j=0;j<nlabels;j++) if(labels[j].y==y && x+1<=labels[j].x1 && end>=labels[j].x0) clear=false;
            if(clear && nlabels<ADSB_MAX_TRACKED) {
                tui_put_str(sf,body,x+1,y,text,ink);
                ls_map_preview_reserve(body,x+1,y,(int)strlen(text));
                labels[nlabels].x0=x+1;labels[nlabels].x1=end;labels[nlabels++].y=y;
            }
        }
    }
    int hx,hy;
    if(ls_map_preview_point(lat,lon,body,&hx,&hy)) {
        tui_put_char(sf,body,hx,hy,'+',bright);
        ls_map_preview_reserve(body,hx,hy,1);
    }
    ls_map_preview_labels(sf,body);
    double width_nm=40075016.686*cos(lat*M_PI/180.0)*(body.w*3)/
        (ldexp(1.0,ls_map_zoom())*ls_map_tile_px()*LS_GEO_M_PER_NM);
    snprintf(text,sizeof(text),"%s z%d  %.1f NM across",ls_map_render_busy()?"LOADING":"OFFLINE",ls_map_zoom(),width_nm);
    tui_put_str(sf,area,area.x+2,area.y+area.h-3,text,LS_ATTR_DIM);
    const char *why=ls_map_status();
    tui_put_str(sf,area,area.x+2,area.y+area.h-2,
        why && !ls_map_render_busy()?"No tiles here; MAPS in full map":"Tap aircraft for details",LS_ATTR_DIM);
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
    s_tools=tui_rect_make(0,-1,0,0);
    if(area.h>=18 && area.w>=24) {
    const ls_btn_t tools[]={{"FULL MAP",NULL,'m',false,false},
        {"SET HOME",NULL,'h',false,false},{s_radar_only?"LIST":"MAP ONLY",NULL,'r',s_radar_only,false},
        {"ZOOM+",NULL,'=',false,false},{"ZOOM-",NULL,'-',false,false}};
    int th=ls_tui_is_wide()?3:10;
    s_tools=tui_rect_make(area.x,area.y,area.w,th);
    if(!ls_tui_is_wide() && s_tools.w>49) {s_tools.x+=(s_tools.w-49)/2;s_tools.w=49;}
    ls_btn_bar_raised_slot(sf,s_tools,tools,5,-1,LS_BTN_SLOT_QUICK);
    area.y+=th;area.h-=th;
    }

    if (area.h >= 18 && area.w >= 42) {
        const int nav_h = ls_tui_is_wide() ? 3 : 5;
        const bool wide=ls_tui_is_wide();
        const char *labels[] = {wide ? "UP prev" : "PREVIOUS", s_detail ? "BACK" :
                               wide ? "ENTER info" : "DETAILS", wide ? "DOWN next" : "NEXT"};
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

    if (s_radar_only) {draw_radar(sf,area);s_radar_rect=area;return;}

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
        ls_tui_split_at(top, top.w / 2, &list, &radar);
        draw_list(sf, list, now);
        draw_radar(sf, radar);
        s_list_rect  = list;
        s_radar_rect = radar;
        return;
    }

    /* Stack all sixteen contacts above the radar in portrait.
   Keep the list height fixed so arriving contacts cannot move the scope. */
    int list_rows = ADSB_MAX_TRACKED + LIST_CHROME_ROWS;
    if(list_rows>area.h-RADAR_MIN_ROWS) list_rows=area.h-RADAR_MIN_ROWS;
    if (list_rows >= LIST_CHROME_ROWS + 2) {
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
    if(k==LS_TK_CHAR && (ch=='='||ch=='+'||ch=='-')) {ls_map_zoom_by(ch=='-'?-1:1);return true;}
    if(k==LS_TK_CHAR && (ch=='r'||ch=='R')) {s_radar_only=!s_radar_only;s_detail=false;return true;}
    if(k==LS_TK_CHAR && (ch=='m'||ch=='M'||ch=='h'||ch=='H')) {
        ls_args_t a={0};ls_val_t out;
        ls_action_call(ch=='m'||ch=='M'?"map.here":"map.home",&a,&out,ls_quick_grant_builtin());
        if(ch=='m'||ch=='M') {
            float lat,lon;
            if(settings_get_home(&lat,&lon) && fabs(lat)<=85) {
                ls_map_follow_set(false);
                ls_map_center(lat,lon);
            }
        }
        return true;
    }
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
    if(hit(s_tools,col,row)) {
        int i=ls_btn_hit_slot(col,row,LS_BTN_SLOT_QUICK);
        if(i>=0 && i<5) return key(LS_TK_CHAR,"mhr=-"[i]);
        return true;
    }

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

static void enter(void)
{
    /* A useful regional overview; preserve the user zoom across relaunches. */
    static bool first=true;
    if(first) {ls_map_zoom_by(8-ls_map_zoom());first=false;}
}
static void leave(void) { s_detail = false; ls_map_preview_leave(); }

const ls_tui_screen_t ls_scr_adsb = {
    /* an aircraft list with no receiver behind it is an empty table. */
    .radio = "ADS-B",
    .name = "ADSB",
    .hint = "UP/DOWN aircraft  ENTER details  ESC back",
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .key = key,
    .touch = touch,
};
