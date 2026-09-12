#include "AppFM.hpp"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

extern "C" {
#include "app_registry.h"
#include "settings.h"
#include "fm_state.h"
#include "fm_mode_label.h"
#include "lakeshark_backend.h"
#include "audio_out.h"
/**/
#include "scan_engine.h"
/**/
#include "fm_sweep_arbitration.h"
/**/
#include "ls_time.h"
}

#include "sdr_ui/sdr_ui.h"
#include "ui/ls_ui.h"
#include "ui/ls_receiver_status.h"

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

/**/ /**/
/* TAB ORDER, NAMED ONCE. run() adds the tabs, timerCb dispatches on the active
   index and switchTab wraps on the count - three places that must agree with
   an order expressed nowhere. Inserting SCAN at 1 already shifted all of them
   silently. These names do not create a compile-time link, but they make the
   next insertion a one-line edit instead of a hunt. */
enum {
    TAB_VFO = 0,
    TAB_SCAN,
    TAB_PAGES,
    TAB_SWEEP,
    TAB_CONFIG,
    TAB_COUNT
};

/**/
/* The MODE button cycles DEMODULATORS only. SWEEP is a job, not a
   demodulator: it drives the tuner across a band and parks nowhere, so
   landing on it while cycling looking for WFM stops audio dead with no
   indication why. It is reached from the SWEEP tab's own button now. */
static const fm_mode_t MODE_CYCLE[] = {
    FM_MODE_LISTEN, FM_MODE_WFM, FM_MODE_POCSAG, FM_MODE_FLEX
};
static const int       MODE_CYCLE_N = (int)(sizeof(MODE_CYCLE) / sizeof(MODE_CYCLE[0]));

static const char *FLEX_RATE_LEVEL[] = {
    "1600 bps / 2-level", "3200 bps / 2-level",
    "3200 bps / 4-level", "6400 bps / 4-level"
};

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
    (void)w;
    (void)h;
    return ls_ui_button(parent, txt, LS_BTN_DEFAULT, cb, ud, nullptr);
}

static lv_obj_t *btn_row(lv_obj_t *parent)
{
    return ls_ui_controls(parent);
}

/**/
/* Hook trampolines for fm_sweep_start_arbitrated(). Kept at file scope so
   fm_sweep_hooks_t can hold their C-language addresses without a std::function
   or a capturing lambda. The ctx is unused - the hooks all resolve to
   translation-unit-global state (scan_engine's s_enabled, the FM app's mode). */
static bool fm_sweep_hook_scanner_active(void *)   { return scan_engine_active(); }
static void fm_sweep_hook_scanner_stop(void *)     { scan_engine_stop(); }
static void fm_sweep_hook_enter_sweep_mode(void *) { lakeshark_fm_set_mode(FM_MODE_SCAN); }
static void fm_sweep_hook_restart_sweep(void *)    { lakeshark_fm_scan_restart(); }

AppFM::AppFM()
    : LsApp("FM", "fm")
{
}

AppFM::~AppFM() = default;

bool AppFM::init(void)   { return true; }
/**/
bool AppFM::pause(void)
{
    if (_timer) lv_timer_pause(_timer);
    lakeshark_radio_park();
    return true;
}

/**/
bool AppFM::background(void)
{
    if (_timer) lv_timer_pause(_timer);
    return true;
}

/**/
bool AppFM::resume(void)
{
    lakeshark_select_fm();
    if (_timer) lv_timer_resume(_timer);
    return true;
}

bool AppFM::back(void)
{
    if (_freq_entry) { closeFreqEntry(); return true; }
    return exitToLauncher();
}

bool AppFM::close(void)
{
    _wf_sweep = 0;
    _last_mode = -1;
    closeFreqEntry();
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
    _tabview = nullptr;
    /**/
    _scan_panel.forget();
    ls_spectrum_waterfall_forget(&_s_spectrum);
    /**/
    fm_sweep_configure(nullptr);
    lakeshark_radio_park();
    return true;
}

bool AppFM::run(lv_obj_t *parent)
{
    lakeshark_select_fm();

    /**/
    /* Wire the sweep-arbitration gateway to this app's tuner. Cleared in
       close() so a stale hook cannot fire after the app is gone. */
    const fm_sweep_hooks_t sweep_hooks = {
        fm_sweep_hook_scanner_active,
        fm_sweep_hook_scanner_stop,
        fm_sweep_hook_enter_sweep_mode,
        fm_sweep_hook_restart_sweep,
        nullptr,
    };
    fm_sweep_configure(&sweep_hooks);

    ls_ui_screen_t screen;
    ls_ui_screen_create(parent, "FM", true, LS_UI_COLOR_ID_TEAL, &screen);
    _screen_readout = screen.readout;
    _screen_lamp = screen.lamp;
    ls_ui_screen_set_readout(&screen, "FM RADIO");
    _tabview = screen.tabs;

    buildVfoTab(ls_ui_screen_add_tab(&screen, "VFO"));
    /**/

    buildScanCtlTab(ls_ui_screen_add_tab(&screen, "SCAN"));
    /* FLEX pages share the pager list; the protocol is printed on
       every row below. */
    buildPageTab(ls_ui_screen_add_tab(&screen, "PAGES"));
    buildScanTab(ls_ui_screen_add_tab(&screen, "SWEEP"));
    buildConfigTab(ls_ui_screen_add_tab(&screen, "CONFIG"));

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
    ls_ui_style_content(parent);

    lv_obj_t *face = sdr_lcd_panel(parent, SDR_ROLE_COLOR(LS_UI_COLOR_ID_TEAL));

    lv_obj_t *strap = lv_obj_create(face);
    lv_obj_set_size(strap, lv_pct(100), LV_SIZE_CONTENT);
    ls_ui_style_lcd_row(strap);
    lv_obj_set_flex_flow(strap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strap, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(strap, LV_OBJ_FLAG_SCROLLABLE);

    _v_mode = sdr_label(strap, &lv_font_montserrat_28, SDR_ROLE_COLOR(LS_UI_COLOR_ID_TEAL));
    lv_obj_set_style_text_letter_space(_v_mode, 2, 0);
    lv_label_set_text(_v_mode, "POCSAG");

    lv_obj_t *rxbox = lv_obj_create(strap);
    lv_obj_set_size(rxbox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    ls_ui_style_lcd_row(rxbox);
    lv_obj_set_flex_flow(rxbox, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rxbox, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(rxbox, LV_OBJ_FLAG_SCROLLABLE);
    _v_lamp = lv_led_create(rxbox);
    lv_obj_set_size(_v_lamp, 14, 14);
    lv_led_set_color(_v_lamp, COL_GREEN);
    lv_led_off(_v_lamp);
    _v_rx = sdr_label(rxbox, &lv_font_montserrat_22, COL_DIM);
    lv_label_set_text(_v_rx, "RX");

    _v_freq = sdr_label(face, &lv_font_montserrat_48, SDR_ROLE_COLOR(LS_UI_COLOR_TEXT));
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
    { lv_obj_t *l = nullptr;
      ls_ui_button(kp, "-", LS_BTN_DEFAULT, stepDownCb, this, &l); _v_dn_lbl = l; }
    ls_ui_button(kp, "STEP", LS_BTN_DEFAULT, stepCycleCb, this, nullptr);
    { lv_obj_t *l = nullptr;
      ls_ui_button(kp, "+", LS_BTN_DEFAULT, stepUpCb, this, &l); _v_up_lbl = l; }

    _v_step_lbl = mono(parent, COL_CYAN);
    lv_obj_set_width(_v_step_lbl, lv_pct(100));
    lv_obj_set_style_text_align(_v_step_lbl, LV_TEXT_ALIGN_CENTER, 0);

    _v_gain_slider = sdr_seg_slider(parent, SDR_ROLE_COLOR(LS_UI_COLOR_ID_TEAL), 496, FM.gain_tenths,
                                    fm_seg_gain_live, this, &_v_gain_lbl);
    sdr_seg_use_steps(_v_gain_slider,10);
    sdr_seg_on_release(_v_gain_slider, fm_seg_gain_commit);
    _v_sq_slider   = sdr_seg_slider(parent, SDR_PAS_CYAN, 100, FM.squelch_tenths,
                                    fm_seg_sq, this, &_v_sq_lbl);
    _v_vol_slider  = sdr_seg_slider(parent, SDR_PAS_GREEN, 100, audio_volume_get(),
                                    fm_seg_vol, this, &_v_vol_lbl);
    sdr_seg_use_steps(_v_vol_slider,5);

    lv_obj_t *ar = btn_row(parent);
    make_btn(ar, "FREQ", freqEntryCb, this, 84, 44);
    make_btn(ar, "MODE", modeCb,      this, 80, 44);
    make_btn(ar, "AGC",  agcCb,       this, 70, 44);
    make_btn(ar, "-1M",  tuneDeltaCb, (void *)(intptr_t)(-1000000), 66, 44);
    make_btn(ar, "+1M",  tuneDeltaCb, (void *)(intptr_t)(1000000),  66, 44);

    /**/

    lv_obj_t *sr = btn_row(parent);
    { lv_obj_t *l = nullptr;
      ls_ui_button(sr, "SCAN", LS_BTN_TOGGLE_OFF, scanToggleCb, this, &l);
      _v_scan_lbl = l; }
    make_btn(sr, "SKIP", scanSkipCb, this, 90, 44);
    /**/
    /* AUTO SQ moved to the SCAN tab with the rest of the scanner settings.
       What stays here is deliberately only START/STOP and SKIP - the two
       things you reach for with the radio already in your hand. The SCAN tab
       is the authoritative surface; this row is a shortcut into it, not a
       second copy of it. */
    _v_scan_state = mono(sr, COL_LABEL);
    lv_label_set_text(_v_scan_state, "scanner off");
}

/**/
void AppFM::buildScanCtlTab(lv_obj_t *parent)
{
    ls_ui_style_content(parent);

    _scan_panel.build(parent);
}

void AppFM::updateVfo(void)
{
    ls_iq_control_status_t radio;
    ls_receiver_presentation_t receiver;
    fm_get_receiver_status(&radio);
    ls_receiver_present(&radio, &receiver);

    if (_v_mode) lv_label_set_text(_v_mode, fm_mode_label(FM.mode));

    /**/
    /* Show what the scanner is actually doing, on the panel that has the
       button. scan_engine_status() is the same string the console prints -
       one source of truth, so the screen and `scan status` can never
       disagree about whether it is holding. */
    if (_v_scan_lbl)
        lv_label_set_text(_v_scan_lbl, scan_engine_active() ? "STOP" : "SCAN");
    if (_v_scan_state) {
        if (scan_engine_active()) {
            char st[96];
            scan_engine_status(st, sizeof(st));
            lv_label_set_text(_v_scan_state, st);
            lv_obj_set_style_text_color(_v_scan_state, sdr_accent(), 0);
        } else {
            lv_label_set_text(_v_scan_state, "scanner off");
            lv_obj_set_style_text_color(_v_scan_state, COL_LABEL, 0);
        }
    }
    if (_v_freq) {
        char fb[24];
        mhz_str(fb, sizeof(fb), FM.freq_hz, 4);
        lv_label_set_text(_v_freq, fb);
        ls_ui_readout_set(_screen_readout, fb);
    }

    bool busy = false;
    const char *rxtxt = "RX";
    lv_color_t lampc = COL_GREEN;
    char status[96];
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
    } else if (FM.mode == FM_MODE_FLEX) {
        busy = FM.flex_sync;
        rxtxt = busy ? "SYNC" : "RX";
        lampc = COL_CYAN;
        unsigned mode = FM.flex_mode < 4 ? FM.flex_mode : 0;
        snprintf(status, sizeof(status), "MHz   %s   %s",
                 FLEX_RATE_LEVEL[mode], busy ? "SYNC" : "hunting");
    } else {
        busy = (FM.iq_level > 0.25f);
        rxtxt = busy ? "HIT" : "SCAN";
        lampc = COL_AMBER;
        char a[12], z[12];
        mhz_str(a, sizeof(a), FM.scan_start_hz, 1);
        mhz_str(z, sizeof(z), FM.scan_stop_hz, 1);
        snprintf(status, sizeof(status), "MHz   scanning %s-%s", a, z);
    }
    if (!receiver.available) {
        busy = false;
        rxtxt = "NO RX";
        lampc = COL_RED;
        snprintf(status, sizeof(status), "%s", receiver.connection);
    } else if (receiver.tune_attention ||
               (radio.effective_center_known &&
                radio.requested_center_hz != radio.effective_center_hz)) {
        snprintf(status, sizeof(status), "%s", receiver.frequency);
    }
    if (_v_status) lv_label_set_text(_v_status, status);
    if (_v_lamp) {
        lv_led_set_color(_v_lamp, lampc);
        if (busy) lv_led_on(_v_lamp); else lv_led_off(_v_lamp);
    }
    ls_ui_lamp_set(_screen_lamp, busy, LS_UI_COLOR_ACCENT);
    if (_v_rx) {
        lv_label_set_text(_v_rx, rxtxt);
        lv_obj_set_style_text_color(_v_rx, busy ? lampc : COL_DIM, 0);
    }
    if (_v_freq) lv_obj_set_style_text_color(_v_freq, busy ? SDR_PAS_GREEN : SDR_ROLE_COLOR(LS_UI_COLOR_TEXT), 0);

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
            s, af, receiver.available ? "DEV" : "NO RX",
            (unsigned long)(FM.iq_bytes_sec / 1024), (unsigned long)FM.read_errors);
        lv_obj_set_style_text_color(_v_diag, receiver.available ? COL_LABEL : COL_RED, 0);
    }

    if (_v_gain_lbl) {
        lv_label_set_text_fmt(_v_gain_lbl, "GAIN  %s", receiver.gain);
        lv_obj_set_style_text_color(_v_gain_lbl,
            receiver.gain_attention ? COL_RED : COL_LABEL, 0);
    }
    sdr_seg_set(_v_gain_slider, FM.gain_tenths);

    if (_v_sq_lbl) lv_label_set_text_fmt(_v_sq_lbl, "SQUELCH  %d", FM.squelch_tenths);
    sdr_seg_set(_v_sq_slider, FM.squelch_tenths);

    if (_v_vol_lbl) lv_label_set_text_fmt(_v_vol_lbl, "VOLUME  %d", audio_volume_get());
    sdr_seg_set(_v_vol_slider, audio_volume_get());
}

void AppFM::buildPageTab(lv_obj_t *parent)
{
    ls_ui_style_content(parent);

    lv_obj_t *strap = ls_ui_panel(parent, nullptr);
    lv_obj_set_flex_flow(strap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strap, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(strap, LV_OBJ_FLAG_SCROLLABLE);

    _p_lamp = ls_ui_lamp(strap, LS_UI_COLOR_ACCENT);
    /* Kill the glow. lv_led draws its halo as a SHADOW, and the
       default shadow width is far wider than the 16x16 widget - it spilled
       out of the strap and printed over the text beside it. A sync lamp only
       has to be on or off; it does not need to bleed. */
    _p_strap = sdr_label(strap, &lv_font_montserrat_16, COL_TEXT);
    lv_obj_set_flex_grow(_p_strap, 1);
    lv_label_set_text(_p_strap, "HUNT  baud 1200");

    {
    _p_baud = ls_ui_button(strap, "BAUD", LS_BTN_DEFAULT, baudCb, this, nullptr);
    }

    lv_obj_t *cp = sdr_panel(parent);
    _p_counts = mono(cp, COL_LABEL);
    lv_obj_set_width(_p_counts, lv_pct(100));
    lv_label_set_text(_p_counts, "frames 0   pages 0   cw-err 0");

    sdr_section(parent, "DECODED PAGES (newest first)");

    lv_obj_t *logbox = lv_obj_create(parent);
    lv_obj_set_width(logbox, lv_pct(100));
    lv_obj_set_flex_grow(logbox, 1);
    ls_ui_style_scroll_panel(logbox);
    lv_obj_set_scroll_dir(logbox, LV_DIR_VER);
    /* The box scrolled but never showed a bar, so there was no way to
       tell there was more above or below. Force it visible and give it enough
       width and contrast to be usable with a finger. */
    lv_obj_set_scrollbar_mode(logbox, LV_SCROLLBAR_MODE_ON);

    _p_log = sdr_label(logbox, sdr_font_mono(), COL_TEXT);
    lv_obj_set_width(_p_log, lv_pct(100));
    lv_label_set_long_mode(_p_log, LV_LABEL_LONG_WRAP);
    lv_label_set_text(_p_log, "(listening for pages...)");
}

void AppFM::updatePages(void)
{
    ls_iq_control_status_t radio;
    ls_receiver_presentation_t receiver;
    fm_get_receiver_status(&radio);
    ls_receiver_present(&radio, &receiver);
    bool flex = FM.mode == FM_MODE_FLEX;
    bool sync = receiver.available &&
                (flex ? FM.flex_sync : FM.pocsag_sync);
    if (_p_lamp) { if (sync) lv_led_on(_p_lamp); else lv_led_off(_p_lamp); }
    if (_p_baud) {
        if (flex) lv_obj_add_flag(_p_baud, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_clear_flag(_p_baud, LV_OBJ_FLAG_HIDDEN);
    }
    if (_p_strap) {
        if (!receiver.available) {
            /**/
            lv_label_set_text_fmt(_p_strap, "%s\n%s",
                                  receiver.connection, receiver.frequency);
        } else if (flex) {
            unsigned mode = FM.flex_mode < 4 ? FM.flex_mode : 0;
            lv_label_set_text_fmt(_p_strap, "FLEX %s   %s   %s\nHUNT: 1600/2  3200/2  3200/4  6400/4",
                sync ? "SYNC" : "HUNT", FLEX_RATE_LEVEL[mode], receiver.frequency);
        } else {
            char bs[24]; fm_baud_str(bs, sizeof(bs));
            /* One line could not hold status, baud and frequency beside
               the BAUD button, so LVGL broke it wherever it ran out - mid
               number, "152." on one line and "6000 MHz" on the next. Break it
               deliberately instead, the way the FLEX branch above already
               does: the frequency gets its own line and the strap is always
               two lines, so its height no longer changes as values do. */
            lv_label_set_text_fmt(_p_strap, "POCSAG %s   %s\n%s",
                sync ? "SYNC" : "HUNT", bs, receiver.frequency);
        }
        lv_obj_set_style_text_color(_p_strap,
            !receiver.available || receiver.tune_attention ? COL_RED
                                                           : sync ? COL_GREEN : COL_DIM, 0);
    }
    if (_p_counts) {
        if (flex) {
            lv_label_set_text_fmt(_p_counts, "FLEX frames %lu  pages %lu  cw-err %lu  near %d",
                (unsigned long)FM.flex_frames, (unsigned long)FM.flex_pages,
                (unsigned long)FM.flex_cw_errs, FM.flex_near_min);
        } else {
            lv_label_set_text_fmt(_p_counts,
                "POCSAG frames %lu  pages %lu  cw-err %lu\naddr %lu  msg %lu  (msg=0 -> tone only)",
                (unsigned long)FM.pocsag_frames, (unsigned long)FM.pocsag_pages,
                (unsigned long)FM.pocsag_cw_errs,
                (unsigned long)FM.pocsag_addr, (unsigned long)FM.pocsag_msg);
        }
    }
    if (_p_log) {
        /* 700 B held barely two pages once messages got long, so the
           log looked empty even when the ring was full. */
        char buf[1600]; int off = 0;
        int n = FM.page_count;
        for (int k = 0; k < n && off < (int)sizeof(buf) - 180; k++) {
            int idx = (FM.page_head - 1 - k + FM_PAGE_LOG_MAX * 2) % FM_PAGE_LOG_MAX;
            const fm_page_t *p = &FM.pages[idx];
            /*'?' is a real outcome, not a tone: the page decoded but
               neither the alphanumeric nor the numeric reading was convincing.
               Showing it as TONE hid the difference between "this pager sent
               no message" and "we could not read the message". */
            const char *tn = (p->type == 'A') ? "ALPHA"
                           : (p->type == 'N') ? "NUM"
                           : (p->type == '?') ? "RAW?"
                                              : "TONE";
            /* Real time if it was known when the page landed, else
               the uptime marker. Rendered through ls_time_render_stamp_at
               so the same "no plausible-looking wrong date" property the
               bench pins is what the panel shows. */
            char stamp[LS_TIME_STAMP_MAX];
            ls_time_render_stamp_at(stamp, sizeof(stamp),
                                    (time_t)p->ts_epoch, p->ts_us);
            if (p->protocol == FM_PAGE_PROTOCOL_FLEX) {
                off += snprintf(buf + off, sizeof(buf) - off,
                                "FLEX    %s  ADDR %lu  %d %s  %s\n",
                                stamp, (unsigned long)p->address,
                                p->baud, tn, p->text);
            } else {
                off += snprintf(buf + off, sizeof(buf) - off,
                                "POCSAG  %s  RIC %lu  F%d  %d %s  %s\n",
                                stamp, (unsigned long)p->address,
                                p->function, p->baud, tn, p->text);
            }
        }
        if (off == 0) snprintf(buf, sizeof(buf), "(listening for pages...)");
        lv_label_set_text(_p_log, buf);
    }
}

void AppFM::buildScanTab(lv_obj_t *parent)
{
    ls_ui_style_content(parent);

    lv_obj_t *ip = ls_ui_panel(parent, "SWEEP STATUS");
    _s_info = mono(ip, COL_TEXT);
    lv_obj_set_width(_s_info, lv_pct(100));
    lv_label_set_text(_s_info, "band --");

    _s_peak = sdr_label(ip, &lv_font_montserrat_18, COL_GOLD);
    lv_obj_set_width(_s_peak, lv_pct(100));
    lv_label_set_text(_s_peak, "PEAK --");

    lv_obj_update_layout(parent);
    int width = lv_obj_get_content_width(parent);
    int height = lv_disp_get_ver_res(lv_obj_get_disp(parent)) / 3;
    if (height > LS_SPECTRUM_CANVAS_MAX_HEIGHT)
        height = LS_SPECTRUM_CANVAS_MAX_HEIGHT;
    (void)ls_spectrum_waterfall_build(
        &_s_spectrum, parent, "SPECTRUM / WATERFALL  (newest sweep at top)",
        width, height, FM_SCAN_BINS_MAX, 50, 100, false, nullptr, nullptr);
    ls_spectrum_waterfall_add_controls(
        &_s_spectrum,
        LS_SPECTRUM_CTL_SPLIT | LS_SPECTRUM_CTL_CONTRAST |
            LS_SPECTRUM_CTL_FULL | LS_SPECTRUM_CTL_GAIN,
        gainDownCb, gainUpCb, this, nullptr, nullptr);

    lv_obj_t *actions = ls_ui_controls(parent);
    lv_obj_t *br = ls_ui_button_group(actions);
    ls_ui_group_button(br, "BAND", LS_BTN_DEFAULT, bandCb, this, nullptr);
    ls_ui_group_button(br, "RESTART", LS_BTN_DEFAULT, scanRestartCb, this, nullptr);
    ls_ui_group_button(br, "FREQ PEAK", LS_BTN_PRIMARY, tunePeakCb, this, nullptr);

    /* The seed height above is a third of the panel and nothing ever
       revisited it, so the sweep panel stood 406 px tall in a 486 px page and
       the GAIN row sat past the bottom edge. This has to run after the action
       row exists or the first measurement hands the canvas the row's height
       as well and the page overflows again on the way in. */
    ls_spectrum_waterfall_fit_height(&_s_spectrum);

    _wf_sweep = FM.scan_sweeps;
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
    if (FM.scan_sweeps != _wf_sweep) {
        _wf_sweep = FM.scan_sweeps;
        int bins = FM.scan_bins;
        if (bins > FM_SCAN_BINS_MAX) bins = FM_SCAN_BINS_MAX;
        ls_spectrum_waterfall_push(&_s_spectrum,
                                   bins > 0 ? FM.scan_db : nullptr, bins);
    }
    char gain[24];
    if (FM.gain_tenths == 0) snprintf(gain, sizeof(gain), "GAIN AGC");
    else snprintf(gain, sizeof(gain), "GAIN %d.%d", FM.gain_tenths / 10,
                  FM.gain_tenths % 10);
    ls_spectrum_waterfall_set_gain_text(&_s_spectrum, gain);
}

void AppFM::buildConfigTab(lv_obj_t *parent)
{
    ls_ui_style_content(parent);

    sdr_setrow_t r;

    sdr_section(parent, "RADIO");

    sdr_setting_row(parent, "FREQUENCY", &r);
    _c_freq = r.value;
    ls_ui_button(r.controls, "-1M", LS_BTN_DEFAULT, tuneDeltaCb, (void *)(intptr_t)(-1000000), nullptr);
    ls_ui_button(r.controls, "-25k", LS_BTN_DEFAULT, tuneDeltaCb, (void *)(intptr_t)(-25000), nullptr);
    ls_ui_button(r.controls, "+25k", LS_BTN_DEFAULT, tuneDeltaCb, (void *)(intptr_t)(25000), nullptr);
    ls_ui_button(r.controls, "+1M", LS_BTN_DEFAULT, tuneDeltaCb, (void *)(intptr_t)(1000000), nullptr);

    sdr_setting_row(parent, "GAIN", &r);
    _c_gain = r.value;
    ls_ui_button(r.controls, "-", LS_BTN_DEFAULT, gainDownCb, this, nullptr);
    ls_ui_button(r.controls, "+", LS_BTN_DEFAULT, gainUpCb, this, nullptr);
    ls_ui_button(r.controls, "STEP", LS_BTN_DEFAULT, gainCb, this, nullptr);
    ls_ui_button(r.controls, "AGC", LS_BTN_TOGGLE_OFF, agcCb, this, nullptr);

    lv_obj_t *gl = nullptr;
    _c_gain_slider = sdr_seg_slider(parent, SDR_ROLE_COLOR(LS_UI_COLOR_ID_TEAL), 496, FM.gain_tenths,
                                    fm_seg_gain_live, this, &gl);
    sdr_seg_use_steps(_c_gain_slider,10);
    sdr_seg_on_release(_c_gain_slider, fm_seg_gain_commit);
    if (gl) lv_label_set_text(gl, "MANUAL GAIN  (drag; left = AGC)");

    sdr_setting_row(parent, "SQUELCH", &r);
    _c_sql = r.value;
    ls_ui_button(r.controls, "-", LS_BTN_DEFAULT, sqDownCb, this, nullptr);
    ls_ui_button(r.controls, "+", LS_BTN_DEFAULT, sqUpCb, this, nullptr);

    sdr_section(parent, "POCSAG");

    sdr_setting_row(parent, "BAUD", &r);
    _c_baud = r.value;
    ls_ui_button(r.controls, "CYCLE", LS_BTN_DEFAULT, baudCb, this, nullptr);

    sdr_section(parent, "SCAN");

    sdr_setting_row(parent, "BAND", &r);
    _c_band = r.value;
    ls_ui_button(r.controls, "NEXT", LS_BTN_DEFAULT, bandCb, this, nullptr);

    sdr_section(parent, "AUDIO");

    _c_vol_slider = sdr_seg_slider(parent, SDR_PAS_GREEN, 100, audio_volume_get(),
                                   fm_seg_vol, this, &_c_vol_lbl);
    sdr_seg_use_steps(_c_vol_slider,5);

    sdr_setting_row(parent, "MUTE", &r);
    _c_mute = r.value;
    ls_ui_button(r.controls, "TOGGLE", LS_BTN_TOGGLE_OFF, muteCb, this, nullptr);

    sdr_section(parent, "DIAGNOSTICS");

    lv_obj_t *dp = sdr_panel(parent);
    _c_diag = mono(dp, COL_LABEL);
    lv_obj_set_width(_c_diag, lv_pct(100));
    lv_label_set_text(_c_diag, "");

    updateConfig();

    /**/
    sdr_section(parent, "DEFAULTS");
    sdr_setting_row(parent, "RESET THIS APP", &r);
    _reset_val = r.value;
    lv_label_set_text(_reset_val, "");
    sdr_hold_btn(r.controls, "HOLD 2", 2000, resetCb, this);
}

void AppFM::updateConfig(void)
{
    char b[96];
    ls_iq_control_status_t radio;
    ls_receiver_presentation_t receiver;
    fm_get_receiver_status(&radio);
    ls_receiver_present(&radio, &receiver);
    if (_c_freq) {
        lv_label_set_text(_c_freq, receiver.frequency);
        lv_obj_set_style_text_color(_c_freq,
            receiver.tune_attention ? COL_RED : COL_TEXT, 0);
    }
    if (_c_gain) {
        lv_label_set_text(_c_gain, receiver.gain);
        lv_obj_set_style_text_color(_c_gain,
            receiver.gain_attention ? COL_RED : COL_TEXT, 0);
    }
    sdr_seg_set(_c_gain_slider, FM.gain_tenths);
    if (_c_sql)  lv_label_set_text_fmt(_c_sql, "%d", FM.squelch_tenths);
    if (_c_baud) { char bs[24]; fm_baud_str(bs, sizeof(bs)); lv_label_set_text(_c_baud, bs); }
    if (_c_band) lv_label_set_text(_c_band, BANDS[s_band_idx].name);
    if (_c_vol_lbl) lv_label_set_text_fmt(_c_vol_lbl, "VOLUME  %d", audio_volume_get());
    sdr_seg_set(_c_vol_slider, audio_volume_get());
    if (_c_mute) lv_label_set_text(_c_mute, audio_is_muted() ? "MUTED" : "ON");

    if (_c_diag) {
        lv_label_set_text_fmt(_c_diag,
            "DEVICE   %s\nIQ RATE  %lu KB/s\nIQ PEAK  %d%%   ACT %d%%\nREAD ERR %lu\nDEMOD    %d Hz   AUDIO %d Hz",
            receiver.connection,
            (unsigned long)(FM.iq_bytes_sec / 1024),
            clampi((int)(FM.iq_level * 100.0f), 0, 100),
            clampi((int)(FM.audio_level / 0.65f * 100.0f), 0, 100),
            (unsigned long)FM.read_errors,
            FM_DEMOD_RATE, FM_AUDIO_RATE);
        lv_obj_set_style_text_color(_c_diag, receiver.available ? COL_GREEN : COL_RED, 0);
    }
}

void AppFM::timerCb(lv_timer_t *t)
{
    AppFM *self = static_cast<AppFM *>(t->user_data);
    if (!self || !self->_tabview) return;
    /**/

    if (self->_last_mode != (int)FM.mode) {
        self->_last_mode = (int)FM.mode;
        if (FM.mode == FM_MODE_SCAN)
            lv_tabview_set_act(self->_tabview, TAB_SWEEP, LV_ANIM_OFF);
    }

    /**/
    /* These cases are TAB INDICES and they shifted when SCAN was inserted at
       1. If a tab is ever added or reordered again, this switch and the N in
       switchTab() both have to move with it - there is no compile-time link
       between them and the tab order in run(). */
    switch (lv_tabview_get_tab_act(self->_tabview)) {
        case TAB_VFO:    self->updateVfo();           break;
        case TAB_SCAN:   self->_scan_panel.refresh(); break;
        case TAB_PAGES:  self->updatePages();         break;
        case TAB_SWEEP:  self->updateScan();          break;
        case TAB_CONFIG: self->updateConfig();        break;
        default: break;
    }
}

void AppFM::switchTab(int delta)
{
    if (!_tabview) return;
    /**/
    const int N = TAB_COUNT;
    int cur = (int)lv_tabview_get_tab_act(_tabview);
    lv_tabview_set_act(_tabview, (cur + delta + N) % N, LV_ANIM_OFF);
}

/**/
void AppFM::modeCb(lv_event_t *)
{
    int cur = lakeshark_fm_get_mode();
    int at  = -1;
    for (int i = 0; i < MODE_CYCLE_N; i++) if ((int)MODE_CYCLE[i] == cur) { at = i; break; }
    /* Cycling out of SWEEP (or anything not in the list) lands on NFM. */
    int next = (at < 0) ? 0 : (at + 1) % MODE_CYCLE_N;
    lakeshark_fm_set_mode((int)MODE_CYCLE[next]);
}

/**/

void AppFM::scanToggleCb(lv_event_t *)
{
    if (scan_engine_active()) {
        scan_engine_stop();
    } else {
        /* A band sweep and a channel sweep both own the tuner; running both
           means neither works. Starting one stops the other, visibly. */
        if (FM.mode == FM_MODE_SCAN) lakeshark_fm_set_mode(FM_MODE_LISTEN);
        scan_engine_start();
    }
}

/**/
/* SKIP - step off a channel the scanner is sitting on. The engine has had
   scan_engine_skip() since and it was only ever reachable from the
   console. */
void AppFM::scanSkipCb(lv_event_t *)
{
    if (scan_engine_active()) scan_engine_skip();
}

/**/
/* Measure the floor and set squelch above it. Asynchronous - the calibration
   sweep runs on the scan task, so this returns instantly and the result turns
   up in the scanner status line a second or so later. */
void AppFM::autoSqCb(lv_event_t *)
{
    scan_engine_autosquelch(-1);
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
/**/
/**/
/* This is one of two ways into the band sweep now that MODE cycles demodulators
   only. It must stop the CHANNEL scanner first: since that scanner
   forces the FM app into LISTEN on every pass, so leaving it running here
   would drag the app straight back out of SWEEP and the button would look
   dead. The stop / set-mode / restart order lives in fm_sweep_arbitration.c
   so BAND (below) cannot drift out of sync with RESTART again. */
void AppFM::scanRestartCb(lv_event_t *)
{
    fm_sweep_start_arbitrated();
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

/**/
/* Cycle to the next band, then hand off to the shared sweep arbitration so the
   channel scanner is stopped before FM enters SWEEP. Before this path
   skipped the stop and BAND looked dead while the channel scanner was on -
   scan_engine forces FM_MODE_LISTEN on every pass and the sweep was replaced
   before the first bin ever rendered. */
void AppFM::bandCb(lv_event_t *)
{
    s_band_idx = (s_band_idx + 1) % BAND_N;
    FM.scan_start_hz = BANDS[s_band_idx].a;
    FM.scan_stop_hz  = BANDS[s_band_idx].b;
    FM.scan_step_hz  = BANDS[s_band_idx].step;
    fm_sweep_start_arbitrated();
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
    /**/
    /* The entry field and the keypad were the only things in this firmware
       still wearing LVGL's stock theme, which is light - so punching in a
       frequency at night blew out night vision on an otherwise dark radio.
       Styled from the same SDR_* palette as every other panel, and sized up:
       montserrat_32 for the readout, _24 for the keys. This is a 480 px wide
       screen and the stock key font is tiny for a thumb. */
    const ls_text_entry_config_t config = {
        .title = "ENTER FREQUENCY (MHz)  -  e.g. 152.600",
        .text = "",
        .placeholder = "152.600",
        .accepted_chars = "0123456789.",
        .max_length = 10,
        .width = lv_pct(90),
        .mode = LS_TEXT_ENTRY_NUMBER,
        .large = true,
    };
    _freq_entry = ls_text_entry_open(&config, freqEntryDone, this);
}

void AppFM::freqEntryDone(bool accepted, const char *text, void *user_data)
{
    AppFM *self = static_cast<AppFM *>(user_data);
    if (!self) return;
    self->_freq_entry = nullptr;
    if (accepted) {
        double mhz = atof(text);
        if (mhz >= 1.0 && mhz <= 2000.0)
            lakeshark_fm_set_freq((uint32_t)(mhz * 1e6 + 0.5));
    }
}

void AppFM::closeFreqEntry(void)
{
    if (_freq_entry) {
        ls_text_entry_t *entry = _freq_entry;
        _freq_entry = nullptr;
        ls_text_entry_close(entry);
    }
}

/**/
void AppFM::resetCb(lv_event_t *e)
{
    AppFM *self = static_cast<AppFM *>(lv_event_get_user_data(e));
    settings_reset_app(app_current());
    lakeshark_radio_park();
    lakeshark_select_fm();
    if (self && self->_reset_val) {
        lv_label_set_text(self->_reset_val, "RESTORED");
        lv_obj_set_style_text_color(self->_reset_val, SDR_OK, 0);
    }
}
