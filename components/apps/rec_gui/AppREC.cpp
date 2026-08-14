#include "rec_gui/AppREC.hpp"

#include <stdio.h>
#include <string.h>

#include "sdr_ui/sdr_ui.h"
#include "shell/ls_shell.hpp"

extern "C" {
#include "lakeshark_backend.h"
#include "rec_state.h"
}

/*LS-020*/

#define COL_BG    SDR_BG
#define COL_PANEL SDR_PANEL
#define COL_TEXT  SDR_TEXT

/*LS-020*/
#define MAG_FULL_SCALE 256

struct rec_preset_t { const char *name; uint32_t hz; };

/*LS-020*/
static const rec_preset_t REC_PRESETS[] = {
    { "OOK 433.92", 433920000UL },
    { "OOK 434.42", 434420000UL },
    { "OOK 315.00", 315000000UL },
    { "OOK 868.35", 868350000UL },
    { "FSK 432.80", 432800000UL },
};
#define N_PRESETS ((int)(sizeof(REC_PRESETS) / sizeof(REC_PRESETS[0])))

/*LS-023*/
static void rec_ascii_bar(char *out, size_t outsz, int pct, int width)
{
    if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
    int fill = (pct * width + 50) / 100;
    size_t n = 0;
    if (n < outsz - 1) out[n++] = '[';
    for (int i = 0; i < width && n < outsz - 2; i++)
        out[n++] = (i < fill) ? '|' : '.';
    if (n < outsz - 1) out[n++] = ']';
    out[n] = 0;
}

static const char *phase_name(int p)
{
    switch (p) {
    case REC_ARMED:     return "ARMED";
    case REC_CAPTURING: return "CAPTURING";
    case REC_DONE:      return "DONE";
    default:            return "IDLE";
    }
}

static lv_color_t phase_color(int p)
{
    switch (p) {
    case REC_ARMED:     return SDR_PAS_AMBER;
    case REC_CAPTURING: return SDR_PAS_ROSE;
    case REC_DONE:      return SDR_PAS_GREEN;
    default:            return SDR_DIM;
    }
}

AppREC::AppREC()
    : LsApp("REC", "rec")
{
}

AppREC::~AppREC() = default;

bool AppREC::init(void)  { return true; }

bool AppREC::pause(void) { lakeshark_radio_park(); return true; }

bool AppREC::resume(void)
{
    lakeshark_select_rec();
    return true;
}

bool AppREC::back(void)  { return exitToLauncher(); }

bool AppREC::close(void)
{
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
    _tabview     = nullptr;
    _files_table = nullptr;
    lakeshark_radio_park();
    return true;
}

void AppREC::switchTab(int delta)
{
    if (!_tabview) return;
    int cur = (int)lv_tabview_get_tab_act(_tabview);
    cur = (cur + delta + 3) % 3;
    lv_tabview_set_act(_tabview, (uint16_t)cur, LV_ANIM_OFF);
}

bool AppREC::run(lv_obj_t *parent)
{
    lakeshark_select_rec();

    sdr_style_screen(parent);

    _tabview = lv_tabview_create(parent, LV_DIR_TOP, 44);
    lv_obj_set_size(_tabview, lv_pct(100), lv_pct(100));
    sdr_style_tabview(_tabview);

    buildRecordTab(lv_tabview_add_tab(_tabview, "RECORD"));
    buildConfigTab(lv_tabview_add_tab(_tabview, "CONFIG"));
    buildFilesTab(lv_tabview_add_tab(_tabview, "FILES"));

    _timer = lv_timer_create(timerCb, 250, this);
    return true;
}

void AppREC::timerCb(lv_timer_t *t)
{
    AppREC *self = (AppREC *)t->user_data;
    self->updateRecord();
    self->updateConfig();
}

void AppREC::buildRecordTab(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_row(parent, 6, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *hdrp = sdr_lcd_panel(parent, SDR_PAS_GOLD);
    _rec_hdr = sdr_label(hdrp, &lv_font_montserrat_20, SDR_PAS_GREEN);
    lv_obj_set_width(_rec_hdr, lv_pct(100));
    lv_label_set_text(_rec_hdr, "IDLE");

    sdr_section(parent, "CARRIER  (does it lift when the gear transmits?)");

    /*LS-023*/
    _rec_magbar = sdr_label(parent, sdr_font_mono(), SDR_PAS_CYAN);
    lv_obj_set_width(_rec_magbar, lv_pct(100));
    lv_label_set_text(_rec_magbar, "");

    _rec_maglbl = sdr_label(parent, sdr_font_mono(), SDR_TEXT);
    lv_obj_set_width(_rec_maglbl, lv_pct(100));
    lv_label_set_text(_rec_maglbl, "mag 0  floor 0  thr 0");

    sdr_section(parent, "CAPTURE");

    _rec_stats = sdr_label(parent, sdr_font_mono(), SDR_TEXT);
    lv_obj_set_width(_rec_stats, lv_pct(100));
    lv_label_set_text(_rec_stats, "");

    _rec_result = sdr_label(parent, sdr_font_mono(), SDR_DIM);
    lv_obj_set_width(_rec_result, lv_pct(100));
    lv_label_set_text(_rec_result, "");

    _rec_file = sdr_label(parent, sdr_font_mono(), SDR_PAS_GOLD);
    lv_obj_set_width(_rec_file, lv_pct(100));
    lv_label_set_text(_rec_file, "");

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 52);
    lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 2, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    sdr_btn(row, "ARM",  armCb,  this, nullptr);
    sdr_btn(row, "STOP", stopCb, this, nullptr);
    sdr_btn(row, "SAVE", saveCb, this, nullptr);

    updateRecord();
}

void AppREC::updateRecord(void)
{
    if (!_rec_hdr) return;

    rec_status_t st;
    rec_get_status(&st);

    char b[192];

    snprintf(b, sizeof(b), "%s   %.4f MHz   %d.%d dB",
             phase_name(st.phase), st.freq_hz / 1e6,
             st.gain_tenths / 10, st.gain_tenths % 10);
    lv_label_set_text(_rec_hdr, b);
    lv_obj_set_style_text_color(_rec_hdr, phase_color(st.phase), 0);

    /*LS-023*/
    int mag = st.mag_now;
    if (mag < 0)              mag = 0;
    if (mag > MAG_FULL_SCALE) mag = MAG_FULL_SCALE;
    {
        char bar[96];
        rec_ascii_bar(bar, sizeof(bar), (mag * 100 + MAG_FULL_SCALE / 2) / MAG_FULL_SCALE,
                      sdr_bar_width(_rec_magbar, 9));
        lv_label_set_text_fmt(_rec_magbar, "M %s %3d%%", bar,
                              (mag * 100 + MAG_FULL_SCALE / 2) / MAG_FULL_SCALE);
        lv_obj_set_style_text_color(_rec_magbar,
            (st.mag_thresh > 0 && st.mag_now >= st.mag_thresh) ? SDR_PAS_GREEN
                                                               : SDR_PAS_CYAN, 0);
    }

    snprintf(b, sizeof(b), "mag %-5d floor %-5d thr %-5d %s",
             st.mag_now, st.mag_floor, st.mag_thresh,
             st.thresh_fixed ? "FIXED" : "AUTO");
    lv_label_set_text(_rec_maglbl, b);

    snprintf(b, sizeof(b),
             "edges   %4d / %d\n"
             "span    %lu.%03lu ms\n"
             "captures %lu      %lu B/s",
             st.edges, REC_MAX_EDGES,
             (unsigned long)(st.span_us / 1000),
             (unsigned long)(st.span_us % 1000),
             (unsigned long)st.captures, (unsigned long)st.bytes_sec);
    lv_label_set_text(_rec_stats, b);

    if (st.end_reason != REC_END_NONE) {
        snprintf(b, sizeof(b), "ended: %s   mark %lu-%lu us   baud~%lu",
                 rec_end_reason_name(st.end_reason),
                 (unsigned long)st.min_mark_us, (unsigned long)st.max_mark_us,
                 (unsigned long)st.baud_est);
        lv_label_set_text(_rec_result, b);
    } else {
        lv_label_set_text(_rec_result, "");
    }

    if (st.last_file[0]) {
        snprintf(b, sizeof(b), "last: %s", st.last_file);
        lv_label_set_text(_rec_file, b);
    }
}

void AppREC::armCb(lv_event_t *e)
{
    AppREC *self = (AppREC *)lv_event_get_user_data(e);
    /*LS-506*/
    rec_arm_request();
    self->updateRecord();
}

void AppREC::stopCb(lv_event_t *e)
{
    AppREC *self = (AppREC *)lv_event_get_user_data(e);
    rec_disarm();
    self->updateRecord();
}

void AppREC::saveCb(lv_event_t *e)
{
    AppREC *self = (AppREC *)lv_event_get_user_data(e);

    rec_status_t st;
    rec_get_status(&st);

    char name[24], path[80], msg[128];
    snprintf(name, sizeof(name), "rec%03lu", (unsigned long)(st.captures));

    /*LS-513*/
    int r = rec_save(name, path, sizeof(path));
    if (r < 0) {
        const char *why = (r == -3) ? "still capturing"
                        : (r == -1) ? "nothing captured yet"
                                    : "cannot open the file";
        snprintf(msg, sizeof(msg), "save failed: %s", why);
        lv_label_set_text(self->_rec_file, msg);
        lv_obj_set_style_text_color(self->_rec_file, SDR_RED, 0);
        return;
    }

    snprintf(msg, sizeof(msg), "saved: %s", path);
    lv_label_set_text(self->_rec_file, msg);
    lv_obj_set_style_text_color(self->_rec_file, SDR_PAS_GOLD, 0);
    self->refreshFiles();
}

void AppREC::buildConfigTab(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 8, 0);
    lv_obj_set_style_pad_row(parent, 6, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    sdr_setrow_t r;

    sdr_section(parent, "TUNING");

    sdr_setting_row(parent, "FREQUENCY", &r);
    _cfg_freq = r.value;
    sdr_btn(r.controls, "<", freqDownCb, this, nullptr);
    sdr_btn(r.controls, ">", freqUpCb,   this, nullptr);

    sdr_setting_row(parent, "COARSE  1 MHz", &r);
    lv_label_set_text(r.value, "");
    sdr_btn(r.controls, "<<", freqCoarseDownCb, this, nullptr);
    sdr_btn(r.controls, ">>", freqCoarseUpCb,   this, nullptr);

    sdr_setting_row(parent, "PRESET", &r);
    _cfg_preset = r.value;
    sdr_btn(r.controls, "CYCLE", presetCb, this, nullptr);

    sdr_setting_row(parent, "GAIN", &r);
    _cfg_gain = r.value;
    sdr_btn(r.controls, "<", gainDownCb, this, nullptr);
    sdr_btn(r.controls, ">", gainUpCb,   this, nullptr);

    /*LS-516*/
    sdr_setting_row(parent, "BANDWIDTH", &r);
    _cfg_bw = r.value;
    sdr_btn(r.controls, "<", bwDownCb, this, nullptr);
    sdr_btn(r.controls, ">", bwUpCb,   this, nullptr);

    sdr_section(parent, "DETECTOR");

    /*LS-503*/
    sdr_setting_row(parent, "THRESHOLD", &r);
    _cfg_thresh = r.value;
    sdr_btn(r.controls, "<",    threshDownCb, this, nullptr);
    sdr_btn(r.controls, ">",    threshUpCb,   this, nullptr);
    sdr_btn(r.controls, "AUTO", threshAutoCb, this, nullptr);

    /*LS-504*/
    sdr_setting_row(parent, "END GAP", &r);
    _cfg_gap = r.value;
    sdr_btn(r.controls, "<", gapDownCb, this, nullptr);
    sdr_btn(r.controls, ">", gapUpCb,   this, nullptr);

    sdr_setting_row(parent, "MIN PULSE", &r);
    _cfg_minpul = r.value;
    sdr_btn(r.controls, "<", minPulseDownCb, this, nullptr);
    sdr_btn(r.controls, ">", minPulseUpCb,   this, nullptr);

    sdr_setting_row(parent, "MAX SPAN", &r);
    _cfg_maxspan = r.value;
    sdr_btn(r.controls, "<", maxSpanDownCb, this, nullptr);
    sdr_btn(r.controls, ">", maxSpanUpCb,   this, nullptr);

    sdr_setting_row(parent, "MIN EDGES", &r);
    _cfg_minedg = r.value;
    sdr_btn(r.controls, "<", minEdgesDownCb, this, nullptr);
    sdr_btn(r.controls, ">", minEdgesUpCb,   this, nullptr);

    updateConfig();
}

void AppREC::updateConfig(void)
{
    if (!_cfg_freq) return;

    char b[48];
    uint32_t hz = rec_get_freq();

    snprintf(b, sizeof(b), "%.4f MHz", hz / 1e6);
    lv_label_set_text(_cfg_freq, b);

    int match = -1;
    for (int i = 0; i < N_PRESETS; i++)
        if (REC_PRESETS[i].hz == hz) { match = i; break; }
    lv_label_set_text(_cfg_preset, match >= 0 ? REC_PRESETS[match].name : "custom");

    rec_status_t st;
    rec_get_status(&st);
    snprintf(b, sizeof(b), "%d.%d dB", st.gain_tenths / 10, st.gain_tenths % 10);
    lv_label_set_text(_cfg_gain, b);

    uint32_t bw = rec_get_bw();
    if (bw == 0) lv_label_set_text(_cfg_bw, "auto");
    else { snprintf(b, sizeof(b), "%lu kHz", (unsigned long)(bw / 1000)); lv_label_set_text(_cfg_bw, b); }

    int th = rec_get_thresh();
    if (th <= 0) lv_label_set_text(_cfg_thresh, "auto");
    else { snprintf(b, sizeof(b), "%d", th); lv_label_set_text(_cfg_thresh, b); }

    snprintf(b, sizeof(b), "%d ms", rec_get_gap_ms());
    lv_label_set_text(_cfg_gap, b);

    snprintf(b, sizeof(b), "%lu us", (unsigned long)rec_get_min_pulse());
    lv_label_set_text(_cfg_minpul, b);

    /*LS-514*/
    snprintf(b, sizeof(b), "%lu ms", (unsigned long)(rec_get_max_span() / 1000));
    lv_label_set_text(_cfg_maxspan, b);

    snprintf(b, sizeof(b), "%d", rec_get_min_edges());
    lv_label_set_text(_cfg_minedg, b);
}

static void freq_nudge(int32_t delta_hz)
{
    int64_t hz = (int64_t)rec_get_freq() + delta_hz;
    if (hz < 24000000LL)   hz = 24000000LL;
    if (hz > 1766000000LL) hz = 1766000000LL;
    rec_set_freq((uint32_t)hz);
}

void AppREC::freqDownCb(lv_event_t *e)
{
    freq_nudge(-10000);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::freqUpCb(lv_event_t *e)
{
    freq_nudge(10000);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::freqCoarseDownCb(lv_event_t *e)
{
    freq_nudge(-1000000);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::freqCoarseUpCb(lv_event_t *e)
{
    freq_nudge(1000000);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::presetCb(lv_event_t *e)
{
    AppREC *self = (AppREC *)lv_event_get_user_data(e);
    self->_preset = (self->_preset + 1) % N_PRESETS;
    rec_set_freq(REC_PRESETS[self->_preset].hz);
    self->updateConfig();
}

void AppREC::gainDownCb(lv_event_t *e)
{
    rec_status_t st; rec_get_status(&st);
    int g = st.gain_tenths - 10;
    if (g < 0) g = 0;
    rec_set_gain(g);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::gainUpCb(lv_event_t *e)
{
    rec_status_t st; rec_get_status(&st);
    int g = st.gain_tenths + 10;
    if (g > 496) g = 496;
    rec_set_gain(g);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::threshDownCb(lv_event_t *e)
{
    int t = rec_get_thresh() - 8;
    if (t < 0) t = 0;
    rec_set_thresh(t);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::threshUpCb(lv_event_t *e)
{
    int t = rec_get_thresh() + 8;
    if (t > MAG_FULL_SCALE) t = MAG_FULL_SCALE;
    rec_set_thresh(t);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::threshAutoCb(lv_event_t *e)
{
    rec_set_thresh(0);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::gapDownCb(lv_event_t *e)
{
    int g = rec_get_gap_ms() - 5;
    if (g < 1) g = 1;
    rec_set_gap_ms(g);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::gapUpCb(lv_event_t *e)
{
    int g = rec_get_gap_ms() + 5;
    if (g > 1000) g = 1000;
    rec_set_gap_ms(g);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::bwDownCb(lv_event_t *e)
{
    uint32_t bw = rec_get_bw();
    bw = (bw <= 50000) ? 0 : bw - 50000;
    rec_set_bw(bw);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::bwUpCb(lv_event_t *e)
{
    uint32_t bw = rec_get_bw() + 50000;
    if (bw > 1000000) bw = 1000000;
    rec_set_bw(bw);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::minPulseDownCb(lv_event_t *e)
{
    uint32_t p = rec_get_min_pulse();
    p = (p <= 10) ? 1 : p - 10;
    rec_set_min_pulse(p);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::minPulseUpCb(lv_event_t *e)
{
    uint32_t p = rec_get_min_pulse() + 10;
    if (p > 2000) p = 2000;
    rec_set_min_pulse(p);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::maxSpanDownCb(lv_event_t *e)
{
    uint32_t s = rec_get_max_span();
    s = (s <= 500000) ? 100000 : s - 500000;
    rec_set_max_span(s);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::maxSpanUpCb(lv_event_t *e)
{
    uint32_t s = rec_get_max_span() + 500000;
    if (s > 30000000UL) s = 30000000UL;
    rec_set_max_span(s);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::minEdgesDownCb(lv_event_t *e)
{
    int n = rec_get_min_edges() - 2;
    if (n < 2) n = 2;
    rec_set_min_edges(n);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::minEdgesUpCb(lv_event_t *e)
{
    int n = rec_get_min_edges() + 2;
    if (n > REC_MAX_EDGES) n = REC_MAX_EDGES;
    rec_set_min_edges(n);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

void AppREC::buildFilesTab(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_row(parent, 6, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    _files_note = sdr_label(parent, sdr_font_mono(), SDR_DIM);
    lv_obj_set_width(_files_note, lv_pct(100));
    lv_label_set_text(_files_note, "");

    _files_table = lv_table_create(parent);
    lv_obj_set_width(_files_table, lv_pct(100));
    lv_obj_set_flex_grow(_files_table, 1);
    lv_obj_set_scrollbar_mode(_files_table, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_text_font(_files_table, sdr_font_mono_sm(), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(_files_table, COL_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_text_color(_files_table, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_border_width(_files_table, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(_files_table, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(_files_table, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(_files_table, 4, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(_files_table, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_files_table, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(_files_table, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_files_table, 0, LV_PART_MAIN);

    lv_table_set_col_cnt(_files_table, 1);
    lv_table_set_col_width(_files_table, 0, 440);
    lv_obj_add_event_cb(_files_table, filesRowCb, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 52);
    lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 2, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    sdr_btn(row, "REFRESH", filesRefreshCb, this, nullptr);
    sdr_btn(row, "DELETE",  filesDeleteCb,  this, nullptr);

    refreshFiles();
}

void AppREC::refreshFiles(void)
{
    if (!_files_table) return;

    char list[768];
    int n = rec_list(list, sizeof(list));

    _file_count = 0;
    _file_sel   = -1;

    lv_table_set_row_cnt(_files_table, 1);
    lv_table_set_cell_value(_files_table, 0, 0, "FILE");

    /*LS-020*/
    char *p = list;
    while (*p && _file_count < FILES_MAX) {
        char *comma = strstr(p, ", ");
        if (comma) *comma = '\0';

        lv_table_set_row_cnt(_files_table, _file_count + 2);
        lv_table_set_cell_value(_files_table, _file_count + 1, 0, p);

        char *sp = strchr(p, ' ');
        size_t nl = sp ? (size_t)(sp - p) : strlen(p);
        if (nl >= sizeof(_file_name[0])) nl = sizeof(_file_name[0]) - 1;
        memcpy(_file_name[_file_count], p, nl);
        _file_name[_file_count][nl] = '\0';
        _file_count++;

        if (!comma) break;
        p = comma + 2;
    }

    char b[64];
    snprintf(b, sizeof(b), "%d file%s on SPIFFS%s", n, n == 1 ? "" : "s",
             (n > FILES_MAX) ? " (list truncated)" : "");
    lv_label_set_text(_files_note, b);
}

void AppREC::filesRowCb(lv_event_t *e)
{
    AppREC *self = (AppREC *)lv_event_get_user_data(e);
    uint16_t row = 0, col = 0;
    lv_table_get_selected_cell(self->_files_table, &row, &col);
    if (row == LV_TABLE_CELL_NONE || row == 0) { self->_file_sel = -1; return; }
    int idx = (int)row - 1;
    self->_file_sel = (idx >= 0 && idx < self->_file_count) ? idx : -1;

    char b[80];
    if (self->_file_sel >= 0) {
        snprintf(b, sizeof(b), "selected: %s", self->_file_name[self->_file_sel]);
        lv_label_set_text(self->_files_note, b);
    }
}

void AppREC::filesRefreshCb(lv_event_t *e)
{
    ((AppREC *)lv_event_get_user_data(e))->refreshFiles();
}

void AppREC::filesDeleteCb(lv_event_t *e)
{
    AppREC *self = (AppREC *)lv_event_get_user_data(e);
    if (self->_file_sel < 0) {
        lv_label_set_text(self->_files_note, "tap a row first, then DELETE");
        return;
    }

    char name[40];
    strlcpy(name, self->_file_name[self->_file_sel], sizeof(name));

    char b[80];
    if (rec_remove(name) == 0) snprintf(b, sizeof(b), "deleted %s", name);
    else                       snprintf(b, sizeof(b), "could not delete %s", name);

    self->refreshFiles();
    lv_label_set_text(self->_files_note, b);
}
