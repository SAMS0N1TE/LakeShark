/* LS-688: bounded, transactional P25 PROGRAM profile model.
 *
 * Parsing never allocates and never keeps process-global state.  The caller
 * owns the destination and a separate p25_profile_parse_scratch_t.  On the
 * device the SD adapter should place both the input file and scratch in PSRAM;
 * none of this belongs on a decoder-task stack or in an internal-RAM
 * singleton.  The destination is copied only after the complete input has
 * validated, so every error leaves the caller's last-good profile untouched.
 */
#ifndef P25_PROFILE_H
#define P25_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "p25_demod_control.h"
#include "p25_cqpsk_controls.h"
#include "radio_endpoint.h"
#include "scan_ctrl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P25_PROFILE_FORMAT_VERSION       1U
#define P25_PROFILE_CONTROL_MAX          16U
#define P25_PROFILE_TALKGROUP_MAX        P25_SCAN_ALLOW_MAX
#define P25_PROFILE_PRIORITY_MAX         P25_SCAN_PRIORITY_MAX
#define P25_PROFILE_ALIAS_LEN            P25_SCAN_NAME_LEN
#define P25_PROFILE_SYSTEM_NAME_LEN      32U
#define P25_PROFILE_SITE_NAME_LEN        32U
#define P25_PROFILE_MAX_LINE_LEN         P25_SCAN_NAMES_MAX_LINE_LEN
#define P25_PROFILE_MAX_FILE_BYTES       (P25_PROFILE_MAX_LINE_LEN * 112U)

typedef struct {
    uint16_t id;
    char     alias[P25_PROFILE_ALIAS_LEN];
    bool     enabled;
    /* The scan controller's rank: zero is ordinary, larger values win. */
    uint8_t  priority;
} p25_profile_talkgroup_t;

typedef struct {
    uint16_t format_version;
    char     system_name[P25_PROFILE_SYSTEM_NAME_LEN];
    char     site_name[P25_PROFILE_SITE_NAME_LEN];

    uint64_t control_channels[P25_PROFILE_CONTROL_MAX];
    uint8_t  control_count;
    uint64_t preferred_control_hz;

    bool     auto_follow;
    bool     encrypted_skip_enabled;
    uint32_t encrypted_skip_ms;
    /* P25_DEMOD_AUTO or one of the existing demod_mode_t values. */
    int      demod_preference;
    p25_cqpsk_config_t cqpsk;

    p25_profile_talkgroup_t talkgroups[P25_PROFILE_TALKGROUP_MAX];
    uint8_t  talkgroup_count;
} p25_profile_t;

/* Actual endpoint ranges are caller-supplied.  A later SD adapter can pass
 * the acquired endpoint's advertised ranges; the parser therefore does not
 * fork tuner limits or assume that all P25 systems are in one band. */
typedef struct {
    const ls_radio_range_t *tune_ranges;
    size_t                  tune_range_count;
} p25_profile_parse_config_t;

typedef enum {
    P25_PROFILE_ERROR_NONE = 0,
    P25_PROFILE_ERROR_ARGUMENT,
    P25_PROFILE_ERROR_INPUT_TOO_LONG,
    P25_PROFILE_ERROR_LINE_TOO_LONG,
    P25_PROFILE_ERROR_SYNTAX,
    P25_PROFILE_ERROR_UNKNOWN_FIELD,
    P25_PROFILE_ERROR_DUPLICATE_FIELD,
    P25_PROFILE_ERROR_MISSING_FIELD,
    P25_PROFILE_ERROR_UNSUPPORTED_VERSION,
    P25_PROFILE_ERROR_INVALID_BOOLEAN,
    P25_PROFILE_ERROR_INVALID_DEMOD,
    P25_PROFILE_ERROR_UNSAFE_LOOP_GAIN,
    P25_PROFILE_ERROR_INVALID_NUMBER,
    P25_PROFILE_ERROR_NUMBER_OVERFLOW,
    P25_PROFILE_ERROR_VALUE_TOO_LONG,
    P25_PROFILE_ERROR_FREQUENCY_OUT_OF_RANGE,
    P25_PROFILE_ERROR_DUPLICATE_CONTROL,
    P25_PROFILE_ERROR_CONTROL_CAPACITY,
    P25_PROFILE_ERROR_PREFERRED_NOT_FOUND,
    P25_PROFILE_ERROR_MALFORMED_TALKGROUP,
    P25_PROFILE_ERROR_DUPLICATE_TALKGROUP,
    P25_PROFILE_ERROR_TALKGROUP_CAPACITY,
    P25_PROFILE_ERROR_PRIORITY_CAPACITY,
} p25_profile_error_t;

typedef struct {
    size_t              line;   /* one-based; zero means call-level error */
    p25_profile_error_t code;
    const char         *reason; /* static storage, never NULL              */
} p25_profile_diagnostic_t;

/* About two KiB with current policy limits.  It is deliberately visible so
 * callers can choose the memory capability and account for it.  It must not
 * overlap dst. */
typedef struct {
    p25_profile_t candidate;
    char          line[P25_PROFILE_MAX_LINE_LEN + 1U];
} p25_profile_parse_scratch_t;

/* Parse input[0..input_len).  input need not be NUL-terminated and the last
 * line need not end in LF.  Returns true only after committing the complete
 * candidate to dst. */
bool p25_profile_parse(p25_profile_t *dst,
                       p25_profile_parse_scratch_t *scratch,
                       const char *input, size_t input_len,
                       const p25_profile_parse_config_t *config,
                       p25_profile_diagnostic_t *diagnostic);

const char *p25_profile_error_reason(p25_profile_error_t error);

#ifdef __cplusplus
}
#endif

#endif /* P25_PROFILE_H */
