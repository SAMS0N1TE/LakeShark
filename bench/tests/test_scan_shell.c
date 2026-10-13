/* LS_TEST_SOURCES: ls_scan_view.c and ls_scan_options.c over the host scan
   engine fixture - what a view change does to a running scan, and what every
   row of the scanner's options list actually sets.

   These drive the real code against the real engine state. Nothing here reads
   a source file or matches a string in one: a scan is started, the views are
   changed, and the engine is asked what happened to it. */

#include "ls_test.h"

#include "ls_options.h"
#include "ls_picker.h"
#include "ls_radio_select.h"
#include "ls_scan_options.h"
#include "ls_scan_view.h"
#include "radio_choice.h"
#include "radio_endpoint.h"
#include "scan_channels.h"
#include "scan_engine.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------- what ls_options needs -- */

static ls_rsel_hw_t s_board;
void ls_rsel_hw(ls_rsel_hw_t *out) { *out = s_board; }
void ls_rsel_hw_restart(void) { }

static uint64_t s_packed = LS_RSEL_PACKED_NONE;
int settings_get_radio_choice(int job) { return ls_rsel_unpack(s_packed, job); }
bool settings_set_radio_choice(int job, int radio)
{
    s_packed = ls_rsel_pack(s_packed, job, radio);
    return true;
}
ls_radio_err_t ls_radio_endpoint_get(const char *id, ls_radio_endpoint_info_t *out)
{
    (void)id;
    memset(out, 0, sizeof(*out));
    return LS_RADIO_ERR_UNAVAILABLE;
}
int ls_tui_screen_current(void) { return 0; }

/* ------------------------------------------------------------- fixtures -- */

static const ls_scan_state_t SCANNING = { .scanning = true, .sweeping = false };
static const ls_scan_state_t SWEEPING = { .scanning = false, .sweeping = true };
static const ls_scan_state_t IDLE     = { .scanning = false, .sweeping = false };

static const ls_scan_view_t VIEWS[] = { LS_SCAN_VIEW_SCANNER, LS_SCAN_VIEW_VFO,
                                        LS_SCAN_VIEW_PAGER, LS_SCAN_VIEW_SPECTRUM };
#define N_VIEWS ((int)(sizeof(VIEWS) / sizeof(VIEWS[0])))

/* Three saved channels and a scan running on the second of them, held. */
static void running_scan(void)
{
    scan_channels_clear();
    scan_engine_set_source(SCAN_SRC_CHANNELS);
    scan_engine_set_zone(-1);
    scan_channel_add("WX1", 162550000, SCAN_MODE_NFM, 0);
    scan_channel_add("WX2", 162400000, SCAN_MODE_NFM, 0);
    scan_channel_add("FIRE", 154785000, SCAN_MODE_NFM, 1);
    scan_engine_set_hang_ms(4000);
    scan_engine_set_threshold_pct(9);
    scan_engine_set_band(151000000, 152000000, 12500);
    scan_engine_start();
    scan_engine_next();           /* move off the first channel */
    scan_engine_hold(true);
}

/* Everything a scan would lose if someone stopped it. */
typedef struct {
    bool     active, hold;
    int      candidate, zone, hang, thresh;
    uint32_t lo, hi, step;
} snapshot_t;

static snapshot_t snap(void)
{
    snapshot_t s = { .active = scan_engine_active(),
                     .hold = scan_engine_manual_hold(),
                     .candidate = scan_engine_candidate(),
                     .zone = scan_engine_get_zone(),
                     .hang = scan_engine_get_hang_ms(),
                     .thresh = scan_engine_get_threshold_pct() };
    scan_engine_get_band(&s.lo, &s.hi, &s.step);
    return s;
}

static bool same(snapshot_t a, snapshot_t b)
{
    return a.active == b.active && a.hold == b.hold && a.candidate == b.candidate &&
           a.zone == b.zone && a.hang == b.hang && a.thresh == b.thresh &&
           a.lo == b.lo && a.hi == b.hi && a.step == b.step;
}

/* The one call a screen makes on a page change. */
static void go(ls_scan_view_t to)
{
    const ls_scan_state_t st = { .scanning = scan_engine_active(), .sweeping = false };
    ls_scan_view_apply(ls_scan_view_change(to, st));
}

/* ============================================================== the views = */

LS_CASE(looking_at_the_spectrum_does_not_stop_a_running_channel_scan)
{
    running_scan();
    const snapshot_t before = snap();
    LS_CHECK(before.active);
    LS_CHECK(before.hold);

    go(LS_SCAN_VIEW_SPECTRUM);
    LS_CHECK_MSG(scan_engine_active(), "the spectrum stopped the scan");
    LS_CHECK_MSG(scan_engine_manual_hold(), "the spectrum dropped the hold");
    LS_CHECK_MSG(same(before, snap()), "the spectrum moved the scan");

    go(LS_SCAN_VIEW_SCANNER);
    LS_CHECK_MSG(scan_engine_active(), "coming back found a stopped scanner");
    LS_CHECK_MSG(same(before, snap()), "coming back moved the scan");
}

/* The report was a loop: scan, look, come back, look again. Ten round trips
   must leave the scan exactly where the first one found it. */
LS_CASE(repeated_scanner_spectrum_round_trips_keep_the_scan_and_its_place)
{
    running_scan();
    const snapshot_t before = snap();
    for (int i = 0; i < 10; i++) {
        go(LS_SCAN_VIEW_SPECTRUM);
        LS_CHECK_MSG(scan_engine_active(), "trip %d stopped the scan on the way out", i);
        go(LS_SCAN_VIEW_SCANNER);
        LS_CHECK_MSG(scan_engine_active(), "trip %d stopped the scan on the way back", i);
        LS_CHECK_MSG(same(before, snap()), "trip %d moved the scan", i);
    }
    LS_CHECK(scan_engine_manual_hold());
}

LS_CASE(no_page_of_the_fm_screen_stops_the_scanner)
{
    for (int from = 0; from < N_VIEWS; from++)
        for (int to = 0; to < N_VIEWS; to++) {
            running_scan();
            const snapshot_t before = snap();
            go(VIEWS[from]);
            go(VIEWS[to]);
            LS_CHECK_MSG(scan_engine_active(), "view %d -> %d stopped the scan", from, to);
            LS_CHECK_MSG(same(before, snap()), "view %d -> %d moved the scan", from, to);
        }
}

/* The other half of the rule: what the operator does to the receiver still
   ends the scan, because after it the channel the scanner was on no longer
   says where the receiver is. */
LS_CASE(tuning_changing_mode_or_sweeping_still_ends_the_scan)
{
    const ls_scan_op_t ops[] = { LS_SCAN_OP_TUNE, LS_SCAN_OP_MODE, LS_SCAN_OP_SWEEP };
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        running_scan();
        LS_EQ_INT(ls_scan_view_operate(ops[i], SCANNING), LS_SCAN_STOP);
        ls_scan_view_apply(ls_scan_view_operate(ops[i], snap().active ? SCANNING : IDLE));
        LS_CHECK_MSG(!scan_engine_active(), "op %u left the scan running", i);
    }
    running_scan();
    LS_EQ_INT(ls_scan_view_operate(LS_SCAN_OP_SCAN, SCANNING), LS_SCAN_KEEP);
    ls_scan_view_apply(ls_scan_view_operate(LS_SCAN_OP_SCAN, SCANNING));
    LS_CHECK(scan_engine_active());
    scan_engine_stop();
}

/* The frequency sweep is a different thing from the channel scanner, and only
   the VFO leaves it behind. */
LS_CASE(the_frequency_sweep_is_not_the_channel_scanner)
{
    LS_CHECK(ls_scan_view_ends_sweep(LS_SCAN_VIEW_VFO, SWEEPING));
    LS_CHECK(!ls_scan_view_ends_sweep(LS_SCAN_VIEW_SPECTRUM, SWEEPING));
    LS_CHECK(!ls_scan_view_ends_sweep(LS_SCAN_VIEW_SCANNER, SWEEPING));
    LS_CHECK(!ls_scan_view_ends_sweep(LS_SCAN_VIEW_VFO, SCANNING));
    LS_CHECK(!ls_scan_view_ends_sweep(LS_SCAN_VIEW_VFO, IDLE));
    /* And a sweep running is not a reason to stop a channel scan that is not. */
    LS_EQ_INT(ls_scan_view_change(LS_SCAN_VIEW_VFO, SWEEPING), LS_SCAN_KEEP);
}

/* ============================================================ the options = */

static const ls_opt_t *row(const ls_opt_ctx_t *ctx, const char *label)
{
    for (int i = 0; i < ctx->n; i++)
        if (!strcmp(ctx->opt[i].label, label)) return &ctx->opt[i];
    return NULL;
}

static void shown(const ls_opt_t *o, char *out, size_t n) { ls_opt_value(o, out, n); }

LS_CASE(the_scanner_options_set_the_things_six_buttons_cannot)
{
    running_scan();
    const ls_opt_ctx_t *ctx = ls_scan_options(true, -1);
    LS_CHECK(ls_opt_count(ctx) > 0);
    char text[64];

    /* Hang and the threshold describe HOW the scan runs: they apply live and
       the scan keeps running. */
    const ls_opt_t *hang = row(ctx, "Hang time");
    LS_CHECK(hang != NULL);
    hang->set_num(hang, 4.5);
    LS_EQ_INT(scan_engine_get_hang_ms(), 4500);
    LS_CHECK_MSG(scan_engine_active(), "setting the hang time stopped the scan");
    shown(hang, text, sizeof(text));
    LS_CHECK_MSG(!strcmp(text, "4.5 s"), "hang shows '%s'", text);

    const ls_opt_t *open = row(ctx, "Open at");
    LS_CHECK(open != NULL);
    open->set_num(open, 23);
    LS_EQ_INT(scan_engine_get_threshold_pct(), 23);
    LS_CHECK_MSG(scan_engine_active(), "setting the threshold stopped the scan");

    const ls_opt_t *pri = row(ctx, "Priority look");
    LS_CHECK(pri != NULL);
    pri->set_num(pri, 2.5);
    LS_EQ_INT(scan_engine_get_priority_ms(), 2500);
    pri->set_num(pri, 0);
    LS_EQ_INT(scan_engine_get_priority_ms(), 0);
    shown(pri, text, sizeof(text));
    LS_CHECK_MSG(!strcmp(text, "OFF"), "priority off shows '%s'", text);

    /* The autosquelch is NFM's, so it is in the FM list and not the P25 one. */
    LS_CHECK(row(ctx, "Autosquelch") != NULL);
    LS_CHECK(row(ls_scan_options(false, -1), "Autosquelch") == NULL);
}

LS_CASE(changing_what_is_scanned_stops_the_scan_first)
{
    const char *const retarget[] = { "Scan source", "Zone", "Band step" };
    for (unsigned i = 0; i < sizeof(retarget) / sizeof(retarget[0]); i++) {
        running_scan();
        const ls_opt_ctx_t *ctx = ls_scan_options(true, -1);
        const ls_opt_t *o = row(ctx, retarget[i]);
        LS_CHECK(o != NULL);
        o->set(o, o->get(o) + 1 < o->n ? o->get(o) + 1 : 0);
        LS_CHECK_MSG(!scan_engine_active(), "%s left a stale scan running", retarget[i]);
    }
    running_scan();
    const ls_opt_ctx_t *ctx = ls_scan_options(true, -1);
    const ls_opt_t *start = row(ctx, "Band start");
    LS_CHECK(start != NULL);
    start->set_num(start, 151.5);
    LS_CHECK(!scan_engine_active());
}

LS_CASE(the_band_edges_can_be_typed_and_stay_in_order)
{
    scan_engine_stop();
    const ls_opt_ctx_t *ctx = ls_scan_options(true, -1);
    const ls_opt_t *start = row(ctx, "Band start");
    const ls_opt_t *stop = row(ctx, "Band stop");
    LS_CHECK(start && stop);
    LS_CHECK(start->kind == LS_OPT_NUMBER && stop->kind == LS_OPT_NUMBER);

    scan_engine_set_band(150000000, 162000000, 12500);
    start->set_num(start, 151.0);
    uint32_t lo, hi, step;
    scan_engine_get_band(&lo, &hi, &step);
    LS_EQ_INT(lo, 151000000u);
    LS_EQ_INT(hi, 162000000u);

    stop->set_num(stop, 152.25);
    scan_engine_get_band(&lo, &hi, &step);
    LS_EQ_INT(hi, 152250000u);
    LS_CHECK_MSG(lo < hi, "typing a stop left %lu..%lu", (unsigned long)lo, (unsigned long)hi);

    /* A start typed past the stop still leaves a span that can be scanned. */
    start->set_num(start, 300.0);
    scan_engine_get_band(&lo, &hi, &step);
    LS_EQ_INT(lo, 300000000u);
    LS_CHECK_MSG(hi > lo, "start past stop left %lu..%lu", (unsigned long)lo,
                 (unsigned long)hi);

    /* A stop typed below the start does the same the other way. */
    scan_engine_set_band(150000000, 162000000, 12500);
    stop->set_num(stop, 149.0);
    scan_engine_get_band(&lo, &hi, &step);
    LS_EQ_INT(hi, 149000000u);
    LS_CHECK_MSG(lo < hi, "stop below start left %lu..%lu", (unsigned long)lo,
                 (unsigned long)hi);
}

LS_CASE(zone_all_is_a_choice_and_not_a_number_you_have_to_know)
{
    scan_engine_stop();
    const ls_opt_ctx_t *ctx = ls_scan_options(true, -1);
    const ls_opt_t *zone = row(ctx, "Zone");
    LS_CHECK(zone != NULL);
    char text[32];

    scan_engine_set_zone(-1);
    LS_EQ_INT(zone->get(zone), 0);
    shown(zone, text, sizeof(text));
    LS_CHECK_MSG(!strcmp(text, "ALL"), "zone -1 shows '%s'", text);

    zone->set(zone, 4);            /* ALL, 0, 1, 2, 3 -> zone 3 */
    LS_EQ_INT(scan_engine_get_zone(), 3);
    LS_EQ_INT(zone->get(zone), 4);
    zone->set(zone, 0);
    LS_EQ_INT(scan_engine_get_zone(), -1);
}

LS_CASE(the_per_channel_rows_edit_the_channel_the_list_is_showing)
{
    running_scan();
    const ls_opt_ctx_t *ctx = ls_scan_options(true, 2);
    LS_EQ_INT(ls_scan_options_channel(), 2);

    const ls_opt_t *lock = row(ctx, "Channel lockout");
    const ls_opt_t *prio = row(ctx, "Channel priority");
    const ls_opt_t *on = row(ctx, "Channel enabled");
    const ls_opt_t *del = row(ctx, "Delete channel");
    LS_CHECK(lock && prio && on && del);
    LS_CHECK_MSG(lock->why_not(lock) == NULL, "a selected channel refused LOCKOUT");

    LS_EQ_INT(lock->get(lock), 0);
    lock->set(lock, 1);
    LS_CHECK(scan_channel_get(2)->flags & SCAN_FLAG_LOCKOUT);
    LS_CHECK_MSG(!scan_engine_active(), "editing a channel left a stale scan running");
    lock->set(lock, 0);
    LS_CHECK(!(scan_channel_get(2)->flags & SCAN_FLAG_LOCKOUT));

    prio->set(prio, 1);
    LS_CHECK(scan_channel_get(2)->flags & SCAN_FLAG_PRIORITY);
    on->set(on, 0);
    LS_CHECK(!(scan_channel_get(2)->flags & SCAN_FLAG_ENABLED));

    const int before = scan_channels_count();
    del->act(del);
    LS_EQ_INT(scan_channels_count(), before - 1);
    LS_EQ_INT(ls_scan_options_channel(), -1);

    /* With nothing selected the rows stay in the list and say why. */
    ctx = ls_scan_options(true, -1);
    const ls_opt_t *none = row(ctx, "Channel lockout");
    LS_CHECK(none->why_not(none) != NULL);
    none->set(none, 1);           /* must not reach into channel -1 */
    scan_channels_clear();
}

LS_CASE(an_index_the_list_no_longer_has_selects_nothing)
{
    scan_channels_clear();
    scan_channel_add("ONE", 154000000, SCAN_MODE_NFM, 0);
    (void)ls_scan_options(true, 0);
    LS_EQ_INT(ls_scan_options_channel(), 0);
    (void)ls_scan_options(true, 9);
    LS_EQ_INT(ls_scan_options_channel(), -1);
    scan_channels_clear();
}
