/* LS_TEST_SOURCES: ${FW}/components/apps/tui/ls_wf_source.c, against fakes */

#include "ls_test.h"

#include <string.h>

#include "esp_timer.h"
#include "ls_wf_source.h"
#include "ls_waterfall.h"
#include "ls_lora.h"
#include "ls_mesh.h"
#include "ls_tui_screen.h"
#include "lakeshark_backend.h"
#include "p25_state.h"
#include "iq_app_control.h"
#include "apps/p25/p25_spectrum.h"
#include "apps/p25/p25_program.h"
#include "apps/fm/fm_state.h"

/* ---- the widget ------------------------------------------------------- */

static ls_wf_owner_t g_owner;
static int           g_claims_none;
static int           g_pushes;
static float         g_row[LS_WF_BINS_MAX];
static int           g_row_n;
static ls_wf_feed_t  g_feed;

void ls_wf_claim(ls_wf_owner_t owner, const char *label)
{
    (void)label;
    if (owner == g_owner) return;
    g_owner = owner;
    if (owner == LS_WF_OWNER_NONE) g_claims_none++;
}

void ls_wf_push(ls_wf_owner_t owner, const float *bins, int n,
                const ls_wf_feed_t *feed)
{
    /* Refused from anyone but the owner, as the real one does. */
    if (owner != g_owner || !bins || n <= 0) return;
    if (n > LS_WF_BINS_MAX) n = LS_WF_BINS_MAX;
    memcpy(g_row, bins, sizeof(float) * (size_t)n);
    g_row_n = n;
    if (feed) g_feed = *feed;
    g_pushes++;
}

/* ---- the SX1262 sweep -------------------------------------------------- */

static bool     g_scanning;
static uint32_t g_scan_lo, g_scan_hi;
static int      g_begins, g_retunes, g_ends;
static int      g_looks = 1, g_look;

bool ls_lora_present(void)  { return true; }
bool ls_lora_scanning(void) { return g_scanning; }

esp_err_t ls_lora_scan_begin(uint32_t lo, uint32_t hi)
{
    if (g_scanning && lo == g_scan_lo && hi == g_scan_hi) return ESP_OK;
    if (g_scanning) g_retunes++;
    else            g_begins++;
    g_scanning = true;
    g_scan_lo = lo;
    g_scan_hi = hi;
    g_look = 0;
    return ESP_OK;
}

int ls_lora_scan_pass(float *dbm, int n, bool *row_done)
{
    if (row_done) *row_done = false;
    if (!g_scanning || !dbm || n <= 0) return 0;
    for (int i = 0; i < n; i++) dbm[i] = (i == n / 2) ? -60.0f : -120.0f;
    if (++g_look >= g_looks) {
        g_look = 0;
        if (row_done) *row_done = true;
    }
    return n;
}

esp_err_t ls_lora_scan_end(void)
{
    if (g_scanning) g_ends++;
    g_scanning = false;
    return ESP_OK;
}

/* ---- the mesh's lease on the radio ------------------------------------- */

static bool g_mesh_parks;   /* whether the mesh lets go the moment it is asked */
static bool g_hold_asked;

bool ls_mesh_radio_held(void) { return g_hold_asked && g_mesh_parks; }

bool ls_mesh_radio_hold(bool on)
{
    g_hold_asked = on;
    return on ? ls_mesh_radio_held() : true;
}

/* ---- the FM receiver --------------------------------------------------- */

fm_state_t FM;
static bool        g_fm_streaming;
static int         g_fm_mode_asked = -1;
static const char *g_radio_asked;

void fm_get_receiver_status(ls_iq_control_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->receiver_streaming = g_fm_streaming;
}

void lakeshark_fm_set_mode(int mode) { g_fm_mode_asked = mode; }
void lakeshark_fm_scan_restart(void) { }
void ls_tui_radio_want(const char *mode_name) { g_radio_asked = mode_name; }

/* ---- P25, which nothing here selects ------------------------------------ */

static bool g_p25_feed;

void p25_get_receiver_status(ls_iq_control_status_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}
bool p25_spectrum_enabled(void) { return g_p25_feed; }
void p25_spectrum_enable(bool on) { g_p25_feed = on; }
bool p25_spectrum_read(float *out, int n, uint32_t now_ms, uint32_t max_age_ms,
                       p25_spectrum_snapshot_t *snap)
{
    (void)out; (void)n; (void)now_ms; (void)max_age_ms; (void)snap;
    return false;
}
const p25_program_t *p25_program_session(void) { return NULL; }
bool p25_program_step_control_now(int delta) { (void)delta; return false; }

/* ---- between cases ------------------------------------------------------ */

/* The module under test keeps its state in statics, as it does on the board,
   so every case starts by leaving the way a screen does and then clearing the
   fakes - in that order, because leaving is what gives a held radio back. */
static void fresh(void)
{
    ls_wf_source_release();
    ls_wf_source_select(LS_WF_SRC_AUTO);
    ls_wf_source_lora_band(902000000u, 928000000u);
    memset(&FM, 0, sizeof(FM));
    g_owner = LS_WF_OWNER_NONE;
    g_claims_none = g_pushes = g_row_n = 0;
    memset(&g_feed, 0, sizeof(g_feed));
    g_scanning = false;
    g_begins = g_retunes = g_ends = 0;
    g_looks = 1;
    g_look = 0;
    g_mesh_parks = true;
    g_hold_asked = false;
    g_fm_streaming = false;
    g_fm_mode_asked = -1;
    g_radio_asked = NULL;
    ls_shim_time_set(5000000);
}

/* An FM receiver sweeping 150-162 MHz: a flat floor at a tenth of full scale
   and one carrier near the top, which is what scan_paint writes. */
static void fm_sweeping_with_one_carrier(void)
{
    FM.mode = FM_MODE_SCAN;
    FM.scan_bins = 128;
    FM.scan_start_hz = 150000000u;
    FM.scan_stop_hz = 162000000u;
    FM.scan_sweep_ms = 11520;
    for (int i = 0; i < FM.scan_bins; i++) FM.scan_db[i] = 0.10f;
    FM.scan_db[64] = 0.85f;
    g_fm_streaming = true;
}

/* ---- FM ---------------------------------------------------------- */

LS_CASE(an_fm_sweep_row_carries_its_carrier_rather_than_a_solid_block)
{
    /* "FM does not work at all in waterfalls." scan_db is 0..1 on
       FM_SCAN_FLOOR_DB..FM_SCAN_TOP_DB, and the pump read it as dBFS on
       -100..-30, so every bin came out as (x + 100) / 70 > 1.4 and was
       clamped to full scale. This is the row that went to the widget. */
    fresh();
    fm_sweeping_with_one_carrier();
    FM.scan_sweeps = 4;

    ls_wf_source_select(LS_WF_SRC_FM);
    ls_wf_source_pump();
    /* The first look notes the count and claims; what was already in
       scan_db was measured before anyone was watching. */
    LS_EQ_INT(0, g_pushes);
    LS_EQ_INT(LS_WF_OWNER_FM, g_owner);

    FM.scan_sweeps++;
    ls_wf_source_pump();
    LS_EQ_INT(1, g_pushes);
    LS_EQ_INT(128, g_row_n);

    int full = 0;
    for (int i = 0; i < g_row_n; i++) if (g_row[i] >= 0.999f) full++;
    LS_CHECK_MSG(full == 0, "%d of %d bins at full scale - the band was "
                 "drawn as a solid block", full, g_row_n);
    LS_NEAR(g_row[64], 0.85, 1e-6);
    LS_NEAR(g_row[10], 0.10, 1e-6);
    LS_NEAR(g_feed.floor_db, FM_SCAN_FLOOR_DB, 1e-6);
    LS_NEAR(g_feed.top_db, FM_SCAN_TOP_DB, 1e-6);

    /* A sweep that has not finished is not a new row. */
    ls_wf_source_pump();
    LS_EQ_INT(1, g_pushes);
}

LS_CASE(fm_decoding_pocsag_is_not_a_spectrum_whatever_it_swept_before)
{

    fresh();
    fm_sweeping_with_one_carrier();
    FM.mode = FM_MODE_POCSAG;
    FM.scan_sweeps = 7;

    ls_wf_source_pump();
    LS_EQ_STR("none", ls_wf_source_name());

    ls_wf_source_select(LS_WF_SRC_FM);
    ls_wf_source_pump();
    FM.scan_sweeps++;
    ls_wf_source_pump();
    LS_EQ_INT(0, g_pushes);
    LS_EQ_INT(LS_WF_OWNER_NONE, g_owner);
    LS_EQ_STR("none", ls_wf_source_name());
}

LS_CASE(an_fm_receiver_that_has_stopped_is_not_a_spectrum)
{
    /* Stopped in SCAN mode - another app took the dongle - is leftovers too. */
    fresh();
    fm_sweeping_with_one_carrier();
    g_fm_streaming = false;
    FM.scan_sweeps = 3;

    ls_wf_source_pump();
    FM.scan_sweeps++;
    ls_wf_source_pump();
    LS_EQ_INT(0, g_pushes);
    LS_EQ_STR("none", ls_wf_source_name());
}

LS_CASE(picking_fm_asks_for_the_receiver_and_for_the_sweep)
{
    /* The one route FALLS and FM's SWEEP page both take now: the receiver by
       name, and the sweep, because FM streaming on one channel has no bins. */
    fresh();
    LS_CHECK(ls_wf_source_start(LS_WF_SRC_FM));
    LS_EQ_STR("FM", g_radio_asked);
    LS_EQ_INT(FM_MODE_SCAN, g_fm_mode_asked);
    LS_EQ_INT(LS_WF_SRC_FM, ls_wf_source_get());
}

/* ---- the mesh's lease -------------------------------------------- */

LS_CASE(a_hold_asked_for_before_the_mesh_parks_is_still_given_back)
{

    fresh();
    g_mesh_parks = false;

    ls_wf_source_select(LS_WF_SRC_LORA);
    ls_wf_source_pump();
    LS_CHECK(g_hold_asked);
    LS_EQ_INT(0, g_begins);

    ls_wf_source_select(LS_WF_SRC_P25);
    ls_wf_source_pump();
    LS_CHECK_MSG(!g_hold_asked, "the mesh was asked to park and never told "
                 "it could stop");
}

/* ---- a band change ----------------------------------------------- */

LS_CASE(a_lora_band_change_retunes_in_place_and_restarts_the_picture_once)
{

    fresh();
    ls_wf_source_select(LS_WF_SRC_LORA);
    ls_wf_source_pump();
    LS_EQ_INT(1, g_begins);
    LS_EQ_INT(1, g_pushes);
    LS_EQ_UINT(902000000u, g_scan_lo);

    const int nones = g_claims_none;
    LS_CHECK(ls_wf_preset_apply(LS_WF_SRC_LORA, 0));      /* mesh watch */
    LS_EQ_INT(0, g_ends);
    LS_EQ_INT(nones + 1, g_claims_none);
    LS_CHECK(g_hold_asked);

    ls_wf_source_pump();
    LS_EQ_INT(0, g_ends);
    LS_EQ_INT(1, g_begins);
    LS_EQ_INT(1, g_retunes);
    LS_EQ_UINT(909500000u, g_scan_lo);
    LS_EQ_UINT(911500000u, g_scan_hi);
    LS_EQ_INT(LS_WF_OWNER_LORA, g_owner);
    LS_EQ_INT(2, g_pushes);
    LS_EQ_UINT(2000000u, g_feed.span_hz);
    LS_EQ_INT(nones + 1, g_claims_none);

    /* And the same band on the next frame is nothing at all. */
    ls_wf_source_pump();
    LS_EQ_INT(1, g_retunes);
    LS_EQ_INT(3, g_pushes);
}

/* ---- and a row built over several passes ------------------ */

LS_CASE(a_wide_band_row_goes_in_once_every_look_is_in)
{
    /* Full range is 26 passes a row. Here four, a frame apart: the source is
       working the whole time - claimed, so the widget says it is waiting and
       not that there is no receiver - and pushes one row, carrying how long
       it took, when the last look is in. */
    fresh();
    g_looks = 4;
    ls_wf_source_select(LS_WF_SRC_LORA);
    for (int p = 0; p < 3; p++) {
        ls_wf_source_pump();
        ls_shim_time_advance(100000);
    }
    LS_EQ_INT(0, g_pushes);
    LS_EQ_INT(LS_WF_OWNER_LORA, g_owner);
    LS_EQ_STR("LORA", ls_wf_source_name());

    ls_wf_source_pump();
    LS_EQ_INT(1, g_pushes);
    LS_EQ_INT(LS_LORA_SCAN_BINS, g_row_n);
    LS_EQ_UINT(300u, g_feed.period_ms);
}

LS_CASE(an_fm_row_tells_the_waterfall_how_long_a_sweep_takes)
{
    fresh();
    fm_sweeping_with_one_carrier();
    ls_wf_source_select(LS_WF_SRC_FM);
    ls_wf_source_pump();
    FM.scan_sweeps++;
    ls_wf_source_pump();
    LS_EQ_INT(1, g_pushes);
    LS_EQ_UINT(11520u, g_feed.period_ms);
}
