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

typedef struct {
    uint16_t number;
    char     name[P25_SCAN_NAME_LEN];
    char     category[P25_SCAN_CATEGORY_LEN];
} p25_scan_name_t;

typedef struct {

    bool auto_follow;

    /* Persisted --------------------------------------------------------- */
    uint16_t lockout[P25_SCAN_LOCKOUT_MAX];
    uint16_t lockout_count;

    uint16_t allow[P25_SCAN_ALLOW_MAX];
    uint16_t allow_count;
    p25_scan_list_mode_t list_mode;

    uint16_t hold_tg;         /* 0 = no hold                                  */

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

/* Firmware-only: NVS-backed reload of the global g_p25_scan singleton, and
 * write-through save. Defined in scan_ctrl_nvs.c; the bench does not link
 * that file so a host test that calls it would fail at link, which is what
 * we want - persistence policy belongs to the firmware. Every mutation from
 * the UI or the console should end in a save. */
void p25_scan_persist_reload(void);
void p25_scan_persist_save_now(void);

bool p25_scan_names_reload_from_sd(void);

#endif  /* P25_SCAN_CTRL_H */
