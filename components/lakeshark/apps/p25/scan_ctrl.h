/* one component owns tuning. */

#ifndef P25_SCAN_CTRL_H
#define P25_SCAN_CTRL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd.h"
#include "grant_follower.h"

/* Bounded tables - all in internal RAM as part of the singleton. A busy site
 * that fills the lockout list can drop a rare-TG lockout without breaking
 * the interesting ones; the design choice is documented in test cases. */
#define P25_SCAN_LOCKOUT_MAX   64
#define P25_SCAN_ALLOW_MAX     64
#define P25_SCAN_PRIORITY_MAX  16
#define P25_SCAN_NAMES_MAX     256
#define P25_SCAN_NAME_LEN      24
#define P25_SCAN_CATEGORY_LEN  16

/* Wire format for NVS. The blob is versioned so a rename or a new field
 * cannot silently reinterpret bytes from a prior build. */
#define P25_SCAN_NVS_MAGIC     0x50533241u  /* 'PS2A' - P25 Scan v2A          */
#define P25_SCAN_NVS_VER       1

typedef enum {
    P25_SCAN_LIST_OFF   = 0,  /* no filter - follow anything not locked out  */
    P25_SCAN_LIST_ALLOW = 1,  /* follow only listed talkgroups                */
    P25_SCAN_LIST_NONE  = 2,  /* remain on control; follow no talkgroups      */
} p25_scan_list_mode_t;

typedef enum {
    P25_SCAN_TUNE_TO_TRAFFIC = 0,  /* retune to freq_hz, mark as new follow  */
    P25_SCAN_RETURN_TO_CONTROL,    /* leave the current call, go to control  */
    P25_SCAN_STAY,                 /* do nothing                              */
} p25_scan_action_t;

typedef enum {
    P25_SCAN_REASON_NONE = 0,
    P25_SCAN_REASON_AUTO_FOLLOW_OFF,
    P25_SCAN_REASON_LOCKED_OUT,
    P25_SCAN_REASON_HOLD_MISMATCH,   /* hold set, grant is for a different TG */
    P25_SCAN_REASON_HOLD_HIT,        /* grant matches the hold TG            */
    P25_SCAN_REASON_ALLOW_LIST,      /* rejected by allow list                */
    P25_SCAN_REASON_LIST_NONE,       /* follow-none mode rejected the grant   */
    P25_SCAN_REASON_PRIORITY,        /* priority TG preempted a lower call   */
    P25_SCAN_REASON_ENCRYPTED_SKIP,  /* skip window still open                */
    P25_SCAN_REASON_ALREADY_FOLLOWING,
    P25_SCAN_REASON_FOREIGN_GRANT,   /* on traffic, different TG              */
    P25_SCAN_REASON_DEFAULT,         /* nothing said no - a plain follow      */
} p25_scan_reason_t;

typedef struct {
    p25_scan_action_t action;
    p25_scan_reason_t reason;
    uint64_t          freq_hz;       /* target frequency if TUNE_TO_TRAFFIC   */
    uint16_t          talkgroup;
    uint32_t          source;
} p25_scan_decision_t;

/* What became of one voice grant. FOLLOWED tuned to it; the rest say why
 * the grant was left. DUPLICATE is a grant for the call already followed. */
typedef enum {
    P25_FOLLOW_FOLLOWED = 0,
    P25_FOLLOW_BUSY,          /* carrier scan, survey or split owns tuning   */
    P25_FOLLOW_UNRESOLVED,    /* no IDEN for the channel, or a bad channel   */
    P25_FOLLOW_UNSUPPORTED,   /* channel type this receiver cannot follow    */
    P25_FOLLOW_P2_OFF,        /* Phase II grant with Phase II follow off     */
    P25_FOLLOW_P2_NO_SYSTEM,  /* Phase II grant before WACN/SYSID are known  */
    P25_FOLLOW_POLICY,        /* p25_scan_decide stayed; see the reason      */
    P25_FOLLOW_REFUSED,       /* the follower refused (other call, skip)     */
    P25_FOLLOW_DUPLICATE,
    P25_FOLLOW_OUTCOME_COUNT
} p25_follow_outcome_t;

#define P25_FOLLOW_TRACE_LEN 8

typedef struct {
    int64_t  at_us;
    uint32_t carrier_hz;
    uint16_t talkgroup;
    uint16_t channel;
    uint8_t  support;         /* p25_call_support_t                          */
    uint8_t  slot;
    uint8_t  slots;
    uint8_t  outcome;         /* p25_follow_outcome_t                        */
    uint8_t  reason;          /* p25_scan_reason_t, for POLICY               */
} p25_follow_trace_t;

typedef struct {
    uint16_t number;
    char     name[P25_SCAN_NAME_LEN];
    char     category[P25_SCAN_CATEGORY_LEN];
} p25_scan_name_t;

/* A P25 system as the control channel names it. wacn 0 = not heard (a
 * site that sent RFSS_STS_BCST, which carries SYSID only). */
typedef struct {
    uint32_t wacn;
    uint16_t sysid;
    bool     valid;
} p25_system_id_t;

/* How long a roster scoped to one system keeps applying on a control
 * channel that has not said which system it is. NET_STS_BCST comes every
 * few seconds; a site that never sends one must not be held to another
 * system's list forever. */
#define P25_ROSTER_UNKNOWN_GRACE_US 10000000LL

typedef struct {

    bool auto_follow;

    /* Persisted --------------------------------------------------------- */
    uint16_t lockout[P25_SCAN_LOCKOUT_MAX];
    uint16_t lockout_count;

    uint16_t allow[P25_SCAN_ALLOW_MAX];
    uint16_t allow_count;
    p25_scan_list_mode_t list_mode;

    uint16_t hold_tg;         /* 0 = no hold                                  */

    /* The system the roster above (lockouts, allow list and mode, hold,
     * priorities) belongs to. !valid = unscoped: a roster saved before
     * scoping existed, or made where no system was ever named. It applies
     * everywhere, as before. A scoped roster applies only on its system. */
    p25_system_id_t roster_sys;
    /* A profile's auto_follow=false, kept with its system's roster: the
     * radio stays on control there and only there. The global switch
     * (auto_follow) still rules everywhere. */
    bool     roster_follow_off;

    /* Runtime: the system on the control channel now. */
    p25_system_id_t current_sys;
    int64_t  sys_unknown_since_us;   /* 0 = known, or nothing heard yet  */
    uint32_t roster_switches;        /* rosters swapped on a new system  */
    /* An unscoped roster made in this session (a profile or an edit before
     * any system was named) goes to the first system named, where an old
     * unscoped roster from NVS is set aside instead. */
    bool     roster_pending_bind;

    /* Runtime only ------------------------------------------------------ */

    struct {
        uint16_t talkgroup;
        uint8_t  rank;
    } priority[P25_SCAN_PRIORITY_MAX];
    uint16_t priority_count;

    /* Names table. Loaded from SD. Not persisted through NVS - the file on
     * SD is authoritative. */
    p25_scan_name_t names[P25_SCAN_NAMES_MAX];
    uint16_t names_count;

    uint32_t lockout_hits;
    uint32_t hold_stays;
    uint32_t allow_rejects;
    uint32_t priority_preempts;
    uint32_t priority_grants;

    /* Every voice grant's outcome, counted, plus the last few that were
     * not duplicates. Read by 'p25 follow' and the P25 screen. */
    uint32_t follow_outcomes[P25_FOLLOW_OUTCOME_COUNT];
    p25_follow_trace_t follow_trace[P25_FOLLOW_TRACE_LEN];
    uint32_t follow_trace_total;
    p25_follow_trace_t follow_last_left;   /* newest grant not followed */
    uint32_t follow_busy_generation;

    /* File-load diagnostics from the last p25_scan_names_load(). Kept on the
     * controller so a UI can display the report without holding the file
     * open. */
    uint32_t names_loaded;
    uint32_t names_bad_lines;
    uint32_t names_truncated;   /* entries dropped because table was full     */
    int32_t  names_first_bad_line;  /* -1 if none                              */
    bool     names_refused_oversize;
} p25_scan_ctrl_t;

/* Global singleton. Only app_p25.c and the console command construct one;
 * everything else uses this handle. */
extern p25_scan_ctrl_t g_p25_scan;

void p25_scan_init(p25_scan_ctrl_t *sc);

bool p25_scan_set_auto_follow(p25_scan_ctrl_t *sc, p25_grant_follower_t *f,
                              bool enabled);
bool p25_scan_get_auto_follow(const p25_scan_ctrl_t *sc);
void p25_scan_restore_controls(p25_scan_ctrl_t *sc,
                               p25_grant_follower_t *f,
                               bool auto_follow,
                               bool leave_on_encrypted,
                               uint32_t encrypted_skip_ms);

/* Hold / lockout / allow list -------------------------------------------- */
void     p25_scan_hold_set(p25_scan_ctrl_t *sc, uint16_t tg);   /* 0 = clear */
uint16_t p25_scan_hold_get(const p25_scan_ctrl_t *sc);
bool     p25_scan_hold_active(const p25_scan_ctrl_t *sc);

bool     p25_scan_lockout_add(p25_scan_ctrl_t *sc, uint16_t tg);
bool     p25_scan_lockout_remove(p25_scan_ctrl_t *sc, uint16_t tg);
bool     p25_scan_is_locked_out(const p25_scan_ctrl_t *sc, uint16_t tg);
size_t   p25_scan_lockout_count(const p25_scan_ctrl_t *sc);

void     p25_scan_list_set_mode(p25_scan_ctrl_t *sc, p25_scan_list_mode_t m);
p25_scan_list_mode_t p25_scan_list_get_mode(const p25_scan_ctrl_t *sc);
bool     p25_scan_allow_add(p25_scan_ctrl_t *sc, uint16_t tg);
bool     p25_scan_allow_remove(p25_scan_ctrl_t *sc, uint16_t tg);
/* drop every allow-list entry without touching lockouts or mode. */
void     p25_scan_allow_clear(p25_scan_ctrl_t *sc);
/* expose membership/count to roster views without making them walk
 * the controller's bounded storage and thereby creating a second policy. */
bool     p25_scan_allow_contains(const p25_scan_ctrl_t *sc, uint16_t tg);
size_t   p25_scan_allow_count(const p25_scan_ctrl_t *sc);
bool     p25_scan_is_allowed(const p25_scan_ctrl_t *sc, uint16_t tg);

/* Priority --------------------------------------------------------------- */
bool     p25_scan_priority_set(p25_scan_ctrl_t *sc, uint16_t tg, uint8_t rank);
uint8_t  p25_scan_priority_rank(const p25_scan_ctrl_t *sc, uint16_t tg);
void     p25_scan_priority_clear(p25_scan_ctrl_t *sc);
size_t   p25_scan_priority_count(const p25_scan_ctrl_t *sc);

/* Names ------------------------------------------------------------------ */
const p25_scan_name_t *p25_scan_name_lookup(const p25_scan_ctrl_t *sc,
                                            uint16_t tg);

bool p25_scan_name_parse_line(const char *line, p25_scan_name_t *out);

/* Feed a whole file. */

#define P25_SCAN_NAMES_MAX_LINE_LEN  128
#define P25_SCAN_NAMES_MAX_FILE_BYTES (P25_SCAN_NAMES_MAX_LINE_LEN * \
                                       (P25_SCAN_NAMES_MAX + 32))

typedef int (*p25_scan_read_line_fn)(void *ctx, char *buf, size_t buf_len);
void p25_scan_names_load(p25_scan_ctrl_t *sc,
                         p25_scan_read_line_fn read_line, void *ctx,
                         size_t file_bytes);
void p25_scan_names_clear(p25_scan_ctrl_t *sc);

/* Roster scope ----------------------------------------------------------- */

/* Loads the roster stored for (wacn, sysid) into sc's roster fields, or
 * leaves them cleared if none is stored. Set by the NVS layer; NULL (the
 * bench default) means no stored rosters. */
typedef void (*p25_roster_load_fn)(p25_scan_ctrl_t *sc, uint32_t wacn,
                                   uint16_t sysid);
void p25_scan_set_roster_loader(p25_roster_load_fn fn);
/* Stores the roster now in sc under its scope and drops the copy under
 * moved_from, the scope it just left (!valid = the unscoped slot). Set by
 * the NVS layer. */
typedef void (*p25_roster_save_fn)(p25_scan_ctrl_t *sc,
                                   p25_system_id_t moved_from);
void p25_scan_set_roster_saver(p25_roster_save_fn fn);
/* Called after another system's roster was swapped in. Stored rosters
 * carry no priorities: the profile layer puts its ranks back from here. */
typedef void (*p25_roster_switched_fn)(p25_scan_ctrl_t *sc);
void p25_scan_set_roster_switched(p25_roster_switched_fn fn);

/* Take the system identity from the decoder. A newly named system swaps
 * in that system's roster. Returns true when the roster changed. */
bool p25_scan_note_system(p25_scan_ctrl_t *sc, const dsd_state *s,
                          int64_t now_us);
/* Whether lockouts, hold, allow list and priorities apply right now. */
bool p25_scan_roster_applies(const p25_scan_ctrl_t *sc, int64_t now_us);
/* Empty the roster (lockouts, allow, mode, hold, priorities); scope kept. */
void p25_scan_roster_clear(p25_scan_ctrl_t *sc);
/* Scope the roster in memory to a system (sysid 0 = unscoped). */
void p25_scan_roster_bind(p25_scan_ctrl_t *sc, uint32_t wacn, uint16_t sysid);
/* True when the roster holds nothing worth storing. */
bool p25_scan_roster_empty(const p25_scan_ctrl_t *sc);
/* After a roster was written for another system (a profile for somewhere
 * else), swap back to the current system's own. True when it swapped. */
bool p25_scan_roster_resync(p25_scan_ctrl_t *sc);
/* Before an edit from the UI or console: make the roster in memory the
 * current system's, so the edit lands on the system the radio is on.
 * Unscoped rosters on an unnamed system are edited as they are. */
bool p25_scan_roster_for_edit(p25_scan_ctrl_t *sc);

/* Follow trace ----------------------------------------------------------- */
void p25_scan_trace_grant(p25_scan_ctrl_t *sc, const p25_call_info_t *g,
                          p25_follow_outcome_t outcome,
                          p25_scan_reason_t reason, int64_t now_us);
/* Grants seen while scan/survey/split hold tuning. Once per TSDU batch. */
void p25_scan_trace_busy(p25_scan_ctrl_t *sc, const dsd_state *s,
                         int64_t now_us);
const char *p25_follow_outcome_name(p25_follow_outcome_t o);
const char *p25_scan_reason_name(p25_scan_reason_t r);
/* Short screen text for why the newest grant was left, or NULL. */
const char *p25_follow_left_label(const p25_follow_trace_t *t);

/* Decision --------------------------------------------------------------- */

/* Given a candidate grant and the follower's current state, decide what the
 * radio should do. This function has no side effects on the follower or the
 * controller state - the caller applies the decision, and does so by driving
 * the follower's own retune callback. That keeps a single owner of the tune
 * hook and keeps this function trivially testable. */
p25_scan_decision_t
p25_scan_decide(const p25_scan_ctrl_t *sc,
                const p25_grant_follower_t *f,
                uint16_t talkgroup, uint32_t source,
                uint64_t freq_hz, int64_t now_us);

/* Called after a decision has been applied, so counters can be updated in one
 * place. The controller only writes to counters here; state was applied by
 * the caller. */
void p25_scan_note_decision(p25_scan_ctrl_t *sc,
                            const p25_scan_decision_t *d);

/* Convenience: decide, then apply the decision to the follower. Releases the
 * current call first if a priority-preempt tune is called for. Returns the
 * decision that was made so a caller (or a bench test) can inspect it. */
p25_scan_decision_t
p25_scan_apply(p25_scan_ctrl_t *sc, p25_grant_follower_t *f,
               uint16_t talkgroup, uint32_t source,
               uint64_t freq_hz, int64_t now_us);

/* Convenience over p25_grant_from_state: pull the last TSBK grant out of the
 * decoder state and route it through p25_scan_apply. Returns true if the
 * follower moved (either to a new traffic channel or back to control). */
bool p25_scan_apply_from_state(p25_scan_ctrl_t *sc, p25_grant_follower_t *f,
                               const dsd_state *state, int64_t now_us);

/* Persistence ------------------------------------------------------------ */

/* Serialise the persistable fields (lockouts, allow list, list mode, hold)
 * into a versioned blob. Returns the number of bytes written. Bench uses
 * this to prove a round trip; firmware routes it through NVS. */
size_t p25_scan_persist_size(const p25_scan_ctrl_t *sc);
size_t p25_scan_persist_save(const p25_scan_ctrl_t *sc, uint8_t *buf,
                             size_t buf_len);
/* Returns true if the blob had a version this build understands. On a
 * version mismatch, sc is left initialised (empty lists) and no partial
 * apply happens - a corrupted or older blob does not silently mutate
 * behaviour. */
bool   p25_scan_persist_load(p25_scan_ctrl_t *sc, const uint8_t *buf,
                             size_t buf_len);
/* Same blob, but replaces only the roster fields and their scope: the
 * follow settings, counters and system identity are left alone. */
bool   p25_scan_persist_load_roster(p25_scan_ctrl_t *sc, const uint8_t *buf,
                                    size_t buf_len);

/* Firmware-only: NVS-backed reload of the global g_p25_scan singleton, and
 * write-through save. Defined in scan_ctrl_nvs.c; the bench does not link
 * that file so a host test that calls it would fail at link, which is what
 * we want - persistence policy belongs to the firmware. Every mutation from
 * the UI or the console should end in a save. */
void p25_scan_persist_reload(void);
void p25_scan_persist_save_now(void);

bool p25_scan_names_reload_from_sd(void);

#endif  /* P25_SCAN_CTRL_H */
