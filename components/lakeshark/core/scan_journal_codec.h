/* Portable scan-journal records and bounded-file policy. */
#ifndef LS_SCAN_JOURNAL_CODEC_H
#define LS_SCAN_JOURNAL_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCAN_JOURNAL_SCHEMA_VERSION 1u
#define SCAN_JOURNAL_FILE_LIMIT (UINT64_C(4) * 1024 * 1024)
#define SCAN_JOURNAL_FREE_RESERVE (UINT64_C(8) * 1024 * 1024)

typedef enum {
    SCAN_JOURNAL_SESSION_START = 0,
    SCAN_JOURNAL_SESSION_STOP,
    SCAN_JOURNAL_HOLD,
    SCAN_JOURNAL_RELEASE,
    SCAN_JOURNAL_RECEIVER,
    SCAN_JOURNAL_SUMMARY,
    SCAN_JOURNAL_QUEUE_OVERFLOW,
} scan_journal_kind_t;

typedef struct {
    scan_journal_kind_t kind;
    uint32_t session_id;
    uint32_t sequence;
    uint64_t uptime_ms;
    int64_t unix_time;
    uint8_t source;                 /* 0 stored channels, 1 frequency grid */
    int8_t mode;                    /* scan mode, or -1 when not applicable */
    uint32_t requested_hz;
    uint32_t effective_hz;
    bool effective_known;
    int16_t power_pct;              /* -1 when not measured */
    int16_t radio_error;
    bool manual_hold;
    char channel[20];
    char reason[28];
    bool gps_valid;
    uint32_t gps_age_ms;
    double latitude;
    double longitude;
    uint32_t tunes;
    uint32_t holds;
    uint32_t releases;
    uint32_t receiver_errors;
    uint32_t dropped_events;
} scan_journal_record_t;

typedef struct {
    bool active;
    bool storage_ok;
    uint64_t bytes;
    uint64_t free_remaining;
    uint32_t records;
    uint32_t write_errors;
    uint32_t queue_dropped;
} scan_journal_policy_t;

const char *scan_journal_kind_name(scan_journal_kind_t kind);
int scan_journal_format_json(const scan_journal_record_t *record,
                             char *out, size_t capacity);

void scan_journal_policy_begin(scan_journal_policy_t *policy,
                               bool storage_available, uint64_t free_bytes);
bool scan_journal_policy_can_write(const scan_journal_policy_t *policy,
                                   size_t bytes);
void scan_journal_policy_note_write(scan_journal_policy_t *policy,
                                    size_t requested, size_t written);
void scan_journal_policy_note_drop(scan_journal_policy_t *policy,
                                   uint32_t count);
void scan_journal_policy_stop(scan_journal_policy_t *policy);

#endif
