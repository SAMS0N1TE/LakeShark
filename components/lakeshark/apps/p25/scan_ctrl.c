/* scan controller. */

#include "scan_ctrl.h"

#include <ctype.h>
#include "esp_attr.h"
#include <string.h>

/* Single global instance. Constructing more than one would only mean two
 * radios in one firmware, which we do not have. */
EXT_RAM_BSS_ATTR p25_scan_ctrl_t g_p25_scan;

void p25_scan_init(p25_scan_ctrl_t *sc)
{
    if (!sc) return;
    memset(sc, 0, sizeof(*sc));
    sc->auto_follow = true;
    sc->list_mode = P25_SCAN_LIST_OFF;
    sc->names_first_bad_line = -1;
}

bool p25_scan_set_auto_follow(p25_scan_ctrl_t *sc, p25_grant_follower_t *f,
                              bool enabled)
{
    if (!sc) return false;
    sc->auto_follow = enabled;
    if (enabled) return false;
    return p25_grant_force_return_to_control(f);
}

bool p25_scan_get_auto_follow(const p25_scan_ctrl_t *sc)
{
    if (!sc) return true;
    /* Effective value, for the screen: the system's own off wins where its
     * roster applies (taken as "now" within the grace, for display). */
    return sc->auto_follow &&
           !(sc->roster_follow_off &&
             p25_scan_roster_applies(sc, sc->sys_unknown_since_us));
}

void p25_scan_restore_controls(p25_scan_ctrl_t *sc,
                               p25_grant_follower_t *f,
                               bool auto_follow,
                               bool leave_on_encrypted,
                               uint32_t encrypted_skip_ms)
{
    (void)p25_scan_set_auto_follow(sc, f, auto_follow);
    p25_grant_set_leave_on_encrypted(f, leave_on_encrypted);
    p25_grant_set_encrypted_skip_ms(f, encrypted_skip_ms);
}

/* ------------------------------------------------------------------ hold */

void p25_scan_hold_set(p25_scan_ctrl_t *sc, uint16_t tg)
{
    if (!sc) return;
    sc->hold_tg = tg;
}
uint16_t p25_scan_hold_get(const p25_scan_ctrl_t *sc)
{
    return sc ? sc->hold_tg : 0;
}
bool p25_scan_hold_active(const p25_scan_ctrl_t *sc)
{
    return sc && sc->hold_tg != 0;
}

/* --------------------------------------------------------------- lockout */

static int find_u16(const uint16_t *arr, size_t n, uint16_t v)
{
    for (size_t i = 0; i < n; i++) if (arr[i] == v) return (int)i;
    return -1;
}

bool p25_scan_lockout_add(p25_scan_ctrl_t *sc, uint16_t tg)
{
    if (!sc || tg == 0) return false;
    if (find_u16(sc->lockout, sc->lockout_count, tg) >= 0) return true;
    if (sc->lockout_count >= P25_SCAN_LOCKOUT_MAX) return false;
    sc->lockout[sc->lockout_count++] = tg;
    return true;
}
bool p25_scan_lockout_remove(p25_scan_ctrl_t *sc, uint16_t tg)
{
    if (!sc) return false;
    int i = find_u16(sc->lockout, sc->lockout_count, tg);
    if (i < 0) return false;
    /* Order does not matter - move-last to i. */
    sc->lockout[i] = sc->lockout[--sc->lockout_count];
    return true;
}
bool p25_scan_is_locked_out(const p25_scan_ctrl_t *sc, uint16_t tg)
{
    if (!sc || tg == 0) return false;
    return find_u16(sc->lockout, sc->lockout_count, tg) >= 0;
}
size_t p25_scan_lockout_count(const p25_scan_ctrl_t *sc)
{
    return sc ? sc->lockout_count : 0;
}

/* ------------------------------------------------------------ allow list */

void p25_scan_list_set_mode(p25_scan_ctrl_t *sc, p25_scan_list_mode_t m)
{
    if (!sc) return;
    sc->list_mode = m;
}
p25_scan_list_mode_t p25_scan_list_get_mode(const p25_scan_ctrl_t *sc)
{
    return sc ? sc->list_mode : P25_SCAN_LIST_OFF;
}
bool p25_scan_allow_add(p25_scan_ctrl_t *sc, uint16_t tg)
{
    if (!sc || tg == 0) return false;
    if (find_u16(sc->allow, sc->allow_count, tg) >= 0) return true;
    if (sc->allow_count >= P25_SCAN_ALLOW_MAX) return false;
    sc->allow[sc->allow_count++] = tg;
    return true;
}

void p25_scan_allow_clear(p25_scan_ctrl_t *sc)
{
    if (!sc) return;
    sc->allow_count = 0;
}
bool p25_scan_allow_remove(p25_scan_ctrl_t *sc, uint16_t tg)
{
    if (!sc) return false;
    int i = find_u16(sc->allow, sc->allow_count, tg);
    if (i < 0) return false;
    sc->allow[i] = sc->allow[--sc->allow_count];
    return true;
}
bool p25_scan_allow_contains(const p25_scan_ctrl_t *sc, uint16_t tg)
{
    if (!sc || tg == 0) return false;
    return find_u16(sc->allow, sc->allow_count, tg) >= 0;
}
size_t p25_scan_allow_count(const p25_scan_ctrl_t *sc)
{
    return sc ? sc->allow_count : 0;
}
bool p25_scan_is_allowed(const p25_scan_ctrl_t *sc, uint16_t tg)
{
    if (!sc || tg == 0) return false;
    /* NONE is explicit control-channel-only operation. It must not
     * inherit ALLOW's empty-list fallback or consult stale list members. */
    if (sc->list_mode == P25_SCAN_LIST_NONE) return false;

    if (sc->list_mode != P25_SCAN_LIST_ALLOW) return true;
    if (sc->allow_count == 0) return true;
    return find_u16(sc->allow, sc->allow_count, tg) >= 0;
}

/* -------------------------------------------------------------- priority */

static int priority_find(const p25_scan_ctrl_t *sc, uint16_t tg)
{
    for (size_t i = 0; i < sc->priority_count; i++)
        if (sc->priority[i].talkgroup == tg && sc->priority[i].rank > 0)
            return (int)i;
    return -1;
}

bool p25_scan_priority_set(p25_scan_ctrl_t *sc, uint16_t tg, uint8_t rank)
{
    if (!sc || tg == 0) return false;
    /* rank==0 removes the slot */
    int i = priority_find(sc, tg);
    if (rank == 0) {
        if (i < 0) return true;
        sc->priority[i] = sc->priority[--sc->priority_count];
        return true;
    }
    if (i >= 0) { sc->priority[i].rank = rank; return true; }
    if (sc->priority_count >= P25_SCAN_PRIORITY_MAX) return false;
    sc->priority[sc->priority_count].talkgroup = tg;
    sc->priority[sc->priority_count].rank      = rank;
    sc->priority_count++;
    return true;
}
uint8_t p25_scan_priority_rank(const p25_scan_ctrl_t *sc, uint16_t tg)
{
    if (!sc || tg == 0) return 0;
    int i = priority_find(sc, tg);
    return i < 0 ? 0 : sc->priority[i].rank;
}
void p25_scan_priority_clear(p25_scan_ctrl_t *sc)
{
    if (!sc) return;
    sc->priority_count = 0;
}
size_t p25_scan_priority_count(const p25_scan_ctrl_t *sc)
{
    return sc ? sc->priority_count : 0;
}

/* ----------------------------------------------------------------- names */

const p25_scan_name_t *p25_scan_name_lookup(const p25_scan_ctrl_t *sc,
                                            uint16_t tg)
{
    if (!sc || tg == 0) return NULL;
    /* Linear search: the table caps at 256 and a UI queries this once per
     * TG event. If a future site needs faster, sort at load time and switch
     * to a bsearch. */
    for (size_t i = 0; i < sc->names_count; i++)
        if (sc->names[i].number == tg) return &sc->names[i];
    return NULL;
}

void p25_scan_names_clear(p25_scan_ctrl_t *sc)
{
    if (!sc) return;
    sc->names_count = 0;
    sc->names_loaded = 0;
    sc->names_bad_lines = 0;
    sc->names_truncated = 0;
    sc->names_first_bad_line = -1;
    sc->names_refused_oversize = false;
}

/* Strip leading/trailing whitespace in-place. Returns the new start. */
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) { *end = 0; end--; }
    return s;
}

bool p25_scan_name_parse_line(const char *line, p25_scan_name_t *out)
{
    if (!line) return false;
    char buf[P25_SCAN_NAMES_MAX_LINE_LEN + 1];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    /* Strip a trailing \r or \n if a caller left one on. */
    for (size_t i = 0; buf[i]; i++)
        if (buf[i] == '\r' || buf[i] == '\n') { buf[i] = 0; break; }

    char *s = trim(buf);
    if (!*s || *s == '#' || *s == ';') return false;

    char *c1 = strchr(s, ',');
    if (!c1) return false;
    *c1 = 0;
    char *num_s  = trim(s);
    char *rest   = trim(c1 + 1);

    /* Number: base-10 uint16_t. Reject anything past a digit so a garbled
     * line does not silently produce TG 42 out of "42abc". */
    if (!*num_s) return false;
    unsigned long n = 0;
    for (char *p = num_s; *p; p++) {
        if (!isdigit((unsigned char)*p)) return false;
        n = n * 10 + (unsigned long)(*p - '0');
        if (n > 0xFFFFu) return false;
    }
    if (n == 0) return false;   /* TG 0 is never a real TG */

    char *c2 = strchr(rest, ',');
    char *name_s = rest, *cat_s = NULL;
    if (c2) { *c2 = 0; name_s = trim(rest); cat_s = trim(c2 + 1); }
    if (!*name_s) return false;

    if (out) {
        memset(out, 0, sizeof(*out));
        out->number = (uint16_t)n;
        strncpy(out->name, name_s, sizeof(out->name) - 1);
        if (cat_s) strncpy(out->category, cat_s, sizeof(out->category) - 1);
    }
    return true;
}

void p25_scan_names_load(p25_scan_ctrl_t *sc,
                         p25_scan_read_line_fn read_line, void *ctx,
                         size_t file_bytes)
{
    if (!sc || !read_line) return;
    p25_scan_names_clear(sc);
    if (file_bytes > P25_SCAN_NAMES_MAX_FILE_BYTES) {
        sc->names_refused_oversize = true;
        return;
    }

    char line[P25_SCAN_NAMES_MAX_LINE_LEN + 1];
    int lineno = 0;
    for (;;) {
        int got = read_line(ctx, line, sizeof(line));
        if (got <= 0) break;
        lineno++;
        line[sizeof(line) - 1] = 0;

        p25_scan_name_t rec;
        if (!p25_scan_name_parse_line(line, &rec)) {
            /* Blank/comment lines do not count as bad. p25_scan_name_parse_line
             * returns false for both, but a comment is trimmed to empty
             * before the format check - so detect blank/comment here to
             * avoid over-counting. */
            char scratch[P25_SCAN_NAMES_MAX_LINE_LEN + 1];
            strncpy(scratch, line, sizeof(scratch) - 1);
            scratch[sizeof(scratch) - 1] = 0;
            /* strip \r\n */
            for (size_t i = 0; scratch[i]; i++)
                if (scratch[i] == '\r' || scratch[i] == '\n')
                    { scratch[i] = 0; break; }
            char *t = trim(scratch);
            if (*t == 0 || *t == '#' || *t == ';') continue;
            sc->names_bad_lines++;
            if (sc->names_first_bad_line < 0)
                sc->names_first_bad_line = lineno;
            continue;
        }

        if (sc->names_count >= P25_SCAN_NAMES_MAX) {
            sc->names_truncated++;
            continue;
        }
        sc->names[sc->names_count++] = rec;
        sc->names_loaded++;
    }
}

/* -------------------------------------------------------------- decision */

static bool grant_is_duplicate(const p25_grant_follower_t *f, uint16_t tg)
{
    return f && f->state == P25_GRANT_ON_TRAFFIC && f->talkgroup == tg;
}
static bool grant_is_foreign(const p25_grant_follower_t *f, uint16_t tg)
{
    return f && f->state == P25_GRANT_ON_TRAFFIC && f->talkgroup != tg;
}

/* Precedence lives here. Read from the top. */
p25_scan_decision_t
p25_scan_decide(const p25_scan_ctrl_t *sc,
                const p25_grant_follower_t *f,
                uint16_t talkgroup, uint32_t source,
                uint64_t freq_hz, int64_t now_us)
{
    p25_scan_decision_t d = {
        .action = P25_SCAN_STAY,
        .reason = P25_SCAN_REASON_NONE,
        .freq_hz = freq_hz,
        .talkgroup = talkgroup,
        .source = source,
    };

    if (!sc || !f) { d.reason = P25_SCAN_REASON_NONE; return d; }
    if (talkgroup == 0 || freq_hz == 0 || freq_hz > UINT32_MAX) {
        d.reason = P25_SCAN_REASON_NONE; return d;
    }
    /* matching TG alone is not proof this is our current call. */
    if (f->state == P25_GRANT_ON_TRAFFIC && f->talkgroup == talkgroup &&
        f->traffic_hz != freq_hz) {
        d.reason = P25_SCAN_REASON_FOREIGN_GRANT;
        return d;
    }

    /* AUTO FOLLOW is a global gate, not an alternate path around the
     * controller. Re-enabling it merely reaches the existing precedence
     * checks below; disabling it can never cause a traffic retune. */
    /* The roster (lockout, hold, list, priority, a profile's follow-off)
     * is one system's. On another system none of it applies. */
    const bool roster = p25_scan_roster_applies(sc, now_us);
    if (!sc->auto_follow || (roster && sc->roster_follow_off)) {
        d.reason = P25_SCAN_REASON_AUTO_FOLLOW_OFF;
        return d;
    }

    /* 1. LOCKOUT beats everything. */
    if (roster && p25_scan_is_locked_out(sc, talkgroup)) {
        d.reason = P25_SCAN_REASON_LOCKED_OUT;
        return d;
    }

    /* 2. HOLD - a hold on this TG says "this one". */
    bool hold_on = roster && p25_scan_hold_active(sc);
    if (hold_on) {
        if (sc->hold_tg != talkgroup) {
            d.reason = P25_SCAN_REASON_HOLD_MISMATCH;
            return d;
        }

        if (grant_is_duplicate(f, talkgroup)) {
            d.reason = P25_SCAN_REASON_ALREADY_FOLLOWING;
            return d;
        }
        d.action = P25_SCAN_TUNE_TO_TRAFFIC;
        d.reason = P25_SCAN_REASON_HOLD_HIT;
        return d;
    }

    /* 3. LIST MODE. Empty ALLOW follows everything, while NONE rejects every
     *    grant (see p25_scan_is_allowed for the distinction). */
    if (roster && !p25_scan_is_allowed(sc, talkgroup)) {
        d.reason = sc->list_mode == P25_SCAN_LIST_NONE
                 ? P25_SCAN_REASON_LIST_NONE
                 : P25_SCAN_REASON_ALLOW_LIST;
        return d;
    }

    /* Duplicate grant for the call already being followed: STAY. Do this
     * before the priority check so a priority TG that is already tuned does
     * not report as a preempt of itself. */
    if (grant_is_duplicate(f, talkgroup)) {
        d.reason = P25_SCAN_REASON_ALREADY_FOLLOWING;
        return d;
    }

    uint8_t new_rank = roster ? p25_scan_priority_rank(sc, talkgroup) : 0;
    if (grant_is_foreign(f, talkgroup)) {
        uint8_t cur_rank = roster ? p25_scan_priority_rank(sc, f->talkgroup) : 0;
        if (new_rank > cur_rank) {
            /* TUNE + PRIORITY reason - the caller releases the current call
             * first and follows the incoming one. Precedence: encrypted skip
             * on the incoming TG still blocks it, checked below. */
        } else {
            d.reason = P25_SCAN_REASON_FOREIGN_GRANT;
            return d;
        }
    }

    if (p25_grant_tg_is_skipped(f, talkgroup, now_us)) {
        d.reason = P25_SCAN_REASON_ENCRYPTED_SKIP;
        return d;
    }

    d.action = P25_SCAN_TUNE_TO_TRAFFIC;
    d.reason = new_rank > 0 ? P25_SCAN_REASON_PRIORITY
                            : P25_SCAN_REASON_DEFAULT;
    return d;
}

void p25_scan_note_decision(p25_scan_ctrl_t *sc,
                            const p25_scan_decision_t *d)
{
    if (!sc || !d) return;
    switch (d->reason) {
    case P25_SCAN_REASON_LOCKED_OUT:      sc->lockout_hits++;      break;
    case P25_SCAN_REASON_HOLD_MISMATCH:   sc->hold_stays++;        break;
    case P25_SCAN_REASON_ALLOW_LIST:      sc->allow_rejects++;     break;
    case P25_SCAN_REASON_PRIORITY:
        /* A priority reason with a TUNE action is either a plain follow of
         * a priority-listed TG (from control) or a preempt (from traffic).
         * The caller marks the preempt by flipping preempt=true before the
         * call - decide() cannot see the future release. */
        sc->priority_grants++;
        break;
    default: break;
    }
}

p25_scan_decision_t
p25_scan_apply(p25_scan_ctrl_t *sc, p25_grant_follower_t *f,
               uint16_t talkgroup, uint32_t source,
               uint64_t freq_hz, int64_t now_us)
{
    p25_scan_decision_t d = p25_scan_decide(sc, f, talkgroup, source,
                                            freq_hz, now_us);
    if (d.action == P25_SCAN_TUNE_TO_TRAFFIC) {
        /* Priority preempt: on traffic with a different TG. Release the
         * current call before following the incoming grant, or the
         * follower's foreign_grants counter will fire and no retune will
         * happen. */
        if (f && f->state == P25_GRANT_ON_TRAFFIC && f->talkgroup != talkgroup) {
            (void)p25_grant_force_return_to_control(f);
            if (sc) sc->priority_preempts++;
        }
        (void)p25_grant_on_grant(f, talkgroup, source, freq_hz, now_us);
    } else if (d.action == P25_SCAN_RETURN_TO_CONTROL) {
        (void)p25_grant_force_return_to_control(f);
    } else if (d.reason == P25_SCAN_REASON_ALREADY_FOLLOWING) {
        /* Feed the follower even on a duplicate: it needs to refresh the
         * hang timer and increment duplicate_grants. Refuses to retune. */
        (void)p25_grant_on_grant(f, talkgroup, source, freq_hz, now_us);
    }
    p25_scan_note_decision(sc, &d);
    return d;
}

/* ---------------------------------------------------------- roster scope */

static p25_roster_load_fn s_roster_load;
static p25_roster_save_fn s_roster_save;
static p25_roster_switched_fn s_roster_switched;

void p25_scan_set_roster_loader(p25_roster_load_fn fn) { s_roster_load = fn; }
void p25_scan_set_roster_saver(p25_roster_save_fn fn) { s_roster_save = fn; }
void p25_scan_set_roster_switched(p25_roster_switched_fn fn) { s_roster_switched = fn; }

void p25_scan_roster_clear(p25_scan_ctrl_t *sc)
{
    if (!sc) return;
    sc->lockout_count = 0;
    sc->allow_count = 0;
    sc->list_mode = P25_SCAN_LIST_OFF;
    sc->hold_tg = 0;
    sc->roster_follow_off = false;
    p25_scan_priority_clear(sc);
}

void p25_scan_roster_bind(p25_scan_ctrl_t *sc, uint32_t wacn, uint16_t sysid)
{
    if (!sc) return;
    sc->roster_sys.wacn = wacn & 0xfffffu;
    sc->roster_sys.sysid = sysid & 0xfffu;
    sc->roster_sys.valid = sc->roster_sys.sysid != 0;
    if (!sc->roster_sys.valid) sc->roster_sys.wacn = 0;
}

bool p25_scan_roster_empty(const p25_scan_ctrl_t *sc)
{
    return !sc || (!sc->lockout_count && !sc->allow_count && !sc->hold_tg &&
                   sc->list_mode == P25_SCAN_LIST_OFF && !sc->roster_follow_off);
}

static bool same_system(p25_system_id_t a, p25_system_id_t b)
{
    /* SYSID must agree; WACN only where both are known. */
    return a.valid && b.valid && a.sysid == b.sysid &&
           (!a.wacn || !b.wacn || a.wacn == b.wacn);
}

/* Rescope the roster in memory to the current system, keeping its
 * contents, and move its stored copy to the new key. */
static void roster_rebind(p25_scan_ctrl_t *sc)
{
    const p25_system_id_t from = sc->roster_sys;
    sc->roster_pending_bind = false;
    p25_scan_roster_bind(sc, sc->current_sys.wacn, sc->current_sys.sysid);
    if (s_roster_save) s_roster_save(sc, from);
}

/* Replace the roster in memory with the current system's stored one. */
static void roster_swap(p25_scan_ctrl_t *sc)
{
    const p25_system_id_t cur = sc->current_sys;
    sc->roster_pending_bind = false;
    p25_scan_roster_clear(sc);
    p25_scan_roster_bind(sc, cur.wacn, cur.sysid);
    if (s_roster_load) s_roster_load(sc, cur.wacn, cur.sysid);
    /* The loader may only find a SYSID-only roster; it belongs here now. */
    p25_scan_roster_bind(sc, cur.wacn, cur.sysid);
    sc->roster_switches++;
    if (s_roster_switched) s_roster_switched(sc);
}

/* Swap in the stored roster for the current, fully named system. */
static bool roster_switch(p25_scan_ctrl_t *sc)
{
    const p25_system_id_t cur = sc->current_sys;
    if (!cur.valid || !cur.wacn) return false;
    if (sc->roster_sys.valid && sc->roster_sys.wacn == cur.wacn &&
        sc->roster_sys.sysid == cur.sysid) return false;
    if (!sc->roster_sys.valid && sc->roster_pending_bind) {
        /* Made here before the system said its name: it is this one's. */
        roster_rebind(sc);
        return false;
    }
    if (sc->roster_sys.valid && !sc->roster_sys.wacn &&
        sc->roster_sys.sysid == cur.sysid) {
        /* Made when only the SYSID was known (a profile with no wacn=): it
         * is this system's, and newer than anything under the full key. */
        roster_rebind(sc);
        return false;
    }
    roster_swap(sc);
    return true;
}

bool p25_scan_note_system(p25_scan_ctrl_t *sc, const dsd_state *s,
                          int64_t now_us)
{
    if (!sc || !s) return false;
    p25_system_id_t id = { 0 };
    if (s->p25_net_valid && s->p25_tsbk_sysid) {
        id.wacn = s->p25_tsbk_wacn & 0xfffffu;
        id.sysid = s->p25_tsbk_sysid & 0xfffu;
        id.valid = true;
    } else if (s->p25_rfss_valid && s->p25_rfss_sysid) {
        id.sysid = s->p25_rfss_sysid & 0xfffu;
        id.valid = true;
    }
    if (!id.valid) {
        sc->current_sys.valid = false;
        if (!sc->sys_unknown_since_us) sc->sys_unknown_since_us = now_us ? now_us : 1;
        return false;
    }
    sc->current_sys = id;
    sc->sys_unknown_since_us = 0;
    return roster_switch(sc);
}

bool p25_scan_roster_resync(p25_scan_ctrl_t *sc)
{
    return sc ? roster_switch(sc) : false;
}

bool p25_scan_roster_for_edit(p25_scan_ctrl_t *sc)
{
    if (!sc || !sc->current_sys.valid) return false;
    if (roster_switch(sc)) return true;
    if (!sc->roster_sys.valid && sc->roster_pending_bind) {
        roster_rebind(sc);
        return false;
    }
    /* Only the SYSID is known here and the roster in memory is another
     * system's: edit this system's roster, not that one. */
    if (sc->roster_sys.valid && same_system(sc->current_sys, sc->roster_sys))
        return false;
    roster_swap(sc);
    return true;
}

bool p25_scan_roster_applies(const p25_scan_ctrl_t *sc, int64_t now_us)
{
    if (!sc) return true;
    if (!sc->roster_sys.valid) {
        /* Made this session before any system was named: it is for here. */
        if (sc->roster_pending_bind) return true;
        /* Saved unscoped by an older build: nobody knows which system it
         * was for, so it is not applied to a system that has named itself
         * (a SYSID-only site never swaps rosters and would keep it). */
        if (sc->current_sys.valid) return false;
    } else if (sc->current_sys.valid) {
        return same_system(sc->current_sys, sc->roster_sys);
    }
    /* Not named yet: hold to the roster briefly (a fresh tune at home must
     * not leak a locked-out call), then stop, so a site that never names
     * itself is not held to another system's list. */
    return !sc->sys_unknown_since_us ||
           now_us - sc->sys_unknown_since_us < P25_ROSTER_UNKNOWN_GRACE_US;
}

/* ---------------------------------------------------------- follow trace */

void p25_scan_trace_grant(p25_scan_ctrl_t *sc, const p25_call_info_t *g,
                          p25_follow_outcome_t outcome,
                          p25_scan_reason_t reason, int64_t now_us)
{
    if (!sc || !g || outcome >= P25_FOLLOW_OUTCOME_COUNT) return;
    sc->follow_outcomes[outcome]++;
    if (outcome == P25_FOLLOW_DUPLICATE) return;
    p25_follow_trace_t t = {
        .at_us = now_us,
        .carrier_hz = g->carrier_hz <= UINT32_MAX ? (uint32_t)g->carrier_hz : 0,
        .talkgroup = g->talkgroup,
        .channel = g->channel,
        .support = (uint8_t)g->support,
        .slot = g->slot,
        .slots = g->slots_per_carrier,
        .outcome = (uint8_t)outcome,
        .reason = (uint8_t)reason,
    };
    sc->follow_trace[sc->follow_trace_total % P25_FOLLOW_TRACE_LEN] = t;
    sc->follow_trace_total++;
    if (outcome != P25_FOLLOW_FOLLOWED) sc->follow_last_left = t;
    else memset(&sc->follow_last_left, 0, sizeof(sc->follow_last_left));
}

void p25_scan_trace_busy(p25_scan_ctrl_t *sc, const dsd_state *s,
                         int64_t now_us)
{
    if (!sc || !s || !s->p25_grant_batch_valid || !s->p25_grant_count ||
        s->p25_grant_count > P25_GRANTS_PER_TSDU ||
        sc->follow_busy_generation == s->p25_grant_generation) return;
    sc->follow_busy_generation = s->p25_grant_generation;
    for (unsigned int i = 0; i < s->p25_grant_count; i++)
        p25_scan_trace_grant(sc, &s->p25_grants[i], P25_FOLLOW_BUSY,
                             P25_SCAN_REASON_NONE, now_us);
}

static p25_follow_outcome_t unfollowable_outcome(const p25_grant_follower_t *f,
                                                 const p25_call_info_t *g)
{
    switch (g->support) {
    case P25_CALL_PHASE2:
        if (!f->phase2_follow) return P25_FOLLOW_P2_OFF;
        if (!g->system_valid || !g->wacn || !g->sysid)
            return P25_FOLLOW_P2_NO_SYSTEM;
        return P25_FOLLOW_UNSUPPORTED;
    case P25_CALL_MISSING_IDEN:
    case P25_CALL_INVALID:
        return P25_FOLLOW_UNRESOLVED;
    default:
        return P25_FOLLOW_UNSUPPORTED;
    }
}

const char *p25_follow_outcome_name(p25_follow_outcome_t o)
{
    static const char *const names[P25_FOLLOW_OUTCOME_COUNT] = {
        "followed", "busy", "unresolved", "unsupported", "p2-off",
        "p2-no-system", "policy", "refused", "duplicate",
    };
    return o < P25_FOLLOW_OUTCOME_COUNT ? names[o] : "?";
}

const char *p25_scan_reason_name(p25_scan_reason_t r)
{
    static const char *const names[] = {
        "none", "auto-follow-off", "locked-out", "hold-mismatch", "hold-hit",
        "allow-list", "list-none", "priority", "encrypted-skip",
        "already-following", "foreign-grant", "default",
    };
    return (unsigned)r < sizeof(names) / sizeof(names[0]) ? names[r] : "?";
}

const char *p25_follow_left_label(const p25_follow_trace_t *t)
{
    if (!t || !t->talkgroup) return NULL;
    switch ((p25_follow_outcome_t)t->outcome) {
    case P25_FOLLOW_BUSY:         return "SKIP: SCAN BUSY";
    case P25_FOLLOW_UNRESOLVED:   return "UNRESOLVED GRANT";
    case P25_FOLLOW_UNSUPPORTED:  return "UNSUPPORTED GRANT";
    case P25_FOLLOW_P2_OFF:       return "SKIP: PHASE II OFF";
    case P25_FOLLOW_P2_NO_SYSTEM: return "SKIP: NO SYSTEM ID";
    case P25_FOLLOW_REFUSED:      return "SKIP: OTHER CALL";
    case P25_FOLLOW_POLICY:
        switch ((p25_scan_reason_t)t->reason) {
        case P25_SCAN_REASON_AUTO_FOLLOW_OFF: return "SKIP: FOLLOW OFF";
        case P25_SCAN_REASON_LOCKED_OUT:      return "SKIP: LOCKED OUT";
        case P25_SCAN_REASON_HOLD_MISMATCH:   return "SKIP: HOLD";
        case P25_SCAN_REASON_ALLOW_LIST:      return "SKIP: NOT ALLOWED";
        case P25_SCAN_REASON_LIST_NONE:       return "SKIP: FOLLOW NONE";
        case P25_SCAN_REASON_ENCRYPTED_SKIP:  return "SKIP: ENCRYPTED";
        case P25_SCAN_REASON_FOREIGN_GRANT:   return "SKIP: OTHER CALL";
        default:                              return "SKIP: POLICY";
        }
    default:
        return NULL;
    }
}

bool p25_scan_apply_from_state(p25_scan_ctrl_t *sc, p25_grant_follower_t *f,
                               const dsd_state *state, int64_t now_us)
{
    if (!sc || !f || !state) return false;
    (void)p25_scan_note_system(sc, state, now_us);
    if (state->p25_grant_batch_valid) {
        if (!p25_grant_take_batch(f, state)) return false;
        bool tuned = false;
        for (unsigned int i = 0; i < state->p25_grant_count; i++) {
            const p25_call_info_t *g = &state->p25_grants[i];
            if (!p25_grant_call_followable(f, g)) {
                p25_scan_trace_grant(sc, g, unfollowable_outcome(f, g),
                                     P25_SCAN_REASON_NONE, now_us);
                p25_grant_observe(f, g);
                continue;
            }
            p25_scan_decision_t d = p25_scan_decide(sc, f, g->talkgroup,
                                                    g->source, g->carrier_hz,
                                                    now_us);
            if (d.action == P25_SCAN_TUNE_TO_TRAFFIC ||
                d.reason == P25_SCAN_REASON_ALREADY_FOLLOWING) {
                if (f->state == P25_GRANT_ON_TRAFFIC &&
                    f->talkgroup != g->talkgroup) {
                    (void)p25_grant_force_return_to_control(f);
                    sc->priority_preempts++;
                }
                const unsigned int followed = f->followed_count;
                const bool on_call = p25_grant_on_call(f, g, now_us);
                tuned |= on_call;
                p25_scan_trace_grant(sc, g,
                    f->followed_count != followed ? P25_FOLLOW_FOLLOWED :
                    d.reason == P25_SCAN_REASON_ALREADY_FOLLOWING ||
                        (f->state == P25_GRANT_ON_TRAFFIC &&
                         f->talkgroup == g->talkgroup)
                        ? P25_FOLLOW_DUPLICATE : P25_FOLLOW_REFUSED,
                    d.reason, now_us);
            } else {
                p25_scan_trace_grant(sc, g, P25_FOLLOW_POLICY, d.reason, now_us);
                p25_grant_observe(f, g);
            }
            p25_scan_note_decision(sc, &d);
        }
        return tuned;
    }
    uint8_t op = state->p25_tsbk_last_opcode;
    /* Voice-grant whitelist, matching grant_follower.c. */
    if (op != 0x00 && op != 0x02 && op != 0x03) return false;
    p25_scan_decision_t d =
        p25_scan_apply(sc, f, state->p25_tsbk_talkgroup,
                       state->p25_tsbk_source,
                       state->p25_tsbk_frequency_hz, now_us);
    return d.action != P25_SCAN_STAY;
}

/* ----------------------------------------------------------- persistence */

/* Wire layout (little-endian, packed by hand - no host/target width surprises):
 *
 *   magic:u32  version:u16  flags:u16
 *   hold_tg:u16  list_mode:u8  _pad:u8
 *   lockout_count:u16  allow_count:u16
 *   lockout[lockout_count]:u16
 *   allow[allow_count]:u16
 *   [flags bit 0] wacn:u32 sysid:u16   - the system the roster belongs to
 *
 * flags was "reserved", always written 0 and never read, and older builds
 * ignore bytes past the allow list, so a scoped blob still loads there (as
 * unscoped) and an older blob loads here as unscoped. No version bump.
 * Priority and names are not persisted (see scan_ctrl.h). */
#define P25_SCAN_FLAG_SCOPED     0x0001u
#define P25_SCAN_FLAG_FOLLOW_OFF 0x0002u  /* roster_follow_off */

static void wr_u8 (uint8_t **p, uint8_t v)  { *(*p)++ = v; }
static void wr_u16(uint8_t **p, uint16_t v)
{
    (*p)[0] = (uint8_t)(v & 0xff);
    (*p)[1] = (uint8_t)(v >> 8);
    *p += 2;
}
static void wr_u32(uint8_t **p, uint32_t v)
{
    (*p)[0] = (uint8_t)(v & 0xff);
    (*p)[1] = (uint8_t)((v >> 8) & 0xff);
    (*p)[2] = (uint8_t)((v >> 16) & 0xff);
    (*p)[3] = (uint8_t)(v >> 24);
    *p += 4;
}
static uint8_t  rd_u8 (const uint8_t **p)
{
    uint8_t v = **p; (*p)++; return v;
}
/* Shift on uint32_t, not on the promoted int. */

static uint16_t rd_u16(const uint8_t **p)
{
    uint16_t v = (uint16_t)((uint32_t)(*p)[0] |
                            ((uint32_t)(*p)[1] << 8));
    *p += 2; return v;
}
static uint32_t rd_u32(const uint8_t **p)
{
    uint32_t v = (uint32_t)(*p)[0]        | ((uint32_t)(*p)[1] << 8) |
                 ((uint32_t)(*p)[2] << 16) | ((uint32_t)(*p)[3] << 24);
    *p += 4; return v;
}

size_t p25_scan_persist_size(const p25_scan_ctrl_t *sc)
{
    if (!sc) return 0;
    return 4 + 2 + 2                       /* magic + ver + reserved         */
         + 2 + 1 + 1                       /* hold + mode + pad              */
         + 2 + 2                           /* counts                          */
         + (size_t)sc->lockout_count * 2
         + (size_t)sc->allow_count   * 2
         + (sc->roster_sys.valid ? 4 + 2 : 0);
}

size_t p25_scan_persist_save(const p25_scan_ctrl_t *sc, uint8_t *buf,
                             size_t buf_len)
{
    if (!sc || !buf) return 0;
    size_t need = p25_scan_persist_size(sc);
    if (buf_len < need) return 0;

    uint8_t *p = buf;
    wr_u32(&p, P25_SCAN_NVS_MAGIC);
    wr_u16(&p, P25_SCAN_NVS_VER);
    wr_u16(&p, (uint16_t)((sc->roster_sys.valid ? P25_SCAN_FLAG_SCOPED : 0) |
                          (sc->roster_follow_off ? P25_SCAN_FLAG_FOLLOW_OFF : 0)));
    wr_u16(&p, sc->hold_tg);
    wr_u8 (&p, (uint8_t)sc->list_mode);
    wr_u8 (&p, 0);
    wr_u16(&p, sc->lockout_count);
    wr_u16(&p, sc->allow_count);
    for (size_t i = 0; i < sc->lockout_count; i++) wr_u16(&p, sc->lockout[i]);
    for (size_t i = 0; i < sc->allow_count;   i++) wr_u16(&p, sc->allow[i]);
    if (sc->roster_sys.valid) {
        wr_u32(&p, sc->roster_sys.wacn);
        wr_u16(&p, sc->roster_sys.sysid);
    }
    return (size_t)(p - buf);
}

typedef struct {
    uint16_t hold, lc, ac;
    uint8_t  mode;
    uint16_t lockout[P25_SCAN_LOCKOUT_MAX];
    uint16_t allow[P25_SCAN_ALLOW_MAX];
    p25_system_id_t sys;
    bool follow_off;
} roster_blob_t;

/* Parse without touching the controller, so a bad blob changes nothing. */
static bool roster_parse(const uint8_t *buf, size_t buf_len, roster_blob_t *r)
{
    if (!buf || buf_len < 12) return false;
    const uint8_t *p = buf;
    uint32_t magic = rd_u32(&p);
    uint16_t ver   = rd_u16(&p);
    uint16_t flags = rd_u16(&p);
    if (magic != P25_SCAN_NVS_MAGIC || ver != P25_SCAN_NVS_VER) return false;

    /* Header consumed 8 bytes; body-scalar block is 8 more (hold + mode +
     * pad + lc + ac). Then per-entry arrays. */
    if (buf_len < 8 + 8) return false;
    r->hold = rd_u16(&p);
    r->mode = rd_u8(&p);
    (void)rd_u8(&p);
    r->lc   = rd_u16(&p);
    r->ac   = rd_u16(&p);

    if (r->lc > P25_SCAN_LOCKOUT_MAX || r->ac > P25_SCAN_ALLOW_MAX) return false;
    size_t need = 16 + (size_t)r->lc * 2 + (size_t)r->ac * 2;
    if (buf_len < need) return false;
    for (size_t i = 0; i < r->lc; i++) r->lockout[i] = rd_u16(&p);
    for (size_t i = 0; i < r->ac; i++) r->allow[i]   = rd_u16(&p);
    memset(&r->sys, 0, sizeof(r->sys));
    r->follow_off = (flags & P25_SCAN_FLAG_FOLLOW_OFF) != 0;
    if (flags & P25_SCAN_FLAG_SCOPED) {
        if (buf_len < need + 6) return false;
        r->sys.wacn  = rd_u32(&p) & 0xfffffu;
        r->sys.sysid = rd_u16(&p) & 0xfffu;
        r->sys.valid = r->sys.sysid != 0;
    }
    return true;
}

static void roster_apply(p25_scan_ctrl_t *sc, const roster_blob_t *r)
{
    p25_scan_roster_clear(sc);
    sc->hold_tg = r->hold;
    if (r->mode == P25_SCAN_LIST_ALLOW)
        sc->list_mode = P25_SCAN_LIST_ALLOW;
    else if (r->mode == P25_SCAN_LIST_NONE)
        sc->list_mode = P25_SCAN_LIST_NONE;
    else
        sc->list_mode = P25_SCAN_LIST_OFF;
    sc->lockout_count = r->lc;
    sc->allow_count   = r->ac;
    memcpy(sc->lockout, r->lockout, (size_t)r->lc * 2);
    memcpy(sc->allow, r->allow, (size_t)r->ac * 2);
    sc->roster_sys = r->sys;
    sc->roster_follow_off = r->follow_off;
}

bool p25_scan_persist_load(p25_scan_ctrl_t *sc, const uint8_t *buf,
                           size_t buf_len)
{
    if (!sc) return false;
    static roster_blob_t r;   /* ~270 B; owner task and bench only */
    if (!roster_parse(buf, buf_len, &r)) return false;
    /* Only mutate on success. */
    p25_scan_init(sc);
    roster_apply(sc, &r);
    return true;
}

bool p25_scan_persist_load_roster(p25_scan_ctrl_t *sc, const uint8_t *buf,
                                  size_t buf_len)
{
    if (!sc) return false;
    static roster_blob_t r;
    if (!roster_parse(buf, buf_len, &r)) return false;
    roster_apply(sc, &r);
    return true;
}
