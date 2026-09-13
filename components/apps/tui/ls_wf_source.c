/* See ls_wf_source.h. The glue between a receiver and the widget. */
#include "ls_wf_source.h"
#include "ls_field.h"

#include <string.h>

#include "esp_timer.h"

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

/* The measurement each source last pushed, so the same one is never pushed twice. */

static uint32_t s_p25_seq;
static bool     s_p25_seq_have;
static uint32_t s_fm_sweeps;
static bool     s_fm_seq_have;
static uint32_t s_fm_live_seq;
static uint32_t s_fm_live_center, s_fm_live_span;
static bool s_fm_was_live;
static int s_fm_preview_idx = -1;

/* One scratch frame, reused. 256 floats is a kilobyte and it is the widest
   the widget will take, so nothing here has to know the panel width. */
static float s_bins[LS_WF_BINS_MAX];

void ls_wf_source_select(ls_wf_src_t src)
{
    if (src >= LS_WF_SRC__COUNT) src = LS_WF_SRC_AUTO;
    if (src == s_want) return;
    s_want = src;

    ls_wf_claim(LS_WF_OWNER_NONE, NULL);
    /* And FM's place in its sweep count goes with it, so the first
       FM row after a selection is a sweep that finished while this was
       watching - not whatever was left in scan_db from the last time. */
    s_fm_seq_have = false;
}

ls_wf_src_t ls_wf_source_get(void) { return s_want; }

const char *ls_wf_source_name(void)
{
    switch (s_active) {
    case LS_WF_SRC_P25:  return "P25";
    case LS_WF_SRC_FM:   return "FM";
    case LS_WF_SRC_LORA: return "LORA";
    default:             return "none";
    }
}

static bool p25_running(void);

const char *ls_wf_source_label(ls_wf_src_t src)
{
    switch (src) {
    case LS_WF_SRC_P25:  return "P25";
    case LS_WF_SRC_FM:   return "FM";
    case LS_WF_SRC_LORA: return "LORA";
    default:             return "AUTO";
    }
}

static uint32_t s_lora_min_hz = 902000000u;
static uint32_t s_lora_max_hz = 928000000u;
/* When the LoRa row being built began, or 0 when none is. Up here
   because a band change starts the next row over. */
static int64_t  s_lora_row_t0;

void ls_wf_source_lora_band(uint32_t min_hz, uint32_t max_hz)
{
    if (max_hz <= min_hz) return;
    if (min_hz < 150000000u || max_hz > 960000000u) return;
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

static const wf_band_t LORA_BANDS[] = {
    { "mesh watch",  909500000u, 911500000u },
    { "US915 ISM",   902000000u, 928000000u },
    { "EU868",       863000000u, 870000000u },
    { "433 ISM",     433050000u, 434790000u },
    { "315 remotes", 314000000u, 316000000u },
    { "full range",  150000000u, 960000000u },
};
#define LORA_BAND_N ((int)(sizeof(LORA_BANDS) / sizeof(LORA_BANDS[0])))

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
        snprintf(s_pre_detail, sizeof(s_pre_detail), fractional ? "%.3f-%.3f" : "%.0f-%.0f",
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
            return true;
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
    lakeshark_fm_set_mode(on ? FM_MODE_SCAN : s_fm_live_mode);
    if (on) lakeshark_fm_scan_restart();
    ls_wf_claim(LS_WF_OWNER_NONE, NULL);
    s_fm_seq_have = false;
}

const char *ls_wf_preset_current(ls_wf_src_t src)
{
    if (src == LS_WF_SRC_LORA) {
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
        if (FM.mode == FM_MODE_SCAN) lakeshark_fm_set_mode(FM_MODE_LISTEN);
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
    s_p25_seq_have = false;
    p25_spectrum_enable(on);
}

static bool p25_running(void)
{
    ls_iq_control_status_t st;
    memset(&st, 0, sizeof(st));
    p25_get_receiver_status(&st);
    return st.receiver_streaming;
}

static bool pump_p25(void)
{
    p25_feed(true);

    p25_spectrum_snapshot_t snap;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!p25_spectrum_read(s_bins, LS_WF_BINS_MAX, now, 500, &snap))
        return false;

    /* The same measurement, read again. The source is working - say
       so - but there is no new row to write. */
    if (s_p25_seq_have && snap.sequence == s_p25_seq) return true;
    s_p25_seq      = snap.sequence;
    s_p25_seq_have = true;

    ls_wf_feed_t f = {
        .center_hz = snap.center_hz,
        .span_hz   = snap.span_hz,
        .floor_db  = P25_SPECTRUM_FLOOR_DB,
        .top_db    = P25_SPECTRUM_TOP_DB,
        .live      = true,
        .note      = snap.following_voice ? "following voice" : "control",
    };
    ls_wf_claim(LS_WF_OWNER_P25, "P25");
    ls_wf_push(LS_WF_OWNER_P25, s_bins, LS_WF_BINS_MAX, &f);
    return true;
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
    if (snap.sequence == s_fm_live_seq) return true;
    s_fm_live_seq = snap.sequence;
    if (snap.center_hz != s_fm_live_center || snap.span_hz != s_fm_live_span) {
        ls_wf_claim(LS_WF_OWNER_NONE, NULL);
        s_fm_live_center = snap.center_hz;
        s_fm_live_span = snap.span_hz;
    }
    ls_wf_claim(LS_WF_OWNER_FM, "FM");
    ls_wf_feed_t feed = {
        .center_hz = snap.center_hz, .span_hz = snap.span_hz,
        .floor_db = FM_SPECTRUM_FLOOR_DB, .top_db = FM_SPECTRUM_TOP_DB,
        .live = true, .note = "live IQ", .period_ms = FM_SPECTRUM_PERIOD_MS,
    };
    ls_wf_push(LS_WF_OWNER_FM, s_bins, LS_WF_BINS_MAX, &feed);
    return true;
}

static bool pump_fm(void)
{
    const bool live = FM.mode != FM_MODE_SCAN;
    if (live != s_fm_was_live) {
        ls_wf_claim(LS_WF_OWNER_NONE, NULL);
        s_fm_live_seq = 0;
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
    static char progress[48];
    if (s_active != LS_WF_SRC_FM || !fm_sweeping() || FM.scan_tunes <= 0)
        return NULL;
    int completed = FM.scan_idx;
    if (completed < 0) completed = 0;
    if (completed > FM.scan_tunes) completed = FM.scan_tunes;
    snprintf(progress, sizeof(progress), "FM sweeping: %d%% (%d/%d)",
             completed * 100 / FM.scan_tunes, completed, FM.scan_tunes);
    return progress;
}

/* One pass of the sweep per call, which is one row per frame. */

static bool s_lora_held;
/* Still one pass per call. What changed is that a row is one pass
   only while the filter covers each bin's slice; on a band too wide for
   that (full range: 26 passes) the driver builds the row a look at a time
   and says when it is complete. In between, the source is working and has
   nothing new to write - the same answer P25 gives between measurements. */

static bool pump_lora(void)
{
    if (ls_lora_fsk_active()) return false;
    if (!ls_lora_present()) return false;

    if (ls_field_owned()) return false;
    if (!ls_lora_scanning()) {

        /* Recorded the moment it is asked for. */

        ls_mesh_radio_hold(true);
        s_lora_held = true;
        if (!ls_mesh_radio_held()) return false;
    }

    if (ls_lora_scan_begin(s_lora_min_hz, s_lora_max_hz) != ESP_OK)
        return false;

    const int n = LS_LORA_SCAN_BINS < LS_WF_BINS_MAX ? LS_LORA_SCAN_BINS
                                                     : LS_WF_BINS_MAX;
    static float dbm[LS_LORA_SCAN_BINS];
    if (!s_lora_row_t0) s_lora_row_t0 = esp_timer_get_time();
    bool row_done = false;
    const int got = ls_lora_scan_pass(dbm, n, &row_done);
    if (got <= 0) return false;

    /* Claimed on the first pass, so a row that takes 26 of them is 26 frames
       of "waiting for the receiver" and not of "no receiver". */
    ls_wf_claim(LS_WF_OWNER_LORA, "LORA");
    if (!row_done) return true;

    /* How long this row took, passes and frames between them
       included, so the stall notice knows what late means for this band. */
    const uint32_t period_ms =
        (uint32_t)((esp_timer_get_time() - s_lora_row_t0) / 1000);
    s_lora_row_t0 = 0;

    const float floor_db = -130.0f, top_db = -40.0f;
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
    };
    ls_wf_push(LS_WF_OWNER_LORA, s_bins, got, &f);
    return true;
}

static void lora_stop(void)
{
    if (s_lora_held && ls_lora_scanning()) ls_lora_scan_end();
    if (s_lora_held) {
        if (!ls_lora_fsk_active()) ls_mesh_radio_hold(false);
        s_lora_held = false;
    }
    s_lora_row_t0 = 0;
}

void ls_wf_source_pump(void)
{
    ls_wf_src_t use = s_want;
    /* AUTO deliberately never picks LORA. The other two are views of
       a receiver that is already running; a sweep STOPS the mesh to take the
       radio, and a screen that did that on its own would knock a node off
       the air because somebody opened the waterfall. It is chosen by hand or
       not at all. */
    if (use == LS_WF_SRC_AUTO)
        use = p25_running() ? LS_WF_SRC_P25
            : (fm_running() ? LS_WF_SRC_FM : LS_WF_SRC_AUTO);

    if (use != LS_WF_SRC_P25)  p25_feed(false);
    if (use != LS_WF_SRC_FM) fm_spectrum_enable(false);
    /* And gives the radio back, which matters more than turning a
       feed off: a mesh left parked is a node that has stopped answering. */
    if (use != LS_WF_SRC_LORA) lora_stop();

    bool ok = false;
    switch (use) {
    case LS_WF_SRC_P25:  ok = pump_p25();  break;
    case LS_WF_SRC_FM:   ok = pump_fm();   break;
    case LS_WF_SRC_LORA: ok = pump_lora(); break;
    default: break;
    }
    s_active = ok ? use : LS_WF_SRC_AUTO;
}

void ls_wf_source_release(void)
{
    fm_spectrum_enable(false);
    s_fm_live_seq = 0;
    p25_feed(false);
    lora_stop();
    s_fm_seq_have = false;
    s_active = LS_WF_SRC_AUTO;
}
