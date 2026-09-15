/* Asynchronous, bounded SD journal for low-rate scanner events. */
#ifndef LS_SCAN_JOURNAL_H
#define LS_SCAN_JOURNAL_H

#include "scan_journal_codec.h"

typedef struct {
    bool initialized;
    bool session_active;
    bool storage_ok;
    uint32_t queue_dropped;
    uint32_t records_written;
    uint32_t write_errors;
    uint64_t bytes_written;
    char path[112];
    char detail[64];
} scan_journal_status_t;

void scan_journal_init(void);
bool scan_journal_session_start(const scan_journal_record_t *record);
bool scan_journal_emit(const scan_journal_record_t *record);
bool scan_journal_session_stop(const scan_journal_record_t *record);
void scan_journal_get_status(scan_journal_status_t *out);

#endif
