#include "AppP25.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"

extern "C" {
#include "app_registry.h"
#include "settings.h"
#include "p25_state.h"
#include "lakeshark_backend.h"
#include "sam_tts.h"
#include "audio_events.h"
#include "audio_out.h"
#include "scan_channels.h"
#include "scan_engine.h"
#include "scan_ctrl.h"
#include "p25_spectrum.h"
#include "p25_controls.h"
#include "p25_program.h"
#include "p25_health.h"
#include "p25_tg_observed.h"
}

/* LS-780: the GUI owns this bounded snapshot.  It is read once a second
   and is deliberately not on the LVGL task stack and not reallocated on
   every tick - both are how earlier roster views ran internal heap down. */
static EXT_RAM_BSS_ATTR p25_tg_observed_snapshot_t s_observed_view;

#include "p25_tabs.h"
#include "sdr_ui/sdr_ui.h"
#include "ui/ls_ui.h"
#include "ui/ls_receiver_status.h"

LV_IMG_DECLARE(img_app_p25);

#define COL_BG      LS_UI_BACKGROUND
#define COL_PANEL   LS_UI_PANEL
#define COL_LABEL   LS_UI_DIM_TEXT
#define COL_TEXT    LS_UI_TEXT
#define COL_BRIGHT  LS_UI_TEXT
#define COL_DIM     LS_UI_DIM_TEXT
#define COL_GREEN   LS_UI_ACCENT
#define COL_AMBER   LS_UI_WARN
#define COL_RED     LS_UI_ALARM
#define COL_CYAN    LS_UI_ACCENT
#define COL_GOLD    LS_UI_WARN

static const char *TAG = "AppP25";
static const uint32_t P25_GUI_TICK_BUDGET_US = 20000;

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    (void)font;
    return sdr_label(parent, sdr_font_mono(), color);
}

static lv_obj_t *make_panel(lv_obj_t *parent)
{
    return ls_ui_panel(parent, nullptr);
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud,
                          lv_obj_t **out_lbl = nullptr,
                          ls_ui_button_role_t role = LS_BTN_DEFAULT)
{
    return ls_ui_button(parent, txt, role, cb, ud, out_lbl);
}

static void ascii_bar(char *out, size_t outsz, int pct, int width)
{
    if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
    int fill = (pct * width + 50) / 100;
    size_t n = 0;
    if (n < outsz - 1) out[n++] = '[';
    for (int i = 0; i < width && n < outsz - 2; i++)
        out[n++] = (i < fill) ? '#' : '.';
    if (n < outsz - 1) out[n++] = ']';
    out[n] = 0;
}

static void set_text_if_changed(lv_obj_t *lbl, const char *s)
{
    const char *cur = lv_label_get_text(lbl);
    if (!cur || strcmp(cur, s) != 0) lv_label_set_text(lbl, s);
}

static uint32_t tg_hash_bytes(uint32_t hash, const void *data, size_t size)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

static const p25_profile_t *active_tg_profile(void)
{
    const p25_program_t *program = p25_program_session();
    return program && program->active_valid ? &program->active : nullptr;
}

static void ascii_spark(char *out, size_t outsz, const int *vals, int head,
                        int n, int maxv)
{
    static const char lvl[] = " .:-=+*#";
    size_t w = 0;
    for (int i = 0; i < n && w < outsz - 1; i++) {
        int v = vals[(head + i) % n];
        int l = maxv > 0 ? (v * 7) / maxv : 0;
        if (l < 0) l = 0; else if (l > 7) l = 7;
        out[w++] = lvl[l];
    }
    out[w] = 0;
}

/*LS-736*/
static void set_toggle(lv_obj_t *button, bool on)
{
    if (button)
        ls_ui_button_set_role(button, on ? LS_BTN_TOGGLE_ON
                                         : LS_BTN_TOGGLE_OFF);
}

static void p25_seg_vol(void *, int v)         { audio_volume_set(v); }
static void p25_seg_gain_live(void *, int v)   { lakeshark_radio_set_gain_live(v); }
static void p25_seg_gain_commit(void *, int v) { lakeshark_radio_set_gain(v); }
static void p25_seg_gate(void *, int v)        { lakeshark_p25_set_voice_gate(v); }
/*LS-746*/ /* hang moved into ScanPanel - it is not a P25 setting. */
/*LS-702*/
static void p25_seg_thresh(void *, int v)      { scan_engine_set_threshold_pct(v); }

/* Text meter (mono ASCII bar) -- intentionally NOT an lv_bar: live lv_bar
 * redraws contend with the DSI framebuffer DMA during P25 voice decode and
 * cause choppy audio + display (blue) flashes. */
static lv_obj_t *p25_meter_label(lv_obj_t *parent)
{
    lv_obj_t *l = sdr_label(parent, sdr_font_mono(), SDR_TEXT);
    lv_obj_set_width(l, lv_pct(100));
    return l;
}

AppP25::AppP25()
    : LsApp("P25", "p25")
{
}

AppP25::~AppP25() = default;

bool AppP25::init(void)   { return true; }

/*LS-600*/
bool AppP25::pause(void)
{
    if (_timer) lv_timer_pause(_timer);
    p25_spectrum_enable(false);
    lakeshark_radio_park();
    return true;
}

/*LS-604*/
bool AppP25::background(void)
{
    if (_timer) lv_timer_pause(_timer);
    p25_spectrum_enable(false);
    return true;
}

/*LS-600*/
bool AppP25::resume(void)
{
    lakeshark_select_p25();
    if (_timer) lv_timer_resume(_timer);
    return true;
}

bool AppP25::back(void)
{
    if (_freq_entry) { closeFreqEntry(); return true; }
    /*LS-706*/
    if (_name_entry) { closeNameEntry(); return true; }
    return exitToLauncher();
}

bool AppP25::close(void)
{
    _tg_observed = nullptr;
    _tg_observed_second = UINT32_MAX;
    p25_spectrum_enable(false);
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
    /*LS-706*/
    closeFreqEntry();
    closeNameEntry();
    _tabview = nullptr;
    _scan_table = nullptr;
    _scan_render_sig = 0;
    _scan_rendered = false;
    _scan_render_cur = -1;
    /*LS-689*/
    _pg_state = nullptr;
    _pg_system = nullptr;
    _pg_site = nullptr;
    _pg_roster = nullptr;
    _pg_source = nullptr;
    _pg_control = nullptr;
    _pg_list = nullptr;
    _pg_status = nullptr;
    /*LS-690*/
    _tg_summary = nullptr;
    _tg_table = nullptr;
    _tg_status = nullptr;
    _tg_render_sig = 0;
    _tg_rendered = false;
    _s_gui = nullptr;
    p25_tg_roster_init(&_tg_roster);
    _zone_val = nullptr;
    _ch_val = nullptr;
    /*LS-746*/
    _scan_panel.forget();
    ls_spectrum_waterfall_forget(&_s_spectrum);
    _s_spectrum_seq = 0;
    lakeshark_radio_park();
    return true;
}

bool AppP25::run(lv_obj_t *parent)
{
    _last_sample_us = 0;
    _last_sync = _last_voice = _rate_head = 0;
    memset(_rate_voice, 0, sizeof(_rate_voice));
    memset(_rate_sync, 0, sizeof(_rate_sync));
    lakeshark_select_p25();

    _gui_last_us = 0;
    _gui_max_us = 0;
    _gui_over_budget = 0;
    _gui_last_warn_us = 0;

    ls_ui_screen_t screen;
    /* LS-736: no app header.  The shell already draws one status bar and the
     * frequency is the 48 px number on DECODE; a second "P25 / 154.7850" strip
     * under the shell bar was duplicate chrome, and on the 480 px panel it was
     * a whole panel of the height the DECODE actions needed. */
    ls_ui_screen_create(parent, nullptr, true, LS_UI_COLOR_ID_RED, &screen);
    _tabview = screen.tabs;

    static const char *const tab_names[P25_TAB_COUNT] = P25_TAB_NAMES;
    buildDecodeTab(ls_ui_screen_add_tab(&screen, tab_names[P25_TAB_DECODE]));
    buildSignalTab(ls_ui_screen_add_tab(&screen, tab_names[P25_TAB_SIGNAL]));
    buildHealthTab(ls_ui_screen_add_tab(&screen, tab_names[P25_TAB_HEALTH]));
    buildScanTab(ls_ui_screen_add_tab(&screen, tab_names[P25_TAB_SCAN]));
    buildSettingsTab(ls_ui_screen_add_tab(&screen, tab_names[P25_TAB_CONFIG]));
    /*LS-689*/
    buildProgramTab(ls_ui_screen_add_tab(&screen, tab_names[P25_TAB_PROGRAM]));
    /*LS-690*/
    buildTalkgroupsTab(ls_ui_screen_add_tab(&screen, tab_names[P25_TAB_GROUPS]));

    _timer = lv_timer_create(timerCb, 250, this);
    return true;
}

void AppP25::buildDecodeTab(lv_obj_t *parent)
{
    ls_ui_style_content(parent);

    /* LS-736: C-SCAN, HOLD and LOCK are what an operator reaches for while a
     * call is running, and on the 480 px panel they were below the fold: the
     * whole tab was one scroller, so the readouts pushed the actions off the
     * bottom and finding them meant dragging first.  The actions now own a
     * row the tab cannot scroll; only the readouts above them move.  This is
     * a property of the layout, not of a pixel budget, so it holds on 720x720
     * and on whatever the third panel turns out to be. */
    ls_ui_split_t split;
    ls_ui_tab_split(parent, &split);
    parent = split.body;

    lv_obj_t *face = sdr_lcd_panel(parent, SDR_ROLE_COLOR(LS_UI_COLOR_ID_RED));

    lv_obj_t *strap = lv_obj_create(face);
    lv_obj_set_size(strap, lv_pct(100), LV_SIZE_CONTENT);
    ls_ui_style_lcd_row(strap);
    lv_obj_set_flex_flow(strap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strap, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(strap, LV_OBJ_FLAG_SCROLLABLE);

    _d_face_mode = sdr_label(strap, &lv_font_montserrat_28, SDR_ROLE_COLOR(LS_UI_COLOR_ID_RED));
    lv_obj_set_style_text_letter_space(_d_face_mode, 2, 0);
    lv_label_set_text(_d_face_mode, "C4FM");

    lv_obj_t *rxbox = lv_obj_create(strap);
    lv_obj_set_size(rxbox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    ls_ui_style_lcd_row(rxbox);
    lv_obj_set_flex_flow(rxbox, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rxbox, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(rxbox, LV_OBJ_FLAG_SCROLLABLE);
    _d_led = ls_ui_lamp(rxbox, LS_UI_COLOR_ACCENT);
    _d_rx = sdr_label(rxbox, &lv_font_montserrat_22, COL_DIM);
    lv_label_set_text(_d_rx, "RX");

    _d_freq = sdr_label(face, &lv_font_montserrat_48, SDR_ROLE_COLOR(LS_UI_COLOR_TEXT));
    lv_obj_set_width(_d_freq, lv_pct(100));
    lv_obj_set_style_text_align(_d_freq, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(_d_freq, 2, 0);
    lv_label_set_text(_d_freq, "154.7850");
    lv_obj_add_flag(_d_freq, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_d_freq, freqEntryCb, LV_EVENT_CLICKED, this);

    _d_status = sdr_label(face, &lv_font_montserrat_16, SDR_PAS_CYAN);
    lv_obj_set_width(_d_status, lv_pct(100));
    lv_obj_set_style_text_align(_d_status, LV_TEXT_ALIGN_CENTER, 0);
    /*LS-807  This wraps, so a longer status became a second line, grew the
       face panel and shoved the gain and volume controls down the screen
       mid-use. One clipped line keeps everything below it still. */
    lv_label_set_long_mode(_d_status, LV_LABEL_LONG_DOT);
    lv_label_set_text(_d_status, "Waiting...");

    _d_smeter = p25_meter_label(face);
    _d_bmeter = p25_meter_label(face);

    lv_obj_t *p1 = make_panel(parent);
    _d_decode = make_label(p1, &lv_font_montserrat_16, COL_TEXT);
    lv_obj_set_width(_d_decode, lv_pct(100));

    lv_obj_t *identity_panel = make_panel(parent);
    _d_identity = make_label(identity_panel, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_width(_d_identity, lv_pct(100));
    lv_label_set_text(_d_identity, "SYSTEM identity waiting");

    /* LS-736: the four-line RTL/IQ/decode diagnostics block that used to sit
     * here is on HEALTH now.  It is a health question, it is the tallest
     * thing on the tab after the LCD face, and nobody watching a call reads
     * it - see buildHealthTab. */

    _d_gain_slider = sdr_seg_slider(parent, SDR_ROLE_COLOR(LS_UI_COLOR_ID_RED), 496, lakeshark_p25_gain_tenths(),
                                    p25_seg_gain_live, this, &_d_gain_lbl);
    sdr_seg_on_release(_d_gain_slider, p25_seg_gain_commit);
    _d_vol_slider  = sdr_seg_slider(parent, SDR_PAS_GREEN, 100, audio_volume_get(),
                                    p25_seg_vol, this, &_d_vol_lbl);

    lv_obj_t *btns = split.actions;

    /*LS-736: the labels come from p25_tabs.h so the host fit case measures
      the row this screen actually builds. */
    static const char *const action[P25_DECODE_ACTION_COUNT] =
        P25_DECODE_ACTIONS;

    /* LS-702: this is the shared carrier scanner, not PROGRAM's
       protocol-evidence control-channel survey. Keep that distinction on the
       button an operator presses, not only in documentation. */
    make_btn(btns, action[0], scanToggleCb, this, &_d_scan_btn_lbl,
             LS_BTN_PRIMARY);
    /* LS-670: one-press hold and lockout, on the DECODE tab so an operator
     * doing anything else does not have to navigate to reach them. HOLD is
     * a toggle: press it again on a held call and hold clears. LOCK is a
     * one-shot: it locks out the current TG and kicks the follower back to
     * the control channel, and the LOCK row on the SCAN tab is where it
     * gets removed. */
    make_btn(btns, action[1], holdCb, this, &_d_hold_btn_lbl, LS_BTN_TOGGLE_OFF);
    make_btn(btns, action[2], lockCb, this, nullptr, LS_BTN_DANGER);
    make_btn(btns, action[3], modeCb, this);
    make_btn(btns, action[4], agcCb, this, nullptr, LS_BTN_TOGGLE_OFF);
    make_btn(btns, action[5], resetCb, this, nullptr, LS_BTN_DANGER);
    make_btn(btns, action[6], beepCb, this, &_d_beepbtn_lbl, LS_BTN_TOGGLE_OFF);
}

void AppP25::updateDecode(void)
{
    char buf[256];
    p25_health_snapshot_t health;
    bool have_health = p25_health_read(&health);
    bool synced = P25.dsd_has_sync;
    bool voice  = P25.voice_active_until_us > esp_timer_get_time();
    ls_iq_control_status_t radio;
    p25_get_receiver_status(&radio);
    /* LS-662: this showed P25.dsd_modulation - the modulation DSD detected
     * on a synced signal - while the MODE button beside it changes
     * lakeshark_p25_set_mode(). Two different things. With no sync
     * dsd_modulation is empty and the label sat on its "C4FM" fallback
     * forever, so cycling the demod appeared to do nothing at all.
     *
     * Show the selected mode: that is what the button controls, so the
     * button now visibly moves it. The detected modulation is still
     * reported on the signal tab (MOD, in updateSignal), so nothing is
     * lost - and when the two disagree that is worth seeing, not hiding. */
    set_text_if_changed(_d_face_mode, lakeshark_p25_mode_name());

    /* LS-702: s_tune_freq_hz is request/saved state. During a scan it can be
       ahead of the tuner, and on an acquire/retune failure it can name a
       frequency the receiver never reached. The primary preview is actual
       endpoint state only; requested versus actual remains visible below. */
    if (radio.effective_center_known) {
        uint64_t hz = radio.effective_center_hz;
        snprintf(buf, sizeof(buf), "%llu.%06llu",
                 (unsigned long long)(hz / 1000000ULL),
                 (unsigned long long)(hz % 1000000ULL));
    } else {
        snprintf(buf, sizeof(buf), "--.------");
    }
    set_text_if_changed(_d_freq, buf);
    sdr_color_if_changed(_d_freq, voice ? SDR_PAS_GREEN
                                        : SDR_ROLE_COLOR(LS_UI_COLOR_TEXT));

    const char *rxs = voice ? "VOX" : synced ? "SYNC" : "RX";
    lv_color_t  rxc = voice ? SDR_PAS_GREEN : synced ? SDR_PAS_CYAN : COL_DIM;
    if (_d_rx) {
        set_text_if_changed(_d_rx, rxs);
        sdr_color_if_changed(_d_rx, rxc);
    }
    if (_d_led) ls_ui_lamp_set(_d_led, synced, LS_UI_COLOR_ACCENT);

    int ok = have_health ? (int)health.nid_valid : P25.dsd_bch_ok_count;
    int fail = have_health ? (int)health.nid_invalid : P25.dsd_bch_fail_count;
    int tot = ok + fail;
    int pct10 = tot > 0 ? (ok * 1000) / tot : 0;
    char nac[16];
    if (P25.dsd_last_ok_nac)      snprintf(nac, sizeof(nac), "0x%03X", P25.dsd_last_ok_nac);
    else if (P25.dsd_nac)         snprintf(nac, sizeof(nac), "0x%03X", P25.dsd_nac);
    else                          snprintf(nac, sizeof(nac), "-----");

    /* LS-303/LS-400: trunk follower line. Talkgroup, source, frequency, and
     * whether the radio is on the control or a traffic channel - a scanner
     * that hops silently is indistinguishable from a broken one. The line
     * only appears when TSBK activity or the IDEN table say the site is
     * trunked, so a conventional user does not see a spurious "CONTROL"
     * label on a repeater output. */
    char trunk[160];
    trunk[0] = 0;
    uint64_t tfhz = P25.grant_freq_hz;
    bool trunked = P25.grant_on_traffic ||
                    (have_health ? health.tsbk_valid : P25.p25_tsbk_ok_count) > 0 ||
                   P25.p25_iden_valid_count > 0;
    if (P25.grant_on_traffic) {
        snprintf(trunk, sizeof(trunk),
                 "TRUNK TRAFFIC  TG %u  SRC %lu  %lu.%04lu MHz  IDEN %u",
                 (unsigned)P25.grant_talkgroup,
                 (unsigned long)P25.grant_source,
                 (unsigned long)(tfhz / 1000000ull),
                 (unsigned long)((tfhz / 100ull) % 10000ull),
                 (unsigned)P25.p25_iden_valid_count);
    } else if (trunked) {
        if (P25.p25_phase2_grant_count != 0) {
            uint64_t p2hz = P25.p25_phase2_last_frequency_hz;
            snprintf(trunk, sizeof(trunk),
                     "TRUNK CONTROL  PHASE 2 (NO VOICE) x%u  TG %u  "
                     "%lu.%04lu MHz  SLOT %u/%u",
                     (unsigned)P25.p25_phase2_grant_count,
                     (unsigned)P25.p25_phase2_last_talkgroup,
                     (unsigned long)(p2hz / 1000000ull),
                     (unsigned long)((p2hz / 100ull) % 10000ull),
                     (unsigned)P25.p25_phase2_last_slot,
                     (unsigned)P25.p25_phase2_last_slots_per_carrier);
        } else {
            snprintf(trunk, sizeof(trunk),
                     "TRUNK CONTROL  %lu.%04lu MHz  IDEN %u  followed %u",
                     (unsigned long)(tfhz / 1000000ull),
                     (unsigned long)((tfhz / 100ull) % 10000ull),
                     (unsigned)P25.p25_iden_valid_count,
                     (unsigned)P25.grant_followed_count);
        }
    }

    /* LS-610: ENC line. Only show it once an LDU2 has been decoded so the
     * operator can distinguish "muted because encrypted (ENC ADP)" from
     * "silent because broken". Prints the algorithm name for the six common
     * ALGIDs and falls back to hex for anything else.
     *
     * LS-611: append cumulative counters so an operator watching the panel
     * can answer "why did it just go quiet?" without a laptop:
     *   ENC ADP  KID $0000  (MUTED)  x42f 3ret 1skp
     * frames muted / returns to CC on encrypted grant / grants skipped. */
    char enc[128];
    enc[0] = 0;
    bool have_counters = (P25.p25_enc_muted_frames_total ||
                          P25.p25_enc_returns ||
                          P25.p25_enc_skips);
    if (P25.p25_ess_valid) {
        const char *name = p25_algid_name(P25.p25_algid);
        char alg[16];
        if (name) snprintf(alg, sizeof(alg), "%s", name);
        else      snprintf(alg, sizeof(alg), "$%02X", (unsigned)P25.p25_algid);
        snprintf(enc, sizeof(enc), "ENC %s  KID $%04X%s  x%uf %ur %us",
                 alg, (unsigned)P25.p25_kid,
                 P25.p25_enc_muted ? "  (MUTED)" : "",
                 (unsigned)P25.p25_enc_muted_frames_total,
                 (unsigned)P25.p25_enc_returns,
                 (unsigned)P25.p25_enc_skips);
    } else if (have_counters) {
        /* Between calls: the ESS is stale, but the counters are what
         * turned "it went quiet" into a fault report. Keep them on-screen. */
        snprintf(enc, sizeof(enc), "ENC (idle)  x%uf %ur %us  leave=%s skip=%us",
                 (unsigned)P25.p25_enc_muted_frames_total,
                 (unsigned)P25.p25_enc_returns,
                 (unsigned)P25.p25_enc_skips,
                 P25.p25_leave_on_encrypted ? "on" : "off",
                 (unsigned)(P25.p25_encrypted_skip_ms / 1000u));
    }

    /* LS-650: LCW identity from LDU1/TDULC. On a call joined mid-stream
     * (missed grant, brief control-channel loss) this is the only path to
     * a talkgroup / source / emergency label. Emergency is the one that
     * must be impossible to miss - the operator is holding the radio for
     * exactly this. When set, prefix the line with "*EMERGENCY*" and let
     * the readout lamp swing red just below. */
    char lcw[128];
    lcw[0] = 0;
    if (P25.p25_lcw_valid) {
        char alias[36];
        alias[0] = 0;
        if (P25.p25_lcw_alias_ready && P25.p25_lcw_alias[0])
            snprintf(alias, sizeof(alias), "  \"%s\"", P25.p25_lcw_alias);
        if (P25.p25_lcw_is_unit_to_unit) {
            snprintf(lcw, sizeof(lcw),
                     "%sLCW U-U  SRC %lu  DST %lu  %s%s%s",
                     P25.p25_lcw_emergency ? "*EMERGENCY* " : "",
                     (unsigned long)P25.p25_lcw_source,
                     (unsigned long)P25.p25_lcw_target,
                     P25.p25_lcw_encrypted ? "ENC " : "",
                     P25.p25_lcw_priority ? "PRI " : "",
                     alias);
        } else {
            snprintf(lcw, sizeof(lcw),
                     "%sLCW %s%u  SRC %lu  %s%sPRI %u%s",
                     P25.p25_lcw_emergency ? "*EMERGENCY* " : "",
                     P25.p25_lcw_is_regroup ? "SG " : "TG ",
                     (unsigned)P25.p25_lcw_talkgroup,
                     (unsigned long)P25.p25_lcw_source,
                     P25.p25_lcw_encrypted ? "ENC " : "",
                     P25.p25_lcw_emergency ? "EMR " : "",
                     (unsigned)P25.p25_lcw_priority,
                     alias);
        }
    }

    /* Flip a lamp red when LCW-emergency is set. This is the "impossible to
     * miss" bit.
     *
     * LS-736: it used to be the app header's lamp, which no longer exists.
     * The lamp on the LCD face is the better home for it anyway - it is
     * larger, it sits beside the RX/SYNC/VOX word an operator is already
     * watching, and it is in the part of DECODE that never scrolls. */
    if (P25.p25_lcw_valid && P25.p25_lcw_emergency)
        ls_ui_lamp_set(_d_led, true, LS_UI_COLOR_ALARM);

    /*LS-846  Two lines, and they never become three.

       The counters that used to live here - BCH ok/fail, the TSBK tallies,
       the frame type and DUID - are how well the decoder is doing, not who is
       talking, and they are a HEALTH question. Keeping them here cost four
       lines of the tab and, worse, three of the lines only appeared once
       voice came up, so the block grew at the exact moment a call started and
       shoved the gain and volume sliders down the screen.

       What is left is the call: who, and one line of status. Encryption first
       because it decides whether there is any point listening, then trunking,
       then the link control word. One of them at a time, so the height is
       fixed and nothing below it can move. */
    const char *status = enc[0]   ? enc :
                         trunk[0] ? trunk :
                         lcw[0]   ? lcw : "";

    snprintf(buf, sizeof(buf),
             "NAC %s    TG %d    SRC %d\n"
             "%s",
             nac, P25.dsd_tg, P25.dsd_src, status);
    set_text_if_changed(_d_decode, buf);
    if (_d_identity) {
        static char identity[320];
        if (have_health)
            p25_health_format_identity(
                &health, (uint32_t)(esp_timer_get_time() / 1000LL),
                identity, sizeof(identity));
        else
            snprintf(identity, sizeof(identity), "SYSTEM identity waiting");
        set_text_if_changed(_d_identity, identity);
        if (have_health) {
            p25_health_identity_state_t state = p25_health_identity_state(
                &health, (uint32_t)(esp_timer_get_time() / 1000LL));
            sdr_color_if_changed(
                _d_identity,
                state == P25_HEALTH_ID_INVALID ? COL_RED :
                state == P25_HEALTH_ID_STALE ? COL_AMBER :
                state == P25_HEALTH_ID_CURRENT ? COL_GREEN : COL_DIM);
        }
    }

    int iqpct = clampi((int)(P25.iq_level * 100.0f + 0.5f), 0, 100);
    bool clip = P25.iq_level >= 0.97f;
    if (_d_smeter) {
        char bar[96];
        char meter[128];
        ascii_bar(bar, sizeof(bar), iqpct, sdr_bar_width(_d_smeter, 9));
        if (clip) snprintf(meter, sizeof(meter), "S %s CLIP", bar);
        else      snprintf(meter, sizeof(meter), "S %s %3d%%", bar, iqpct);
        set_text_if_changed(_d_smeter, meter);
        sdr_color_if_changed(_d_smeter,
            clip ? COL_RED : iqpct < 5 ? SDR_PAS_ROSE : iqpct < 20 ? SDR_PAS_AMBER : SDR_PAS_GREEN);
    }
    if (clip && _d_rx) {
        set_text_if_changed(_d_rx, "CLIP");
        sdr_color_if_changed(_d_rx, COL_RED);
    }

    int bufpct = P25.ring_size > 0 ? (P25.ring_fill * 100) / P25.ring_size : 0;
    if (_d_bmeter) {
        char bar[96];
        char meter[128];
        ascii_bar(bar, sizeof(bar), bufpct, sdr_bar_width(_d_bmeter, 9));
        snprintf(meter, sizeof(meter), "B %s %3d%%", bar, bufpct);
        set_text_if_changed(_d_bmeter, meter);
        sdr_color_if_changed(_d_bmeter,
            bufpct < 20 ? SDR_PAS_ROSE : bufpct < 60 ? SDR_PAS_AMBER : SDR_PAS_GREEN);
    }

    if (_d_beepbtn_lbl)
        set_text_if_changed(_d_beepbtn_lbl,
                            P25.sync_beep_enabled ? "BEEP*" : "BEEP");
    if (_d_scan_btn_lbl)
        set_text_if_changed(_d_scan_btn_lbl,
                            scan_engine_active() ? "C-STOP" : "C-SCAN");
    /*LS-670: the HOLD label shows which TG is held, so an operator does not
     * have to remember what they pressed. Fits in the same 5-char slot as
     * BEEP*/
    if (_d_hold_btn_lbl) {
        uint16_t held = p25_scan_hold_get(&g_p25_scan);
        if (held) {
            char lbl[16];
            snprintf(lbl, sizeof(lbl), "HLD%u", (unsigned)held);
            set_text_if_changed(_d_hold_btn_lbl, lbl);
        } else {
            set_text_if_changed(_d_hold_btn_lbl, "HOLD");
        }
    }

    if (_d_gain_lbl) {
        char text[40];
        if (lakeshark_p25_agc_enabled())
            snprintf(text, sizeof(text), "GAIN  AGC %d.%d",
                     P25.rtl_gain_tenths / 10, P25.rtl_gain_tenths % 10);
        else if (P25.rtl_gain_tenths <= 0)
            snprintf(text, sizeof(text), "GAIN  AGC");
        else
            snprintf(text, sizeof(text), "GAIN  %d.%d dB",
                     P25.rtl_gain_tenths / 10, P25.rtl_gain_tenths % 10);
        set_text_if_changed(_d_gain_lbl, text);
    }
    sdr_seg_set(_d_gain_slider, P25.rtl_gain_tenths);
    if (_d_vol_lbl) {
        char text[32];
        snprintf(text, sizeof(text), "VOLUME  %d", audio_volume_get());
        set_text_if_changed(_d_vol_lbl, text);
    }
    sdr_seg_set(_d_vol_slider, audio_volume_get());

    lv_color_t scol = COL_AMBER;
    scan_phase_t scan_phase = scan_engine_phase();
    if (scan_phase != SCAN_PHASE_OFF) {
        scan_engine_status(buf, sizeof(buf));
        scol = scan_phase == SCAN_PHASE_ERROR ? COL_RED :
               scan_phase == SCAN_PHASE_NO_CANDIDATES ? COL_AMBER :
               scan_phase == SCAN_PHASE_HELD ? COL_GREEN : COL_CYAN;
    } else if (!radio.receiver_streaming) {
        snprintf(buf, sizeof(buf), "P25 RECEIVER ERROR: %s",
                 ls_radio_err_name(radio.receiver_error));
        scol = COL_RED;
    } else if (radio.tune_state == LS_IQ_RESULT_FAILED) {
        snprintf(buf, sizeof(buf), "P25 TUNE ERROR: %s",
                 ls_radio_err_name(radio.tune_error));
        scol = COL_RED;
    } else if (!P25.dsd_buffers_ok) {
        snprintf(buf, sizeof(buf), "*** DSD BUFFERS NOT ALLOCATED ***");
        scol = COL_RED;
    } else if (P25.dsd_sync_count == 0 && P25.iq_level < 0.05f) {
        snprintf(buf, sizeof(buf), "Waiting for signal (try GAIN or check antenna)");
    } else if (P25.dsd_sync_count == 0) {
        snprintf(buf, sizeof(buf), "Hunting for sync (signal %d%%, no lock yet)", iqpct);
    } else if (!P25.dsd_has_sync) {
        snprintf(buf, sizeof(buf), "Lost sync (last NAC 0x%03X, %d good frames)",
                 P25.dsd_last_ok_nac, P25.dsd_sync_count);
        scol = COL_DIM;
    } else {
        snprintf(buf, sizeof(buf), "LOCKED  %s  NAC=0x%03X", P25.dsd_modulation, P25.dsd_nac);
        scol = COL_GREEN;
    }
    set_text_if_changed(_d_status, buf);
    sdr_color_if_changed(_d_status, scol);
}

void AppP25::buildSignalTab(lv_obj_t *parent)
{
    ls_ui_style_content(parent);

    lv_obj_t *status = ls_ui_panel(parent, "RECEPTION");

    _s_hdr = make_label(status, &lv_font_montserrat_16, COL_CYAN);
    lv_label_set_text(_s_hdr, "DEMOD");

    _s_rf = make_label(status, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_width(_s_rf, lv_pct(100));
    lv_label_set_text(_s_rf, "IQ snapshot waiting");

    _s_peak = make_label(status, &lv_font_montserrat_14, COL_AMBER);
    lv_obj_set_width(_s_peak, lv_pct(100));
    lv_label_set_text(_s_peak, "PEAK --   tap spectrum to tune");

    _s_iqlbl = nullptr;
    _s_iqbar = make_label(status, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_width(_s_iqbar, lv_pct(100));

    /* LS-692: width and both view heights come from the active display.  The
     * shared widget allocates only its canvas in PSRAM; the P25 producer owns
     * no framebuffer and folds the already-owned decoder IQ.
     *
     * LS-736: two fifths of the DISPLAY is 320 rows on the 800 px panel, and
     * the tab does not have 320 rows to give once RECEPTION and the widget's
     * own VIEW/CONTRAST/FULL/GAIN rows are laid out - those rows fell off the
     * bottom and only appeared if you dragged.  Build the canvas at the seed
     * height, lay the real controls out, then grow the plot into exactly what
     * the measurement says is left.  The seed allocation is a few KB, so the
     * large PSRAM buffer is still allocated once, at a size that fits. */
    const int PLOT_SEED_H = 64;
    lv_obj_update_layout(parent);
    int plot_w = lv_obj_get_content_width(parent);
    int plot_points = plot_w / 2;
    if (plot_points < 32) plot_points = 32;
    if (plot_points > 240) plot_points = 240;
    if (plot_w >= 2 &&
        ls_spectrum_waterfall_build(
            &_s_spectrum, parent, "SPECTRUM / WATERFALL  (tap to tune)",
            plot_w, PLOT_SEED_H, plot_points, 50, 100, false,
            spectrumTapCb, this)) {
        ls_spectrum_waterfall_add_controls(
            &_s_spectrum,
            LS_SPECTRUM_CTL_SPLIT | LS_SPECTRUM_CTL_CONTRAST |
                LS_SPECTRUM_CTL_FULL | LS_SPECTRUM_CTL_GAIN,
            spectrumGainDownCb, spectrumGainUpCb, this, nullptr, nullptr);

        int plot_h = PLOT_SEED_H + (int)ls_ui_free_height(parent);
        if (plot_h > LS_SPECTRUM_CANVAS_MAX_HEIGHT)
            plot_h = LS_SPECTRUM_CANVAS_MAX_HEIGHT;
        if (plot_h > PLOT_SEED_H)
            (void)ls_spectrum_waterfall_resize(&_s_spectrum, _s_spectrum.width,
                                               plot_h);
    }
}

void AppP25::buildHealthTab(lv_obj_t *parent)
{
    ls_ui_style_content(parent);
    /* LS-736: one scroller per tab, and it is the page.  A scrollable panel
     * inside a scrollable page meant a row could be pushed out of view by
     * either of them, and the lower CONFIG rows behaved exactly that way -
     * see buildSettingsTab. */
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    parent = ls_ui_panel(parent, "P25 HEALTH");

    lv_obj_t *cl = make_label(parent, &lv_font_montserrat_12, COL_LABEL);
    lv_label_set_text(cl, "DECODE RATE (60s)   V=voice  S=sync");

    _s_chart = make_label(parent, &lv_font_montserrat_14, COL_AMBER);
    lv_obj_set_width(_s_chart, lv_pct(100));
    _s_voice = nullptr;
    _s_sync  = nullptr;

    lv_obj_t *p2 = make_panel(parent);
    _s_totals = make_label(p2, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_width(_s_totals, lv_pct(100));

    lv_obj_t *p3 = make_panel(parent);
    _s_err = make_label(p3, &lv_font_montserrat_14, COL_AMBER);
    lv_obj_set_width(_s_err, lv_pct(100));
    lv_label_set_text(_s_err, "LAST ERR: (none)");

    _s_gui = make_label(parent, &lv_font_montserrat_14, COL_DIM);
    lv_obj_set_width(_s_gui, lv_pct(100));
    lv_label_set_text(_s_gui, "GUI CALLBACK last --  max --  budget 20 ms");

    /* LS-736: the RTL front-end block moved off DECODE.  Gain, IQ throughput,
     * read errors, per-LDU decode cost and the endpoint's own streaming state
     * are all answers to "is the receiver well", which is this tab; on DECODE
     * they were four lines nobody reads during a call, sitting on top of the
     * actions everybody does. */
    lv_obj_t *radio_panel = ls_ui_panel(parent, "RECEIVER");
    _d_radio = make_label(radio_panel, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_width(_d_radio, lv_pct(100));
    lv_label_set_text(_d_radio, "RTL front end waiting");
}

void AppP25::rateSample(void)
{
    int64_t now = esp_timer_get_time();
    if (_last_sample_us == 0) { _last_sample_us = now;
                                _last_sync = P25.dsd_sync_count;
                                _last_voice = P25.dsd_voice_count; return; }
    if (now - _last_sample_us < 1000000LL) return;
    _last_sample_us = now;

    int dsync  = P25.dsd_sync_count  - _last_sync;
    int dvoice = P25.dsd_voice_count - _last_voice;
    _last_sync  = P25.dsd_sync_count;
    _last_voice = P25.dsd_voice_count;
    if (dsync  < 0) dsync  = 0;
    if (dvoice < 0) dvoice = 0;
    _rate_sync[_rate_head]  = dsync;
    _rate_voice[_rate_head] = dvoice;
    _rate_head = (_rate_head + 1) % RATE_N;
}

void AppP25::updateSignal(void)
{
    char buf[256], nac[16];
    if (P25.dsd_last_ok_nac)      snprintf(nac, sizeof(nac), "0x%03X", P25.dsd_last_ok_nac);
    else if (P25.dsd_nac)         snprintf(nac, sizeof(nac), "0x%03X", P25.dsd_nac);
    else                          snprintf(nac, sizeof(nac), "-----");
    /* LS-655: selected/hunting mode and DSD's observed modulation are both
     * shown. The former is the control decision; the latter is deliberately
     * only a slicer label. Collapsing them hid wrong-mode acquisition as a
     * weak signal. */
    snprintf(buf, sizeof(buf), "MODE %s    MOD %s    NAC %s",
             lakeshark_p25_mode_name(),
             P25.dsd_modulation[0] ? P25.dsd_modulation : "----", nac);
    set_text_if_changed(_s_hdr, buf);

    char bar[40];
    int iqpct = clampi((int)(P25.iq_level * 100.0f + 0.5f), 0, 100);
    ascii_bar(bar, sizeof(bar), iqpct, 20);
    snprintf(buf, sizeof(buf), "INPUT  %s %3d%%", bar, iqpct);
    set_text_if_changed(_s_iqbar, buf);
    sdr_color_if_changed(_s_iqbar,
        P25.iq_level < 0.05f ? COL_RED : P25.iq_level < 0.20f ? COL_AMBER : COL_GREEN);

    if (_s_spectrum.chart) {
        int points = _s_spectrum.points;
        if (points < 2) points = 2;
        if (points > 240) points = 240;
        float spectrum_bins[240];
        p25_spectrum_snapshot_t snapshot;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
        if (p25_spectrum_read(spectrum_bins, points, now_ms,
                              P25_SPECTRUM_STALE_MS, &snapshot)) {
            if (snapshot.sequence != _s_spectrum_seq) {
                _s_spectrum_seq = snapshot.sequence;
                ls_spectrum_waterfall_push(&_s_spectrum, spectrum_bins, points);
            }

            if (_s_rf) {
                if (snapshot.filter_hz) {
                    snprintf(buf, sizeof(buf),
                             "ACTUAL %lu.%06lu MHz   span %lu kHz   RF filter %lu kHz   %s",
                             (unsigned long)(snapshot.center_hz / 1000000UL),
                             (unsigned long)(snapshot.center_hz % 1000000UL),
                             (unsigned long)(snapshot.span_hz / 1000UL),
                             (unsigned long)(snapshot.filter_hz / 1000UL),
                             snapshot.following_voice ? "FOLLOWING VOICE" : "CONTROL");
                } else {
                    snprintf(buf, sizeof(buf),
                             "ACTUAL %lu.%06lu MHz   span %lu kHz   RF filter AUTO   %s",
                             (unsigned long)(snapshot.center_hz / 1000000UL),
                             (unsigned long)(snapshot.center_hz % 1000000UL),
                             (unsigned long)(snapshot.span_hz / 1000UL),
                             snapshot.following_voice ? "FOLLOWING VOICE" : "CONTROL");
                }
                set_text_if_changed(_s_rf, buf);
            }

            int peak = 0;
            for (int i = 1; i < points; i++)
                if (spectrum_bins[i] > spectrum_bins[peak]) peak = i;
            uint32_t peak_hz = 0;
            if (_s_peak && p25_spectrum_tap_hz(
                    peak, points, snapshot.center_hz, snapshot.span_hz,
                    P25_CONTROL_TUNER_MIN_HZ, P25_CONTROL_TUNER_MAX_HZ,
                    &peak_hz)) {
                snprintf(buf, sizeof(buf),
                         "PEAK %lu.%06lu MHz   %d%%   tap spectrum to tune",
                         (unsigned long)(peak_hz / 1000000UL),
                         (unsigned long)(peak_hz % 1000000UL),
                         (int)(spectrum_bins[peak] * 100.0f));
                set_text_if_changed(_s_peak, buf);
            }
        } else if (_s_rf) {
            set_text_if_changed(_s_rf,
                "IQ snapshot waiting/stale   bounded 1 FFT per 8 RX blocks");
            if (_s_spectrum.has_data)
                ls_spectrum_waterfall_clear(&_s_spectrum);
        }
    }

    char gain[24];
    int gain_tenths = lakeshark_p25_gain_tenths();
    snprintf(gain, sizeof(gain), "GAIN %d.%d", gain_tenths / 10,
             gain_tenths % 10);
    ls_spectrum_waterfall_set_gain_text(&_s_spectrum, gain);

}

void AppP25::updateHealth(void)
{
    char buf[256];
    int vmax = 1;
    for (int i = 0; i < RATE_N; i++) { if (_rate_voice[i] > vmax) vmax = _rate_voice[i];
                                       if (_rate_sync[i]  > vmax) vmax = _rate_sync[i]; }
    char sv[RATE_N + 1], ss[RATE_N + 1];
    ascii_spark(sv, sizeof(sv), _rate_voice, _rate_head, RATE_N, vmax);
    ascii_spark(ss, sizeof(ss), _rate_sync,  _rate_head, RATE_N, vmax);
    snprintf(buf, sizeof(buf), "V %s\nS %s", sv, ss);
    set_text_if_changed(_s_chart, buf);

    p25_health_snapshot_t health;
    static char health_text[768];
    if (p25_health_read(&health))
        p25_health_format_signal(&health, health_text, sizeof(health_text));
    else
        snprintf(health_text, sizeof(health_text), "HEALTH snapshot waiting");
    set_text_if_changed(_s_totals, health_text);

    snprintf(buf, sizeof(buf), "LAST ERR: %s", P25.dsd_err_str[0] ? P25.dsd_err_str : "(none)");
    set_text_if_changed(_s_err, buf);
    sdr_color_if_changed(_s_err, P25.dsd_err_str[0] ? COL_AMBER : COL_DIM);

    if (_s_gui) {
        snprintf(buf, sizeof(buf),
                 "GUI CALLBACK last %lu.%03lu ms  max %lu.%03lu ms  >20 ms %lu",
                 (unsigned long)(_gui_last_us / 1000U),
                 (unsigned long)(_gui_last_us % 1000U),
                 (unsigned long)(_gui_max_us / 1000U),
                 (unsigned long)(_gui_max_us % 1000U),
                 (unsigned long)_gui_over_budget);
        set_text_if_changed(_s_gui, buf);
        sdr_color_if_changed(_s_gui,
            _gui_over_budget ? COL_AMBER : COL_DIM);
    }

    /*LS-736*/
    if (_d_radio) {
        ls_iq_control_status_t radio;
        ls_receiver_presentation_t receiver;
        p25_get_receiver_status(&radio);
        ls_receiver_present(&radio, &receiver);

        char gain[24];
        if (P25.rtl_gain_tenths > 0)
            snprintf(gain, sizeof(gain), "%d.%d dB",
                     P25.rtl_gain_tenths / 10, P25.rtl_gain_tenths % 10);
        else
            snprintf(gain, sizeof(gain), "AGC");
        snprintf(buf, sizeof(buf),
                 "RTL GAIN %s   DEMOD %.0f   BEEP %s"
                 "\nIQ %lu KB/s   AUDIO %lu sps   READ ERR %d"
                 "\nDECODE %.0f ms/LDU (>180=choppy)   DROP %lu"
                 "\nP25 RX %s   FREQ %s",
                 gain, (double)P25.demod_gain,
                 P25.sync_beep_enabled ? "on" : "off",
                 (unsigned long)(P25.iq_bytes_sec / 1024),
                 (unsigned long)P25.audio_samples_sec, P25.read_errors,
                 (double)P25.dsd_decode_ms, (unsigned long)P25.audio_drops,
                 radio.receiver_streaming
                     ? "STREAMING" : ls_radio_err_name(radio.receiver_error),
                 receiver.frequency);
        set_text_if_changed(_d_radio, buf);
    }
}

/*LS-607*/
void AppP25::scanFit(lv_obj_t *t)
{
    if (!t) return;
    const int w = lv_obj_get_content_width(t);
    if (w < 240) return;

    static const int pct[4] = { 9, 31, 25, 13 };
    int used = 0;
    for (int c = 0; c < 4; c++) {
        int cw = (w * pct[c]) / 100;
        lv_table_set_col_width(t, c, cw);
        used += cw;
    }
    lv_table_set_col_width(t, 4, w - used);
}

/*LS-607*/
void AppP25::scanFitCb(lv_event_t *e)
{
    scanFit(lv_event_get_target(e));
}

void AppP25::buildScanTab(lv_obj_t *parent)
{
    ls_ui_style_content(parent);

    /*LS-736*/
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    parent = ls_ui_panel(parent, nullptr);

    /*LS-746*/
    /* The status line, SCAN/SKIP, SOURCE, the band range/step and HANG all
       used to be built here by hand - and the FM app built its own half of
       the same set, differently. They are one engine's controls, so they are
       now one shared widget. Anything added to the scanner belongs in
       ScanPanel, NOT here, or the two apps drift apart again. */
    _scan_panel.build(parent);

    ls_ui_value_t r;

    /*LS-702*/
    ls_ui_section(parent, "CARRIER SQUELCH  %");
    lv_obj_t *dt = nullptr;
    sdr_seg_slider(parent, SDR_PAS_LAV, 30, scan_engine_get_threshold_pct(),
                   p25_seg_thresh, this, &dt);

    /*LS-703*/
    /* LS-736: a zone is a position in a list, so it gets the shared
     * previous/next selector rather than a button per direction sized like a
     * labelled action. */
    ls_ui_value(parent, "ZONE", &r);
    _zone_val = r.value;
    ls_ui_stepper(&r, zonePrevCb, this, zoneNextCb, this);
    updateZone();

    /*LS-706*/
    /* LS-736: four labelled actions on one explicit line.  In the wrapping
     * controls row they were four screen/6 buttons plus gaps against a row
     * that is narrower than the display, which is how DEL ended up past the
     * right edge on the 480 px panel. */
    ls_ui_value(parent, "CHANNEL", &r);
    _ch_val = r.value;
    lv_obj_t *ch_group = ls_ui_button_group(r.controls);
    ls_ui_group_button(ch_group, "ADD",  LS_BTN_PRIMARY, chAddCb,  this, nullptr);
    ls_ui_group_button(ch_group, "NAME", LS_BTN_DEFAULT, chNameCb, this, nullptr);
    /*LS-711*/
    ls_ui_group_button(ch_group, "LOCK", LS_BTN_DEFAULT, chLockCb, this, nullptr);
    ls_ui_group_button(ch_group, "DEL",  LS_BTN_DANGER, chDelCb,  this, nullptr);
    updateChSel();

    /*LS-711*/
    ls_ui_section(parent, "CHANNELS");

    _scan_table = lv_table_create(parent);
    lv_obj_set_width(_scan_table, lv_pct(100));
    lv_obj_set_style_text_font(_scan_table, sdr_font_mono_sm(), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(_scan_table, COL_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_text_color(_scan_table, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(_scan_table, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_scan_table, LV_OPA_COVER, LV_PART_MAIN);
    ls_ui_style_table(_scan_table);

    lv_table_set_col_cnt(_scan_table, 5);
    lv_table_set_row_cnt(_scan_table, 1);
    static const char *hdr[5] = {"#", "NAME", "FREQ", "MODE", "FLAG"};
    for (int c = 0; c < 5; c++) {
        lv_table_set_cell_value(_scan_table, 0, c, hdr[c]);
        /*LS-607*/
        lv_table_add_cell_ctrl(_scan_table, 0, c, LV_TABLE_CELL_CTRL_TEXT_CROP);
    }
    lv_obj_add_event_cb(_scan_table, scanTableCb, LV_EVENT_VALUE_CHANGED, this);
    /*LS-607*/
    lv_obj_add_event_cb(_scan_table, scanFitCb, LV_EVENT_SIZE_CHANGED, this);
    scanFit(_scan_table);

    updateScan();
}

void AppP25::updateScan(void)
{
    /*LS-746*/
    _scan_panel.refresh();

    if (!_scan_table) return;
    int n   = scan_channels_count();
    /*LS-733*/
    /* s_cur is a GRID position in band mode, not a channel index, so it would
       put the ">" marker on an unrelated stored row. Only track it when the
       scanner is actually running the stored list. */
    int cur = (scan_engine_get_source() == SCAN_SRC_CHANNELS)
            ? scan_engine_current() : -1;

    /* LS-734: the visible SCAN tab rebuilt as many as 64 x 5 LVGL table
     * strings every 250 ms even when the stored list was unchanged.  Hash the
     * bounded channel model, as TALK GROUPS already does, and make the moving
     * scan cursor a one-cell update. */
    uint32_t sig = 2166136261u;
    sig = tg_hash_bytes(sig, &n, sizeof(n));
    for (int i = 0; i < n; ++i) {
        const scan_channel_t *channel = scan_channel_get(i);
        if (channel) sig = tg_hash_bytes(sig, channel, sizeof(*channel));
    }
    if (_scan_rendered && sig == _scan_render_sig) {
        if (cur != _scan_render_cur) {
            char num[8];
            if (_scan_render_cur >= 0 && _scan_render_cur < n) {
                snprintf(num, sizeof(num), "%d", _scan_render_cur);
                lv_table_set_cell_value(_scan_table,
                                        (uint16_t)(_scan_render_cur + 1), 0,
                                        num);
            }
            if (cur >= 0 && cur < n) {
                snprintf(num, sizeof(num), ">%d", cur);
                lv_table_set_cell_value(_scan_table, (uint16_t)(cur + 1), 0,
                                        num);
            }
            _scan_render_cur = cur;
        }
        return;
    }
    _scan_rendered = true;
    _scan_render_sig = sig;
    _scan_render_cur = cur;

    lv_table_set_row_cnt(_scan_table, n > 0 ? (uint16_t)(n + 1) : 2);
    for (int i = 0; i < n; i++) {
        const scan_channel_t *c = scan_channel_get(i);
        if (!c) continue;
        int row = i + 1;
        char num[8], freq[16], flag[12];
        snprintf(num, sizeof(num), "%s%d", (i == cur) ? ">" : "", i);
        snprintf(freq, sizeof(freq), "%.4f", c->freq_hz / 1e6);
        if (!(c->flags & SCAN_FLAG_ENABLED))   snprintf(flag, sizeof(flag), "OFF");
        else if (c->flags & SCAN_FLAG_LOCKOUT) snprintf(flag, sizeof(flag), "%sLOCK",
                                                        (c->flags & SCAN_FLAG_PRIORITY) ? "PRI " : "");
        else if (c->flags & SCAN_FLAG_PRIORITY) snprintf(flag, sizeof(flag), "PRI");
        else                                    flag[0] = 0;
        lv_table_set_cell_value(_scan_table, row, 0, num);
        lv_table_set_cell_value(_scan_table, row, 1, c->name);
        lv_table_set_cell_value(_scan_table, row, 2, freq);
        lv_table_set_cell_value(_scan_table, row, 3, scan_mode_name(c->mode));
        lv_table_set_cell_value(_scan_table, row, 4, flag);
        /*LS-607*/
        for (int col = 0; col < 5; col++)
            lv_table_add_cell_ctrl(_scan_table, row, col, LV_TABLE_CELL_CTRL_TEXT_CROP);
    }
    /*LS-607*/
    if (n == 0) {
        lv_table_set_cell_value(_scan_table, 1, 0, "-");
        lv_table_set_cell_value(_scan_table, 1, 1, "NO CHANNELS");
        for (int col = 0; col < 5; col++)
            lv_table_add_cell_ctrl(_scan_table, 1, col, LV_TABLE_CELL_CTRL_TEXT_CROP);
    }
}

void AppP25::scanToggleCb(lv_event_t *)
{
    if (scan_engine_active()) scan_engine_stop();
    else {
        /* LS-691: starting the shared carrier scanner is a manual tuning
           action and therefore cancels any profile survey first. */
        (void)p25_program_survey_cancel_now(P25_SURVEY_CANCEL_MANUAL_TUNE);
        scan_engine_start();
    }
}

void AppP25::scanSkipCb(lv_event_t *) { scan_engine_skip(); }

/*LS-670*/
void AppP25::holdCb(lv_event_t *)
{
    (void)p25_ui_hold_toggle();
}

void AppP25::lockCb(lv_event_t *)
{
    (void)p25_ui_lockout_current();
}

/*LS-746*/
/* scanSrcCb/updateSrc moved into ScanPanel - they were half of a control set
   whose other half lived in the FM app. */

void AppP25::scanTableCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    uint16_t row = 0, col = 0;
    lv_table_get_selected_cell(self->_scan_table, &row, &col);
    if (row < 1) return;
    int idx = (int)row - 1;
    const scan_channel_t *c = scan_channel_get(idx);
    if (!c) return;
    /*LS-711*/
    self->_sel_idx = idx;
    self->updateChSel();
}

/*LS-711*/
void AppP25::chLockCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (!self || self->_sel_idx < 0) return;
    const scan_channel_t *c = scan_channel_get(self->_sel_idx);
    if (!c) return;
    scan_channel_set_lockout(self->_sel_idx, !(c->flags & SCAN_FLAG_LOCKOUT));
    self->updateChSel();
}

/*LS-703*/
void AppP25::updateZone(void)
{
    if (!_zone_val) return;
    int z = scan_engine_get_zone();
    if (z < 0) lv_label_set_text(_zone_val, "ALL");
    else       lv_label_set_text_fmt(_zone_val, "%d", z);
}

/*LS-703*/
void AppP25::zonePrevCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    int z = scan_engine_get_zone();
    z = (z < 0) ? (SCAN_MAX_ZONES - 1) : (z - 1);
    scan_engine_set_zone(z);
    if (self) self->updateZone();
}

/*LS-703*/
void AppP25::zoneNextCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    int z = scan_engine_get_zone();
    z = (z < 0 || z >= SCAN_MAX_ZONES - 1) ? -1 : (z + 1);
    scan_engine_set_zone(z);
    if (self) self->updateZone();
}

/*LS-706*/
void AppP25::updateChSel(void)
{
    if (!_ch_val) return;
    const scan_channel_t *c = (_sel_idx >= 0) ? scan_channel_get(_sel_idx) : nullptr;
    if (!c) { lv_label_set_text(_ch_val, "none"); _sel_idx = -1; return; }
    /*LS-711*/
    lv_label_set_text_fmt(_ch_val, "%d %s%s", _sel_idx, c->name,
                          (c->flags & SCAN_FLAG_LOCKOUT) ? " LOCK" : "");
}

/*LS-706*/
void AppP25::chAddCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    uint32_t hz = lakeshark_p25_get_freq();
    if (hz == 0) return;

    int zone = scan_engine_get_zone();
    if (zone < 0) zone = 0;

    /*LS-706*/
    /*LS-723*/
    /* Dedup against the zone this is about to land in, not the whole store,
       and resolve the zone FIRST so the two agree. The console `ch add` now
       applies the same rule - that inconsistency was LS-711's open item. */
    int dup = scan_channel_find_freq_zone(hz, (uint8_t)zone);
    if (dup >= 0) {
        if (self) { self->_sel_idx = dup; self->updateChSel(); }
        return;
    }

    int idx = scan_channel_add(nullptr, hz, SCAN_MODE_P25, (uint8_t)zone);
    if (idx < 0) {
        if (self && self->_ch_val) lv_label_set_text(self->_ch_val, "list full");
        return;
    }
    if (self) { self->_sel_idx = idx; self->updateChSel(); }
}

/*LS-706*/
void AppP25::chDelCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (!self || self->_sel_idx < 0) return;
    scan_channel_remove(self->_sel_idx);
    self->_sel_idx = -1;
    self->updateChSel();
}

/*LS-706*/
void AppP25::chNameCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (!self || self->_sel_idx < 0) return;
    self->openNameEntry();
}

/*LS-706*/
void AppP25::openNameEntry(void)
{
    if (_name_entry) return;
    const scan_channel_t *c = scan_channel_get(_sel_idx);
    if (!c) return;
    /*LS-735*/
    /* Predates this session and is the same defect: %f through LVGL's own
       formatter. It would have taken the NAME modal down the same way. */
    char tbuf[48];
    snprintf(tbuf, sizeof(tbuf), "NAME CHANNEL %d  -  %.4f MHz",
             _sel_idx, c->freq_hz / 1e6);
    const ls_text_entry_config_t config = {
        .title = tbuf,
        .text = c->name,
        .placeholder = nullptr,
        .accepted_chars = nullptr,
        .max_length = SCAN_NAME_LEN - 1,
        .width = lv_pct(80),
        .mode = LS_TEXT_ENTRY_TEXT,
        .large = false,
    };
    _name_entry = ls_text_entry_open(&config, nameEntryDone, this);
}

/*LS-706*/
void AppP25::nameEntryDone(bool accepted, const char *text, void *user_data)
{
    AppP25 *self = static_cast<AppP25 *>(user_data);
    if (!self) return;
    self->_name_entry = nullptr;
    if (accepted && text && *text) scan_channel_set_name(self->_sel_idx, text);
    self->updateChSel();
}

/*LS-706*/
void AppP25::closeNameEntry(void)
{
    if (_name_entry) {
        ls_text_entry_t *entry = _name_entry;
        _name_entry = nullptr;
        ls_text_entry_close(entry);
    }
}

void AppP25::buildSettingsTab(lv_obj_t *parent)
{
    ls_ui_style_content(parent);

    /* LS-736: the page is the only scroller.  A scrollable panel inside a
     * scrollable page gives every row below the fold two ways to be out of
     * view and no way to tell which drag brings it back; the operator
     * reported the lower rows - USB auto-reboot among them - as having no
     * control on them at all.  The panel is content-sized and the page moves
     * it, so every row is reachable by one drag.
     *
     * The instructional prose that used to sit in the value column and in two
     * section titles is gone with it.  "Higher acquires faster; lower rejects
     * more noise" is manual text: on the panel it was a wrapped paragraph
     * where a value belongs, and it cost more rows than the setting it
     * described. */
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    parent = ls_ui_panel(parent, nullptr);

    ls_ui_value_t r;

    ls_ui_section(parent, "RADIO");

    ls_ui_value(parent, "FREQUENCY", &r);
    _set_freq_val = r.value;
    ls_ui_button(r.controls, "ENTER", LS_BTN_PRIMARY, freqEntryCb, this, nullptr);
    /* The four nudges are one explicit line: five wrapping screen/6 buttons
     * measured 424 px against a 430 px row, which is not a margin. */
    lv_obj_t *freq_group = ls_ui_button_group(r.controls);
    ls_ui_group_button(freq_group, "-1M",  LS_BTN_DEFAULT, freqM1Cb,  this, nullptr);
    ls_ui_group_button(freq_group, "-25k", LS_BTN_DEFAULT, freqm25Cb, this, nullptr);
    ls_ui_group_button(freq_group, "+25k", LS_BTN_DEFAULT, freqp25Cb, this, nullptr);
    ls_ui_group_button(freq_group, "+1M",  LS_BTN_DEFAULT, freqP1Cb,  this, nullptr);

    ls_ui_value(parent, "GAIN", &r);
    _set_gain_val = r.value;
    ls_ui_button(r.controls, "STEP", LS_BTN_DEFAULT, gainStepCb, this, nullptr);
    _set_agc_btn = ls_ui_button(r.controls, "AGC", LS_BTN_TOGGLE_OFF, agcCb2,
                                this, nullptr);

    ls_ui_value(parent, "DEMOD MODE", &r);
    _set_mode_val = r.value;
    ls_ui_button(r.controls, "CYCLE", LS_BTN_DEFAULT, modeCycleCb, this, nullptr);

    ls_ui_value(parent, "POLARITY", &r);
    _set_pol_val = r.value;
    _set_pol_btn = ls_ui_button(r.controls, "INVERT", LS_BTN_TOGGLE_OFF,
                                polarityCb, this, nullptr);

    ls_ui_value(parent, "SYNC BEEP", &r);
    _set_beep_val = r.value;
    _set_beep_btn = ls_ui_button(r.controls, "TOGGLE", LS_BTN_TOGGLE_OFF,
                                 beepCb2, this, nullptr);

    ls_ui_section(parent, "TRUNKING");

    ls_ui_value(parent, "AUTO FOLLOW", &r);
    _set_follow_val = r.value;
    _set_follow_btn = ls_ui_button(r.controls, "TOGGLE", LS_BTN_TOGGLE_OFF,
                                   autoFollowCb, this, nullptr);

    ls_ui_value(parent, "SKIP ENCRYPTED", &r);
    _set_skip_val = r.value;
    _set_skip_btn = ls_ui_button(r.controls, "TOGGLE", LS_BTN_TOGGLE_OFF,
                                 skipEncryptedCb, this, nullptr);

    ls_ui_value(parent, "ENCRYPTED SKIP", &r);
    _set_skip_ms_val = r.value;
    ls_ui_stepper(&r, skipDurationDownCb, this, skipDurationUpCb, this);

    ls_ui_value(parent, "PREFERENCE WRITE", &r);
    _set_control_status = r.value;
    lv_label_set_text(_set_control_status, "READY");

    ls_ui_section(parent, "ADVANCED CQPSK");

    ls_ui_value(parent, "TIMING GAIN", &r);
    _set_cqpsk_timing_val = r.value;
    ls_ui_stepper(&r, cqpskTimingDownCb, this, cqpskTimingUpCb, this);

    ls_ui_value(parent, "CARRIER GAIN", &r);
    _set_cqpsk_carrier_val = r.value;
    ls_ui_stepper(&r, cqpskCarrierDownCb, this, cqpskCarrierUpCb, this);

    ls_ui_value(parent, "CQPSK DEFAULTS", &r);
    lv_label_set_text(r.value, "");
    ls_ui_button(r.controls, "RESET", LS_BTN_DANGER, cqpskDefaultsCb, this, nullptr);

    ls_ui_section(parent, "FAVOURITES");

    ls_ui_value(parent, "SLOT", &r);
    _set_fav_val = r.value;
    ls_ui_stepper(&r, favPrevCb, this, favNextCb, this);

    ls_ui_value(parent, "ACTION", &r);
    lv_label_set_text(r.value, "");
    lv_obj_t *fav_group = ls_ui_button_group(r.controls);
    ls_ui_group_button(fav_group, "SAVE", LS_BTN_PRIMARY, favSaveCb, this, nullptr);
    ls_ui_group_button(fav_group, "TUNE", LS_BTN_PRIMARY, favTuneCb, this, nullptr);
    ls_ui_group_button(fav_group, "CLR",  LS_BTN_DANGER, favClrCb,  this, nullptr);

    ls_ui_section(parent, "VOICE (SAM)");

    ls_ui_value(parent, "PRESET", &r);
    _set_preset_val = r.value;
    ls_ui_stepper(&r, presetLeftCb, this, presetRightCb, this);

    ls_ui_value(parent, "LOW-PASS", &r);
    _set_lp_val = r.value;
    ls_ui_stepper(&r, lpLeftCb, this, lpRightCb, this);

    ls_ui_value(parent, "LOW-SHELF", &r);
    _set_shelf_val = r.value;
    ls_ui_stepper(&r, shelfLeftCb, this, shelfRightCb, this);

    ls_ui_value(parent, "VOICE TEST", &r);
    lv_label_set_text(r.value, "");
    ls_ui_button(r.controls, "SPEAK", LS_BTN_PRIMARY, voiceTestCb, this, nullptr);

    ls_ui_section(parent, "AUDIO");

    _set_vol_slider = sdr_seg_slider(parent, SDR_PAS_CYAN, 100, audio_volume_get(),
                                     p25_seg_vol, this, &_set_vol_val);

    ls_ui_section(parent, "VOICE GATE");
    _set_gate_slider = sdr_seg_slider(parent, SDR_PAS_LAV, 99, lakeshark_p25_voice_gate(),
                                      p25_seg_gate, this, &_set_gate_lbl);

    ls_ui_value(parent, "MUTE", &r);
    _set_mute_val = r.value;
    _set_mute_btn = ls_ui_button(r.controls, "TOGGLE", LS_BTN_TOGGLE_OFF,
                                 muteCb, this, nullptr);

    ls_ui_section(parent, "SYSTEM");

    ls_ui_value(parent, "USB AUTO-REBOOT", &r);
    _set_reboot_val = r.value;
    _set_reboot_btn = ls_ui_button(r.controls, "TOGGLE", LS_BTN_TOGGLE_OFF,
                                   rebootToggleCb, this, nullptr);

    /*LS-608*/
    ls_ui_section(parent, "DEFAULTS");
    ls_ui_value(parent, "RESET THIS APP", &r);
    _reset_val = r.value;
    lv_label_set_text(_reset_val, "");
    sdr_hold_btn(r.controls, "HOLD 2", 2000, defaultsCb, this);

    updateSettings();
}

/* LS-689: the PROGRAM page.
 *
 * Everything here reads the session and reports what it says. RELOAD does not
 * write a success label of its own: it asks for a reload and the next refresh
 * shows LOADING, then either the loaded system or the reason it was refused.
 * A panel that says "loaded" before the apply has run is worse than one that
 * says nothing, because the operator then trusts a frequency the radio is not
 * on.
 *
 * There is nothing to share with the other screens here. The value rows,
 * sections and buttons are already the shared kit; what is left is the P25
 * trunking profile itself, which no other app has. If a second protocol ever
 * grows a programmable system - DMR has the same shape - the session model in
 * p25_program.c is what would move, not this page. */
void AppP25::buildProgramTab(lv_obj_t *parent)
{
    ls_ui_style_content(parent);

    parent = ls_ui_panel(parent, nullptr);
    lv_obj_set_flex_grow(parent, 1);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    ls_ui_value_t r;

    ls_ui_section(parent, "PROFILE");

    ls_ui_value(parent, "STATE", &r);
    _pg_state = r.value;
    ls_ui_button(r.controls, "RELOAD", LS_BTN_PRIMARY, programReloadCb, this,
                 nullptr);

    ls_ui_value(parent, "SYSTEM", &r);
    _pg_system = r.value;

    ls_ui_value(parent, "SITE", &r);
    _pg_site = r.value;

    ls_ui_value(parent, "ROSTER", &r);
    _pg_roster = r.value;

    ls_ui_value(parent, "SOURCE", &r);
    _pg_source = r.value;

    /* The first-run answer to "so where do I put one". Always shown, not only
       when empty - an operator with a profile that will not load needs the
       path just as much as one who has never had a profile at all. */
    ls_ui_value(parent, "EXPECTED AT", &r);
    lv_label_set_text(r.value, P25_PROGRAM_DEFAULT_PATH);

    ls_ui_section(parent, "PROFILE CONTROL CHANNEL");

    ls_ui_value(parent, "SELECTED", &r);
    _pg_control = r.value;
    ls_ui_button(r.controls, "<", LS_BTN_DEFAULT, programPrevCb, this, nullptr);
    ls_ui_button(r.controls, ">", LS_BTN_DEFAULT, programNextCb, this, nullptr);

    /* LS-691: this is a PROGRAM/profile operation, so its UI is framed with
       the shared value/button kit and stays beside the profile control list.
       The legacy SCAN page is a carrier scanner shared with FM and cannot
       express protocol-valid P25 evidence. */
    ls_ui_value(parent, "CONTROL SURVEY", &r);
    _pg_survey = r.value;
    ls_ui_button(r.controls, "START", LS_BTN_PRIMARY, programSurveyCb, this,
                 nullptr);
    ls_ui_button(r.controls, "CANCEL", LS_BTN_DANGER,
                 programSurveyCancelCb, this, nullptr);

    ls_ui_section(parent, "CONTROL LIST");
    _pg_list = sdr_label(parent, sdr_font_mono_sm(), COL_TEXT);
    lv_obj_set_width(_pg_list, lv_pct(100));
    lv_label_set_long_mode(_pg_list, LV_LABEL_LONG_WRAP);

    ls_ui_section(parent, "LAST LOAD");
    _pg_status = sdr_label(parent, sdr_font_mono_sm(), COL_DIM);
    lv_obj_set_width(_pg_status, lv_pct(100));
    lv_label_set_long_mode(_pg_status, LV_LABEL_LONG_WRAP);

    updateProgram();
}

void AppP25::updateProgram(void)
{
    const p25_program_t *program = p25_program_session();
    const bool loaded = program && program->active_valid;
    char b[P25_PROGRAM_CONTROL_TEXT_MAX];

    if (_pg_state) set_text_if_changed(_pg_state, p25_program_state_name(program));
    if (_pg_system)
        set_text_if_changed(_pg_system, loaded ? program->active.system_name : "-");
    if (_pg_site)
        set_text_if_changed(_pg_site, loaded ? program->active.site_name : "-");
    if (_pg_roster) {
        p25_program_format_roster(program, b, sizeof(b));
        set_text_if_changed(_pg_roster, b);
    }
    if (_pg_source) {
        p25_program_format_source(program, b, sizeof(b));
        set_text_if_changed(_pg_source, b);
    }
    if (_pg_control) {
        p25_program_format_selected(program, b, sizeof(b));
        set_text_if_changed(_pg_control, b);
    }
    if (_pg_survey) {
        p25_program_format_survey(program, b, sizeof(b));
        set_text_if_changed(_pg_survey, b);
        const bool no_control = program &&
            program->survey.state == P25_SURVEY_NO_CONTROL;
        const bool canceled = program &&
            program->survey.state == P25_SURVEY_CANCELED;
        const bool found = program &&
            program->survey.state == P25_SURVEY_FOUND;
        sdr_color_if_changed(_pg_survey,
            (no_control || canceled) ? COL_AMBER : found ? COL_GREEN : COL_TEXT);
    }
    if (_pg_list) {
        p25_program_format_controls(program, b, sizeof(b));
        set_text_if_changed(_pg_list, b);
    }
    if (_pg_status) {
        p25_program_format_status(program, b, sizeof(b));
        set_text_if_changed(_pg_status, b);
        const bool failed = program && program->state == P25_PROGRAM_FAILED;
        sdr_color_if_changed(_pg_status, failed ? COL_RED : COL_DIM);
    }
}

void AppP25::programReloadCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    /* Deliberately ignoring the return: a refused request is already visible
       in the state the next line reads back. */
    (void)p25_program_request_reload();
    if (self) self->updateProgram();
}

void AppP25::programPrevCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    (void)p25_program_step_control_now(-1);
    if (self) self->updateProgram();
}

void AppP25::programNextCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    (void)p25_program_step_control_now(+1);
    if (self) self->updateProgram();
}

void AppP25::programSurveyCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    /* There can only be one tuner owner.  The carrier scan engine is stopped
       before the profile survey takes the latch; it remains stopped after
       survey completion rather than silently resuming stale scan work. */
    scan_engine_stop();
    (void)p25_program_survey_start_now();
    if (self) self->updateProgram();
}

void AppP25::programSurveyCancelCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    (void)p25_program_survey_cancel_now(P25_SURVEY_CANCEL_OPERATOR);
    if (self) self->updateProgram();
}

/* LS-690: TALK GROUPS is P25-specific policy, while all of its visual
 * structure comes from the shared kit.  The table is only a projection of
 * p25_program's active profile and scan_ctrl's effective state; it owns one
 * selected TG ID and no policy flags. */
void AppP25::buildTalkgroupsTab(lv_obj_t *parent)
{
    ls_ui_style_content(parent);

    parent = ls_ui_panel(parent, nullptr);
    lv_obj_set_flex_grow(parent, 1);

    ls_ui_value_t r;
    ls_ui_value(parent, "POLICY", &r);
    _tg_summary = r.value;
    ls_ui_button(r.controls, "OPEN / ALLOW", LS_BTN_PRIMARY, tgModeCb, this,
                 nullptr);

    /* LS-780: observations are RAM only and change no scan rule, so they get
       their own fixed-height list rather than being mixed into the programmed
       table, where an operator would read them as configured aliases. */
    /*LS-804  A heading names the section. It is not the place for a caveat
       about where the data lives - that was both wider than the panel, so it
       clipped mid-sentence, and an implementation detail the operator has no
       use for. Same for the "tap row = select" hints on the two headings
       below: the rows are obviously tappable. */
    ls_ui_section(parent, "OBSERVED IDS");
    _tg_observed = lv_table_create(parent);
    ls_ui_style_table(_tg_observed);
    lv_obj_set_width(_tg_observed, lv_pct(100));
    lv_obj_set_height(_tg_observed, 160);
    /*LS-804  ACTIVE made the bar appear and vanish as rows arrived. AUTO
       draws it only while the table is actually being scrolled. */
    lv_obj_set_scrollbar_mode(_tg_observed, LV_SCROLLBAR_MODE_AUTO);
    lv_table_set_col_cnt(_tg_observed, 3);
    const lv_coord_t observed_width = lv_disp_get_hor_res(lv_obj_get_disp(parent)) - 64;
    lv_table_set_col_width(_tg_observed, 0, observed_width / 5);
    lv_table_set_col_width(_tg_observed, 1, observed_width * 3 / 5);
    lv_table_set_col_width(_tg_observed, 2, observed_width / 5);
    lv_table_set_row_cnt(_tg_observed, 2);
    lv_table_set_cell_value(_tg_observed, 0, 0, "TG ID");
    lv_table_set_cell_value(_tg_observed, 0, 1, "RX MHz / NAC");
    lv_table_set_cell_value(_tg_observed, 0, 2, "AGE s");
    _tg_observed_second = UINT32_MAX;

    ls_ui_section(parent, "PROGRAMMED ALIASES");
    _tg_table = lv_table_create(parent);
    lv_obj_set_width(_tg_table, lv_pct(100));
    lv_obj_set_flex_grow(_tg_table, 1);
    lv_obj_set_scrollbar_mode(_tg_table, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_text_font(_tg_table, sdr_font_mono_sm(), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(_tg_table, COL_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_text_color(_tg_table, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(_tg_table, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_tg_table, LV_OPA_COVER, LV_PART_MAIN);
    ls_ui_style_table(_tg_table);

    lv_table_set_col_cnt(_tg_table, 7);
    const lv_coord_t width = lv_disp_get_hor_res(lv_obj_get_disp(parent));
    const lv_coord_t id_width = width / 8;
    const lv_coord_t alias_width = (width * 2) / 7;
    const lv_coord_t flag_width = (width - id_width - alias_width) / 5;
    lv_table_set_col_width(_tg_table, 0, id_width);
    lv_table_set_col_width(_tg_table, 1, alias_width);
    for (int col = 2; col < 7; ++col)
        lv_table_set_col_width(_tg_table, col, flag_width);
    static const char *headers[7] = {
        "ID", "ALIAS", "CFG", "LIST", "HOLD", "LOCK", "PRI"
    };
    lv_table_set_row_cnt(_tg_table, 1);
    for (int col = 0; col < 7; ++col) {
        lv_table_set_cell_value(_tg_table, 0, col, headers[col]);
        lv_table_add_cell_ctrl(_tg_table, 0, col,
                               LV_TABLE_CELL_CTRL_TEXT_CROP);
    }
    lv_obj_add_event_cb(_tg_table, tgTableCb, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *controls = ls_ui_controls(parent);
    ls_ui_button(controls, "LIST", LS_BTN_TOGGLE_OFF, tgListCb, this, nullptr);
    ls_ui_button(controls, "HOLD", LS_BTN_TOGGLE_OFF, tgHoldCb, this, nullptr);
    ls_ui_button(controls, "LOCK", LS_BTN_DANGER, tgLockCb, this, nullptr);
    ls_ui_button(controls, "PRI -", LS_BTN_DEFAULT, tgPriorityDownCb, this,
                 nullptr);
    ls_ui_button(controls, "PRI +", LS_BTN_DEFAULT, tgPriorityUpCb, this,
                 nullptr);

    ls_ui_value(parent, "EDIT", &r);
    _tg_status = r.value;
    lv_label_set_text(_tg_status, "SELECT A TALK GROUP");
    updateTalkgroups();
}

void AppP25::updateTalkgroups(void)
{
    updateObservedTalkgroups();
    if (!_tg_table) return;
    const p25_profile_t *profile = active_tg_profile();
    (void)p25_tg_roster_bind(&_tg_roster, profile);

    /* This tab may contain 64 x 7 cells.  Hash the bounded source state and
     * rebuild only when it changes; rewriting 448 LVGL strings four times a
     * second while decoding voice is needless display-heap churn. */
    uint32_t sig = 2166136261u;
    sig = tg_hash_bytes(sig, &_tg_roster.selected_tg,
                        sizeof(_tg_roster.selected_tg));
    if (profile) {
        sig = tg_hash_bytes(sig, &profile->talkgroup_count,
                            sizeof(profile->talkgroup_count));
        sig = tg_hash_bytes(sig, profile->talkgroups,
                            (size_t)profile->talkgroup_count *
                                sizeof(profile->talkgroups[0]));
    }
    sig = tg_hash_bytes(sig, &g_p25_scan.list_mode,
                        sizeof(g_p25_scan.list_mode));
    sig = tg_hash_bytes(sig, &g_p25_scan.hold_tg,
                        sizeof(g_p25_scan.hold_tg));
    sig = tg_hash_bytes(sig, &g_p25_scan.allow_count,
                        sizeof(g_p25_scan.allow_count));
    sig = tg_hash_bytes(sig, g_p25_scan.allow,
                        (size_t)g_p25_scan.allow_count *
                            sizeof(g_p25_scan.allow[0]));
    sig = tg_hash_bytes(sig, &g_p25_scan.lockout_count,
                        sizeof(g_p25_scan.lockout_count));
    sig = tg_hash_bytes(sig, g_p25_scan.lockout,
                        (size_t)g_p25_scan.lockout_count *
                            sizeof(g_p25_scan.lockout[0]));
    sig = tg_hash_bytes(sig, &g_p25_scan.priority_count,
                        sizeof(g_p25_scan.priority_count));
    sig = tg_hash_bytes(sig, g_p25_scan.priority,
                        (size_t)g_p25_scan.priority_count *
                            sizeof(g_p25_scan.priority[0]));
    sig = tg_hash_bytes(sig, &g_p25_scan.names_count,
                        sizeof(g_p25_scan.names_count));
    sig = tg_hash_bytes(sig, g_p25_scan.names,
                        (size_t)g_p25_scan.names_count *
                            sizeof(g_p25_scan.names[0]));
    if (_tg_rendered && sig == _tg_render_sig) return;
    _tg_rendered = true;
    _tg_render_sig = sig;

    char summary[160];
    p25_tg_roster_format_summary(profile, &g_p25_scan, summary,
                                 sizeof(summary));
    if (_tg_summary) set_text_if_changed(_tg_summary, summary);

    const uint16_t count = profile ? profile->talkgroup_count : 0;
    lv_table_set_row_cnt(_tg_table, count ? (uint16_t)(count + 1U) : 2);
    if (!count) {
        lv_table_set_cell_value(_tg_table, 1, 0, "-");
        lv_table_set_cell_value(_tg_table, 1, 1,
                                profile ? "NO TALK GROUPS" : "NO PROFILE");
        for (int col = 2; col < 7; ++col)
            lv_table_set_cell_value(_tg_table, 1, col, "");
        return;
    }

    for (uint16_t i = 0; i < count; ++i) {
        p25_tg_roster_row_t item;
        if (!p25_tg_roster_row(profile, &g_p25_scan, i, &item)) continue;
        const uint16_t row = (uint16_t)(i + 1U);
        char id[12], priority[8];
        snprintf(id, sizeof(id), "%c%u",
                 item.id == _tg_roster.selected_tg ? '>' : ' ',
                 (unsigned)item.id);
        if (item.priority)
            snprintf(priority, sizeof(priority), "%u", (unsigned)item.priority);
        else
            snprintf(priority, sizeof(priority), "-");
        lv_table_set_cell_value(_tg_table, row, 0, id);
        lv_table_set_cell_value(_tg_table, row, 1, item.alias);
        lv_table_set_cell_value(_tg_table, row, 2,
                                item.profile_enabled ? "ON" : "OFF");
        lv_table_set_cell_value(_tg_table, row, 3,
                                item.list_member ? "YES" : "NO");
        lv_table_set_cell_value(_tg_table, row, 4, item.held ? "YES" : "-");
        lv_table_set_cell_value(_tg_table, row, 5,
                                item.locked_out ? "YES" : "-");
        lv_table_set_cell_value(_tg_table, row, 6, priority);
        for (int col = 0; col < 7; ++col)
            lv_table_add_cell_ctrl(_tg_table, row, col,
                                   LV_TABLE_CELL_CTRL_TEXT_CROP);
    }
}

void AppP25::updateObservedTalkgroups(void)
{
    if (!_tg_observed) return;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    uint32_t second = now_ms / 1000U;
    /* One read per second, and a failed try-lock simply skips this tick -
       the decoder must never wait on the GUI to publish an observation. */
    if (second == _tg_observed_second || !p25_tg_observed_read(&s_observed_view)) return;
    _tg_observed_second = second;
    const unsigned count = s_observed_view.count;
    lv_table_set_row_cnt(_tg_observed, count ? count + 1 : 2);
    auto cell = [this](unsigned row, unsigned col, const char *text) {
        if (strcmp(lv_table_get_cell_value(_tg_observed, row, col), text))
            lv_table_set_cell_value(_tg_observed, row, col, text);
    };
    if (!count) {
        cell(1, 0, "-"); cell(1, 1, "Waiting for validated TG"); cell(1, 2, "-");
        return;
    }
    for (unsigned i = 0; i < count; ++i) {
        const auto &entry = s_observed_view.rows[i];
        char text[48];
        snprintf(text, sizeof(text), "%u", (unsigned)entry.talkgroup);
        cell(i + 1, 0, text);
        snprintf(text, sizeof(text), "%lu.%06lu / %03X",
            (unsigned long)(entry.channel_hz / 1000000ULL),
            (unsigned long)(entry.channel_hz % 1000000ULL), (unsigned)entry.nac);
        cell(i + 1, 1, text);
        snprintf(text, sizeof(text), "%lu", (unsigned long)((now_ms - entry.last_seen_ms) / 1000U));
        cell(i + 1, 2, text);
    }
}

void AppP25::finishTalkgroupEdit(p25_tg_edit_result_t result, bool persistent,
                                 const char *success)
{
    if (result == P25_TG_EDIT_OK && persistent)
        p25_scan_persist_save_now();
    if (_tg_status) {
        const char *text = result == P25_TG_EDIT_OK
                               ? success : p25_tg_edit_result_text(result);
        lv_label_set_text(_tg_status, text);
        lv_obj_set_style_text_color(_tg_status,
                                    result == P25_TG_EDIT_OK ? COL_GREEN : COL_RED,
                                    0);
    }
    _tg_rendered = false;
    updateTalkgroups();
}

void AppP25::tgTableCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (!self || !self->_tg_table) return;
    uint16_t row = LV_TABLE_CELL_NONE, col = LV_TABLE_CELL_NONE;
    lv_table_get_selected_cell(self->_tg_table, &row, &col);
    if (row == LV_TABLE_CELL_NONE || row == 0) return;
    const p25_profile_t *profile = active_tg_profile();
    if (!p25_tg_roster_select(&self->_tg_roster, profile,
                              (size_t)(row - 1U)))
        return;
    if (self->_tg_status) {
        char text[48];
        snprintf(text, sizeof(text), "SELECTED TG %u",
                 (unsigned)self->_tg_roster.selected_tg);
        lv_label_set_text(self->_tg_status, text);
        lv_obj_set_style_text_color(self->_tg_status, COL_TEXT, 0);
    }
    self->_tg_rendered = false;
    self->updateTalkgroups();
}

void AppP25::tgModeCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (!self) return;
    p25_scan_list_mode_t next =
        p25_scan_list_get_mode(&g_p25_scan) == P25_SCAN_LIST_ALLOW
            ? P25_SCAN_LIST_OFF : P25_SCAN_LIST_ALLOW;
    self->finishTalkgroupEdit(p25_tg_roster_set_mode(&g_p25_scan, next), true,
                              next == P25_SCAN_LIST_ALLOW
                                  ? "ALLOW-LIST MODE" : "OPEN-MONITOR MODE");
}

void AppP25::tgListCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (!self) return;
    self->finishTalkgroupEdit(
        p25_tg_roster_toggle_allow(&self->_tg_roster, active_tg_profile(),
                                   &g_p25_scan),
        true, "LIST MEMBERSHIP UPDATED");
}

void AppP25::tgHoldCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (!self) return;
    self->finishTalkgroupEdit(
        p25_tg_roster_toggle_hold(&self->_tg_roster, active_tg_profile(),
                                  &g_p25_scan),
        true, "HOLD UPDATED");
}

void AppP25::tgLockCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (!self) return;
    const p25_profile_t *profile = active_tg_profile();
    uint16_t tg = p25_tg_roster_selected_id(&self->_tg_roster, profile);
    bool was_locked = p25_scan_is_locked_out(&g_p25_scan, tg);
    p25_tg_edit_result_t result = p25_tg_roster_toggle_lockout(
        &self->_tg_roster, profile, &g_p25_scan);
    if (result == P25_TG_EDIT_OK && !was_locked &&
        P25.grant_on_traffic && P25.grant_talkgroup == tg)
        p25_return_to_control();
    self->finishTalkgroupEdit(result, true, "LOCKOUT UPDATED");
}

void AppP25::tgPriorityDownCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (!self) return;
    self->finishTalkgroupEdit(
        p25_tg_roster_priority_delta(&self->_tg_roster, active_tg_profile(),
                                     &g_p25_scan, -1),
        false, "PRIORITY UPDATED (RUNTIME)");
}

void AppP25::tgPriorityUpCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (!self) return;
    self->finishTalkgroupEdit(
        p25_tg_roster_priority_delta(&self->_tg_roster, active_tg_profile(),
                                     &g_p25_scan, +1),
        false, "PRIORITY UPDATED (RUNTIME)");
}

void AppP25::updateSettings(void)
{
    char b[40];
    if (_set_freq_val) {
        uint32_t f = lakeshark_p25_get_freq();
        p25_controls_format_mhz(b, sizeof(b), f);
        set_text_if_changed(_set_freq_val, b);
    }
    if (_set_gain_val) {
        int g = lakeshark_p25_gain_tenths();
        if (lakeshark_p25_agc_enabled()) {
            snprintf(b, sizeof(b), "AGC  %d.%d dB", g / 10, g % 10);
            set_text_if_changed(_set_gain_val, b);
        }
        else if (g <= 0) set_text_if_changed(_set_gain_val, "AGC");
        else { snprintf(b, sizeof(b), "%d.%d dB", g / 10, g % 10);
               set_text_if_changed(_set_gain_val, b); }
    }
    if (_set_mode_val) set_text_if_changed(_set_mode_val, lakeshark_p25_mode_name());
    if (_set_pol_val)  set_text_if_changed(_set_pol_val,
                                           lakeshark_p25_polarity_inverted() ? "INVERTED" : "NORMAL");
    if (_set_beep_val) set_text_if_changed(_set_beep_val,
                                           lakeshark_p25_beep_enabled() ? "ON" : "OFF");
    if (_set_follow_val)
        set_text_if_changed(_set_follow_val, p25_get_auto_follow() ? "ON" : "OFF");
    if (_set_skip_val)
        set_text_if_changed(_set_skip_val, p25_get_leave_on_encrypted() ? "ON" : "OFF");

    /* LS-736: the button carries the state as well as the value column, so a
     * row that is switched on is identifiable as a control that is on rather
     * than as a line of text that happens to read ON. */
    set_toggle(_set_agc_btn, lakeshark_p25_agc_enabled());
    set_toggle(_set_pol_btn, lakeshark_p25_polarity_inverted());
    set_toggle(_set_beep_btn, lakeshark_p25_beep_enabled());
    set_toggle(_set_follow_btn, p25_get_auto_follow());
    set_toggle(_set_skip_btn, p25_get_leave_on_encrypted());
    set_toggle(_set_mute_btn, audio_is_muted());
    set_toggle(_set_reboot_btn, app_usb_autoreboot());
    if (_set_skip_ms_val) {
        snprintf(b, sizeof(b), "%lu s",
                 (unsigned long)(p25_get_encrypted_skip_ms() / 1000u));
        set_text_if_changed(_set_skip_ms_val, b);
    }
    if (_set_cqpsk_timing_val || _set_cqpsk_carrier_val) {
        p25_cqpsk_config_t config;
        bool pending = false;
        p25_get_cqpsk_config(&config, &pending);
        if (_set_cqpsk_timing_val) {
            snprintf(b, sizeof(b), "%.8f%s", (double)config.timing_gain,
                     pending ? "  QUEUED" : "");
            set_text_if_changed(_set_cqpsk_timing_val, b);
        }
        if (_set_cqpsk_carrier_val) {
            snprintf(b, sizeof(b), "%.4f%s", (double)config.carrier_gain,
                     pending ? "  QUEUED" : "");
            set_text_if_changed(_set_cqpsk_carrier_val, b);
        }
    }
    if (_set_fav_val) {
        const app_t *a = app_current();
        uint32_t f = a ? settings_fav_get(a, _fav_slot) : 0;
        if (f) snprintf(b, sizeof(b), "%d/%d: %lu.%03lu", _fav_slot + 1, MAX_FAVOURITES,
                        (unsigned long)(f / 1000000UL), (unsigned long)((f / 1000UL) % 1000UL));
        else   snprintf(b, sizeof(b), "%d/%d: (empty)", _fav_slot + 1, MAX_FAVOURITES);
        set_text_if_changed(_set_fav_val, b);
    }
    if (_set_preset_val)
        set_text_if_changed(_set_preset_val,
            sam_tts_preset_name((sam_tts_voice_preset_t)settings_voice_preset_get()));
    if (_set_lp_val)
        set_text_if_changed(_set_lp_val, sam_tts_lowpass_name(settings_voice_lowpass_get()));
    if (_set_shelf_val)
        set_text_if_changed(_set_shelf_val, sam_tts_lowshelf_name(settings_voice_lowshelf_get()));
    if (_set_vol_val) {
        snprintf(b, sizeof(b), "VOLUME  %d", audio_volume_get());
        set_text_if_changed(_set_vol_val, b);
    }
    sdr_seg_set(_set_vol_slider, audio_volume_get());
    if (_set_gate_lbl) {
        snprintf(b, sizeof(b), "VOICE GATE  %d", lakeshark_p25_voice_gate());
        set_text_if_changed(_set_gate_lbl, b);
    }
    sdr_seg_set(_set_gate_slider, lakeshark_p25_voice_gate());
    if (_set_mute_val)
        set_text_if_changed(_set_mute_val, audio_is_muted() ? "MUTED" : "ON");
    if (_set_reboot_val)
        set_text_if_changed(_set_reboot_val, app_usb_autoreboot() ? "ON" : "OFF");
}

static void p25_cycle_preset(int dir)
{
    int p = (settings_voice_preset_get() + SAM_PRESET_COUNT + dir) % SAM_PRESET_COUNT;
    settings_voice_preset_set(p);
    sam_tts_set_preset((sam_tts_voice_preset_t)p);
}
static void p25_cycle_lp(int dir)
{
    int m = (settings_voice_lowpass_get() + 3 + dir) % 3;
    settings_voice_lowpass_set(m);
    sam_tts_set_lowpass(m);
}
static void p25_cycle_shelf(int dir)
{
    int m = (settings_voice_lowshelf_get() + 3 + dir) % 3;
    settings_voice_lowshelf_set(m);
    sam_tts_set_lowshelf(m);
}

void AppP25::freqM1Cb(lv_event_t *)  { lakeshark_p25_tune(-1000000); }
void AppP25::freqm25Cb(lv_event_t *) { lakeshark_p25_tune(-25000); }
void AppP25::freqp25Cb(lv_event_t *) { lakeshark_p25_tune(+25000); }
void AppP25::freqP1Cb(lv_event_t *)  { lakeshark_p25_tune(+1000000); }
void AppP25::gainStepCb(lv_event_t *){ lakeshark_p25_gain_step(); }
void AppP25::agcCb2(lv_event_t *)    { lakeshark_p25_agc(); }
void AppP25::modeCycleCb(lv_event_t *){ lakeshark_p25_cycle_mode(); }
void AppP25::polarityCb(lv_event_t *){ lakeshark_p25_toggle_polarity(); }
void AppP25::beepCb2(lv_event_t *)   { lakeshark_p25_beep_toggle(); }

void AppP25::cqpskTimingDownCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (self) self->setControlStatus(p25_step_cqpsk_timing(-1), "CQPSK QUEUED");
}
void AppP25::cqpskTimingUpCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (self) self->setControlStatus(p25_step_cqpsk_timing(+1), "CQPSK QUEUED");
}
void AppP25::cqpskCarrierDownCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (self) self->setControlStatus(p25_step_cqpsk_carrier(-1), "CQPSK QUEUED");
}
void AppP25::cqpskCarrierUpCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (self) self->setControlStatus(p25_step_cqpsk_carrier(+1), "CQPSK QUEUED");
}
void AppP25::cqpskDefaultsCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (self) self->setControlStatus(p25_reset_cqpsk_config(),
                                     "TESTED VALUES QUEUED");
}

void AppP25::setControlStatus(bool ok, const char *ok_text)
{
    if (!_set_control_status) return;
    lv_label_set_text(_set_control_status, ok ? ok_text : "SAVE FAILED");
    lv_obj_set_style_text_color(_set_control_status, ok ? SDR_OK : SDR_ERR, 0);
}

void AppP25::autoFollowCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (self) self->setControlStatus(p25_set_auto_follow(!p25_get_auto_follow()));
}

void AppP25::skipEncryptedCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (self) self->setControlStatus(
        p25_set_leave_on_encrypted(!p25_get_leave_on_encrypted()));
}

void AppP25::skipDurationDownCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    uint32_t cur = p25_get_encrypted_skip_ms();
    uint32_t next = cur > P25_CONTROL_ENCRYPTED_SKIP_MIN_MS + 5000u
                        ? cur - 5000u : P25_CONTROL_ENCRYPTED_SKIP_MIN_MS;
    if (self) self->setControlStatus(p25_set_encrypted_skip_ms(next));
}

void AppP25::skipDurationUpCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    uint32_t cur = p25_get_encrypted_skip_ms();
    uint32_t next = cur < P25_CONTROL_ENCRYPTED_SKIP_MAX_MS - 5000u
                        ? cur + 5000u : P25_CONTROL_ENCRYPTED_SKIP_MAX_MS;
    if (self) self->setControlStatus(p25_set_encrypted_skip_ms(next));
}

void AppP25::favPrevCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    self->_fav_slot = (self->_fav_slot + MAX_FAVOURITES - 1) % MAX_FAVOURITES;
}
void AppP25::favNextCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    self->_fav_slot = (self->_fav_slot + 1) % MAX_FAVOURITES;
}
void AppP25::favSaveCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    const app_t *a = app_current();
    if (a) settings_fav_set(a, self->_fav_slot, lakeshark_p25_get_freq());
}
void AppP25::favTuneCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    const app_t *a = app_current();
    uint32_t f = a ? settings_fav_get(a, self->_fav_slot) : 0;
    if (f) lakeshark_p25_set_freq(f);
}
void AppP25::favClrCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    const app_t *a = app_current();
    if (a) settings_fav_clear(a, self->_fav_slot);
}

void AppP25::presetLeftCb(lv_event_t *)  { p25_cycle_preset(-1); }
void AppP25::presetRightCb(lv_event_t *) { p25_cycle_preset(+1); }
void AppP25::lpLeftCb(lv_event_t *)      { p25_cycle_lp(-1); }
void AppP25::lpRightCb(lv_event_t *)     { p25_cycle_lp(+1); }
void AppP25::shelfLeftCb(lv_event_t *)   { p25_cycle_shelf(-1); }
void AppP25::shelfRightCb(lv_event_t *)  { p25_cycle_shelf(+1); }
void AppP25::voiceTestCb(lv_event_t *)   { audio_out_ensure_unmuted(); audio_events_play_test(); }
void AppP25::rebootToggleCb(lv_event_t *){ app_set_usb_autoreboot(!app_usb_autoreboot()); }
void AppP25::volDownCb(lv_event_t *)     { audio_volume_delta(-5); }
void AppP25::volUpCb(lv_event_t *)       { audio_volume_delta(+5); }
void AppP25::muteCb(lv_event_t *)        { audio_toggle_mute(); }
void AppP25::volSliderCb(lv_event_t *e)
{
    audio_volume_set((int)lv_slider_get_value(lv_event_get_target(e)));
}

void AppP25::timerCb(lv_timer_t *t)
{
    AppP25 *self = static_cast<AppP25 *>(t->user_data);
    if (!self->_tabview) return;

    const int64_t started_us = esp_timer_get_time();
    self->rateSample();
    uint16_t active_tab = lv_tabview_get_tab_act(self->_tabview);
    p25_spectrum_enable(active_tab == 1);
    switch (active_tab) {
        case 0:  self->updateDecode();   break;
        case 1:  self->updateSignal();   break;
        case 2:  self->updateHealth();   break;
        case 3:  self->updateScan();     break;
        case 4:  self->updateSettings(); break;
        /*LS-689*/
        case 5:  self->updateProgram();  break;
        /*LS-690*/
        case 6:  self->updateTalkgroups(); break;
        default: break;
    }

    /* LS-734: RF counters proved the receiver was moving, but there was no
     * evidence for time spent inside the visible-tab GUI callback.  Retain the
     * last/max duration and budget misses for the HEALTH tab, and rate-limit a
     * serial warning so a slow panel can be diagnosed without log-driven lag.
     * This measures callback work; DSI flush/touch latency remains a hardware
     * observation. */
    const int64_t finished_us = esp_timer_get_time();
    const int64_t elapsed_us = finished_us - started_us;
    self->_gui_last_us = elapsed_us > 0 ? (uint32_t)elapsed_us : 0U;
    if (self->_gui_last_us > self->_gui_max_us)
        self->_gui_max_us = self->_gui_last_us;
    if (self->_gui_last_us > P25_GUI_TICK_BUDGET_US) {
        ++self->_gui_over_budget;
        if (self->_gui_last_warn_us == 0 ||
            finished_us - self->_gui_last_warn_us >= 5000000LL) {
            self->_gui_last_warn_us = finished_us;
            ESP_LOGW(TAG, "GUI tab %u callback %lu us (budget %lu us, max %lu)",
                     (unsigned)active_tab, (unsigned long)self->_gui_last_us,
                     (unsigned long)P25_GUI_TICK_BUDGET_US,
                     (unsigned long)self->_gui_max_us);
        }
    }
}

void AppP25::switchTab(int delta)
{
    if (!_tabview) return;
    /*LS-689*/
    const int N = P25_TAB_COUNT;
    int cur = (int)lv_tabview_get_tab_act(_tabview);
    lv_tabview_set_act(_tabview, (cur + delta + N) % N, LV_ANIM_OFF);
}

void AppP25::freqDownCb(lv_event_t *) { lakeshark_p25_tune(-25000); }
void AppP25::freqUpCb(lv_event_t *)   { lakeshark_p25_tune(+25000); }

/* LS-692: a tap is only coordinate selection.  The callback never touches a
 * radio session; lakeshark_p25_set_freq() places the request in the P25 RX
 * owner's existing tune latch, alongside every other manual tune. */
void AppP25::spectrumTapCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    lv_obj_t *chart = lv_event_get_target(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!self || !chart || !indev) return;

    lv_point_t point;
    lv_area_t area;
    lv_indev_get_point(indev, &point);
    lv_obj_get_coords(chart, &area);
    int width = lv_area_get_width(&area);
    int x = (int)point.x - (int)area.x1;

    float unused;
    p25_spectrum_snapshot_t snapshot;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    uint32_t hz = 0;
    if (!p25_spectrum_read(&unused, 1, now_ms,
                           P25_SPECTRUM_STALE_MS, &snapshot) ||
        !p25_spectrum_tap_hz(x, width, snapshot.center_hz, snapshot.span_hz,
                             P25_CONTROL_TUNER_MIN_HZ,
                             P25_CONTROL_TUNER_MAX_HZ, &hz)) {
        if (self->_s_peak)
            set_text_if_changed(self->_s_peak, "TUNE IGNORED   snapshot stale/out of range");
        return;
    }

    /* A deliberate tap transfers ownership from an active grant first.  The
     * follower queues its old control, then this manual tune replaces that
     * single-slot request; the RX task remains the only radio writer. */
    p25_return_to_control();
    lakeshark_p25_set_freq(hz);
    if (self->_s_peak) {
        char text[80];
        snprintf(text, sizeof(text), "TUNE QUEUED %lu.%06lu MHz",
                 (unsigned long)(hz / 1000000UL),
                 (unsigned long)(hz % 1000000UL));
        set_text_if_changed(self->_s_peak, text);
    }
}

void AppP25::freqEntryCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (self) self->openFreqEntry();
}

void AppP25::openFreqEntry(void)
{
    if (_freq_entry) return;
    char current[24];
    snprintf(current, sizeof(current), "%lu.%06lu",
             (unsigned long)(lakeshark_p25_get_freq() / 1000000UL),
             (unsigned long)(lakeshark_p25_get_freq() % 1000000UL));
    const ls_text_entry_config_t config = {
        .title = "ENTER FREQUENCY (MHz)  -  24 to 1766",
        .text = "",
        .placeholder = current,
        .accepted_chars = "0123456789.",
        .max_length = 11,
        .width = lv_pct(80),
        .mode = LS_TEXT_ENTRY_NUMBER,
        .large = false,
    };
    _freq_entry = ls_text_entry_open(&config, freqEntryDone, this);
}

void AppP25::freqEntryDone(bool accepted, const char *text, void *user_data)
{
    AppP25 *self = static_cast<AppP25 *>(user_data);
    if (!self) return;
    self->_freq_entry = nullptr;
    if (accepted) {
        uint32_t hz = 0;
        if (p25_controls_parse_mhz(text, &hz)) {
            lakeshark_p25_set_freq(hz);
            self->setControlStatus(true, "FREQUENCY SET");
        } else if (self->_set_control_status) {
            lv_label_set_text(self->_set_control_status, "INVALID FREQUENCY");
            lv_obj_set_style_text_color(self->_set_control_status, SDR_ERR, 0);
        }
    }
}

void AppP25::closeFreqEntry(void)
{
    if (_freq_entry) {
        ls_text_entry_t *entry = _freq_entry;
        _freq_entry = nullptr;
        ls_text_entry_close(entry);
    }
}
void AppP25::modeCb(lv_event_t *)     { lakeshark_p25_cycle_mode(); }
void AppP25::resetCb(lv_event_t *)    { lakeshark_p25_reset_stats(); }
void AppP25::gainCb(lv_event_t *)     { lakeshark_p25_gain_step(); }
void AppP25::spectrumGainDownCb(lv_event_t *)
{
    int gain = lakeshark_p25_gain_tenths() - 10;
    lakeshark_radio_set_gain(gain < 0 ? 0 : gain);
}
void AppP25::spectrumGainUpCb(lv_event_t *)
{
    int gain = lakeshark_p25_gain_tenths() + 10;
    lakeshark_radio_set_gain(gain > 496 ? 496 : gain);
}
void AppP25::agcCb(lv_event_t *)      { lakeshark_p25_agc(); }
void AppP25::beepCb(lv_event_t *)     { lakeshark_p25_beep_toggle(); }

/*LS-608*/
void AppP25::defaultsCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    settings_reset_app(app_current());
    lakeshark_radio_park();
    lakeshark_select_p25();
    if (self && self->_reset_val) {
        lv_label_set_text(self->_reset_val, "RESTORED");
        lv_obj_set_style_text_color(self->_reset_val, SDR_OK, 0);
    }
}
