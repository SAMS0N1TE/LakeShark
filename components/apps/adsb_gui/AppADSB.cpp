#include "AppADSB.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>

#include "esp_timer.h"

extern "C" {
#include "app_registry.h"
#include "settings.h"
#include "adsb_state.h"
#include "adsb_decode.h"
#include "lakeshark_backend.h"
#include "sam_tts.h"
#include "audio_events.h"
#include "audio_out.h"

#define PERF_HISTORY_LEN 60
int      perf_get_active_count(void);
int      perf_get_msgs_total(void);
int      perf_get_msgs_per_sec(void);
int      perf_get_crc_good(void);
int      perf_get_crc_err(void);
int      perf_get_burst_total(void);
int      perf_get_bursts_per_sec(void);
int      perf_get_mag_avg(void);
int      perf_get_mag_peak(void);
uint32_t perf_get_bytes_per_sec(void);
int64_t  perf_get_last_good_us(void);
int64_t  perf_get_last_burst_us(void);
int64_t  perf_get_last_position_us(void);
const uint16_t *perf_history_bursts(void);
const uint16_t *perf_history_good(void);
const uint8_t  *perf_history_mag_avg(void);
}

#include "sdr_ui/sdr_ui.h"

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

LV_IMG_DECLARE(img_app_adsb);

#define COL_BG      SDR_BG
#define COL_PANEL   SDR_PANEL
#define COL_LABEL   SDR_LABEL
#define COL_TEXT    SDR_TEXT
#define COL_BRIGHT  SDR_BRIGHT
#define COL_DIM     SDR_DIM
#define COL_GREEN   SDR_GREEN
#define COL_AMBER   SDR_AMBER
#define COL_RED     SDR_RED
#define COL_CYAN    SDR_CYAN
#define COL_GOLD    SDR_GOLD
#define COL_MAGENTA SDR_MAGENTA

static const char *octant(double deg)
{
    while (deg < 0) deg += 360;
    while (deg >= 360) deg -= 360;
    static const char *o[8] = {"N","NE","E","SE","S","SW","W","NW"};
    return o[((int)((deg + 22.5) / 45.0)) % 8];
}

static void fmt_elapsed(char *out, size_t n, int64_t us)
{
    if (us < 0) us = 0;
    int s = (int)(us / 1000000LL);
    if      (s < 1)    snprintf(out, n, "<1s");
    else if (s < 60)   snprintf(out, n, "%ds", s);
    else if (s < 3600) snprintf(out, n, "%dm%02ds", s / 60, s % 60);
    else               snprintf(out, n, "%dh%02dm", s / 3600, (s / 60) % 60);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    (void)font;
    return sdr_label(parent, sdr_font_mono(), color);
}

static lv_obj_t *make_panel(lv_obj_t *parent)
{
    return sdr_panel(parent);
}

static void adsb_seg_vol(void *, int v) { audio_volume_set(v); }

AppADSB::AppADSB()
    : LsApp("ADS-B", "adsb")
{
}

AppADSB::~AppADSB() = default;

bool AppADSB::init(void)   { return true; }

bool AppADSB::pause(void)  { lakeshark_radio_park(); return true; }

bool AppADSB::resume(void)
{
    lakeshark_select_adsb();
    return true;
}

bool AppADSB::back(void)   { return exitToLauncher(); }

bool AppADSB::close(void)
{
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
    _tabview = nullptr;
    lakeshark_radio_park();
    return true;
}

bool AppADSB::run(lv_obj_t *parent)
{
    lakeshark_select_adsb();

    lv_obj_t *scr = parent;
    sdr_style_screen(scr);

    _tabview = lv_tabview_create(scr, LV_DIR_TOP, 44);
    lv_obj_set_size(_tabview, lv_pct(100), lv_pct(100));
    sdr_style_tabview(_tabview);

    buildListTab(lv_tabview_add_tab(_tabview, "LIST"));
    buildTrackTab(lv_tabview_add_tab(_tabview, "TRACK"));
    buildDiagTab(lv_tabview_add_tab(_tabview, "DIAG"));
    buildRadarTab(lv_tabview_add_tab(_tabview, "RADAR"));
    buildSettingsTab(lv_tabview_add_tab(_tabview, "CONFIG"));

    _timer = lv_timer_create(timerCb, 250, this);
    return true;
}

void AppADSB::buildListTab(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *hdrp = sdr_lcd_panel(parent, SDR_PAS_GOLD);
    _list_hdr = sdr_label(hdrp, &lv_font_montserrat_20, SDR_PAS_GREEN);
    lv_obj_set_width(_list_hdr, lv_pct(100));
    lv_label_set_text(_list_hdr, "AIRCRAFT 0   MSGS 0 (0/s)   CRC 0/0");

    _list_table = lv_table_create(parent);
    lv_obj_set_width(_list_table, lv_pct(100));
    lv_obj_set_flex_grow(_list_table, 1);
    lv_obj_set_scrollbar_mode(_list_table, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_text_font(_list_table, sdr_font_mono_sm(), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(_list_table, COL_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_text_color(_list_table, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_border_width(_list_table, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(_list_table, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(_list_table, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(_list_table, 4, LV_PART_ITEMS);

    lv_obj_set_style_bg_color(_list_table, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_list_table, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(_list_table, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_list_table, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(_list_table, 12, LV_PART_MAIN);

    lv_table_set_col_cnt(_list_table, 9);

    static const int cw[9] = {74, 104, 66, 54, 56, 100, 100, 66, 58};
    for (int c = 0; c < 9; c++) lv_table_set_col_width(_list_table, c, cw[c]);
    lv_table_set_row_cnt(_list_table, 1);
    static const char *hdr[9] = {"ICAO","CALL","ALT","KT","HDG","LAT","LON","V/S","MSG"};
    for (int c = 0; c < 9; c++) lv_table_set_cell_value(_list_table, 0, c, hdr[c]);
    lv_obj_add_event_cb(_list_table, tableClickCb, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 2, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    sdr_btn(row, LV_SYMBOL_PLUS " TEST", testBtnCb, this, nullptr);
    sdr_btn(row, "GAIN", gainBtnCb, this, &_gain_lbl);
    sdr_btn(row, "AGC",  agcBtnCb,  this, nullptr);
}

void AppADSB::updateList(void)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "AIRCRAFT %d    MSGS %d (%d/s)    CRC %d/%d",
             perf_get_active_count(), perf_get_msgs_total(), perf_get_msgs_per_sec(),
             perf_get_crc_good(), perf_get_crc_err());
    lv_label_set_text(_list_hdr, buf);

    if (_gain_lbl) {
        int g = lakeshark_adsb_gain_tenths();
        if (g <= 0) lv_label_set_text(_gain_lbl, "GAIN\nAGC");
        else { char gb[24]; snprintf(gb, sizeof(gb), "GAIN\n%d.%d dB", g / 10, g % 10);
               lv_label_set_text(_gain_lbl, gb); }
    }

    int row = 1;
    for (int i = 0; i < ADSB_MAX_TRACKED && row < 20; i++) {
        const adsb_aircraft_t *a = adsb_state_get(i);
        if (!a || !a->active) continue;

        char icao[10], call[10], alt[10], kt[8], hdg[8], lat[12], lon[12], vs[10], msg[8];
        snprintf(icao, sizeof(icao), "%lX", (unsigned long)a->icao);
        snprintf(call, sizeof(call), "%s", a->callsign[0] ? a->callsign : "--");
        snprintf(alt,  sizeof(alt),  "%d", a->altitude);
        snprintf(kt,   sizeof(kt),   "%d", a->velocity);
        snprintf(hdg,  sizeof(hdg),  "%d", a->heading);
        if (a->pos_valid) { snprintf(lat, sizeof(lat), "%+.3f", a->lat);
                            snprintf(lon, sizeof(lon), "%+.3f", a->lon); }
        else { snprintf(lat, sizeof(lat), "--"); snprintf(lon, sizeof(lon), "--"); }
        if (a->vert_rate > 200)       snprintf(vs, sizeof(vs), "+%d", a->vert_rate);
        else if (a->vert_rate < -200) snprintf(vs, sizeof(vs), "%d", a->vert_rate);
        else                          snprintf(vs, sizeof(vs), "--");
        snprintf(msg, sizeof(msg), "%d", a->msg_count);

        lv_table_set_cell_value(_list_table, row, 0, icao);
        lv_table_set_cell_value(_list_table, row, 1, call);
        lv_table_set_cell_value(_list_table, row, 2, alt);
        lv_table_set_cell_value(_list_table, row, 3, kt);
        lv_table_set_cell_value(_list_table, row, 4, hdg);
        lv_table_set_cell_value(_list_table, row, 5, lat);
        lv_table_set_cell_value(_list_table, row, 6, lon);
        lv_table_set_cell_value(_list_table, row, 7, vs);
        lv_table_set_cell_value(_list_table, row, 8, msg);
        _row_icao[row] = a->icao;
        row++;
    }
    lv_table_set_row_cnt(_list_table, row > 1 ? row : 2);
    if (row == 1) lv_table_set_cell_value(_list_table, 1, 0, "(no aircraft)");
}

void AppADSB::buildTrackTab(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    _trk_hdr = make_label(parent, &lv_font_montserrat_16, COL_BRIGHT);
    lv_label_set_text(_trk_hdr, "No aircraft selected");

    lv_obj_t *p1 = make_panel(parent);
    _trk_pos = make_label(p1, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_width(_trk_pos, lv_pct(100));

    lv_obj_t *p2 = make_panel(parent);
    _trk_motion = make_label(p2, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_width(_trk_motion, lv_pct(100));

    lv_obj_t *p3 = make_panel(parent);
    _trk_qual = make_label(p3, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_width(_trk_qual, lv_pct(100));

    lv_obj_t *p4 = make_panel(parent);
    _trk_types = make_label(p4, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_width(_trk_types, lv_pct(100));

    _trk_chart = lv_chart_create(parent);
    lv_obj_set_size(_trk_chart, lv_pct(100), 90);
    lv_chart_set_type(_trk_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(_trk_chart, 32);
    lv_chart_set_update_mode(_trk_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_bg_color(_trk_chart, COL_PANEL, 0);
    lv_obj_set_style_border_width(_trk_chart, 0, 0);
    _trk_series = lv_chart_add_series(_trk_chart, COL_GREEN, LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_t *nav = lv_obj_create(parent);
    lv_obj_set_size(nav, lv_pct(100), 48);
    lv_obj_set_style_bg_opa(nav, LV_OPA_0, 0);
    lv_obj_set_style_border_width(nav, 0, 0);
    lv_obj_set_style_pad_all(nav, 2, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    sdr_btn(nav, LV_SYMBOL_LEFT " PREV", prevBtnCb, this, nullptr);
    sdr_btn(nav, "NEXT " LV_SYMBOL_RIGHT, nextBtnCb, this, nullptr);
}

void AppADSB::updateTrack(void)
{
    const adsb_aircraft_t *a = adsb_select_get();
    if (!a) {
        lv_label_set_text(_trk_hdr, "No aircraft selected - pick one on the LIST tab");
        lv_label_set_text(_trk_pos, "");
        lv_label_set_text(_trk_motion, "");
        lv_label_set_text(_trk_qual, "");
        lv_label_set_text(_trk_types, "");
        return;
    }

    int64_t now = esp_timer_get_time();
    char first[16], last[16];
    fmt_elapsed(first, sizeof(first), now - a->first_seen_us);
    fmt_elapsed(last,  sizeof(last),  now - a->last_seen_us);

    int idx = -1, total = 0;
    adsb_select_index(&idx, &total);

    char buf[256];
    snprintf(buf, sizeof(buf), "ICAO %06lX   [ %s ]   %d/%d",
             (unsigned long)a->icao, a->callsign[0] ? a->callsign : "--------",
             idx + 1, total);
    lv_label_set_text(_trk_hdr, buf);

    if (a->pos_valid)
        snprintf(buf, sizeof(buf), "POSITION\nLAT  %+10.5f\nLON  %+10.5f", a->lat, a->lon);
    else
        snprintf(buf, sizeof(buf), "POSITION\nLAT  ------\nLON  ------\n(waiting on position)");
    lv_label_set_text(_trk_pos, buf);

    const char *vsarrow = a->vert_rate > 200 ? "^" :
                          a->vert_rate < -200 ? "v" : " ";
    snprintf(buf, sizeof(buf),
             "MOTION\nALT     %6d ft  %s %+d fpm\nSPEED   %6d kt\nHEADING %4d deg %s\nE/W %+d kt   N/S %+d kt",
             a->altitude, vsarrow, a->vert_rate, a->velocity,
             a->heading, octant(a->heading), a->ew_velocity, a->ns_velocity);
    lv_label_set_text(_trk_motion, buf);
    lv_obj_set_style_text_color(_trk_motion,
        a->vert_rate > 200 ? COL_GREEN : a->vert_rate < -200 ? COL_AMBER : COL_TEXT, 0);

    snprintf(buf, sizeof(buf),
             "QUALITY\nFIRST SEEN %s   LAST SEEN %s\nCRC GOOD %d    STATUS %s",
             first, last, a->good_msg_count, a->announced ? "CONFIRMED" : "GATING...");
    lv_label_set_text(_trk_qual, buf);

    snprintf(buf, sizeof(buf),
             "MSG TYPES\nDF11 all-call %u   DF17 pos %u   vel %u\nDF17 ident %u   surv %u   other %u",
             a->mt_df11, a->mt_df17_pos, a->mt_df17_vel,
             a->mt_df17_id, a->mt_surv, a->mt_other);
    lv_label_set_text(_trk_types, buf);

    int n = (int)(sizeof(a->alt_history) / sizeof(a->alt_history[0]));
    for (int s = 0; s < n; s++) {
        int srcidx = (a->alt_history_head + s) % n;
        int16_t v = a->alt_history[srcidx];
        lv_chart_set_value_by_id(_trk_chart, _trk_series, s,
                                 v < 0 ? LV_CHART_POINT_NONE : v);
    }
    lv_chart_refresh(_trk_chart);
}

static const char *mag_state(int v)
{
    if (v < 60)  return "LOW";
    if (v < 180) return "NOMINAL";
    if (v < 220) return "HOT";
    return "SATURATED";
}
static lv_color_t mag_color(int v)
{
    if (v < 60)  return COL_DIM;
    if (v < 180) return COL_GREEN;
    if (v < 220) return COL_AMBER;
    return COL_RED;
}

void AppADSB::buildDiagTab(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    _diag_hdr = make_label(parent, &lv_font_montserrat_14, COL_TEXT);
    lv_label_set_text(_diag_hdr, "RF DIAGNOSTICS");

    lv_obj_t *p1 = make_panel(parent);
    lv_obj_set_flex_flow(p1, LV_FLEX_FLOW_COLUMN);
    _diag_inlbl = make_label(p1, &lv_font_montserrat_14, COL_TEXT);
    lv_label_set_text(_diag_inlbl, "INPUT LEVEL");
    _diag_inbar = lv_bar_create(p1);
    lv_obj_set_size(_diag_inbar, lv_pct(100), 16);
    lv_bar_set_range(_diag_inbar, 0, 255);

    lv_obj_t *clbl = make_label(parent, &lv_font_montserrat_12, COL_LABEL);
    lv_label_set_text(clbl, "DEMOD ACTIVITY (60s)  green=bursts amber=crc cyan=mag");
    _diag_chart = lv_chart_create(parent);
    lv_obj_set_size(_diag_chart, lv_pct(100), 96);
    lv_chart_set_type(_diag_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(_diag_chart, PERF_HISTORY_LEN);
    lv_obj_set_style_bg_color(_diag_chart, COL_PANEL, 0);
    lv_obj_set_style_border_width(_diag_chart, 0, 0);
    lv_obj_set_style_width(_diag_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(_diag_chart, 0, LV_PART_INDICATOR);
    _s_bursts = lv_chart_add_series(_diag_chart, COL_GREEN, LV_CHART_AXIS_PRIMARY_Y);
    _s_good   = lv_chart_add_series(_diag_chart, COL_AMBER, LV_CHART_AXIS_PRIMARY_Y);
    _s_mag    = lv_chart_add_series(_diag_chart, COL_CYAN,  LV_CHART_AXIS_SECONDARY_Y);
    lv_chart_set_range(_diag_chart, LV_CHART_AXIS_SECONDARY_Y, 0, 255);

    lv_obj_t *p2 = make_panel(parent);
    _diag_cum = make_label(p2, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_width(_diag_cum, lv_pct(100));

    lv_obj_t *p3 = make_panel(parent);
    lv_obj_set_flex_flow(p3, LV_FLEX_FLOW_COLUMN);
    _diag_usblbl = make_label(p3, &lv_font_montserrat_14, COL_TEXT);
    lv_label_set_text(_diag_usblbl, "USB DATA PATH");
    _diag_usbbar = lv_bar_create(p3);
    lv_obj_set_size(_diag_usbbar, lv_pct(100), 16);
    lv_bar_set_range(_diag_usbbar, 0, 100);
}

void AppADSB::updateDiag(void)
{
    const app_t *app = app_current();
    uint32_t freq = app ? settings_get_freq(app) : 0;
    int gain = app ? settings_get_gain(app) : 0;
    int up = (int)(esp_timer_get_time() / 1000000LL);

    char buf[200];
    snprintf(buf, sizeof(buf), "RF DIAG   GAIN %d.%d dB   FREQ %lu.%03lu MHz   UP %dm%02ds",
             gain / 10, gain % 10,
             (unsigned long)(freq / 1000000UL), (unsigned long)((freq / 1000UL) % 1000UL),
             up / 60, up % 60);
    lv_label_set_text(_diag_hdr, buf);

    int avg  = clampi(perf_get_mag_avg()  >> 8, 0, 255);
    int peak = clampi(perf_get_mag_peak() >> 8, 0, 255);
    lv_bar_set_value(_diag_inbar, avg, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_diag_inbar, mag_color(avg), LV_PART_INDICATOR);
    snprintf(buf, sizeof(buf), "INPUT LEVEL   AVG %d   PEAK %d   %s", avg, peak, mag_state(avg));
    lv_label_set_text(_diag_inlbl, buf);

    const uint16_t *hb = perf_history_bursts();
    const uint16_t *hg = perf_history_good();
    const uint8_t  *hm = perf_history_mag_avg();
    int ceil = 4;
    for (int i = 0; i < PERF_HISTORY_LEN; i++) {
        if (hb[i] > ceil) ceil = hb[i];
        if (hg[i] > ceil) ceil = hg[i];
    }
    lv_chart_set_range(_diag_chart, LV_CHART_AXIS_PRIMARY_Y, 0, ceil);
    for (int i = 0; i < PERF_HISTORY_LEN; i++) {
        lv_chart_set_value_by_id(_diag_chart, _s_bursts, i, hb[i]);
        lv_chart_set_value_by_id(_diag_chart, _s_good,   i, hg[i]);
        lv_chart_set_value_by_id(_diag_chart, _s_mag,    i, hm[i]);
    }
    lv_chart_refresh(_diag_chart);

    int cg = perf_get_crc_good(), cb = perf_get_crc_err();
    int pct = (cg + cb > 0) ? (100 * cg) / (cg + cb) : 0;
    int64_t now = esp_timer_get_time();
    char eb[16], eg[16], ep[16];
    int64_t lb = perf_get_last_burst_us(), lg = perf_get_last_good_us(), lp = perf_get_last_position_us();
    if (lb) fmt_elapsed(eb, sizeof(eb), now - lb); else snprintf(eb, sizeof(eb), "never");
    if (lg) fmt_elapsed(eg, sizeof(eg), now - lg); else snprintf(eg, sizeof(eg), "never");
    if (lp) fmt_elapsed(ep, sizeof(ep), now - lp); else snprintf(ep, sizeof(ep), "never");
    snprintf(buf, sizeof(buf),
             "CUMULATIVE\nPREAMBLES %d   CRC %d/%d   OK %d%%\nBURSTS/s %d   CRC OK/s %d\nLAST burst %s  good %s  pos %s",
             perf_get_burst_total(), cg, cb, pct,
             perf_get_bursts_per_sec(), perf_get_msgs_per_sec(), eb, eg, ep);
    lv_label_set_text(_diag_cum, buf);

    uint32_t bps = perf_get_bytes_per_sec();
    int upct = clampi((int)(bps / 40000UL), 0, 100);
    lv_bar_set_value(_diag_usbbar, upct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_diag_usbbar,
        upct >= 90 ? COL_GREEN : upct >= 50 ? COL_AMBER : COL_RED, LV_PART_INDICATOR);
    snprintf(buf, sizeof(buf), "USB %lu.%02lu MB/s   %s",
             (unsigned long)(bps / 1000000UL), (unsigned long)((bps / 10000UL) % 100UL),
             upct >= 90 ? "HEALTHY" : upct >= 50 ? "DEGRADED" : "STALLED");
    lv_label_set_text(_diag_usblbl, buf);
}

void AppADSB::buildRadarTab(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    _radar_hdr = sdr_label(parent, sdr_font_mono(), SDR_CYAN);
    lv_label_set_text(_radar_hdr, "RADAR");

    const int SZ = 460;
    lv_obj_t *scope = lv_obj_create(parent);
    _radar_scope = scope;
    lv_obj_set_size(scope, SZ, SZ);
    lv_obj_set_style_bg_color(scope, SDR_PANEL, 0);
    lv_obj_set_style_bg_opa(scope, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(scope, 2, 0);
    lv_obj_set_style_border_color(scope, SDR_BORDER, 0);
    lv_obj_set_style_border_width(scope, 1, 0);
    lv_obj_set_style_pad_all(scope, 0, 0);
    lv_obj_clear_flag(scope, LV_OBJ_FLAG_SCROLLABLE);

    auto deco = [](lv_obj_t *o) {
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    };

    int radii[3] = { SZ / 6, SZ / 3, SZ / 2 - 4 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *ring = lv_obj_create(scope);
        lv_obj_set_size(ring, radii[i] * 2, radii[i] * 2);
        lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_0, 0);
        lv_obj_set_style_border_color(ring, SDR_BORDER, 0);
        lv_obj_set_style_border_opa(ring, LV_OPA_40, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *hl = lv_obj_create(scope);
    lv_obj_set_size(hl, SZ - 8, 1); lv_obj_align(hl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(hl, SDR_BORDER, 0); lv_obj_set_style_bg_opa(hl, LV_OPA_40, 0);
    deco(hl);
    lv_obj_t *vl = lv_obj_create(scope);
    lv_obj_set_size(vl, 1, SZ - 8); lv_obj_align(vl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(vl, SDR_BORDER, 0); lv_obj_set_style_bg_opa(vl, LV_OPA_40, 0);
    deco(vl);

    lv_obj_t *nlab = sdr_label(scope, sdr_font_mono_sm(), SDR_DIM);
    lv_label_set_text(nlab, "N"); lv_obj_align(nlab, LV_ALIGN_TOP_MID, 0, 3);

    lv_obj_t *ctr = lv_obj_create(scope);
    lv_obj_set_size(ctr, 8, 8); lv_obj_align(ctr, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(ctr, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ctr, SDR_AMBER, 0);
    deco(ctr);

    for (int i = 0; i < RADAR_MAX; i++) {
        lv_obj_t *d = lv_obj_create(scope);
        lv_obj_set_size(d, 9, 9);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, SDR_GREEN, 0);
        deco(d);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        _radar_dot[i] = d;

        lv_obj_t *l = sdr_label(scope, sdr_font_mono_sm(), SDR_TEXT);
        lv_label_set_text(l, "");
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        _radar_lbl[i] = l;
    }
}

void AppADSB::updateRadar(void)
{
    float home_lat = 0, home_lon = 0;
    if (!settings_get_home(&home_lat, &home_lon)) {
        lv_label_set_text(_radar_hdr,
            "RADAR   set home location:  home <lat> <lon>");
        for (int i = 0; i < RADAR_MAX; i++) {
            lv_obj_add_flag(_radar_dot[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(_radar_lbl[i], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    const double DEG2NM = 60.0;
    double clat = cos((double)home_lat * 0.017453292519943295);

    double xs[RADAR_MAX], ys[RADAR_MAX], rr[RADAR_MAX];
    const char *cs[RADAR_MAX];
    uint32_t ic[RADAR_MAX];
    int n = 0;
    double maxr = 0;
    for (int i = 0; i < ADSB_MAX_TRACKED && n < RADAR_MAX; i++) {
        const adsb_aircraft_t *a = adsb_state_get(i);
        if (!a || !a->active || !a->pos_valid) continue;
        double x = ((double)a->lon - home_lon) * clat * DEG2NM;
        double y = ((double)a->lat - home_lat) * DEG2NM;
        xs[n] = x; ys[n] = y;
        rr[n] = sqrt(x * x + y * y);
        if (rr[n] > maxr) maxr = rr[n];
        ic[n] = a->icao;
        cs[n] = a->callsign[0] ? a->callsign : nullptr;
        n++;
    }

    if (n == 0) {
        lv_label_set_text(_radar_hdr, "RADAR   (no positioned aircraft in range)");
        for (int i = 0; i < RADAR_MAX; i++) {
            lv_obj_add_flag(_radar_dot[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(_radar_lbl[i], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    static const double RINGS[] = { 10, 25, 50, 100, 200, 400 };
    const int NRINGS = (int)(sizeof(RINGS) / sizeof(RINGS[0]));
    double range = RINGS[NRINGS - 1];
    for (int i = 0; i < NRINGS; i++) {
        if (maxr <= RINGS[i]) { range = RINGS[i]; break; }
    }

    const double OUT_PX = 460 / 2 - 14;
    double scale = OUT_PX / range;
    for (int i = 0; i < n; i++) {
        double x = xs[i], y = ys[i], r = rr[i];
        if (r > range && r > 0) { double k = range / r; x *= k; y *= k; }
        int dx = (int)(x * scale);
        int dy = (int)(-y * scale);
        lv_obj_align(_radar_dot[i], LV_ALIGN_CENTER, dx, dy);
        lv_obj_clear_flag(_radar_dot[i], LV_OBJ_FLAG_HIDDEN);
        char tag[12];
        if (cs[i]) snprintf(tag, sizeof(tag), "%s", cs[i]);
        else       snprintf(tag, sizeof(tag), "%lX", (unsigned long)ic[i]);
        lv_label_set_text(_radar_lbl[i], tag);
        lv_obj_align(_radar_lbl[i], LV_ALIGN_CENTER, dx + 9, dy - 7);
        lv_obj_clear_flag(_radar_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = n; i < RADAR_MAX; i++) {
        lv_obj_add_flag(_radar_dot[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_radar_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }
    char hdr[88];
    snprintf(hdr, sizeof(hdr), "RADAR   %d aircraft   range %.0f nm   home %.3f,%.3f",
             n, range, (double)home_lat, (double)home_lon);
    lv_label_set_text(_radar_hdr, hdr);
}

void AppADSB::buildSettingsTab(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 8, 0);
    lv_obj_set_style_pad_row(parent, 6, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    sdr_setrow_t r;

    sdr_section(parent, "RADIO");

    sdr_setting_row(parent, "FREQUENCY", &r);
    lv_label_set_text(r.value, "1090.000 MHz");
    lv_obj_t *fx = sdr_label(r.controls, sdr_font_mono(), SDR_DIM);
    lv_label_set_text(fx, "FIXED");

    sdr_setting_row(parent, "GAIN", &r);
    _set_gain_val = r.value;
    sdr_btn(r.controls, "STEP", setGainStepCb, this, nullptr);
    sdr_btn(r.controls, "AGC",  setAgcCb,      this, nullptr);

    sdr_section(parent, "VOICE (SAM)");

    sdr_setting_row(parent, "PRESET", &r);
    _set_preset_val = r.value;
    sdr_btn(r.controls, "<", presetLeftCb,  this, nullptr);
    sdr_btn(r.controls, ">", presetRightCb, this, nullptr);

    sdr_setting_row(parent, "LOW-PASS", &r);
    _set_lp_val = r.value;
    sdr_btn(r.controls, "<", lpLeftCb,  this, nullptr);
    sdr_btn(r.controls, ">", lpRightCb, this, nullptr);

    sdr_setting_row(parent, "LOW-SHELF", &r);
    _set_shelf_val = r.value;
    sdr_btn(r.controls, "<", shelfLeftCb,  this, nullptr);
    sdr_btn(r.controls, ">", shelfRightCb, this, nullptr);

    sdr_setting_row(parent, "VOICE TEST", &r);
    lv_label_set_text(r.value, "");
    sdr_btn(r.controls, "SPEAK", voiceTestCb, this, nullptr);

    sdr_section(parent, "CALLOUTS  (off / beep / voice)");

    sdr_setting_row(parent, "NEW CONTACT", &r);
    _set_new_val = r.value;
    sdr_btn(r.controls, "CYCLE", newCycleCb, this, nullptr);

    sdr_setting_row(parent, "LOST", &r);
    _set_lost_val = r.value;
    sdr_btn(r.controls, "CYCLE", lostCycleCb, this, nullptr);

    sdr_setting_row(parent, "POSITION", &r);
    _set_pos_val = r.value;
    sdr_btn(r.controls, "CYCLE", posCycleCb, this, nullptr);

    sdr_section(parent, "AUDIO");

    _set_vol_slider = sdr_seg_slider(parent, SDR_PAS_CYAN, 100, audio_volume_get(),
                                     adsb_seg_vol, this, &_set_vol_val);

    sdr_setting_row(parent, "MUTE", &r);
    _set_mute_val = r.value;
    sdr_btn(r.controls, "TOGGLE", muteCb, this, nullptr);

    sdr_section(parent, "OUTPUT  (CartoTUI on a host PC)");

    sdr_setting_row(parent, "CARTOTUI FEED", &r);
    _set_carto_val = r.value;
    sdr_btn(r.controls, "TOGGLE", cartoCb, this, nullptr);

    updateSettings();
}

void AppADSB::updateSettings(void)
{
    char b[24];
    if (_set_gain_val) {
        int g = lakeshark_adsb_gain_tenths();
        if (g <= 0) lv_label_set_text(_set_gain_val, "AGC");
        else { snprintf(b, sizeof(b), "%d.%d dB", g / 10, g % 10);
               lv_label_set_text(_set_gain_val, b); }
    }
    if (_set_preset_val)
        lv_label_set_text(_set_preset_val,
            sam_tts_preset_name((sam_tts_voice_preset_t)settings_voice_preset_get()));
    if (_set_lp_val)
        lv_label_set_text(_set_lp_val, sam_tts_lowpass_name(settings_voice_lowpass_get()));
    if (_set_shelf_val)
        lv_label_set_text(_set_shelf_val, sam_tts_lowshelf_name(settings_voice_lowshelf_get()));
    if (_set_new_val)
        lv_label_set_text(_set_new_val,
            audio_mode_label(audio_event_mode_get(AUDIO_EVT_NEW_CONTACT)));
    if (_set_lost_val)
        lv_label_set_text(_set_lost_val,
            audio_mode_label(audio_event_mode_get(AUDIO_EVT_LOST_CONTACT)));
    if (_set_pos_val)
        lv_label_set_text(_set_pos_val,
            audio_mode_label(audio_event_mode_get(AUDIO_EVT_POSITION)));
    if (_set_vol_val) {
        snprintf(b, sizeof(b), "VOLUME  %d", audio_volume_get());
        lv_label_set_text(_set_vol_val, b);
    }
    sdr_seg_set(_set_vol_slider, audio_volume_get());
    if (_set_mute_val)
        lv_label_set_text(_set_mute_val, audio_is_muted() ? "MUTED" : "ON");
    if (_set_carto_val)
        lv_label_set_text(_set_carto_val, lakeshark_cartotui_enabled() ? "ON" : "OFF");
}

static void adsb_cycle_preset(int dir)
{
    int p = (settings_voice_preset_get() + SAM_PRESET_COUNT + dir) % SAM_PRESET_COUNT;
    settings_voice_preset_set(p);
    sam_tts_set_preset((sam_tts_voice_preset_t)p);
}
static void adsb_cycle_lp(int dir)
{
    int m = (settings_voice_lowpass_get() + 3 + dir) % 3;
    settings_voice_lowpass_set(m);
    sam_tts_set_lowpass(m);
}
static void adsb_cycle_shelf(int dir)
{
    int m = (settings_voice_lowshelf_get() + 3 + dir) % 3;
    settings_voice_lowshelf_set(m);
    sam_tts_set_lowshelf(m);
}

void AppADSB::setGainStepCb(lv_event_t *) { lakeshark_adsb_gain_step(); }
void AppADSB::setAgcCb(lv_event_t *)      { lakeshark_adsb_agc(); }
void AppADSB::presetLeftCb(lv_event_t *)  { adsb_cycle_preset(-1); }
void AppADSB::presetRightCb(lv_event_t *) { adsb_cycle_preset(+1); }
void AppADSB::lpLeftCb(lv_event_t *)      { adsb_cycle_lp(-1); }
void AppADSB::lpRightCb(lv_event_t *)     { adsb_cycle_lp(+1); }
void AppADSB::shelfLeftCb(lv_event_t *)   { adsb_cycle_shelf(-1); }
void AppADSB::shelfRightCb(lv_event_t *)  { adsb_cycle_shelf(+1); }
void AppADSB::voiceTestCb(lv_event_t *)   { audio_out_ensure_unmuted(); audio_events_play_test(); }
void AppADSB::newCycleCb(lv_event_t *)    { audio_event_mode_cycle(AUDIO_EVT_NEW_CONTACT); }
void AppADSB::lostCycleCb(lv_event_t *)   { audio_event_mode_cycle(AUDIO_EVT_LOST_CONTACT); }
void AppADSB::posCycleCb(lv_event_t *)    { audio_event_mode_cycle(AUDIO_EVT_POSITION); }
void AppADSB::volDownCb(lv_event_t *)     { audio_volume_delta(-5); }
void AppADSB::volUpCb(lv_event_t *)       { audio_volume_delta(+5); }
void AppADSB::muteCb(lv_event_t *)        { audio_toggle_mute(); }
void AppADSB::volSliderCb(lv_event_t *e)
{
    audio_volume_set((int)lv_slider_get_value(lv_event_get_target(e)));
}
void AppADSB::cartoCb(lv_event_t *)       { lakeshark_cartotui_set_enabled(!lakeshark_cartotui_enabled()); }

void AppADSB::switchTab(int delta)
{
    if (!_tabview) return;
    const int N = 5;
    int cur = (int)lv_tabview_get_tab_act(_tabview);
    lv_tabview_set_act(_tabview, (cur + delta + N) % N, LV_ANIM_OFF);
}

void AppADSB::timerCb(lv_timer_t *t)
{
    AppADSB *self = static_cast<AppADSB *>(t->user_data);
    if (!self->_tabview) return;
    switch (lv_tabview_get_tab_act(self->_tabview)) {
        case 0: self->updateList();     break;
        case 1: self->updateTrack();    break;
        case 2: self->updateDiag();     break;
        case 3: self->updateRadar();    break;
        case 4: self->updateSettings(); break;
        default: break;
    }
}

void AppADSB::tableClickCb(lv_event_t *e)
{
    AppADSB *self = static_cast<AppADSB *>(lv_event_get_user_data(e));
    uint16_t row, col;
    lv_table_get_selected_cell(self->_list_table, &row, &col);
    if (row >= 1 && row < 20 && self->_row_icao[row]) {
        adsb_select_set_icao(self->_row_icao[row]);
        lv_tabview_set_act(self->_tabview, 1, LV_ANIM_ON);
    }
}

void AppADSB::testBtnCb(lv_event_t *e)
{
    (void)e;
    adsb_inject_fake_aircraft();
}

void AppADSB::prevBtnCb(lv_event_t *e)
{
    (void)e;
    (void)adsb_select_get();
    adsb_select_prev();
}

void AppADSB::nextBtnCb(lv_event_t *e)
{
    (void)e;
    (void)adsb_select_get();
    adsb_select_next();
}

void AppADSB::gainBtnCb(lv_event_t *e)
{
    (void)e;
    lakeshark_adsb_gain_step();
}

void AppADSB::agcBtnCb(lv_event_t *e)
{
    (void)e;
    lakeshark_adsb_agc();
}
