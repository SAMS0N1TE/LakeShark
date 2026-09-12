/* see p25_program.h.  No allocation, no globals, no IDF. */

#include "p25_program.h"

#include <stdio.h>
#include <string.h>

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    size_t i = 0;
    if (src) {
        for (; src[i] != '\0' && i + 1U < cap; ++i) dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void append(char *out, size_t cap, size_t *used, const char *text)
{
    if (!out || cap == 0 || !text) return;
    while (*text != '\0' && *used + 1U < cap) out[(*used)++] = *text++;
    out[*used] = '\0';
}

/* Integer hertz, never a float: 851.012500 must render exactly, and the P25
 * channel grid is 6.25 kHz, so the sixth decimal is load-bearing. */
static void format_hz(char *out, size_t cap, uint64_t hz)
{
    char line[64];
    snprintf(line, sizeof(line), "%lu.%06lu MHz",
             (unsigned long)(hz / 1000000ULL),
             (unsigned long)(hz % 1000000ULL));
    copy_str(out, cap, line);
}

static void append_uint(char *out, size_t cap, size_t *used, unsigned long v)
{
    char digits[24];
    snprintf(digits, sizeof(digits), "%lu", v);
    append(out, cap, used, digits);
}

const char *p25_program_result_reason(p25_program_result_t result)
{
    switch (result) {
    case P25_PROGRAM_OK:                return "ok";
    case P25_PROGRAM_ERR_ARGUMENT:      return "internal: bad reload argument";
    case P25_PROGRAM_ERR_PATH_TOO_LONG: return "profile path is too long";
    case P25_PROGRAM_ERR_BUSY:          return "a reload is already running";
    case P25_PROGRAM_ERR_NOT_CLAIMED:   return "internal: reload without claim";
    case P25_PROGRAM_ERR_NOT_FOUND:     return "no profile file at that path";
    case P25_PROGRAM_ERR_TOO_LARGE:     return "profile file is too large";
    case P25_PROGRAM_ERR_READ:          return "profile file could not be read";
    case P25_PROGRAM_ERR_EMPTY:         return "profile file is empty";
    case P25_PROGRAM_ERR_PARSE:         return "profile is not valid";
    case P25_PROGRAM_ERR_NO_MEMORY:     return "no memory to stage the profile";
    default:                            return "unknown program error";
    }
}

const char *p25_program_state_name(const p25_program_t *program)
{
    if (!program) return "IDLE";
    switch (program->state) {
    case P25_PROGRAM_BUSY:   return "LOADING";
    case P25_PROGRAM_LOADED: return "LOADED";
    case P25_PROGRAM_FAILED: return "FAILED";
    case P25_PROGRAM_IDLE:
    default:                 return "IDLE";
    }
}

void p25_program_init(p25_program_t *program)
{
    if (!program) return;
    memset(program, 0, sizeof(*program));
    program->state       = P25_PROGRAM_IDLE;
    program->last_result = P25_PROGRAM_OK;
    program->last_diagnostic.line   = 0;
    program->last_diagnostic.code   = P25_PROFILE_ERROR_NONE;
    program->last_diagnostic.reason = p25_profile_error_reason(P25_PROFILE_ERROR_NONE);
}

/* Every failure exit runs through here, so the busy latch is always released
 * and the panel can never be left saying LOADING after a worker gave up. */
static p25_program_result_t fail(p25_program_t *program,
                                 p25_program_result_t result)
{
    program->last_result = result;
    if (result != P25_PROGRAM_ERR_PARSE) {
        program->last_diagnostic.line   = 0;
        program->last_diagnostic.code   = P25_PROFILE_ERROR_NONE;
        program->last_diagnostic.reason = p25_program_result_reason(result);
    }
    program->last_bytes = 0;
    program->loads_failed++;
    program->state = P25_PROGRAM_FAILED;
    return result;
}

/* Fixed order, and the only place decoder state is touched.  release() first
 * so the radio is off any traffic channel belonging to the outgoing system;
 * set_control() last so exactly one tune survives in the single-slot tune
 * latch and it is the one for the incoming system.
 *
 * Nothing here touches the vocoder's encryption mute.  encrypted_skip steers
 * the grant follower only - a profile cannot ask the radio to play encrypted
 * or unknown-ESS audio, and p25_ldu_should_mute_encrypted keeps deciding
 * that on its own. */
static void apply_profile(const p25_profile_t *profile, uint64_t control_hz,
                          const p25_program_ops_t *ops, bool restore)
{
    if (ops->release) ops->release(ops->user);
    if (ops->set_auto_follow)
        ops->set_auto_follow(ops->user, profile->auto_follow);
    if (ops->set_encrypted_policy)
        ops->set_encrypted_policy(ops->user, profile->encrypted_skip_enabled,
                                  profile->encrypted_skip_ms);
    if (ops->set_cqpsk_loops)
        ops->set_cqpsk_loops(ops->user, &profile->cqpsk);
    if (ops->set_demod_preference)
        ops->set_demod_preference(ops->user, profile->demod_preference);
    if (restore && ops->restore_roster)
        ops->restore_roster(ops->user, profile->talkgroups,
                            profile->talkgroup_count);
    else if (!restore && ops->set_roster)
        ops->set_roster(ops->user, profile->talkgroups,
                        profile->talkgroup_count);
    if (ops->set_control) ops->set_control(ops->user, control_hz);
}

static uint8_t preferred_index(const p25_profile_t *profile)
{
    for (uint8_t i = 0; i < profile->control_count; ++i) {
        if (profile->control_channels[i] == profile->preferred_control_hz)
            return i;
    }
    return 0;
}

static bool survey_running(const p25_control_survey_t *survey)
{
    return survey && (survey->state == P25_SURVEY_SETTLING ||
                      survey->state == P25_SURVEY_DWELLING);
}

bool p25_program_survey_active(const p25_program_t *program)
{
    return program && survey_running(&program->survey);
}

static bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint8_t survey_index(const p25_program_t *program, uint8_t offset)
{
    return (uint8_t)((program->survey.previous_control + offset) %
                     program->active.control_count);
}

bool p25_program_survey_cancel(p25_program_t *program,
                               p25_survey_cancel_t reason,
                               const p25_program_ops_t *ops)
{
    if (!program || !ops || !survey_running(&program->survey)) return false;

    /* Do not release here.  FOLLOWING_CALL arrives from inside the follower's
     * retune callback; recursively forcing a return there would leave a stale
     * traffic tune in the single-slot latch.  Restoring the prior profile
     * control first is sufficient: the explicit owner action (manual tune,
     * profile apply, app stop, or traffic follow) is issued after this. */
    program->survey.state = P25_SURVEY_CANCELED;
    program->survey.cancel_reason = reason;
    program->selected_control = program->survey.previous_control;
    if (ops->set_control)
        ops->set_control(ops->user,
                         program->active.control_channels[
                             program->survey.previous_control]);
    return true;
}

bool p25_program_survey_start(p25_program_t *program, uint32_t now_ms,
                              uint32_t valid_nids, uint32_t valid_tsbks,
                              const p25_program_ops_t *ops)
{
    if (!program || !ops || program->state == P25_PROGRAM_BUSY ||
        !program->active_valid || program->active.control_count == 0 ||
        survey_running(&program->survey))
        return false;

    p25_control_survey_t *survey = &program->survey;
    memset(survey, 0, sizeof(*survey));
    survey->state = P25_SURVEY_SETTLING;
    survey->previous_control = program->selected_control <
                                       program->active.control_count
                                   ? program->selected_control : 0;
    survey->candidate_index = survey->previous_control;
    survey->best_index = survey->previous_control;
    survey->deadline_ms = now_ms + P25_SURVEY_SETTLE_MS;
    survey->nid_baseline = valid_nids;
    survey->tsbk_baseline = valid_tsbks;

    /* Survey owns tuning until it terminates.  Release a traffic call once,
     * then use only frequencies from the immutable active profile list. */
    if (ops->release) ops->release(ops->user);
    if (ops->set_control)
        ops->set_control(ops->user,
                         program->active.control_channels[
                             survey->candidate_index]);
    return true;
}

bool p25_program_survey_poll(p25_program_t *program, uint32_t now_ms,
                             uint32_t valid_nids, uint32_t valid_tsbks,
                             const p25_program_ops_t *ops)
{
    if (!program || !ops || !survey_running(&program->survey)) return false;
    p25_control_survey_t *survey = &program->survey;
    if (!deadline_reached(now_ms, survey->deadline_ms)) return false;

    if (survey->state == P25_SURVEY_SETTLING) {
        /* Discard every decode observed during settling; it may have been in
         * the USB/ring pipeline before the retune completed. */
        survey->nid_baseline = valid_nids;
        survey->tsbk_baseline = valid_tsbks;
        survey->counter_loss = false;
        survey->state = P25_SURVEY_DWELLING;
        survey->deadline_ms = now_ms + P25_SURVEY_DWELL_MS;
        return true;
    }

    uint32_t nids = 0, tsbks = 0;
    if (valid_nids < survey->nid_baseline ||
        valid_tsbks < survey->tsbk_baseline) {

        survey->counter_loss = true;
    } else {
        nids = valid_nids - survey->nid_baseline;
        tsbks = valid_tsbks - survey->tsbk_baseline;
    }

    if (!survey->counter_loss && (nids > 0 || tsbks > 0) &&
        (tsbks > survey->best_tsbks ||
         (tsbks == survey->best_tsbks && nids > survey->best_nids))) {
        survey->best_index = survey->candidate_index;
        survey->best_nids = nids;
        survey->best_tsbks = tsbks;
    }

    survey->candidates_checked++;
    if (survey->candidates_checked < program->active.control_count) {
        survey->candidate_offset++;
        survey->candidate_index = survey_index(program,
                                                survey->candidate_offset);
        survey->state = P25_SURVEY_SETTLING;
        survey->deadline_ms = now_ms + P25_SURVEY_SETTLE_MS;
        survey->nid_baseline = valid_nids;
        survey->tsbk_baseline = valid_tsbks;
        survey->counter_loss = false;
        if (ops->set_control)
            ops->set_control(ops->user,
                             program->active.control_channels[
                                 survey->candidate_index]);
        return true;
    }

    if (survey->best_nids > 0 || survey->best_tsbks > 0) {
        survey->state = P25_SURVEY_FOUND;
        survey->selected_nids = survey->best_nids;
        survey->selected_tsbks = survey->best_tsbks;
        program->selected_control = survey->best_index;
    } else {
        survey->state = P25_SURVEY_NO_CONTROL;
        program->selected_control = survey->previous_control;
    }
    if (ops->set_control)
        ops->set_control(ops->user,
                         program->active.control_channels[
                             program->selected_control]);
    program->applies++;
    return true;
}

bool p25_program_claim(p25_program_t *program, const char *path)
{
    if (!program || !path || path[0] == '\0') return false;
    if (program->state == P25_PROGRAM_BUSY) return false;
    if (strlen(path) + 1U > sizeof(program->last_path)) {
        program->last_path[0] = '\0';
        (void)fail(program, P25_PROGRAM_ERR_PATH_TOO_LONG);
        return false;
    }
    copy_str(program->last_path, sizeof(program->last_path), path);
    program->state = P25_PROGRAM_BUSY;
    return true;
}

p25_program_result_t p25_program_reload(p25_program_t *program,
                                        p25_program_read_fn read, void *read_ctx,
                                        p25_program_staging_t *staging,
                                        const p25_profile_parse_config_t *config,
                                        const p25_program_ops_t *ops)
{
    if (!program) return P25_PROGRAM_ERR_ARGUMENT;
    if (program->state != P25_PROGRAM_BUSY) return P25_PROGRAM_ERR_NOT_CLAIMED;
    if (!read || !staging || !config || !ops)
        return fail(program, P25_PROGRAM_ERR_ARGUMENT);

    size_t length = 0;
    p25_program_result_t result = read(read_ctx, program->last_path,
                                       staging->text, sizeof(staging->text),
                                       &length);
    if (result == P25_PROGRAM_OK && length > sizeof(staging->text))
        result = P25_PROGRAM_ERR_READ;
    if (result == P25_PROGRAM_OK && length == 0)
        result = P25_PROGRAM_ERR_EMPTY;
    if (result != P25_PROGRAM_OK) return fail(program, result);

    p25_profile_diagnostic_t diagnostic;
    if (!p25_profile_parse(&staging->candidate, &staging->parse, staging->text,
                           length, config, &diagnostic)) {
        program->last_diagnostic = diagnostic;
        return fail(program, P25_PROGRAM_ERR_PARSE);
    }

    /* Validated in full.  Only now may anything change. */
    (void)p25_program_survey_cancel(program,
                                    P25_SURVEY_CANCEL_PROFILE_CHANGE, ops);
    uint8_t index = preferred_index(&staging->candidate);
    apply_profile(&staging->candidate,
                  staging->candidate.control_channels[index], ops, false);

    program->active            = staging->candidate;
    program->active_valid      = true;
    program->selected_control  = index;
    copy_str(program->active_path, sizeof(program->active_path),
             program->last_path);
    program->last_bytes      = length;
    program->last_diagnostic = diagnostic;
    program->last_result     = P25_PROGRAM_OK;
    program->state           = P25_PROGRAM_LOADED;
    program->loads_ok++;
    program->applies++;
    return P25_PROGRAM_OK;
}

p25_program_result_t p25_program_abandon(p25_program_t *program,
                                         p25_program_result_t result)
{
    if (!program) return P25_PROGRAM_ERR_ARGUMENT;
    if (program->state != P25_PROGRAM_BUSY) return P25_PROGRAM_ERR_NOT_CLAIMED;
    if (result == P25_PROGRAM_OK) result = P25_PROGRAM_ERR_ARGUMENT;
    return fail(program, result);
}

bool p25_program_reapply(p25_program_t *program, const p25_program_ops_t *ops)
{
    if (!program || !ops) return false;
    if (program->state == P25_PROGRAM_BUSY) return false;
    if (!program->active_valid || program->active.control_count == 0) return false;
    (void)p25_program_survey_cancel(program, P25_SURVEY_CANCEL_APP_EXIT, ops);
    if (program->selected_control >= program->active.control_count)
        program->selected_control = 0;
    apply_profile(&program->active,
                  program->active.control_channels[program->selected_control],
                  ops, true);
    program->applies++;
    return true;
}

bool p25_program_select_control(p25_program_t *program, uint8_t index,
                                const p25_program_ops_t *ops)
{
    if (!program || !ops) return false;
    if (program->state == P25_PROGRAM_BUSY) return false;
    if (!program->active_valid || index >= program->active.control_count)
        return false;

    (void)p25_program_survey_cancel(program,
                                    P25_SURVEY_CANCEL_MANUAL_TUNE, ops);

    program->selected_control = index;
    /* A manual control change is the same transaction as a profile change,
     * minus the policy: drop any call in progress on the outgoing control
     * channel first, then issue the one tune. */
    if (ops->release) ops->release(ops->user);
    if (ops->set_control)
        ops->set_control(ops->user, program->active.control_channels[index]);
    program->applies++;
    return true;
}

bool p25_program_step_control(p25_program_t *program, int delta,
                              const p25_program_ops_t *ops)
{
    if (!program || !ops) return false;
    if (program->state == P25_PROGRAM_BUSY) return false;
    if (!program->active_valid) return false;

    int count = (int)program->active.control_count;
    if (count <= 1 || delta == 0) return false;

    int next = (int)program->selected_control + delta;
    next %= count;
    if (next < 0) next += count;
    return p25_program_select_control(program, (uint8_t)next, ops);
}

uint64_t p25_program_selected_control_hz(const p25_program_t *program)
{
    if (!program || !program->active_valid) return 0;
    if (program->selected_control >= program->active.control_count) return 0;
    return program->active.control_channels[program->selected_control];
}

void p25_program_format_status(const p25_program_t *program,
                               char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';

    /* A session that has not been created yet reads as first run, not as a
     * blank line: the panel must always say something. */
    size_t used = 0;
    if (!program) {
        append(out, out_size, &used, "NO PROFILE LOADED");
        return;
    }

    switch (program->state) {
    case P25_PROGRAM_BUSY:
        append(out, out_size, &used, "LOADING");
        return;
    case P25_PROGRAM_IDLE:
        append(out, out_size, &used, "NO PROFILE LOADED");
        return;
    case P25_PROGRAM_LOADED:
        append(out, out_size, &used, "OK  ");
        append_uint(out, out_size, &used, (unsigned long)program->last_bytes);
        append(out, out_size, &used, " bytes");
        return;
    case P25_PROGRAM_FAILED:
    default:
        break;
    }

    if (program->last_result == P25_PROGRAM_ERR_PARSE &&
        program->last_diagnostic.line > 0) {
        append(out, out_size, &used, "line ");
        append_uint(out, out_size, &used,
                    (unsigned long)program->last_diagnostic.line);
        append(out, out_size, &used, ": ");
        append(out, out_size, &used, program->last_diagnostic.reason);
    } else if (program->last_result == P25_PROGRAM_ERR_PARSE) {
        append(out, out_size, &used, program->last_diagnostic.reason);
    } else {
        append(out, out_size, &used,
               p25_program_result_reason(program->last_result));
    }

    append(out, out_size, &used, program->active_valid
                                     ? "  (active profile kept)"
                                     : "  (no active profile)");
}

void p25_program_format_source(const p25_program_t *program,
                               char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    if (!program || !program->active_valid || program->active_path[0] == '\0') {
        copy_str(out, out_size, "(none)");
        return;
    }
    copy_str(out, out_size, program->active_path);
}

void p25_program_format_selected(const p25_program_t *program,
                                 char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!program || !program->active_valid ||
        program->active.control_count == 0) {
        copy_str(out, out_size, "-");
        return;
    }

    size_t used = 0;
    append_uint(out, out_size, &used,
                (unsigned long)program->selected_control + 1UL);
    append(out, out_size, &used, "/");
    append_uint(out, out_size, &used,
                (unsigned long)program->active.control_count);
    append(out, out_size, &used, "  ");

    char hz[32];
    format_hz(hz, sizeof(hz), p25_program_selected_control_hz(program));
    append(out, out_size, &used, hz);
}

void p25_program_format_controls(const p25_program_t *program,
                                 char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!program || !program->active_valid ||
        program->active.control_count == 0) {
        copy_str(out, out_size, "(no profile loaded)");
        return;
    }

    size_t used = 0;
    for (uint8_t i = 0; i < program->active.control_count; ++i) {
        if (i > 0) append(out, out_size, &used, "\n");
        append(out, out_size, &used,
               i == program->selected_control ? "> " : "  ");
        append_uint(out, out_size, &used, (unsigned long)i + 1UL);
        append(out, out_size, &used, "  ");
        char hz[32];
        format_hz(hz, sizeof(hz), program->active.control_channels[i]);
        append(out, out_size, &used, hz);
    }
}

void p25_program_format_roster(const p25_program_t *program,
                               char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!program || !program->active_valid) {
        copy_str(out, out_size, "-");
        return;
    }

    unsigned enabled = 0, priority = 0;
    for (uint8_t i = 0; i < program->active.talkgroup_count; ++i) {
        if (program->active.talkgroups[i].enabled) enabled++;
        if (program->active.talkgroups[i].priority > 0) priority++;
    }

    size_t used = 0;
    append_uint(out, out_size, &used,
                (unsigned long)program->active.talkgroup_count);
    append(out, out_size, &used, " TG  ");
    append_uint(out, out_size, &used, enabled);
    append(out, out_size, &used, " allowed  ");
    append_uint(out, out_size, &used, priority);
    append(out, out_size, &used, " priority");
}

static const char *survey_cancel_name(p25_survey_cancel_t reason)
{
    switch (reason) {
    case P25_SURVEY_CANCEL_OPERATOR:       return "OPERATOR";
    case P25_SURVEY_CANCEL_MANUAL_TUNE:    return "MANUAL TUNE";
    case P25_SURVEY_CANCEL_APP_EXIT:       return "APP EXIT";
    case P25_SURVEY_CANCEL_PROFILE_CHANGE: return "PROFILE CHANGE";
    case P25_SURVEY_CANCEL_FOLLOWING_CALL: return "FOLLOWING CALL";
    case P25_SURVEY_CANCEL_NONE:
    default:                               return "UNKNOWN";
    }
}

void p25_program_format_survey(const p25_program_t *program,
                               char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!program || !program->active_valid) {
        copy_str(out, out_size, "NO PROFILE / NO CONTROL");
        return;
    }

    const p25_control_survey_t *survey = &program->survey;
    size_t used = 0;
    if (survey->state == P25_SURVEY_IDLE) {
        append(out, out_size, &used, "READY");
    } else if (survey_running(survey)) {
        append(out, out_size, &used, "SEARCHING  ");
        append_uint(out, out_size, &used,
                    (unsigned long)survey->candidates_checked + 1UL);
        append(out, out_size, &used, "/");
        append_uint(out, out_size, &used,
                    (unsigned long)program->active.control_count);
        append(out, out_size, &used,
               survey->state == P25_SURVEY_SETTLING
                   ? "  SETTLE 250 ms  " : "  DWELL 1500 ms  ");
        char hz[32];
        format_hz(hz, sizeof(hz),
                  program->active.control_channels[survey->candidate_index]);
        append(out, out_size, &used, hz);
    } else if (survey->state == P25_SURVEY_FOUND) {
        append(out, out_size, &used, "LOCKED  ");
        char hz[32];
        format_hz(hz, sizeof(hz), p25_program_selected_control_hz(program));
        append(out, out_size, &used, hz);
        append(out, out_size, &used, "  NID ");
        append_uint(out, out_size, &used, survey->selected_nids);
        append(out, out_size, &used, "  TSBK ");
        append_uint(out, out_size, &used, survey->selected_tsbks);
    } else if (survey->state == P25_SURVEY_NO_CONTROL) {
        append(out, out_size, &used,
               "SEARCHING / NO CONTROL  (previous restored)");
    } else if (survey->state == P25_SURVEY_CANCELED) {
        append(out, out_size, &used, "CANCELED - ");
        append(out, out_size, &used,
               survey_cancel_name(survey->cancel_reason));
        append(out, out_size, &used, "  (previous restored)");
    }
}

void p25_program_apply_roster(p25_scan_ctrl_t *scan,
                              const p25_profile_talkgroup_t *talkgroups,
                              size_t count)
{
    if (!scan) return;
    if (!talkgroups) count = 0;

    p25_scan_allow_clear(scan);
    p25_scan_priority_clear(scan);
    p25_scan_hold_set(scan, 0);

    size_t enabled = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!talkgroups[i].enabled) continue;
        if (p25_scan_allow_add(scan, talkgroups[i].id)) enabled++;
        /* Rank is only meaningful on a talkgroup the radio is allowed to
         * follow, so a disabled row does not get one. */
        if (talkgroups[i].priority > 0)
            (void)p25_scan_priority_set(scan, talkgroups[i].id,
                                        talkgroups[i].priority);
    }

    p25_scan_list_set_mode(scan, enabled > 0 ? P25_SCAN_LIST_ALLOW
                           : count > 0       ? P25_SCAN_LIST_NONE
                                             : P25_SCAN_LIST_OFF);
}

void p25_program_restore_roster(p25_scan_ctrl_t *scan,
                                const p25_profile_talkgroup_t *talkgroups,
                                size_t count)
{
    if (!scan) return;
    if (!talkgroups) count = 0;

    p25_scan_priority_clear(scan);
    for (size_t i = 0; i < count; ++i) {
        if (talkgroups[i].enabled && talkgroups[i].priority > 0)
            (void)p25_scan_priority_set(scan, talkgroups[i].id,
                                        talkgroups[i].priority);
    }
}
