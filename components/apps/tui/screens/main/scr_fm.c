#include "fm_lr_dsp.h"
#include "ls_lora.h"
#include "../../ls_calls.h"
#include "ls_textpack.h"
/* FM screen: VFO on one page, decoded pages on another.

   `FM` is a plain global like `P25`, and `FM.scan_db[256]` is a bare
   float array the sweep writes into - so the spectrum needs no widget, no
   canvas and no allocation, only a loop that maps bins to columns. */
#include "../../ls_tui_screen.h"
#include "../../ls_i18n.h"
#include "../../ls_radio_panel.h"
#include "../../ls_numpad.h"
#include "../../ls_box.h"
#include "../../ls_options.h"
#include "../../ls_rffe_ui.h"
#include "scan_engine.h"
#include "settings.h"
#include "scan_channels.h"
#include "../../ls_motion.h"
#include "../../ls_text.h"
#include "fm_same_view.h"
#include "fm_aprs_view.h"
#include "fm_ais_view.h"
#include "fm_rds_view.h"
#include "fm_air_view.h"
#include "fm_sstv_view.h"
#include "apps/fm/fm_bcast.h"

#include <stdio.h>
#include <string.h>

#include "apps/fm/fm_state.h"
#include "apps/fm/fm_dsp.h"
#include "apps/acars/acars_app.h"
#include "apps/fm/aprs_store.h"
#include "apps/fm/fm_mode_label.h"
#include "apps/fm/fm_hf.h"
#include "apps/fm/fm_spectrum.h"
#include "radio/ls_radio_hf.h"
#include "hf_demod.h"
#include "../../ls_braille.h"
#include "audio/audio_out.h"
#include "lakeshark_backend.h"

#include "../../ls_quick.h"
#include "../../ls_picker.h"
#include "../../ls_radio_select.h"
#include "../../ls_field.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "../../ls_tui_ui.h"
#include "../../ls_waterfall.h"
#include "../../ls_glyph.h"
#include "../../ls_wf_source.h"
#include "../../rx_panels/rx_pages.h"
#include "../../rx_panels/rx_tape.h"

static bool s_details;
static char s_hint[96] = "LEFT/RIGHT tune  UP/DOWN controls  M details";
static ls_radio_panel_t s_radio = { .focus = -1 };
static ls_radio_view_t s_view;
static uint32_t s_standby = 152600000;
static int s_page;          /* 0 vfo, 1 pages, 2 sweep */

/* The same free-running blink MESH and REC already use (scr_mesh.c
   s_blink, scr_rec.c s_blink): incremented once a draw, read back as
   `(s_blink / 12) & 1`. CARRIER read "OPEN" in plain green the instant
   squelch broke and sat there in exactly the same green for as long as it
   stayed open - a burst and a held carrier looked identical at a glance,
   which is the one distinction a squelch light exists to make. */
static uint32_t s_blink;

/* What a thumb can reach without leaving the screen. */

static const fm_mode_t FM_MODES[] = {
    FM_MODE_LISTEN, FM_MODE_WFM, FM_MODE_AM, FM_MODE_POCSAG,
    FM_MODE_FLEX, FM_MODE_ACARS, FM_MODE_SAME, FM_MODE_APRS, FM_MODE_AIS,
};
#define N_MODES ((int)(sizeof(FM_MODES) / sizeof(FM_MODES[0])))
static int s_last_mode = -1;
/* Set when show_page asked the receiver to change mode. Those requests are
   asynchronous - ls_wf_fm_sweep and choose_mode only queue a handoff - so
   FM.mode still holds the old value when show_page returns, and the change
   lands a frame or two later. Without this the draw below read that late
   change as "the mode moved on its own" and re-derived the page from it:
   leaving SPECTRUM for VFO restored POCSAG, which maps to the PAGER page, so
   the tap appeared to select the wrong button. Only a change nobody asked
   for should move the page. */
static bool s_mode_requested;

/* POCSAG ON THE LORA CHIP.

   When RADIO puts the pager on the LoRa socket's chip, POCSAG runs there:
   the LoRa Labs paging engine (ls_field, LS_LAB_POCSAG) at this receiver's
   frequency and baud, the SDR handed back, and the PAGER tab listing what
   the chip decodes. FLEX and the other modes stay on the SDR. */
static bool s_lora_pager;       /* POCSAG is running on the LoRa chip     */
static bool s_pager_leaving;    /* a mode away from POCSAG was asked for */
static uint32_t s_lora_hz;
static int s_lora_baud;
static ls_lab_mode_t s_lora_was;
/* The chip matches one sync word, so it hears one polarity at a time where
   the SDR's decoder takes both. Until frames arrive it changes polarity
   every LORA_FLIP_US, and holds the one that syncs. */
#define LORA_FLIP_US  5000000
#define LORA_HOLD_US 30000000
static bool s_lora_inv;
static int s_lora_search_baud = 1200;
/* What OPTIONS asked for: 0 both in turn, 1 normal, 2 inverted. */
static int s_lora_pol;
static int64_t s_lora_flip_at;
static uint32_t s_lora_rx;
EXT_RAM_BSS_ATTR static ls_field_state_t s_field;

static ls_rsel_job_t fm_job(void)
{
    if (FM.mode == FM_MODE_POCSAG || FM.mode == FM_MODE_FLEX) return LS_RSEL_PAGER;
    if (FM.mode == FM_MODE_ACARS) return LS_RSEL_ACARS;
    return LS_RSEL_FM;
}

/* The radio really in use: the chip while it pages, otherwise the dongle
   the receiver holds. */
static ls_rsel_radio_t fm_in_use(void)
{
    return (s_lora_pager || FM.lr_iq_active) ? LS_RSEL_LORA : ls_rsel_sdr_held_by("fm");
}

static void lora_pager_stop(bool give_back)
{
    if (!s_lora_pager) return;
    s_lora_pager = false;
    ls_field_direct(false);
    ls_field_mode(s_lora_was);
    if (give_back) ls_tui_radio_want("FM");
}

static void lora_pager_sync(void)
{
    if (s_pager_leaving && FM.mode != FM_MODE_POCSAG) s_pager_leaving = false;
    /* The LR2021 pages through its I/Q stream (app_fm.c); this FSK route is
       for the SX1262. */
    const bool want = FM.mode == FM_MODE_POCSAG && !s_pager_leaving &&
                      ls_lora_chip() != LS_LORA_CHIP_LR20XX &&
                      ls_rsel_resolved(LS_RSEL_PAGER) == LS_RSEL_LORA &&
                      ls_lora_rx_range_ok(ls_lora_caps(), FM.freq_hz, FM.freq_hz);
    if (want && !s_lora_pager) {
        if (!ls_field_start()) return;
        ls_field_snapshot(&s_field);
        s_lora_was = s_field.mode;
        s_lora_pager = true;
        s_lora_hz = 0;
        s_lora_baud = 0;
        s_lora_inv = false;
        s_lora_search_baud = 1200;
        s_lora_rx = s_field.pages;
        s_lora_flip_at = esp_timer_get_time() + LORA_FLIP_US;
        ls_field_mode(LS_LAB_POCSAG);
        ls_field_direct(true);
        ls_tui_radio_want(NULL);            /* the SDR goes back */
    } else if (!want && s_lora_pager) {
        lora_pager_stop(true);
    }
    if (!s_lora_pager) return;
    ls_field_snapshot(&s_field);
    /* AUTO visits both rates in each allowed polarity. */
    const int64_t now = esp_timer_get_time();
    if (s_field.pages > s_lora_rx) {
        s_lora_rx = s_field.pages;
        s_lora_flip_at = now + LORA_HOLD_US;
    } else if ((!s_lora_pol || FM.pocsag_auto) && now >= s_lora_flip_at) {
        if (FM.pocsag_auto) {
            s_lora_search_baud = s_lora_search_baud == 1200 ? 2400 : 1200;
            if (!s_lora_pol && s_lora_search_baud == 1200) s_lora_inv = !s_lora_inv;
        } else s_lora_inv = !s_lora_inv;
        s_lora_flip_at = now + LORA_FLIP_US;
        s_lora_hz = 0;
    }
    s_lora_rx = s_field.pages;
    /* A polarity chosen in OPTIONS is held, and taking it is a retune. */
    if (s_lora_pol && s_lora_inv != (s_lora_pol == 2)) {
        s_lora_inv = s_lora_pol == 2;
        s_lora_hz = 0;
    }
    const int asked = FM.pocsag_auto ? s_lora_search_baud : FM.pocsag_baud;
    const int baud = asked == 2400 ? 2400 : 1200;
    if (FM.freq_hz == s_lora_hz && baud == s_lora_baud) return;
    ls_lora_cfg_t cfg = s_field.config;
    cfg.freq_hz = FM.freq_hz;
    cfg.invert_iq = s_lora_inv;
    cfg.cal_min_mhz = (uint16_t)(FM.freq_hz / 4000000u * 4u);
    cfg.cal_max_mhz = cfg.cal_min_mhz + 4;
    ls_fsk_cfg_t fsk = s_field.fsk;
    fsk.bitrate = (uint32_t)baud;
    if (ls_field_configure(&cfg) && ls_field_configure_fsk(&fsk)) {
        s_lora_hz = FM.freq_hz;
        s_lora_baud = baud;
    }
}

/* A different dongle than the one held: the receiver starts again on it.
   The chip needs nothing here; the next frame's sync moves the pager. */
static void radio_chosen(ls_rsel_radio_t radio)
{
    const ls_rsel_radio_t held = ls_rsel_sdr_held_by("fm");
    if ((radio == LS_RSEL_SDR_RTL || radio == LS_RSEL_SDR_HACKRF) &&
        held != LS_RSEL_NONE && held != radio) ls_rsel_restart_sdr();
}

static void open_radio(void) { ls_rsel_fm_context(FM.freq_hz, fm_lr_mode(FM.mode)); ls_rsel_open(fm_job(), radio_chosen); }


/* SHORTWAVE. HF is AM mode handing its audio to the HF demodulators
   (fm_hf.h): below 30 MHz, or any SSB/CW/SAM. The screen keeps every shared
   control and changes what the glass says: the sideband on the rail, the
   demod line, and a passband scope where the activity trace sits. */
static bool hf_on(void)
{
    return FM.freq_hz >= LS_HF_MIN_HZ &&
           fm_hf_chain(FM.mode == FM_MODE_AM, FM.freq_hz, fm_hf_get()->demod);
}

/* What the MODE key and the rail call the receiver now. */
static const char *mode_word(void)
{
    static char word[12];
    if (scan_engine_seek_direction() < 0) return "SEEK <";
    if (scan_engine_seek_direction() > 0) return "SEEK >";
    if (FM.mode == FM_MODE_SCAN) return "SWEEP";
    if (hf_on()) {
        snprintf(word, sizeof(word), "HF %s", fm_hf_demod_label(fm_hf_get()->demod));
        return word;
    }
    return fm_mode_label(FM.mode);
}

/* The band the dial is in, for the BAND key; "HF" between bands. */
static const char *hf_band_word(void)
{
    const int b = fm_hf_band_find(FM.freq_hz);
    return b >= 0 ? fm_hf_band(b)->name : "HF";
}

static void hf_band_picked(int index)
{
    if (s_lora_pager) { s_pager_leaving = true; lora_pager_stop(true); }
    lakeshark_fm_frequency_lock(false);
    if (!fm_hf_band_apply(index)) return;
    s_page = 0;
    ls_wf_source_release();
}

/* Every band, named with the demod and dial it lands on: one tap to a
   station from anywhere in FM. */
static void open_hf_band_picker(void)
{
    ls_picker_open("SHORTWAVE BAND", hf_band_picked);
    const int cur = hf_on() ? fm_hf_band_find(FM.freq_hz) : -1;
    for (int i = 0; i < fm_hf_band_count(); ++i) {
        const fm_hf_band_t *b = fm_hf_band(i);
        char detail[24];
        snprintf(detail, sizeof(detail), "%s%s %.3f", i == cur ? "* " : "",
                 fm_hf_demod_label(b->demod), b->dial_hz / 1e6);
        ls_picker_add(b->name, detail);
    }
}

static int mode_page(fm_mode_t mode)
{
    if (mode == FM_MODE_SCAN) return 2;
    if (mode == FM_MODE_POCSAG || mode == FM_MODE_FLEX ||
        mode == FM_MODE_SAME || mode == FM_MODE_APRS || mode == FM_MODE_AIS || mode == FM_MODE_ACARS) return 1;
    return 0;
}

static bool choose_mode(fm_mode_t mode)
{
    /* NFM is an FM demodulator.  A previously enabled mixed channel scan
       must not silently pull P25 into this screen. */
    if (mode == FM_MODE_LISTEN) scan_engine_set_mixed(false);
    /* Off the chip first, so the receiver is running to take the mode. */
    if (mode != FM_MODE_POCSAG && s_lora_pager) {
        s_pager_leaving = true;
        lora_pager_stop(true);
    }
    ls_args_t args = {0};
    args.n = 1;
    args.v[0].kind = LS_VAL_TEXT;
    args.v[0].s = fm_mode_command_name(mode);
    ls_val_t result;
    if (ls_action_call("fm.submode", &args, &result,
                       ls_quick_grant_builtin()) != LS_ACT_OK) return false;
    s_last_mode = (int)FM.mode;
    s_page = mode_page(mode);
    if (mode != FM_MODE_SCAN) ls_wf_source_release();
    return true;
}

static void mode_picked(int index)
{
    if (index >= 0 && index < N_MODES) choose_mode(FM_MODES[index]);
    else if (index == N_MODES) open_hf_band_picker();
}

static void open_mode_picker(void)
{
    ls_picker_open(FM.mode==FM_MODE_SCAN ? "STOP SWEEP / MODE" : "RECEIVER MODE", mode_picked);
    for (int i = 0; i < N_MODES; ++i)
        ls_picker_add(fm_mode_label(FM_MODES[i]),
                      FM.mode == FM_MODES[i] && !hf_on() ? "selected"
                      : FM.mode==FM_MODE_SCAN ? "stops sweep" : "select mode");
    /* Last, so every mode keeps its row: HF opens straight onto the bands. */
    ls_picker_add("SHORTWAVE", hf_on() ? "selected - pick a band" : "HF bands: AM SSB CW");
}

static const ls_quick_t QUICK[] = {
    /* See scr_p25.c: typing a frequency is the common case. */
    { .label = "TUNE", .kind = LS_QUICK_ACTION, .action = "fm.tune",
      .key = 't' },
    { .label = "VOLUME", .kind = LS_QUICK_STEP, .action = "audio.volume",
      .value = "sys.volume", .delta = 5, .lo = 0, .hi = 100,
      .key = '+', .key_down = '-' },
    { .label = "GAIN", .kind = LS_QUICK_STEP, .action = "fm.gain",
      .value = "fm.gain", .delta = 2.0f, .lo = 0, .hi = 50,
      .key = 'u', .key_down = 'j' },
    /* Not 'w': RUN SWEEP claims it in key() and returns before the quick bar
       is consulted, so squelch down never ran and the sweep toggled instead. */
    /* A letter opens exact entry, a pair nudges. 'q' is the SQUELCH button
       on the control row, so the nudge pair is 's' and 'a'. Not 'w': RUN
       SWEEP had it and key() returns before the quick bar is consulted. */
    { .label = "SQUELCH", .kind = LS_QUICK_STEP, .action = "fm.sql",
      .value = "fm.sql", .delta = 1.0f, .lo = 0, .hi = 100,
      .key = 's', .key_down = 'a' },
};
#define N_QUICK ((int)(sizeof(QUICK) / sizeof(QUICK[0])))

/* Where the panel was drawn, so the hit test asks the same geometry. */

static uint32_t tune_step_hz(void)
{
    if (hf_on()) return fm_hf_step_at(FM.freq_hz);
    uint32_t chosen = settings_get_fm_step();
    if (chosen) return chosen;
    if (FM.mode == FM_MODE_WFM) return 100000;
    if (FM.mode == FM_MODE_AM) {
        if (FM.freq_hz >= 26965000 && FM.freq_hz <= 27405000) return 10000;
        if (FM.freq_hz >= 29000000 && FM.freq_hz <= 29200000) return 5000;
        return 25000;
    }
    if (FM.mode == FM_MODE_ACARS) return 25000;
    return 12500;
}

static void tune_step_label(char *out, size_t n)
{
    ls_radio_tune_step_label(tune_step_hz(), out, n);
}

static bool tune_step(int direction)
{
    scan_engine_stop();
    int64_t hz = (int64_t)FM.freq_hz + direction * (int64_t)tune_step_hz();
    const int64_t floor_hz = FM.mode == FM_MODE_AM ? LS_HF_MIN_HZ : LS_RTL_TUNER_MIN_HZ;
    if (hz < floor_hz) hz = floor_hz;
    if (hz > 1766000000) hz = 1766000000;
    ls_args_t args = {0};
    args.n = 1;
    args.v[0].kind = LS_VAL_INT;
    args.v[0].i = (int32_t)hz;
    ls_val_t result;
    /* Release, tune, then hold the new carrier. See tuned_fm in
       ls_action_builtin.c: locking first captured the frequency being left,
       which every other path then refused to move away from. */
    lakeshark_fm_frequency_lock(false);
    const bool ok = ls_action_call("fm.freq_hz", &args, &result,
                                   ls_quick_grant_builtin()) == LS_ACT_OK;
    if (ok) lakeshark_fm_frequency_lock(true);
    return ok;
}

static void cycle_tune_step(void)
{
    /* HF has its own raster, down to 10 Hz for SSB and CW. */
    if (hf_on()) fm_hf_set_step(fm_hf_step_next(tune_step_hz()));
    else settings_set_fm_step(ls_radio_tune_step_next(tune_step_hz()));
}

static void seek_tune(int direction)
{
    if (scan_engine_seek_direction()) { scan_engine_stop(); return; }
    /* Seek uses normal LISTEN and its qualified squelch, as the scanner does. */
    uint32_t lo = FM.lr_iq_active ? 104000000u
                : FM.mode == FM_MODE_AM ? LS_HF_MIN_HZ : LS_RTL_TUNER_MIN_HZ;
    uint32_t hi = 1766000000u;
    if (FM.scan_stop_hz > FM.scan_start_hz && FM.freq_hz >= FM.scan_start_hz &&
        FM.freq_hz <= FM.scan_stop_hz) {
        if (FM.scan_start_hz > lo) lo = FM.scan_start_hz;
        if (FM.scan_stop_hz < hi) hi = FM.scan_stop_hz;
    }
    (void)scan_engine_seek(FM.freq_hz, tune_step_hz(), direction, lo, hi);
}

/* RECEIVER and SIGNAL used to be two boxes, eighteen rows in portrait, and
   carried squelch, volume and gain a second time on top of the buttons that
   set them. They are one panel now, the way P25's DECODE / SIGNAL is: the
   readouts a thumb watches, in label/value pairs, with the level bar inline.
   What is set somewhere shows its value where it is set and nowhere else. */
#define STATUS_ROWS      8      /* portrait: frequency, readouts, bar, gate */
/* The VFO page measures its own glass; see lcd_split below. */
#define STATUS_ROWS_WIDE 5      /* landscape: one line, the bar, the gate   */

static int kv(tui_surface *sf, tui_rect box, int x, int y, const char *k,
              const char *v, uint8_t vattr)
{
    k = ls_tr(k);
    v = ls_tr(v);
    tui_put_str(sf, box, x, y, k, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    x += (int)strlen(k) + 1;
    tui_put_str(sf, box, x, y, v, vattr);
    return x + (int)strlen(v) + 3;
}

/* WHAT EACH MODE CAN SAY ABOUT THE SIGNAL AND THE SQUELCH.

   NFM measures quieting and gates on it, in percent. AM measures carrier
   level and gates on that, in tenths of a percent. WFM has no squelch and the
   decoders never write either reading, so FM.noise and FM.squelch_open there
   are whatever NFM last left behind: those modes show "--" instead. */
typedef enum { SIG_NONE, SIG_QUIETING, SIG_CARRIER } sig_kind_t;

static const char *receiver_text(void)
{
    ls_iq_control_status_t status;
    fm_get_receiver_status(&status);
    if (status.tune_state == LS_IQ_RESULT_FAILED) return "TUNE FAILED";
    if (status.tune_state == LS_IQ_RESULT_PENDING) return "WAITING";
    if (status.receiver_streaming) return NULL;
    if (status.receiver_error == LS_RADIO_ERR_TIMEOUT) return "STALLED";
    if (status.receiver_error == LS_RADIO_OK || status.receiver_error == LS_RADIO_ERR_BUSY)
        return "WAITING";
    if (status.receiver_error == LS_RADIO_ERR_DISCONNECTED ||
        status.receiver_error == LS_RADIO_ERR_UNAVAILABLE) return "OFFLINE";
    return "FAILED";
}

static sig_kind_t sig_kind(void)
{
    if (!s_lora_pager && receiver_text()) return SIG_NONE;
    if (FM.mode == FM_MODE_LISTEN) return SIG_QUIETING;
    if (FM.mode == FM_MODE_AM)     return SIG_CARRIER;
    return SIG_NONE;
}

/* How far along the meter the signal is, 0 to 1. AM's meter spans the same
   0 to 10.0% the squelch is set on, so the gate lands where it was set. */
static float sig_level(sig_kind_t kind)
{
    float s = kind == SIG_CARRIER ? FM.iq_level * 10.0f : 1.0f - FM.noise;
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    return s;
}

/* HF's squelch opens at a level on the dB meter: say the level. */
static bool hf_squelch_text(char *out, size_t n)
{
    if (!hf_on()) return false;
    if (lakeshark_fm_squelch_get() <= 0) snprintf(out, n, "OPEN");
    else snprintf(out, n, "%.0fdB", (double)fm_hf_squelch_dbfs(lakeshark_fm_squelch_get()));
    return true;
}

static void squelch_text(char *out, size_t n)
{
    if (hf_squelch_text(out, n)) return;
    switch (sig_kind()) {
    case SIG_QUIETING: snprintf(out, n, "%d%%", lakeshark_fm_squelch_get()); break;
    case SIG_CARRIER:  snprintf(out, n, "%d.%d%%", lakeshark_fm_squelch_get() / 10,
                                lakeshark_fm_squelch_get() % 10); break;
    default:           snprintf(out, n, "--"); break;
    }
}

static const char *squelch_hint(void)
{
    if (hf_on()) return "0 open; 1-100 is -110 to -20 dBFS";
    return FM.mode == FM_MODE_AM ? "carrier level, tenths of a percent"
                                 : "0 opens on anything, 100 on nothing";
}

/* WHAT THE PAGER DECODER THAT IS RUNNING HAS COUNTED.

   POCSAG on an SDR fills FM.pocsag_*, FLEX fills FM.flex_*, and POCSAG on the
   chip is counted in the ls_field snapshot, which has packets heard and pages
   but no lock and no codeword errors. Every other mode runs no pager decoder,
   and then there is nothing to show. */
typedef rx_pager_counts_t pager_counts_t;

static pager_counts_t pager_counts(void)
{
    pager_counts_t c = {0};
    if (FM.mode == FM_MODE_POCSAG && s_lora_pager) {
        c.on = c.chip = true;
        c.baud = s_lora_baud;
        c.pages = s_field.pages;
        c.heard = s_field.rx;
    } else if (FM.mode == FM_MODE_POCSAG) {
        c.on = c.tape = true;
        c.sync = FM.pocsag_sync;
        c.baud = FM.pocsag_lock_baud ? FM.pocsag_lock_baud : FM.pocsag_baud;
        c.pages = FM.pocsag_pages;
        c.errs = FM.pocsag_cw_errs;
        c.frames = FM.pocsag_frames;
        c.addr = FM.pocsag_addr;
        c.msg = FM.pocsag_msg;
    } else if (FM.mode == FM_MODE_FLEX) {
        c.on = true;
        c.sync = FM.flex_sync;
        c.pages = FM.flex_pages;
        c.errs = FM.flex_cw_errs;
    }
    return c;
}

EXT_RAM_BSS_ATTR static ls_radio_glass_t s_glass;
static sig_kind_t s_sig_kind;    /* what that peak was measured on    */
/* The dial spins and the mode wipes. Both are frame-counted off s_blink, and
   both are covering something the radio is really doing: a retune has a
   settle time before the squelch is allowed to open, and a mode change tears
   the receiver down and builds it again. The motion is the length of the
   wait, so it reads as the radio working rather than as decoration. */
/* The level meter and the squelch gate under it, one row each.

   QUIETING, NOT LEVEL. This bar used to read FM.iq_level, the carrier level
   across the whole IQ block, and it barely moved: measured on hardware, dead
   air 3% and the strongest local broadcast 5%. An FM receiver measures signal
   by how much a carrier quietens the discriminator, so the bar reads the
   inverse of the noise the squelch is already measuring. Same bench:
   broadcast 0.46 of full hiss, dead air 0.81 to 0.97, which is most of the
   bar's travel. That also puts the squelch on this scale, so the gate is a
   mark under the bar and can be set by eye. */
static void draw_level(tui_surface *sf, tui_rect box, int x0, int y, int w,
                       sig_kind_t kind, bool open, bool gate_row)
{
    const uint8_t lab = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    tui_put_str(sf, box, x0, y, ls_tr("SIG"), lab);
    const int bx = x0 + 4;
    const int bw = w - 9;
    if (bw < 4) return;
    if (kind == SIG_NONE) {
        /* No reading in this mode, so no bar and no gate to mark on it. */
        tui_put_str(sf, box, bx, y, ls_tr("--"), LS_ATTR_DIM);
        return;
    }
    const float sig = sig_level(kind);
    /* Peak hold, decaying, so a burst between two looks still registers. */
    if (sig >= s_glass.hold) s_glass.hold = sig;
    else                   s_glass.hold -= (s_glass.hold - sig) * 0.06f;

    const int lit  = (int)(sig * (float)bw);
    const int hold = (int)(s_glass.hold * (float)bw);
    for (int i = 0; i < bw; i++) {
        const bool on = i < lit;
        uint8_t c = on ? (i > bw * 3 / 4 ? TUI_RED | TUI_BRIGHT
                        : i > bw / 2     ? TUI_YELLOW | TUI_BRIGHT
                                         : TUI_GREEN | TUI_BRIGHT)
                       : (TUI_BLACK | TUI_BRIGHT);
        char g = on ? '|' : '.';
        if (!on && i == hold && hold > lit) {
            g = '^';
            c = TUI_WHITE | TUI_BRIGHT;
        }
        tui_put_char(sf, box, bx + i, y, g, TUI_ATTR(c, TUI_BLACK));
    }
    char pct[6];
    snprintf(pct, sizeof(pct), "%3d%%", (int)(sig * 100.0f + 0.5f));
    tui_put_str(sf, box, bx + bw + 1, y, pct,
                TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    if (!gate_row) return;

    /* Where the squelch opens, on the same travel as the bar. Above it the
       squelch is open, below it the audio is muted. */
    /* On the bar's quieting scale (1 - noise), the gate's own curve: 0 and the
       low settings sit at the far left, open on anything. AM gates on level. */
    const float open_below = FM.mode == FM_MODE_AM ? (float)(100 - lakeshark_fm_squelch_get()) * 0.01f
                                                   : fm_nfm_squelch_gate(lakeshark_fm_squelch_get());
    int gate = (int)((1.0f - open_below) * (float)bw);
    if (gate < 0) gate = 0;
    if (gate >= bw) gate = bw - 1;
    tui_put_char(sf, box, bx + gate, y + 1, '^',
                 TUI_ATTR(open ? TUI_GREEN | TUI_BRIGHT : TUI_CYAN | TUI_BRIGHT,
                          TUI_BLACK));
    tui_put_str(sf, box, x0, y + 1, ls_tr("SQL"),
                TUI_ATTR(TUI_BLACK | TUI_BRIGHT, TUI_BLACK));
}

/* The frequency, spinning over a retune. A retune is not instant, so neither
   is the readout: the dial runs from where it was to where it is going over
   the settle, and the digits still moving are the ones drawn bright. */
static int draw_dial(tui_surface *sf, tui_rect box, int x, int y, bool centre)
{
    const uint32_t hz = FM.freq_hz;
    const uint8_t val = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t moving = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    char buf[24], settled[24];
    const uint32_t shown = ls_radio_glass_tick(&s_glass, FM.freq_hz);
    snprintf(buf, sizeof(buf), "%u.%04u MHz", (unsigned)(shown / 1000000u),
             (unsigned)((shown % 1000000u) / 100u));
    snprintf(settled, sizeof(settled), "%u.%04u MHz",
             (unsigned)(hz / 1000000u), (unsigned)((hz % 1000000u) / 100u));
    const int n = (int)strlen(buf);
    if (centre) x = box.x + (box.w - n) / 2;
    for (int i = 0; buf[i]; i++) {
        const bool differs = settled[i] && buf[i] != settled[i];
        tui_put_char(sf, box, x + i, y, buf[i], differs ? moving : val);
    }
    return x + n + 3;
}

/* What the decoder or the tone gate adds, after the carrier. */
static int draw_extra(tui_surface *sf, tui_rect box, int x, int y, bool step_too)
{
    const uint8_t good = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim  = LS_ATTR_DIM;
    char buf[24];
    const pager_counts_t pc = pager_counts();
    if (pc.on) {
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)pc.pages);
        x = kv(sf, box, x, y, "PAGES", buf, pc.pages ? good : dim);
        if (pc.chip) {
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)pc.heard);
            x = kv(sf, box, x, y, "HEARD", buf, pc.heard ? good : dim);
        } else {
            x = kv(sf, box, x, y, "SYNC", pc.sync ? "LOCKED" : "hunting",
                   pc.sync ? good : dim);
        }
    } else if (FM.mode == FM_MODE_WFM) {
        /* The station's name and type, as RDS gives them. */
        char station[40];
        fm_rds_glass_line(station, sizeof(station));
        if (station[0]) x = kv(sf, box, x, y, "RDS", station, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    } else if (FM.mode == FM_MODE_LISTEN && FM.tone_show) {
        const bool lock = FM.tone_detected.selection != 0;
        fm_tone_label(FM.tone_detected.selection, buf, sizeof(buf));
        tui_put_char(sf, box, x, y, ls_motion_pip(lock), good);
        x = kv(sf, box, x + 2, y, lock ? "TONE LOCK" : "TONE", lock ? buf : "--",
               lock ? good : dim);
    }
    /* The step [ and ] move by: nowhere else says it. */
    if (step_too && FM.mode != FM_MODE_SCAN && x + 12 <= box.x + box.w - 2) {
        const uint32_t step = tune_step_hz();
        if (step < 1000u) snprintf(buf, sizeof(buf), "%uHz", (unsigned)step);
        else if (step % 1000u) snprintf(buf, sizeof(buf), "%u.%uk", (unsigned)(step / 1000u),
                                   (unsigned)(step % 1000u / 100u));
        else snprintf(buf, sizeof(buf), "%uk", (unsigned)(step / 1000u));
        x = kv(sf, box, x, y, "STEP", buf, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    }
    return x;
}

static void draw_vfo(tui_surface *sf, tui_rect r)
{
    const char *receiver = s_lora_pager ? NULL : receiver_text();
    const sig_kind_t kind = sig_kind();
    const bool     open  = kind != SIG_NONE && FM.squelch_open;
    const bool     wide  = ls_tui_is_wide();
    const uint8_t good  = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t val   = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim   = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    const uint8_t warn  = TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK);
    char buf[24];

    tui_box(sf, r, ls_tr("RECEIVER / SIGNAL"), TUI_ATTR(TUI_CYAN, TUI_BLACK));
    const int inner = r.h - 2;
    const int iw = r.w - 4;
    if (inner < 1 || iw < 16) return;
    const int x0 = r.x + 2;
    const int last = r.y + r.h - 2;
    int y = r.y + 1;

    if (receiver) s_glass.hold = 0.0f;
    if (kind != s_sig_kind) { s_glass.hold = 0.0f; s_sig_kind = kind; }

    /* Landscape puts the frequency and the readouts on one line; portrait
       gives the frequency its own, centred, with air under it when there is
       room. */
    int x = draw_dial(sf, r, x0, y, !wide);

    /* A mode change tears the receiver down and brings it back. The wipe is
       that gap, made visible, across the frequency line. */
    ls_radio_glass_wipe(&s_glass, sf, r, y, y + 1, (int)FM.mode);
    if (!wide) {
        x = x0;
        y += inner >= 6 ? 2 : 1;
    }
    if (y > last) return;

    /* CARRIER blinks while open, so a burst and a held carrier differ. */
    const char *carrier = receiver ? receiver : kind == SIG_NONE ? "--"
                        : open ? "OPEN" : "closed";
    x = kv(sf, r, x, y, "CARRIER", carrier, receiver ? warn : open ? good : dim);
    if (open && ((s_blink / 12) & 1))
        tui_put_char(sf, r, x - 2, y, LS_TUI_SHADE_FULL, good);
    snprintf(buf, sizeof(buf), "%lu B/s", (unsigned long)FM.iq_bytes_sec);
    x = kv(sf, r, x, y, "IQ", buf, val);
    if (wide) draw_extra(sf, r, x, y, true);
    y++;

    /* The bar, then the gate under it while there is a row for it. */
    if (y <= last) {
        draw_level(sf, r, x0, y, iw, kind, open, y + 1 <= last);
        y += 2;
    }
    if (FM.lr_iq_active) {
        char loss[80];
        LS_SNPRINTF(t_scr_fm_lr_iq_lost_lu, loss, sizeof(loss), "LR IQ lost >=%lu gaps %lu audio %lu",
            (unsigned long)FM.lr_lost_pairs, (unsigned long)FM.lr_gaps,
            (unsigned long)FM.lr_audio_drops);
        /* The gate's row is available in the compact landscape VFO too. */
        tui_put_str(sf, r, x0, last, loss, dim);
    } else if (!wide && y <= last) draw_extra(sf, r, x0, y, true);
}

/* The codeword tape: rx_panels/rx_tape.c, shared with the RADIO head. */
static EXT_RAM_BSS_ATTR rx_tape_t s_tape;
static void tape_sample(void) { const pager_counts_t pc = pager_counts(); rx_tape_sample(&s_tape, &pc); }
static void draw_tape(tui_surface *sf, tui_rect a)
{
    const pager_counts_t pc = pager_counts();
    rx_tape_draw(&s_tape, &pc, sf, a);
}

/* The pages list, and the page behind it. */

/* The list and its selection: rx_panels/rx_pages.c, shared with the RADIO
   head. This screen's source is the FM ring, the chip's pages or ACARS. */
static rx_pages_t s_pv;
static int page_count(void);
static const fm_page_t *page_at(int i);
static const rx_pages_src_t FM_PAGES = { .count = page_count, .at = page_at };

/* Newest first, which is the order the list shows and the order a selection
   index means. */
static const acars_msg_out_t *acars_at(int i)
{
    const acars_state_t *state = acars_app_state();
    if (i < 0 || i >= state->msg_count || i >= ACARS_MSG_LOG_MAX) return NULL;
    return &state->msgs[(state->msg_head - 1 - i + 2 * ACARS_MSG_LOG_MAX) % ACARS_MSG_LOG_MAX];
}

static const fm_page_t *page_at(int i)
{
    if (FM.mode == FM_MODE_ACARS) {
        static EXT_RAM_BSS_ATTR fm_page_t one;
        const acars_msg_out_t *msg = acars_at(i);
        if (!msg) return NULL;
        memset(&one, 0, sizeof(one));
        one.ts_us = msg->ts_us;
        return &one;
    }
    if (s_lora_pager) {
        /* The chip's pages, newest first already, in the receiver's shape.
           One at a time: every caller is done with a page before the next. */
        EXT_RAM_BSS_ATTR static fm_page_t one;
        if (i < 0 || i >= s_field.page_log_count) return NULL;
        const ls_field_page_t *p = &s_field.page_log[i];
        memset(&one, 0, sizeof(one));
        one.ts_us = p->ts_us;
        one.address = p->address;
        one.function = p->function;
        one.protocol = FM_PAGE_PROTOCOL_POCSAG;
        one.type = p->type;
        one.baud = p->baud;
        snprintf(one.text, sizeof(one.text), "%s", p->text);
        return &one;
    }
    if (i < 0 || i >= (int)FM.page_count || i >= FM_PAGE_LOG_MAX) return NULL;
    const int idx = (FM.page_head - 1 - i + 2 * FM_PAGE_LOG_MAX) % FM_PAGE_LOG_MAX;
    return &FM.pages[idx];
}

static int page_count(void)
{
    if (FM.mode == FM_MODE_ACARS) return acars_app_state()->msg_count;
    if (s_lora_pager) return s_field.page_log_count;
    int n = (int)FM.page_count;
    if (n > FM_PAGE_LOG_MAX) n = FM_PAGE_LOG_MAX;
    return n;
}

static void draw_pages(tui_surface *sf, tui_rect whole)
{
    rx_pages_refind(&s_pv, &FM_PAGES);
    tape_sample();

    s_pv.list = tui_rect_make(0, -1, 0, 0);
    s_pv.back = tui_rect_make(0, -1, 0, 0);
    whole.h -= rx_pages_controls(&s_pv, &FM_PAGES, sf, whole);

    /* The full view takes the whole pane. It is one message and it
       wants the width and the height; the tape above it answers a different
       question and can wait. */
    if (s_pv.open) {
        rx_pages_detail(&s_pv, &FM_PAGES, sf, whole);
        return;
    }

    /* The stream takes the top third, the pages the rest. Below about
       fourteen rows there is only room for one of them, and it is the pages:
       a decoded page is the answer and the tape is the working. */
    tui_rect area = whole;
    if (whole.h >= 14) {
        int th = whole.h / 3;
        if (th < 6)  th = 6;
        if (th > 10) th = 10;
        draw_tape(sf, tui_rect_make(whole.x, whole.y, whole.w, th));
        area = tui_rect_make(whole.x, whole.y + th, whole.w, whole.h - th);
    }

    char box[40], why[64];
    if (s_lora_pager) snprintf(box, sizeof(box), "DECODED PAGES / %s", ls_rsel_name(LS_RSEL_LORA));
    else snprintf(box, sizeof(box), "DECODED PAGES");
    /* FLEX with the chip chosen: the chip reads POCSAG only, so FLEX
       stays on the SDR, and says so rather than looking deaf. */
    const bool flex_sdr = FM.mode == FM_MODE_FLEX && ls_rsel_saved(LS_RSEL_PAGER) == LS_RSEL_LORA;
    if (flex_sdr) LS_SNPRINTF(t_scr_fm_flex_needs_an_sdr, why, sizeof(why), "FLEX needs an SDR; the %s reads POCSAG",
                           ls_rsel_name(LS_RSEL_LORA));
    rx_pages_list(&s_pv, &FM_PAGES, sf, area, box, flex_sdr ? why : NULL);
}

static void draw_sweep(tui_surface *sf, tui_rect area)
{
    ls_wf_note_full_view();
    ls_wf_source_select(LS_WF_SRC_FM);
    ls_wf_source_pump();

    const char *why = ls_wf_idle_reason();
    /* Keep HOLD reachable while the shared display is paused. */
    if (why && !ls_wf_cfg()->paused) {
        ls_panel_box(sf, area, "SWEEP", TUI_CYAN);
        tui_put_str(sf, area, area.x + 2, area.y + 2, why,
                    LS_ATTR_DIM);
        /* Where the first row is, when one is coming. A sweep of
           VHF land takes eleven seconds before it has a row to show, and
           "waiting for the receiver" alone reads the same as a receiver that
           is not there. */
        char line[48];
        if (FM.mode == FM_MODE_SCAN && FM.scan_tunes > 0)
            LS_SNPRINTF(t_scr_fm_sweeping_tune_d_of, line, sizeof(line), "sweeping: tune %d of %d",
                     FM.scan_idx + 1, FM.scan_tunes);
        else
            snprintf(line, sizeof(line), "%s", ls_tui_is_wide() ? "W starts the band sweep" : "Tap RUN SWEEP to start");
        tui_put_str(sf, area, area.x + 2, area.y + 4, line, LS_ATTR_DIM);
        return;
    }
    ls_wf_draw(sf, area);
}

static void draw_acars(tui_surface *sf, tui_rect area)
{
    rx_pages_refind(&s_pv, &FM_PAGES);
    const uint8_t val = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t lab = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    char buf[96];
    const acars_state_t *state = acars_app_state();
    s_pv.list = s_pv.back = tui_rect_make(0, -1, 0, 0);
    area.h -= rx_pages_controls(&s_pv, &FM_PAGES, sf, area);
    ls_panel_box(sf, area, "MESSAGES", TUI_CYAN);
    LS_SNPRINTF(t_scr_fm_delivered_lu_bad_crc, buf, sizeof(buf), "DELIVERED %lu  BAD CRC %lu",
             (unsigned long)state->n_delivered, (unsigned long)state->n_bad_crc);
    tui_put_str(sf, area, area.x + 2, area.y + 1, buf, lab);
    snprintf(buf, sizeof(buf), "SYNC %lu", (unsigned long)state->n_synced);
    tui_put_str(sf, area, area.x + 2, area.y + 2, buf, lab);
    if (s_pv.open) {
        const acars_msg_out_t *msg = s_pv.missing ? NULL : acars_at(s_pv.sel);
        s_pv.back = tui_rect_make(area.x + 1, area.y + area.h - 4, area.w - 2, 3);
        tui_box(sf, s_pv.back, ls_tr("BACK"), lab);
        if (!msg) {
            tui_put_str(sf, area, area.x + 2, area.y + 4, ls_tr("that message is gone"), val);
            return;
        }
        LS_SNPRINTF(t_scr_fm_s_label_s_block, buf, sizeof(buf), "%s  LABEL %s  BLOCK %c", msg->reg, msg->label, msg->block_id);
        tui_put_str(sf, area, area.x + 2, area.y + 4, buf, lab);
        static EXT_RAM_BSS_ATTR char wrap[16][96];
        int width = area.w - 4;
        if (width > 95) width = 95;
        const int lines = ls_wrap_text(msg->text, width, (char *)wrap, 96, 16);
        for (int i = 0; i < lines && area.y + 6 + i < s_pv.back.y; ++i)
            tui_put_str(sf, area, area.x + 2, area.y + 6 + i, wrap[i], val);
        return;
    }
    s_pv.list = tui_rect_make(area.x, area.y + 3, area.w, area.h - 3);
    const int rows = (s_pv.list.h - 2) / rx_pages_row_h();
    if (s_pv.sel < s_pv.top) s_pv.top = s_pv.sel;
    if (s_pv.sel >= s_pv.top + rows) s_pv.top = s_pv.sel - rows + 1;
    if (s_pv.top < 0) s_pv.top = 0;
    if (!page_count()) tui_put_str(sf, area, area.x + 2, area.y + 4, ls_tr("no messages yet"), val);
    for (int row = 0; row < rows; ++row) {
        const int index = s_pv.top + row;
        const acars_msg_out_t *msg = acars_at(index);
        if (!msg) break;
        snprintf(buf, sizeof(buf), "%s %s %s", msg->reg, msg->label, msg->text);
        tui_put_str(sf, area, area.x + 2, s_pv.list.y + 1 + row * rx_pages_row_h(),
                    buf, index == s_pv.sel ? TUI_ATTR(TUI_BLACK, TUI_CYAN) : val);
    }
}

/* SCANNER is the stored-channel scanner panel. RADIO is the radio itself,
   the button every app carries, on the control row above. */
static const ls_btn_t PAGES[] = {
    { "VFO",   NULL, '1', false, false },
    { "PAGER", NULL, '2', false, false },
    { "SPECTRUM", NULL, '3', false, false },
    { "SCANNER", NULL, '0', false, false },
};
#define N_PAGES ((int)(sizeof(PAGES) / sizeof(PAGES[0])))

/* PAGER decodes; the other three are true of any mode. Offering a tab that
   can only ever be empty is the kind of thing that makes a screen feel
   bigger than it is. */
static bool pager_mode(void)
{
    return FM.mode == FM_MODE_POCSAG || FM.mode == FM_MODE_FLEX || FM.mode == FM_MODE_SAME || FM.mode == FM_MODE_APRS || FM.mode == FM_MODE_AIS || FM.mode == FM_MODE_ACARS;
}

/* The broadcast extras' second page (fm_rds_view.c and friends): drawn,
   keyed and touched by their own files, never by the pager list. */
/* SSTV shows on LISTEN with the option on, and on AM while no channel scan
   is running (the scan's page is AM's otherwise). */
static bool sstv_page(void)
{
    if (!fm_bcast_opt(FM_BC_SSTV)) return false;
    if (FM.mode == FM_MODE_LISTEN) return true;
    if (FM.mode != FM_MODE_AM) return false;
    fm_air_status_t air;
    fm_air_status(&air);
    return air.preset == AIR_PRESET_OFF;
}
static bool own_page(void)
{
    return FM.mode == FM_MODE_WFM || FM.mode == FM_MODE_AM || sstv_page();
}
static const char *own_tab(void)
{
    if (sstv_page()) return "PICTURE";
    return FM.mode == FM_MODE_AM ? fm_air_tab() : "STATION";
}
static void own_draw(tui_surface *sf, tui_rect body)
{
    if (sstv_page()) { fm_sstv_draw(sf, body); return; }
    if (FM.mode == FM_MODE_AM) fm_air_draw(sf, body);
    else fm_rds_draw(sf, body);
}
static bool own_key(ls_tk_t k, char ch)
{
    return !sstv_page() && FM.mode == FM_MODE_AM && fm_air_key(k, ch);
}
static bool own_touch(int col, int row)
{
    if (!sstv_page() && FM.mode == FM_MODE_AM) return fm_air_touch(col, row);
    return true;
}

static tui_rect s_bar;
/* The keyboard/keypad toggle, beside the view tabs at the screen's own
   bottom-right rather than up with MODE/BAND/LOCK: portrait's tab strip
   already lives there, and landscape's deck is the lowest thing FM draws
   even though its tabs sit at the top (see draw()). VFO only; empty,
   and untappable, on the other pages. */
static tui_rect s_kbd_btn;
/* Which page each drawn tab belongs to. The strip hides PAGER outside a pager
   mode, so the slot a thumb lands on is not the page index; without this map
   tapping RADIO acted on SPECTRUM and SPECTRUM acted on a hidden tab. */
static int      s_tab_page[N_PAGES];
static int      s_tab_count;
static tui_rect s_controls;
/* What each drawn control button does, in the order the bar drew them: the
   bar drops OPTIONS for a mode with nothing to set, so a slot is not an
   action until this says which. */
enum { CTL_MODE, CTL_BAND, CTL_VOLUME, CTL_SQUELCH, CTL_LOCK,
       CTL_RADIO, CTL_OPTIONS, CTL_CALLS, CTL_MUTE, CTL__COUNT };
static uint8_t  s_ctl_id[CTL__COUNT];
static int      s_ctl_n;
/* The VFO page's VOLUME, GAIN and SQUELCH steppers: one row, each value
   drawn once, inside the control that sets it. A tap on the left third steps
   down, the right third up, and the value in the middle opens exact entry. */
#define N_STEP 3
static tui_rect s_quick_rect;
static tui_rect s_step_box[N_STEP];
static tui_rect s_step_down[N_STEP], s_step_value[N_STEP], s_step_up[N_STEP];
/* The bar itself, live cards only - set by draw_step_card and read by a tap.
   Empty while a card is disabled, so a tap cannot promise an unavailable
   adjustment. */
static tui_rect s_step_bar[N_STEP];
/* A tap on a +/- key lights it for a few frames, the same acknowledgement
   ls_quick's own panel gives: the value changing is the real answer, but it
   lands a frame or two later, and a key that looks the same before and after
   the touch is a key a thumb cannot tell it reached. */
#define STEP_FLASH_FRAMES 6
static int  s_step_flash = -1;      /* which of N_STEP, -1 none       */
static bool s_step_flash_up;        /* that one's '+' key, not '-'    */
static int  s_step_flash_ttl;
/* The VFO's frequency editor lives in the quick-control panel.  It is fixed
   storage: drawing and editing the keypad never allocate. */
static bool s_freq_keyboard;
static bool s_freq_invalid;
static char s_freq_entry[16];
static tui_rect s_freq_key[12], s_freq_action[3];

static void freq_entry_clear(void) { s_freq_entry[0] = '\0'; }
static void freq_entry_add(char c)
{
    ls_freq_entry_add(s_freq_entry, sizeof(s_freq_entry), c);
}

static void freq_entry_delete(void)
{
    ls_freq_entry_delete(s_freq_entry);
}

/* Parse MHz as integer Hz, with at most six fractional places. */
static bool freq_entry_hz(uint32_t *out)
{
    /* HF frequencies are typed in AM mode (SHORTWAVE puts it there); NFM
       and the decoders keep the tuner's 24 MHz floor. */
    return ls_freq_entry_hz_min(s_freq_entry, FM.mode == FM_MODE_AM ? LS_HF_MIN_HZ
                                                                     : LS_RTL_TUNER_MIN_HZ, out);
}

static void freq_entry_tune(void)
{
    uint32_t hz;
    if (!freq_entry_hz(&hz)) { s_freq_invalid = true; return; }
    scan_engine_stop();
    const bool was_locked = lakeshark_fm_frequency_locked();
    if (was_locked) lakeshark_fm_frequency_lock(false);
    ls_args_t args = {0}; ls_val_t out;
    args.n = 1; args.v[0].kind = LS_VAL_INT; args.v[0].i = (int32_t)hz;
    const bool ok = ls_action_call("fm.freq_hz", &args, &out,
                                   ls_quick_grant_builtin()) == LS_ACT_OK;
    if (ok) {
        lakeshark_fm_frequency_lock(true);
        s_freq_keyboard = false; s_freq_invalid = false;
    } else {
        if (was_locked) lakeshark_fm_frequency_lock(true);
        s_freq_invalid = true;
    }
}

static bool freq_key_char(char c)
{
    if ((c >= '0' && c <= '9') || c == '.') {
        freq_entry_add(c); s_freq_invalid = false; return true;
    }
    if (c == 'd' || c == 'D' || c == '<') {
        freq_entry_delete(); s_freq_invalid = false; return true;
    }
    if (c == 'c' || c == 'C') {
        freq_entry_clear(); s_freq_invalid = false; return true;
    }
    if (c == 't' || c == 'T') { freq_entry_tune(); return true; }
    if (c == 'x' || c == 'X') { s_freq_keyboard = false; return true; }
    return false;
}

static bool freq_keyboard_touch(int col, int row)
{
    const char c = ls_freq_pad_hit(s_freq_key, s_freq_action, col, row);
    if (c) return freq_key_char(c);
    return true;
}

static void band_picked(int index)
{
    /* Choosing a named band is an explicit replacement for a custom carrier
       lock.  The next sweep follows that band exactly. */
    if (!ls_wf_preset_apply(LS_WF_SRC_FM, index)) return;
    lakeshark_fm_frequency_lock(false);
}

static void open_band_picker(void)
{
    ls_picker_open("FM BAND", band_picked);
    const int n = ls_wf_preset_count(LS_WF_SRC_FM);
    for (int i = 0; i < n; ++i)
        ls_picker_add(ls_wf_preset_label(LS_WF_SRC_FM, i),
                      ls_wf_preset_detail(LS_WF_SRC_FM, i));
}

static void toggle_frequency_lock(void)
{
    if (FM.mode == FM_MODE_SCAN) ls_wf_fm_sweep(false);
    lakeshark_fm_frequency_lock(!lakeshark_fm_frequency_locked());
}

static void toggle_sweep(void)
{
    /* A span sweep and the saved-channel scan both own the tuner. Starting a
       sweep is an explicit radio change, so release the channel scanner here;
       merely opening the Spectrum view remains a view-only operation. */
    if (FM.mode != FM_MODE_SCAN) scan_engine_stop();
    ls_wf_fm_sweep(FM.mode != FM_MODE_SCAN);
    s_page = FM.mode == FM_MODE_SCAN ? 2 : 0;
}

/* ------------------------------------------------------------- OPTIONS -- */

/* What each mode has to set, and only that: squelch where the mode has one
   (NFM gates on noise, AM on level, WFM and the decoders on nothing), the
   paging rate and polarity for POCSAG, the channel for ACARS, and gain for
   whatever an SDR is doing. */

static void set_squelch(double value);

static double o_gain(const ls_opt_t *o) { (void)o; return FM.gain_tenths / 10.0; }
static void o_set_gain(const ls_opt_t *o, double db)
{
    (void)o;
    ls_args_t args = {.n = 1}; ls_val_t out;
    args.v[0].kind = LS_VAL_FLOAT; args.v[0].f = (float)db;
    ls_action_call("fm.gain", &args, &out, ls_quick_grant_builtin());
}
static void o_show_gain(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    if (FM.gain_tenths <= 0) snprintf(out, n, "AUTO");
    else snprintf(out, n, "%d.%d dB", FM.gain_tenths / 10, FM.gain_tenths % 10);
}

static double o_squelch(const ls_opt_t *o) { (void)o; return lakeshark_fm_squelch_get(); }
static void o_set_squelch(const ls_opt_t *o, double v) { (void)o; set_squelch(v); }
static void o_show_squelch(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    if (hf_squelch_text(out, n)) return;
    if (FM.mode == FM_MODE_AM) snprintf(out, n, "%d.%d%%", lakeshark_fm_squelch_get() / 10, lakeshark_fm_squelch_get() % 10);
    else snprintf(out, n, "%d%%", lakeshark_fm_squelch_get());
}

/* The SDR's decoders run 512, 1200 and 2400 at once on AUTO; the chip hears
   one of 1200 and 2400 at a time. arg 1 is the chip's row. */
static const char *const BAUD_SDR[] = { "AUTO", "512", "1200", "2400" };
static const char *const BAUD_CHIP[] = { "AUTO", "1200", "2400" };
static const int BAUD_SDR_V[] = { 0, 512, 1200, 2400 };
static const int BAUD_CHIP_V[] = { 0, 1200, 2400 };

static int o_baud(const ls_opt_t *o)
{
    const int *v = o->arg ? BAUD_CHIP_V : BAUD_SDR_V;
    if (FM.pocsag_auto) return 0;
    for (int i = 1; i < o->n; i++) if (v[i] == FM.pocsag_baud) return i;
    return 1;
}
static void o_set_baud(const ls_opt_t *o, int i)
{
    const int *v = o->arg ? BAUD_CHIP_V : BAUD_SDR_V;
    if (i >= 0 && i < o->n) lakeshark_fm_set_baud(v[i]);
}
static void o_show_baud(const ls_opt_t *o, char *out, size_t n)
{
    if (!FM.pocsag_auto) { snprintf(out, n, "%d", FM.pocsag_baud); return; }
    /* On AUTO, the rate in use once there is one to name. */
    if (o->arg) snprintf(out, n, "AUTO (%d)", s_lora_pager && s_lora_baud ? s_lora_baud : 1200);
    else if (FM.pocsag_lock_baud) snprintf(out, n, "AUTO (%d)", FM.pocsag_lock_baud);
    else snprintf(out, n, "AUTO");
}

static const char *const POLARITY[] = { "AUTO", "NORMAL", "INVERTED" };
static int o_polarity(const ls_opt_t *o) { (void)o; return s_lora_pol; }
static void o_set_polarity(const ls_opt_t *o, int v)
{
    (void)o;
    s_lora_pol = v >= 0 && v < 3 ? v : 0;
    s_lora_flip_at = esp_timer_get_time() + LORA_FLIP_US;
}
static void o_show_polarity(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    if (s_lora_pol) snprintf(out, n, "%s", POLARITY[s_lora_pol]);
    else snprintf(out, n, "AUTO (%s)", s_lora_inv ? "inverted" : "normal");
}

/* ACARS: the three North American channels. */
static const char *const ACARS_CH[] = { "131.550", "130.025", "129.125" };
static const uint32_t ACARS_HZ[] = { 131550000u, 130025000u, 129125000u };
static int o_acars(const ls_opt_t *o)
{
    (void)o;
    for (int i = 0; i < 3; i++) if (FM.freq_hz == ACARS_HZ[i]) return i;
    return -1;
}
static void o_set_acars(const ls_opt_t *o, int i)
{
    (void)o;
    if (i < 0 || i >= 3) return;
    ls_args_t args = {.n = 1}; ls_val_t out;
    args.v[0].kind = LS_VAL_INT; args.v[0].i = (int32_t)ACARS_HZ[i];
    /* The keypad's order: release, tune, hold the new carrier. */
    lakeshark_fm_frequency_lock(false);
    if (ls_action_call("fm.freq_hz", &args, &out, ls_quick_grant_builtin()) == LS_ACT_OK)
        lakeshark_fm_frequency_lock(true);
}
static void o_show_acars(const ls_opt_t *o, char *out, size_t n)
{
    const int i = o_acars(o);
    if (i >= 0) snprintf(out, n, "%s MHz", ACARS_CH[i]);
    else snprintf(out, n, "%.3f MHz", FM.freq_hz / 1e6);
}

#define OPT_GAIN { .label = "GAIN", .kind = LS_OPT_LEVEL, .step = 2, .radios = LS_OPT_SDR, \
                   .num = o_gain, .set_num = o_set_gain, .lo = 0, .hi = 49.6, \
                   .unit = "dB, 0 is automatic", .show = o_show_gain }

static int o_tone(const ls_opt_t *o) { (void)o; return FM.tone_required; }
static void o_set_tone(const ls_opt_t *o, int v)
{
    (void)o;
    if (v < 0 || v >= FM_TONE_CHOICES) return;
    FM.tone_required = (uint16_t)v;
    /* Saved NFM memories retain the receive gate; free VFOs stay transient. */
    int zone = scan_engine_get_zone();
    for (int i = 0; i < scan_channels_count(); ++i) {
        const scan_channel_t *c = scan_channel_get(i);
        if (c && c->mode == SCAN_MODE_NFM && c->freq_hz == FM.freq_hz &&
            (zone < 0 || c->zone == zone)) {
            scan_channel_set_tone(i, (uint16_t)v);
            break;
        }
    }
}
static void o_show_tone(const ls_opt_t *o, char *out, size_t n)
{
    (void)o; fm_tone_label(FM.tone_required, out, n);
}
static int o_tone_show(const ls_opt_t *o) { (void)o; return FM.tone_show; }
static void o_set_tone_show(const ls_opt_t *o, int v) { (void)o; FM.tone_show = v != 0; }
static const ls_opt_t OPT_TONE[] = {
    { .label = "TONE SQUELCH", .kind = LS_OPT_CYCLE, .n = FM_TONE_CHOICES,
      .get = o_tone, .set = o_set_tone, .show = o_show_tone, .step = 1 },
    { .label = "SHOW DETECTED", .kind = LS_OPT_TOGGLE,
      .get = o_tone_show, .set = o_set_tone_show },
};
static const ls_opt_ctx_t CTX_TONE = {
    .name = "TONE", .job = LS_RSEL_FM, .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_TONE)
};
static const char *const LR_CHANNELS[] = { "12.5 kHz", "25 kHz" };
static int o_lr_channel(const ls_opt_t *o) { (void)o; return FM.lr_channel_hz == 12500 ? 0 : 1; }
static void o_set_lr_channel(const ls_opt_t *o, int value) { (void)o; FM.lr_channel_hz = value ? 25000 : 12500; }
/* SSTV listens on NFM, AM and the HF chain's USB/LSB alike. */
#define OPT_SSTV { .label = "SSTV PICTURES", .kind = LS_OPT_TOGGLE, .get = fm_sstv_opt_get, \
                   .set = fm_sstv_opt_set }
static const ls_opt_t OPT_NFM[] = {
    { .label = "CHANNEL", .kind = LS_OPT_CYCLE, .radios = LS_OPT_LORA,
      .names = LR_CHANNELS, .n = 2, .get = o_lr_channel, .set = o_set_lr_channel },
    { .label = "TONE", .kind = LS_OPT_MENU, .sub = &CTX_TONE, .show = o_show_tone },
    LS_CALLS_MENU,
    { .label = "SQUELCH", .kind = LS_OPT_LEVEL, .step = 1, .show = o_show_squelch, .num = o_squelch, .set_num = o_set_squelch,
      .lo = 0, .hi = 100, .unit = "0 opens on anything, 100 on nothing" },
    OPT_GAIN,
    LS_RFFE_MENUS,
    OPT_SSTV,
};
/* HF rows. They sit on AM's list because HF is AM mode on the HF chain;
   while the chain is off (plain AM on airband) all but DEMOD say why not,
   and DEMOD is how SSB on VHF gets there. */
static const char *hf_only(const ls_opt_t *o)
{
    (void)o;
    return hf_on() ? NULL : "below 30 MHz, or SSB/CW";
}
static const char *const HF_DEMODS[] = { "AM", "SAM", "USB", "LSB", "CW" };
static int o_hf_demod(const ls_opt_t *o) { (void)o; return fm_hf_get()->demod; }
static void o_set_hf_demod(const ls_opt_t *o, int v) { (void)o; fm_hf_set_demod(v); }
static int o_hf_narrow(const ls_opt_t *o) { (void)o; return fm_hf_get()->narrow; }
static void o_set_hf_narrow(const ls_opt_t *o, int v) { (void)o; fm_hf_set_narrow(v != 0); }
static void o_show_hf_filter(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    const uint32_t w = fm_hf_filter_hz(fm_hf_get());
    if (w < 1000) snprintf(out, n, "%u Hz", (unsigned)w);
    else snprintf(out, n, "%u.%u kHz", (unsigned)(w / 1000), (unsigned)(w % 1000 / 100));
}
static const char *const HF_AGCS[] = { "FAST", "SLOW", "OFF" };
static int o_hf_agc(const ls_opt_t *o) { (void)o; return fm_hf_get()->agc; }
static void o_set_hf_agc(const ls_opt_t *o, int v) { (void)o; fm_hf_set_agc(v); }
static const char *const HF_NBS[] = { "OFF", "LOW", "HIGH" };
static int o_hf_nb(const ls_opt_t *o) { (void)o; return fm_hf_get()->nb; }
static void o_set_hf_nb(const ls_opt_t *o, int v) { (void)o; fm_hf_set_nb(v); }
static double o_hf_rit(const ls_opt_t *o) { (void)o; return fm_hf_get()->rit_hz; }
static void o_set_hf_rit(const ls_opt_t *o, double v) { (void)o; fm_hf_set_rit((int)(v >= 0 ? v + 0.5 : v - 0.5)); }
static void o_show_hf_rit(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    snprintf(out, n, "%+d Hz", fm_hf_get()->rit_hz);
}
static double o_hf_pitch(const ls_opt_t *o) { (void)o; return fm_hf_get()->pitch_hz; }
static void o_set_hf_pitch(const ls_opt_t *o, double v) { (void)o; fm_hf_set_pitch((int)(v + 0.5)); }
static const char *cw_only(const ls_opt_t *o)
{
    (void)o;
    return fm_hf_get()->demod == FM_HF_CW ? NULL : "CW only";
}
static void o_hf_bands(const ls_opt_t *o) { (void)o; open_hf_band_picker(); }
static void o_show_hf_band(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    snprintf(out, n, "%s", hf_on() ? hf_band_word() : "--");
}
static const ls_opt_t OPT_HF_MORE[] = {
    { .label = "AGC", .kind = LS_OPT_CYCLE, .names = HF_AGCS, .n = 3,
      .get = o_hf_agc, .set = o_set_hf_agc, .why_not = hf_only, .step = 1 },
    { .label = "NOISE BLANKER", .kind = LS_OPT_CYCLE, .names = HF_NBS, .n = 3,
      .get = o_hf_nb, .set = o_set_hf_nb, .why_not = hf_only, .step = 1 },
    { .label = "CW PITCH", .kind = LS_OPT_LEVEL, .step = 10, .num = o_hf_pitch,
      .set_num = o_set_hf_pitch, .lo = FM_HF_PITCH_MIN_HZ, .hi = FM_HF_PITCH_MAX_HZ,
      .unit = "Hz the CW carrier is heard at", .why_not = cw_only },
};
static const ls_opt_ctx_t CTX_HF_MORE = {
    .name = "HF", .job = LS_RSEL_FM, .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_HF_MORE)
};
#define OPT_HF_DEMOD { .label = "DEMOD", .kind = LS_OPT_CYCLE, .names = HF_DEMODS, \
                       .n = FM_HF_DEMOD_COUNT, .get = o_hf_demod, .set = o_set_hf_demod, .step = 1 }
/* The airband / CB channel scanner: on both AM lists, since CB on 27 MHz
   is on the HF chain until a scan starts and the scanner takes the LO. */
#define OPT_AIR_SCAN { .label = "CHANNEL SCAN", .kind = LS_OPT_MENU, .radios = LS_OPT_SDR, \
                       .sub = &fm_air_options, .show = fm_air_options_show }
/* Plain AM (airband) keeps its list; DEMOD at the end is the way to SSB or
   CW on VHF, which moves the receiver onto the HF chain and its own list. */
static const ls_opt_t OPT_AM[] = {
    LS_CALLS_MENU,
    { .label = "SQUELCH", .kind = LS_OPT_LEVEL, .step = 1, .show = o_show_squelch, .num = o_squelch, .set_num = o_set_squelch,
      .lo = 0, .hi = 100, .unit = "carrier level, tenths of a percent" },
    OPT_GAIN,
    OPT_HF_DEMOD,
    LS_RFFE_MENUS,
    OPT_AIR_SCAN,
    OPT_SSTV,
};
/* HF: what is changed while tuning first, the rest a level down. */
static const ls_opt_t OPT_HF[] = {
    OPT_HF_DEMOD,
    { .label = "FILTER", .kind = LS_OPT_TOGGLE, .get = o_hf_narrow, .set = o_set_hf_narrow,
      .show = o_show_hf_filter, .why_not = hf_only },
    { .label = "CLARIFIER", .kind = LS_OPT_LEVEL, .step = FM_HF_RIT_STEP_HZ,
      .num = o_hf_rit, .set_num = o_set_hf_rit, .show = o_show_hf_rit,
      .lo = -FM_HF_RIT_MAX_HZ, .hi = FM_HF_RIT_MAX_HZ,
      .unit = "Hz, moves what is heard, not the dial", .why_not = hf_only },
    { .label = "BAND", .kind = LS_OPT_ACTION, .act = o_hf_bands, .show = o_show_hf_band,
      .leaves = true },
    { .label = "SQUELCH", .kind = LS_OPT_LEVEL, .step = 1, .show = o_show_squelch, .num = o_squelch, .set_num = o_set_squelch,
      .lo = 0, .hi = 100, .unit = "0 open; 1-100 is -110 to -20 dBFS" },
    OPT_GAIN,
    { .label = "AGC / BLANKER", .kind = LS_OPT_MENU, .sub = &CTX_HF_MORE },
    LS_CALLS_MENU,
    LS_RFFE_MENUS,
    OPT_AIR_SCAN,
    OPT_SSTV,
};
static const ls_opt_t OPT_GAIN_ONLY[] = { OPT_GAIN, LS_RFFE_MENUS };
static int o_pager_voice(const ls_opt_t *o) { (void)o; return FM.pager_voice; }
static void o_set_pager_voice(const ls_opt_t *o, int v) { (void)o; lakeshark_fm_pager_voice(v != 0); }
static const ls_opt_t OPT_POCSAG[] = {
    { .label = "BAUD", .kind = LS_OPT_CYCLE, .radios = LS_OPT_SDR, .arg = 0,
      .names = BAUD_SDR, .n = 4, .get = o_baud, .set = o_set_baud, .show = o_show_baud },
    { .label = "BAUD", .kind = LS_OPT_CYCLE, .radios = LS_OPT_LORA, .arg = 1,
      .names = BAUD_CHIP, .n = 3, .get = o_baud, .set = o_set_baud, .show = o_show_baud },
    { .label = "POLARITY", .kind = LS_OPT_CYCLE, .radios = LS_OPT_LORA,
      .names = POLARITY, .n = 3, .get = o_polarity, .set = o_set_polarity, .show = o_show_polarity },
    /* The RX HEAD runs the voice beside the decoder: hear the channel too. */
    { .label = "HEAR CHANNEL", .kind = LS_OPT_TOGGLE, .radios = LS_OPT_SDR | LS_OPT_LORA,
      .get = o_pager_voice, .set = o_set_pager_voice },
    OPT_GAIN,
    LS_RFFE_MENUS,
};
static const ls_opt_t OPT_ACARS[] = {
    { .label = "CHANNEL", .kind = LS_OPT_CYCLE, .names = ACARS_CH, .n = 3,
      .get = o_acars, .set = o_set_acars, .show = o_show_acars },
    OPT_GAIN,
    LS_RFFE_MENUS,
};

static const ls_opt_ctx_t CTX_NFM    = { .name = "NFM", .job = LS_RSEL_FM, .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_NFM) };
static const ls_opt_ctx_t CTX_AM     = { .name = "AM", .job = LS_RSEL_FM, .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_AM) };
static const ls_opt_ctx_t CTX_HF     = { .name = "HF", .job = LS_RSEL_FM, .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_HF) };
static const ls_opt_ctx_t CTX_POCSAG = { .name = "POCSAG", .job = LS_RSEL_PAGER, .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_POCSAG) };
static const ls_opt_ctx_t CTX_FLEX   = { .name = "FLEX", .job = LS_RSEL_PAGER, .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_GAIN_ONLY) };
static const ls_opt_ctx_t CTX_ACARS  = { .name = "ACARS", .job = LS_RSEL_ACARS, .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_ACARS) };

/* The mode's, or NULL in a sweep, which has nothing here to set. */
static const ls_opt_ctx_t *fm_options(void)
{
    switch (FM.mode) {
    case FM_MODE_LISTEN: return &CTX_NFM;
    case FM_MODE_WFM:    return &fm_wfm_options;
    case FM_MODE_AM:     return hf_on() ? &CTX_HF : &CTX_AM;
    case FM_MODE_POCSAG: return &CTX_POCSAG;
    case FM_MODE_FLEX:   return &CTX_FLEX;
    case FM_MODE_ACARS:  return &CTX_ACARS;
    case FM_MODE_SAME:   return &fm_same_options;
    case FM_MODE_APRS:   return &fm_aprs_options;
    case FM_MODE_AIS:    return &fm_ais_options;
    default:             return NULL;
    }
}

/* MODE, BAND, TUNE and LOCK, then the four every app carries.

   On the VFO page VOLUME and SQUELCH were buttons here as well as steppers
   below and readouts in the panel: three places for one number. There they
   are steppers only, with the value inside, and TUNE takes the slot as a
   button the size of its neighbours instead of a slab across the pane. The
   other pages have no steppers, so they keep VOLUME and SQUELCH here. */
static int ctl_count(bool vfo)
{
    const int opts = ls_opt_count(fm_options()) ? 1 : 0;
    /* The keyboard/keypad toggle moved to its own button beside the view
       tabs (see draw_kbd_toggle); it no longer takes a slot here. */
    return vfo ? 6 + opts : 8 + opts;
}

static int draw_controls(tui_surface *sf, tui_rect area, bool vfo)
{
    char locked[20];
    const uint32_t lock_hz = lakeshark_fm_frequency_lock_hz();
    if (lock_hz) snprintf(locked, sizeof(locked), "%.4f", lock_hz / 1e6);
    else snprintf(locked, sizeof(locked), "OFF");
    char band[16], vol[12], sql[16];
    snprintf(vol, sizeof(vol), "%d%%", audio_volume_get());
    squelch_text(sql, sizeof(sql));
    const bool wide = ls_tui_is_wide();
    /* APRS sits on one channel, so name it; the sweep presets are for the rest. */
    if (FM.mode == FM_MODE_APRS) snprintf(band, sizeof(band), "%.2f", aprs_frequency() / 1e6);
    else snprintf(band, sizeof(band), "%s", FM.mode == FM_MODE_AIS ? "87B/88B"
                  : hf_on() ? hf_band_word() : ls_wf_preset_current(LS_WF_SRC_FM));
    const bool opts = ls_opt_count(fm_options()) > 0;
    ls_btn_t buttons[CTL__COUNT];
    s_ctl_n = 0;
#define CTL(id, ...) do { buttons[s_ctl_n] = (ls_btn_t)__VA_ARGS__; \
                          s_ctl_id[s_ctl_n++] = (uint8_t)(id); } while (0)
    CTL(CTL_MODE, {"MODE", mode_word(), 'e', false, false, false});
    CTL(CTL_BAND, {"BAND", band, 'n', false, false, false});
    if (!vfo) {
        CTL(CTL_VOLUME, {wide ? "VOL" : "VOLUME", vol, 'v', false, false, false});
        CTL(CTL_SQUELCH, {wide ? "SQL" : "SQUELCH", sql, 'q',
                          sig_kind() != SIG_NONE && FM.squelch_open, false, false});
    }
    CTL(CTL_LOCK, {"LOCK", locked, 'k', lakeshark_fm_frequency_locked(), false, false});
    buttons[s_ctl_n] = ls_rsel_button(fm_job()); s_ctl_id[s_ctl_n++] = CTL_RADIO;
    if (opts) { buttons[s_ctl_n] = ls_opt_button(fm_options()); s_ctl_id[s_ctl_n++] = CTL_OPTIONS; }
    CTL(CTL_CALLS, {"CALLS", NULL, '5', false, false, false});
    CTL(CTL_MUTE, {"MUTE", audio_is_muted() ? "ON" : "OFF", 'b', audio_is_muted(), false, false});
#undef CTL
    s_controls = area;
    ls_btn_bar_raised(sf, s_controls, buttons, s_ctl_n, -1);
    return area.h;
}

static void set_squelch(double value);

static void set_volume(double value)
{
    if (!(value >= 0 && value <= 100)) return;
    ls_args_t args = {.n = 1}; ls_val_t out;
    args.v[0].kind = LS_VAL_INT; args.v[0].i = (int)(value + 0.5);
    ls_action_call("audio.volume", &args, &out, ls_quick_grant_builtin());
}

static void panel_set_volume_pct(int pct)
{
    set_volume((double)pct);
}

static void panel_set_squelch_pct(int pct)
{
    set_squelch((double)pct);
}

static void set_gain(double db)
{
    if (!(db >= 0 && db <= 49.6)) return;
    o_set_gain(NULL, db);
}

static void open_volume_entry(void)
{
    ls_numpad_open("VOLUME", "0 to 100", audio_volume_get(), set_volume);
}

static void open_squelch_entry(void)
{
    ls_numpad_open("SQUELCH", squelch_hint(), lakeshark_fm_squelch_get(), set_squelch);
}

static void open_gain_entry(void)
{
    ls_numpad_open("GAIN", "dB, 0 is automatic", FM.gain_tenths / 10.0, set_gain);
}

/* Shared by the relocated keyboard/keypad button and nothing else now -
   draw_controls() no longer carries a CTL_TUNE slot. */
static void toggle_freq_keyboard(void)
{
    if (s_freq_keyboard) s_freq_keyboard = false;
    else { scan_engine_stop(); freq_entry_clear(); s_freq_invalid = false; s_freq_keyboard = true; }
}

static bool control_action(int slot)
{
    if (slot < 0 || slot >= s_ctl_n) return false;
    switch (s_ctl_id[slot]) {
    case CTL_MODE:    open_mode_picker(); return true;
    case CTL_BAND:    if (hf_on()) open_hf_band_picker(); else open_band_picker(); return true;
    case CTL_VOLUME:  open_volume_entry(); return true;
    case CTL_SQUELCH: open_squelch_entry(); return true;
    case CTL_LOCK:    toggle_frequency_lock(); return true;
    case CTL_RADIO:   open_radio(); return true;
    case CTL_OPTIONS: ls_opt_open(fm_options()); return true;
    case CTL_CALLS:   ls_calls_open("FM"); ls_wf_source_release(); return true;
    case CTL_MUTE:    audio_toggle_mute(); return true;
    default:          return false;
    }
}

/* ------------------------------------------------------------ steppers -- */

/* Squelch as the mode reads it: NFM in percent of quieting, AM in tenths of
   a percent of carrier, and nothing where the mode has no squelch. Unlike the
   panel's readings this does not wait on the receiver: it is a setting, and
   a setting is true whether or not the dongle is streaming. */
static bool mode_has_squelch(void)
{
    return FM.mode == FM_MODE_LISTEN || FM.mode == FM_MODE_AM;
}

/* The LR2021 path takes the chip's own gain as lr_wf_session copied it at
   open, not a live setting this screen can read or move: fm_lr_step()
   (app_fm.c) throws away the generic gain request it is handed, so a dial
   that still read like a setting here would be promising an adjustment that
   never reaches the receiver. There is no FM-specific manual gain on that
   path yet - p25.lrgain is P25's own state and would not move FM - so until
   one exists this control goes inert instead of lying about what it does.
   LR is "selected" from the moment the radio picker says so, not only once
   lr_iq_active catches up a frame or two later: a dial that is briefly live
   while the session comes up would still be the thing this disables. */
static bool fm_gain_live(void)
{
    return !FM.lr_iq_active && ls_rsel_saved(LS_RSEL_FM) != LS_RSEL_LORA;
}

static void step_value(int i, char *out, size_t n)
{
    switch (i) {
    case 0: snprintf(out, n, "%d%%", audio_volume_get()); break;
    case 1:
        /* Not "AUTO": that is a known state (0 dB, the chip's own AGC) and
           this screen cannot read what the chip was actually opened with -
           lr_wf_session keeps its own copy of the gain step. Unavailable,
           not guessed. */
        if (!fm_gain_live()) snprintf(out, n, "LR N/A");
        else if (FM.gain_tenths <= 0) snprintf(out, n, "AUTO");
        else snprintf(out, n, "%d.%d", FM.gain_tenths / 10, FM.gain_tenths % 10);
        break;
    default:
        if (hf_squelch_text(out, n)) break;
        if (FM.mode == FM_MODE_AM)
            snprintf(out, n, "%d.%d%%", lakeshark_fm_squelch_get() / 10, lakeshark_fm_squelch_get() % 10);
        else if (mode_has_squelch()) snprintf(out, n, "%d%%", lakeshark_fm_squelch_get());
        else snprintf(out, n, "--");
        break;
    }
}

/* ========================================================== THE GLASS == */

/* A HANDHELD'S LCD, NOT ANOTHER APP CARD.

   The VFO page is one piece of glass with a control deck under it, the way
   the radio it is pretending to be is laid out. Everything on the glass is
   text cells: the carrier is drawn in block figures four cells wide and five
   tall - about twenty times the area of a line of type, which is what makes
   it readable at arm's length - over the faint "every segment" ghost a real
   LCD shows. Thin underscore-and-pipe segments were tried first and were
   unreadable on the panel; strokes a whole cell thick are not.

   The only state the glass keeps is the trace ring below. It samples the
   signal the squelch is already measuring, one value a frame, so the moving
   history costs no acquisition, no FFT and no buffer beyond these bytes. */

/* The rail of annunciators: what the receiver is, and the three states a
   thumb can put it into without looking. */
static void lcd_rail(tui_surface *sf, tui_rect r, int x0, int y, int iw)
{
    const ls_btn_t radio = ls_rsel_button(fm_job());
    const bool tone = FM.mode == FM_MODE_LISTEN && FM.tone_show &&
                      FM.tone_detected.selection != 0;
    const ls_radio_chip_t left[] = {
        { mode_word(), true, hf_on() ? TUI_MAGENTA : TUI_YELLOW },
        { radio.value, true, TUI_BLUE }
    };
    const ls_radio_chip_t right[] = {
        { "LOCK", lakeshark_fm_frequency_locked(), TUI_GREEN },
        { "MUTE", audio_is_muted(), TUI_RED },
        { "TONE", tone, TUI_MAGENTA }
    };
    /* WFM has no tone gate: its lamps are the pilot and the RDS lock. */
    const ls_radio_chip_t wfm[] = {
        { "LOCK", lakeshark_fm_frequency_locked(), TUI_GREEN },
        { "MUTE", audio_is_muted(), TUI_RED },
        { "STEREO", fm_rds_stereo(), TUI_GREEN },
        { "RDS", fm_rds_synced(), TUI_CYAN }
    };
    if (FM.mode == FM_MODE_WFM) ls_radio_glass_rail(sf, r, x0, y, iw, left, 2, wfm, 4);
    else ls_radio_glass_rail(sf, r, x0, y, iw, left, 2, right, 3);
}

/* THE HF LINE: band, demod and filter, AGC, blanker, clarifier, in the
   colours they are on the scope below. The band is coloured for what lives
   there - broadcasting, amateurs, CB, the time stations. */
static int draw_hf_line(tui_surface *sf, tui_rect r, int x, int y, int w)
{
    static const uint8_t KIND[] = { TUI_YELLOW, TUI_GREEN, TUI_MAGENTA, TUI_CYAN };
    const fm_hf_settings_t *h = fm_hf_get();
    const int end = x + w;
    char buf[24];
    const int b = fm_hf_band_find(FM.freq_hz);
    if (b >= 0) {
        const fm_hf_band_t *band = fm_hf_band(b);
        const uint8_t hue = band->kind < 4 ? KIND[band->kind] : TUI_WHITE;
        tui_put_str(sf, r, x, y, band->name, TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK));
        x += (int)strlen(band->name) + 2;
    }
    o_show_hf_filter(NULL, buf, sizeof(buf));
    char dm[32];
    snprintf(dm, sizeof(dm), "%s %s", fm_hf_demod_label(h->demod), buf);
    if (x + (int)strlen(dm) <= end) {
        tui_put_str(sf, r, x, y, dm, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
        x += (int)strlen(dm) + 2;
    }
    if (x + 8 <= end) x = kv(sf, r, x, y, "AGC", fm_hf_agc_label(h->agc),
                             TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    if (x + 7 <= end && h->nb)
        x = kv(sf, r, x, y, "NB", fm_hf_nb_label(h->nb), TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
    if (x + 9 <= end && h->rit_hz) {
        snprintf(buf, sizeof(buf), "%+d", h->rit_hz);
        x = kv(sf, r, x, y, "RIT", buf, TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK));
    }
    return x;
}

/* THE PASSBAND SCOPE, where NFM keeps its activity trace.

   +-12 kHz of the real spectrum around the dial in braille columns (the FFT
   the waterfall already runs, 1 kHz a bin, so it moves only when the band
   does), over the filter drawn as a skirt: '_' outside, '/' '\' at the edges
   and '=' across what is heard, with the carrier '|' and, when the clarifier
   is off centre, a '^' where the passband has moved to. The demod and width
   sit inside the passband they name. */
/* PSRAM: internal RAM is the DMA pool. */
EXT_RAM_BSS_ATTR static float s_hf_bins[FM_SPECTRUM_BINS];
EXT_RAM_BSS_ATTR static float s_hf_cols[2 * 96];
static void draw_hf_scope(tui_surface *sf, tui_rect r, int x0, int y0, int w, int h)
{
    if (w < 16 || h < 2) return;
    const fm_hf_settings_t *st = fm_hf_get();
    /* Wide enough to see the neighbours, narrow enough that the passband
       is more than a few columns: +-12 kHz for AM, 6 for SSB, 3 for CW. */
    const int half = st->demod == FM_HF_CW ? 3000
                          : st->demod == FM_HF_USB || st->demod == FM_HF_LSB ? 6000 : 12000;
    const int base = y0 + h - 1;          /* the skirt row */
    const int rows = h - 1;               /* braille rows above it */
    int32_t lo, hi;
    hf_demod_passband((hf_demod_mode_t)st->demod, fm_hf_filter_hz(st), &lo, &hi);
    lo += st->rit_hz;
    hi += st->rit_hz;
    const double hz_per_col = 2.0 * half / (double)w;

    /* The spectrum, from the shared FFT; flat and still when it is not running. */
    fm_spectrum_enable(true);
    fm_spectrum_snapshot_t snap;
    const bool live = fm_spectrum_read(s_hf_bins, FM_SPECTRUM_BINS,
                                       (uint32_t)(esp_timer_get_time() / 1000), &snap) &&
                      snap.span_hz > 0;
    if (live && rows > 0) {
        /* Scale to what is on screen: the quietest column is the floor. */
        float lo_v = 1.0f, hi_v = 0.0f;
        float *col = s_hf_cols;
        const int cap = (int)(sizeof(s_hf_cols) / sizeof(s_hf_cols[0]));
        const int n = 2 * w < cap ? 2 * w : cap;
        for (int c = 0; c < n; ++c) {
            const double off = -half + (c + 0.5) * half * 2.0 / n;
            const double f = (double)FM.freq_hz + off - (double)snap.center_hz;
            const double bin = f / ((double)snap.span_hz / FM_SPECTRUM_BINS) + FM_SPECTRUM_BINS / 2;
            int b0 = (int)bin;
            const float t = (float)(bin - b0);
            if (b0 < 0) b0 = 0;
            if (b0 > FM_SPECTRUM_BINS - 2) b0 = FM_SPECTRUM_BINS - 2;
            const float v = s_hf_bins[b0] * (1.0f - t) + s_hf_bins[b0 + 1] * t;
            col[c] = v;
            if (v < lo_v) lo_v = v;
            if (v > hi_v) hi_v = v;
        }
        if (hi_v < lo_v + 0.15f) hi_v = lo_v + 0.15f;
        const int dots = rows * 4;
        for (int x = 0; x < w && 2 * x + 1 < n; ++x) {
            const double off = -half + (x + 0.5) * hz_per_col;
            const bool in = off >= lo && off <= hi;
            const uint8_t ink = TUI_ATTR(in ? TUI_GREEN | TUI_BRIGHT : TUI_CYAN, TUI_BLACK);
            int lv[2];
            for (int k = 0; k < 2; ++k) {
                lv[k] = (int)((col[2 * x + k] - lo_v) / (hi_v - lo_v) * (float)dots + 0.5f);
                if (lv[k] < 1) lv[k] = 1;   /* the floor is a line of dots */
            }
            for (int row = 0; row < rows; ++row) {
                const int below = (rows - 1 - row) * 4;
                int bits = 0;
                for (int k = 0; k < 2; ++k) {
                    int hgt = lv[k] - below;
                    if (hgt > 4) hgt = 4;
                    if (hgt > 0) bits |= ls_braille_column((unsigned)hgt, k);
                }
                if (bits) ls_braille_put(sf, r, x0 + x, y0 + row, (int16_t)(0x2800 | bits), ink);
            }
        }
    } else if (rows > 0) {
        tui_put_str(sf, r, x0, y0 + rows - 1, ls_tr("no spectrum yet"), LS_ATTR_DIM);
    }

    /* The skirt, the carrier and the clarifier. */
    int run0 = -1, run1 = -1;
    for (int x = 0; x < w; ++x) {
        const double a = -half + x * hz_per_col, b = a + hz_per_col;
        char g = '_';
        uint8_t c = TUI_ATTR(TUI_BLACK | TUI_BRIGHT, TUI_BLACK);
        if (b > lo && a < hi) {
            const bool left = a <= lo, right = b >= hi;
            g = left ? '/' : right ? '\\' : '=';
            c = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
            if (!left && !right) { if (run0 < 0) run0 = x; run1 = x; }
        }
        if (a <= 0.0 && b > 0.0) { g = '|'; c = TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK); }
        else if (st->rit_hz && a <= st->rit_hz && b > st->rit_hz) {
            g = '^'; c = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
        }
        tui_put_char(sf, r, x0 + x, base, g, c);
    }
    /* The demod inside its own passband when it fits, else under the scale. */
    char name[16];
    snprintf(name, sizeof(name), "%s", fm_hf_demod_label(st->demod));
    const int nl = (int)strlen(name);
    const int dial_x = (int)(half / hz_per_col);
    if (run0 >= 0 && run1 - run0 + 1 >= nl + 2) {
        int nx = run0 + (run1 - run0 + 1 - nl) / 2;
        if (nx <= dial_x && nx + nl > dial_x) nx = dial_x + 1 <= run1 - nl ? dial_x + 1 : nx;
        tui_put_str(sf, r, x0 + nx, base, name, TUI_ATTR(TUI_BLACK, TUI_YELLOW));
    }
    if (w >= 30) {
        char edge[8];
        snprintf(edge, sizeof(edge), "-%dk", half / 1000);
        tui_put_str(sf, r, x0, base, edge, TUI_ATTR(TUI_BLUE | TUI_BRIGHT, TUI_BLACK));
        edge[0] = '+';
        tui_put_str(sf, r, x0 + w - (int)strlen(edge), base, edge,
                    TUI_ATTR(TUI_BLUE | TUI_BRIGHT, TUI_BLACK));
    }
}

/* Portrait stacks the glass; landscape has width to spare and no height, so
   the readouts stand in a column beside the figures instead of under them. */
static void draw_lcd(tui_surface *sf, tui_rect r)
{
    const bool wide = ls_tui_is_wide();
    const char *receiver = s_lora_pager ? NULL : receiver_text();
    const sig_kind_t kind = sig_kind();
    const bool open = kind != SIG_NONE && FM.squelch_open;
    const uint8_t good = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t val  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim  = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    const uint8_t warn = TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK);
    const uint8_t key  = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    char buf[32];

    if (r.w < 20 || r.h < 5) { draw_vfo(sf, r); return; }
    ls_radio_glass_bezel(sf, r, s_freq_keyboard ? "ENTER FREQUENCY" : "RECEIVER");
    const int x0 = r.x + 2, iw = r.w - 4;
    int y = r.y + 1;
    const int last = r.y + r.h - 2;

    if (receiver) s_glass.hold = 0.0f;
    if (kind != s_sig_kind) { s_glass.hold = 0.0f; s_sig_kind = kind; }
    ls_radio_glass_sample(&s_glass, receiver || kind == SIG_NONE ? 0.0f : sig_level(kind));

    lcd_rail(sf, r, x0, y, iw);
    y++;

    /* The carrier, as big as the glass will carry it. While a frequency is
       being typed the figures are the entry - that is what the radio this
       copies does - and the carrier it is still listening to moves to the
       line underneath, which also carries the step and the lock. */
    const uint32_t hz = FM.freq_hz;
    const int fig_w = wide ? (iw * 2) / 3 : iw;
    y = ls_radio_glass_frequency(&s_glass, sf, r, y, hz,
                                 s_freq_keyboard ? s_freq_entry : NULL, s_blink, wide);
    ls_radio_glass_wipe(&s_glass, sf, r, r.y + 2, y, (int)FM.mode);

    /* The readouts: beside the figures in landscape, under them otherwise. */
    int rx = wide ? x0 + fig_w + 2 : x0;
    int ry = wide ? r.y + 2 : y;
    const int rw = wide ? x0 + iw - rx : iw;
    if (rw >= 16) {
        if (s_freq_keyboard) {
            snprintf(buf, sizeof(buf), "%u.%04u MHz",
                     (unsigned)(hz / 1000000u), (unsigned)((hz % 1000000u) / 100u));
            tui_put_str(sf, r, rx, ry, ls_tr("RX"), key);
            tui_put_str(sf, r, rx + 3, ry, buf, val);
        } else if (hf_on()) {
            /* HF reads to the 10 Hz: the figures stop at 100 Hz. */
            tune_step_label(buf, sizeof(buf));
            int x = kv(sf, r, rx, ry, "STEP", buf, val);
            snprintf(buf, sizeof(buf), "%.2f kHz", FM.freq_hz / 1e3);
            if (x + 16 <= rx + rw) x = kv(sf, r, x, ry, "DIAL", buf, val);
            if (x + 10 <= rx + rw && lakeshark_fm_frequency_locked())
                kv(sf, r, x, ry, "LOCK", "ON", good);
        } else {
            tune_step_label(buf, sizeof(buf));
            int x = kv(sf, r, rx, ry, "STEP", buf, val);
            const uint32_t lock_hz = lakeshark_fm_frequency_lock_hz();
            if (lock_hz) snprintf(buf, sizeof(buf), "%.4f", lock_hz / 1e6);
            else snprintf(buf, sizeof(buf), "FREE");
            if (x + 12 <= rx + rw) x = kv(sf, r, x, ry, "LOCK", buf,
                                          lock_hz ? good : dim);
            /* The carrier A swaps with, which until now only the scanner's
               panel ever showed; a handheld prints both. */
            snprintf(buf, sizeof(buf), "%u.%04u", (unsigned)(s_standby / 1000000u),
                     (unsigned)((s_standby % 1000000u) / 100u));
            if (x + 13 <= rx + rw) kv(sf, r, x, ry, "B", buf, dim);
        }
        ry++;

        /* CARRIER blinks while it is open, so a burst and a held carrier are
           not the same steady word. */
        const char *carrier = receiver ? receiver
                            : kind == SIG_NONE ? "--"
                            : open ? "OPEN" : "closed";
        /* HF: the meter and the scope say what CARRIER would, so the row goes
           to the passband scope - unless the receiver has something wrong to
           report, which always gets its row. */
        if (!hf_on() || receiver) {
            int x = kv(sf, r, rx, ry, "CARRIER", carrier,
                       receiver ? warn : open ? good : dim);
            if (open && ((s_blink / 12) & 1))
                tui_put_char(sf, r, x - 2, ry, LS_TUI_SHADE_FULL, good);
            if (!wide && !hf_on()) {
                snprintf(buf, sizeof(buf), "%lu B/s", (unsigned long)FM.iq_bytes_sec);
                kv(sf, r, x, ry, "IQ", buf, val);
            }
            ry++;
        }
        if (hf_on() && ry <= last) { draw_hf_line(sf, r, rx, ry, rw); ry++; }
        if (ry <= last) {
            if (wide && !hf_on()) {
                snprintf(buf, sizeof(buf), "%lu B/s", (unsigned long)FM.iq_bytes_sec);
                kv(sf, r, rx, ry, "IQ", buf, val);
                ry++;
            }
        }
        /* A row only for what there is to say: with no tone gate and no
           pager running this line would be blank, and a blank line is the
           hole the trace below would rather have. */
        if (ry <= last && draw_extra(sf, r, rx, ry, false) > rx) ry++;
        if (!wide) y = ry;
    }

    if (s_freq_invalid) {
        const int n = (int)strlen("INVALID RANGE");
        tui_put_str(sf, r, x0 + iw - n, wide ? last : r.y + 1, ls_tr("INVALID RANGE"),
                    ((s_blink / 8) & 1) ? warn : LS_ATTR_DIM);
    }

    if (FM.lr_iq_active && y <= last) {
        LS_SNPRINTF(t_scr_fm_lr_iq_lost_lu_2, buf, sizeof(buf), "LR IQ lost %lu gaps %lu",
                 (unsigned long)FM.lr_lost_pairs, (unsigned long)FM.lr_gaps);
        tui_put_str(sf, r, x0, y++, buf, dim);
    }

    /* The meter last, against the bottom rule, where the eye expects an
       instrument on a radio, with the trace in whatever is left between the
       readouts and it. */
    /* HF gives the meter's second row to the scope above it. */
    const int meter_rows = (last - y) >= 5 && !hf_on() ? 2 : 1;
    int meter_y = last - meter_rows;
    if (meter_y < y) meter_y = y;
    /* One row of breathing room before the meter below, none before this:
       `y` is the row right after the readouts, and the trace starts there
       rather than leaving it as a blank line nothing else claims. */
    int trace_h = meter_y - y - 1;
    /* The desk's glass is tall: the activity trace keeps more history rows. */
    if (trace_h > (ls_tui_compact() ? 16 : 6)) trace_h = ls_tui_compact() ? 16 : 6;
    /* The scope takes every row down to the meter: on HF it is the
       instrument, and landscape has only two to give it. */
    const int scope_h = meter_y - y > 7 ? 7 : meter_y - y;
    if (hf_on() && scope_h >= 2 && iw > 26) {
        int tw = wide ? fig_w : iw;
        if (tw > 96) tw = 96;
        draw_hf_scope(sf, r, x0, y, tw, scope_h);
    } else if (trace_h >= 2 && iw > 26) {
        int tw = (wide ? fig_w : iw) - 10;
        if (tw > LS_RADIO_TRACE_N) tw = LS_RADIO_TRACE_N;
        const int ty = y;
        tui_put_str(sf, r, x0, ty, ls_tr("ACT"),
                    TUI_ATTR(TUI_CYAN, TUI_BLACK));
        for (int x = 0; x < tw; ++x)
            tui_put_char(sf, r, x0 + 4 + x, ty + trace_h - 1, '-', LS_ATTR_DIM);
        ls_radio_glass_trace(&s_glass, sf, r, x0 + 4, ty, tw, trace_h,
                             lakeshark_fm_squelch_get() * 0.01f);
    }
    if (meter_y + meter_rows <= last)
        /* Landscape's tone/IQ readouts own the right column, including
           the rows shared with the meter. Never paint across that column. */
        ls_radio_glass_meter(&s_glass, sf, r, x0, meter_y, wide ? fig_w : iw, meter_rows,
                             kind == SIG_NONE ? 0.0f : sig_level(kind),
                             kind != SIG_NONE, open, meter_y + meter_rows <= last,
                             lakeshark_fm_squelch_get() * 0.01f, "SQL");
}

/* ========================================================== THE DECK === */

/* THE CONTROLS, AT THE SIZE OF A CONTROL.

   One card a setting: its name and its value on one line at the top left,
   where three cards read as a column of aligned readouts, and under them a
   bar that shows where the value sits in its range with the minus and plus
   keys at the end of it. The keys keep the dithered field every other button
   on this firmware has - a key with no surface is not a key - and they are
   five columns by however many rows the card has, which is a thumb and not a
   slab. Nothing floats: the bar fills the width between the label and the
   keys, so there is no empty middle for the eye to fall into. */
static void draw_step_card(tui_surface *sf, tui_rect b, int i, uint8_t hue)
{
    static const char *const NAME[N_STEP] = { "VOLUME", "GAIN dB", "SQUELCH" };
    static const int ITEM[N_STEP] = { 1, 2, 3 };
    char value[16];
    step_value(i, value, sizeof(value));
    const ls_quick_t *q = &QUICK[ITEM[i]];
    ls_radio_card_t card = {
        .label = NAME[i], .value = value, .hue = hue,
        .live = i == 2 ? mode_has_squelch() : i == 1 ? fm_gain_live() : true,
        .frac = i == 0 ? audio_volume_get() / 100.0f
              : i == 1 ? FM.gain_tenths / 496.0f : lakeshark_fm_squelch_get() / 100.0f,
        .key_down = q->key_down, .key_up = q->key,
        .flash_down = s_step_flash == i && !s_step_flash_up && s_step_flash_ttl > 0,
        .flash_up = s_step_flash == i && s_step_flash_up && s_step_flash_ttl > 0
    };
    ls_radio_vfo_card_draw(sf, b, &card);
    s_step_down[i] = card.down; s_step_up[i] = card.up;
    s_step_value[i] = card.value_rect; s_step_bar[i] = card.bar;
}

/* The keypad that takes the deck's place. Keys carry the same dithered
   surface as every other button, and the figure on them is drawn at the size
   of the key - a digit the width of a fingertip, not a letter lost in a
   slab - with the plain character kept in the corner the way the bar keeps
   its shortcut badges. */
static void draw_key(tui_surface *sf, tui_rect k, const char *cap, char badge,
                     uint8_t hue, bool strong)
{
    ls_freq_key_draw(sf, k, cap, badge, hue, strong);
}

/* The relocated keyboard/keypad toggle: a key like any other, filled and
   bordered the same way, in its own colour so it is not mistaken for a view
   tab or a control. CONTROLS, heavier-filled, is the active state - the way
   back, the same visual weight the keypad's own TUNE key uses. */
static void draw_kbd_toggle(tui_surface *sf, tui_rect r)
{
    if (r.w < 4 || r.h < 1) { s_kbd_btn = tui_rect_make(0, -1, 0, 0); return; }
    s_kbd_btn = r;
    const bool active = s_freq_keyboard;
    const char *label = active ? "CONTROLS" : r.w >= 10 ? "KEYBOARD" : "KEYPAD";
    draw_key(sf, r, label, 0, active ? TUI_GREEN : TUI_MAGENTA, active);
}

static void draw_keypad(tui_surface *sf, tui_rect area)
{
    ls_freq_pad_draw(sf, area, s_freq_entry, s_freq_invalid, s_blink,
                     s_freq_key, s_freq_action);
}

static uint32_t tune_frequency(void) { return FM.freq_hz; }
/* Not EXT_RAM_BSS_ATTR: that section is zeroed at boot and drops the callbacks. */
static ls_radio_tune_t s_tune = {
    .step = tune_step_hz, .frequency = tune_frequency, .cycle = cycle_tune_step,
    .tune = tune_step, .seek = seek_tune,
};

static void draw_quick_controls(tui_surface *sf, tui_rect area)
{
    static const uint8_t HUE[N_STEP] = { TUI_GREEN, TUI_MAGENTA, TUI_CYAN };
    if (s_step_flash_ttl > 0 && --s_step_flash_ttl == 0) s_step_flash = -1;
    memset(s_step_box, 0, sizeof(s_step_box));
    memset(s_step_down, 0, sizeof(s_step_down));
    memset(s_step_up, 0, sizeof(s_step_up));
    memset(s_step_value, 0, sizeof(s_step_value));
    memset(s_step_bar, 0, sizeof(s_step_bar));
    memset(s_tune.keys, 0, sizeof(s_tune.keys));
    if (s_freq_keyboard) { draw_keypad(sf, area); return; }
    /* Five-row keys leave three filled rows between their rules. Shorten
       the lower cards' bar travel to pay for the portrait TUNE enclosure. */
    const bool compact = ls_tui_compact();
    const int tune_h = compact ? 1 : ls_tui_is_wide() ? 4 : 7;
    ls_radio_tune_draw(&s_tune, sf, tui_rect_make(area.x, area.y, area.w, tune_h));
    area.y += tune_h + (compact ? 0 : 1); area.h -= tune_h + (compact ? 0 : 1);
    memset(s_freq_key, 0, sizeof(s_freq_key));
    memset(s_freq_action, 0, sizeof(s_freq_action));

    if (ls_tui_is_wide() || compact) {
        int h = area.h;
        if (h > 9) h = 9;
        if (compact) h = 1;            /* desk: three one-row sliders */
        const int y = area.y + (area.h - h) / 2;
        for (int i = 0; i < N_STEP; ++i) {
            const int x0 = area.x + area.w * i / N_STEP;
            const int x1 = area.x + area.w * (i + 1) / N_STEP;
            s_step_box[i] = tui_rect_make(x0, y, x1 - x0 - 1, h);
            draw_step_card(sf, s_step_box[i], i, HUE[i]);
        }
        return;
    }

    /* A pane too short for cards keeps the one-line controls: a value, and a
       minus and plus beside it, which still sets everything the cards do. */
    if (area.h < 15) {
        static const char *const NAME[N_STEP] = { "VOLUME", "GAIN", "SQUELCH" };
        for (int i = 0; i < N_STEP; ++i) {
            const bool live = i == 2 ? mode_has_squelch() : i == 1 ? fm_gain_live() : true;
            char v[16]; step_value(i, v, sizeof(v));
            const int y = area.y + i * area.h / N_STEP;
            /* The slot has rows this single line never uses; the tap target
               takes all of them instead of leaving the rest dead. */
            const int y1 = area.y + (i + 1) * area.h / N_STEP;
            const int slot_h = y1 > y ? y1 - y : 1;
            const int right = area.x + area.w - 2;
            s_step_box[i] = tui_rect_make(area.x + 1, y, area.w - 2, slot_h);
            s_step_down[i] = tui_rect_make(right - 7, y, 3, slot_h);
            s_step_up[i] = tui_rect_make(right - 3, y, 3, slot_h);
            s_step_value[i] = tui_rect_make(area.x + 1, y, right - 8 - area.x, slot_h);
            const bool flash_dn = live && s_step_flash == i && !s_step_flash_up && s_step_flash_ttl > 0;
            const bool flash_up = live && s_step_flash == i && s_step_flash_up && s_step_flash_ttl > 0;
            char edit[20];
            snprintf(edit, sizeof(edit), "[%s]", v);
            tui_put_str(sf, area, area.x + 1, y, NAME[i],
                        live ? TUI_ATTR(HUE[i] | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
            tui_put_str(sf, area, area.x + 2 + (int)strlen(NAME[i]), y, edit,
                        live ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
            tui_put_str(sf, area, s_step_down[i].x, y, ls_tr("[-]"),
                        flash_dn ? TUI_ATTR(TUI_BLACK, TUI_WHITE | TUI_BRIGHT)
                                 : live ? TUI_ATTR(HUE[i] | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
            tui_put_str(sf, area, s_step_up[i].x, y, ls_tr("[+]"),
                        flash_up ? TUI_ATTR(TUI_BLACK, TUI_WHITE | TUI_BRIGHT)
                                 : live ? TUI_ATTR(HUE[i] | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
        }
        return;
    }
    /* Spend the separating blank rows before reducing a key's filled face:
       eight-row cards retain five-row +/- keys on the shorter TFT. */
    const int gap = area.h >= N_STEP * 8 + (N_STEP - 1) ? 1 : 0;
    int card_h = (area.h - gap * (N_STEP - 1)) / N_STEP;
    if (card_h > 9) card_h = 9;
    const int group_h = card_h * N_STEP + gap * (N_STEP - 1);
    const int group_y = area.y + (area.h - group_h) / 2;
    for (int i = 0; i < N_STEP; ++i) {
        s_step_box[i] = tui_rect_make(area.x + 1, group_y + i * (card_h + gap),
                                      area.w - 2, card_h);
        draw_step_card(sf, s_step_box[i], i, HUE[i]);
    }
}

static bool step_touch(int col, int row)
{
    if (ls_radio_tune_touch(&s_tune, col, row)) return true;
    for (int i = 0; i < N_STEP; i++) {
        const tui_rect b = s_step_box[i];
        if (b.h <= 0 || row < b.y || row >= b.y + b.h ||
            col < b.x || col >= b.x + b.w) continue;
        /* GAIN on LR2021 and SQUELCH in a mode with none: the card already
           reads as disabled and nothing tapped on it would reach the
           receiver (fm_gain_live(), mode_has_squelch()), so the whole card
           is a no-op and not a disguised one. */
        if (i == 1 && !fm_gain_live()) return true;
        if (i == 2 && !mode_has_squelch()) return true;
        const int part = ls_radio_vfo_card_hit(s_step_down[i], s_step_up[i],
                             s_step_value[i], s_step_bar[i], col, row);
        if (part == LS_RADIO_PART_DOWN) {
            ls_quick_t item = QUICK[i + 1]; item.delta = -item.delta;
            ls_quick_fire(&item, ls_quick_grant_builtin());
            s_step_flash = i; s_step_flash_up = false; s_step_flash_ttl = STEP_FLASH_FRAMES;
            return true;
        }
        if (part == LS_RADIO_PART_UP) {
            ls_quick_fire(&QUICK[i + 1], ls_quick_grant_builtin());
            s_step_flash = i; s_step_flash_up = true; s_step_flash_ttl = STEP_FLASH_FRAMES;
            return true;
        }
        if (part == LS_RADIO_PART_BAR) {
            const float f = ls_radio_vfo_card_bar_frac(s_step_bar[i], col);
            if (i == 0) set_volume(f * 100.0);
            else if (i == 1) set_gain(f * 49.6);
            else set_squelch(f * 100.0);
            return true;
        }
        if (part == LS_RADIO_PART_VALUE) {
            if (i == 0) open_volume_entry();
            else if (i == 1) open_gain_entry();
            else open_squelch_entry();
            return true;
        }
    }
    return false;
}

/* Turning to SWEEP starts the sweep, which is what its own idle text always said it did. */

static void show_page(int i)
{
    s_pv.open=false;
    s_freq_keyboard = false;
    s_freq_invalid = false;
    /* A tab that is not on offer does nothing at all. Falling back to VFO
       would stop a running sweep, which is the same reaching-over this
       change exists to remove. */
    if (i == 1 && !pager_mode() && !own_page()) return;
    if (i==3) {s_details=false;ls_wf_source_release();return;}
    s_page = i;
    s_last_mode = (int)FM.mode;
    if (i == 2) {
        /* OPENING THE SPECTRUM DOES NOT START A SWEEP.

           It used to: entering this page took a receiver that was listening
           to something and put it into SCAN, so asking to SEE the signal
           stopped you hearing it. That is the opposite of what a waterfall
           is for, and it is not even the only way in - this page carries a
           RUN button (w) and a MODE button (e) that do exactly this, on
           purpose, when it is wanted.

           So the page now opens on whatever the receiver is already doing:
           the live FFT of the tuned channel while listening, or the sweep
           if a sweep was already running. Starting one is a button press,
           which is where a decision like that belongs. */
    } else {
        ls_wf_source_release();
        if (i == 0 && FM.mode == FM_MODE_SCAN) { ls_wf_fm_sweep(false); s_mode_requested = true; }
        /* PAGER no longer reaches over and changes the receiver. A tab is a
           view; picking what the radio does is what MODE is for, and this
           silently putting a listening receiver into POCSAG was one of the
           ways the speaker ended up carrying pager bursts instead of speech.
           The tab only offers itself when a pager mode is already running. */
    }
}

/* Called by the UI task when the control head chooses FM or POCSAG. */
void ls_scr_fm_show_page(int page)
{
    if (page >= 0 && page < N_PAGES) {s_details=(page!=3);show_page(page);}
}

static void radio_view(void)
{
    memset(&s_view,0,sizeof(s_view));
    s_view.fm=true;
    s_view.tone_required=FM.tone_required;
    s_view.frequency=FM.freq_hz;
    s_view.standby=s_standby;
    s_view.mode=FM.mode==FM_MODE_LISTEN?"NFM":fm_mode_label(FM.mode);
    s_view.power=FM.iq_level;
    {
        const sig_kind_t kind = sig_kind();
    s_view.volume = audio_volume_get();
    s_view.set_volume_pct = panel_set_volume_pct;
    s_view.set_squelch_pct = panel_set_squelch_pct;
        s_view.signal = kind == SIG_NONE ? 0.0f : sig_level(kind);
        s_view.gate = (float)lakeshark_fm_squelch_get() * 0.01f;
        s_view.squelch_open = kind != SIG_NONE && FM.squelch_open;
        s_view.has_squelch = kind != SIG_NONE;
    }
    fm_get_receiver_status(&s_view.receiver);
    char sql[12];
    squelch_text(sql, sizeof(sql));
    snprintf(s_view.detail[0],64,"CARRIER %s",s_view.receiver.receiver_streaming?(!s_view.has_squelch?"--":FM.squelch_open?"OPEN":"CLOSED"):"OFFLINE");
    LS_SNPRINTF(t_scr_fm_sql_s_gain_1f, s_view.detail[1],64,"SQL %s  GAIN %.1f dB",sql,FM.gain_tenths/10.0);
    snprintf(s_view.detail[2],64,"VOL %d  STEP %.1fk",audio_volume_get(),tune_step_hz()/1000.0);
    LS_SNPRINTF(t_scr_fm_s_more_mode_waterfall, s_view.detail[3],64,"%s / MORE: mode, waterfall, pager",
             lakeshark_fm_frequency_locked() ? "FREQ LOCKED" : "FREQ FREE");
    if (FM.lr_iq_active) {
        snprintf(s_view.detail[1],64,"LR IQ %s / SQL %s", FM.lr_channel_hz == 12500 ? "12.5k" : "25k", sql);
        LS_SNPRINTF(t_scr_fm_lost_lu_gaps_lu, s_view.detail[3],64,"Lost >=%lu gaps %lu audio %lu",
            (unsigned long)FM.lr_lost_pairs, (unsigned long)FM.lr_gaps,
            (unsigned long)FM.lr_audio_drops);
    }
    if (FM.mode == FM_MODE_LISTEN && FM.tone_show) {
        char tone[20];
        fm_tone_label(FM.tone_detected.selection, tone, sizeof(tone));
        snprintf(s_view.detail[2],64,"TONE %s %c", FM.tone_detected.selection ? tone : "--",
                 ls_motion_pip(FM.tone_detected.selection != 0));
    }
}
static void set_squelch(double value)
{
    if(!(value>=0 && value<=100)) return;
    ls_args_t args={.n=1}; ls_val_t out;
    args.v[0].kind=LS_VAL_FLOAT; args.v[0].f=(float)value;
    ls_action_call("fm.sql",&args,&out,ls_quick_grant_builtin());
}
static void radio_action(char c)
{
    if(c=='M') {s_details=true;return;}
    if(c=='Q') {ls_numpad_open("SQUELCH",squelch_hint(),lakeshark_fm_squelch_get(),set_squelch);return;}
    if(c=='A') {
        scan_engine_stop();
        uint32_t previous=FM.freq_hz;
        ls_args_t args={0}; ls_val_t out;
        args.n=1; args.v[0].kind=LS_VAL_INT; args.v[0].i=s_standby;
        /* Same order as the keypad: the swap is the decision, so release the
           lock, make the move, then hold whichever carrier we landed on. */
        lakeshark_fm_frequency_lock(false);
        if(ls_action_call("fm.freq_hz",&args,&out,ls_quick_grant_builtin())==LS_ACT_OK) {
            s_standby=previous;
            lakeshark_fm_frequency_lock(true);
        }
        return;
    }
    if(c=='['||c==']') {scan_engine_stop();tune_step(c=='['?-1:1);return;}
    if(c=='T') scan_engine_stop();
    /* GAIN's nudge keys: a no-op on LR2021, same as the card's own +/-. */
    if((c=='u'||c=='U'||c=='j'||c=='J') && !fm_gain_live()) return;
    if(c) ls_quick_key(c,QUICK,N_QUICK,ls_quick_grant_builtin(),NULL);
}

/* Keep receiver identity, tuned frequency and signal adjacent to the analysis
   canvas. The VFO itself remains a dedicated quick-control page. */
static void draw_spectrum_page(tui_surface *sf, tui_rect body)
{
    const int vfo_h = ls_tui_is_wide() ? STATUS_ROWS_WIDE + (FM.lr_iq_active ? 1 : 0) : STATUS_ROWS;
    if (body.h < vfo_h + 6) {
        draw_sweep(sf, body);
        return;
    }
    draw_vfo(sf, tui_rect_make(body.x, body.y, body.w, vfo_h));
    /* The receiver identity and tuned signal stay attached to the plot; the
       spectrum remains the only page with the full analysis canvas. */
    draw_sweep(sf, tui_rect_make(body.x, body.y + vfo_h, body.w,
                                 body.h - vfo_h));
}

/* The view tabs as they stand: which pages this mode has, and their names. */
static int build_tabs(ls_btn_t *b)
{
    int nb = 0;
    for (int i = 0; i < N_PAGES; i++) {
        if (i == 1 && !pager_mode() && !own_page()) continue;   /* PAGER decodes or it hides */
        b[nb] = PAGES[i];
        if (i == 1 && own_page()) b[nb].label = own_tab();
        if (i == 1 && FM.mode == FM_MODE_ACARS) b[nb].label = "MESSAGES";
        if (i == 1 && FM.mode == FM_MODE_SAME) b[nb].label = "ALERTS";
        if (i == 1 && FM.mode == FM_MODE_APRS) b[nb].label = "STATIONS";
        if (i == 1 && FM.mode == FM_MODE_AIS) b[nb].label = "VESSELS";
        b[nb].on = (i == s_page);
        s_tab_page[nb] = i;
        nb++;
    }
    return nb;
}

static int tab_bar_width(void)
{
    ls_btn_t b[N_PAGES];
    return ls_btn_bar_width(b, build_tabs(b));
}

static void draw(tui_surface *sf, tui_rect area)
{
    ls_rsel_fm_context(FM.freq_hz, fm_lr_mode(FM.mode));
    if (ls_calls_active()) {
        LS_SNPUTS(t_scr_fm_up_down_calls_p, s_hint, sizeof(s_hint), "UP/DOWN calls  P play  S stop  D delete  O options  ESC back");
        ls_wf_source_release(); ls_calls_draw(sf, area); return;
    }
    lora_pager_sync();
    snprintf(s_hint,sizeof(s_hint),"%s%s",s_details?
             (ls_tui_is_wide() && s_page == 2 ?
              "LEFT/RIGHT select  SPACE tune  E mode  T freq  K lock  R radio  0 scanner" :
              ls_tui_is_wide() && s_page == 0 ?
              "LEFT/RIGHT step  ,/. seek  / step size  T freq  0 scanner" :
              "E mode  T tune  K lock  N band  W sweep  R radio  0 scanner"):
             "LEFT/RIGHT tune  UP/DOWN controls  M details  R radio",
             ls_opt_count(fm_options()) ? "  O options" : "");
    if (s_details && s_page == 0) LS_SNPUTS(t_scr_fm_left_right_step_seek, s_hint, sizeof(s_hint), "LEFT/RIGHT step  ,/. seek  / STEP  T freq  O options");
    if (s_details && s_page == 0 && scan_engine_active())
        LS_SNPUTS(t_scr_fm_n_next_k_skip, s_hint, sizeof(s_hint), "N next K skip H hold/resume X stop ,/. seek / STEP");
    if (FM.lr_iq_active && s_page != 0) LS_SNPRINTF(t_scr_fm_lr_iq_lost_lu_3, s_hint, sizeof(s_hint), "LR IQ lost >=%lu / gaps %lu / audio drops %lu",
        (unsigned long)FM.lr_lost_pairs, (unsigned long)FM.lr_gaps,
        (unsigned long)FM.lr_audio_drops);
    if (ls_rsel_saved(LS_RSEL_FM) == LS_RSEL_LORA &&
        !ls_lora_rx_range_ok(ls_lora_caps(), FM.freq_hz, FM.freq_hz))
        LS_SNPUTS(t_scr_fm_lr2021_104_2740_mhz, s_hint, sizeof(s_hint), "LR2021: 104-2740 MHz; below 104 MHz needs an SDR");
    if (!s_details) { radio_view(); ls_radio_panel_draw(&s_radio,&s_view,sf,area); return; }
    s_blink++;
    if (s_last_mode != (int)FM.mode) {
        s_last_mode = (int)FM.mode;
        /* Ours, arriving late: honour the page the user picked. Anyone
           else's - console, Flipper, the control head - still moves it. */
        if (s_mode_requested) s_mode_requested = false;
        else s_page = mode_page(FM.mode);
    }
    /* The desk lays out as landscape does, at one row a bar. */
    const bool compact = ls_tui_compact();
    const bool wide = ls_tui_is_wide() || compact;
    /* Three rows in portrait: a two row bar with one line of text has no middle row to put it on. */

    const int bar_h = compact ? 1 : wide ? 3 : 5;
    const int bar_pad = 0;

    /* Portrait puts the page bar at the bottom, where the thumb is.

       Right at the top in landscape, where the strip is a legend for the
       function keys and the keyboard is under the screen. Wrong in portrait,
       where the hand holding the thing reaches the bottom third and the top
       of a 1232 pixel panel wants a second hand. */
    tui_rect body;
    if (wide) {
        s_bar = tui_rect_make(area.x, area.y, area.w, bar_h);
        body = tui_rect_make(area.x, area.y + bar_h, area.w, area.h - bar_h);
    } else {
        s_bar = tui_rect_make(area.x, area.y + area.h - bar_h - bar_pad,
                              area.w, bar_h);
        body = tui_rect_make(area.x, area.y, area.w,
                             area.h - bar_h - bar_pad);
    }


    if (body.h <= 0) return;
    /* Landscape keeps the compact bar even without a keyboard: the VFO
       page's waterfall is only a few rows tall there already. */
    const bool vfo = s_page == 0;
    int control_rows = ls_btn_raised_height(body, ctl_count(vfo));
    /* Portrait uses two compact rows on both receiver pages. The spectrum
       keeps volume/squelch here while the VFO groups its quick controls below. */
    if (!wide && body.h >= 40) control_rows = 8;
    if (wide && control_rows > 3) control_rows = 3;
    /* Desk: the view tabs and the controls share one toolbar row when the
       window is wide enough for both, else the controls take the next. */
    int tabs_w = 0;
    tui_rect ctl_rect = tui_rect_make(area.x, wide ? area.y + bar_h : area.y, area.w, control_rows);
    if (compact) {
        tabs_w = tab_bar_width();
        if (area.w - tabs_w - 3 >= 100) {
            control_rows = 0;
            s_bar.w = tabs_w;
            ctl_rect = tui_rect_make(area.x + tabs_w + 3, area.y, area.w - tabs_w - 3, 1);
            ls_box_put(sf, area, area.x + tabs_w + 1, area.y, LS_BOX_V, TUI_ATTR(LS_FAINT_FG, TUI_BLACK));
        } else {
            control_rows = 1;
            ctl_rect = tui_rect_make(area.x, area.y + 1, area.w, 1);
        }
    }
    body.y += control_rows;
    body.h -= control_rows;

    /* THE SPLIT.

       The VFO page is glass over a deck, and the two share the body. The
       glass wants enough rows for the block figures, a line of readouts and
       the meter; the deck wants enough for three cards or for the keypad,
       and the keypad asks for more, so entering a frequency hands it rows
       the glass can spare. Below the point where both fit, the glass takes
       the pane and the controls stay on the bar above - which is the only
       posture where nothing is lost by it. */
    memset(s_step_box, 0, sizeof(s_step_box));
    s_kbd_btn = tui_rect_make(0, -1, 0, 0);
    if (vfo) {
        /* Short landscape panes still need the deck: seven glass rows
           suffice when the meter stays in the frequency column. */
        const int lcd_min  = compact ? 5 : wide ? 7 : 12;
        /* Desk: a tune row and a slider row, or the frequency field. */
        const int deck_min = compact ? 2 : wide ? 11 : 23;
        int lcd_h = compact ? body.h - deck_min
                  : wide ? (body.h < 19 ? 7 : 8)
                         : (s_freq_keyboard ? 14 : body.h * 7 / 20);
        if (!wide && lcd_h > 24) lcd_h = 24;
        if (lcd_h > body.h) lcd_h = body.h;
        if (body.h - lcd_h < deck_min) {
            lcd_h = body.h >= lcd_min + deck_min ? body.h - deck_min : body.h;
        }
        if (body.h - lcd_h >= deck_min) {
            s_quick_rect = tui_rect_make(body.x, body.y + lcd_h,
                                         body.w, body.h - lcd_h);
            body.h = lcd_h;
            /* Landscape's view tabs sit at the top (see below); its deck is
               the actual bottom of the screen, so the toggle's corner comes
               out of the deck instead of the tab strip. */
            if (compact) {
                /* The toggle is a key at the right end of the tune row. */
                const int kw = 14;
                if (s_quick_rect.w - kw >= 60)
                    s_kbd_btn = tui_rect_make(s_quick_rect.x + s_quick_rect.w - kw,
                                              s_quick_rect.y, kw, 1);
            } else if (wide) {
                int kw = s_quick_rect.w / 8;
                if (kw < 10) kw = 10;
                if (kw > 14) kw = 14;
                if (s_quick_rect.w - kw >= 40) {
                    s_kbd_btn = tui_rect_make(s_quick_rect.x + s_quick_rect.w - kw,
                                              s_quick_rect.y, kw, s_quick_rect.h);
                    s_quick_rect.w -= kw;
                }
            }
        } else s_quick_rect = tui_rect_make(0, -1, 0, 0);
    } else s_quick_rect = tui_rect_make(0, -1, 0, 0);

    if (s_page == 1 && FM.mode == FM_MODE_ACARS) draw_acars(sf, body);
    else if (s_page == 1 && FM.mode == FM_MODE_AIS) fm_ais_draw(sf, body);
    else if (s_page == 1 && FM.mode == FM_MODE_APRS) fm_aprs_draw(sf, body);
    else if (s_page == 1 && FM.mode == FM_MODE_SAME) fm_same_draw(sf, body);
    else if (s_page == 1 && own_page()) own_draw(sf, body);
    else if (s_page == 1) draw_pages(sf, body);
    else if (s_page == 2) draw_spectrum_page(sf, body);
    else draw_lcd(sf, body);
    /* The picture's colours belong to its own cells: off its page the grid
       has the theme's back. */
    if (!(s_page == 1 && sstv_page())) fm_sstv_view_idle();
    if (s_quick_rect.h > 0) draw_quick_controls(sf, s_quick_rect);
    if (wide && s_kbd_btn.h > 0) draw_kbd_toggle(sf, s_kbd_btn);

    /* Draw navigation after the waterfall so each bar keeps a distinct hit
       slot and cannot steal the other's touch targets. */
    ls_btn_t b[N_PAGES];
    const int nb = build_tabs(b);
    s_tab_count = nb;
    tui_rect tabs_bar = s_bar;
    /* Portrait's view tabs already sit at the screen's bottom - the toggle's
       corner comes out of their bar instead, right where "beside the view
       controls" and "screen bottom-right" are the same place. */
    if (!wide && vfo) {
        int kw = tabs_bar.w / 5;
        if (kw < 8) kw = 8;
        if (kw > 14) kw = 14;
        if (tabs_bar.w - kw >= 24) {
            tabs_bar.w -= kw;
            s_kbd_btn = tui_rect_make(tabs_bar.x + tabs_bar.w, tabs_bar.y,
                                      kw, tabs_bar.h);
        }
    }
    ls_btn_bar_slot(sf, tabs_bar, b, nb, -1, LS_BTN_SLOT_QUICK);
    if (!wide && s_kbd_btn.h > 0) draw_kbd_toggle(sf, s_kbd_btn);
    (void)draw_controls(sf, ctl_rect, vfo);
}

static void leave(void)
{
    s_freq_keyboard = false;
    s_freq_invalid = false;
    fm_sstv_view_idle();
    ls_calls_leave(); ls_wf_source_release(); lora_pager_stop(false);
}

static void enter(void)
{
    s_freq_keyboard = false;
    s_freq_invalid = false;
    fm_same_enter();
    ls_rsel_track(LS_RSEL_FM, fm_in_use);
    ls_rsel_track(LS_RSEL_PAGER, fm_in_use);
    ls_rsel_track(LS_RSEL_ACARS, fm_in_use);
    /* Open on the receiver, not on the scanner.

       s_details was set from mode_page(), which returns 0 for LISTEN, so
       opening FM in its ordinary mode rendered the shared scan panel and
       returned before the VFO page could draw. The front door of a receiver
       was a channel list in a different interface. RADIO is still a tab away
       for when the scanner IS what you want. */
    s_details=true;
    s_last_mode = (int)FM.mode;
    s_page = mode_page(FM.mode);
    s_pv.open = false;
    s_pv.held = s_pv.missing = false;
}

static bool key_in(ls_tk_t k, char ch)
{
    if (ls_calls_active()) return ls_calls_key(k, ch);
    if (k == LS_TK_CHAR && (ch == 'b' || ch == 'B')) { audio_toggle_mute(); return true; }
    if (k >= LS_TK_F1) return false;
    if (s_details && s_page == 0 && s_freq_keyboard) {
        if (k == LS_TK_ESC) { s_freq_keyboard = false; return true; }
        if (k == LS_TK_ENTER) { freq_entry_tune(); return true; }
        if (k == LS_TK_BACKSPACE) { freq_entry_delete(); s_freq_invalid = false; return true; }
        if (k == LS_TK_CHAR && freq_key_char(ch)) return true;
    }
    if (k == LS_TK_CHAR && ch == '5') { ls_calls_open("FM"); ls_wf_source_release(); return true; }
    if(k>=LS_TK_F1) return false;
    if(k==LS_TK_CHAR && ch>='1' && ch<='3') {s_details=true;show_page(ch-'1');return true;}
    /* R is RADIO on every page, except inside the scanner's lists, where
       R is RANGE. */
    if (k==LS_TK_CHAR && (ch=='r'||ch=='R') && (s_details || (!s_radio.lists && !s_radio.scan_choice))) {
        open_radio(); return true;
    }
    /* O is OPTIONS for the mode, on the same terms: inside the scanner's
       lists it is SCAN TYPE. */
    if (k==LS_TK_CHAR && (s_details || (!s_radio.lists && !s_radio.scan_choice)) &&
        ls_opt_key(fm_options(), ch)) return true;
    if (!s_details) {
        if (k == LS_TK_ESC && !s_radio.lists && !s_radio.scan_choice) {
            s_details = true;
            return true;
        }
        radio_view(); radio_action(ls_radio_panel_key(&s_radio,&s_view,k,ch)); return true;
    }
    if (s_page == 1 && FM.mode == FM_MODE_AIS) {
        if (fm_ais_key(k)) return true;
        if (k == LS_TK_CHAR && (ch == 'u' || ch == 'a' || ch == 'd'))
            return fm_ais_key(ch == 'u' ? LS_TK_UP : ch == 'd' ? LS_TK_DOWN : LS_TK_ENTER);
    }
    if (s_page == 1 && FM.mode == FM_MODE_APRS) {
        if (fm_aprs_key(k)) return true;
        if (k == LS_TK_CHAR && (ch == 'u' || ch == 'a' || ch == 'd'))
            return fm_aprs_key(ch == 'u' ? LS_TK_UP : ch == 'd' ? LS_TK_DOWN : LS_TK_ENTER);
    }
    if (s_page == 1 && FM.mode == FM_MODE_SAME) {
        if (fm_same_key(k)) return true;
        if (k == LS_TK_CHAR && (ch == 'u' || ch == 'a' || ch == 'd'))
            return fm_same_key(ch == 'u' ? LS_TK_UP : ch == 'd' ? LS_TK_DOWN : LS_TK_ENTER);
    }
    if (s_page == 1 && own_page() && own_key(k, ch)) return true;
    /* An open page is closed before ESC does anything else. */
    if (k==LS_TK_ESC && s_page==1 && s_pv.open) { s_pv.open=false; return true; }
    if (k==LS_TK_ESC) return false;
    if (k==LS_TK_CHAR && ch=='0') { s_details=false; ls_wf_source_release(); return true; }
    if (s_page == 2 &&
        (k==LS_TK_LEFT || k==LS_TK_RIGHT || (k==LS_TK_CHAR && ch==' ')))
        return ls_wf_key(k,ch);
    if (s_page == 0 && ls_radio_tune_key(&s_tune, k, ch)) return true;
    if (s_page == 2 && k == LS_TK_CHAR && (ch == 'm' || ch == 'M'))
        return ls_wf_key(k, ch);
    if (k == LS_TK_CHAR) {
        if (ch == 'e' || ch == 'E' || ch == 'm' || ch == 'M') {
            open_mode_picker(); return true;
        }
        if (ch == 'k' || ch == 'K') { toggle_frequency_lock(); return true; }
        if (ch == 'n' || ch == 'N') { if (hf_on()) open_hf_band_picker(); else open_band_picker(); return true; }
        if (ch == 'v' || ch == 'V') { open_volume_entry(); return true; }
        if (ch == 'q' || ch == 'Q') { open_squelch_entry(); return true; }
        if (ch == 'w' || ch == 'W') { toggle_sweep(); return true; }
        if (ch == 't' || ch == 'T') {
            if (s_page == 0) {
                scan_engine_stop(); freq_entry_clear(); s_freq_invalid = false;
                s_freq_keyboard = true;
            } else ls_quick_fire(&QUICK[0], ls_quick_grant_builtin());
            return true;
        }
        if (ch == '[' || ch == ']') {
            scan_engine_stop();
            return tune_step(ch == '[' ? -1 : 1);
        }
        const int i = ls_btn_key(ch, PAGES, N_PAGES);
        if (i >= 0) { show_page(i); return true; }
        /* GAIN's nudge keys: a no-op on LR2021, same as the card's own +/-
           and the bar beneath it (fm_gain_live()). */
        if ((ch == 'u' || ch == 'U' || ch == 'j' || ch == 'J') &&
            !fm_gain_live()) return true;
        /* The same controls the panel draws, so a keyboard and a thumb reach
           them by one path and cannot drift apart. Checked after the page
           keys so a digit still turns the page. */
        if (ls_quick_key(ch, QUICK, N_QUICK,
                         ls_quick_grant_builtin(), NULL)) return true;
    }
    if (s_page == 2 && ls_wf_key(k, ch)) return true;

    /* The open page owns its keys while it is up. UP/DOWN walk to the next
       message without going back to the list first, which is what reading a
       run of pages actually looks like. */
    if (s_page == 1 && s_pv.open) return rx_pages_key(&s_pv, &FM_PAGES, k);

    switch (k) {
    case LS_TK_LEFT:  if (s_page > 0) show_page(s_page - 1); return true;
    case LS_TK_RIGHT: if (s_page < N_PAGES - 1) show_page(s_page + 1); return true;
    case LS_TK_UP: case LS_TK_DOWN:
        if (s_page == 1) rx_pages_key(&s_pv, &FM_PAGES, k);
        return true;
    case LS_TK_ENTER:
        return s_page == 1 && rx_pages_key(&s_pv, &FM_PAGES, k);
    default: return false;
    }
}

static bool touch_in(int col, int row)
{
    if (ls_calls_active()) return ls_calls_touch(col, row);
    if (!s_details) { radio_view(); radio_action(ls_radio_panel_touch(&s_radio,&s_view,col,row)); return true; }
    /* Checked first: it shares a row band with s_bar in portrait and with
       s_quick_rect in landscape, and both of those claim their whole band
       regardless of column. */
    if (s_page == 0 && tui_rect_contains(s_kbd_btn, col, row)) {
        toggle_freq_keyboard();
        return true;
    }
    if (tui_rect_contains(s_controls, col, row))
        return control_action(ls_btn_hit_slot(col, row, LS_BTN_SLOT_SCREEN));
    if (row >= s_bar.y && row < s_bar.y + s_bar.h) {
        const int slot = ls_btn_hit_slot(col, row, LS_BTN_SLOT_QUICK);
        if (slot >= 0 && slot < s_tab_count) { show_page(s_tab_page[slot]); return true; }
        return true;
    }
    if (s_quick_rect.h > 0 && row >= s_quick_rect.y &&
        row < s_quick_rect.y + s_quick_rect.h) {
        if (s_page == 0 && s_freq_keyboard) return freq_keyboard_touch(col, row);
        step_touch(col, row);
        return true;
    }
    /* Spectrum is the sole FM plot view; VFO rows belong to the quick controls. */
    if (s_page == 2) return ls_wf_touch(col, row);

    if (s_page == 1 && FM.mode == FM_MODE_AIS) return fm_ais_touch(col, row);
    if (s_page == 1 && FM.mode == FM_MODE_APRS) return fm_aprs_touch(col, row);
    if (s_page == 1 && FM.mode == FM_MODE_SAME) return fm_same_touch(col, row);
    if (s_page == 1 && own_page()) return own_touch(col, row);
    if (s_page == 1 && rx_pages_touch(&s_pv, &FM_PAGES, col, row)) return true;
    return false;
}

/* The selection is re-found before a key or tap acts on it and noted again
   after, whichever of the many places in them moved it. */
static bool key(ls_tk_t k, char ch)
{
    rx_pages_refind(&s_pv, &FM_PAGES);
    const bool handled = key_in(k, ch);
    rx_pages_remember(&s_pv, &FM_PAGES);
    return handled;
}

static bool touch(int col, int row)
{
    rx_pages_refind(&s_pv, &FM_PAGES);
    const bool handled = touch_in(col, row);
    rx_pages_remember(&s_pv, &FM_PAGES);
    return handled;
}

const ls_tui_screen_t ls_scr_fm = {
    /* the VFO, the sweep and the pager pages are all this receiver. */
    .radio = "FM",
    .name = "FM",
    .hint = s_hint,
    .enter = enter,
    .leave = leave,
    .diff_switch = true,
    .draw = draw,
    .key = key,
    .touch = touch,
};
