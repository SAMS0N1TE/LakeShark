#ifndef P25_GRANT_FOLLOWER_H
#define P25_GRANT_FOLLOWER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd.h"
#include "p25_controls.h"

typedef enum {
    P25_GRANT_ON_CONTROL = 0,
    P25_GRANT_ON_TRAFFIC = 1,
} p25_grant_state_t;

typedef enum {
    P25_GRANT_FILTER_OFF = 0,
    P25_GRANT_FILTER_ALLOW = 1,
    P25_GRANT_FILTER_DENY = 2,
} p25_grant_filter_mode_t;

typedef enum {
    P25_RX_CONTROL_SEARCH = 0,
    P25_RX_CONTROL_LOCKED,
    P25_RX_TRAFFIC_WAIT,
    P25_RX_TRAFFIC_SYNC,
    P25_RX_AUDIO,
    P25_RX_ENCRYPTED,
    P25_RX_TUNE_FAILED,
} p25_receive_state_t;

#define P25_GRANT_FILTER_MAX 16

/* per-talkgroup state. On first LDU2 the follower records last-seen
 * ALGID / KID; if the ALGID is not CLEAR (0x80) the entry is stamped with an
 * expiry and the follower returns to the control channel. A subsequent grant
 * for the same TG within the expiry window is skipped without retune. Bounded
 * table: when full, the oldest-expiring entry is evicted so a busy system
 * cannot make it grow. Each entry is 16 bytes, so P25_GRANT_TG_STATE_MAX=32
 * costs 512 B of state - internal RAM, sits inside p25_grant_follower_t. */
#define P25_GRANT_TG_STATE_MAX 32

typedef struct {
    uint16_t talkgroup;
    uint8_t  algid;
    uint16_t kid;
    int64_t  last_seen_us;
    int64_t  skip_expires_us; /* 0 = no active skip */
} p25_grant_tg_state_t;

/* Retune callback. to_traffic=true when following a grant; false when
 * returning to the control channel. */
typedef void (*p25_grant_retune_fn)(void *user, uint64_t center_hz,
                                    bool to_traffic);

typedef struct {
    uint64_t control_hz;
    p25_grant_retune_fn retune;
    void *user;

    p25_grant_state_t state;
    uint64_t traffic_hz;
    uint16_t talkgroup;
    uint32_t source;
    p25_call_info_t active_call;
    p25_call_info_t observed_grant;
    p25_receive_state_t receive_state;
    unsigned int unsupported_grants;
    unsigned int unresolved_grants;
    unsigned int control_relocks;
    uint32_t consumed_grant_generation;
    bool consumed_grant_valid;
    uint32_t system_wacn;
    uint16_t system_sysid;
    bool system_valid;
    uint16_t control_nac;
    bool control_nac_valid;

    int64_t last_activity_us;
    int64_t hang_us;

    p25_grant_filter_mode_t filter_mode;
    uint16_t filter[P25_GRANT_FILTER_MAX];
    uint8_t filter_count;

    bool     leave_on_encrypted;
    int64_t  encrypted_skip_us;

    /* bounded per-TG history. Used for skip-window enforcement and for
     * the "last ALGID/KID seen on this TG" readout. */
    p25_grant_tg_state_t tg_state[P25_GRANT_TG_STATE_MAX];
    uint8_t  tg_state_count;
    unsigned int tg_state_evictions;

    unsigned int followed_count;
    unsigned int returned_count;
    unsigned int duplicate_grants;
    unsigned int foreign_grants;
    unsigned int filtered_grants;
    /* */
    unsigned int encrypted_returns;    /* returned to control on encrypted ESS */
    unsigned int encrypted_skips;      /* grants refused because skip is active */
} p25_grant_follower_t;

void p25_grant_init(p25_grant_follower_t *f,
                    uint64_t control_hz,
                    p25_grant_retune_fn retune,
                    void *user);
void p25_grant_set_control(p25_grant_follower_t *f, uint64_t control_hz);
void p25_grant_set_hang_ms(p25_grant_follower_t *f, unsigned int ms);
void p25_grant_set_filter(p25_grant_follower_t *f,
                          p25_grant_filter_mode_t mode,
                          const uint16_t *talkgroups, size_t count);

/* encrypted-channel policy. Defaults: leave_on_encrypted=true,
 * skip_ms=30000. Rationale for 30 s: a typical dispatch exchange is 10-20 s
 * and the next grant on the same TG is very likely part of the same
 * conversation - re-following just to leave again wastes control-channel
 * time. 30 s clears the tail without silencing a talkgroup for minutes. */
#define P25_GRANT_DEFAULT_ENCRYPTED_SKIP_MS P25_CONTROL_ENCRYPTED_SKIP_DEFAULT_MS
void p25_grant_set_leave_on_encrypted(p25_grant_follower_t *f, bool enabled);
void p25_grant_set_encrypted_skip_ms(p25_grant_follower_t *f, unsigned int ms);

/* Feed a decoded grant. Returns true when a retune to the traffic channel
 * was issued. A grant for the call already being followed refreshes the hang
 * timer but does not retune; a grant for a different talkgroup while on
 * traffic is ignored so the scanner does not thrash. A grant for a talkgroup
 * whose encrypted-skip is still active is refused and counted. */
bool p25_grant_on_grant(p25_grant_follower_t *f,
                        uint16_t talkgroup, uint32_t source,
                        uint64_t freq_hz, int64_t now_us);

/* Voice-frame activity on the traffic channel. Resets the silence timer. */
void p25_grant_on_voice(p25_grant_follower_t *f, int64_t now_us);

/* TDU / TDULC terminator: end the call and return to control. Returns true
 * if a retune to control was issued. */
bool p25_grant_on_terminator(p25_grant_follower_t *f, int64_t now_us);

/* force a return to control from outside the follower - used by the
 * scan controller when a priority preempt or a lockout demands that the
 * radio drop the current call. Same code path as the TDU terminator; the
 * retune callback is the sole writer of the tune. */
bool p25_grant_force_return_to_control(p25_grant_follower_t *f);

/* Called periodically. If the silence hang timer has expired, return to
 * control. Returns true if a retune to control was issued. */
bool p25_grant_tick(p25_grant_follower_t *f, int64_t now_us);

/* Convenience: after a successful p25_tsbk_parse(), inspect the decoded
 * fields and feed the follower if the opcode was a grant. */
bool p25_grant_from_state(p25_grant_follower_t *f, const dsd_state *state,
                          int64_t now_us);

/* Typed path used by the production scanner. Observations are never audio. */
bool p25_grant_on_call(p25_grant_follower_t *f, const p25_call_info_t *call,
                       int64_t now_us);
bool p25_grant_take_batch(p25_grant_follower_t *f, const dsd_state *state);
void p25_grant_observe(p25_grant_follower_t *f, const p25_call_info_t *call);
void p25_grant_on_frame(p25_grant_follower_t *f, const dsd_state *state,
                        int64_t now_us);

/* ESS from the just-decoded LDU2. Records the algorithm and KID
 * against the currently-followed TG, and - if leave_on_encrypted is set and
 * the ALGID is not CLEAR - stamps a skip and hands control back to the CC
 * follower. Returns true if a return-to-control retune was issued. */
bool p25_grant_on_ess(p25_grant_follower_t *f, uint16_t talkgroup,
                      uint8_t algid, uint16_t kid, int64_t now_us);

/* readout helpers used by the console command and the P25 screen. */
const p25_grant_tg_state_t *p25_grant_tg_state(const p25_grant_follower_t *f,
                                               size_t index);
size_t p25_grant_tg_state_count(const p25_grant_follower_t *f);
bool   p25_grant_tg_is_skipped(const p25_grant_follower_t *f,
                               uint16_t talkgroup, int64_t now_us);

#endif
