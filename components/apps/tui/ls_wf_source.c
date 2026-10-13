/* See ls_wf_source.h. The glue between a receiver and the widget. */
#include "ls_wf_source.h"
#include "ls_field.h"
#include "ls_radio_select.h"
#include "lr_iq_spectrum.h"
#include "lr_wf_session.h"
#include "scan_engine.h"

#include <string.h>

#include "esp_timer.h"
#include "esp_attr.h"

#include "ls_waterfall.h"

#include "ls_lora.h"
#include "ls_mesh.h"
#include "ls_tui_screen.h"
#include "lakeshark_backend.h"

#include "p25_state.h"
#include "iq_app_control.h"
#include "apps/p25/p25_spectrum.h"
#include "apps/fm/fm_state.h"
#include "apps/fm/fm_spectrum.h"
#include "apps/p25/p25_program.h"
#include "apps/p25/p25_profile.h"

static ls_wf_src_t s_want = LS_WF_SRC_AUTO;
static ls_wf_src_t s_active;
static uint32_t s_noaa_pending;
static bool s_lr_mode, s_lr_owned, s_lora_band_set;
static uint32_t s_live_seq, s_live_center, s_live_span, s_view_hz;
static ls_wf_owner_t s_live_owner;
void ls_wf_source_view(uint32_t hz) { s_view_hz = hz; }
static bool lr_only(void)
{
    ls_rsel_hw_t hw;
    ls_rsel_hw(&hw);
    return hw.present[LS_RSEL_LORA] && hw.lora_lr20xx &&
           !hw.present[LS_RSEL_SDR_RTL] && !hw.present[LS_RSEL_SDR_HACKRF];
}
static ls_wf_idle_t s_idle;
static const char *s_idle_reason;
ls_wf_idle_t ls_wf_source_idle(void) { return s_idle; }
const char *ls_wf_source_idle_reason(void) { return s_idle_reason; }
static bool idle(ls_wf_idle_t status, const char *reason)
{
    s_idle = status; s_idle_reason = reason;
    return false;
}

/* The measurement each source last pushed, so the same one is never pushed twice. */

static uint32_t s_fm_sweeps;
static bool     s_fm_seq_have;
static bool s_fm_was_live;
static int s_fm_preview_idx = -1;

/* One scratch frame, reused. 256 floats is a kilobyte and it is the widest
   the widget will take, so nothing here has to know the panel width. */
static EXT_RAM_BSS_ATTR float s_bins[LS_WF_BINS_MAX];

void ls_wf_source_select(ls_wf_src_t src)
{
    if (src >= LS_WF_SRC__COUNT) src = LS_WF_SRC_AUTO;
    if (src == s_want) return;
    s_want = src;
    s_live_owner = LS_WF_OWNER_NONE;

    ls_wf_claim(LS_WF_OWNER_NONE, NULL);
    /* And FM's place in its sweep count goes with it, so the first
       FM row after a selection is a sweep that finished while this was
       watching - not whatever was left in scan_db from the last time. */
    s_fm_seq_have = false;
}

static bool p25_running(void);
static bool fm_running(void);
static bool app_uses_lr(ls_rsel_job_t job)
{
    ls_rsel_hw_t hw;
    ls_rsel_hw(&hw);
    return hw.present[LS_RSEL_LORA] && hw.lora_lr20xx &&
           ls_rsel_effective(job) == LS_RSEL_LORA;
}
static bool fm_lr_tapping(void)
{
    return FM.lr_iq_active || (app_uses_lr(LS_RSEL_FM) && fm_running());
}
ls_wf_src_t ls_wf_source_get(void)
{
    /* Keep the app identity: its effective receiver decides which IQ feed to tap. */
    if (s_want == LS_WF_SRC_AUTO) {
        if (p25_running() || P25.lr_iq_active) return LS_WF_SRC_P25;
        if (fm_running() || FM.lr_iq_active) return LS_WF_SRC_FM;
        if (lr_spec_center()) return LS_WF_SRC_LORA;
    }
    if (s_want == LS_WF_SRC_P25 && P25.lr_iq_active) return LS_WF_SRC_P25;
    if (s_want == LS_WF_SRC_FM && FM.lr_iq_active) return LS_WF_SRC_FM;
    /* An explicit decoder view must never start a standalone LoRa scan
       while its receiver is starting or recovering. AUTO may own a view. */
    if (s_want != LS_WF_SRC_AUTO) return s_want;
    if (lr_only() && !p25_running() && !fm_running()) return LS_WF_SRC_LORA;
    return p25_running() ? LS_WF_SRC_P25
         : fm_running() ? LS_WF_SRC_FM : LS_WF_SRC_AUTO;
}

const char *ls_wf_source_name(void)
{
    if (s_active == LS_WF_SRC_AUTO) return "none";
    if (s_active == LS_WF_SRC_FM && s_lr_mode && s_live_owner == LS_WF_OWNER_LORA)
        return "LR IQ";
    return ls_wf_source_label(s_active);
}

const char *ls_wf_source_label(ls_wf_src_t src)
{
    switch (src) {
    case LS_WF_SRC_P25:  return "P25";
    case LS_WF_SRC_FM:   return "FM";
    case LS_WF_SRC_LORA: return s_lr_mode ? "LR IQ" : "LR SWEEP";
    default:             return "AUTO";
    }
}

static uint32_t s_lora_min_hz = 902000000u;
static uint32_t s_lora_max_hz = 928000000u;
/* When the LoRa row being built began, or 0 when none is. Up here
   because a band change starts the next row over. */
static int64_t  s_lora_row_t0;

/* THE WINDOW THE SWEEP IS DRAWN THROUGH, tracked rather than fixed.

   A fixed -130/-40 was the original. A quiet ISM band is about
   thirteen decibels wide - a measured 902-928 sweep read -107 to -94 -
   so the whole band arrived inside a seventh of a ninety decibel
   window, low in it. The renderer then estimates its floor as the 25th
   percentile less six, which on a distribution that tight lands above
   the weakest bins: nineteen of sixty-four drawn as nothing, five of
   sixteen levels ever reached. Measured, on the board, with tui cost.

   Per-row minimum and maximum was tried next and was worse. Every row
   gets its own scale, so identical noise renders at a different
   brightness depending on whether that row happened to catch a peak,
   and the history stops being comparable - which is the whole point of
   a waterfall rather than a spectrum.

   So: tracked across rows and moved an eighth of the way each time,
   the same smoothing the renderer uses on its own window. The band
   sets the scale, and it sets it slowly enough that two rows a second
   apart still mean the same thing. Reset on a band change, because the
   next band is not this one. */
static float s_lora_floor_db = -120.0f, s_lora_top_db = -60.0f;
static bool  s_lora_window_seeded;

void ls_wf_source_lora_band(uint32_t min_hz, uint32_t max_hz)
{
    if (max_hz <= min_hz) return;
    if (!ls_lora_rx_range_ok(ls_lora_caps(), min_hz, max_hz)) return;
    s_lora_band_set = true;
    s_lora_min_hz = min_hz;
    s_lora_max_hz = max_hz;
}

void ls_wf_source_lora_band_get(uint32_t *min_hz, uint32_t *max_hz)
{
    if (min_hz) *min_hz = s_lora_min_hz;
    if (max_hz) *max_hz = s_lora_max_hz;
}

/* What can NEVER run, which is a much shorter list than it looks. */

/* ------------------------------------------------------------- presets -- */

typedef struct {
    const char *name;
    uint32_t    lo_hz;
    uint32_t    hi_hz;
} wf_band_t;

/* The first six are every LoRa chip's. After them, the bands only a part
   that sweeps past 960 MHz can reach, in the order the capabilities unlock
   them: the rest of the LF input, then the HF input. */
static const wf_band_t LORA_BANDS[] = {
    { "mesh watch",  909500000u, 911500000u },
    { "US915 ISM",   LS_MESH_US915_MIN_HZ, LS_MESH_US915_MAX_HZ },
    { "EU868",       863000000u, 870000000u },
    { "433 ISM",     433050000u, 434790000u },
    { "315 remotes", 314000000u, 316000000u },
    { "full range",  150000000u, 960000000u },
    { "aviation",    960000000u, 1100000000u },
    { "LF input",    150000000u, 1100000000u },
    { "GPS L1",     1570000000u, 1581000000u },
    { "Iridium",    1616000000u, 1626500000u },
    { "2.4G ISM",   2400000000u, 2483500000u },
    { "HF input",   1500000000u, 2500000000u },
};
#define LORA_BAND_ALL ((int)(sizeof(LORA_BANDS) / sizeof(LORA_BANDS[0])))
#define LORA_BAND_SX  6
#define LORA_BAND_LF  8

/* How many of LORA_BANDS the radio fitted can sweep. */
static int lora_band_n(void)
{
    const uint32_t caps = ls_lora_caps();
    if (!(caps & LS_LORA_CAP_RX_WIDE)) return LORA_BAND_SX;
    return (caps & LS_LORA_CAP_BAND_1G5_2G5) ? LORA_BAND_ALL : LORA_BAND_LF;
}
#define LORA_BAND_N lora_band_n()

static const wf_band_t FM_BANDS[] = {
    { "VHF land",    150000000u, 162000000u },
    { "FM bcast",     88000000u, 108000000u },
    { "Air AM",      118000000u, 137000000u },
    { "2 m ham",     144000000u, 148000000u },
    { "marine",      156000000u, 162000000u },
    { "UHF land",    450000000u, 470000000u },
    { "70 cm ham",   420000000u, 450000000u },
    { "pagers",      929000000u, 932000000u },
    { "Mil air AM",  225000000u, 400000000u },
    { "CB AM",        26965000u,  27405000u },
    { "10m AM",       29000000u,  29200000u },
    { "NOAA 162.400", 162400000u, 162400000u },
    { "NOAA 162.425", 162425000u, 162425000u },
    { "NOAA 162.450", 162450000u, 162450000u },
    { "NOAA 162.475", 162475000u, 162475000u },
    { "NOAA 162.500", 162500000u, 162500000u },
    { "NOAA 162.525", 162525000u, 162525000u },
    { "NOAA 162.550", 162550000u, 162550000u },

};
#define FM_BAND_N ((int)(sizeof(FM_BANDS) / sizeof(FM_BANDS[0])))

static const wf_band_t *band_table(ls_wf_src_t src, int *n)
{
    switch (src) {
    case LS_WF_SRC_LORA: *n = LORA_BAND_N; return LORA_BANDS;
    case LS_WF_SRC_FM:   *n = FM_BAND_N;   return FM_BANDS;
    default:             *n = 0;           return NULL;
    }
}

static int p25_control_count(void)
{
    const p25_program_t *pg = p25_program_session();
    if (!pg || !pg->active_valid) return 0;
    return (int)pg->active.control_count;
}

int ls_wf_preset_count(ls_wf_src_t src)
{
    int n = 0;
    if (band_table(src, &n)) return n;
    if (src == LS_WF_SRC_P25) return p25_control_count();
    return 0;
}

const char *ls_wf_preset_none(ls_wf_src_t src)
{
    if (src == LS_WF_SRC_P25 && p25_control_count() == 0)
        return "no profile loaded - put one on the card";
    if (ls_wf_preset_count(src) == 0) return "nothing to point this at";
    return NULL;
}

static char s_pre_label[24];
static char s_pre_detail[16];

const char *ls_wf_preset_label(ls_wf_src_t src, int i)
{
    int n = 0;
    const wf_band_t *t = band_table(src, &n);
    if (t) {
        if (i < 0 || i >= n) return "";
        return t[i].name;
    }
    if (src == LS_WF_SRC_P25) {
        const p25_program_t *pg = p25_program_session();
        if (!pg || i < 0 || i >= (int)pg->active.control_count) return "";
        snprintf(s_pre_label, sizeof(s_pre_label), "%.4f MHz%s",
                 (double)pg->active.control_channels[i] / 1e6,
                 i == (int)pg->selected_control ? " *" : "");
        return s_pre_label;
    }
    return "";
}

const char *ls_wf_preset_detail(ls_wf_src_t src, int i)
{
    int n = 0;
    const wf_band_t *t = band_table(src, &n);
    if (t) {
        if (i < 0 || i >= n) return "";
        /* The span, not the endpoints: the endpoints are most of the label's
           information already and twelve columns will not hold both. */
        const bool fractional = t[i].lo_hz % 1000000u || t[i].hi_hz % 1000000u;
        /* Above 1 GHz a third decimal no longer fits, and the bands there are
           never closer than 100 kHz. */
        const bool ghz = t[i].hi_hz >= 1000000000u;
        snprintf(s_pre_detail, sizeof(s_pre_detail),
                 !fractional ? "%.0f-%.0f" : ghz ? "%.1f-%.1f" : "%.3f-%.3f",
                 t[i].lo_hz / 1e6, t[i].hi_hz / 1e6);
        return s_pre_detail;
    }
    if (src == LS_WF_SRC_P25) return "control";
    return "";
}

bool ls_wf_preset_apply(ls_wf_src_t src, int i)
{
    int n = 0;
    const wf_band_t *t = band_table(src, &n);
    if (t) {
        if (i < 0 || i >= n) return false;
        if (src == LS_WF_SRC_LORA) {
            ls_wf_source_lora_band(t[i].lo_hz, t[i].hi_hz);

            /* The PICTURE restarts, once; the radio is retuned in place. */

            ls_wf_claim(LS_WF_OWNER_NONE, NULL);
            s_lora_row_t0 = 0;
            s_lora_window_seeded = false;
            return true;
        }
        if (t[i].lo_hz == t[i].hi_hz) {
            if (FM.mode != FM_MODE_LISTEN) {
                s_noaa_pending = t[i].lo_hz;
                lakeshark_fm_set_mode(FM_MODE_LISTEN);
                return true;
            }
            if (!lakeshark_fm_set_freq(t[i].lo_hz)) return false;
        }
        FM.scan_start_hz = t[i].lo_hz;
        FM.scan_stop_hz  = t[i].hi_hz;
        if (FM.mode == FM_MODE_SCAN) {
            lakeshark_fm_scan_restart();
        } else if (FM.freq_hz < t[i].lo_hz || FM.freq_hz > t[i].hi_hz) {
            lakeshark_fm_set_freq(t[i].lo_hz + (t[i].hi_hz - t[i].lo_hz) / 2);
        }
        ls_wf_claim(LS_WF_OWNER_NONE, NULL);
        s_fm_seq_have = false;   /* see ls_wf_source_select */
        return true;
    }
    if (src == LS_WF_SRC_P25) {
        const p25_program_t *pg = p25_program_session();
        if (!pg || i < 0 || i >= (int)pg->active.control_count) return false;

        const int delta = i - (int)pg->selected_control;
        if (delta) p25_program_step_control_now(delta);
        return true;
    }
    return false;
}

static fm_mode_t s_fm_live_mode = FM_MODE_LISTEN;

void ls_wf_fm_sweep(bool on)
{
    if (on && FM.mode != FM_MODE_SCAN) s_fm_live_mode = FM.mode;
    if (on && lakeshark_fm_frequency_locked()) {
        /* LOCK means centre, not "start again at the VHF default". Preserve
           the selected span but centre it on the carrier the user typed. */
        const uint32_t center = lakeshark_fm_frequency_lock_hz();
        uint32_t span = FM.scan_stop_hz > FM.scan_start_hz
                      ? FM.scan_stop_hz - FM.scan_start_hz : 12000000u;
        if (span < 100000u || span > 100000000u) span = 12000000u;
        const uint32_t floor_hz = 24000000u, ceiling_hz = 1766000000u;
        uint64_t lo = center > span / 2 ? center - span / 2 : floor_hz;
        uint64_t hi = lo + span;
        if (lo < floor_hz) { lo = floor_hz; hi = lo + span; }
        if (hi > ceiling_hz) { hi = ceiling_hz; lo = hi - span; }
        FM.scan_start_hz = (uint32_t)lo;
        FM.scan_stop_hz = (uint32_t)hi;
    }
    lakeshark_fm_set_mode(on ? FM_MODE_SCAN : s_fm_live_mode);
    if (on) lakeshark_fm_scan_restart();
    ls_wf_claim(LS_WF_OWNER_NONE, NULL);
    s_fm_seq_have = false;
}

const char *ls_wf_preset_current(ls_wf_src_t src)
{
    if (src == LS_WF_SRC_LORA) {
        if (s_lr_mode && lr_spec_center()) return "LR IQ 125k";
        uint32_t lo = 0, hi = 0;
        ls_wf_source_lora_band_get(&lo, &hi);
        for (int i = 0; i < LORA_BAND_N; i++)
            if (LORA_BANDS[i].lo_hz == lo && LORA_BANDS[i].hi_hz == hi)
                return LORA_BANDS[i].name;
        snprintf(s_pre_label, sizeof(s_pre_label), "%.0f-%.0f",
                 lo / 1e6, hi / 1e6);
        return s_pre_label;
    }
    if (src == LS_WF_SRC_FM) {
        for (int i = 0; i < FM_BAND_N; i++)
            if (FM_BANDS[i].lo_hz == FM.scan_start_hz &&
                FM_BANDS[i].hi_hz == FM.scan_stop_hz)
                return FM_BANDS[i].name;
        snprintf(s_pre_label, sizeof(s_pre_label), "%.0f-%.0f",
                 FM.scan_start_hz / 1e6, FM.scan_stop_hz / 1e6);
        return s_pre_label;
    }
    if (src == LS_WF_SRC_P25) {
        const p25_program_t *pg = p25_program_session();
        if (!pg || !pg->active_valid || !pg->active.control_count)
            return "no profile";
        const uint8_t k = pg->selected_control < pg->active.control_count
                        ? pg->selected_control : 0;
        snprintf(s_pre_label, sizeof(s_pre_label), "%.4f",
                 (double)pg->active.control_channels[k] / 1e6);
        return s_pre_label;
    }
    return "-";
}

const char *ls_wf_source_blocked(ls_wf_src_t src)
{
    if (src == LS_WF_SRC_LORA && ls_field_owned()) return "LoRa Labs is returning the radio";
    if (src == LS_WF_SRC_LORA && !ls_lora_present())
        return "no LoRa radio on this board";
    return NULL;
}

bool ls_wf_source_start(ls_wf_src_t src)
{
    if (ls_wf_source_blocked(src)) return false;

    switch (src) {
    case LS_WF_SRC_P25:
        /* Through the same claim the P25 screen makes, so the receiver is
           started once and by one route. */
        ls_tui_radio_want("P25");
        break;
    case LS_WF_SRC_FM:
        ls_tui_radio_want("FM");
        break;
    case LS_WF_SRC_LORA:
        /* Nothing to start: the sweep IS this module, and it begins on the
           first pump. */
        break;
    default:
        break;
    }
    ls_wf_source_select(src);
    return true;
}

/* Ask the module, do not remember its answer. */

static void p25_feed(bool on)
{
    if (p25_spectrum_enabled() == on) return;
    /* Turning the feed off invalidates the generation, so the first
       frame after it comes back is a new measurement even if its sequence
       number happens to match the one before. */
    s_live_owner = LS_WF_OWNER_NONE;
    p25_spectrum_enable(on);
}

static bool p25_running(void)
{
    ls_iq_control_status_t st;
    memset(&st, 0, sizeof(st));
    p25_get_receiver_status(&st);
    return st.receiver_streaming;
}

static bool push_live(ls_wf_owner_t owner, uint32_t sequence, const ls_wf_feed_t *feed)
{
    if (s_live_owner == owner && s_live_seq == sequence) return true;
    if (s_live_owner != owner || s_live_center != feed->center_hz || s_live_span != feed->span_hz)
        ls_wf_claim(LS_WF_OWNER_NONE, NULL);
    s_live_seq = sequence; s_live_owner = owner;
    s_live_center = feed->center_hz; s_live_span = feed->span_hz;
    ls_wf_claim(owner, owner == LS_WF_OWNER_LORA ? "LR IQ" : owner == LS_WF_OWNER_P25 ? "P25" : "FM");
    ls_wf_push(owner, s_bins, LS_WF_BINS_MAX, feed);
    return true;
}

static bool pump_lr_tap(void);
static bool pump_p25(void)
{
    if (app_uses_lr(LS_RSEL_P25)) {
        p25_feed(false);
        if (!P25.lr_iq_active) {
            lr_spec_enable(false);
            return idle(LS_WF_IDLE_NONE, "LR FSK: no IQ");
        }
        return pump_lr_tap();
    }
    p25_feed(true);

    p25_spectrum_snapshot_t snap;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!p25_spectrum_read(s_bins, LS_WF_BINS_MAX, now, 500, &snap))
        return false;

    ls_wf_feed_t f = {
        .center_hz = snap.center_hz,
        .span_hz   = snap.span_hz,
        .floor_db  = P25_SPECTRUM_FLOOR_DB,
        .top_db    = P25_SPECTRUM_TOP_DB,
        .live      = true,
        .note      = snap.following_voice ? "following voice" : "control",
    };
    return push_live(LS_WF_OWNER_P25, snap.sequence, &f);
}

static bool fm_sweeping(void)
{
    if (FM.mode != FM_MODE_SCAN || FM.scan_bins <= 0) return false;
    ls_iq_control_status_t st;
    memset(&st, 0, sizeof(st));
    fm_get_receiver_status(&st);
    return st.receiver_streaming;
}

static bool fm_running(void)
{
    ls_iq_control_status_t st;
    fm_get_receiver_status(&st);
    return st.receiver_streaming;
}

static bool pump_fm_live(void)
{
    fm_spectrum_enable(true);
    fm_spectrum_snapshot_t snap;
    if (!fm_spectrum_read(s_bins, LS_WF_BINS_MAX,
                          (uint32_t)(esp_timer_get_time() / 1000), &snap))
        return false;
    ls_wf_feed_t feed = {
        .center_hz = snap.center_hz, .span_hz = snap.span_hz,
        .floor_db = FM_SPECTRUM_FLOOR_DB, .top_db = FM_SPECTRUM_TOP_DB,
        .live = true, .note = "live IQ", .period_ms = FM_SPECTRUM_PERIOD_MS,
    };
    return push_live(LS_WF_OWNER_FM, snap.sequence, &feed);
}

static bool pump_fm(void)
{
    if (app_uses_lr(LS_RSEL_FM) || FM.lr_iq_active) {
        fm_spectrum_enable(false);
        return fm_lr_tapping() && pump_lr_tap();
    }
    const bool live = FM.mode != FM_MODE_SCAN;
    if (live != s_fm_was_live) {
        ls_wf_claim(LS_WF_OWNER_NONE, NULL);
        s_live_owner = LS_WF_OWNER_NONE;
        s_fm_seq_have = false;
        s_fm_was_live = live;
    }
    if (live) return fm_running() && pump_fm_live();
    fm_spectrum_enable(false);
    if (!fm_sweeping()) return false;
    int bins = FM.scan_bins;
    if (bins > FM_SCAN_BINS_MAX) bins = FM_SCAN_BINS_MAX;

    ls_wf_claim(LS_WF_OWNER_FM, "FM");

    /* One row per sweep, for the same reason P25 gets one row per
       measurement. FM publishes scan_sweeps, which is the count of completed
       passes, so the row that goes into the history is the pass that
       produced it. */
    /* And the first look only notes where the count stands. What is
       in scan_db at that moment is a sweep from before anyone was watching -
       or from before FM was switched away and back - and a waterfall is a
       record of what was measured while it ran. */
    bool completed = false;
    if (!s_fm_seq_have) {
        s_fm_sweeps   = FM.scan_sweeps;
        s_fm_seq_have = true;
        s_fm_preview_idx = -1;
    } else if (FM.scan_sweeps != s_fm_sweeps) {
        completed = true;
    }
    if (!completed && FM.scan_idx == s_fm_preview_idx) return true;
    s_fm_preview_idx = FM.scan_idx;
    s_fm_sweeps = FM.scan_sweeps;

    /* scan_db is already the 0..1 the widget takes, on
       FM_SCAN_FLOOR_DB..FM_SCAN_TOP_DB - app_fm normalises as it paints.
       Reading it as dBFS is what put every bin at full scale. So this only
       resamples, by MAX as the widget does, and passes the scale on for the
       readout. */
    const int n = bins < LS_WF_BINS_MAX ? bins : LS_WF_BINS_MAX;
    for (int i = 0; i < n; i++) {
        const int lo = i * bins / n;
        int hi = (i + 1) * bins / n;
        if (hi <= lo) hi = lo + 1;
        float peak = 0.0f;
        for (int b = lo; b < hi && b < bins; b++)
            if (FM.scan_db[b] > peak) peak = FM.scan_db[b];
        s_bins[i] = peak > 1.0f ? 1.0f : peak;
    }

    const uint32_t lo_hz = FM.scan_start_hz, hi_hz = FM.scan_stop_hz;
    ls_wf_feed_t f = {
        .center_hz = (hi_hz > lo_hz) ? (lo_hz + (hi_hz - lo_hz) / 2) : FM.freq_hz,
        .span_hz   = (hi_hz > lo_hz) ? (hi_hz - lo_hz) : 0,
        .floor_db  = FM_SCAN_FLOOR_DB,
        .top_db    = FM_SCAN_TOP_DB,
        .live      = true,
        .note      = "sweep",
        .period_ms = FM.scan_sweep_ms,   /**/
    };
    if (completed) ls_wf_push(LS_WF_OWNER_FM, s_bins, n, &f);
    else if (FM.scan_idx > 0) ls_wf_preview(LS_WF_OWNER_FM, s_bins, n, &f);
    return true;
}

const char *ls_wf_source_progress(void)
{
        static char progress[64];
    if (ls_wf_source_get() != LS_WF_SRC_FM || !fm_sweeping() || FM.scan_tunes <= 0)
        return NULL;
    int completed = FM.scan_idx;
    if (completed < 0) completed = 0;
    if (completed > FM.scan_tunes) completed = FM.scan_tunes;
    snprintf(progress, sizeof(progress), "FM sweeping: %d%% (%d/%d)",
             completed * 100 / FM.scan_tunes, completed, FM.scan_tunes);
    return progress;
}

/* One pass of the sweep per call, which is one row per frame. */

static bool s_lora_held, s_lora_owned;
/* Still one pass per call. What changed is that a row is one pass
   only while the filter covers each bin's slice; on a band too wide for
   that (full range: 26 passes) the driver builds the row a look at a time
   and says when it is complete. In between, the source is working and has
   nothing new to write - the same answer P25 gives between measurements. */

static void lora_stop(void);
static bool pump_lr_line(void)
{
    lr_spec_line snap;
    if (!lr_spec_read(s_bins, &snap, (uint32_t)(esp_timer_get_time() / 1000)))
        return idle(LS_WF_IDLE_WAITING, "Waiting for LR IQ");
    ls_wf_feed_t f = {
        .center_hz = snap.center_hz, .span_hz = 125000,
        .floor_db = -100, .top_db = 0, .live = true,
        .note = snap.gap ? "IQ gap" : "live IQ", .period_ms = LR_SPEC_PERIOD_MS,
        .iq = true, .gap = snap.gap,
    };
    return push_live(LS_WF_OWNER_LORA, snap.sequence, &f);
}

static bool pump_lr_tap(void)
{
    s_lr_mode = true;
    if (!lr_spec_enable(true)) return idle(LS_WF_IDLE_FAILED, "LR IQ memory unavailable");
    return pump_lr_line();
}

static bool lora_hold(void)
{
    /* P25 or ADS-B left listening on the chip gives it up when asked below. */
    if (ls_lora_fsk_active() && !ls_mesh_radio_bg())
        return idle(LS_WF_IDLE_BUSY, "LoRa busy: stop FSK receiver");
    if (!ls_lora_present()) return idle(LS_WF_IDLE_FAILED, "No LoRa radio on this board");

    if (ls_field_owned()) return idle(LS_WF_IDLE_BUSY, "LoRa busy: stop Labs DIRECT");
    if (!s_lora_held) {
        if (ls_lora_scanning()) return idle(LS_WF_IDLE_BUSY, "LoRa busy: another scan");
        ls_mesh_radio_hold(true);
        s_lora_held = true;
    }
    if (!ls_mesh_radio_held()) return idle(LS_WF_IDLE_STARTING, "Waiting for mesh release");
    return true;
}

static bool pump_lora_iq(uint32_t hz)
{
    s_lr_mode = true;
    if (!lr_spec_enable(true)) return idle(LS_WF_IDLE_FAILED, "LR IQ memory unavailable");
    /* An existing decoder owns capture and tuning. Only tap its checked blocks. */
    if (lr_spec_center() && !s_lr_owned) return pump_lr_line();
    /* Ours already: the capture holds the chip's SPI lock for its whole
       session, so asking the chip anything here (lora_hold's FSK check)
       would wait on this very stream forever. Same frequency is a no-op;
       a new one restarts the session without touching the chip first. */
    if (s_lr_owned) {
        if (lr_wf_start(hz) != ESP_OK) { lora_stop(); return idle(LS_WF_IDLE_FAILED, "LR IQ session failed"); }
        return pump_lr_line();
    }
    if ((P25.lr_iq_active || FM.lr_iq_active) && !s_lr_owned)
        return idle(LS_WF_IDLE_WAITING, "Waiting for LR IQ");
    if (hz < 150000000u || hz > 960000000u) {
        lora_stop(); return idle(LS_WF_IDLE_FAILED, "LR tunes 150-960 MHz");
    }
    if (s_want == LS_WF_SRC_FM && FM.mode == FM_MODE_LISTEN && lr_only() && !s_lr_owned)
        return idle(LS_WF_IDLE_WAITING, "Waiting for FM LR IQ");
    if (!lora_hold()) return false;
    if (lr_wf_start(hz) != ESP_OK) return idle(LS_WF_IDLE_FAILED, "LR IQ session failed");
    s_lr_owned = true;
    return pump_lr_line();
}

static bool pump_lora(void)
{
    uint32_t hz = 0, span = 125000;
    const ls_lora_cfg_t *cfg = ls_lora_cfg();
    if (s_want == LS_WF_SRC_FM) {
        hz = FM.freq_hz;
        if (FM.mode == FM_MODE_SCAN && FM.scan_stop_hz > FM.scan_start_hz) {
            span = FM.scan_stop_hz - FM.scan_start_hz;
            s_lora_min_hz = FM.scan_start_hz; s_lora_max_hz = FM.scan_stop_hz;
        }
    } else if (s_want == LS_WF_SRC_P25) hz = s_tune_freq_hz;
    else if (s_view_hz) hz = s_view_hz;
    else if (s_lora_band_set) {
        span = s_lora_max_hz - s_lora_min_hz;
        hz = s_lora_min_hz + span / 2;
    } else if (cfg) hz = cfg->freq_hz;
    ls_rsel_hw_t hw; ls_rsel_hw(&hw);
    if (lr_spec_center() && !s_lr_owned) return pump_lora_iq(lr_spec_center());
    const bool iq = hw.lora_lr20xx && span <= 125000;
    if (iq != s_lr_mode) { lora_stop(); ls_wf_claim(LS_WF_OWNER_NONE, NULL); }
    if (iq) return pump_lora_iq(hz);
    s_lr_mode = false; lr_spec_enable(false);
    if (hw.lora_lr20xx && (s_lora_min_hz < 150000000u || s_lora_max_hz > 960000000u))
        return idle(LS_WF_IDLE_FAILED, "LR tunes 150-960 MHz");
    if (!lora_hold()) return false;
    if (ls_lora_scan_begin(s_lora_min_hz, s_lora_max_hz) != ESP_OK)
        return idle(ls_lora_scanning() ? LS_WF_IDLE_BUSY : LS_WF_IDLE_FAILED,
                    ls_lora_scanning() ? "LoRa busy: another scan" : "LoRa scan failed; retry RADIO");
    s_lora_owned = true;

    const int n = LS_LORA_SCAN_BINS < LS_WF_BINS_MAX ? LS_LORA_SCAN_BINS
                                                     : LS_WF_BINS_MAX;
    static EXT_RAM_BSS_ATTR float dbm[LS_LORA_SCAN_BINS];
    if (!s_lora_row_t0) s_lora_row_t0 = esp_timer_get_time();
    bool row_done = false;
    const int got = ls_lora_scan_pass(dbm, n, &row_done);
    if (got <= 0) return idle(LS_WF_IDLE_FAILED, "LoRa scan read failed");

    /* Claimed on the first pass, so a row that takes 26 of them is 26 frames
       of "waiting for the receiver" and not of "no receiver". */
    ls_wf_claim(LS_WF_OWNER_LORA, "LR SWEEP");
    if (!row_done) return true;

    /* How long this row took, passes and frames between them
       included, so the stall notice knows what late means for this band. */
    const uint32_t period_ms =
        (uint32_t)((esp_timer_get_time() - s_lora_row_t0) / 1000);
    s_lora_row_t0 = 0;

    /* This row first, then the window it moves. A twelve dB minimum
       span keeps a genuinely flat band from being amplified into the
       receiver's own scatter, and two dB of headroom below the
       weakest bin keeps that bin visible instead of sitting exactly
       on the floor. */
    float row_lo = dbm[0], row_hi = dbm[0];
    for (int i = 1; i < got; i++) {
        if (dbm[i] < row_lo) row_lo = dbm[i];
        if (dbm[i] > row_hi) row_hi = dbm[i];
    }
    /* The SX1262 reports RSSI in half decibels, so a thirteen dB band is
       about twenty-six distinct values. Stretched across a window much
       narrower than that, consecutive values jump more than one level and
       the ramp posterises - the histogram comes back with every other
       bucket empty. A wider minimum span keeps a step below one level. */
    row_lo -= 4.0f;
    if (row_hi - row_lo < 26.0f) row_hi = row_lo + 26.0f;
    if (!s_lora_window_seeded) {
        s_lora_window_seeded = true;
        s_lora_floor_db = row_lo;
        s_lora_top_db   = row_hi;
    } else {
        s_lora_floor_db += (row_lo - s_lora_floor_db) / 8.0f;
        s_lora_top_db   += (row_hi - s_lora_top_db)   / 8.0f;
    }
    const float floor_db = s_lora_floor_db, top_db = s_lora_top_db;
    for (int i = 0; i < got; i++) {
        float v = (dbm[i] - floor_db) / (top_db - floor_db);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        s_bins[i] = v;
    }

    ls_wf_feed_t f = {
        .center_hz = s_lora_min_hz + (s_lora_max_hz - s_lora_min_hz) / 2,
        .span_hz   = s_lora_max_hz - s_lora_min_hz,
        .floor_db  = floor_db,
        .top_db    = top_db,
        .live      = true,
        .note      = "swept",
        .period_ms = period_ms,
        .scaled    = true,
    };
    ls_wf_push(LS_WF_OWNER_LORA, s_bins, got, &f);
    return true;
}

static void lora_stop(void)
{
    if (s_lr_owned) { lr_wf_stop(); s_lr_owned = false; }
    if (s_live_owner == LS_WF_OWNER_LORA) s_live_owner = LS_WF_OWNER_NONE;
    if (s_lora_owned && ls_lora_scanning()) ls_lora_scan_end();
    if (s_lora_held) {
        ls_mesh_radio_hold(false);
        s_lora_held = false;
    }
    s_lora_owned = false;
    s_lora_row_t0 = 0;
}

extern bool ls_scr_labs_tune_marker(uint32_t hz);

static bool tune_marker(ls_wf_owner_t owner,uint32_t hz)
{
    if(owner==LS_WF_OWNER_P25) {
        ls_iq_control_status_t status;p25_get_receiver_status(&status);
        if(!p25_running()||!status.receiver_streaming||hz<24000000||hz>1766000000)return false;
        scan_engine_stop();lakeshark_p25_set_freq(hz);return true;
    }
    if(owner==LS_WF_OWNER_FM) {
        ls_iq_control_status_t status;fm_get_receiver_status(&status);
        if(!fm_running()||!status.receiver_streaming||hz<24000000||hz>1766000000)return false;
        if (lakeshark_fm_frequency_locked() && hz != lakeshark_fm_frequency_lock_hz()) return false;
        if (!lakeshark_fm_set_freq(hz)) return false;
        scan_engine_stop();
        if(FM.mode==FM_MODE_SCAN)ls_wf_fm_sweep(false);
        return true;
    }
    if(owner==LS_WF_OWNER_LORA) {
        if (s_lr_mode) {
            /* Decoder-owned sessions keep their own tuner; never hand them to Labs. */
            if (hz < 150000000u || hz > 960000000u) return false;
            if (FM.lr_iq_active && s_want == LS_WF_SRC_FM)
                return lakeshark_fm_set_freq(hz);
            if (!s_lr_owned) return false;
            if (s_want == LS_WF_SRC_FM) return lakeshark_fm_set_freq(hz);
            if (s_want == LS_WF_SRC_P25) lakeshark_p25_set_freq(hz);
            else s_view_hz = hz;
            return true;
        }
        if (!ls_lora_present() || !ls_lora_packet_rx_hz_ok(ls_lora_caps(), hz) ||
            (hz > 959000000u && hz < 1900000000u)) return false;
        return ls_scr_labs_tune_marker(hz);
    }
    return false;
}

void ls_wf_source_pump(void)
{
    if (s_noaa_pending && FM.mode == FM_MODE_LISTEN) {
        uint32_t hz = s_noaa_pending; s_noaa_pending = 0;
        lakeshark_fm_set_freq(hz);
    }
    s_idle = LS_WF_IDLE_WAITING;
    s_idle_reason = "Waiting for first row";
    uint32_t marker = ls_wf_marker_hz();
    bool packet_band = marker ? ls_lora_packet_rx_hz_ok(ls_lora_caps(), marker) &&
                              (marker <= 959000000u || marker >= 1900000000u) :
                       s_lora_min_hz <= 959000000u ||
                       (s_lora_max_hz >= 1900000000u &&
                        ls_lora_packet_rx_hz_ok(ls_lora_caps(), s_lora_max_hz));
    ls_wf_set_tuner(ls_wf_source_get() == LS_WF_SRC_LORA && !packet_band ? NULL : tune_marker);
    ls_wf_src_t use = ls_wf_source_get();

    if (use != LS_WF_SRC_P25)  p25_feed(false);
    if (use != LS_WF_SRC_FM) fm_spectrum_enable(false);
    /* And gives the radio back, which matters more than turning a
       feed off: a mesh left parked is a node that has stopped answering. */
    if (use != LS_WF_SRC_LORA) {
        const bool lr_app = (use == LS_WF_SRC_P25 && app_uses_lr(LS_RSEL_P25) && P25.lr_iq_active) ||
                            (use == LS_WF_SRC_FM && fm_lr_tapping());
        if (!lr_app) lr_spec_enable(false);
        if (!lr_app || s_lr_owned || s_lora_held || s_lora_owned) lora_stop();
        /* Decoder taps do not own field DIRECT. */
    }

    bool ok = false;
    switch (use) {
    case LS_WF_SRC_P25:  ok = pump_p25();  break;
    case LS_WF_SRC_FM:   ok = pump_fm();   break;
    case LS_WF_SRC_LORA: ok = pump_lora(); break;
    default: idle(LS_WF_IDLE_NONE, "No receiver selected"); break;
    }
    if ((use == LS_WF_SRC_FM && !fm_running() && !FM.lr_iq_active) ||
        (use == LS_WF_SRC_P25 && !p25_running() && !P25.lr_iq_active))
        idle(LS_WF_IDLE_STARTING, "Waiting for receiver startup");
    s_active = ok ? use : LS_WF_SRC_AUTO;
}

void ls_wf_source_release(void)
{
    s_view_hz = 0; s_lr_mode = false;
    lr_spec_enable(false);
    /* No field DIRECT ownership was acquired here. */
    fm_spectrum_enable(false);
    s_live_owner = LS_WF_OWNER_NONE;
    p25_feed(false);
    lora_stop();
    s_fm_seq_have = false;
    s_active = LS_WF_SRC_AUTO;
}
