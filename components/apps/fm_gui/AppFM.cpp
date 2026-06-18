#include "AppFM.hpp"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

extern "C" {
#include "app_registry.h"
#include "settings.h"
#include "fm_state.h"
#include "lakeshark_backend.h"
#include "audio_out.h"
}

#include "sdr_ui/sdr_ui.h"

LV_IMG_DECLARE(img_app_music_player);

#define COL_LABEL   SDR_LABEL
#define COL_TEXT    SDR_TEXT
#define COL_BRIGHT  SDR_BRIGHT
#define COL_DIM     SDR_DIM
#define COL_CYAN    SDR_CYAN
#define COL_GREEN   SDR_GREEN
#define COL_AMBER   SDR_AMBER
#define COL_GOLD    SDR_GOLD
#define COL_RED     SDR_RED
#define COL_PANEL   SDR_PANEL

static const char *MODE_NAMES[FM_MODE_COUNT] = { "LISTEN", "SCAN", "POCSAG", "WFM" };

static const int   STEP_HZ[]   = { 5000, 10000, 12500, 25000, 100000, 1000000 };
static const char *STEP_NAME[] = { "5k", "10k", "12.5k", "25k", "100k", "1M" };
static const int   STEP_N      = sizeof(STEP_HZ) / sizeof(STEP_HZ[0]);

struct band_t { uint32_t a, b, step; const char *name; };
static const band_t BANDS[] = {
    { 150000000UL, 162000000UL, 100000UL, "VHF HI 150-162" },
    { 144000000UL, 148000000UL,  25000UL, "2m HAM 144-148" },
    { 159000000UL, 161000000UL,  12500UL, "PAGER 159-161"  },
    { 450000000UL, 460000000UL,  25000UL, "UHF 450-460"    },
    { 462000000UL, 468000000UL,  25000UL, "GMRS/FRS 462+"  },
};
static const int BAND_N = sizeof(BANDS) / sizeof(BANDS[0]);
static int s_band_idx = 0;

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void mhz_str(char *b, size_t n, uint32_t hz, int dec)
{
    unsigned long whole = hz / 1000000UL;
    unsigned long rem   = hz % 1000000UL;
    if (dec <= 1)      snprintf(b, n, "%lu.%01lu", whole, rem / 100000UL);
    else if (dec == 3) snprintf(b, n, "%lu.%03lu", whole, rem / 1000UL);
    else               snprintf(b, n, "%lu.%04lu", whole, rem / 100UL);
}

static void fm_baud_str(char *buf, size_t n)
{
    if (FM.pocsag_auto) {
        if (FM.pocsag_sync && FM.pocsag_baud)
            snprintf(buf, n, "AUTO=%d", FM.pocsag_baud);
        else if (FM.pocsag_lock_baud)
            snprintf(buf, n, "AUTO last %d", FM.pocsag_lock_baud);
        else
            snprintf(buf, n, "AUTO");
    } else {
        snprintf(buf, n, "%d", FM.pocsag_baud);
    }
}

static lv_obj_t *mono(lv_obj_t *parent, lv_color_t col)
{
    return sdr_label(parent, sdr_font_mono(), col);
}

static void fm_seg_gain_live(void *, int v)   { lakeshark_fm_set_gain_live(v); }
static void fm_seg_gain_commit(void *, int v) { lakeshark_fm_set_gain(v); }
static void fm_seg_sq(void *, int v)   { lakeshark_fm_set_squelch(v); }
static void fm_seg_vol(void *, int v)  { audio_volume_set(v); }

static lv_obj_t *make_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud,
                          int w = 90, int h = 48)
{
    lv_obj_t *b = sdr_btn(parent, txt, cb, ud, nullptr);
    lv_obj_set_size(b, w, h);
    return b;
}

static lv_obj_t *btn_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 2, 0);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_set_style_pad_row(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

AppFM::AppFM()
    : LsApp("FM", "fm")
{
}

AppFM::~AppFM() = default;

bool AppFM::init(void)   { return true; }
bool AppFM::pause(void)  { lakeshark_radio_park(); return true; }

bool AppFM::resume(void)
{
    lakeshark_select_fm();
    return true;
}

bool AppFM::back(void)
{
    if (_freq_modal) { closeFreqEntry(); return true; }
    return exitToLauncher();
}

bool AppFM::close(void)
{
    closeFreqEntry();
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
    _tabview = nullptr;
    lakeshark_radio_park();
    return true;
}

bool AppFM::run(lv_obj_t *parent)
{
    lakeshark_select_fm();

    lv_obj_t *scr = parent;
    sdr_style_screen(scr);

    _tabview = lv_tabview_create(scr, LV_DIR_TOP, 44);
    lv_obj_set_size(_tabview, lv_pct(100), lv_pct(100));
    sdr_style_tabview(_tabview);

    buildVfoTab(lv_tabview_add_tab(_tabview, "VFO"));
    buildPocsagTab(lv_tabview_add_tab(_tabview, "POCSAG"));
    buildScanTab(lv_tabview_add_tab(_tabview, "SCAN"));
    buildConfigTab(lv_tabview_add_tab(_tabview, "CONFIG"));

    _timer = lv_timer_create(timerCb, 200, this);
    return true;
}

static void fm_ascii_bar(char *out, size_t outsz, int pct, int width)
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

/* Text meter (mono ASCII bar), NOT an lv_bar: live lv_bar redraws contend with
 * the DSI framebuffer DMA during demod and glitch the audio. */
static lv_obj_t *meter_label(lv_obj_t *parent)
{
    lv_obj_t *l = sdr_label(parent, sdr_font_mono(), COL_TEXT);
    lv_obj_set_width(l, lv_pct(100));
    return l;
}

void AppFM::buildVfoTab(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_row(parent, 5, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *face = sdr_lcd_panel(parent, SDR_PAS_AMBER);

    lv_obj_t *strap = lv_obj_create(face);
    lv_obj_set_size(strap, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(strap, LV_OPA_0, 0);
    lv_obj_set_style_border_width(strap, 0, 0);
    lv_obj_set_style_pad_all(strap, 0, 0);
    lv_obj_set_flex_flow(strap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strap, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(strap, LV_OBJ_FLAG_SCROLLABLE);

    _v_mode = sdr_label(strap, &lv_font_montserrat_28, SDR_PAS_GOLD);
    lv_obj_set_style_text_letter_space(_v_mode, 2, 0);
    lv_label_set_text(_v_mode, "POCSAG");

    lv_obj_t *rxbox = lv_obj_create(strap);
    lv_obj_set_size(rxbox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(rxbox, LV_OPA_0, 0);
    lv_obj_set_style_border_width(rxbox, 0, 0);
    lv_obj_set_style_pad_all(rxbox, 0, 0);
    lv_obj_set_style_pad_column(rxbox, 6, 0);
    lv_obj_set_flex_flow(rxbox, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rxbox, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(rxbox, LV_OBJ_FLAG_SCROLLABLE);
    _v_lamp = lv_led_create(rxbox);
    lv_obj_set_size(_v_lamp, 14, 14);
    lv_led_set_color(_v_lamp, COL_GREEN);
    lv_led_off(_v_lamp);
    _v_rx = sdr_label(rxbox, &lv_font_montserrat_22, COL_DIM);
    lv_label_set_text(_v_rx, "RX");

    _v_freq = sdr_label(face, &lv_font_montserrat_48, SDR_PAS_AMBER);
    lv_obj_set_width(_v_freq, lv_pct(100));
    lv_obj_set_style_text_align(_v_freq, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(_v_freq, 2, 0);
    lv_label_set_text(_v_freq, "152.6000");
    lv_obj_add_flag(_v_freq, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_v_freq, freqEntryCb, LV_EVENT_CLICKED, this);

    _v_status = sdr_label(face, &lv_font_montserrat_16, SDR_PAS_CYAN);
    lv_obj_set_width(_v_status, lv_pct(100));
    lv_obj_set_style_text_align(_v_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(_v_status, "MHz");

    _v_smeter = meter_label(face);
    _v_act    = meter_label(face);

    lv_obj_t *dp = sdr_panel(parent);
    _v_diag = mono(dp, COL_LABEL);
    lv_obj_set_width(_v_diag, lv_pct(100));
    lv_label_set_text(_v_diag, "");

    lv_obj_t *kp = btn_row(parent);
    { lv_obj_t *l = nullptr; lv_obj_t *b = sdr_btn(kp, "-", stepDownCb, this, &l);
      lv_obj_set_size(b, 96, 52); _v_dn_lbl = l; }
    { lv_obj_t *b = sdr_btn(kp, "STEP", stepCycleCb, this, nullptr);
      lv_obj_set_size(b, 96, 52); }
    { lv_obj_t *l = nullptr; lv_obj_t *b = sdr_btn(kp, "+", stepUpCb, this, &l);
      lv_obj_set_size(b, 96, 52); _v_up_lbl = l; }

    _v_step_lbl = mono(parent, COL_CYAN);
    lv_obj_set_width(_v_step_lbl, lv_pct(100));
    lv_obj_set_style_text_align(_v_step_lbl, LV_TEXT_ALIGN_CENTER, 0);

    _v_gain_slider = sdr_seg_slider(parent, SDR_PAS_AMBER, 496, FM.gain_tenths,
                                    fm_seg_gain_live, this, &_v_gain_lbl);
    sdr_seg_on_release(_v_gain_slider, fm_seg_gain_commit);
    _v_sq_slider   = sdr_seg_slider(parent, SDR_PAS_CYAN, 100, FM.squelch_tenths,
                                    fm_seg_sq, this, &_v_sq_lbl);
    _v_vol_slider  = sdr_seg_slider(parent, SDR_PAS_GREEN, 100, audio_volume_get(),
                                    fm_seg_vol, this, &_v_vol_lbl);

    lv_obj_t *ar = btn_row(parent);
    make_btn(ar, "FREQ", freqEntryCb, this, 84, 44);
    make_btn(ar, "MODE", modeCb,      this, 80, 44);
    make_btn(ar, "AGC",  agcCb,       this, 70, 44);
    make_btn(ar, "-1M",  tuneDeltaCb, (void *)(intptr_t)(-1000000), 66, 44);
    make_btn(ar, "+1M",  tuneDeltaCb, (void *)(intptr_t)(1000000),  66, 44);
}

void AppFM::updateVfo(void)
{
    if (_v_mode) lv_label_set_text(_v_mode, MODE_NAMES[FM.mode % FM_MODE_COUNT]);
    if (_v_freq) { char fb[16]; mhz_str(fb, sizeof(fb), FM.freq_hz, 4); lv_label_set_text(_v_freq, fb); }

    bool busy = false;
    const char *rxtxt = "RX";
    lv_color_t lampc = COL_GREEN;
    char status[48];
    if (FM.mode == FM_MODE_LISTEN) {
        busy = FM.squelch_open;
        rxtxt = busy ? "BUSY" : "RX";
        snprintf(status, sizeof(status), "MHz   %s", busy ? "RX AUDIO" : "squelched");
    } else if (FM.mode == FM_MODE_WFM) {
        busy = true;
        rxtxt = "FM";
        snprintf(status, sizeof(status), "MHz   broadcast FM");
    } else if (FM.mode == FM_MODE_POCSAG) {
        busy = FM.pocsag_sync;
        rxtxt = busy ? "SYNC" : "RX";
        lampc = COL_CYAN;
        char bs[24]; fm_baud_str(bs, sizeof(bs));
        snprintf(status, sizeof(status), "MHz   %s   %s", bs, busy ? "SYNC" : "hunting");
    } else {
        busy = (FM.iq_level > 0.25f);
        rxtxt = busy ? "HIT" : "SCAN";
        lampc = COL_AMBER;
        char a[12], z[12];
        mhz_str(a, sizeof(a), FM.scan_start_hz, 1);
        mhz_str(z, sizeof(z), FM.scan_stop_hz, 1);
        snprintf(status, sizeof(status), "MHz   scanning %s-%s", a, z);
    }
    if (_v_status) lv_label_set_text(_v_status, status);
    if (_v_lamp) {
        lv_led_set_color(_v_lamp, lampc);
        if (busy) lv_led_on(_v_lamp); else lv_led_off(_v_lamp);
    }
    if (_v_rx) {
        lv_label_set_text(_v_rx, rxtxt);
        lv_obj_set_style_text_color(_v_rx, busy ? lampc : COL_DIM, 0);
    }
    if (_v_freq) lv_obj_set_style_text_color(_v_freq, busy ? SDR_PAS_GREEN : SDR_PAS_AMBER, 0);

    int s = clampi((int)(FM.iq_level * 100.0f + 0.5f), 0, 100);
    bool clip = FM.iq_level >= 0.97f;
    if (_v_smeter) {
        char bar[96];
        fm_ascii_bar(bar, sizeof(bar), s, sdr_bar_width(_v_smeter, 9));
        if (clip) lv_label_set_text_fmt(_v_smeter, "S %s CLIP", bar);
        else      lv_label_set_text_fmt(_v_smeter, "S %s %3d%%", bar, s);
        lv_obj_set_style_text_color(_v_smeter,
            clip ? COL_RED : s < 8 ? SDR_PAS_ROSE : s < 25 ? SDR_PAS_AMBER : SDR_PAS_GREEN, 0);
    }

    int af = clampi((int)(FM.audio_level / 0.65f * 100.0f + 0.5f), 0, 100);
    if (_v_act) {
        char bar[96];
        fm_ascii_bar(bar, sizeof(bar), af, sdr_bar_width(_v_act, 9));
        lv_label_set_text_fmt(_v_act, "A %s %3d%%", bar, af);
        lv_obj_set_style_text_color(_v_act, COL_CYAN, 0);
    }

    if (_v_dn_lbl) lv_label_set_text_fmt(_v_dn_lbl, "-%s", STEP_NAME[_step_idx]);
    if (_v_up_lbl) lv_label_set_text_fmt(_v_up_lbl, "+%s", STEP_NAME[_step_idx]);
    if (_v_step_lbl) lv_label_set_text_fmt(_v_step_lbl, "STEP  %s   (DN / UP tune by this)",
                                           STEP_NAME[_step_idx]);

    if (_v_diag) {
        lv_label_set_text_fmt(_v_diag,
            "IQ %3d%%  AF %3d%%   %s %luKB/s  ERR %lu",
            s, af, (FM.iq_bytes_sec > 0) ? "DEV" : "no-dev",
            (unsigned long)(FM.iq_bytes_sec / 1024), (unsigned long)FM.read_errors);
        lv_obj_set_style_text_color(_v_diag, (FM.iq_bytes_sec > 0) ? COL_LABEL : COL_RED, 0);
    }

    if (_v_gain_lbl) {
        if (FM.gain_tenths == 0) lv_label_set_text(_v_gain_lbl, "GAIN  AGC");
        else lv_label_set_text_fmt(_v_gain_lbl, "GAIN  %d.%d dB",
                                   FM.gain_tenths / 10, FM.gain_tenths % 10);
    }
    sdr_seg_set(_v_gain_slider, FM.gain_tenths);

    if (_v_sq_lbl) lv_label_set_text_fmt(_v_sq_lbl, "SQUELCH  %d", FM.squelch_tenths);
    sdr_seg_set(_v_sq_slider, FM.squelch_tenths);

    if (_v_vol_lbl) lv_label_set_text_fmt(_v_vol_lbl, "VOLUME  %d", audio_volume_get());
    sdr_seg_set(_v_vol_slider, audio_volume_get());
}

void AppFM::buildPocsagTab(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_row(parent, 6, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *strap = lv_obj_create(parent);
    lv_obj_set_size(strap, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(strap, COL_PANEL, 0);
    lv_obj_set_style_border_width(strap, 0, 0);
    lv_obj_set_style_radius(strap, 6, 0);
    lv_obj_set_style_pad_all(strap, 8, 0);
    lv_obj_set_style_pad_column(strap, 10, 0);
    lv_obj_set_flex_flow(strap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strap, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(strap, LV_OBJ_FLAG_SCROLLABLE);

    _p_lamp = lv_led_create(strap);
    lv_obj_set_size(_p_lamp, 16, 16);
    lv_led_set_color(_p_lamp, COL_CYAN);
    lv_led_off(_p_lamp);
    _p_strap = sdr_label(strap, &lv_font_montserrat_16, COL_TEXT);
    lv_obj_set_flex_grow(_p_strap, 1);
    lv_label_set_text(_p_strap, "HUNT  baud 1200");

    {
        lv_obj_t *baudb = sdr_btn(strap, "BAUD", baudCb, this, nullptr);
        lv_obj_set_size(baudb, 90, 40);
    }

    lv_obj_t *cp = sdr_panel(parent);
    _p_counts = mono(cp, COL_LABEL);
    lv_obj_set_width(_p_counts, lv_pct(100));
    lv_label_set_text(_p_counts, "frames 0   pages 0   cw-err 0");

    sdr_section(parent, "DECODED PAGES (newest first)");

    lv_obj_t *logbox = lv_obj_create(parent);
    lv_obj_set_width(logbox, lv_pct(100));
    lv_obj_set_flex_grow(logbox, 1);
    lv_obj_set_style_bg_color(logbox, lv_color_hex(0x141414), 0);
    lv_obj_set_style_bg_opa(logbox, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(logbox, SDR_BORDER, 0);
    lv_obj_set_style_border_width(logbox, 1, 0);
    lv_obj_set_style_radius(logbox, 4, 0);
    lv_obj_set_style_pad_all(logbox, 8, 0);
    lv_obj_set_scroll_dir(logbox, LV_DIR_VER);

    _p_log = sdr_label(logbox, sdr_font_mono(), COL_TEXT);
    lv_obj_set_width(_p_log, lv_pct(100));
    lv_label_set_long_mode(_p_log, LV_LABEL_LONG_WRAP);
    lv_label_set_text(_p_log, "(listening for pages...)");
}

void AppFM::updatePocsag(void)
{
    if (_p_lamp) { if (FM.pocsag_sync) lv_led_on(_p_lamp); else lv_led_off(_p_lamp); }
    if (_p_strap) {
        char bs[24]; fm_baud_str(bs, sizeof(bs));
        char fb[16]; mhz_str(fb, sizeof(fb), FM.freq_hz, 4);
        lv_label_set_text_fmt(_p_strap, "%s   %s   %s MHz",
            FM.pocsag_sync ? "SYNC" : "HUNT", bs, fb);
        lv_obj_set_style_text_color(_p_strap, FM.pocsag_sync ? COL_GREEN : COL_DIM, 0);
    }
    if (_p_counts) {
        lv_label_set_text_fmt(_p_counts,
            "frames %lu  pages %lu  cw-err %lu\naddr %lu  msg %lu  (msg=0 -> tone only)",
            (unsigned long)FM.pocsag_frames, (unsigned long)FM.pocsag_pages,
            (unsigned long)FM.pocsag_cw_errs,
            (unsigned long)FM.pocsag_addr, (unsigned long)FM.pocsag_msg);
    }
    if (_p_log) {
        char buf[700]; int off = 0;
        int n = FM.page_count;
        for (int k = 0; k < n && off < (int)sizeof(buf) - 110; k++) {
            int idx = (FM.page_head - 1 - k + FM_PAGE_LOG_MAX * 2) % FM_PAGE_LOG_MAX;
            const fm_page_t *p = &FM.pages[idx];
            const char *tn = (p->type == 'A') ? "ALPHA" : (p->type == 'N') ? "NUM" : "TONE";
            off += snprintf(buf + off, sizeof(buf) - off, "RIC %lu  F%d  %d %s  %s\n",
                            (unsigned long)p->address, p->function, p->baud, tn, p->text);
        }
        if (off == 0) snprintf(buf, sizeof(buf), "(listening for pages...)");
        lv_label_set_text(_p_log, buf);
    }
}

void AppFM::buildScanTab(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_row(parent, 6, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *ip = sdr_panel(parent);
    _s_info = mono(ip, COL_TEXT);
    lv_obj_set_width(_s_info, lv_pct(100));
    lv_label_set_text(_s_info, "band --");

    _s_peak = sdr_label(parent, &lv_font_montserrat_18, COL_GOLD);
    lv_obj_set_width(_s_peak, lv_pct(100));
    lv_label_set_text(_s_peak, "PEAK --");

    lv_obj_t *cl = sdr_label(parent, &lv_font_montserrat_12, COL_LABEL);
    lv_label_set_text(cl, "BAND ENERGY (per channel)");
    _s_chart = lv_chart_create(parent);
    lv_obj_set_width(_s_chart, lv_pct(100));
    lv_obj_set_flex_grow(_s_chart, 1);
    lv_chart_set_type(_s_chart, LV_CHART_TYPE_BAR);
    lv_obj_set_style_bg_color(_s_chart, COL_PANEL, 0);
    lv_obj_set_style_border_width(_s_chart, 0, 0);
    lv_obj_set_style_pad_all(_s_chart, 4, 0);
    lv_chart_set_point_count(_s_chart, 1);
    _s_chart_pts = 1;
    _s_ser = lv_chart_add_series(_s_chart, COL_GREEN, LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_t *br = btn_row(parent);
    make_btn(br, "BAND",   bandCb,        this, 96, 46);
    make_btn(br, "RESTART",scanRestartCb, this, 110, 46);
    make_btn(br, "->PEAK", tunePeakCb,    this, 110, 46);
}

void AppFM::updateScan(void)
{
    uint32_t cur = FM.scan_start_hz + (uint32_t)FM.scan_idx * FM.scan_step_hz;
    if (_s_info) {
        char a[12], z[12], c[12];
        mhz_str(a, sizeof(a), FM.scan_start_hz, 3);
        mhz_str(z, sizeof(z), FM.scan_stop_hz, 3);
        mhz_str(c, sizeof(c), cur, 3);
        lv_label_set_text_fmt(_s_info,
            "%s\n%s-%s  step %lukHz  now %s  sweep %lu",
            BANDS[s_band_idx].name, a, z,
            (unsigned long)(FM.scan_step_hz / 1000), c,
            (unsigned long)FM.scan_sweeps);
    }
    if (_s_peak) {
        if (FM.scan_peak_hz) {
            char p[16]; mhz_str(p, sizeof(p), FM.scan_peak_hz, 4);
            lv_label_set_text_fmt(_s_peak, "PEAK  %s MHz   %d%%",
                p, (int)(FM.scan_peak_db * 100.0f));
        } else {
            lv_label_set_text(_s_peak, "PEAK  --");
        }
    }
    if (_s_chart && _s_ser) {
        int bins = FM.scan_bins;
        if (bins < 1) bins = 1;
        if (bins > FM_SCAN_BINS_MAX) bins = FM_SCAN_BINS_MAX;
        if (bins != _s_chart_pts) {
            lv_chart_set_point_count(_s_chart, bins);
            _s_chart_pts = bins;
        }
        float mx = 0.01f;
        for (int i = 0; i < bins; i++) if (FM.scan_db[i] > mx) mx = FM.scan_db[i];
        lv_chart_set_range(_s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, (int)(mx * 100.0f) + 1);
        for (int i = 0; i < bins; i++)
            lv_chart_set_value_by_id(_s_chart, _s_ser, i, (int)(FM.scan_db[i] * 100.0f));
        lv_chart_refresh(_s_chart);
    }
}

void AppFM::buildConfigTab(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 8, 0);
    lv_obj_set_style_pad_row(parent, 6, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    sdr_setrow_t r;

    sdr_section(parent, "RADIO");

    sdr_setting_row(parent, "FREQUENCY", &r);
    _c_freq = r.value;
    sdr_btn(r.controls, "-1M",  tuneDeltaCb, (void *)(intptr_t)(-1000000), nullptr);
    sdr_btn(r.controls, "-25k", tuneDeltaCb, (void *)(intptr_t)(-25000),  nullptr);
    sdr_btn(r.controls, "+25k", tuneDeltaCb, (void *)(intptr_t)(25000),   nullptr);
    sdr_btn(r.controls, "+1M",  tuneDeltaCb, (void *)(intptr_t)(1000000), nullptr);

    sdr_setting_row(parent, "GAIN", &r);
    _c_gain = r.value;
    sdr_btn(r.controls, "-",    gainDownCb, this, nullptr);
    sdr_btn(r.controls, "+",    gainUpCb,   this, nullptr);
    sdr_btn(r.controls, "STEP", gainCb,     this, nullptr);
    sdr_btn(r.controls, "AGC",  agcCb,      this, nullptr);

    lv_obj_t *gl = nullptr;
    _c_gain_slider = sdr_seg_slider(parent, SDR_PAS_AMBER, 496, FM.gain_tenths,
                                    fm_seg_gain_live, this, &gl);
    sdr_seg_on_release(_c_gain_slider, fm_seg_gain_commit);
    if (gl) lv_label_set_text(gl, "MANUAL GAIN  (drag; left = AGC)");

    sdr_setting_row(parent, "SQUELCH", &r);
    _c_sql = r.value;
    sdr_btn(r.controls, "-", sqDownCb, this, nullptr);
    sdr_btn(r.controls, "+", sqUpCb,   this, nullptr);

    sdr_section(parent, "POCSAG");

    sdr_setting_row(parent, "BAUD", &r);
    _c_baud = r.value;
    sdr_btn(r.controls, "CYCLE", baudCb, this, nullptr);

    sdr_section(parent, "SCAN");

    sdr_setting_row(parent, "BAND", &r);
    _c_band = r.value;
    sdr_btn(r.controls, "NEXT", bandCb, this, nullptr);

    sdr_section(parent, "AUDIO");

    _c_vol_slider = sdr_seg_slider(parent, SDR_PAS_GREEN, 100, audio_volume_get(),
                                   fm_seg_vol, this, &_c_vol_lbl);

    sdr_setting_row(parent, "MUTE", &r);
    _c_mute = r.value;
    sdr_btn(r.controls, "TOGGLE", muteCb, this, nullptr);

    sdr_section(parent, "DIAGNOSTICS");

    lv_obj_t *dp = sdr_panel(parent);
    _c_diag = mono(dp, COL_LABEL);
    lv_obj_set_width(_c_diag, lv_pct(100));
    lv_label_set_text(_c_diag, "");

    updateConfig();
}

void AppFM::updateConfig(void)
{
    char b[40];
    if (_c_freq) {
        uint32_t f = FM.freq_hz;
        snprintf(b, sizeof(b), "%lu.%04lu MHz",
                 (unsigned long)(f / 1000000UL), (unsigned long)((f / 100UL) % 10000UL));
        lv_label_set_text(_c_freq, b);
    }
    if (_c_gain) {
        if (FM.gain_tenths == 0) lv_label_set_text(_c_gain, "AGC");
        else { snprintf(b, sizeof(b), "%d.%d dB", FM.gain_tenths / 10, FM.gain_tenths % 10);
               lv_label_set_text(_c_gain, b); }
    }
    sdr_seg_set(_c_gain_slider, FM.gain_tenths);
    if (_c_sql)  lv_label_set_text_fmt(_c_sql, "%d", FM.squelch_tenths);
    if (_c_baud) { char bs[24]; fm_baud_str(bs, sizeof(bs)); lv_label_set_text(_c_baud, bs); }
    if (_c_band) lv_label_set_text(_c_band, BANDS[s_band_idx].name);
    if (_c_vol_lbl) lv_label_set_text_fmt(_c_vol_lbl, "VOLUME  %d", audio_volume_get());
    sdr_seg_set(_c_vol_slider, audio_volume_get());
    if (_c_mute) lv_label_set_text(_c_mute, audio_is_muted() ? "MUTED" : "ON");

    if (_c_diag) {
        bool dev = (FM.iq_bytes_sec > 0);
        lv_label_set_text_fmt(_c_diag,
            "DEVICE   %s\nIQ RATE  %lu KB/s\nIQ PEAK  %d%%   ACT %d%%\nREAD ERR %lu\nDEMOD    %d Hz   AUDIO %d Hz",
            dev ? "streaming" : "NO RTL-SDR",
            (unsigned long)(FM.iq_bytes_sec / 1024),
            clampi((int)(FM.iq_level * 100.0f), 0, 100),
            clampi((int)(FM.audio_level / 0.65f * 100.0f), 0, 100),
            (unsigned long)FM.read_errors,
            FM_DEMOD_RATE, FM_AUDIO_RATE);
        lv_obj_set_style_text_color(_c_diag, (FM.iq_bytes_sec > 0) ? COL_GREEN : COL_RED, 0);
    }
}

void AppFM::timerCb(lv_timer_t *t)
{
    AppFM *self = static_cast<AppFM *>(t->user_data);
    if (!self || !self->_tabview) return;
    switch (lv_tabview_get_tab_act(self->_tabview)) {
        case 0:  self->updateVfo();    break;
        case 1:  self->updatePocsag(); break;
        case 2:  self->updateScan();   break;
        case 3:  self->updateConfig(); break;
        default: break;
    }
}

void AppFM::switchTab(int delta)
{
    if (!_tabview) return;
    const int N = 4;
    int cur = (int)lv_tabview_get_tab_act(_tabview);
    lv_tabview_set_act(_tabview, (cur + delta + N) % N, LV_ANIM_OFF);
}

void AppFM::modeCb(lv_event_t *)
{
    lakeshark_fm_set_mode((lakeshark_fm_get_mode() + 1) % FM_MODE_COUNT);
}

void AppFM::stepDownCb(lv_event_t *e)
{
    AppFM *self = static_cast<AppFM *>(lv_event_get_user_data(e));
    lakeshark_fm_tune(-STEP_HZ[self->_step_idx]);
}
void AppFM::stepUpCb(lv_event_t *e)
{
    AppFM *self = static_cast<AppFM *>(lv_event_get_user_data(e));
    lakeshark_fm_tune(+STEP_HZ[self->_step_idx]);
}
void AppFM::stepCycleCb(lv_event_t *e)
{
    AppFM *self = static_cast<AppFM *>(lv_event_get_user_data(e));
    self->_step_idx = (self->_step_idx + 1) % STEP_N;
    if (self->_v_step_lbl)
        lv_label_set_text_fmt(self->_v_step_lbl, "STEP  %s", STEP_NAME[self->_step_idx]);
}
void AppFM::tuneDeltaCb(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    lakeshark_fm_tune(delta);
}

void AppFM::gainCb(lv_event_t *)     { lakeshark_fm_gain_step(); }
void AppFM::gainDownCb(lv_event_t *) { lakeshark_fm_gain_delta(-10); }
void AppFM::gainUpCb(lv_event_t *)   { lakeshark_fm_gain_delta(+10); }
void AppFM::gainSliderCb(lv_event_t *e)
{
    lakeshark_fm_set_gain((int)lv_slider_get_value(lv_event_get_target(e)));
}
void AppFM::sqSliderCb(lv_event_t *e)
{
    lakeshark_fm_set_squelch((int)lv_slider_get_value(lv_event_get_target(e)));
}
void AppFM::agcCb(lv_event_t *)    { lakeshark_fm_agc(); }
void AppFM::sqDownCb(lv_event_t *) { lakeshark_fm_squelch_delta(-1); }
void AppFM::sqUpCb(lv_event_t *)   { lakeshark_fm_squelch_delta(+1); }
void AppFM::scanRestartCb(lv_event_t *)
{
    lakeshark_fm_set_mode(FM_MODE_SCAN);
    lakeshark_fm_scan_restart();
}
void AppFM::tunePeakCb(lv_event_t *)  { lakeshark_fm_tune_to_peak(); }

void AppFM::baudCb(lv_event_t *)
{
    int nb;
    if (FM.pocsag_auto)            nb = 512;
    else if (FM.pocsag_baud == 512)  nb = 1200;
    else if (FM.pocsag_baud == 1200) nb = 2400;
    else                          nb = 0;
    lakeshark_fm_set_baud(nb);
}

void AppFM::bandCb(lv_event_t *)
{
    s_band_idx = (s_band_idx + 1) % BAND_N;
    FM.scan_start_hz = BANDS[s_band_idx].a;
    FM.scan_stop_hz  = BANDS[s_band_idx].b;
    FM.scan_step_hz  = BANDS[s_band_idx].step;
    lakeshark_fm_set_mode(FM_MODE_SCAN);
    lakeshark_fm_scan_restart();
}

void AppFM::volSliderCb(lv_event_t *e)
{
    audio_volume_set((int)lv_slider_get_value(lv_event_get_target(e)));
}
void AppFM::muteCb(lv_event_t *) { audio_toggle_mute(); }

void AppFM::freqEntryCb(lv_event_t *e)
{
    AppFM *self = static_cast<AppFM *>(lv_event_get_user_data(e));
    if (self) self->openFreqEntry();
}

void AppFM::openFreqEntry(void)
{
    if (_freq_modal) return;

    lv_obj_t *bg = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bg, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_80, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 10, 0);
    lv_obj_set_style_pad_row(bg, 8, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    _freq_modal = bg;

    lv_obj_t *title = sdr_label(bg, &lv_font_montserrat_16, SDR_CYAN);
    lv_label_set_text(title, "ENTER FREQUENCY (MHz)  -  e.g. 152.600");

    lv_obj_t *ta = lv_textarea_create(bg);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_accepted_chars(ta, "0123456789.");
    lv_textarea_set_max_length(ta, 10);
    lv_textarea_set_placeholder_text(ta, "152.600");
    lv_textarea_set_text(ta, "");
    lv_obj_set_width(ta, lv_pct(80));
    lv_obj_set_style_text_font(ta, sdr_font_mono(), 0);
    _freq_ta = ta;

    lv_obj_t *kb = lv_keyboard_create(bg);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_set_width(kb, lv_pct(100));
    lv_obj_set_flex_grow(kb, 1);
    lv_obj_add_event_cb(kb, freqKbCb, LV_EVENT_READY,  this);
    lv_obj_add_event_cb(kb, freqKbCb, LV_EVENT_CANCEL, this);
}

void AppFM::freqKbCb(lv_event_t *e)
{
    AppFM *self = static_cast<AppFM *>(lv_event_get_user_data(e));
    if (!self) return;
    if (lv_event_get_code(e) == LV_EVENT_READY && self->_freq_ta) {
        double mhz = atof(lv_textarea_get_text(self->_freq_ta));
        if (mhz >= 1.0 && mhz <= 2000.0)
            lakeshark_fm_set_freq((uint32_t)(mhz * 1e6 + 0.5));
    }
    self->closeFreqEntry();
}

void AppFM::closeFreqEntry(void)
{
    if (_freq_modal) { lv_obj_del(_freq_modal); _freq_modal = nullptr; _freq_ta = nullptr; }
}
