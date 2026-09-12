/* scan controller. */

#include "scan_ctrl.h"

#include <ctype.h>
#include <string.h>

/* Single global instance. Constructing more than one would only mean two
 * radios in one firmware, which we do not have. */
p25_scan_ctrl_t g_p25_scan;

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
    return sc ? sc->auto_follow : true;
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
    if (!sc->auto_follow) {
        d.reason = P25_SCAN_REASON_AUTO_FOLLOW_OFF;
        return d;
    }

    /* 1. LOCKOUT beats everything. */
    if (p25_scan_is_locked_out(sc, talkgroup)) {
        d.reason = P25_SCAN_REASON_LOCKED_OUT;
        return d;
    }

    /* 2. HOLD - a hold on this TG says "this one". */
    bool hold_on = p25_scan_hold_active(sc);
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
    if (!p25_scan_is_allowed(sc, talkgroup)) {
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

    uint8_t new_rank = p25_scan_priority_rank(sc, talkgroup);
    if (grant_is_foreign(f, talkgroup)) {
        uint8_t cur_rank = p25_scan_priority_rank(sc, f->talkgroup);
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

bool p25_scan_apply_from_state(p25_scan_ctrl_t *sc, p25_grant_follower_t *f,
                               const dsd_state *state, int64_t now_us)
{
    if (!sc || !f || !state) return false;
    if (state->p25_grant_batch_valid) {
        if (!p25_grant_take_batch(f, state)) return false;
        bool tuned = false;
        for (unsigned int i = 0; i < state->p25_grant_count; i++) {
            const p25_call_info_t *g = &state->p25_grants[i];
            if (g->support != P25_CALL_PHASE1 || g->slots_per_carrier != 1 ||
                g->slot != 0) {
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
                tuned |= p25_grant_on_call(f, g, now_us);
            } else {
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
 *   magic:u32  version:u16  reserved:u16
 *   hold_tg:u16  list_mode:u8  _pad:u8
 *   lockout_count:u16  allow_count:u16
 *   lockout[lockout_count]:u16
 *   allow[allow_count]:u16
 *
 * Priority and names are not persisted (see scan_ctrl.h). */

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
         + (size_t)sc->allow_count   * 2;
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
    wr_u16(&p, 0);
    wr_u16(&p, sc->hold_tg);
    wr_u8 (&p, (uint8_t)sc->list_mode);
    wr_u8 (&p, 0);
    wr_u16(&p, sc->lockout_count);
    wr_u16(&p, sc->allow_count);
    for (size_t i = 0; i < sc->lockout_count; i++) wr_u16(&p, sc->lockout[i]);
    for (size_t i = 0; i < sc->allow_count;   i++) wr_u16(&p, sc->allow[i]);
    return (size_t)(p - buf);
}

bool p25_scan_persist_load(p25_scan_ctrl_t *sc, const uint8_t *buf,
                           size_t buf_len)
{
    if (!sc) return false;
    if (!buf || buf_len < 12) return false;
    const uint8_t *p = buf;
    uint32_t magic = rd_u32(&p);
    uint16_t ver   = rd_u16(&p);
    (void)rd_u16(&p);
    if (magic != P25_SCAN_NVS_MAGIC || ver != P25_SCAN_NVS_VER) return false;

    /* Header consumed 8 bytes; body-scalar block is 8 more (hold + mode +
     * pad + lc + ac). Then per-entry arrays. */
    if (buf_len < 8 + 8) return false;
    uint16_t hold = rd_u16(&p);
    uint8_t  mode = rd_u8(&p);
    (void)rd_u8(&p);
    uint16_t lc   = rd_u16(&p);
    uint16_t ac   = rd_u16(&p);

    if (lc > P25_SCAN_LOCKOUT_MAX || ac > P25_SCAN_ALLOW_MAX) return false;
    size_t need = 16 + (size_t)lc * 2 + (size_t)ac * 2;
    if (buf_len < need) return false;

    /* Only mutate on success. */
    p25_scan_init(sc);
    sc->hold_tg = hold;
    if (mode == P25_SCAN_LIST_ALLOW)
        sc->list_mode = P25_SCAN_LIST_ALLOW;
    else if (mode == P25_SCAN_LIST_NONE)
        sc->list_mode = P25_SCAN_LIST_NONE;
    else
        sc->list_mode = P25_SCAN_LIST_OFF;
    sc->lockout_count = lc;
    sc->allow_count   = ac;
    for (size_t i = 0; i < lc; i++) sc->lockout[i] = rd_u16(&p);
    for (size_t i = 0; i < ac; i++) sc->allow[i]   = rd_u16(&p);
    return true;
}
