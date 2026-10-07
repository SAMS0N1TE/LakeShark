#include "../../ls_calls.h"
/* FM screen: VFO on one page, decoded pages on another.

   `FM` is a plain global like `P25`, and `FM.scan_db[256]` is a bare
   float array the sweep writes into - so the spectrum needs no widget, no
   canvas and no allocation, only a loop that maps bins to columns. */
#include "../../ls_tui_screen.h"
#include "../../ls_radio_panel.h"
#include "../../ls_numpad.h"
#include "../../ls_options.h"
#include "scan_engine.h"
#include "scan_channels.h"
#include "../../ls_motion.h"
#include "../../ls_text.h"
#include "fm_same_view.h"
#include "fm_aprs_view.h"
#include "fm_ais_view.h"

#include <stdio.h>
#include <string.h>

#include "apps/fm/fm_state.h"
#include "apps/fm/aprs_store.h"
#include "apps/fm/fm_mode_label.h"
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
#include "../../ls_wf_source.h"

static bool s_details;
static EXT_RAM_BSS_ATTR char s_hint[96] = "LEFT/RIGHT tune  UP/DOWN controls  M details";
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
    return s_lora_pager ? LS_RSEL_LORA : ls_rsel_sdr_held_by("fm");
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
    const bool want = FM.mode == FM_MODE_POCSAG && !s_pager_leaving &&
                      ls_rsel_resolved(LS_RSEL_PAGER) == LS_RSEL_LORA &&
                      FM.freq_hz >= 150000000u && FM.freq_hz <= 959000000u;
    if (want && !s_lora_pager) {
        if (!ls_field_start()) return;
        ls_field_snapshot(&s_field);
        s_lora_was = s_field.mode;
        s_lora_pager = true;
        s_lora_hz = 0;
        s_lora_baud = 0;
        s_lora_inv = false;
        s_lora_rx = s_field.rx;
        s_lora_flip_at = esp_timer_get_time() + LORA_FLIP_US;
        ls_field_mode(LS_LAB_POCSAG);
        ls_field_direct(true);
        ls_tui_radio_want(NULL);            /* the SDR goes back */
    } else if (!want && s_lora_pager) {
        lora_pager_stop(true);
    }
    if (!s_lora_pager) return;
    ls_field_snapshot(&s_field);
    /* The chip decodes 1200 and 2400 baud, one at a time: the rate the
       receiver is set to, or the one its search last locked. */
    const int asked = FM.pocsag_auto ? FM.pocsag_lock_baud : FM.pocsag_baud;
    const int baud = asked == 2400 ? 2400 : 1200;
    const int64_t now = esp_timer_get_time();
    if (s_field.rx != s_lora_rx) {
        s_lora_rx = s_field.rx;
        s_lora_flip_at = now + LORA_HOLD_US;
    } else if (!s_lora_pol && now >= s_lora_flip_at) {
        s_lora_inv = !s_lora_inv;
        s_lora_flip_at = now + LORA_FLIP_US;
        s_lora_hz = 0;
    }
    /* A polarity chosen in OPTIONS is held, and taking it is a retune. */
    if (s_lora_pol && s_lora_inv != (s_lora_pol == 2)) {
        s_lora_inv = s_lora_pol == 2;
        s_lora_hz = 0;
    }
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

static void open_radio(void) { ls_rsel_open(fm_job(), radio_chosen); }

static int mode_page(fm_mode_t mode)
{
    if (mode == FM_MODE_SCAN) return 2;
    if (mode == FM_MODE_POCSAG || mode == FM_MODE_FLEX ||
        mode == FM_MODE_SAME || mode == FM_MODE_APRS || mode == FM_MODE_AIS) return 1;
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
}

static void open_mode_picker(void)
{
    ls_picker_open(FM.mode==FM_MODE_SCAN ? "STOP SWEEP / MODE" : "RECEIVER MODE", mode_picked);
    for (int i = 0; i < N_MODES; ++i)
        ls_picker_add(fm_mode_label(FM_MODES[i]),
                      FM.mode == FM_MODES[i] ? "selected" : FM.mode==FM_MODE_SCAN ? "stops sweep" : "select mode");
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
    if (FM.mode == FM_MODE_WFM) return 100000;
    if (FM.mode == FM_MODE_AM) {
        if (FM.freq_hz >= 26965000 && FM.freq_hz <= 27405000) return 10000;
        if (FM.freq_hz >= 29000000 && FM.freq_hz <= 29200000) return 5000;
        return 25000;
    }
    if (FM.mode == FM_MODE_ACARS) return 25000;
    return 12500;
}

static bool tune_step(int direction)
{
    int64_t hz = (int64_t)FM.freq_hz + direction * (int64_t)tune_step_hz();
    if (hz < 24000000) hz = 24000000;
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

/* draw_vfo splits its rect in two, so in portrait it draws a VFO box above a
   SIGNAL box and needs room for both: six fields and a border, then a bar and
   three readouts and a border. Nine was the first guess and it clipped the
   pair into each other on the panel - two titles landed on the borders below
   them and read as a corrupt frame. */
#define VFO_ROWS 18

static void field(tui_surface *sf, tui_rect a, int row, const char *l,
                  const char *v, uint8_t la, uint8_t va)
{
    tui_put_str(sf, a, a.x + 2, a.y + row, l, la);
    tui_put_str(sf, a, a.x + 12, a.y + row, v, va);
}

/* WHAT EACH MODE CAN SAY ABOUT THE SIGNAL AND THE SQUELCH.

   NFM measures quieting and gates on it, in percent. AM measures carrier
   level and gates on that, in tenths of a percent. WFM has no squelch and the
   decoders never write either reading, so FM.noise and FM.squelch_open there
   are whatever NFM last left behind: those modes show "--" instead. */
typedef enum { SIG_NONE, SIG_QUIETING, SIG_CARRIER } sig_kind_t;

static sig_kind_t sig_kind(void)
{
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

static void squelch_text(char *out, size_t n)
{
    switch (sig_kind()) {
    case SIG_QUIETING: snprintf(out, n, "%d%%", FM.squelch_tenths); break;
    case SIG_CARRIER:  snprintf(out, n, "%d.%d%%", FM.squelch_tenths / 10,
                                FM.squelch_tenths % 10); break;
    default:           snprintf(out, n, "--"); break;
    }
}

static const char *squelch_hint(void)
{
    return FM.mode == FM_MODE_AM ? "carrier level, tenths of a percent"
                                 : "0 opens on anything, 100 on nothing";
}

/* WHAT THE PAGER DECODER THAT IS RUNNING HAS COUNTED.

   POCSAG on an SDR fills FM.pocsag_*, FLEX fills FM.flex_*, and POCSAG on the
   chip is counted in the ls_field snapshot, which has packets heard and pages
   but no lock and no codeword errors. Every other mode runs no pager decoder,
   and then there is nothing to show. */
typedef struct {
    bool     on;
    bool     chip;      /* packets heard and pages, no lock or error count */
    bool     tape;      /* counts codewords, which is what the tape draws  */
    bool     sync;
    int      baud;      /* 0 where the decoder reports no rate             */
    uint32_t pages, errs, heard;
} pager_counts_t;

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
    } else if (FM.mode == FM_MODE_FLEX) {
        c.on = true;
        c.sync = FM.flex_sync;
        c.pages = FM.flex_pages;
        c.errs = FM.flex_cw_errs;
    }
    return c;
}

static float    s_sig_hold;      /* decaying peak on the signal meter */
static sig_kind_t s_sig_kind;    /* what that peak was measured on    */
/* The dial spins and the mode wipes. Both are frame-counted off s_blink, and
   both are covering something the radio is really doing: a retune has a
   settle time before the squelch is allowed to open, and a mode change tears
   the receiver down and builds it again. The motion is the length of the
   wait, so it reads as the radio working rather than as decoration. */
static uint32_t s_dial_hz;       /* what the readout is showing right now   */
static uint32_t s_dial_target;   /* where it is heading                     */
static int      s_dial_frames;   /* frames left in the spin                 */
static int      s_mode_wipe;     /* frames left in the mode change wipe     */
static int      s_wipe_mode = -1;

static void draw_vfo(tui_surface *sf, tui_rect area)
{
    const uint32_t hz    = FM.freq_hz;
    const int      sq    = FM.squelch_tenths;
    const sig_kind_t kind = sig_kind();
    const bool     open  = kind != SIG_NONE && FM.squelch_open;
    const uint8_t frame = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t lab   = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t val   = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t good  = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim   = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    char buf[48];

    tui_rect left, right;
    ls_tui_split(area, &left, &right);

    tui_box(sf, left, "RECEIVER", frame);

    /* A retune is not instant, so neither is the readout. The dial runs from
       where it was to where it is going over the settle, and the digits that
       are still moving are the ones drawn bright. */
    if (hz != s_dial_target) {
        s_dial_target = hz;
        s_dial_frames = 10;
        if (s_dial_hz == 0) s_dial_hz = hz;
    }
    if (s_dial_frames > 0) {
        s_dial_frames--;
        const int64_t gap = (int64_t)s_dial_target - (int64_t)s_dial_hz;
        s_dial_hz = (uint32_t)((int64_t)s_dial_hz + gap / 3);
        if (s_dial_frames == 0) s_dial_hz = s_dial_target;
    } else {
        s_dial_hz = s_dial_target;
    }
    const uint32_t shown = s_dial_hz ? s_dial_hz : hz;
    snprintf(buf, sizeof(buf), "%u.%04u MHz", (unsigned)(shown / 1000000u),
             (unsigned)((shown % 1000000u) / 100u));
    {
        char settled[48];
        snprintf(settled, sizeof(settled), "%u.%04u MHz",
                 (unsigned)(hz / 1000000u), (unsigned)((hz % 1000000u) / 100u));
        const int x0 = left.x + (left.w - (int)strlen(buf)) / 2;
        const uint8_t moving = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
        for (int i = 0; buf[i]; i++) {
            const bool differs = settled[i] && buf[i] != settled[i];
            char one[2] = { buf[i], 0 };
            tui_put_str(sf, left, x0 + i, left.y + 2, one, differs ? moving : val);
        }
    }

    /* A mode change tears the receiver down and brings it back. The wipe is
       that gap, made visible, rather than the readout simply changing. */
    if (s_wipe_mode != (int)FM.mode) {
        if (s_wipe_mode >= 0) s_mode_wipe = 12;
        s_wipe_mode = (int)FM.mode;
    }
    if (s_mode_wipe > 0) {
        const int col = (12 - s_mode_wipe) * (left.w - 2) / 12;
        for (int y = 1; y < left.h - 1 && y < 4; y++)
            tui_put_char(sf, left, left.x + 1 + col, left.y + y,
                         LS_TUI_SHADE_50,
                         TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        s_mode_wipe--;
    }
    squelch_text(buf, sizeof(buf));
    field(sf, left, 5, "SQUELCH", buf, lab, kind == SIG_NONE ? dim : val);
    field(sf, left, 4, "CARRIER",
          kind == SIG_NONE ? "--" : open ? "OPEN" : "closed", lab,
          open ? good : dim);

    if (open && ((s_blink / 12) & 1))
        tui_put_char(sf, left, left.x + 17, left.y + 4, LS_TUI_SHADE_FULL,
                    good);
    snprintf(buf, sizeof(buf), "%d %%", audio_volume_get());
    field(sf, left, 6, "VOLUME", buf, lab, val);
    snprintf(buf, sizeof(buf), "%d.%d dB", FM.gain_tenths / 10,
             FM.gain_tenths % 10);
    if (left.h > 8) field(sf, left, 7, "GAIN", buf, lab, val);
    else {
        snprintf(buf, sizeof(buf), "%d%%  %.1f dB", audio_volume_get(), FM.gain_tenths / 10.0);
        field(sf, left, 6, "VOL/GAIN", buf, lab, val);
    }
    if (FM.mode == FM_MODE_LISTEN && FM.tone_show) {
        fm_tone_label(FM.tone_detected.selection, buf, sizeof(buf));
        field(sf, left, 3, FM.tone_detected.selection ? "TONE LOCK" : "TONE",
              FM.tone_detected.selection ? buf : "--", lab,
              FM.tone_detected.selection ? good : dim);
        tui_put_char(sf, left, left.x + 1, left.y + 3,
                     ls_motion_pip(FM.tone_detected.selection != 0), good);
    }

    tui_box(sf, right, "SIGNAL", frame);
    /* QUIETING, NOT LEVEL.

       This bar used to read FM.iq_level, the carrier level across the whole
       IQ block, and it barely moved: measured on hardware, dead air 3% and
       the strongest local broadcast 5%, so the meter sat near empty whatever
       the radio heard. An FM receiver measures signal by how much a carrier
       quietens the discriminator, so the bar reads the inverse of the noise
       the squelch is already measuring. Same bench: broadcast 0.46 of full
       hiss, dead air 0.81 to 0.97, which is most of the bar's travel.

       That also puts the squelch on this scale, so the gate can be drawn as
       a mark on the bar and set by eye. */
    const int bw = right.w - 4;
    if (kind != s_sig_kind) { s_sig_hold = 0.0f; s_sig_kind = kind; }

    if (kind == SIG_NONE) {
        /* No reading in this mode, so no bar and no gate to mark on it. */
        tui_put_str(sf, right, right.x + 2, right.y + 2, "--", dim);
    } else {
        const float sig = sig_level(kind);

        /* Peak hold, decaying, so a burst between two looks still registers. */
        if (sig >= s_sig_hold) s_sig_hold = sig;
        else                   s_sig_hold -= (s_sig_hold - sig) * 0.06f;

        const int lit  = (int)(sig * (float)bw);
        const int hold = (int)(s_sig_hold * (float)bw);
        /* Where the squelch opens, on the same travel as the bar. */
        int gate = (int)((float)(100 - sq) * 0.01f * (float)bw);
        if (gate < 0) gate = 0;
        if (gate >= bw) gate = bw - 1;
        gate = bw - gate;

        for (int i = 0; i < bw; i++) {
            const bool on = i < lit;
            uint8_t c = on ? (i > bw * 3 / 4 ? TUI_RED | TUI_BRIGHT
                            : i > bw / 2     ? TUI_YELLOW | TUI_BRIGHT
                                             : TUI_GREEN | TUI_BRIGHT)
                           : (TUI_BLACK | TUI_BRIGHT);
            char g = on ? LS_TUI_SHADE_FULL : LS_TUI_SHADE_25;
            if (!on && i == hold && hold > lit) {
                g = LS_TUI_SHADE_50;
                c = TUI_WHITE | TUI_BRIGHT;
            }
            tui_put_char(sf, right, right.x + 2 + i, right.y + 2, g,
                         TUI_ATTR(c, TUI_BLACK));
        }
        /* The gate marker sits under the bar so it never covers the reading.
           Above it the squelch is open, below it the audio is muted. */
        for (int i = 0; i < bw; i++)
            tui_put_char(sf, right, right.x + 2 + i, right.y + 3,
                         i == gate ? '^' : ' ',
                         TUI_ATTR(i == gate ? (open ? TUI_GREEN | TUI_BRIGHT
                                                    : TUI_CYAN | TUI_BRIGHT)
                                            : TUI_BLACK, TUI_BLACK));
    }
    snprintf(buf, sizeof(buf), "%lu B/s", (unsigned long)FM.iq_bytes_sec);
    field(sf, right, 4, "IQ RATE", buf, lab, val);
    const pager_counts_t pc = pager_counts();
    if (pc.on) {
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)pc.pages);
        field(sf, right, 5, "PAGES", buf, lab, pc.pages ? good : dim);
        if (pc.chip) {
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)pc.heard);
            field(sf, right, 6, "HEARD", buf, lab, pc.heard ? good : dim);
        } else {
            field(sf, right, 6, "SYNC", pc.sync ? "LOCKED" : "hunting", lab,
                  pc.sync ? good : dim);
        }
    }
}

/* The codeword tape. */

#define TAPE_MAX 256

static uint8_t  s_tape[TAPE_MAX];
static int      s_tape_n;                  /* marks held, up to TAPE_MAX */
static uint32_t s_seen_frames, s_seen_addr, s_seen_msg, s_seen_err;
static bool     s_seen_valid;

enum { TAPE_IDLE = 0, TAPE_ADDR, TAPE_MSG, TAPE_ERR };

static void tape_push(uint8_t mark, int n)
{
    /* A burst is capped: a counter that jumped by thousands because the
       screen was away means the tape cannot show what happened in between,
       and filling it entirely with one mark would claim that it can. */
    if (n > TAPE_MAX / 4) n = TAPE_MAX / 4;
    for (int i = 0; i < n; i++) {
        if (s_tape_n < TAPE_MAX) {
            s_tape[s_tape_n++] = mark;
        } else {
            memmove(s_tape, s_tape + 1, TAPE_MAX - 1);
            s_tape[TAPE_MAX - 1] = mark;
        }
    }
}

static void tape_sample(void)
{
    /* Only POCSAG on an SDR counts codewords. Anywhere else the tape holds
       nothing, and the next time it does it starts from that count. */
    if (!pager_counts().tape) { s_tape_n = 0; s_seen_valid = false; return; }

    const uint32_t f = FM.pocsag_frames, a = FM.pocsag_addr;
    const uint32_t m = FM.pocsag_msg,    e = FM.pocsag_cw_errs;

    if (!s_seen_valid) {
        s_seen_valid = true;
        s_seen_frames = f; s_seen_addr = a; s_seen_msg = m; s_seen_err = e;
        return;
    }

    /* Counters only go up, and a decoder restart takes them back to zero.
       Treating that as a huge negative delta would push nothing; treating it
       as a huge positive one would fill the tape. Re-baseline instead. */
    if (f < s_seen_frames || a < s_seen_addr ||
        m < s_seen_msg || e < s_seen_err) {
        s_tape_n = 0;
        s_seen_frames = f; s_seen_addr = a; s_seen_msg = m; s_seen_err = e;
        return;
    }

    const uint32_t d_addr = a - s_seen_addr;
    const uint32_t d_msg  = m - s_seen_msg;
    const uint32_t d_err  = e - s_seen_err;
    uint32_t d_frame = f - s_seen_frames;

    /* The interesting ones first, then whatever frames are left over as
       idle - so a batch that carried one address among fifteen idles reads
       as one address among fifteen idles. */
    tape_push(TAPE_ERR,  (int)d_err);
    tape_push(TAPE_ADDR, (int)d_addr);
    tape_push(TAPE_MSG,  (int)d_msg);

    const uint32_t named = d_addr + d_msg + d_err;
    if (d_frame > named) d_frame -= named; else d_frame = 0;
    /* A frame is sixteen codewords; the tape is codewords. */
    tape_push(TAPE_IDLE, (int)(d_frame * 16));

    s_seen_frames = f; s_seen_addr = a; s_seen_msg = m; s_seen_err = e;
}

static void draw_tape(tui_surface *sf, tui_rect a)
{
    const uint8_t dim = LS_ATTR_DIM;
    const uint8_t yel  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t wht  = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t red  = TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK);
    const uint8_t good = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    char buf[48];

    ls_panel_box(sf, a, "STREAM", TUI_CYAN);
    if (a.h < 5 || a.w < 24) return;

    /* The counters go on the right, against the frame, so the tape gets the
       width. Sync first: it is the one that decides whether any of the rest
       means anything. */
    const int cw = 13;
    const int tw = a.w - 2 - cw;
    if (tw < 8) return;

    const pager_counts_t pc = pager_counts();
    const int cx = a.x + tw + 2;
    int row = 1;
    if (pc.chip) {
        snprintf(buf, sizeof(buf), "%lu rx", (unsigned long)pc.heard);
        tui_put_str(sf, a, cx, a.y + row++, buf, pc.heard ? good : dim);
    } else {
        tui_put_str(sf, a, cx, a.y + row++, pc.sync ? "SYNC" : "no sync",
                    pc.sync ? good : dim);
    }
    if (pc.baud) {
        snprintf(buf, sizeof(buf), "%d bd", pc.baud);
        tui_put_str(sf, a, cx, a.y + row++, buf, dim);
    }
    snprintf(buf, sizeof(buf), "%lu pg", (unsigned long)pc.pages);
    tui_put_str(sf, a, cx, a.y + row++, buf, pc.pages ? yel : dim);
    if (!pc.chip && row < a.h - 2) {
        snprintf(buf, sizeof(buf), "%lu err", (unsigned long)pc.errs);
        tui_put_str(sf, a, cx, a.y + row++, buf, pc.errs ? red : dim);
    }

    if (!pc.tape) {
        snprintf(buf, sizeof(buf), "%.*s", tw - 1, "tape: POCSAG on an SDR");
        tui_put_str(sf, a, a.x + 2, a.y + 1, buf, dim);
        return;
    }

    const int rows = a.h - 2;
    const int cap  = rows * tw;
    int start = s_tape_n - cap;
    if (start < 0) start = 0;

    for (int i = start; i < s_tape_n; i++) {
        const int at = i - start;
        const int x = a.x + 1 + (at % tw);
        const int y = a.y + 1 + (at / tw);
        if (y >= a.y + a.h - 1) break;

        char ch; uint8_t attr;
        switch (s_tape[i]) {
        case TAPE_ADDR: ch = 'A'; attr = yel;  break;
        case TAPE_MSG:  ch = '#'; attr = wht;  break;
        case TAPE_ERR:  ch = 'X'; attr = red;  break;
        default:        ch = '.'; attr = dim;  break;
        }
        tui_put_char(sf, a, x, y, ch, attr);
    }

    if (!s_tape_n)
        tui_put_str(sf, a, a.x + 2, a.y + 1, "no codewords yet", dim);
}

/* The pages list, and the page behind it. */

static int  s_page_sel;
static int  s_page_top;          /* first list row shown                  */
static bool s_page_open;         /* the full view of the selected page    */
static tui_rect s_page_rect;     /* where the list landed, for taps       */
static tui_rect s_page_back;     /* the BACK target on the detail view    */
static tui_rect s_pager_hit[3];

/* Newest first, which is the order the list shows and the order a selection
   index means. */
static const fm_page_t *page_at(int i)
{
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
        one.type = p->text[0] ? 'A' : 'T';
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
    if (s_lora_pager) return s_field.page_log_count;
    int n = (int)FM.page_count;
    if (n > FM_PAGE_LOG_MAX) n = FM_PAGE_LOG_MAX;
    return n;
}

/* HOLDING THE SELECTION ON ITS MESSAGE.

   The selection is an index into a newest-first list, so each page that
   arrives moves every message one place down and the index then names the
   next newer one: a page being read turned into another under the reader.
   While anything but the newest is selected, or a page is open, the message's
   own time and address are kept as well, and found again before the next
   draw, key or tap. Messages only move down the list, so the search starts
   where the selection was; one that has left the list altogether keeps its
   index. */
static int64_t  s_sel_ts;
static uint32_t s_sel_addr;
static bool     s_sel_held;

static void pager_refind(void)
{
    if (!s_sel_held) return;
    const int count = page_count();
    for (int i = s_page_sel; i < count; i++) {
        const fm_page_t *p = page_at(i);
        if (p && p->ts_us == s_sel_ts && p->address == s_sel_addr) {
            s_page_top += i - s_page_sel;
            s_page_sel = i;
            return;
        }
    }
}

static void pager_remember(void)
{
    const fm_page_t *p = (s_page_sel > 0 || s_page_open) ? page_at(s_page_sel)
                                                         : NULL;
    s_sel_held = p != NULL;
    if (p) { s_sel_ts = p->ts_us; s_sel_addr = p->address; }
}

static void pager_move(int delta)
{
    const int count = page_count();
    s_page_sel += delta;
    if (s_page_sel >= count) s_page_sel = count - 1;
    if (s_page_sel < 0) s_page_sel = 0;
}

static int draw_pager_controls(tui_surface *sf, tui_rect area)
{
    memset(s_pager_hit, 0, sizeof(s_pager_hit));
    const int height = ls_tui_is_wide() ? 3 : 5;
    if (area.w < 24 || area.h < height + 4) return 0;
    pager_move(0);
    const int count = page_count();
    char center[32];
    snprintf(center, sizeof(center), "%s %d/%d", s_page_open ? "LIST" : "OPEN",
             count ? s_page_sel + 1 : 0, count);
    const char *labels[] = {"UP", center, "DOWN"};
    const bool enabled[] = {s_page_sel > 0, count > 0, s_page_sel + 1 < count};
    for (int i = 0; i < 3; ++i) {
        int x0 = area.x + area.w * i / 3;
        int x1 = area.x + area.w * (i + 1) / 3;
        tui_rect box = tui_rect_make(x0, area.y + area.h - height, x1 - x0, height);
        s_pager_hit[i] = box;
        uint8_t attr = enabled[i] ? TUI_ATTR(TUI_BLACK, TUI_CYAN)
                                  : TUI_ATTR(TUI_WHITE, TUI_BLACK);
        tui_box(sf, box, NULL, TUI_ATTR(enabled[i] ? TUI_CYAN : TUI_WHITE, TUI_BLACK));
        for (int y = box.y + 1; y < box.y + box.h - 1; ++y)
            for (int x = box.x + 1; x < box.x + box.w - 1; ++x)
                tui_put_char(sf, box, x, y, ' ', attr);
        tui_put_str(sf, box, box.x + (box.w - (int)strlen(labels[i])) / 2,
                    box.y + box.h / 2, labels[i], attr);
    }
    return height + 1;
}

/* One row per page in landscape, three in portrait: the same measurement the
   mesh node list and the mesh settings use, and for the same reason - one row
   of a 10x17 cell is 1.3 mm and not a target. */
static int page_row_h(void) { return ls_tui_is_wide() ? 1 : 3; }

/* Which page a row belongs to. Draw and hit test both go through this so
   they cannot disagree about where a row is. */
static int page_at_row(tui_rect r, int row)
{
    const int i = (row - r.y - 1) / page_row_h();
    return i >= 0 ? s_page_top + i : -1;
}

static const char *type_word(char t)
{
    switch (t) {
    case 'A': return "ALPHANUMERIC";
    case 'N': return "NUMERIC";
    case 'T': return "TONE ONLY";
    default:  return "NOT SURE - see below";
    }
}

/* The page, whole.

   Wrapped at a word where there is one, because a message broken mid-word
   every forty characters is harder to read than the truncation it replaced.
   Everything the decoder recorded is on here: a page that looks wrong is
   either the wrong baud or the wrong classification, and those are the two
   fields that say which. */
static void draw_page_detail(tui_surface *sf, tui_rect pane)
{
    const uint8_t lab   = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t val   = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t txt   = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim   = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    const uint8_t warn  = TUI_ATTR(TUI_YELLOW, TUI_BLACK);

    s_page_back = tui_rect_make(0, -1, 0, 0);

    const fm_page_t *pg = page_at(s_page_sel);
    if (!pg) {
        ls_panel_box(sf, pane, "PAGE", TUI_CYAN);
        tui_put_str(sf, pane, pane.x + 2, pane.y + 2, "that page is gone", dim);
        return;
    }

    /* Wrapped FIRST, so the box can be the size of what is in it.

       A box stretched over the whole pane with a line of text near the top
       is the fault recorded on the P25 spectrum notice: forty-five
       rows of empty rectangle whose bottom edge lands on the page buttons.
       A page is six fields and a couple of lines; it should look like it. */
#define DETAIL_LINES 8
    char wrap[DETAIL_LINES][96];
    int nlines = 0;
    {

        nlines = ls_wrap_text(pg->text, pane.w - 4,
                              (char *)wrap, sizeof(wrap[0]), DETAIL_LINES);
        if (!nlines) snprintf(wrap[nlines++], sizeof(wrap[0]), "%s", "(no text)");
    }

    const bool unsure = (pg->type == '?');
    const int bh = ls_tui_is_wide() ? 1 : 3;
    /* border, five fields, blank, the unsure line, the message, blank, the
       way back, border. */
    int want = 1 + 5 + 1 + (unsure ? 2 : 0) + nlines + 1 + bh + 1;
    if (want > pane.h) want = pane.h;

    tui_rect area = tui_rect_make(pane.x, pane.y, pane.w, want);
    ls_panel_box(sf, area, "PAGE", TUI_CYAN);

    int y = area.y + 1;
    char buf[64];

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)pg->address);
    field(sf, area, y - area.y, "RIC", buf, lab, val);
    y++;

    field(sf, area, y - area.y, "TYPE", type_word(pg->type), lab,
          unsure ? warn : val);
    y++;

    snprintf(buf, sizeof(buf), "%u", (unsigned)pg->function);
    field(sf, area, y - area.y, "FUNCTION", buf, lab, val);
    y++;

    /* The baud is on the screen for the first time. The receiver
       runs three decoders at once and they all write this one list, so
       "which decoder produced this" was unanswerable from the panel - and
       it is the first question to ask about a page that reads as rubbish. */
    snprintf(buf, sizeof(buf), "%u", (unsigned)pg->baud);
    field(sf, area, y - area.y, "BAUD", pg->baud ? buf : "-", lab, val);
    y++;

    field(sf, area, y - area.y, "PROTOCOL",
          pg->protocol == FM_PAGE_PROTOCOL_FLEX ? "FLEX" : "POCSAG", lab, val);
    y += 2;

    if (unsure && y < area.y + area.h - 2) {
        tui_put_str(sf, area, area.x + 2, y,
                    "not sure this is readable - see BAUD", warn);
        y += 2;
    }

    for (int i = 0; i < nlines && y < area.y + area.h - 1 - bh; i++, y++)
        tui_put_str(sf, area, area.x + 2, y, wrap[i], txt);

    /* A way back that is a target, not a key. Portrait has no
       keyboard, so ESC is not available and a page you cannot leave is worse
       than no page. */
    if (area.h > bh + 4) {
        s_page_back = tui_rect_make(area.x + 2, area.y + area.h - 1 - bh,
                                    area.w - 4, bh);
        ls_fill_dither(sf, s_page_back, LS_DITHER_LIGHT, TUI_CYAN);
        ls_dither_label(sf, s_page_back, (bh - 1) / 2, "BACK TO THE LIST",
                        TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    }
#undef DETAIL_LINES
}

static void draw_pages(tui_surface *sf, tui_rect whole)
{
    const uint8_t frame = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t sel   = TUI_ATTR(TUI_BLACK, TUI_CYAN | TUI_BRIGHT);
    const uint8_t addr  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t body  = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    const uint8_t dim = LS_ATTR_DIM;

    pager_refind();
    tape_sample();

    s_page_rect = tui_rect_make(0, -1, 0, 0);
    s_page_back = tui_rect_make(0, -1, 0, 0);
    whole.h -= draw_pager_controls(sf, whole);

    /* The full view takes the whole pane. It is one message and it
       wants the width and the height; the tape above it answers a different
       question and can wait. */
    if (s_page_open) {
        draw_page_detail(sf, whole);
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

    char box[40];
    if (s_lora_pager) snprintf(box, sizeof(box), "DECODED PAGES / %s", ls_rsel_name(LS_RSEL_LORA));
    else snprintf(box, sizeof(box), "DECODED PAGES");
    tui_box(sf, area, box, frame);
    s_page_rect = area;

    const int count = page_count();
    if (!count) {
        tui_put_str(sf, area, area.x + 2, area.y + 2, "listening...", dim);
        /* FLEX with the chip chosen: the chip reads POCSAG only, so FLEX
           stays on the SDR, and says so rather than looking deaf. */
        if (FM.mode == FM_MODE_FLEX && ls_rsel_saved(LS_RSEL_PAGER) == LS_RSEL_LORA) {
            char why[64];
            snprintf(why, sizeof(why), "FLEX needs an SDR; the %s reads POCSAG",
                     ls_rsel_name(LS_RSEL_LORA));
            tui_put_str(sf, area, area.x + 2, area.y + 3, why, dim);
        }
        return;
    }

    const int rh = page_row_h();
    /* The last interior row carries the hint, so it is not a list row. */
    const int rows = (area.h - 3) / rh;
    if (rows < 1) return;

    /* The window follows the selection instead of the selection
       running off the end of the window. Without this the list showed the
       newest `rows` pages and nothing else could ever be reached - which is
       the "scrolling messages as they get cut" half of the report. */
    if (s_page_sel < 0) s_page_sel = 0;
    if (s_page_sel > count - 1) s_page_sel = count - 1;
    if (s_page_sel < s_page_top) s_page_top = s_page_sel;
    if (s_page_sel >= s_page_top + rows) s_page_top = s_page_sel - rows + 1;
    if (s_page_top > count - rows) s_page_top = count - rows;
    if (s_page_top < 0) s_page_top = 0;

    for (int r = 0; r < rows; r++) {
        const int i = s_page_top + r;
        const fm_page_t *pg = page_at(i);
        if (!pg) break;

        const int top = area.y + 1 + r * rh;
        const int y = top + (rh - 1) / 2;
        const bool on = (i == s_page_sel);

        if (on) {
            tui_fill(sf, tui_rect_make(area.x + 1, top, area.w - 2, rh), ' ', sel);
        } else if (rh > 1) {

            ls_fill_dither(sf, tui_rect_make(area.x + 1, top, area.w - 2, rh),
                           LS_DITHER_LIGHT, TUI_CYAN);
        }

        char line[160];
        snprintf(line, sizeof(line), "%-9lu %c %.*s", (unsigned long)pg->address,
                 pg->type ? pg->type : '?', area.w - 16, pg->text);
        tui_put_str(sf, area, area.x + 2, y, line, on ? sel : body);
        if (!on) {
            char id[12];
            snprintf(id, sizeof(id), "%-9lu", (unsigned long)pg->address);
            tui_put_str(sf, area, area.x + 2, y, id, addr);
        }
    }

    /* Where you are in the list, and how to open one. A count that only
       appears when something is off screen: a list that fits has nothing to
       say about its own position. */
    char hint[64];
    if (count > rows)
        snprintf(hint, sizeof(hint), "%d-%d of %d   %s", s_page_top + 1,
                 s_page_top + rows, count,
                 ls_tui_is_wide() ? "ENTER opens it" : "tap twice to open");
    else
        snprintf(hint, sizeof(hint), "%s",
                 ls_tui_is_wide() ? "ENTER opens it" : "tap a page twice to open it");
    tui_put_str(sf, area, area.x + 2, area.y + area.h - 2, hint, dim);
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
            snprintf(line, sizeof(line), "sweeping: tune %d of %d",
                     FM.scan_idx + 1, FM.scan_tunes);
        else
            snprintf(line, sizeof(line), "%s", ls_tui_is_wide() ? "W starts the band sweep" : "Tap RUN SWEEP to start");
        tui_put_str(sf, area, area.x + 2, area.y + 4, line, LS_ATTR_DIM);
        return;
    }
    ls_wf_draw(sf, area);
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
    return FM.mode == FM_MODE_POCSAG || FM.mode == FM_MODE_FLEX || FM.mode == FM_MODE_SAME || FM.mode == FM_MODE_APRS || FM.mode == FM_MODE_AIS;
}

static tui_rect s_bar;
/* Which page each drawn tab belongs to. The strip hides PAGER outside a pager
   mode, so the slot a thumb lands on is not the page index; without this map
   tapping RADIO acted on SPECTRUM and SPECTRUM acted on a hidden tab. */
static int      s_tab_page[N_PAGES];
static int      s_tab_count;
static tui_rect s_controls;
/* Where VOLUME, GAIN and SQUELCH landed. They were defined in QUICK all
   along but only ls_quick_key ever reached them, so they existed on the
   keyboard and nowhere on the screen - unreachable on a handheld whose
   keyboard detaches. GPS, MAP and P25 all draw theirs; this one did not. */
static tui_rect s_quick_rect;

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

static double o_squelch(const ls_opt_t *o) { (void)o; return FM.squelch_tenths; }
static void o_set_squelch(const ls_opt_t *o, double v) { (void)o; set_squelch(v); }

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

#define OPT_GAIN { .label = "GAIN", .kind = LS_OPT_NUMBER, .radios = LS_OPT_SDR, \
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
static const ls_opt_t OPT_NFM[] = {
    { .label = "TONE", .kind = LS_OPT_MENU, .sub = &CTX_TONE, .show = o_show_tone },
    LS_CALLS_MENU,
    { .label = "SQUELCH", .kind = LS_OPT_NUMBER, .num = o_squelch, .set_num = o_set_squelch,
      .lo = 0, .hi = 100, .unit = "0 opens on anything, 100 on nothing" },
    OPT_GAIN,
};
static const ls_opt_t OPT_AM[] = {
    LS_CALLS_MENU,
    { .label = "SQUELCH", .kind = LS_OPT_NUMBER, .num = o_squelch, .set_num = o_set_squelch,
      .lo = 0, .hi = 100, .unit = "carrier level, tenths of a percent" },
    OPT_GAIN,
};
static const ls_opt_t OPT_GAIN_ONLY[] = { OPT_GAIN };
static const ls_opt_t OPT_POCSAG[] = {
    { .label = "BAUD", .kind = LS_OPT_CYCLE, .radios = LS_OPT_SDR, .arg = 0,
      .names = BAUD_SDR, .n = 4, .get = o_baud, .set = o_set_baud, .show = o_show_baud },
    { .label = "BAUD", .kind = LS_OPT_CYCLE, .radios = LS_OPT_LORA, .arg = 1,
      .names = BAUD_CHIP, .n = 3, .get = o_baud, .set = o_set_baud, .show = o_show_baud },
    { .label = "POLARITY", .kind = LS_OPT_CYCLE, .radios = LS_OPT_LORA,
      .names = POLARITY, .n = 3, .get = o_polarity, .set = o_set_polarity, .show = o_show_polarity },
    OPT_GAIN,
};
static const ls_opt_t OPT_ACARS[] = {
    { .label = "CHANNEL", .kind = LS_OPT_CYCLE, .names = ACARS_CH, .n = 3,
      .get = o_acars, .set = o_set_acars, .show = o_show_acars },
    OPT_GAIN,
};

static const ls_opt_ctx_t CTX_NFM    = { .name = "NFM", .job = LS_RSEL_FM, .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_NFM) };
static const ls_opt_ctx_t CTX_WFM    = { .name = "WFM", .job = LS_RSEL_FM, .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_GAIN_ONLY) };
static const ls_opt_ctx_t CTX_AM     = { .name = "AM", .job = LS_RSEL_FM, .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_AM) };
static const ls_opt_ctx_t CTX_POCSAG = { .name = "POCSAG", .job = LS_RSEL_PAGER, .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_POCSAG) };
static const ls_opt_ctx_t CTX_FLEX   = { .name = "FLEX", .job = LS_RSEL_PAGER, .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_GAIN_ONLY) };
static const ls_opt_ctx_t CTX_ACARS  = { .name = "ACARS", .job = LS_RSEL_ACARS, .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_ACARS) };

/* The mode's, or NULL in a sweep, which has nothing here to set. */
static const ls_opt_ctx_t *fm_options(void)
{
    switch (FM.mode) {
    case FM_MODE_LISTEN: return &CTX_NFM;
    case FM_MODE_WFM:    return &CTX_WFM;
    case FM_MODE_AM:     return &CTX_AM;
    case FM_MODE_POCSAG: return &CTX_POCSAG;
    case FM_MODE_FLEX:   return &CTX_FLEX;
    case FM_MODE_ACARS:  return &CTX_ACARS;
    case FM_MODE_SAME:   return &fm_same_options;
    case FM_MODE_APRS:   return &fm_aprs_options;
    case FM_MODE_AIS:    return &fm_ais_options;
    default:             return NULL;
    }
}

static int draw_controls(tui_surface *sf, tui_rect area)
{
    char locked[20];
    const uint32_t lock_hz = lakeshark_fm_frequency_lock_hz();
    if (lock_hz) snprintf(locked, sizeof(locked), "%.4f", lock_hz / 1e6);
    else snprintf(locked, sizeof(locked), "OFF");
    /* SWEEP is a mode, so it lives in the mode picker with the rest and not
       in a button of its own; TUNE was this row and the quick bar calling the
       same action twice. The two slots that frees are the two controls a
       receiver actually needs to hand and that this screen did not offer
       without going through the quick bar: how loud, and when to open. */
    char vol[12], sql[12], band[16];
    /* APRS sits on one channel, so name it; the sweep presets are for the rest. */
    if (FM.mode == FM_MODE_APRS) snprintf(band, sizeof(band), "%.2f", aprs_frequency() / 1e6);
    else snprintf(band, sizeof(band), "%s", FM.mode == FM_MODE_AIS ? "87B/88B" : ls_wf_preset_current(LS_WF_SRC_FM));
    snprintf(vol, sizeof(vol), "%d%%", audio_volume_get());
    squelch_text(sql, sizeof(sql));
    ls_btn_t buttons[] = {
        {"MODE", FM.mode == FM_MODE_SCAN ? "SWEEP" : fm_mode_label(FM.mode), 'e', false, false},
        {"BAND", band, 'n', false, false},
        {"VOLUME", vol, 'v', false, false},
        {"SQUELCH", sql, 'q', sig_kind() != SIG_NONE && FM.squelch_open, false},
        {"LOCK", locked, 'k', lakeshark_fm_frequency_locked(), false},
        ls_rsel_button(fm_job()),
        ls_opt_button(fm_options()),
        {"CALLS", NULL, '5', false, false},
    };
    if (!ls_opt_count(fm_options())) buttons[6] = buttons[7];
    const int h = area.h;
    s_controls = area;
    ls_btn_bar_raised(sf, s_controls, buttons, ls_opt_count(fm_options()) ? 8 : 7, -1);
    return h;
}

static void set_squelch(double value);

static void set_volume(double value)
{
    if (!(value >= 0 && value <= 100)) return;
    ls_args_t args = {.n = 1}; ls_val_t out;
    args.v[0].kind = LS_VAL_INT; args.v[0].i = (int)(value + 0.5);
    ls_action_call("audio.volume", &args, &out, ls_quick_grant_builtin());
}

static bool control_action(int index)
{
    switch (index) {
    case 0: open_mode_picker(); return true;
    case 1: open_band_picker(); return true;
    case 2: ls_numpad_open("VOLUME", "0 to 100", audio_volume_get(),
                           set_volume); return true;
    case 3: ls_numpad_open("SQUELCH", squelch_hint(),
                           FM.squelch_tenths, set_squelch); return true;
    case 4: toggle_frequency_lock(); return true;
    case 5: open_radio(); return true;
    case 6: if (ls_opt_count(fm_options())) ls_opt_open(fm_options()); else ls_calls_open("FM"); return true;
    case 7: ls_calls_open("FM"); ls_wf_source_release(); return true;
    default: return false;
    }
}

/* Turning to SWEEP starts the sweep, which is what its own idle text always said it did. */

static void show_page(int i)
{
    s_page_open=false;
    /* A tab that is not on offer does nothing at all. Falling back to VFO
       would stop a running sweep, which is the same reaching-over this
       change exists to remove. */
    if (i == 1 && !pager_mode()) return;
    if (i==3) {s_details=false;ls_wf_source_release();return;}
    scan_engine_stop();
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
        s_view.signal = kind == SIG_NONE ? 0.0f : sig_level(kind);
        s_view.gate = (float)FM.squelch_tenths * 0.01f;
        s_view.squelch_open = kind != SIG_NONE && FM.squelch_open;
        s_view.has_squelch = kind != SIG_NONE;
    }
    fm_get_receiver_status(&s_view.receiver);
    char sql[12];
    squelch_text(sql, sizeof(sql));
    snprintf(s_view.detail[0],64,"CARRIER %s",s_view.receiver.receiver_streaming?(!s_view.has_squelch?"--":FM.squelch_open?"OPEN":"CLOSED"):"OFFLINE");
    snprintf(s_view.detail[1],64,"SQL %s  GAIN %.1f dB",sql,FM.gain_tenths/10.0);
    snprintf(s_view.detail[2],64,"VOL %d  STEP %.1fk",audio_volume_get(),tune_step_hz()/1000.0);
    snprintf(s_view.detail[3],64,"%s / MORE: mode, waterfall, pager",
             lakeshark_fm_frequency_locked() ? "FREQ LOCKED" : "FREQ FREE");
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
    if(c=='Q') {ls_numpad_open("SQUELCH",squelch_hint(),FM.squelch_tenths,set_squelch);return;}
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
    if(c) ls_quick_key(c,QUICK,N_QUICK,ls_quick_grant_builtin(),NULL);
}

/* Whether the VFO page had room for the waterfall under the box. Touch asks,
   because when there was no room ls_waterfall's plot rect still holds
   whatever the sweep page left there and would claim taps meant for the VFO. */
static bool s_vfo_sweep;

static void draw_vfo_waterfall(tui_surface *sf, tui_rect body)
{
    const int vfo_h = ls_tui_is_wide() ? 9 : VFO_ROWS;
    if (body.h < vfo_h + 6) {
        s_vfo_sweep = false;
        draw_vfo(sf, body);
        return;
    }
    s_vfo_sweep = true;
    draw_vfo(sf, tui_rect_make(body.x, body.y, body.w, vfo_h));
    draw_sweep(sf, tui_rect_make(body.x, body.y + vfo_h, body.w,
                                 body.h - vfo_h));
}

static void draw(tui_surface *sf, tui_rect area)
{
    if (ls_calls_active()) {
        snprintf(s_hint, sizeof(s_hint), "UP/DOWN calls  P play  S stop  D delete  O options  ESC back");
        ls_wf_source_release(); ls_calls_draw(sf, area); return;
    }
    lora_pager_sync();
    snprintf(s_hint,sizeof(s_hint),"%s%s",s_details?
             (ls_tui_is_wide() && (s_page==0 || s_page==2) ?
              "LEFT/RIGHT select  SPACE tune  E mode  T freq  K lock  R radio  0 scanner" :
              "E mode  T tune  K lock  N band  W sweep  R radio  0 scanner"):
             "LEFT/RIGHT tune  UP/DOWN controls  M details  R radio",
             ls_opt_count(fm_options()) ? "  O options" : "");
    if (!s_details) { radio_view(); ls_radio_panel_draw(&s_radio,&s_view,sf,area); return; }
    s_blink++;
    if (s_last_mode != (int)FM.mode) {
        s_last_mode = (int)FM.mode;
        /* Ours, arriving late: honour the page the user picked. Anyone
           else's - console, Flipper, the control head - still moves it. */
        if (s_mode_requested) s_mode_requested = false;
        else s_page = mode_page(FM.mode);
    }
    const bool wide = ls_tui_is_wide();
    /* Three rows in portrait: a two row bar with one line of text has no middle row to put it on. */

    const int bar_h = wide ? 3 : 5;
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
    int control_rows = ls_btn_raised_height(body, ls_opt_count(fm_options()) ? 8 : 7);
    if (wide && control_rows > 3) control_rows = 3;
    body.y += control_rows;
    body.h -= control_rows;

    /* The VFO page only. The pager page owns its bottom rows with UP/OPEN/
       DOWN, and the spectrum page already carries the waterfall's own two
       rows of controls; a third bar on either would be a screen that wants
       splitting rather than one more strip. */
    const int want = s_page == 0 ? ls_quick_rows(QUICK, N_QUICK, area.w, wide) : 0;
    const int ctl_h = (want && body.h > want + 12) ? want : 0;
    if (ctl_h) {
        s_quick_rect = tui_rect_make(body.x, body.y + body.h - ctl_h,
                                     body.w, ctl_h);
        body.h -= ctl_h;
    } else s_quick_rect = tui_rect_make(0, -1, 0, 0);

    if (s_page == 1 && FM.mode == FM_MODE_AIS) fm_ais_draw(sf, body);
    else if (s_page == 1 && FM.mode == FM_MODE_APRS) fm_aprs_draw(sf, body);
    else if (s_page == 1 && FM.mode == FM_MODE_SAME) fm_same_draw(sf, body);
    else if (s_page == 1) draw_pages(sf, body);
    else if (s_page == 2) draw_sweep(sf, body);
    else draw_vfo_waterfall(sf, body);
    if (s_quick_rect.h > 0)
        ls_quick_draw_posture(sf, s_quick_rect, wide, QUICK, N_QUICK);

    /* Draw navigation after the waterfall so each bar keeps a distinct hit
       slot and cannot steal the other's touch targets. */
    ls_btn_t b[N_PAGES];
    int nb = 0;
    for (int i = 0; i < N_PAGES; i++) {
        if (i == 1 && !pager_mode()) continue;   /* PAGER decodes or it hides */
        b[nb] = PAGES[i];
        if (i == 1 && FM.mode == FM_MODE_SAME) b[nb].label = "ALERTS";
        if (i == 1 && FM.mode == FM_MODE_APRS) b[nb].label = "STATIONS";
        if (i == 1 && FM.mode == FM_MODE_AIS) b[nb].label = "VESSELS";
        b[nb].on = (i == s_page);
        s_tab_page[nb] = i;
        nb++;
    }
    s_tab_count = nb;
    ls_btn_bar_slot(sf, s_bar, b, nb, -1, LS_BTN_SLOT_QUICK);
    (void)draw_controls(sf, tui_rect_make(area.x, wide ? area.y + bar_h : area.y,
                                          area.w, control_rows));
}

static void leave(void) { ls_calls_leave(); ls_wf_source_release(); lora_pager_stop(false); }

static void enter(void)
{
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
    s_page_open = false;
}

static bool key_in(ls_tk_t k, char ch)
{
    if (ls_calls_active()) return ls_calls_key(k, ch);
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
    if (!s_details) { radio_view(); radio_action(ls_radio_panel_key(&s_radio,&s_view,k,ch)); return true; }
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
    /* An open page is closed before ESC does anything else. */
    if (k==LS_TK_ESC && s_page==1 && s_page_open) { s_page_open=false; return true; }
    if (k==LS_TK_ESC || (k==LS_TK_CHAR && ch=='0')) { s_details=false; ls_wf_source_release(); return true; }
    if ((s_page==0 || s_page==2) &&
        (k==LS_TK_LEFT || k==LS_TK_RIGHT || (k==LS_TK_CHAR && ch==' ')))
        return ls_wf_key(k,ch);
    if (s_page == 2 && k == LS_TK_CHAR && (ch == 'm' || ch == 'M'))
        return ls_wf_key(k, ch);
    if (k == LS_TK_CHAR) {
        if (ch == 'e' || ch == 'E' || ch == 'm' || ch == 'M') {
            open_mode_picker(); return true;
        }
        if (ch == 'k' || ch == 'K') { toggle_frequency_lock(); return true; }
        if (ch == 'n' || ch == 'N') { open_band_picker(); return true; }
        if (ch == 'v' || ch == 'V') { control_action(2); return true; }
        if (ch == 'q' || ch == 'Q') { control_action(3); return true; }
        if (ch == 'w' || ch == 'W') { toggle_sweep(); return true; }
        if (ch == 't' || ch == 'T') {
            ls_quick_fire(&QUICK[0], ls_quick_grant_builtin()); return true;
        }
        if (ch == '[') return tune_step(-1);
        if (ch == ']') return tune_step(1);
        const int i = ls_btn_key(ch, PAGES, N_PAGES);
        if (i >= 0) { show_page(i); return true; }
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
    if (s_page == 1 && s_page_open) {
        switch (k) {
        case LS_TK_UP:    pager_move(-1); return true;
        case LS_TK_DOWN:  pager_move(1); return true;
        default: return true;
        }
    }

    switch (k) {
    case LS_TK_LEFT:  if (s_page > 0) show_page(s_page - 1); return true;
    case LS_TK_RIGHT: if (s_page < N_PAGES - 1) show_page(s_page + 1); return true;
    case LS_TK_UP:    if (s_page == 1) pager_move(-1); return true;
    case LS_TK_DOWN:
        if (s_page == 1) pager_move(1);
        return true;
    case LS_TK_ENTER:
        if (s_page == 1 && page_count() > 0) { s_page_open = true; return true; }
        return false;
    default: return false;
    }
}

static bool touch_in(int col, int row)
{
    if (ls_calls_active()) return ls_calls_touch(col, row);
    if (!s_details) { radio_view(); radio_action(ls_radio_panel_touch(&s_radio,&s_view,col,row)); return true; }
    if (tui_rect_contains(s_controls, col, row))
        return control_action(ls_btn_hit_slot(col, row, LS_BTN_SLOT_SCREEN));
    if (row >= s_bar.y && row < s_bar.y + s_bar.h) {
        const int slot = ls_btn_hit_slot(col, row, LS_BTN_SLOT_QUICK);
        if (slot >= 0 && slot < s_tab_count) { show_page(s_tab_page[slot]); return true; }
        return true;
    }
    if (s_quick_rect.h > 0 && row >= s_quick_rect.y &&
        row < s_quick_rect.y + s_quick_rect.h) {
        ls_quick_touch(col, row, QUICK, N_QUICK, ls_quick_grant_builtin(), NULL);
        return true;
    }
    /* The VFO page draws the same waterfall page 2 does, under the VFO box,
       so a tap in its plot has to reach the marker too. The key path already
       treats both pages alike - see LEFT/RIGHT/SPACE in key() - and touch did
       not, which is why arrows moved that marker and a finger did not.
       Falls through when the tap misses the plot, because the box above owns
       those rows, and is skipped entirely when the pane was too short to draw
       a waterfall at all and the plot rect is left over from page 2. */
    if (s_page == 0 && s_vfo_sweep && ls_wf_touch(col, row)) return true;
    if (s_page == 2) return ls_wf_touch(col, row);

    if (s_page == 1 && FM.mode == FM_MODE_AIS) return fm_ais_touch(col, row);
    if (s_page == 1 && FM.mode == FM_MODE_APRS) return fm_aprs_touch(col, row);
    if (s_page == 1 && FM.mode == FM_MODE_SAME) return fm_same_touch(col, row);
    if (s_page == 1) {
        for (int i = 0; i < 3; ++i) {
            if (!tui_rect_contains(s_pager_hit[i], col, row)) continue;
            if (i == 0) pager_move(-1);
            else if (i == 2) pager_move(1);
            else if (page_count() > 0) s_page_open = !s_page_open;
            return true;
        }
    }

    /* The open page: its BACK target, and nothing else - a stray tap
       must not lose the message you opened. */
    if (s_page == 1 && s_page_open) {
        if (tui_rect_contains(s_page_back,col,row))
            s_page_open = false;
        return true;
    }

    /* A page in the list: the first tap picks it and a second tap on
       the same one opens it. The rule the settings fields and the mesh node
       list already use - one tap must never fire an action, because a tap
       that both moves the cursor and does something is one you cannot take
       back. */
    if (s_page == 1 && s_page_rect.h > 0 &&
        row > s_page_rect.y && row < s_page_rect.y + s_page_rect.h - 1 &&
        col >= s_page_rect.x && col < s_page_rect.x + s_page_rect.w) {
        const int i = page_at_row(s_page_rect, row);
        if (i >= 0 && i < page_count()) {
            if (i == s_page_sel) s_page_open = true;
            else                 s_page_sel = i;
        }
        return true;
    }
    return false;
}

/* The selection is re-found before a key or tap acts on it and noted again
   after, whichever of the many places in them moved it. */
static bool key(ls_tk_t k, char ch)
{
    pager_refind();
    const bool handled = key_in(k, ch);
    pager_remember();
    return handled;
}

static bool touch(int col, int row)
{
    pager_refind();
    const bool handled = touch_in(col, row);
    pager_remember();
    return handled;
}

const ls_tui_screen_t ls_scr_fm = {
    /* the VFO, the sweep and the pager pages are all this receiver. */
    .radio = "FM",
    .name = "FM",
    .hint = s_hint,
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .key = key,
    .touch = touch,
};
