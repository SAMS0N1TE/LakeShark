#include "rec_gui/AppREC.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "sdr_ui/sdr_ui.h"
#include "shell/ls_shell.hpp"
#include "ui/ls_ui.h"
#include "ui/ls_receiver_status.h"

extern "C" {
#include "lakeshark_backend.h"
#include "rec_state.h"
#include "rec_storage_label.h"
/*LS-961*/
#include "rec_space.h"
/*LS-560*/
#include "rec_scout_span.h"
/*LS-963*/
#include "spectrum.h"
/*LS-984*/
#include "nvs.h"
#include "nvs_flash.h"
}

/*LS-963*/
#include "esp_timer.h"

/*LS-020*/

#define COL_BG    SDR_BG
#define COL_PANEL SDR_PANEL
#define COL_TEXT  SDR_TEXT

/*LS-020*/
#define MAG_FULL_SCALE 256

/*LS-028*/
/* The R820T's tuning range. These were the two literals inside freq_nudge();
   the keypad has to clamp to exactly the same window or typing a number would
   accept what the < > buttons refuse to walk to. */
#define REC_FREQ_MIN_HZ   24000000UL
#define REC_FREQ_MAX_HZ 1766000000UL

/*LS-963*/
/* Named indices for the tab strip.  updateRecord/updateScout/updateConfig
   dispatch on the active index and switchTab wraps on the count - three
   places that must agree with an order expressed nowhere else. */
enum {
    REC_TAB_RECORD = 0,
    REC_TAB_SCOUT,
    REC_TAB_CONFIG,
    REC_TAB_FILES,
    REC_TAB_COUNT
};

/*LS-963  The display span is a crop of the native ~200 kHz window.  The
   tuner does not move when this changes: 200 kHz shows the passband and
   25 kHz resolves individual FSK deviation lobes.  The pure ladder and its
   edge arithmetic live in rec_scout_span.c so the host bench covers the
   controls that manipulate it. */
static void scout_span_label(lv_obj_t *label, int level)
{
    if (!label) return;
    lv_label_set_text_fmt(label, "SPAN %lu kHz",
                          (unsigned long)(rec_scout_span_hz(level) / 1000u));
}

/*LS-984*/
/* SPLIT snap points: 0/25/50/75/100 percent of the chart+waterfall area
   given to the spectrum.  A discrete set stops SPLIT+/SPLIT- becoming a
   fine-adjust exercise on a thumb, and 0/100 give the two operators who
   only ever want one of the two views a one-tap shortcut. */
/*LS-984  NVS namespace for SCOUT operator preferences.  Split and
   fullscreen mode are the only settings and they belong to the SCOUT
   tab specifically - not global "settings", not per-app freq/gain,
   which already live in the settings module. */
static const char *SCOUT_NVS_NS   = "rec-scout";
static const char *SCOUT_KEY_SPLIT = "split";
static const char *SCOUT_KEY_FULL  = "full";
static const char *SCOUT_KEY_CONTRAST = "contrast";

struct rec_preset_t { const char *name; uint32_t hz; };

/*LS-024*/
/*LS-030*/
/* 434.07 IS FIRST BECAUSE IT IS THE ONLY ONE IN THIS LIST CONFIRMED ON AIR BY
   THIS RECEIVER - two captures, 350 and 500 edges, both ended on gap. Every
   other entry is a plausible number off a datasheet or a retracted note.
   868.35 is KEPT BUT DEMOTED: it is the 2nd harmonic of a ~434 fundamental,
   not a transmitter, and tuning there records nothing. See LS-030. */
static const rec_preset_t REC_PRESETS[] = {
    { "LIGHT 434.07",  434070000UL },
    { "TX rem 433.66", 433660000UL },
    { "TX rem 433.89", 433890000UL },
    { "OOK 433.92",    433920000UL },
    { "OOK 434.42",    434420000UL },
    { "OOK 315.00",    315000000UL },
    { "harm 868.35",   868350000UL },
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

static void rec_receiver_present(const rec_status_t *status,
                                 ls_receiver_presentation_t *out)
{
    ls_iq_control_status_t radio = {};
    radio.requested_center_hz = status->freq_hz;
    radio.effective_center_hz = status->effective_freq_hz;
    radio.requested_gain_tenths_db = status->gain_tenths;
    radio.effective_gain_tenths_db = status->effective_gain_tenths;
    radio.effective_center_known = status->effective_freq_known;
    radio.effective_gain_known = status->effective_gain_known;
    radio.tune_state = status->tune_state;
    radio.gain_state = status->gain_state;
    radio.tune_error = status->tune_error;
    radio.gain_error = status->gain_error;
    radio.receiver_streaming = status->receiver_streaming;
    radio.receiver_error = status->receiver_error;
    ls_receiver_present(&radio, out);
}

AppREC::AppREC()
    : LsApp("REC", "rec")
{
}

AppREC::~AppREC() = default;

bool AppREC::init(void)  { return true; }

/*LS-600*/
bool AppREC::pause(void)
{
    /*LS-028*/
    closeFreqEntry();
    if (_timer) lv_timer_pause(_timer);
    /*LS-963*/
    rec_scout_enable(false);
    lakeshark_radio_park();
    return true;
}

/*LS-604*/
bool AppREC::background(void)
{
    /*LS-028*/
    closeFreqEntry();
    if (_timer) lv_timer_pause(_timer);
    /*LS-963*/
    rec_scout_enable(false);
    return true;
}

/*LS-600*/
bool AppREC::resume(void)
{
    lakeshark_select_rec();
    if (_timer) lv_timer_resume(_timer);
    return true;
}

bool AppREC::back(void)
{
    /*LS-028*/
    if (_freq_entry) { closeFreqEntry(); return true; }
    return exitToLauncher();
}

bool AppREC::close(void)
{
    /*LS-028*/
    closeFreqEntry();
    /*LS-963*/
    rec_scout_enable(false);
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
    _tabview     = nullptr;
    _files_table = nullptr;
    ls_spectrum_waterfall_forget(&_scout_spectrum);
    /*LS-984*/
    _scout_chrome    = nullptr;
    _scout_area      = nullptr;
    _scout_rec_lbl   = nullptr;
    _scout_row1      = nullptr;
    _scout_row2      = nullptr;
    lakeshark_radio_park();
    return true;
}

void AppREC::switchTab(int delta)
{
    if (!_tabview) return;
    int cur = (int)lv_tabview_get_tab_act(_tabview);
    cur = (cur + delta + REC_TAB_COUNT) % REC_TAB_COUNT;
    lv_tabview_set_act(_tabview, (uint16_t)cur, LV_ANIM_OFF);
}

bool AppREC::run(lv_obj_t *parent)
{
    lakeshark_select_rec();

    ls_ui_screen_t screen;
    ls_ui_screen_create(parent, "REC", true, LS_UI_COLOR_ID_ORANGE, &screen);
    _screen_readout = screen.readout;
    _screen_lamp = screen.lamp;
    ls_ui_screen_set_readout(&screen, "433.9200 MHz");

    /*LS-984*/
    /* Every span-a-column dimension in this tab derives from the
       display.  A literal 240 shipped as a waterfall on half the panel
       and disagreed with a neighbouring 460 assumption for the chart -
       so the two views drew to different widths without noticing. */
    int hor = lv_disp_get_hor_res(NULL);
    int ver = lv_disp_get_ver_res(NULL);
    if (hor <= 0) hor = 480;
    if (ver <= 0) ver = 800;

    /* Waterfall spans the full tab width minus the tab-content padding
       either side (see buildScoutTab pad_all).  This is what "fill the
       panel" means in pixels on this build. */
    _wf_w = hor - 12;
    if (_wf_w < 128)  _wf_w = 128;

    /* Spectral resolution matched to display width - two pixels per
       bin reads well without blowing the FFT bin count out of what
       rec_scout_read is willing to hand back. */
    _scout_bins = _wf_w / 2;
    if (_scout_bins < 120) _scout_bins = 120;
    if (_scout_bins > 240) _scout_bins = 240;

    /* Allocation ceiling comes from the panel.  The visible height is not
       estimated here: the chart+waterfall object flex-grows into whatever
       remains after the wrapping controls have laid themselves out. */
    int chrome_h = 44 /* tabstrip */ + SDR_STATUS_H + SDR_RAIL_H + 20;
    int area = ver - chrome_h;
    if (area < 120) area = 120;
    if (area > LS_SPECTRUM_CANVAS_MAX_HEIGHT)
        area = LS_SPECTRUM_CANVAS_MAX_HEIGHT;
    _scout_area_cap_h = area;

    scoutLoadPrefs();

    _tabview = screen.tabs;

    buildRecordTab(ls_ui_screen_add_tab(&screen, "RECORD"));
    /*LS-963  SCOUT sits next to RECORD because it is the same job -
       find something worth capturing, then capture it.  Placing it
       further right would push the operator to type a frequency they
       could have just seen a peak at. */
    buildScoutTab (ls_ui_screen_add_tab(&screen, "SCOUT"));
    buildConfigTab(ls_ui_screen_add_tab(&screen, "CONFIG"));
    buildFilesTab (ls_ui_screen_add_tab(&screen, "FILES"));

    _timer = lv_timer_create(timerCb, 200, this);
    return true;
}

void AppREC::timerCb(lv_timer_t *t)
{
    AppREC *self = (AppREC *)t->user_data;
    if (!self->_tabview) return;

    /*LS-963  Scout is a live view; enable it only while its tab is up.
       When the operator moves to RECORD/CONFIG/FILES the rx task stops
       paying for the FFT.  This edge trigger keeps the enable/disable
       out of every tick. */
    int cur = (int)lv_tabview_get_tab_act(self->_tabview);
    bool want_scout = (cur == REC_TAB_SCOUT);
    if (want_scout != rec_scout_enabled()) rec_scout_enable(want_scout);

    switch (cur) {
        case REC_TAB_RECORD: self->updateRecord(); break;
        case REC_TAB_SCOUT:  self->updateScout();  break;
        case REC_TAB_CONFIG: self->updateConfig(); break;
        default: break;
    }
}

/*LS-963  RECORD in the LCD-face style AppP25 uses on DECODE.  What was
   here before was controls stacked in the order they were added: an
   ALL-CAPS "IDLE" label, then a bar with no context, then five lines
   of stats, then three unlabelled buttons.  This is the app most used
   away from a desk and it read like a test harness. */
void AppREC::buildRecordTab(lv_obj_t *parent)
{
    ls_ui_style_content(parent);

    _rec_face = sdr_lcd_panel(parent, SDR_ROLE_COLOR(LS_UI_COLOR_ID_ORANGE));

    lv_obj_t *strap = lv_obj_create(_rec_face);
    lv_obj_set_size(strap, lv_pct(100), LV_SIZE_CONTENT);
    ls_ui_style_lcd_row(strap);
    lv_obj_set_flex_flow(strap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strap, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(strap, LV_OBJ_FLAG_SCROLLABLE);

    _rec_phase_lbl = sdr_label(strap, &lv_font_montserrat_28, SDR_ROLE_COLOR(LS_UI_COLOR_ID_ORANGE));
    lv_obj_set_style_text_letter_space(_rec_phase_lbl, 2, 0);
    lv_label_set_text(_rec_phase_lbl, "IDLE");

    lv_obj_t *rxbox = lv_obj_create(strap);
    lv_obj_set_size(rxbox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    ls_ui_style_lcd_row(rxbox);
    lv_obj_set_flex_flow(rxbox, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rxbox, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(rxbox, LV_OBJ_FLAG_SCROLLABLE);
    _rec_lamp = lv_led_create(rxbox);
    lv_obj_set_size(_rec_lamp, 14, 14);
    lv_led_set_color(_rec_lamp, SDR_PAS_GREEN);
    lv_led_off(_rec_lamp);
    /* LS-827  Kill the shadow spread - the LVGL LED default is far
       wider than a 14 px widget and bleeds across the strap. */
    lv_obj_set_style_shadow_width(_rec_lamp, 0, 0);
    lv_obj_set_style_shadow_spread(_rec_lamp, 0, 0);
    _rec_rx = sdr_label(rxbox, &lv_font_montserrat_22, SDR_DIM);
    lv_label_set_text(_rec_rx, "RX");

    _rec_freq = sdr_label(_rec_face, &lv_font_montserrat_48, SDR_ROLE_COLOR(LS_UI_COLOR_TEXT));
    lv_obj_set_width(_rec_freq, lv_pct(100));
    lv_obj_set_style_text_align(_rec_freq, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(_rec_freq, 2, 0);
    lv_label_set_text(_rec_freq, "433.9200");
    lv_obj_add_flag(_rec_freq, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_rec_freq, freqEntryCb, LV_EVENT_CLICKED, this);

    _rec_sub = sdr_label(_rec_face, &lv_font_montserrat_16, SDR_PAS_CYAN);
    lv_obj_set_width(_rec_sub, lv_pct(100));
    lv_obj_set_style_text_align(_rec_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(_rec_sub, "MHz");

    /*LS-023*/
    _rec_magbar = sdr_label(_rec_face, sdr_font_mono(), SDR_PAS_CYAN);
    lv_obj_set_width(_rec_magbar, lv_pct(100));
    lv_label_set_text(_rec_magbar, "");

    sdr_section(parent, "CAPTURE");

    _rec_stats = sdr_label(parent, sdr_font_mono(), SDR_TEXT);
    lv_obj_set_width(_rec_stats, lv_pct(100));
    lv_label_set_text(_rec_stats, "");

    _rec_result = sdr_label(parent, sdr_font_mono(), SDR_DIM);
    lv_obj_set_width(_rec_result, lv_pct(100));
    lv_label_set_text(_rec_result, "");

    _rec_file = sdr_label(parent, sdr_font_mono(), SDR_ROLE_COLOR(LS_UI_COLOR_TEXT));
    lv_obj_set_width(_rec_file, lv_pct(100));
    lv_label_set_text(_rec_file, "");

    lv_obj_t *row = ls_ui_controls(parent);
    ls_ui_button(row, "ARM", LS_BTN_PRIMARY, armCb, this, nullptr);
    ls_ui_button(row, "STOP", LS_BTN_DANGER, stopCb, this, nullptr);
    ls_ui_button(row, "SAVE", LS_BTN_PRIMARY, saveCb, this, nullptr);
    /*LS-963*/
    ls_ui_button(row, "SCOUT", LS_BTN_DEFAULT, toScoutCb, this, nullptr);

    updateRecord();
}

void AppREC::updateRecord(void)
{
    if (!_rec_phase_lbl) return;

    rec_status_t st;
    rec_get_status(&st);
    ls_receiver_presentation_t receiver;
    rec_receiver_present(&st, &receiver);

    char b[192];

    /* LCD-face strap: phase name in phase-tinted colour, RX lamp lit
       when the detector believes a carrier is up.  A capture is a
       hard-to-miss event; DONE glows green until the operator arms
       again. */
    lv_label_set_text(_rec_phase_lbl,
                      receiver.available ? phase_name(st.phase)
                                         : receiver.connection);
    lv_obj_set_style_text_color(_rec_phase_lbl,
        receiver.available ? phase_color(st.phase) : SDR_PAS_ROSE, 0);

    bool live = receiver.available &&
                (st.mag_thresh > 0 && st.mag_now >= st.mag_thresh);
    const char *rxtxt = !receiver.available ? "NO RX" : live ? "HOT" :
                        st.phase == REC_ARMED     ? "ARM" :
                        st.phase == REC_CAPTURING ? "REC" :
                        st.phase == REC_DONE      ? "OK"  : "RX";
    lv_color_t rxc  = !receiver.available ? SDR_PAS_ROSE
                         : live ? SDR_PAS_GREEN : phase_color(st.phase);
    lv_label_set_text(_rec_rx, rxtxt);
    lv_obj_set_style_text_color(_rec_rx, rxc, 0);
    lv_led_set_color(_rec_lamp, rxc);
    if (receiver.available &&
        (live || st.phase == REC_ARMED || st.phase == REC_CAPTURING)) lv_led_on(_rec_lamp);
    else                                                            lv_led_off(_rec_lamp);

    snprintf(b, sizeof(b), "%lu.%04lu",
             (unsigned long)(st.freq_hz / 1000000UL),
             (unsigned long)((st.freq_hz / 100UL) % 10000UL));
    lv_label_set_text(_rec_freq, b);
    ls_ui_readout_set(_screen_readout, b);
    ls_ui_lamp_set(_screen_lamp,
                   receiver.available &&
                       (live || st.phase == REC_ARMED || st.phase == REC_CAPTURING),
                   st.phase == REC_CAPTURING ? LS_UI_COLOR_ALARM
                                             : LS_UI_COLOR_WARN);
    lv_obj_set_style_text_color(_rec_freq,
        !receiver.available ? SDR_PAS_ROSE
                            : live ? SDR_PAS_GREEN : phase_color(st.phase), 0);

    snprintf(b, sizeof(b), "%s    GAIN %s    bw %s",
             receiver.frequency, receiver.gain, st.bw_hz ? "" : "auto");
    if (st.bw_hz) {
        size_t off = strlen(b);
        snprintf(b + off, sizeof(b) - off, "%lu kHz",
                 (unsigned long)(st.bw_hz / 1000));
    }
    lv_label_set_text(_rec_sub, b);

    /*LS-023*/
    int mag = st.mag_now;
    if (mag < 0)              mag = 0;
    if (mag > MAG_FULL_SCALE) mag = MAG_FULL_SCALE;
    {
        char bar[96];
        int pct = (mag * 100 + MAG_FULL_SCALE / 2) / MAG_FULL_SCALE;
        rec_ascii_bar(bar, sizeof(bar), pct, sdr_bar_width(_rec_magbar, 12));
        lv_label_set_text_fmt(_rec_magbar,
                              "S %s %3d%%  thr %d",
                              bar, pct, st.mag_thresh);
        lv_obj_set_style_text_color(_rec_magbar,
            live ? SDR_PAS_GREEN : SDR_PAS_CYAN, 0);
    }

    /*LS-961*/
    /* Free-space column so the operator sees the card filling up as it
       happens; the FILES tab has the same reading, but nobody actively
       capturing is looking at FILES. */
    char free_s[16];
    if (st.bytes_free == UINT64_MAX) {
        snprintf(free_s, sizeof(free_s), "?");
    } else {
        rec_space_format_free(free_s, sizeof(free_s), st.bytes_free);
    }

    snprintf(b, sizeof(b),
             "edges    %4d / %d\n"
             "span     %lu.%03lu ms\n"
             "captures %lu      %lu B/s\n"
             "free     %s",
             st.edges, REC_MAX_EDGES,
             (unsigned long)(st.span_us / 1000),
             (unsigned long)(st.span_us % 1000),
             (unsigned long)st.captures, (unsigned long)st.bytes_sec,
             free_s);
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

/*LS-963*/
void AppREC::toScoutCb(lv_event_t *e)
{
    AppREC *self = (AppREC *)lv_event_get_user_data(e);
    if (self && self->_tabview)
        lv_tabview_set_act(self->_tabview, REC_TAB_SCOUT, LV_ANIM_OFF);
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
        /*LS-961*/
        /* -4 is the space check refusing before any file is opened.
           Name both numbers so the operator sees whether to delete
           one file or empty the card. */
        if (r == -4) {
            rec_status_t st2;
            rec_get_status(&st2);
            char detail[80];
            rec_space_format_shortage(detail, sizeof(detail),
                rec_space_estimate_bytes(st2.edges), st2.bytes_free);
            snprintf(msg, sizeof(msg), "save refused: %s", detail);
        } else {
            const char *why = (r == -3) ? "still capturing"
                            : (r == -1) ? "nothing captured yet"
                                        : "cannot open the file";
            snprintf(msg, sizeof(msg), "save failed: %s", why);
        }
        lv_label_set_text(self->_rec_file, msg);
        lv_obj_set_style_text_color(self->_rec_file, SDR_RED, 0);
        return;
    }

    snprintf(msg, sizeof(msg), "saved: %s", path);
    lv_label_set_text(self->_rec_file, msg);
    lv_obj_set_style_text_color(self->_rec_file, SDR_ROLE_COLOR(LS_UI_COLOR_TEXT), 0);
    self->refreshFiles();
}

/*LS-963  SCOUT tab.  A spectrum + waterfall over the ~200 kHz window
   centred on rec_get_freq().  The tuner does NOT move here - a scout
   view that retunes underneath the RECORD tab would be an argument the
   two tabs would have quietly and the operator would see as "REC will
   not trigger".  The zoom cycle crops the display, not the tuner.

   LS-984  Split spectrum/waterfall, fullscreen waterfall, and gain +
   ARM on the main screen.  Chrome (header/freq/sub/peak/stats/spec
   cap/wf cap) is grouped so hiding it for fullscreen is one flag flip.
   Nothing here holds a literal pixel width - _wf_w is computed in run().
*/
void AppREC::buildScoutTab(lv_obj_t *parent)
{
    ls_ui_style_content(parent);
    /*LS-792  One scroller, and it is the tab - this app has no inner body, so
       LS-736's "two scrollers confuse the operator" does not apply here. */
    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);

    /* Chrome container - one flag toggles the entire non-waterfall
       header set in and out for fullscreen mode. */
    _scout_chrome = lv_obj_create(parent);
    lv_obj_set_width(_scout_chrome, lv_pct(100));
    lv_obj_set_height(_scout_chrome, LV_SIZE_CONTENT);
    ls_ui_style_content(_scout_chrome);
    lv_obj_set_flex_flow(_scout_chrome, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(_scout_chrome, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdrp = sdr_panel(_scout_chrome);
    _scout_hdr = sdr_label(hdrp, sdr_font_mono(), SDR_PAS_CYAN);
    lv_obj_set_width(_scout_hdr, lv_pct(100));
    /*LS-784  This wrapped to a second line whenever the centre frequency or
       span grew a digit, which changed the header's height and pushed the
       whole tab - chart, waterfall, every button - down a row mid-tune. One
       clipped line keeps the layout fixed while values change. */
    lv_label_set_long_mode(_scout_hdr, LV_LABEL_LONG_DOT);
    lv_label_set_text(_scout_hdr, "SCOUT");

    _scout_freq = sdr_label(_scout_chrome, &lv_font_montserrat_28, SDR_ROLE_COLOR(LS_UI_COLOR_TEXT));
    lv_obj_set_width(_scout_freq, lv_pct(100));
    lv_obj_set_style_text_align(_scout_freq, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(_scout_freq, 2, 0);
    lv_label_set_text(_scout_freq, "433.9200");
    lv_obj_add_flag(_scout_freq, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_scout_freq, freqEntryCb, LV_EVENT_CLICKED, this);

    _scout_sub = sdr_label(_scout_chrome, &lv_font_montserrat_14, SDR_PAS_CYAN);
    lv_obj_set_width(_scout_sub, lv_pct(100));
    lv_obj_set_style_text_align(_scout_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(_scout_sub, "centre - span - MHz");

    _scout_peak_lbl = sdr_label(_scout_chrome, &lv_font_montserrat_16, SDR_ROLE_COLOR(LS_UI_COLOR_ID_ORANGE));
    lv_obj_set_width(_scout_peak_lbl, lv_pct(100));
    lv_obj_set_style_text_align(_scout_peak_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(_scout_peak_lbl, "PEAK  --");

    /* Chart+waterfall area.  Its height comes from the panel via
       flex-grown remainder of the tab, and applyScoutSplit divides that
       measured height between the two viewers.  Wrapping them in a
       sub-container makes SPLIT changes one place to touch. */
    /*LS-792  SCOUT had no scroller at all. P25 puts its content in the
       scrollable body from ls_ui_tab_split(); every other app, this one
       included, builds straight onto the tab, and ls_ui_screen_add_tab leaves
       that tab unscrollable. The spectrum widget adds its VIEW -/50-50/VIEW +,
       CONTRAST and GAIN groups inside its own panel below the chart, so on a
       fixed-height area they were simply clipped: the operator could not
       change the split, and with the waterfall squeezed to a sliver there was
       no way to get it back. Size to content and let the tab scroll. */
    _scout_area = lv_obj_create(parent);
    lv_obj_set_width(_scout_area, lv_pct(100));
    lv_obj_set_height(_scout_area, LV_SIZE_CONTENT);
    ls_ui_style_content(_scout_area);
    lv_obj_set_flex_flow(_scout_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(_scout_area, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_update_layout(parent);
    int plot_w = lv_obj_get_content_width(_scout_area);
    if (plot_w > LS_SPECTRUM_CANVAS_MAX_WIDTH)
        plot_w = LS_SPECTRUM_CANVAS_MAX_WIDTH;
    (void)ls_spectrum_waterfall_build(
        &_scout_spectrum, _scout_area, "SPECTRUM / WATERFALL",
        plot_w, _scout_area_cap_h, _scout_bins, _scout_split_pct,
        _scout_contrast_pct, _scout_full, nullptr, nullptr);
    ls_spectrum_waterfall_add_controls(
        &_scout_spectrum,
        LS_SPECTRUM_CTL_SPLIT | LS_SPECTRUM_CTL_CONTRAST |
            LS_SPECTRUM_CTL_FULL | LS_SPECTRUM_CTL_GAIN,
        scoutGainDownCb, scoutGainUpCb, this, scoutViewChanged, this);

    _scout_stats = sdr_label(_scout_chrome, sdr_font_mono(), SDR_LABEL);
    lv_obj_set_width(_scout_stats, lv_pct(100));
    lv_label_set_text(_scout_stats, "waiting for IQ");

    /*LS-560  Control groups use content height plus ROW_WRAP.  The fixed
       50 px rows from LS-984 accepted more children than their panel width,
       so LVGL placed the buttons on top of one another.  Flex now derives
       both the number of lines and their total height from the display. */
    _scout_row1 = ls_ui_controls(parent);
    lv_obj_t *freq_fine = ls_ui_button_group(_scout_row1);
    ls_ui_group_button(freq_fine, "FREQ -", LS_BTN_DEFAULT, freqDownCb, this, nullptr);
    ls_ui_group_button(freq_fine, "FREQ +", LS_BTN_DEFAULT, freqUpCb, this, nullptr);
    ls_ui_group_button(freq_fine, "FREQ SET", LS_BTN_PRIMARY, freqEntryCb, this, nullptr);
    lv_obj_t *freq_coarse = ls_ui_button_group(_scout_row1);
    ls_ui_group_button(freq_coarse, "FREQ <<", LS_BTN_DEFAULT, freqCoarseDownCb, this, nullptr);
    ls_ui_group_button(freq_coarse, "FREQ >>", LS_BTN_DEFAULT, freqCoarseUpCb, this, nullptr);
    ls_ui_group_button(freq_coarse, "FREQ PEAK", LS_BTN_PRIMARY, scoutTunePeakCb, this, nullptr);

    _scout_row2 = ls_ui_controls(parent);

    lv_obj_t *span_group = ls_ui_button_group(_scout_row2);
    lv_obj_t *span_lbl = nullptr;
    ls_ui_group_button(span_group, "SPAN", LS_BTN_DEFAULT, scoutSpanCb, this, &span_lbl);
    scout_span_label(span_lbl, _scout_zoom);
    _scout_span_lbl = span_lbl;
    ls_ui_group_button(span_group, "ZOOM -", LS_BTN_DEFAULT, scoutZoomOutCb, this, nullptr);
    ls_ui_group_button(span_group, "ZOOM +", LS_BTN_DEFAULT, scoutZoomInCb, this, nullptr);
    ls_ui_group_button(span_group, "REC", LS_BTN_DANGER, scoutArmCb, this, &_scout_rec_lbl);

    applyScoutSplit();
    scoutRefreshRecBtn();
}

void AppREC::updateScout(void)
{
    if (!_scout_hdr) return;

    rec_status_t st_ui;
    rec_get_status(&st_ui);
    ls_receiver_presentation_t receiver;
    rec_receiver_present(&st_ui, &receiver);

    uint32_t centre = st_ui.freq_hz;
    uint32_t span   = rec_scout_span_hz(_scout_zoom);
    if (span > REC_SCOUT_SPAN_HZ) span = REC_SCOUT_SPAN_HZ;

    char b[96];
    snprintf(b, sizeof(b), "%lu.%04lu",
             (unsigned long)(centre / 1000000UL),
             (unsigned long)((centre / 100UL) % 10000UL));
    lv_label_set_text(_scout_freq, b);

    if (!receiver.available) {
        lv_label_set_text(_scout_hdr, receiver.connection);
        lv_label_set_text(_scout_sub, receiver.frequency);
        lv_label_set_text(_scout_peak_lbl, "PEAK --  NO RECEIVER");
        lv_label_set_text(_scout_stats, "RECONNECTING - no IQ samples");
        ls_spectrum_waterfall_set_gain_text(&_scout_spectrum, receiver.gain);
        if (_scout_spectrum.has_data)
            ls_spectrum_waterfall_clear(&_scout_spectrum);
        scoutRefreshRecBtn();
        return;
    }

    /* subline: centre + span in kHz.  A window that starts negative
       (centre below span/2) is not reachable inside the R820T range,
       so an unsigned formula is safe. */
    uint32_t half = span / 2;
    uint32_t lo   = (centre > half) ? centre - half : 0;
    uint32_t hi   = centre + half;
    snprintf(b, sizeof(b), "%lu.%03lu - %lu.%03lu MHz    span %lu kHz",
             (unsigned long)(lo / 1000000UL),
             (unsigned long)((lo / 1000UL) % 1000UL),
             (unsigned long)(hi / 1000000UL),
             (unsigned long)((hi / 1000UL) % 1000UL),
             (unsigned long)(span / 1000u));
    lv_label_set_text(_scout_sub, b);

    uint32_t sweeps = rec_scout_sweeps();

    if (sweeps == _scout_last_sweep) {
        /* No fresh frame yet.  Leave the last drawing up; do not
           blank the chart, that reads as "signal disappeared". */
        return;
    }
    _scout_last_sweep = sweeps;

    /* Rolling FPS window: last two ticks are enough for a display
       rate.  The rx task publishes at ~15-30 Hz depending on read
       size, and the LVGL tick is 200 ms - so we sample the delta of
       sweeps between ticks. */
    int64_t now = esp_timer_get_time();
    if (_scout_last_us > 0) {
        int64_t d_us = now - _scout_last_us;
        if (d_us > 0) {
            float inst = (float)((sweeps - (sweeps - 1)) * 1000000ULL) / (float)d_us;
            (void)inst;
            /* Sweep counter delta divided by wall time between ticks. */
            static uint32_t prev_sweeps = 0;
            uint32_t d = sweeps - prev_sweeps;
            prev_sweeps = sweeps;
            float fps = (float)d * 1000000.0f / (float)d_us;
            if (_scout_fps == 0.0f) _scout_fps = fps;
            else                    _scout_fps = 0.7f * _scout_fps + 0.3f * fps;
        }
    }
    _scout_last_us = now;

    /* Pull the full-width spectrum first, then window it to the chosen
       zoom.  Reading the whole thing once and cropping the display
       means the FPS cost is constant across zoom levels.

       LS-984  Bin count now derives from the panel width (see run()),
       so the caps on the stack array match _scout_bins.  240 was the
       old cap and is still enough for the widest supported panel. */
    static float bins_full[240];
    int nbins = _scout_bins;
    if (nbins < 8)  nbins = 8;
    if (nbins > 240) nbins = 240;
    if (!rec_scout_read(bins_full, nbins)) {
        lv_label_set_text(_scout_stats, "waiting for IQ");
        if (_scout_spectrum.has_data)
            ls_spectrum_waterfall_clear(&_scout_spectrum);
        return;
    }

    /* Crop to the current zoom.  A span smaller than the native 200 kHz
       maps to a centre slice of the full array; the tuner is still
       showing 200 kHz around centre, we are only displaying a portion. */
    float frac = (float)span / (float)REC_SCOUT_SPAN_HZ;
    if (frac > 1.0f) frac = 1.0f;
    int keep  = (int)(nbins * frac + 0.5f);
    if (keep < 4) keep = 4;
    if (keep > nbins) keep = nbins;
    int start = (nbins - keep) / 2;

    float bins[240];
    for (int i = 0; i < nbins; i++) {
        int src = start + (i * keep) / nbins;
        if (src < 0) src = 0;
        if (src >= nbins) src = nbins - 1;
        bins[i] = bins_full[src];
    }

    ls_spectrum_waterfall_push(&_scout_spectrum, bins, nbins);

    /* Peak within the CROPPED window - not the backend's peak, which
       is over the whole 200 kHz.  Zooming in and having ->PEAK jump
       to a peak that is off-screen would be worse than no peak line. */
    int peak_i = 0;
    float peak_v = 0.0f;
    for (int i = 0; i < nbins; i++)
        if (bins[i] > peak_v) { peak_v = bins[i]; peak_i = i; }

    /* Map peak_i back to absolute Hz.  keep bins cover the middle
       `frac` of the passband, centred on `centre`. */
    float rel = ((float)peak_i / (float)(nbins - 1)) - 0.5f;
    int32_t off_hz = (int32_t)(rel * (float)span);
    int64_t peak_hz = (int64_t)centre + off_hz;
    if (peak_v > 0.05f && peak_hz > 0) {
        snprintf(b, sizeof(b), "PEAK  %lu.%04lu MHz   %d%%",
                 (unsigned long)((uint64_t)peak_hz / 1000000ULL),
                 (unsigned long)(((uint64_t)peak_hz / 100ULL) % 10000ULL),
                 (int)(peak_v * 100.0f));
        lv_label_set_text(_scout_peak_lbl, b);
    } else {
        lv_label_set_text(_scout_peak_lbl, "PEAK  --  (floor only)");
    }

    /* Stats: frame rate + PSRAM footprint of the waterfall so the
       cost of running this view is on the panel next to it, not
       assumed.  A graph that starves the decoder is worse than no
       graph. LS-703 reports the shared widget's actual allocation, not the
       currently visible waterfall slice. */
    size_t wf_bytes = _scout_spectrum.allocation_bytes;
    size_t fft_bytes = (size_t)SPEC_FFT_N * sizeof(float) * 2;   /* pub + db */
    snprintf(b, sizeof(b),
             "%.1f Hz   sweeps %lu   %dx%d\nPSRAM: waterfall %u B   FFT pub %u B",
             (double)_scout_fps,
             (unsigned long)sweeps,
             _scout_spectrum.width, _scout_spectrum.waterfall_height,
             (unsigned)wf_bytes, (unsigned)fft_bytes);
    lv_label_set_text(_scout_stats, b);

    if (receiver.tune_attention ||
        (st_ui.effective_freq_known &&
         st_ui.freq_hz != st_ui.effective_freq_hz))
        snprintf(b, sizeof(b), "SCOUT  %s", receiver.frequency);
    else
        /*LS-784  centre and span are already on the line below; repeating
           them here is what made this the widest, wrap-prone label. Carry the
           receiver state, which is not shown anywhere else on this tab. */
        snprintf(b, sizeof(b), "SCOUT  %s", receiver.connection);
    lv_label_set_text(_scout_hdr, b);

    /*LS-984  Live-update the on-screen GAIN readout and REC/STOP
       button label so the operator does not need to leave the tab
       to check either. */
    ls_spectrum_waterfall_set_gain_text(&_scout_spectrum, receiver.gain);
    scoutRefreshRecBtn();
}

/*LS-963*/
void AppREC::scoutTunePeakCb(lv_event_t *e)
{
    AppREC *self = (AppREC *)lv_event_get_user_data(e);
    if (!self) return;

    /* Prefer the DISPLAY peak (as the user sees it in the chart) over
       the backend peak: at zoom levels below 200 kHz the two disagree,
       and the operator is looking at the chart. */
    uint32_t centre = rec_get_freq();
    uint32_t span   = rec_scout_span_hz(self->_scout_zoom);

    float bins_full[240];
    int nbins = self->_scout_bins;
    if (nbins < 8)  nbins = 8;
    if (nbins > 240) nbins = 240;
    uint32_t peak_hz_abs = 0;
    float peak_v = 0.0f;
    if (rec_scout_read(bins_full, nbins)) {
        float frac = (float)span / (float)REC_SCOUT_SPAN_HZ;
        if (frac > 1.0f) frac = 1.0f;
        int keep  = (int)(nbins * frac + 0.5f);
        if (keep < 4) keep = 4;
        int start = (nbins - keep) / 2;
        int peak_i = 0;
        for (int i = 0; i < nbins; i++) {
            int src = start + (i * keep) / nbins;
            if (src < 0) src = 0;
            if (src >= nbins) src = nbins - 1;
            float v = bins_full[src];
            if (v > peak_v) { peak_v = v; peak_i = i; }
        }
        float rel = ((float)peak_i / (float)(nbins - 1)) - 0.5f;
        int32_t off_hz = (int32_t)(rel * (float)span);
        int64_t hz = (int64_t)centre + off_hz;
        if (hz > 0) peak_hz_abs = (uint32_t)hz;
    } else {
        /* No display frame yet - fall back to the backend peak. */
        rec_scout_peak(&peak_hz_abs, &peak_v);
    }

    if (peak_v > 0.05f && peak_hz_abs >= REC_FREQ_MIN_HZ &&
        peak_hz_abs <= REC_FREQ_MAX_HZ) {
        rec_set_freq(peak_hz_abs);
        self->_preset = -1;   /* out of the preset cycle */
    }
}

/*LS-560  SPAN remains as the quick cyclic preset shortcut from LS-963.
   ZOOM -/+ are directional and clamp at the wide/narrow ends, so repeated
   presses never jump across the ladder in the opposite direction. */
void AppREC::scoutSpanCb(lv_event_t *e)
{
    AppREC *self = (AppREC *)lv_event_get_user_data(e);
    if (!self) return;
    self->_scout_zoom = rec_scout_span_cycle(self->_scout_zoom);
    scout_span_label(self->_scout_span_lbl, self->_scout_zoom);
}

void AppREC::scoutZoomOutCb(lv_event_t *e)
{
    AppREC *self = (AppREC *)lv_event_get_user_data(e);
    if (!self) return;
    self->_scout_zoom = rec_scout_span_zoom_out(self->_scout_zoom);
    scout_span_label(self->_scout_span_lbl, self->_scout_zoom);
}

void AppREC::scoutZoomInCb(lv_event_t *e)
{
    AppREC *self = (AppREC *)lv_event_get_user_data(e);
    if (!self) return;
    self->_scout_zoom = rec_scout_span_zoom_in(self->_scout_zoom);
    scout_span_label(self->_scout_span_lbl, self->_scout_zoom);
}

/*LS-984  Divide the chart+waterfall area between the two viewers.

   Split is a percentage of the vertical area given to the spectrum;
   the waterfall gets the rest.  0 hides the spectrum, 100 hides the
   waterfall, everything else splits proportionally.  Fullscreen mode
   hides the chrome and gives the waterfall the entire tab except the
   wrapping controls, so an operator watching a slow signal is not
   staring at header labels that could have been more waterfall.

   Buffer size does not change on split - only the visible slice via
   lv_canvas_set_buffer.  Reallocating on every SPLIT+/SPLIT- press
   would fragment PSRAM and stutter the display; the tradeoff is
   bounded PSRAM buffer that is not currently painted. The shared widget owns
   that allocation and the same split arithmetic used by FM and P25. */
void AppREC::applyScoutSplit(void)
{
    if (!_scout_area) return;
    if (_scout_chrome) {
        if (_scout_full) lv_obj_add_flag(_scout_chrome, LV_OBJ_FLAG_HIDDEN);
        else             lv_obj_clear_flag(_scout_chrome, LV_OBJ_FLAG_HIDDEN);
    }
    ls_spectrum_waterfall_set_split(&_scout_spectrum, _scout_split_pct);
    ls_spectrum_waterfall_set_contrast(&_scout_spectrum,
                                       _scout_contrast_pct);
    ls_spectrum_waterfall_set_fullscreen(&_scout_spectrum, _scout_full);
}

void AppREC::scoutViewChanged(ls_spectrum_waterfall_t *view, void *user_data)
{
    AppREC *self = static_cast<AppREC *>(user_data);
    if (!self || !view) return;
    self->_scout_split_pct = view->split_pct;
    self->_scout_contrast_pct = view->contrast_pct;
    self->_scout_full = view->fullscreen;
    self->applyScoutSplit();
    self->scoutSavePrefs();
}

/*LS-984  Persist SCOUT split + fullscreen so the choice survives
   leaving the tab (or the whole app).  A named NVS namespace so
   another tab does not conflict on the same keys. */
void AppREC::scoutLoadPrefs(void)
{
    nvs_handle_t h;
    if (nvs_open(SCOUT_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t v = 0;
    if (nvs_get_i32(h, SCOUT_KEY_SPLIT, &v) == ESP_OK) {
        if (v < 0)   v = 0;
        if (v > 100) v = 100;
        _scout_split_pct = (int)v;
    }
    uint8_t f = 0;
    if (nvs_get_u8(h, SCOUT_KEY_FULL, &f) == ESP_OK) {
        _scout_full = (f != 0);
    }
    if (nvs_get_i32(h, SCOUT_KEY_CONTRAST, &v) == ESP_OK) {
        if (v < 50) v = 50;
        if (v > 200) v = 200;
        _scout_contrast_pct = (int)v;
    }
    nvs_close(h);
}

void AppREC::scoutSavePrefs(void)
{
    nvs_handle_t h;
    if (nvs_open(SCOUT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, SCOUT_KEY_SPLIT, (int32_t)_scout_split_pct);
    nvs_set_u8 (h, SCOUT_KEY_FULL,  _scout_full ? 1 : 0);
    nvs_set_i32(h, SCOUT_KEY_CONTRAST, (int32_t)_scout_contrast_pct);
    nvs_commit(h);
    nvs_close(h);
}

/*LS-984  REC button on the main SCOUT screen so the operator does not
   walk back to the RECORD tab to start a capture on a signal they can
   see right there in the chart.  Label doubles as state: REC arms,
   STOP stops. */
void AppREC::scoutRefreshRecBtn(void)
{
    if (!_scout_rec_lbl) return;
    rec_status_t st;
    rec_get_status(&st);
    const char *txt = (st.phase == REC_ARMED || st.phase == REC_CAPTURING)
                    ? "STOP" : "REC";
    lv_label_set_text(_scout_rec_lbl, txt);
}

/*LS-984  Gain on the main screen, same step and clamps as the CONFIG
   tab so the two do not disagree.  Keeps GAIN in the operator's line
   of sight while a peak is up in the chart. */
void AppREC::scoutGainDownCb(lv_event_t *e)
{
    AppREC *self = (AppREC *)lv_event_get_user_data(e);
    rec_status_t st; rec_get_status(&st);
    int g = st.gain_tenths - 10;
    if (g < 0) g = 0;
    rec_set_gain(g);
    (void)self;
}

void AppREC::scoutGainUpCb(lv_event_t *e)
{
    AppREC *self = (AppREC *)lv_event_get_user_data(e);
    rec_status_t st; rec_get_status(&st);
    int g = st.gain_tenths + 10;
    if (g > 496) g = 496;
    rec_set_gain(g);
    (void)self;
}

/*LS-984  ARM/STOP as one button: the operator only ever wants
   whichever action is not the current state.  Stopping mid-capture
   from SCOUT is exactly the sequence "I saw it happen, that is
   enough" - and having to leave the tab to hit STOP is why the
   operator asked for this. */
void AppREC::scoutArmCb(lv_event_t *e)
{
    AppREC *self = (AppREC *)lv_event_get_user_data(e);
    if (!self) return;
    rec_status_t st;
    rec_get_status(&st);
    if (st.phase == REC_ARMED || st.phase == REC_CAPTURING) rec_disarm();
    else                                                    rec_arm_request();
    self->scoutRefreshRecBtn();
}

void AppREC::buildConfigTab(lv_obj_t *parent)
{
    ls_ui_style_content(parent);

    sdr_setrow_t r;

    sdr_section(parent, "TUNING");

    sdr_setting_row(parent, "FREQUENCY", &r);
    _cfg_freq = r.value;
    ls_ui_button(r.controls, "<", LS_BTN_DEFAULT, freqDownCb, this, nullptr);
    ls_ui_button(r.controls, ">", LS_BTN_DEFAULT, freqUpCb, this, nullptr);
    /*LS-028*/
    ls_ui_button(r.controls, "SET", LS_BTN_PRIMARY, freqEntryCb, this, nullptr);

    sdr_setting_row(parent, "COARSE  1 MHz", &r);
    lv_label_set_text(r.value, "");
    ls_ui_button(r.controls, "<<", LS_BTN_DEFAULT, freqCoarseDownCb, this, nullptr);
    ls_ui_button(r.controls, ">>", LS_BTN_DEFAULT, freqCoarseUpCb, this, nullptr);

    sdr_setting_row(parent, "PRESET", &r);
    _cfg_preset = r.value;
    ls_ui_button(r.controls, "CYCLE", LS_BTN_DEFAULT, presetCb, this, nullptr);

    sdr_setting_row(parent, "GAIN", &r);
    _cfg_gain = r.value;
    ls_ui_button(r.controls, "<", LS_BTN_DEFAULT, gainDownCb, this, nullptr);
    ls_ui_button(r.controls, ">", LS_BTN_DEFAULT, gainUpCb, this, nullptr);

    /*LS-516*/
    sdr_setting_row(parent, "BANDWIDTH", &r);
    _cfg_bw = r.value;
    ls_ui_button(r.controls, "<", LS_BTN_DEFAULT, bwDownCb, this, nullptr);
    ls_ui_button(r.controls, ">", LS_BTN_DEFAULT, bwUpCb, this, nullptr);
    /*LS-830  AUTO, the same as THRESHOLD has. The down button does reach
       auto - it steps 50 kHz and 0 is the bottom - but from 1000 kHz that is
       twenty presses, so in practice there was no way back and the setting
       looked one-way. */
    ls_ui_button(r.controls, "AUTO", LS_BTN_TOGGLE_OFF, bwAutoCb, this, nullptr);

    sdr_section(parent, "DETECTOR");

    /*LS-503*/
    sdr_setting_row(parent, "THRESHOLD", &r);
    _cfg_thresh = r.value;
    ls_ui_button(r.controls, "<", LS_BTN_DEFAULT, threshDownCb, this, nullptr);
    ls_ui_button(r.controls, ">", LS_BTN_DEFAULT, threshUpCb, this, nullptr);
    ls_ui_button(r.controls, "AUTO", LS_BTN_TOGGLE_OFF, threshAutoCb, this, nullptr);

    /*LS-504*/
    sdr_setting_row(parent, "END GAP", &r);
    _cfg_gap = r.value;
    ls_ui_button(r.controls, "<", LS_BTN_DEFAULT, gapDownCb, this, nullptr);
    ls_ui_button(r.controls, ">", LS_BTN_DEFAULT, gapUpCb, this, nullptr);

    sdr_setting_row(parent, "MIN PULSE", &r);
    _cfg_minpul = r.value;
    ls_ui_button(r.controls, "<", LS_BTN_DEFAULT, minPulseDownCb, this, nullptr);
    ls_ui_button(r.controls, ">", LS_BTN_DEFAULT, minPulseUpCb, this, nullptr);

    sdr_setting_row(parent, "MAX SPAN", &r);
    _cfg_maxspan = r.value;
    ls_ui_button(r.controls, "<", LS_BTN_DEFAULT, maxSpanDownCb, this, nullptr);
    ls_ui_button(r.controls, ">", LS_BTN_DEFAULT, maxSpanUpCb, this, nullptr);

    sdr_setting_row(parent, "MIN EDGES", &r);
    _cfg_minedg = r.value;
    ls_ui_button(r.controls, "<", LS_BTN_DEFAULT, minEdgesDownCb, this, nullptr);
    ls_ui_button(r.controls, ">", LS_BTN_DEFAULT, minEdgesUpCb, this, nullptr);

    /*LS-830  A way back. Every detector setting here persists, and a bad
       combination stops the recorder triggering with nothing on screen to say
       which one did it - so without this the only reliable escape was
       reflashing. */
    sdr_setting_row(parent, "RESET ALL", &r);
    lv_label_set_text(r.value, "defaults");
    ls_ui_button(r.controls, "RESET", LS_BTN_DANGER, cfgResetCb, this, nullptr);

    updateConfig();
}

void AppREC::updateConfig(void)
{
    if (!_cfg_freq) return;

    char b[96];
    uint32_t hz = rec_get_freq();

    rec_status_t st;
    rec_get_status(&st);
    ls_receiver_presentation_t receiver;
    rec_receiver_present(&st, &receiver);

    lv_label_set_text(_cfg_freq, receiver.frequency);
    lv_obj_set_style_text_color(_cfg_freq,
        receiver.tune_attention ? SDR_PAS_ROSE : COL_TEXT, 0);

    int match = -1;
    for (int i = 0; i < N_PRESETS; i++)
        if (REC_PRESETS[i].hz == hz) { match = i; break; }
    lv_label_set_text(_cfg_preset, match >= 0 ? REC_PRESETS[match].name : "custom");

    lv_label_set_text(_cfg_gain, receiver.gain);
    lv_obj_set_style_text_color(_cfg_gain,
        receiver.gain_attention ? SDR_PAS_ROSE : COL_TEXT, 0);

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
    if (hz < (int64_t)REC_FREQ_MIN_HZ) hz = (int64_t)REC_FREQ_MIN_HZ;
    if (hz > (int64_t)REC_FREQ_MAX_HZ) hz = (int64_t)REC_FREQ_MAX_HZ;
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

/*LS-028*/
/* Direct frequency entry. Same keypad AppFM got in LS-732, and deliberately
   the same code shape rather than a shared helper - see the marker entry for
   why that call was made. */
void AppREC::freqEntryCb(lv_event_t *e)
{
    AppREC *self = (AppREC *)lv_event_get_user_data(e);
    if (self) self->openFreqEntry();
}

void AppREC::openFreqEntry(void)
{
    /* Placeholder is where the receiver is NOW, so the field doubles as a
       readout and a small correction does not need the number memorised. */
    char cur[24];
    snprintf(cur, sizeof(cur), "%.4f", rec_get_freq() / 1e6);
    /*LS-732*/
    const ls_text_entry_config_t config = {
        .title = "ENTER FREQUENCY (MHz)  -  24 to 1766",
        .text = "",
        .placeholder = cur,
        .accepted_chars = "0123456789.",
        .max_length = 10,
        .width = lv_pct(90),
        .mode = LS_TEXT_ENTRY_NUMBER,
        .large = true,
    };
    _freq_entry = ls_text_entry_open(&config, freqEntryDone, this);
}

void AppREC::freqEntryDone(bool accepted, const char *text, void *user_data)
{
    AppREC *self = static_cast<AppREC *>(user_data);
    if (!self) return;
    self->_freq_entry = nullptr;
    if (accepted) {
        double mhz = atof(text);
        double hz  = mhz * 1e6;
        /* Out of range is REFUSED, not clamped. Clamping a mistyped 4339.2 to
           1766 would tune somewhere the user never asked for and then sit
           there looking tuned - the meter would read a flat floor and the
           frequency would be the last thing suspected. */
        if (hz >= (double)REC_FREQ_MIN_HZ && hz <= (double)REC_FREQ_MAX_HZ) {
            rec_set_freq((uint32_t)(hz + 0.5));
            /* Entering a frequency by hand leaves PRESET showing "custom"
               until it matches one; re-sync the cycle index so the next CYCLE
               starts from the top rather than from wherever it last was. */
            self->_preset = -1;
        }
    }
    self->updateConfig();
}

void AppREC::closeFreqEntry(void)
{
    if (_freq_entry) {
        ls_text_entry_t *entry = _freq_entry;
        _freq_entry = nullptr;
        ls_text_entry_close(entry);
    }
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

/*LS-830*/
void AppREC::bwAutoCb(lv_event_t *e)
{
    rec_set_bw(0);
    ((AppREC *)lv_event_get_user_data(e))->updateConfig();
}

/*LS-830  Put every REC setting back to its built-in default.

   There was no way to undo a session of experimenting short of reflashing:
   threshold, gap, bandwidth, min pulse, max span and min edges all persist,
   and a bad combination silently stops the recorder triggering at all. The
   values here are the REC_* defaults from rec_state.h, not new numbers. */
void AppREC::cfgResetCb(lv_event_t *e)
{
    rec_set_freq(REC_DEFAULT_FREQ);
    rec_set_gain(REC_DEFAULT_GAIN);
    rec_set_thresh(0);                          /* auto */
    rec_set_bw(0);                              /* auto */
    rec_set_gap_ms(REC_GAP_END_US / 1000);
    rec_set_min_pulse(REC_MIN_PULSE_US);
    rec_set_max_span(REC_MAX_SPAN_US);          /* setter takes MICROseconds */
    rec_set_min_edges(REC_MIN_EDGES);
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
    ls_ui_style_content(parent);

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
    lv_obj_set_style_bg_color(_files_table, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_files_table, LV_OPA_COVER, LV_PART_MAIN);
    ls_ui_style_table(_files_table);

    lv_table_set_col_cnt(_files_table, 1);
    lv_table_set_col_width(_files_table, 0, 440);
    lv_obj_add_event_cb(_files_table, filesRowCb, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *row = ls_ui_controls(parent);
    ls_ui_button(row, "REFRESH", LS_BTN_DEFAULT, filesRefreshCb, this, nullptr);
    ls_ui_button(row, "DELETE", LS_BTN_DANGER, filesDeleteCb, this, nullptr);

    refreshFiles();
}

void AppREC::refreshFiles(void)
{
    if (!_files_table) return;

    char list[768];
    /*LS-907*/
    /* rec_list returns the true total on disk and reports byte-buffer
       truncation separately, so the note can name both limits honestly
       instead of only the row-cap one. */
    bool byte_trunc = false;
    int total = rec_list(list, sizeof(list), &byte_trunc);

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

    /*LS-768*/
    /* Was "on SPIFFS" for every case, but rec_dir() prefers /sdcard/lakeshark
       when a card is mounted (see the LS-031 note on that function) - so this
       label lied on any board with a card in the slot.  Route through the
       classifier so the two paths cannot drift. */
    /*LS-907*/
    /* Anything the user cannot see is "truncated": either the byte
       buffer filled before rec_list finished walking the directory, or
       the total exceeds the row cap.  Was only n > FILES_MAX before,
       which hid maximum-length name captures that never even reached
       the row loop. */
    bool truncated = byte_trunc || (total > FILES_MAX) ||
                     (_file_count < total);
    char b[96];
    rec_files_note(b, sizeof(b), rec_dir(), total, truncated ? 1 : 0);
    /*LS-961*/
    /* Free-space suffix so the user knows without opening a console
       whether a SAVE is going to be refused.  UINT64_MAX from the
       probe reads as "?" so a failed statvfs is not mistaken for a
       full disk. */
    uint64_t free_b = rec_dir_free_bytes();
    size_t off = strlen(b);
    if (off + 2 < sizeof(b)) {
        if (free_b == UINT64_MAX) {
            snprintf(b + off, sizeof(b) - off, "  -  free ?");
        } else {
            char human[32];
            rec_space_format_free(human, sizeof(human), free_b);
            snprintf(b + off, sizeof(b) - off, "  -  %s free", human);
        }
    }
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
