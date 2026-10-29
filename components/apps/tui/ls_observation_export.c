/* SPDX-License-Identifier: GPL-3.0-or-later
 * Read-only, bounded export over the snapshots their existing owners publish.
 * No directory walk, SD read/write, receiver control or background task. */
#include "ls_observation_export.h"
#include "scan_journal.h"
#include "call_archive.h"
#include "fm_state.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "ls_textpack.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define ALLOC(n) heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define FREE(p) heap_caps_free(p)
#else
#define ALLOC(n) calloc(1, n)
#define FREE(p) free(p)
#endif

typedef struct {
    union {
        struct {
            scan_journal_status_t scan;
            call_entry_t call;
            ls_iq_observation_t iq;
        };
        struct {
            ls_iq_mission_status_t status;
            ls_iq_mission_entry_t entries[LS_IQ_MISSION_CAPACITY];
        } mission;
    };
    char line[LS_OBSERVATION_LINE_BYTES];
    size_t used;
    bool failed;
    unsigned schema;
} receipt_t;
_Static_assert(sizeof(receipt_t) <= 14336, "observation receipt PSRAM budget");

static void raw(receipt_t *r, const char *text)
{
    size_t n = strlen(text);
    if (n >= sizeof(r->line) - r->used) { r->failed = true; return; }
    memcpy(r->line + r->used, text, n); r->used += n; r->line[r->used] = 0;
}
static void character(receipt_t *r, char c)
{
    if (r->used + 1 >= sizeof(r->line)) { r->failed = true; return; }
    r->line[r->used++] = c; r->line[r->used] = 0;
}
static void number(receipt_t *r, uint64_t value)
{
    char digits[20]; unsigned n = 0;
    do { digits[n++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (n) character(r, digits[--n]);
}
static void string(receipt_t *r, const char *text, size_t bound)
{
    static const char hex[] = "0123456789abcdef";
    character(r, '"');
    for (size_t i = 0; i < bound; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!c) { character(r, '"'); return; }
        if (c == '"' || c == '\\') { character(r, '\\'); character(r, (char)c); }
        else if (c < 32) {
            raw(r, "\\u00"); character(r, hex[c >> 4]); character(r, hex[c & 15]);
        } else character(r, (char)c);
    }
    r->failed = true; /* Never read beyond or silently truncate a native field. */
}
static void key(receipt_t *r, const char *name)
{
    raw(r, ",\""); raw(r, name); raw(r, "\":");
}
static void key_bool(receipt_t *r, const char *name, bool value)
{ key(r, name); raw(r, value ? "true" : "false"); }
/* Counters/monotonic timestamps are strings to preserve native integer bits
 * in browser consumers, including values above JavaScript's 2^53 limit. */
static void key_number(receipt_t *r, const char *name, bool known, uint64_t value)
{
    key(r, name);
    if (!known) { raw(r, "null"); return; }
    character(r, '"'); number(r, value); character(r, '"');
}
static void key_string(receipt_t *r, const char *name, const char *value, size_t cap)
{ key(r, name); string(r, value, cap); }
static void begin(receipt_t *r, const char *type)
{
    r->used = 0; r->failed = false; r->schema = 1;
    raw(r, "{\"schema\":1,\"type\":"); string(r, type, strlen(type) + 1);
}
static void begin_mission(receipt_t *r, const char *type)
{
    r->used = 0; r->failed = false; r->schema = 2;
    raw(r, "{\"schema\":2,\"type\":"); string(r, type, strlen(type) + 1);
}
static bool emit(receipt_t *r, ls_observation_write_t write, void *context, bool *complete)
{
    raw(r, "}\n");
    if (r->failed) {
        *complete = false;
        const char *error = r->schema == 2
            ? "{\"schema\":2,\"type\":\"error\",\"reason\":\"record_bounds\",\"partial\":true}\n"
            : "{\"schema\":1,\"type\":\"error\",\"reason\":\"record_bounds\",\"partial\":true}\n";
        return write(context, error, strlen(error));
    }
    return write(context, r->line, r->used);
}

size_t ls_observation_export_workspace_bytes(void) { return sizeof(receipt_t); }

bool ls_observation_export(ls_observation_write_t write, void *context)
{
    if (!write) return false;
    receipt_t *r = ALLOC(sizeof(*r));
    if (!r) {
        const char *error = "{\"schema\":1,\"type\":\"error\",\"reason\":\"psram_unavailable\",\"partial\":true}\n";
        (void)write(context, error, strlen(error)); return false;
    }
    bool complete = true;
    scan_journal_get_status(&r->scan);
    /* DETECTOR is an SD app (SDK 2.2): the board keeps no detector history,
       so the detector keys say so and no detector_event lines follow. */
    const bool detector_ready = false;
    int calls = call_archive_count();
    if (calls < 0) calls = 0;
    fm_get_iq_observation(&r->iq);
    begin(r, "snapshot");
    key_string(r, "scope", LS_TEXT(t_ls_observation_export_bounded_ram_snapshots, "bounded_ram_snapshots"), 22);
    key_number(r, "uptime_us", true, (uint64_t)esp_timer_get_time());
    key_bool(r, LS_TEXT(t_ls_observation_export_atomic_across_stores, "atomic_across_stores"), false);
    key_string(r, "time_scope", "current_boot_only", 18);
    raw(r, LS_TEXT(t_ls_observation_export_rf_coverage_null_sample, ",\"rf_coverage\":null,\"sample_continuity\":null,\"relationships\":null"));
    key_bool(r, LS_TEXT(t_ls_observation_export_detector_snapshot_available, "detector_snapshot_available"), detector_ready);
    key_number(r, LS_TEXT(t_ls_observation_export_detector_recent_total, "detector_recent_total"), detector_ready, 0);
    key_number(r, "detector_exported", true, 0);
    key_number(r, LS_TEXT(t_ls_observation_export_calls_recent_available, "calls_recent_available"), true, (uint64_t)calls);
    raw(r, LS_TEXT(t_ls_observation_export_call_limit_8_detector, ",\"call_limit\":8,\"detector_limit\":4,\"omitted\":[\"GPS\",\"payload_text\",\"SAME\",\"other_stores\"]"));
    if (!emit(r, write, context, &complete)) goto output_failed;

    begin(r, "scanner_journal");
    key_bool(r, "initialized", r->scan.initialized);
    key_bool(r, "session_active", r->scan.session_active);
    key_bool(r, "storage_ok", r->scan.storage_ok);
    key_number(r, "records_written", r->scan.initialized, r->scan.records_written);
    key_number(r, "queue_dropped", r->scan.initialized, r->scan.queue_dropped);
    key_number(r, "write_errors", r->scan.initialized, r->scan.write_errors);
    key_number(r, "bytes_written", r->scan.initialized, r->scan.bytes_written);
    key(r, "path");
    if (r->scan.path[0]) string(r, r->scan.path, sizeof(r->scan.path)); else raw(r, "null");
    if (!emit(r, write, context, &complete)) goto output_failed;

    int limit = calls < (int)LS_OBSERVATION_CALL_LIMIT ? calls : (int)LS_OBSERVATION_CALL_LIMIT;
    for (int i = 0; i < limit; i++) {
        if (!call_archive_entry(i, &r->call)) {
            complete = false;
            begin(r, "error");
            key_string(r, "reason", LS_TEXT(t_ls_observation_export_call_snapshot_changed, "call_snapshot_changed"), 22);
            key_bool(r, "partial", true);
            if (!emit(r, write, context, &complete)) goto output_failed;
            continue;
        }
        const call_entry_t *c = &r->call;
        begin(r, "call_artifact");
        key_string(r, "path", c->path, sizeof(c->path));
        key_number(r, "utc", c->meta.time > 0, (uint64_t)c->meta.time);
        key_number(r, "frequency_hz", c->meta.hz != 0, c->meta.hz);
        key_number(r, "talkgroup", true, c->meta.talkgroup);
        key_number(r, "source_unit_id", true, c->meta.source);
        key_number(r, "pcm_rate_hz", c->meta.rate != 0, c->meta.rate);
        key_number(r, "duration_ms", true, c->duration_ms);
        key_number(r, "artifact_bytes", c->bytes != 0, c->bytes);
        /* call_store_scan copies file size including the WAV header. The
         * native sidecar carries no per-file drops; its zero-initialized
         * struct member cannot establish a lossless recording. */
        raw(r, LS_TEXT(t_ls_observation_export_recorder_drops_null, ",\"recorder_drops\":null"));
        key_bool(r, "protected", c->kept);
        raw(r, LS_TEXT(t_ls_observation_export_artifact_verified_false_receiver, ",\"artifact_verified\":false,\"receiver\":null,\"decoder_build\":null"));
        if (!emit(r, write, context, &complete)) goto output_failed;
    }
    begin(r, "fm_dsp_iq");
    key_string(r, "scope", LS_TEXT(t_ls_observation_export_latest_external_fm_segment, "latest_external_fm_segment"), 27);
    key_bool(r, "known", r->iq.known);
    key_bool(r, "segment_open", r->iq.accepting);
    const char *state = ls_iq_observation_state_name(r->iq.state);
    key_string(r, "state", state, strlen(state) + 1);
    key_number(r, "segment", r->iq.known, r->iq.generation);
    key_number(r, LS_TEXT(t_ls_observation_export_configured_center_hz, "configured_center_hz"), r->iq.known && r->iq.configured_center_hz != 0,
               r->iq.configured_center_hz);
    key_number(r, "nominal_rate_hz", r->iq.known, r->iq.nominal_rate_hz);
    key_number(r, LS_TEXT(t_ls_observation_export_delivered_u8_iq_bytes, "delivered_u8_iq_bytes"), r->iq.known, r->iq.delivered_bytes);
    key_number(r, "delivered_iq_pairs", r->iq.known && !r->iq.saturated, r->iq.delivered_bytes / 2);
    key_number(r, "delivered_blocks", r->iq.known, r->iq.delivered_blocks);
    uint64_t duration = 0;
    bool duration_known = ls_iq_observation_nominal_us(&r->iq, &duration);
    key_number(r, LS_TEXT(t_ls_observation_export_nominal_sample_duration_us, "nominal_sample_duration_us"), duration_known, duration);
    key_number(r, "first_delivery_us", r->iq.known && r->iq.delivered_blocks != 0, r->iq.first_delivery_us);
    key_number(r, "last_delivery_us", r->iq.known && r->iq.delivered_blocks != 0, r->iq.last_delivery_us);
    key_bool(r, "counter_saturated", r->iq.saturated);
    raw(r, LS_TEXT(t_ls_observation_export_rf_coverage_us_null, ",\"rf_coverage_us\":null,\"lost_sample_pairs\":null,\"sample_frequency_verified\":false"));
    if (!emit(r, write, context, &complete)) goto output_failed;
    FREE(r); return complete;
output_failed:
    FREE(r); return false;
}

static bool console_write(void *unused, const char *line, size_t n)
{ (void)unused; return fwrite(line, 1, n, stdout) == n; }

bool ls_observation_mission_export(ls_observation_write_t write, void *context)
{
    if (!write) return false;
    receipt_t *r = ALLOC(sizeof(*r));
    if (!r) {
        const char *error = "{\"schema\":2,\"type\":\"error\",\"reason\":\"psram_unavailable\",\"partial\":true}\n";
        (void)write(context, error, strlen(error)); return false;
    }
    size_t n = 0;
    bool present = fm_mission_snapshot(&r->mission.status, r->mission.entries,
                                        LS_IQ_MISSION_CAPACITY, &n);
    bool complete = true;
    if (!present || n > LS_IQ_MISSION_CAPACITY) {
        begin_mission(r, "error");
        key_string(r, "reason", "mission_unavailable", 20);
        key_bool(r, "partial", true);
        (void)emit(r, write, context, &complete); FREE(r); return false;
    }
    const ls_iq_mission_status_t *s = &r->mission.status;
    begin_mission(r, "mission_session");
    key_string(r, "scope", LS_TEXT(t_ls_observation_export_bounded_fm_dsp_ledger, "bounded_fm_dsp_ledger"), 22);
    key_string(r, "time_scope", "current_boot_only", 18);
    key_number(r, "mission_id", true, s->mission_id);
    key_string(r, "name", s->name, sizeof(s->name));
    key_bool(r, "active", s->active);
    key_number(r, "started_us", true, s->started_us);
    key_number(r, "stopped_us", !s->active, s->stopped_us);
    key_number(r, "retained_entries", true, n);
    key_number(r, "overwritten_entries", true, s->overwritten_entries);
    key_number(r, "clock_regressions", true, s->clock_regressions);
    key_bool(r, "counter_saturated", s->counter_saturated);
    key_number(r, LS_TEXT(t_ls_observation_export_delivered_u8_iq_bytes, "delivered_u8_iq_bytes"), !s->counter_saturated, s->delivered_bytes);
    key_number(r, "delivered_blocks", !s->counter_saturated, s->delivered_blocks);
    raw(r, LS_TEXT(t_ls_observation_export_rf_coverage_us_null_2, ",\"rf_coverage_us\":null,\"sample_continuity\":null,\"sample_frequency_verified\":false,\"boot_id\":null"));
    if (!emit(r, write, context, &complete)) goto failed;
    for (size_t i = 0; i < n; ++i) {
        const ls_iq_mission_entry_t *e = &r->mission.entries[i];
        bool samples = e->kind == LS_IQ_MISSION_SEGMENT;
        bool known = samples && e->sample_accounting_known && !e->counter_saturated;
        begin_mission(r, "mission_entry");
        key_number(r, "mission_id", true, s->mission_id);
        key_number(r, "sequence", true, e->sequence);
        const char *kind = ls_iq_mission_kind_name(e->kind);
        key_string(r, "kind", kind, strlen(kind) + 1);
        key_number(r, "at_us", true, e->at_us);
        key_number(r, "last_delivery_us", samples, e->last_delivery_us);
        key_number(r, "generation", true, e->generation);
        const char *state = ls_iq_observation_state_name(e->state);
        key_string(r, "state", state, strlen(state) + 1);
        key_number(r, LS_TEXT(t_ls_observation_export_configured_center_hz, "configured_center_hz"), e->configured_center_hz != 0, e->configured_center_hz);
        key_number(r, "nominal_rate_hz", e->nominal_rate_hz != 0, e->nominal_rate_hz);
        key_number(r, LS_TEXT(t_ls_observation_export_delivered_u8_iq_bytes, "delivered_u8_iq_bytes"), known, e->delivered_bytes);
        key_number(r, "delivered_blocks", known, e->delivered_blocks);
        ls_iq_observation_t segment = {.known=known, .nominal_rate_hz=e->nominal_rate_hz,
            .delivered_bytes=e->delivered_bytes, .saturated=e->counter_saturated};
        uint64_t nominal = 0;
        bool nominal_known = ls_iq_observation_nominal_us(&segment, &nominal);
        key_number(r, LS_TEXT(t_ls_observation_export_nominal_sample_duration_us, "nominal_sample_duration_us"), nominal_known, nominal);
        key_bool(r, "counter_saturated", e->counter_saturated);
        key_bool(r, "clock_regressed", e->clock_regressed);
        raw(r, LS_TEXT(t_ls_observation_export_wall_interval_observation_unknow, ",\"wall_interval_observation\":\"unknown\",\"rf_coverage_us\":null,\"sample_continuity\":null,\"sample_frequency_verified\":false"));
        if (!emit(r, write, context, &complete)) goto failed;
    }
    FREE(r); return complete;
failed:
    FREE(r); return false;
}

static int mission_command(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[2], "start")) {
        bool ok = fm_mission_start(argv[3]);
        puts(ok ? "mission started; wall RF coverage unknown" : "mission start failed: active, invalid name, exhausted ID, or PSRAM unavailable");
        return ok ? 0 : 1;
    }
    if (argc == 3 && !strcmp(argv[2], "stop")) {
        bool ok = fm_mission_stop();
        puts(ok ? "mission stopped; receipt retained until next start or reboot" : "no active mission");
        return ok ? 0 : 1;
    }
    if (argc == 3 && !strcmp(argv[2], "json"))
        return ls_observation_mission_export(console_write, NULL) ? 0 : 1;
    if (argc == 3 && !strcmp(argv[2], "status")) {
        ls_iq_mission_status_t s;
        if (!fm_mission_status(&s)) { LS_PUTS(t_ls_observation_export_no_mission_this_boot, "no mission this boot"); return 0; }
        char blocks[32];
        if (s.counter_saturated) strcpy(blocks, "unknown (counter saturated)");
        else snprintf(blocks, sizeof(blocks), "%" PRIu64, s.delivered_blocks);
        printf("mission %u %s [%s]; delivered blocks=%s"
               "; retained=%u overwritten=%" PRIu64 "; clock regressions=%" PRIu64
               "; wall RF coverage unknown\n",
               (unsigned)s.mission_id, s.name, s.active ? "active" : "stopped",
               blocks, (unsigned)s.retained_entries, s.overwritten_entries, s.clock_regressions);
        return 0;
    }
    LS_PUTS(msg_ls_observation_export_usage_observations_mission_start, "usage: observations mission start <name>|stop|status|json (name: 1..31 printable ASCII bytes)");
    return 1;
}
int ls_observation_command(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "mission")) return mission_command(argc, argv);
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "json"))) {
        LS_PUTS(msg_ls_observation_export_usage_observations_json_bounded, "usage: observations [json] — bounded read-only receipt; no RF coverage claim");
        return 1;
    }
    return ls_observation_export(console_write, NULL) ? 0 : 1;
}
