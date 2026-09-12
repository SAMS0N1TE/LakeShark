/* the PROGRAM session - staged reload and transactional apply. */

#ifndef P25_PROGRAM_H
#define P25_PROGRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "p25_profile.h"
#include "scan_ctrl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Long enough for "/sdcard/" plus an 8.3-plus name with room to spare, short
 * enough that the session stays small and the panel can show it whole. */
#define P25_PROGRAM_PATH_MAX     64U

/* One line per control channel: "> 16  1766.000000 MHz\n". */
#define P25_PROGRAM_CONTROL_TEXT_MAX (P25_PROFILE_CONTROL_MAX * 24U + 1U)

/* control-channel survey timing is deliberately visible and fixed.
 * A retune gets a settle window so buffered dibits from the outgoing channel
 * cannot vote for the incoming one; only evidence acquired during the dwell
 * window is scored.  Sixteen profile controls therefore put a hard upper
 * bound of 28 seconds on a survey. */
#define P25_SURVEY_SETTLE_MS 250U
#define P25_SURVEY_DWELL_MS  1500U

typedef enum {

    P25_PROGRAM_IDLE = 0,
    P25_PROGRAM_BUSY,     /* claimed; the worker is reading/parsing/applying */
    P25_PROGRAM_LOADED,   /* the active profile is the one named in source   */
    P25_PROGRAM_FAILED,   /* the last reload failed; the active one survives */
} p25_program_state_t;

typedef enum {
    P25_PROGRAM_OK = 0,
    P25_PROGRAM_ERR_ARGUMENT,
    P25_PROGRAM_ERR_PATH_TOO_LONG,
    P25_PROGRAM_ERR_BUSY,        /* a reload is already in flight            */
    P25_PROGRAM_ERR_NOT_CLAIMED, /* reload without a claim - a call-order bug */
    P25_PROGRAM_ERR_NOT_FOUND,
    P25_PROGRAM_ERR_TOO_LARGE,
    P25_PROGRAM_ERR_READ,
    P25_PROGRAM_ERR_EMPTY,
    P25_PROGRAM_ERR_PARSE,       /* diagnostic carries the line and reason   */
    P25_PROGRAM_ERR_NO_MEMORY,   /* staging or the worker could not be made  */
} p25_program_result_t;

typedef enum {
    P25_SURVEY_IDLE = 0,
    P25_SURVEY_SETTLING,
    P25_SURVEY_DWELLING,
    P25_SURVEY_FOUND,
    P25_SURVEY_NO_CONTROL,
    P25_SURVEY_CANCELED,
} p25_survey_state_t;

typedef enum {
    P25_SURVEY_CANCEL_NONE = 0,
    P25_SURVEY_CANCEL_OPERATOR,
    P25_SURVEY_CANCEL_MANUAL_TUNE,
    P25_SURVEY_CANCEL_APP_EXIT,
    P25_SURVEY_CANCEL_PROFILE_CHANGE,
    P25_SURVEY_CANCEL_FOLLOWING_CALL,
} p25_survey_cancel_t;

typedef struct {
    p25_survey_state_t  state;
    p25_survey_cancel_t cancel_reason;
    uint8_t previous_control;
    uint8_t candidate_offset;
    uint8_t candidate_index;
    uint8_t candidates_checked;
    uint8_t best_index;
    uint32_t deadline_ms;
    uint32_t nid_baseline;
    uint32_t tsbk_baseline;
    uint32_t best_nids;
    uint32_t best_tsbks;
    uint32_t selected_nids;
    uint32_t selected_tsbks;
    bool     counter_loss;
} p25_control_survey_t;

typedef p25_program_result_t (*p25_program_read_fn)(void *ctx, const char *path,
                                                    char *dst, size_t cap,
                                                    size_t *out_len);

typedef struct {
    void (*release)(void *user);
    void (*set_auto_follow)(void *user, bool enabled);
    void (*set_encrypted_policy)(void *user, bool skip_enabled, uint32_t skip_ms);
    void (*set_cqpsk_loops)(void *user, const p25_cqpsk_config_t *config);
    void (*set_demod_preference)(void *user, int preference);
    void (*set_roster)(void *user, const p25_profile_talkgroup_t *talkgroups,
                       size_t count);
    /* app handoff reloads the profile's runtime priority ranks but
     * must not replace allow/hold policy just restored from NVS.  Keeping a
     * distinct op makes that difference explicit at the decoder boundary. */
    void (*restore_roster)(void *user,
                           const p25_profile_talkgroup_t *talkgroups,
                           size_t count);
    void (*set_control)(void *user, uint64_t control_hz);
    void *user;
} p25_program_ops_t;

/* Caller-owned staging.  Never a local; on the device this is one PSRAM
 * allocation held only for the duration of a reload. */
typedef struct {
    char                        text[P25_PROFILE_MAX_FILE_BYTES];
    p25_profile_parse_scratch_t parse;
    /* The parser's destination.  Separate from the session's active profile
     * on purpose: the ops run against this, and only a complete apply
     * promotes it.  It is also separate from parse.candidate, which the
     * parser refuses to alias. */
    p25_profile_t               candidate;
} p25_program_staging_t;

typedef struct {
    /* Active state - what the decoder is actually running. */
    p25_profile_t active;
    bool          active_valid;
    char          active_path[P25_PROGRAM_PATH_MAX];
    uint8_t       selected_control;   /* index into active.control_channels  */

    /* Report of the last reload attempt, whatever it did.  last_diagnostic
     * carries the parser's line and reason only when last_result is
     * ERR_PARSE; for every other failure its line is zero, its code is
     * P25_PROFILE_ERROR_NONE and its reason mirrors last_result. */
    p25_program_state_t      state;
    p25_program_result_t     last_result;
    p25_profile_diagnostic_t last_diagnostic;
    char                     last_path[P25_PROGRAM_PATH_MAX];
    size_t                   last_bytes;

    uint32_t loads_ok;
    uint32_t loads_failed;
    uint32_t applies;    /* includes reapply and control selection           */

    /* Fixed-size and held with the PROGRAM session in PSRAM.  No survey
     * transition allocates, blocks, or stores frequencies outside active. */
    p25_control_survey_t survey;
} p25_program_t;

void p25_program_init(p25_program_t *program);

bool p25_program_claim(p25_program_t *program, const char *path);

/* Perform the claimed reload.  Runs on the worker.  On any failure the active
 * profile, the selected control and every decoder setting are untouched and no
 * op is called.  On success the ops run in vtable order and the candidate is
 * then committed. */
p25_program_result_t p25_program_reload(p25_program_t *program,
                                        p25_program_read_fn read, void *read_ctx,
                                        p25_program_staging_t *staging,
                                        const p25_profile_parse_config_t *config,
                                        const p25_program_ops_t *ops);

/* Give up a claimed reload that could not even be attempted - no memory for
 * the staging area, or the worker could not be started.  Releases the busy
 * latch, records the reason and leaves the active profile alone.  Without it
 * the panel would sit on LOADING forever. */
p25_program_result_t p25_program_abandon(p25_program_t *program,
                                         p25_program_result_t result);

/* Re-issue the active profile to a decoder that has just been (re)started -
 * app handoff.  No-op without an active profile.  Does not re-read the file:
 * what is on SD may have changed, and re-entering an app is not a reload. */
bool p25_program_reapply(p25_program_t *program, const p25_program_ops_t *ops);

/* Move the selected control channel within the active profile's list.  Returns
 * false when there is no profile, no movement is possible, or a reload is in
 * flight.  Issues release() + set_control() only. */
bool p25_program_select_control(p25_program_t *program, uint8_t index,
                                const p25_program_ops_t *ops);
bool p25_program_step_control(p25_program_t *program, int delta,
                              const p25_program_ops_t *ops);

/* Survey exactly active.control_channels, beginning with the currently
 * selected entry.  Counters are cumulative protocol-valid NID (BCH accepted)
 * and TSBK (CRC accepted) counts from the decoder.  poll() handles at most
 * one transition/tune per call and is safe across uint32_t clock wrap.
 * Candidate frequencies are never invented or appended to the profile. */
bool p25_program_survey_start(p25_program_t *program, uint32_t now_ms,
                              uint32_t valid_nids, uint32_t valid_tsbks,
                              const p25_program_ops_t *ops);
bool p25_program_survey_poll(p25_program_t *program, uint32_t now_ms,
                             uint32_t valid_nids, uint32_t valid_tsbks,
                             const p25_program_ops_t *ops);
bool p25_program_survey_cancel(p25_program_t *program,
                               p25_survey_cancel_t reason,
                               const p25_program_ops_t *ops);
bool p25_program_survey_active(const p25_program_t *program);

uint64_t p25_program_selected_control_hz(const p25_program_t *program);

const char *p25_program_result_reason(p25_program_result_t result);
const char *p25_program_state_name(const p25_program_t *program);

void p25_program_format_status(const p25_program_t *program,
                               char *out, size_t out_size);
void p25_program_format_source(const p25_program_t *program,
                               char *out, size_t out_size);
void p25_program_format_selected(const p25_program_t *program,
                                 char *out, size_t out_size);
void p25_program_format_controls(const p25_program_t *program,
                                 char *out, size_t out_size);
void p25_program_format_roster(const p25_program_t *program,
                               char *out, size_t out_size);
void p25_program_format_survey(const p25_program_t *program,
                               char *out, size_t out_size);

void p25_program_apply_roster(p25_scan_ctrl_t *scan,
                              const p25_profile_talkgroup_t *talkgroups,
                              size_t count);

/* Reapply only runtime roster state after an app handoff.  Persisted
 * allow-list membership, mode, hold and lockouts remain exactly as restored;
 * profile priority ranks are reconstructed because scan_ctrl intentionally
 * does not persist them. */
void p25_program_restore_roster(p25_scan_ctrl_t *scan,
                                const p25_profile_talkgroup_t *talkgroups,
                                size_t count);

/* ------------------------------------------------------- firmware-only ---
 * Defined in p25_program_sd.c, which the bench does not link: a host test
 * that called these would fail at link, which is what we want.  The session
 * lives in PSRAM and is created on first use; p25_program_session() returns
 * NULL until then, and every format_ function above renders NULL as the
 * first-run empty state. */

#define P25_PROGRAM_DEFAULT_PATH "/sdcard/p25_profile.txt"

const p25_program_t *p25_program_session(void);

/* Ask for a reload of P25_PROGRAM_DEFAULT_PATH.  Returns as soon as the
 * request is accepted: the read, the parse and the apply all happen on a
 * worker, so this never blocks the LVGL task and never puts a 16 KiB staging
 * buffer on its stack.  false means the request was refused - a reload is
 * already running, or there was no memory for the session.  Poll
 * p25_program_session() for what actually happened. */
bool p25_program_request_reload(void);

/* Step the selected control channel of the active profile, and re-issue the
 * active profile after the decoder restarts.  Both run in the caller's task:
 * they only touch the follower and the tune latch. */
bool p25_program_step_control_now(int delta);
bool p25_program_reapply_now(void);
bool p25_program_survey_start_now(void);
bool p25_program_survey_poll_now(uint32_t now_ms, uint32_t valid_nids,
                                 uint32_t valid_tsbks);
bool p25_program_survey_cancel_now(p25_survey_cancel_t reason);
bool p25_program_survey_active_now(void);

#ifdef __cplusplus
}
#endif

#endif /* P25_PROGRAM_H */
