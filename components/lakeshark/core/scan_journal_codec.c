#include "scan_journal_codec.h"

#include <stdio.h>
#include <string.h>

const char *scan_journal_kind_name(scan_journal_kind_t kind)
{
    switch (kind) {
    case SCAN_JOURNAL_SESSION_START:  return "session_start";
    case SCAN_JOURNAL_SESSION_STOP:   return "session_stop";
    case SCAN_JOURNAL_HOLD:           return "hold";
    case SCAN_JOURNAL_RELEASE:        return "release";
    case SCAN_JOURNAL_RECEIVER:       return "receiver";
    case SCAN_JOURNAL_SUMMARY:        return "summary";
    case SCAN_JOURNAL_QUEUE_OVERFLOW: return "queue_overflow";
    default:                          return "unknown";
    }
}

static size_t json_escape(char *out, size_t capacity, const char *text)
{
    size_t used = 0;
    if (!text) text = "";
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        const char *escape = NULL;
        char encoded[7];
        if (*p == '"') escape = "\\\"";
        else if (*p == '\\') escape = "\\\\";
        else if (*p == '\n') escape = "\\n";
        else if (*p == '\r') escape = "\\r";
        else if (*p == '\t') escape = "\\t";
        else if (*p < 0x20) {
            snprintf(encoded, sizeof(encoded), "\\u%04x", (unsigned)*p);
            escape = encoded;
        }
        if (escape) {
            size_t n = strlen(escape);
            if (used + n < capacity) memcpy(out + used, escape, n);
            used += n;
        } else {
            if (used + 1 < capacity) out[used] = (char)*p;
            used++;
        }
    }
    if (capacity) out[used < capacity ? used : capacity - 1] = 0;
    return used;
}

int scan_journal_format_json(const scan_journal_record_t *r,
                             char *out, size_t capacity)
{
    if (!r || !out || capacity == 0) return -1;
    char channel[64], reason[96];
    char latitude[32] = "null", longitude[32] = "null";
    json_escape(channel, sizeof(channel), r->channel);
    json_escape(reason, sizeof(reason), r->reason);
    if (r->gps_valid) {
        snprintf(latitude, sizeof(latitude), "%.7f", r->latitude);
        snprintf(longitude, sizeof(longitude), "%.7f", r->longitude);
    }
    int n = snprintf(out, capacity,
        "{\"schema\":%u,\"event\":\"%s\",\"session_id\":%lu,\"sequence\":%lu,"
        "\"uptime_ms\":%llu,\"unix_time\":%lld,\"source\":\"%s\","
        "\"mode\":%d,\"channel\":\"%s\",\"reason\":\"%s\","
        "\"requested_hz\":%lu,\"effective_known\":%s,\"effective_hz\":%lu,"
        "\"power_pct\":%d,\"radio_error\":%d,\"manual_hold\":%s,"
        "\"gps_valid\":%s,\"gps_age_ms\":%lu,\"latitude\":%s,"
        "\"longitude\":%s,\"counts\":{\"tunes\":%lu,\"holds\":%lu,"
        "\"releases\":%lu,\"receiver_errors\":%lu,\"queue_dropped\":%lu}}",
        SCAN_JOURNAL_SCHEMA_VERSION, scan_journal_kind_name(r->kind),
        (unsigned long)r->session_id, (unsigned long)r->sequence,
        (unsigned long long)r->uptime_ms,
        (long long)r->unix_time, r->source ? "band" : "channels", (int)r->mode,
        channel, reason, (unsigned long)r->requested_hz,
        r->effective_known ? "true" : "false", (unsigned long)r->effective_hz,
        (int)r->power_pct, (int)r->radio_error,
        r->manual_hold ? "true" : "false", r->gps_valid ? "true" : "false",
        (unsigned long)r->gps_age_ms, latitude, longitude, (unsigned long)r->tunes,
        (unsigned long)r->holds, (unsigned long)r->releases,
        (unsigned long)r->receiver_errors, (unsigned long)r->dropped_events);
    if (n < 0 || (size_t)n >= capacity) {
        out[0] = 0;
        return -1;
    }
    return n;
}

void scan_journal_policy_begin(scan_journal_policy_t *p,
                               bool storage_available, uint64_t free_bytes)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->active = true;
    p->free_remaining = free_bytes;
    p->storage_ok = storage_available && free_bytes > SCAN_JOURNAL_FREE_RESERVE;
}

bool scan_journal_policy_can_write(const scan_journal_policy_t *p, size_t bytes)
{
    if (!p || !p->active || !p->storage_ok || bytes == 0) return false;
    if (p->bytes >= SCAN_JOURNAL_FILE_LIMIT) return false;
    if ((uint64_t)bytes > SCAN_JOURNAL_FILE_LIMIT - p->bytes) return false;
    return p->free_remaining > SCAN_JOURNAL_FREE_RESERVE &&
           (uint64_t)bytes <= p->free_remaining - SCAN_JOURNAL_FREE_RESERVE;
}

void scan_journal_policy_note_write(scan_journal_policy_t *p,
                                    size_t requested, size_t written)
{
    if (!p) return;
    if (requested == 0 || written != requested) {
        p->write_errors++;
        p->storage_ok = false;
        return;
    }
    p->bytes += written;
    if (p->free_remaining >= written) p->free_remaining -= written;
    else p->free_remaining = 0;
    p->records++;
}

void scan_journal_policy_note_drop(scan_journal_policy_t *p, uint32_t count)
{
    if (p) p->queue_dropped += count;
}

void scan_journal_policy_stop(scan_journal_policy_t *p)
{
    if (p) p->active = false;
}
