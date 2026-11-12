/* LS_TEST_SOURCES: ${APP}/p25/scan_ctrl.c ${APP}/p25/grant_follower.c ${APP}/p25/p25_ess.c ${APP}/p25/p25_controls.c */
/* the scan controller. Drives hold / lockout / allow list / priority
 * / precedence / persistence round-trip / names file parse against a live
 * grant follower. Nothing here needs a radio - the follower's retune callback
 * is a function pointer, the same one test_p25_grant uses.
 *
 * Every case starts fresh with an inited controller and an inited follower so
 * a failure earlier in the file cannot bleed state into a later case. */

#include "ls_test.h"

#include "scan_ctrl.h"
#include "grant_follower.h"

#include <string.h>
#include <stdio.h>

/* Retune witness so a case can assert what the follower asked the radio to
 * do, in order. Same shape as test_p25_grant.c uses. */
#define RETUNE_LOG_MAX 16
typedef struct {
    uint64_t hz[RETUNE_LOG_MAX];
    bool     to_traffic[RETUNE_LOG_MAX];
    int      count;
} retune_log_t;

static retune_log_t g_log;

static void retune_cb(void *user, uint64_t center_hz, bool to_traffic)
{
    retune_log_t *log = (retune_log_t *)user;
    if (log->count < RETUNE_LOG_MAX) {
        log->hz[log->count] = center_hz;
        log->to_traffic[log->count] = to_traffic;
    }
    log->count++;
}

static void reset_all(p25_scan_ctrl_t *sc, p25_grant_follower_t *f)
{
    memset(&g_log, 0, sizeof(g_log));
    p25_scan_init(sc);
    p25_grant_init(f, 851012500ull, retune_cb, &g_log);
}

/* Helper that mimics what app_p25.c's DSD task does when a TSBK grant is
 * parsed: hand the raw talkgroup/freq to the controller which decides,
 * applies to the follower, and reports. The bench never calls
 * p25_grant_on_grant directly - the controller is the sole gate. */
static p25_scan_decision_t feed(p25_scan_ctrl_t *sc, p25_grant_follower_t *f,
                                uint16_t tg, uint32_t src, uint64_t freq,
                                int64_t now_us)
{
    return p25_scan_apply(sc, f, tg, src, freq, now_us);
}

/* ==================================================================== hold */

LS_CASE(hold_prevents_retune_for_a_different_talkgroup)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    p25_scan_hold_set(&sc, 100);

    /* A grant lands for a different TG - no retune, no state change. */
    p25_scan_decision_t d = feed(&sc, &f, 200, 5000, 852000000ull, 0);
    LS_EQ_INT(d.action, P25_SCAN_STAY);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_HOLD_MISMATCH);
    LS_EQ_INT(g_log.count, 0);
    LS_EQ_INT(f.state, P25_GRANT_ON_CONTROL);

    /* A grant for the held TG is followed. */
    d = feed(&sc, &f, 100, 5000, 851525000ull, 1000);
    LS_EQ_INT(d.action, P25_SCAN_TUNE_TO_TRAFFIC);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_HOLD_HIT);
    LS_EQ_INT(f.state, P25_GRANT_ON_TRAFFIC);
    LS_EQ_UINT(f.talkgroup, 100);
    LS_EQ_INT(g_log.count, 1);
    LS_CHECK(g_log.to_traffic[0]);
    LS_EQ_UINT(g_log.hz[0], 851525000ull);
}

LS_CASE(clearing_hold_re_enables_normal_follow)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    p25_scan_hold_set(&sc, 100);
    (void)feed(&sc, &f, 200, 1, 852000000ull, 0);
    LS_EQ_INT(g_log.count, 0);

    p25_scan_hold_set(&sc, 0);
    LS_CHECK(!p25_scan_hold_active(&sc));

    p25_scan_decision_t d = feed(&sc, &f, 200, 1, 852000000ull, 1000);
    LS_EQ_INT(d.action, P25_SCAN_TUNE_TO_TRAFFIC);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_DEFAULT);
    LS_EQ_INT(g_log.count, 1);
}

/* ================================================================= lockout */

LS_CASE(lockout_blocks_grants_and_unlocked_tgs_are_still_followed)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    LS_CHECK(p25_scan_lockout_add(&sc, 100));

    p25_scan_decision_t d = feed(&sc, &f, 100, 1, 851525000ull, 0);
    LS_EQ_INT(d.action, P25_SCAN_STAY);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_LOCKED_OUT);
    LS_EQ_UINT(sc.lockout_hits, 1);
    LS_EQ_INT(g_log.count, 0);

    /* TG 200 is not locked out: it is followed. */
    d = feed(&sc, &f, 200, 1, 852000000ull, 1);
    LS_EQ_INT(d.action, P25_SCAN_TUNE_TO_TRAFFIC);
    LS_EQ_INT(f.state, P25_GRANT_ON_TRAFFIC);
    LS_EQ_UINT(f.talkgroup, 200);
}

LS_CASE(lockout_remove_re_enables_follow)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    p25_scan_lockout_add(&sc, 100);
    LS_CHECK(p25_scan_is_locked_out(&sc, 100));
    LS_CHECK(p25_scan_lockout_remove(&sc, 100));
    LS_CHECK(!p25_scan_is_locked_out(&sc, 100));

    p25_scan_decision_t d = feed(&sc, &f, 100, 1, 851525000ull, 0);
    LS_EQ_INT(d.action, P25_SCAN_TUNE_TO_TRAFFIC);
}

LS_CASE(lockout_table_is_bounded)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    for (int i = 0; i < P25_SCAN_LOCKOUT_MAX; i++)
        LS_CHECK(p25_scan_lockout_add(&sc, (uint16_t)(1000 + i)));
    LS_EQ_UINT(p25_scan_lockout_count(&sc), (unsigned)P25_SCAN_LOCKOUT_MAX);

    LS_CHECK(!p25_scan_lockout_add(&sc, 9999));
    LS_EQ_UINT(p25_scan_lockout_count(&sc), (unsigned)P25_SCAN_LOCKOUT_MAX);
}

/* ============================================================== allow list */

LS_CASE(allow_list_only_follows_listed_talkgroups)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    p25_scan_list_set_mode(&sc, P25_SCAN_LIST_ALLOW);
    p25_scan_allow_add(&sc, 100);
    p25_scan_allow_add(&sc, 200);

    /* Not on the list. */
    p25_scan_decision_t d = feed(&sc, &f, 999, 1, 851525000ull, 0);
    LS_EQ_INT(d.action, P25_SCAN_STAY);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_ALLOW_LIST);
    LS_EQ_UINT(sc.allow_rejects, 1);
    LS_EQ_INT(g_log.count, 0);

    /* On the list. */
    d = feed(&sc, &f, 200, 1, 852000000ull, 1);
    LS_EQ_INT(d.action, P25_SCAN_TUNE_TO_TRAFFIC);
    LS_EQ_UINT(f.talkgroup, 200);
}

LS_CASE(empty_allow_list_means_follow_everything_not_nothing)
{

    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    p25_scan_list_set_mode(&sc, P25_SCAN_LIST_ALLOW);
    LS_EQ_UINT(sc.allow_count, 0);

    p25_scan_decision_t d = feed(&sc, &f, 200, 1, 852000000ull, 0);
    LS_EQ_INT(d.action, P25_SCAN_TUNE_TO_TRAFFIC);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_DEFAULT);
}

LS_CASE(precedence_lockout_beats_follow_none)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    p25_scan_list_set_mode(&sc, P25_SCAN_LIST_NONE);
    p25_scan_lockout_add(&sc, 100);

    p25_scan_decision_t d = feed(&sc, &f, 100, 1, 851525000ull, 0);
    LS_EQ_INT(d.action, P25_SCAN_STAY);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_LOCKED_OUT);
}

LS_CASE(precedence_hold_beats_follow_none)
{

    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    p25_scan_list_set_mode(&sc, P25_SCAN_LIST_NONE);
    p25_scan_hold_set(&sc, 100);

    p25_scan_decision_t d = feed(&sc, &f, 100, 1, 851525000ull, 0);
    LS_EQ_INT(d.action, P25_SCAN_TUNE_TO_TRAFFIC);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_HOLD_HIT);
}

LS_CASE(precedence_follow_none_beats_priority_and_allow_membership)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    p25_scan_allow_add(&sc, 500);
    p25_scan_priority_set(&sc, 500, 3);
    p25_scan_list_set_mode(&sc, P25_SCAN_LIST_NONE);

    LS_CHECK(!p25_scan_is_allowed(&sc, 500));
    p25_scan_decision_t d = feed(&sc, &f, 500, 1, 853000000ull, 0);
    LS_EQ_INT(d.action, P25_SCAN_STAY);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_LIST_NONE);
    LS_EQ_INT(g_log.count, 0);
}

/* ================================================================= priority */

LS_CASE(priority_grant_from_control_is_followed_and_counted)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    p25_scan_priority_set(&sc, 500, 3);

    p25_scan_decision_t d = feed(&sc, &f, 500, 1, 853000000ull, 0);
    LS_EQ_INT(d.action, P25_SCAN_TUNE_TO_TRAFFIC);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_PRIORITY);
    LS_EQ_UINT(sc.priority_grants, 1);
    LS_EQ_UINT(f.talkgroup, 500);
}

LS_CASE(priority_higher_rank_preempts_a_call_in_progress)
{
    /* Design: priority preempts. When a higher-rank priority TG grants while
     * a lower-rank (or non-priority) TG is being followed, the follower must
     * release the current call and follow the incoming one. */
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    p25_scan_priority_set(&sc, 500, 3);
    p25_scan_priority_set(&sc, 100, 1);

    /* Land TG 100 first. */
    p25_scan_decision_t d = feed(&sc, &f, 100, 1, 851525000ull, 0);
    LS_EQ_INT(d.action, P25_SCAN_TUNE_TO_TRAFFIC);
    LS_EQ_INT(f.state, P25_GRANT_ON_TRAFFIC);
    LS_EQ_INT(g_log.count, 1);

    /* TG 500 (higher rank) comes in - must preempt. */
    d = feed(&sc, &f, 500, 1, 853000000ull, 100000);
    LS_EQ_INT(d.action, P25_SCAN_TUNE_TO_TRAFFIC);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_PRIORITY);
    LS_EQ_UINT(f.talkgroup, 500);
    LS_EQ_UINT(sc.priority_preempts, 1);
    /* Retune log: to_traffic(100), to_control(back), to_traffic(500) */
    LS_EQ_INT(g_log.count, 3);
    LS_CHECK(g_log.to_traffic[0]);
    LS_CHECK(!g_log.to_traffic[1]);
    LS_CHECK(g_log.to_traffic[2]);
    LS_EQ_UINT(g_log.hz[2], 853000000ull);
}

LS_CASE(priority_same_rank_does_not_preempt_would_be_thrash)
{
    /* Two equal-rank priorities in a busy shift must not preempt each other
     * - it produces exactly the thrash the scanner is meant to avoid. Higher
     * rank preempts, equal rank does not. */
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    p25_scan_priority_set(&sc, 100, 2);
    p25_scan_priority_set(&sc, 500, 2);

    (void)feed(&sc, &f, 100, 1, 851525000ull, 0);
    p25_scan_decision_t d = feed(&sc, &f, 500, 1, 853000000ull, 100000);
    LS_EQ_INT(d.action, P25_SCAN_STAY);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_FOREIGN_GRANT);
    LS_EQ_UINT(f.talkgroup, 100);   /* still on 100 */
    LS_EQ_INT(g_log.count, 1);      /* the initial follow only */
}

/* ================================================================ precedence */

LS_CASE(precedence_lockout_beats_hold)
{

    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    p25_scan_hold_set(&sc, 100);
    p25_scan_lockout_add(&sc, 100);

    p25_scan_decision_t d = feed(&sc, &f, 100, 1, 851525000ull, 0);
    LS_EQ_INT(d.action, P25_SCAN_STAY);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_LOCKED_OUT);
}

LS_CASE(precedence_hold_beats_priority)
{
    /* A priority TG grant lands while a hold is set on another TG. The hold
     * wins - if priority could preempt a hold, hold would offer nothing over
     * the plain follower. */
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    p25_scan_hold_set(&sc, 100);
    p25_scan_priority_set(&sc, 500, 3);
    (void)feed(&sc, &f, 100, 1, 851525000ull, 0);  /* on hold TG */
    LS_EQ_INT(f.state, P25_GRANT_ON_TRAFFIC);
    LS_EQ_UINT(f.talkgroup, 100);

    p25_scan_decision_t d = feed(&sc, &f, 500, 1, 853000000ull, 100000);
    LS_EQ_INT(d.action, P25_SCAN_STAY);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_HOLD_MISMATCH);
    LS_EQ_UINT(f.talkgroup, 100);
}

LS_CASE(precedence_priority_beats_default)
{
    /* No hold, no lockout. Non-priority TG 100 followed. Priority TG 500
     * grants - preempts, because priority beats default. */
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    p25_scan_priority_set(&sc, 500, 3);
    (void)feed(&sc, &f, 100, 1, 851525000ull, 0);
    p25_scan_decision_t d = feed(&sc, &f, 500, 1, 853000000ull, 100000);
    LS_EQ_INT(d.action, P25_SCAN_TUNE_TO_TRAFFIC);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_PRIORITY);
    LS_EQ_UINT(f.talkgroup, 500);
}

LS_CASE(precedence_allow_list_beats_priority)
{

    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    p25_scan_list_set_mode(&sc, P25_SCAN_LIST_ALLOW);
    p25_scan_allow_add(&sc, 100);
    p25_scan_priority_set(&sc, 500, 3);

    p25_scan_decision_t d = feed(&sc, &f, 500, 1, 853000000ull, 0);
    LS_EQ_INT(d.action, P25_SCAN_STAY);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_ALLOW_LIST);
}

LS_CASE(precedence_hold_beats_encrypted_skip)
{

    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);

    /* Get onto TG 100, then arm an encrypted skip via an ADP ESS. */
    (void)feed(&sc, &f, 100, 1, 851525000ull, 0);
    LS_EQ_INT(f.state, P25_GRANT_ON_TRAFFIC);
    LS_CHECK(p25_grant_on_ess(&f, 100, 0x84, 0, 100000LL) == true);
    LS_CHECK(p25_grant_tg_is_skipped(&f, 100, 200000LL));

    /* Now hold TG 100. The next grant for it must be followed even though
     * a skip is armed. */
    p25_scan_hold_set(&sc, 100);
    p25_scan_decision_t d = feed(&sc, &f, 100, 1, 851525000ull, 300000LL);
    LS_EQ_INT(d.action, P25_SCAN_TUNE_TO_TRAFFIC);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_HOLD_HIT);
}

LS_CASE(precedence_encrypted_skip_still_applies_without_hold)
{
    /* No hold: the encrypted skip must still refuse the grant. */
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    (void)feed(&sc, &f, 100, 1, 851525000ull, 0);
    LS_CHECK(p25_grant_on_ess(&f, 100, 0x84, 0, 100000LL));
    LS_CHECK(p25_grant_tg_is_skipped(&f, 100, 200000LL));

    p25_scan_decision_t d = feed(&sc, &f, 100, 1, 851525000ull, 300000LL);
    LS_EQ_INT(d.action, P25_SCAN_STAY);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_ENCRYPTED_SKIP);
}

LS_CASE(duplicate_grant_reports_already_following)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    (void)feed(&sc, &f, 100, 1, 851525000ull, 0);
    p25_scan_decision_t d = feed(&sc, &f, 100, 1, 851525000ull, 1000);
    LS_EQ_INT(d.action, P25_SCAN_STAY);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_ALREADY_FOLLOWING);
    /* No extra retune, and the follower counter for duplicates advanced. */
    LS_EQ_INT(g_log.count, 1);
    LS_EQ_UINT(f.duplicate_grants, 1);
}

/* =============================================================== persistence */

LS_CASE(persistence_round_trip_survives_a_simulated_restart)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f);
    p25_scan_hold_set(&sc, 100);
    p25_scan_lockout_add(&sc, 200);
    p25_scan_lockout_add(&sc, 201);
    p25_scan_list_set_mode(&sc, P25_SCAN_LIST_ALLOW);
    p25_scan_allow_add(&sc, 300);
    p25_scan_allow_add(&sc, 400);

    uint8_t buf[256];
    size_t need = p25_scan_persist_size(&sc);
    LS_CHECK(need > 0);
    LS_CHECK(need <= sizeof(buf));
    size_t n = p25_scan_persist_save(&sc, buf, sizeof(buf));
    LS_EQ_UINT(n, need);

    /* Restart: init a fresh controller, load. State must match. */
    p25_scan_ctrl_t sc2;
    p25_scan_init(&sc2);
    LS_CHECK(p25_scan_persist_load(&sc2, buf, n));
    LS_EQ_UINT(sc2.hold_tg, 100);
    LS_EQ_INT(sc2.list_mode, P25_SCAN_LIST_ALLOW);
    LS_EQ_UINT(sc2.lockout_count, 2);
    LS_CHECK(p25_scan_is_locked_out(&sc2, 200));
    LS_CHECK(p25_scan_is_locked_out(&sc2, 201));
    LS_EQ_UINT(sc2.allow_count, 2);
    LS_CHECK(p25_scan_is_allowed(&sc2, 300));
    LS_CHECK(p25_scan_is_allowed(&sc2, 400));
    LS_CHECK(!p25_scan_is_allowed(&sc2, 500));
}

LS_CASE(follow_none_mode_survives_a_simulated_restart)
{
    p25_scan_ctrl_t sc;
    p25_scan_init(&sc);
    p25_scan_list_set_mode(&sc, P25_SCAN_LIST_NONE);

    uint8_t buf[32];
    size_t n = p25_scan_persist_save(&sc, buf, sizeof(buf));
    LS_CHECK(n > 0);

    p25_scan_ctrl_t sc2;
    p25_scan_init(&sc2);
    LS_CHECK(p25_scan_persist_load(&sc2, buf, n));
    LS_EQ_INT(sc2.list_mode, P25_SCAN_LIST_NONE);
    LS_CHECK(!p25_scan_is_allowed(&sc2, 500));
}

LS_CASE(persistence_rejects_wrong_magic_and_does_not_mutate_state)
{
    p25_scan_ctrl_t sc;
    p25_scan_init(&sc);
    p25_scan_lockout_add(&sc, 500);

    uint8_t junk[64];
    memset(junk, 0xaa, sizeof(junk));
    LS_CHECK(!p25_scan_persist_load(&sc, junk, sizeof(junk)));
    /* Prior lockout intact - a garbage blob does not silently clear. */
    LS_CHECK(p25_scan_is_locked_out(&sc, 500));
}

/* ============================================================ roster scope */

/* Fake store for scoped rosters, keyed like the NVS one by (wacn, sysid). */
#define STORE_MAX 8
static struct { uint32_t wacn; uint16_t sysid; uint8_t blob[512]; size_t n; } s_store[STORE_MAX];
static int s_store_n, s_saves, s_unscoped_moves;

static int store_find(uint32_t wacn, uint16_t sysid)
{
    for (int i = 0; i < s_store_n; i++)
        if (s_store[i].wacn == wacn && s_store[i].sysid == sysid) return i;
    return -1;
}
static void store_erase(uint32_t wacn, uint16_t sysid)
{
    int i = store_find(wacn, sysid);
    if (i < 0) return;
    s_store[i] = s_store[--s_store_n];
}
static void store_put(p25_scan_ctrl_t *sc);
/* As the NVS loader: the full key first, then a SYSID-only one, moved. */
static void store_load(p25_scan_ctrl_t *sc, uint32_t wacn, uint16_t sysid)
{
    int i = store_find(wacn, sysid);
    if (i >= 0) {
        (void)p25_scan_persist_load_roster(sc, s_store[i].blob, s_store[i].n);
        return;
    }
    i = store_find(0, sysid);
    if (!wacn || i < 0) return;
    (void)p25_scan_persist_load_roster(sc, s_store[i].blob, s_store[i].n);
    p25_scan_roster_bind(sc, wacn, sysid);
    store_put(sc);
    store_erase(0, sysid);
}
static void store_put(p25_scan_ctrl_t *sc)
{
    LS_CHECK(sc->roster_sys.valid);
    int i = store_find(sc->roster_sys.wacn, sc->roster_sys.sysid);
    if (i < 0) { LS_CHECK(s_store_n < STORE_MAX); i = s_store_n++; }
    s_store[i].wacn = sc->roster_sys.wacn;
    s_store[i].sysid = sc->roster_sys.sysid;
    s_store[i].n = p25_scan_persist_save(sc, s_store[i].blob, sizeof(s_store[i].blob));
    s_saves++;
}
static void store_save(p25_scan_ctrl_t *sc, p25_system_id_t moved_from)
{
    store_put(sc);
    if (!moved_from.valid) s_unscoped_moves++;
    else store_erase(moved_from.wacn, moved_from.sysid);
}
static int s_switched_calls;
static void store_reset(void)
{
    memset(s_store, 0, sizeof(s_store));
    s_store_n = s_saves = s_unscoped_moves = s_switched_calls = 0;
    p25_scan_set_roster_loader(store_load);
    p25_scan_set_roster_saver(store_save);
    p25_scan_set_roster_switched(NULL);
}

static dsd_state s_sys;
static void on_system(p25_scan_ctrl_t *sc, uint32_t wacn, uint16_t sysid,
                      int64_t now_us)
{
    memset(&s_sys, 0, sizeof(s_sys));
    s_sys.p25_net_valid = wacn != 0;
    s_sys.p25_tsbk_wacn = wacn;
    s_sys.p25_tsbk_sysid = wacn ? sysid : 0;
    s_sys.p25_rfss_valid = !wacn && sysid;
    s_sys.p25_rfss_sysid = wacn ? 0 : sysid;
    (void)p25_scan_note_system(sc, &s_sys, now_us);
}

#define FL_WACN 0xBEE00u   /* Manatee-Sarasota */
#define FL_SYS  0x9DCu
#define AR_WACN 0x7A1C3u   /* a made-up Argentine system */
#define AR_SYS  0x1F2u

LS_CASE(scoped_roster_applies_only_on_its_own_system)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f); store_reset();

    on_system(&sc, FL_WACN, FL_SYS, 1000);
    p25_scan_list_set_mode(&sc, P25_SCAN_LIST_ALLOW);
    p25_scan_allow_add(&sc, 101);
    p25_scan_lockout_add(&sc, 202);
    LS_CHECK(sc.roster_sys.valid && sc.roster_sys.sysid == FL_SYS);
    store_put(&sc);
    LS_EQ_INT(feed(&sc, &f, 303, 1, 851500000ull, 2000).reason, P25_SCAN_REASON_ALLOW_LIST);

    /* Argentina: nothing stored there, so every call is followed. */
    on_system(&sc, AR_WACN, AR_SYS, 3000);
    LS_EQ_UINT(sc.roster_switches, 2);
    LS_CHECK(p25_scan_roster_applies(&sc, 3000));
    LS_EQ_UINT(sc.allow_count, 0);
    LS_EQ_UINT(sc.lockout_count, 0);
    LS_EQ_INT(feed(&sc, &f, 202, 1, 851600000ull, 4000).action, P25_SCAN_TUNE_TO_TRAFFIC);
    p25_grant_force_return_to_control(&f);

    /* Home again: the Florida roster comes back from the store. */
    on_system(&sc, FL_WACN, FL_SYS, 5000);
    LS_EQ_UINT(sc.allow_count, 1);
    LS_CHECK(p25_scan_is_locked_out(&sc, 202));
    LS_EQ_INT(feed(&sc, &f, 202, 1, 851600000ull, 6000).reason, P25_SCAN_REASON_LOCKED_OUT);
}

LS_CASE(an_edit_abroad_lands_on_that_system_and_leaves_home_alone)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f); store_reset();
    on_system(&sc, FL_WACN, FL_SYS, 1000);
    p25_scan_lockout_add(&sc, 7);
    store_put(&sc);

    on_system(&sc, AR_WACN, AR_SYS, 2000);
    (void)p25_scan_roster_for_edit(&sc);
    p25_scan_lockout_add(&sc, 99);
    store_put(&sc);
    LS_EQ_INT(s_store_n, 2);

    on_system(&sc, FL_WACN, FL_SYS, 3000);
    LS_CHECK(p25_scan_is_locked_out(&sc, 7));
    LS_CHECK(!p25_scan_is_locked_out(&sc, 99));
    on_system(&sc, AR_WACN, AR_SYS, 4000);
    LS_CHECK(p25_scan_is_locked_out(&sc, 99));
    LS_CHECK(!p25_scan_is_locked_out(&sc, 7));
}

LS_CASE(an_unnamed_site_is_held_to_the_roster_only_for_the_grace_period)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f); store_reset();
    on_system(&sc, FL_WACN, FL_SYS, 1000);
    p25_scan_lockout_add(&sc, 202);

    /* Retuned: the decoder has not heard NET_STS_BCST yet. */
    on_system(&sc, 0, 0, 2000000);
    LS_CHECK(p25_scan_roster_applies(&sc, 2000000 + P25_ROSTER_UNKNOWN_GRACE_US - 1));
    LS_EQ_INT(feed(&sc, &f, 202, 1, 851600000ull, 3000000).reason, P25_SCAN_REASON_LOCKED_OUT);
    /* A site that never names itself is not held to it for good. */
    LS_CHECK(!p25_scan_roster_applies(&sc, 2000000 + P25_ROSTER_UNKNOWN_GRACE_US));
    LS_EQ_INT(feed(&sc, &f, 202, 1, 851600000ull,
                   2000000 + P25_ROSTER_UNKNOWN_GRACE_US).action, P25_SCAN_TUNE_TO_TRAFFIC);
}

LS_CASE(a_sysid_only_site_matches_on_sysid)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f); store_reset();
    on_system(&sc, FL_WACN, FL_SYS, 1000);
    p25_scan_lockout_add(&sc, 202);
    on_system(&sc, 0, FL_SYS, 2000);          /* RFSS_STS_BCST only */
    LS_CHECK(p25_scan_roster_applies(&sc, 2000));
    on_system(&sc, 0, AR_SYS, 3000);
    LS_CHECK(!p25_scan_roster_applies(&sc, 3000));
    LS_EQ_INT(feed(&sc, &f, 202, 1, 851600000ull, 4000).action, P25_SCAN_TUNE_TO_TRAFFIC);
}

LS_CASE(an_old_unscoped_roster_is_set_aside_on_the_first_named_system)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f); store_reset();
    /* What NVS held before scoping: an allow list from a home profile. */
    p25_scan_list_set_mode(&sc, P25_SCAN_LIST_ALLOW);
    p25_scan_allow_add(&sc, 10001);
    uint8_t buf[256];
    size_t n = p25_scan_persist_save(&sc, buf, sizeof(buf));
    LS_CHECK(p25_scan_persist_load(&sc, buf, n));
    LS_CHECK(!sc.roster_sys.valid && !sc.roster_pending_bind);

    on_system(&sc, FL_WACN, FL_SYS, 1000);
    LS_EQ_UINT(sc.allow_count, 0);
    LS_EQ_INT(sc.list_mode, P25_SCAN_LIST_OFF);
    LS_EQ_INT(feed(&sc, &f, 303, 1, 851500000ull, 2000).action, P25_SCAN_TUNE_TO_TRAFFIC);
    p25_grant_force_return_to_control(&f);

    /* A site that only ever sends RFSS_STS_BCST never swaps rosters; the
     * old list must still not hold it. */
    LS_CHECK(p25_scan_persist_load(&sc, buf, n));
    on_system(&sc, 0, FL_SYS, 3000);
    LS_EQ_UINT(sc.allow_count, 1);
    LS_CHECK(!p25_scan_roster_applies(&sc, 3000));
    LS_EQ_INT(feed(&sc, &f, 303, 1, 851500000ull, 4000).action, P25_SCAN_TUNE_TO_TRAFFIC);
    p25_grant_force_return_to_control(&f);

    /* Before any system has named itself it still holds, briefly. */
    LS_CHECK(p25_scan_persist_load(&sc, buf, n));
    on_system(&sc, 0, 0, 5000);
    LS_CHECK(p25_scan_roster_applies(&sc, 5000));
    LS_CHECK(!p25_scan_roster_applies(&sc, 5000 + P25_ROSTER_UNKNOWN_GRACE_US));
}

LS_CASE(a_roster_made_before_the_system_is_named_belongs_to_it)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f); store_reset();
    p25_scan_lockout_add(&sc, 202);
    sc.roster_pending_bind = true;            /* as the NVS save marks it */
    on_system(&sc, AR_WACN, AR_SYS, 1000);
    LS_CHECK(sc.roster_sys.valid && sc.roster_sys.wacn == AR_WACN &&
             sc.roster_sys.sysid == AR_SYS);
    LS_CHECK(p25_scan_is_locked_out(&sc, 202));
    LS_EQ_INT(s_unscoped_moves, 1);
    LS_EQ_INT(store_find(AR_WACN, AR_SYS), 0);
}

LS_CASE(scope_survives_a_restart_and_old_blobs_load_unscoped)
{
    p25_scan_ctrl_t sc, sc2;
    p25_scan_init(&sc);
    p25_scan_lockout_add(&sc, 5);
    p25_scan_roster_bind(&sc, AR_WACN, AR_SYS);
    uint8_t buf[256];
    size_t n = p25_scan_persist_save(&sc, buf, sizeof(buf));
    LS_EQ_UINT(n, p25_scan_persist_size(&sc));
    LS_CHECK(p25_scan_persist_load(&sc2, buf, n));
    LS_CHECK(sc2.roster_sys.valid);
    LS_EQ_UINT(sc2.roster_sys.wacn, AR_WACN);
    LS_EQ_UINT(sc2.roster_sys.sysid, AR_SYS);
    LS_CHECK(p25_scan_is_locked_out(&sc2, 5));

    /* An older build reads only up to the allow list and ignores the
     * flags word: the same bytes with the scope cut off still load. */
    buf[6] = buf[7] = 0;
    LS_CHECK(p25_scan_persist_load(&sc2, buf, n - 6));
    LS_CHECK(!sc2.roster_sys.valid);
    LS_CHECK(p25_scan_is_locked_out(&sc2, 5));
    /* A truncated scoped blob is refused, not half applied. */
    buf[6] = 1;
    LS_CHECK(!p25_scan_persist_load(&sc2, buf, n - 6));
}

LS_CASE(a_profile_follow_off_holds_only_on_its_system)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f); store_reset();
    on_system(&sc, FL_WACN, FL_SYS, 1000);
    sc.roster_follow_off = true;             /* profile said auto_follow=false */
    LS_CHECK(!p25_scan_roster_empty(&sc));
    store_put(&sc);
    LS_CHECK(sc.auto_follow);                /* the global switch is untouched */
    LS_CHECK(!p25_scan_get_auto_follow(&sc));
    LS_EQ_INT(feed(&sc, &f, 303, 1, 851500000ull, 2000).reason,
              P25_SCAN_REASON_AUTO_FOLLOW_OFF);

    on_system(&sc, AR_WACN, AR_SYS, 3000);
    LS_CHECK(!sc.roster_follow_off);
    LS_CHECK(p25_scan_get_auto_follow(&sc));
    LS_EQ_INT(feed(&sc, &f, 303, 1, 851500000ull, 4000).action, P25_SCAN_TUNE_TO_TRAFFIC);
    p25_grant_force_return_to_control(&f);

    on_system(&sc, FL_WACN, FL_SYS, 5000);
    LS_CHECK(sc.roster_follow_off);           /* came back with the roster */

    /* It survives a restart in the blob's flags word. */
    uint8_t buf[256];
    size_t n = p25_scan_persist_save(&sc, buf, sizeof(buf));
    p25_scan_ctrl_t sc2;
    LS_CHECK(p25_scan_persist_load(&sc2, buf, n));
    LS_CHECK(sc2.roster_follow_off);
}

LS_CASE(roster_load_keeps_follow_settings_and_identity)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f); store_reset();
    p25_scan_set_auto_follow(&sc, &f, false);
    on_system(&sc, FL_WACN, FL_SYS, 1000);
    LS_CHECK(!sc.auto_follow);
    LS_CHECK(sc.current_sys.valid);
}

/* Stand-in for the profile layer: a profile for Florida ranking TG 101. */
static void fl_profile_switched(p25_scan_ctrl_t *sc)
{
    s_switched_calls++;
    if (sc->roster_sys.valid && sc->roster_sys.sysid == FL_SYS &&
        sc->roster_sys.wacn == FL_WACN)
        (void)p25_scan_priority_set(sc, 101, 5);
}

LS_CASE(profile_priorities_come_back_when_the_stored_roster_is_swapped_in)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f); store_reset();
    on_system(&sc, FL_WACN, FL_SYS, 1000);
    p25_scan_list_set_mode(&sc, P25_SCAN_LIST_ALLOW);
    p25_scan_allow_add(&sc, 101);
    p25_scan_allow_add(&sc, 202);
    store_put(&sc);

    /* Reboot / P25 re-entry: an empty unscoped roster, and the profile
     * reapply puts its ranks on it before the system names itself. */
    reset_all(&sc, &f);
    p25_scan_set_roster_switched(fl_profile_switched);
    (void)p25_scan_priority_set(&sc, 101, 5);
    on_system(&sc, FL_WACN, FL_SYS, 2000);
    LS_EQ_INT(s_switched_calls, 1);
    LS_EQ_UINT(sc.allow_count, 2);
    LS_EQ_UINT(p25_scan_priority_rank(&sc, 101), 5);

    /* So TG 101 still preempts a lower call. */
    LS_EQ_INT(feed(&sc, &f, 202, 1, 851600000ull, 3000).action, P25_SCAN_TUNE_TO_TRAFFIC);
    p25_scan_decision_t d = feed(&sc, &f, 101, 1, 851700000ull, 4000);
    LS_EQ_INT(d.reason, P25_SCAN_REASON_PRIORITY);
    p25_grant_force_return_to_control(&f);

    /* Not onto another system's roster; back again on the way home. */
    on_system(&sc, AR_WACN, AR_SYS, 5000);
    LS_EQ_UINT(p25_scan_priority_rank(&sc, 101), 0);
    on_system(&sc, FL_WACN, FL_SYS, 6000);
    LS_EQ_UINT(p25_scan_priority_rank(&sc, 101), 5);
    LS_EQ_INT(s_switched_calls, 3);
    p25_scan_set_roster_switched(NULL);
}

LS_CASE(a_sysid_only_profile_reload_replaces_an_older_full_key_roster)
{
    p25_scan_ctrl_t sc; p25_grant_follower_t f;
    reset_all(&sc, &f); store_reset();
    /* An older roster for Florida, stored under the full key. */
    on_system(&sc, FL_WACN, FL_SYS, 1000);
    p25_scan_list_set_mode(&sc, P25_SCAN_LIST_ALLOW);
    p25_scan_allow_add(&sc, 101);
    store_put(&sc);

    /* The profile is reloaded with sysid= but no wacn= (as op_roster). */
    p25_scan_allow_clear(&sc);
    p25_scan_allow_add(&sc, 202);
    p25_scan_roster_bind(&sc, 0, FL_SYS);
    store_put(&sc);
    LS_CHECK(!p25_scan_roster_resync(&sc));

    /* Upgraded in place: the profile's list, under the full key only. */
    LS_EQ_UINT(sc.roster_sys.wacn, FL_WACN);
    LS_EQ_UINT(sc.roster_sys.sysid, FL_SYS);
    LS_CHECK(p25_scan_allow_contains(&sc, 202));
    LS_CHECK(!p25_scan_allow_contains(&sc, 101));
    LS_EQ_INT(store_find(0, FL_SYS), -1);
    LS_CHECK(store_find(FL_WACN, FL_SYS) >= 0);
    LS_EQ_UINT(sc.roster_switches, 1);

    /* It is the one that comes back after a trip away. */
    on_system(&sc, AR_WACN, AR_SYS, 2000);
    on_system(&sc, FL_WACN, FL_SYS, 3000);
    LS_CHECK(p25_scan_allow_contains(&sc, 202));
    LS_CHECK(!p25_scan_allow_contains(&sc, 101));
}

/* ==================================================================== names */

/* Fixture reader: feeds one line at a time from a fixed array. */
typedef struct {
    const char **lines;
    size_t       n;
    size_t       i;
} names_ctx_t;

static int names_read_line(void *ctx, char *buf, size_t buf_len)
{
    names_ctx_t *c = (names_ctx_t *)ctx;
    if (c->i >= c->n) return 0;
    const char *src = c->lines[c->i++];
    size_t k = 0;
    while (src[k] && k + 1 < buf_len) { buf[k] = src[k]; k++; }
    buf[k] = 0;
    return (int)k;
}

LS_CASE(names_file_loads_and_skips_comments)
{
    p25_scan_ctrl_t sc;
    p25_scan_init(&sc);
    const char *lines[] = {
        "# LakeShark names",
        "  ",
        "100,PD Dispatch,Police",
        "200,Fire Ch1,Fire",
        "; another comment",
        "300,Sheriff",
    };
    names_ctx_t c = { lines, sizeof(lines) / sizeof(lines[0]), 0 };

    p25_scan_names_load(&sc, names_read_line, &c, 1024);
    LS_EQ_UINT(sc.names_loaded, 3);
    LS_EQ_UINT(sc.names_bad_lines, 0);
    LS_EQ_UINT(sc.names_count, 3);

    const p25_scan_name_t *n = p25_scan_name_lookup(&sc, 200);
    LS_CHECK(n != NULL);
    LS_EQ_STR(n->name, "Fire Ch1");
    LS_EQ_STR(n->category, "Fire");

    n = p25_scan_name_lookup(&sc, 300);
    LS_CHECK(n != NULL);
    LS_EQ_STR(n->name, "Sheriff");
    LS_EQ_STR(n->category, "");
}

LS_CASE(names_file_reports_first_bad_line_and_loads_the_rest)
{
    p25_scan_ctrl_t sc;
    p25_scan_init(&sc);
    const char *lines[] = {
        "100,Good",
        "not a valid line",   /* bad, no comma */
        "200,Also good",
        "abc,Bad number",     /* bad, non-numeric TG */
        "300,Third good",
    };
    names_ctx_t c = { lines, sizeof(lines) / sizeof(lines[0]), 0 };

    p25_scan_names_load(&sc, names_read_line, &c, 1024);
    LS_EQ_UINT(sc.names_loaded, 3);
    LS_EQ_UINT(sc.names_bad_lines, 2);
    LS_EQ_INT(sc.names_first_bad_line, 2);
    LS_CHECK(p25_scan_name_lookup(&sc, 100) != NULL);
    LS_CHECK(p25_scan_name_lookup(&sc, 200) != NULL);
    LS_CHECK(p25_scan_name_lookup(&sc, 300) != NULL);
}

LS_CASE(names_file_refuses_when_over_size_cap)
{
    p25_scan_ctrl_t sc;
    p25_scan_init(&sc);
    const char *lines[] = { "100,Nope" };
    names_ctx_t c = { lines, 1, 0 };
    p25_scan_names_load(&sc, names_read_line, &c,
                        (size_t)P25_SCAN_NAMES_MAX_FILE_BYTES + 1);
    LS_CHECK(sc.names_refused_oversize);
    LS_EQ_UINT(sc.names_loaded, 0);
    LS_EQ_UINT(sc.names_count, 0);
}

/* ================================================= parse-line direct tests */

LS_CASE(name_parse_accepts_valid_and_trims_whitespace)
{
    p25_scan_name_t out;
    LS_CHECK(p25_scan_name_parse_line("  1234 ,  Dispatch , Police  ", &out));
    LS_EQ_UINT(out.number, 1234);
    LS_EQ_STR(out.name, "Dispatch");
    LS_EQ_STR(out.category, "Police");

    LS_CHECK(p25_scan_name_parse_line("42,Foo", &out));
    LS_EQ_UINT(out.number, 42);
    LS_EQ_STR(out.name, "Foo");
    LS_EQ_STR(out.category, "");
}

LS_CASE(name_parse_rejects_zero_and_out_of_range)
{
    p25_scan_name_t out;
    LS_CHECK(!p25_scan_name_parse_line("0,Bad", &out));
    LS_CHECK(!p25_scan_name_parse_line("65536,Overflow", &out));
    LS_CHECK(!p25_scan_name_parse_line(",EmptyNumber", &out));
    LS_CHECK(!p25_scan_name_parse_line("100,", &out));   /* empty name */
}
