#include "AppP25.hpp"

#include <cstdio>
#include <cstring>

#include "esp_timer.h"

extern "C" {
#include "app_registry.h"
#include "settings.h"
#include "p25_state.h"
#include "lakeshark_backend.h"
#include "sam_tts.h"
#include "audio_events.h"
#include "audio_out.h"
}

#include "sdr_ui/sdr_ui.h"

LV_IMG_DECLARE(img_app_p25);

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
    return sdr_panel(parent);
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud,
                          lv_obj_t **out_lbl = nullptr)
{
    lv_obj_t *b = sdr_btn(parent, txt, cb, ud, out_lbl);
    lv_obj_set_size(b, 104, 50);
    return b;
}

/* Tint a control button a soft muted pastel with dark, readable text. The
 * text color must go on the LABEL child -- sdr_btn gives it an explicit (light)
 * style that overrides text-color inheritance from the button. */
static lv_obj_t *btn_pastel(lv_obj_t *b, uint32_t hex)
{
    if (b) {
        lv_obj_set_style_bg_color(b, lv_color_hex(hex), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_t *lbl = lv_obj_get_child(b, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(0x102018), 0);
    }
    return b;
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

static void p25_seg_vol(void *, int v)         { audio_volume_set(v); }
static void p25_seg_gain_live(void *, int v)   { lakeshark_radio_set_gain_live(v); }
static void p25_seg_gain_commit(void *, int v) { lakeshark_radio_set_gain(v); }
static void p25_seg_gate(void *, int v)        { lakeshark_p25_set_voice_gate(v); }

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

bool AppP25::pause(void)  { lakeshark_radio_park(); return true; }

bool AppP25::resume(void)
{
    lakeshark_select_p25();
    return true;
}

bool AppP25::back(void)
{
    if (_freq_modal) { closeFreqEntry(); return true; }
    return exitToLauncher();
}

bool AppP25::close(void)
{
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
    _tabview = nullptr;
    lakeshark_radio_park();
    return true;
}

bool AppP25::run(lv_obj_t *parent)
{
    lakeshark_select_p25();

    lv_obj_t *scr = parent;
    sdr_style_screen(scr);

    _tabview = lv_tabview_create(scr, LV_DIR_TOP, 44);
    lv_obj_set_size(_tabview, lv_pct(100), lv_pct(100));
    sdr_style_tabview(_tabview);

    buildDecodeTab(lv_tabview_add_tab(_tabview, "DECODE"));
    buildSignalTab(lv_tabview_add_tab(_tabview, "SIGNAL"));
    buildSettingsTab(lv_tabview_add_tab(_tabview, "CONFIG"));

    _timer = lv_timer_create(timerCb, 250, this);
    return true;
}

void AppP25::buildDecodeTab(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *face = sdr_lcd_panel(parent, SDR_PAS_GOLD);

    lv_obj_t *strap = lv_obj_create(face);
    lv_obj_set_size(strap, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(strap, LV_OPA_0, 0);
    lv_obj_set_style_border_width(strap, 0, 0);
    lv_obj_set_style_pad_all(strap, 0, 0);
    lv_obj_set_flex_flow(strap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strap, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(strap, LV_OBJ_FLAG_SCROLLABLE);

    _d_face_mode = sdr_label(strap, &lv_font_montserrat_28, SDR_PAS_GOLD);
    lv_obj_set_style_text_letter_space(_d_face_mode, 2, 0);
    lv_label_set_text(_d_face_mode, "C4FM");

    lv_obj_t *rxbox = lv_obj_create(strap);
    lv_obj_set_size(rxbox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(rxbox, LV_OPA_0, 0);
    lv_obj_set_style_border_width(rxbox, 0, 0);
    lv_obj_set_style_pad_all(rxbox, 0, 0);
    lv_obj_set_style_pad_column(rxbox, 6, 0);
    lv_obj_set_flex_flow(rxbox, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rxbox, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(rxbox, LV_OBJ_FLAG_SCROLLABLE);
    _d_led = lv_led_create(rxbox);
    lv_obj_set_size(_d_led, 14, 14);
    lv_led_set_color(_d_led, SDR_PAS_GREEN);
    lv_led_off(_d_led);
    _d_rx = sdr_label(rxbox, &lv_font_montserrat_22, COL_DIM);
    lv_label_set_text(_d_rx, "RX");

    _d_freq = sdr_label(face, &lv_font_montserrat_48, SDR_PAS_AMBER);
    lv_obj_set_width(_d_freq, lv_pct(100));
    lv_obj_set_style_text_align(_d_freq, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(_d_freq, 2, 0);
    lv_label_set_text(_d_freq, "154.7850");
    lv_obj_add_flag(_d_freq, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_d_freq, freqEntryCb, LV_EVENT_CLICKED, this);

    _d_status = sdr_label(face, &lv_font_montserrat_16, SDR_PAS_CYAN);
    lv_obj_set_width(_d_status, lv_pct(100));
    lv_obj_set_style_text_align(_d_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(_d_status, "Waiting...");

    _d_smeter = p25_meter_label(face);
    _d_bmeter = p25_meter_label(face);

    lv_obj_t *p1 = make_panel(parent);
    _d_decode = make_label(p1, &lv_font_montserrat_16, COL_TEXT);
    lv_obj_set_width(_d_decode, lv_pct(100));

    lv_obj_t *p3 = make_panel(parent);
    _d_radio = make_label(p3, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_width(_d_radio, lv_pct(100));

    _d_gain_slider = sdr_seg_slider(parent, SDR_PAS_AMBER, 496, lakeshark_p25_gain_tenths(),
                                    p25_seg_gain_live, this, &_d_gain_lbl);
    sdr_seg_on_release(_d_gain_slider, p25_seg_gain_commit);
    _d_vol_slider  = sdr_seg_slider(parent, SDR_PAS_GREEN, 100, audio_volume_get(),
                                    p25_seg_vol, this, &_d_vol_lbl);

    lv_obj_t *btns = lv_obj_create(parent);
    lv_obj_set_size(btns, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btns, LV_OPA_0, 0);
    lv_obj_set_style_border_width(btns, 0, 0);
    lv_obj_set_style_pad_all(btns, 2, 0);
    lv_obj_set_style_pad_column(btns, 6, 0);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

    btn_pastel(make_btn(btns, "SET F", freqEntryCb, this), 0x8CB6D9);
    btn_pastel(make_btn(btns, "MODE",  modeCb,      this), 0xE0C27A);
    btn_pastel(make_btn(btns, "AGC",   agcCb,       this), 0xE0C27A);
    btn_pastel(make_btn(btns, "RESET", resetCb,     this), 0xE39B96);
    btn_pastel(make_btn(btns, "BEEP",  beepCb,      this, &_d_beepbtn_lbl), 0xB3A7E0);
}

void AppP25::updateDecode(void)
{
    char buf[256];
    bool synced = P25.dsd_has_sync;
    bool voice  = P25.voice_active_until_us > esp_timer_get_time();
    set_text_if_changed(_d_face_mode, P25.dsd_modulation[0] ? P25.dsd_modulation : "C4FM");

    snprintf(buf, sizeof(buf), "%lu.%04lu",
             (unsigned long)(s_tune_freq_hz / 1000000UL),
             (unsigned long)((s_tune_freq_hz / 100UL) % 10000UL));
    set_text_if_changed(_d_freq, buf);
    lv_obj_set_style_text_color(_d_freq, voice ? SDR_PAS_GREEN : SDR_PAS_AMBER, 0);

    const char *rxs = voice ? "VOX" : synced ? "SYNC" : "RX";
    lv_color_t  rxc = voice ? SDR_PAS_GREEN : synced ? SDR_PAS_CYAN : COL_DIM;
    if (_d_rx) { lv_label_set_text(_d_rx, rxs); lv_obj_set_style_text_color(_d_rx, rxc, 0); }
    if (_d_led) {
        lv_led_set_color(_d_led, voice ? SDR_PAS_GREEN : SDR_PAS_CYAN);
        if (synced) lv_led_on(_d_led); else lv_led_off(_d_led);
    }

    int ok = P25.dsd_bch_ok_count, fail = P25.dsd_bch_fail_count, tot = ok + fail;
    int pct10 = tot > 0 ? (ok * 1000) / tot : 0;
    char nac[16];
    if (P25.dsd_nac)              snprintf(nac, sizeof(nac), "0x%03X", P25.dsd_nac);
    else if (P25.dsd_last_ok_nac) snprintf(nac, sizeof(nac), "0x%03X", P25.dsd_last_ok_nac);
    else                          snprintf(nac, sizeof(nac), "-----");
    snprintf(buf, sizeof(buf),
             "NAC %s    TG %s%d    SRC %s%d\nFRAME %s   DUID %s\nBCH ok %d  fail %d  ratio %d.%d%%   VOICE %d",
             nac,
             P25.dsd_tg ? "" : "", P25.dsd_tg,
             P25.dsd_src ? "" : "", P25.dsd_src,
             P25.dsd_ftype[0] ? P25.dsd_ftype : "----",
             P25.dsd_fsubtype[0] ? P25.dsd_fsubtype : "----",
             ok, fail, pct10 / 10, pct10 % 10, P25.dsd_voice_count);
    set_text_if_changed(_d_decode, buf);

    int iqpct = clampi((int)(P25.iq_level * 100.0f + 0.5f), 0, 100);
    bool clip = P25.iq_level >= 0.97f;
    if (_d_smeter) {
        char bar[96];
        ascii_bar(bar, sizeof(bar), iqpct, sdr_bar_width(_d_smeter, 9));
        if (clip) lv_label_set_text_fmt(_d_smeter, "S %s CLIP", bar);
        else      lv_label_set_text_fmt(_d_smeter, "S %s %3d%%", bar, iqpct);
        lv_obj_set_style_text_color(_d_smeter,
            clip ? COL_RED : iqpct < 5 ? SDR_PAS_ROSE : iqpct < 20 ? SDR_PAS_AMBER : SDR_PAS_GREEN, 0);
    }
    if (clip && _d_rx) { lv_label_set_text(_d_rx, "CLIP"); lv_obj_set_style_text_color(_d_rx, COL_RED, 0); }

    int bufpct = P25.ring_size > 0 ? (P25.ring_fill * 100) / P25.ring_size : 0;
    if (_d_bmeter) {
        char bar[96];
        ascii_bar(bar, sizeof(bar), bufpct, sdr_bar_width(_d_bmeter, 9));
        lv_label_set_text_fmt(_d_bmeter, "B %s %3d%%", bar, bufpct);
        lv_obj_set_style_text_color(_d_bmeter,
            bufpct < 20 ? SDR_PAS_ROSE : bufpct < 60 ? SDR_PAS_AMBER : SDR_PAS_GREEN, 0);
    }

    char gain[24];
    if (P25.rtl_gain_tenths > 0) snprintf(gain, sizeof(gain), "%d.%d dB",
                                          P25.rtl_gain_tenths / 10, P25.rtl_gain_tenths % 10);
    else                         snprintf(gain, sizeof(gain), "AGC");
    snprintf(buf, sizeof(buf),
             "RTL GAIN %s   DEMOD %.0f   BEEP %s\nIQ %lu KB/s   AUDIO %lu sps   READ ERR %d"
             "\nDECODE %.0f ms/LDU (>180=choppy)   DROP %lu",
             gain, (double)P25.demod_gain, P25.sync_beep_enabled ? "on" : "off",
             (unsigned long)(P25.iq_bytes_sec / 1024),
             (unsigned long)P25.audio_samples_sec, P25.read_errors,
             (double)P25.dsd_decode_ms, (unsigned long)P25.audio_drops);
    set_text_if_changed(_d_radio, buf);

    if (_d_beepbtn_lbl)
        lv_label_set_text(_d_beepbtn_lbl, P25.sync_beep_enabled ? "BEEP*" : "BEEP");

    if (_d_gain_lbl) {
        if (P25.rtl_gain_tenths <= 0) lv_label_set_text(_d_gain_lbl, "GAIN  AGC");
        else lv_label_set_text_fmt(_d_gain_lbl, "GAIN  %d.%d dB",
                                   P25.rtl_gain_tenths / 10, P25.rtl_gain_tenths % 10);
    }
    sdr_seg_set(_d_gain_slider, P25.rtl_gain_tenths);
    if (_d_vol_lbl) lv_label_set_text_fmt(_d_vol_lbl, "VOLUME  %d", audio_volume_get());
    sdr_seg_set(_d_vol_slider, audio_volume_get());

    lv_color_t scol = COL_AMBER;
    if (!P25.dsd_buffers_ok) {
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
    lv_label_set_text(_d_status, buf);
    lv_obj_set_style_text_color(_d_status, scol, 0);
}

void AppP25::buildSignalTab(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    _s_hdr = make_label(parent, &lv_font_montserrat_16, COL_CYAN);
    lv_label_set_text(_s_hdr, "DEMOD");

    lv_obj_t *p1 = make_panel(parent);
    _s_iqlbl = make_label(p1, &lv_font_montserrat_14, COL_TEXT);
    lv_label_set_text(_s_iqlbl, "INPUT");
    _s_iqbar = make_label(p1, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_width(_s_iqbar, lv_pct(100));

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
    rateSample();

    char buf[200], nac[16];
    if (P25.dsd_nac)              snprintf(nac, sizeof(nac), "0x%03X", P25.dsd_nac);
    else if (P25.dsd_last_ok_nac) snprintf(nac, sizeof(nac), "0x%03X", P25.dsd_last_ok_nac);
    else                          snprintf(nac, sizeof(nac), "-----");
    snprintf(buf, sizeof(buf), "DEMOD %lu.%03lu MHz    MOD %s    NAC %s",
             (unsigned long)(s_tune_freq_hz / 1000000UL),
             (unsigned long)((s_tune_freq_hz / 1000UL) % 1000UL),
             P25.dsd_modulation[0] ? P25.dsd_modulation : "----", nac);
    set_text_if_changed(_s_hdr, buf);

    char bar[40];
    int iqpct = clampi((int)(P25.iq_level * 100.0f + 0.5f), 0, 100);
    ascii_bar(bar, sizeof(bar), iqpct, 20);
    snprintf(buf, sizeof(buf), "INPUT  %s %3d%%", bar, iqpct);
    set_text_if_changed(_s_iqbar, buf);
    lv_obj_set_style_text_color(_s_iqbar,
        P25.iq_level < 0.05f ? COL_RED : P25.iq_level < 0.20f ? COL_AMBER : COL_GREEN, 0);

    int vmax = 1;
    for (int i = 0; i < RATE_N; i++) { if (_rate_voice[i] > vmax) vmax = _rate_voice[i];
                                       if (_rate_sync[i]  > vmax) vmax = _rate_sync[i]; }
    char sv[RATE_N + 1], ss[RATE_N + 1];
    ascii_spark(sv, sizeof(sv), _rate_voice, _rate_head, RATE_N, vmax);
    ascii_spark(ss, sizeof(ss), _rate_sync,  _rate_head, RATE_N, vmax);
    snprintf(buf, sizeof(buf), "V %s\nS %s", sv, ss);
    set_text_if_changed(_s_chart, buf);

    int ok = P25.dsd_bch_ok_count, fail = P25.dsd_bch_fail_count, tot = ok + fail;
    int pct10 = tot > 0 ? (ok * 1000) / tot : 0;
    snprintf(buf, sizeof(buf),
             "TOTALS\nsync %d    voice %d\nBCH ok %d  fail %d  ratio %d.%d%%",
             P25.dsd_sync_count, P25.dsd_voice_count, ok, fail, pct10 / 10, pct10 % 10);
    lv_label_set_text(_s_totals, buf);

    snprintf(buf, sizeof(buf), "LAST ERR: %s", P25.dsd_err_str[0] ? P25.dsd_err_str : "(none)");
    lv_label_set_text(_s_err, buf);
    lv_obj_set_style_text_color(_s_err, P25.dsd_err_str[0] ? COL_AMBER : COL_DIM, 0);
}

void AppP25::buildSettingsTab(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 8, 0);
    lv_obj_set_style_pad_row(parent, 6, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    sdr_setrow_t r;

    sdr_section(parent, "RADIO");

    sdr_setting_row(parent, "FREQUENCY", &r);
    _set_freq_val = r.value;
    sdr_btn(r.controls, "-1M",  freqM1Cb,  this, nullptr);
    sdr_btn(r.controls, "-25k", freqm25Cb, this, nullptr);
    sdr_btn(r.controls, "+25k", freqp25Cb, this, nullptr);
    sdr_btn(r.controls, "+1M",  freqP1Cb,  this, nullptr);

    sdr_setting_row(parent, "GAIN", &r);
    _set_gain_val = r.value;
    sdr_btn(r.controls, "STEP", gainStepCb, this, nullptr);
    sdr_btn(r.controls, "AGC",  agcCb2,     this, nullptr);

    sdr_setting_row(parent, "DEMOD MODE", &r);
    _set_mode_val = r.value;
    sdr_btn(r.controls, "CYCLE", modeCycleCb, this, nullptr);

    sdr_setting_row(parent, "POLARITY", &r);
    _set_pol_val = r.value;
    sdr_btn(r.controls, "INVERT", polarityCb, this, nullptr);

    sdr_setting_row(parent, "SYNC BEEP", &r);
    _set_beep_val = r.value;
    sdr_btn(r.controls, "TOGGLE", beepCb2, this, nullptr);

    sdr_section(parent, "FAVOURITES");

    sdr_setting_row(parent, "SLOT", &r);
    _set_fav_val = r.value;
    sdr_btn(r.controls, "<", favPrevCb, this, nullptr);
    sdr_btn(r.controls, ">", favNextCb, this, nullptr);

    sdr_setting_row(parent, "ACTION", &r);
    lv_label_set_text(r.value, "");
    sdr_btn(r.controls, "SAVE", favSaveCb, this, nullptr);
    sdr_btn(r.controls, "TUNE", favTuneCb, this, nullptr);
    sdr_btn(r.controls, "CLR",  favClrCb,  this, nullptr);

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

    sdr_section(parent, "AUDIO");

    _set_vol_slider = sdr_seg_slider(parent, SDR_PAS_CYAN, 100, audio_volume_get(),
                                     p25_seg_vol, this, &_set_vol_val);

    sdr_section(parent, "VOICE GATE  (higher = less muting / more static)");
    _set_gate_slider = sdr_seg_slider(parent, SDR_PAS_LAV, 99, lakeshark_p25_voice_gate(),
                                      p25_seg_gate, this, &_set_gate_lbl);

    sdr_setting_row(parent, "MUTE", &r);
    _set_mute_val = r.value;
    sdr_btn(r.controls, "TOGGLE", muteCb, this, nullptr);

    sdr_section(parent, "SYSTEM");

    sdr_setting_row(parent, "USB AUTO-REBOOT", &r);
    _set_reboot_val = r.value;
    sdr_btn(r.controls, "TOGGLE", rebootToggleCb, this, nullptr);

    updateSettings();
}

void AppP25::updateSettings(void)
{
    char b[40];
    if (_set_freq_val) {
        uint32_t f = lakeshark_p25_get_freq();
        snprintf(b, sizeof(b), "%lu.%03lu MHz",
                 (unsigned long)(f / 1000000UL), (unsigned long)((f / 1000UL) % 1000UL));
        lv_label_set_text(_set_freq_val, b);
    }
    if (_set_gain_val) {
        int g = lakeshark_p25_gain_tenths();
        if (g <= 0) lv_label_set_text(_set_gain_val, "AGC");
        else { snprintf(b, sizeof(b), "%d.%d dB", g / 10, g % 10);
               lv_label_set_text(_set_gain_val, b); }
    }
    if (_set_mode_val) lv_label_set_text(_set_mode_val, lakeshark_p25_mode_name());
    if (_set_pol_val)  lv_label_set_text(_set_pol_val,
                                         lakeshark_p25_polarity_inverted() ? "INVERTED" : "NORMAL");
    if (_set_beep_val) lv_label_set_text(_set_beep_val,
                                         lakeshark_p25_beep_enabled() ? "ON" : "OFF");
    if (_set_fav_val) {
        const app_t *a = app_current();
        uint32_t f = a ? settings_fav_get(a, _fav_slot) : 0;
        if (f) snprintf(b, sizeof(b), "%d/%d: %lu.%03lu", _fav_slot + 1, MAX_FAVOURITES,
                        (unsigned long)(f / 1000000UL), (unsigned long)((f / 1000UL) % 1000UL));
        else   snprintf(b, sizeof(b), "%d/%d: (empty)", _fav_slot + 1, MAX_FAVOURITES);
        lv_label_set_text(_set_fav_val, b);
    }
    if (_set_preset_val)
        lv_label_set_text(_set_preset_val,
            sam_tts_preset_name((sam_tts_voice_preset_t)settings_voice_preset_get()));
    if (_set_lp_val)
        lv_label_set_text(_set_lp_val, sam_tts_lowpass_name(settings_voice_lowpass_get()));
    if (_set_shelf_val)
        lv_label_set_text(_set_shelf_val, sam_tts_lowshelf_name(settings_voice_lowshelf_get()));
    if (_set_vol_val) {
        snprintf(b, sizeof(b), "VOLUME  %d", audio_volume_get());
        lv_label_set_text(_set_vol_val, b);
    }
    sdr_seg_set(_set_vol_slider, audio_volume_get());
    if (_set_gate_lbl) lv_label_set_text_fmt(_set_gate_lbl, "VOICE GATE  %d", lakeshark_p25_voice_gate());
    sdr_seg_set(_set_gate_slider, lakeshark_p25_voice_gate());
    if (_set_mute_val)
        lv_label_set_text(_set_mute_val, audio_is_muted() ? "MUTED" : "ON");
    if (_set_reboot_val)
        lv_label_set_text(_set_reboot_val, app_usb_autoreboot() ? "ON" : "OFF");
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

    self->rateSample();
    switch (lv_tabview_get_tab_act(self->_tabview)) {
        case 0:  self->updateDecode();   break;
        case 1:  self->updateSignal();   break;
        case 2:  self->updateSettings(); break;
        default: break;
    }
}

void AppP25::switchTab(int delta)
{
    if (!_tabview) return;
    const int N = 3;
    int cur = (int)lv_tabview_get_tab_act(_tabview);
    lv_tabview_set_act(_tabview, (cur + delta + N) % N, LV_ANIM_OFF);
}

void AppP25::freqDownCb(lv_event_t *) { lakeshark_p25_tune(-25000); }
void AppP25::freqUpCb(lv_event_t *)   { lakeshark_p25_tune(+25000); }

void AppP25::freqEntryCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (self) self->openFreqEntry();
}

void AppP25::openFreqEntry(void)
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
    lv_label_set_text(title, "ENTER FREQUENCY (MHz)  -  e.g. 154.785");

    lv_obj_t *ta = lv_textarea_create(bg);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_accepted_chars(ta, "0123456789.");
    lv_textarea_set_max_length(ta, 10);
    lv_textarea_set_placeholder_text(ta, "154.785");
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

void AppP25::freqKbCb(lv_event_t *e)
{
    AppP25 *self = static_cast<AppP25 *>(lv_event_get_user_data(e));
    if (!self) return;
    if (lv_event_get_code(e) == LV_EVENT_READY && self->_freq_ta) {
        double mhz = atof(lv_textarea_get_text(self->_freq_ta));
        if (mhz >= 1.0 && mhz <= 2000.0)
            lakeshark_p25_set_freq((uint32_t)(mhz * 1e6 + 0.5));
    }
    self->closeFreqEntry();
}

void AppP25::closeFreqEntry(void)
{
    if (_freq_modal) { lv_obj_del(_freq_modal); _freq_modal = nullptr; _freq_ta = nullptr; }
}
void AppP25::modeCb(lv_event_t *)     { lakeshark_p25_cycle_mode(); }
void AppP25::resetCb(lv_event_t *)    { lakeshark_p25_reset_stats(); }
void AppP25::gainCb(lv_event_t *)     { lakeshark_p25_gain_step(); }
void AppP25::agcCb(lv_event_t *)      { lakeshark_p25_agc(); }
void AppP25::beepCb(lv_event_t *)     { lakeshark_p25_beep_toggle(); }
