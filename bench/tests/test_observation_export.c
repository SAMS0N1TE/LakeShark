/* LS_TEST_SOURCES: ${FW}/components/apps/tui/ls_observation_export.c ${FW}/components/lakeshark/radio/iq_observation.c */
#include "ls_test.h"
#include "ls_observation_export.h"
#include "scan_journal.h"
#include "call_archive.h"
#include "fm_state.h"
#include <stdlib.h>
#include <string.h>

static scan_journal_status_t journal;
static call_entry_t call;
static ls_iq_observation_t iq;
static ls_iq_mission_t mission;
static bool call_disappeared;
static int calls, calls_read;
typedef struct { char text[32768]; size_t used, lines; bool refuse, nested; } output_t;
static output_t output, inner;
static bool allocation_failed;
void *__real_calloc(size_t count, size_t bytes);
void *__wrap_calloc(size_t count, size_t bytes)
{ return allocation_failed ? NULL : __real_calloc(count, bytes); }

void scan_journal_get_status(scan_journal_status_t *out) { *out = journal; }
int call_archive_count(void) { return calls; }
bool call_archive_entry(int index, call_entry_t *out)
{
    ++calls_read;
    if (call_disappeared || index >= calls) return false;
    *out = call; return true;
}
void fm_get_iq_observation(ls_iq_observation_t *out) { *out = iq; }
bool fm_mission_start(const char *name)
{ return ls_iq_mission_start(&mission, name, &iq, 600000); }
bool fm_mission_stop(void) { return ls_iq_mission_stop(&mission, 900000); }
bool fm_mission_snapshot(ls_iq_mission_status_t *s, ls_iq_mission_entry_t *e, size_t cap, size_t *n)
{ return ls_iq_mission_snapshot(&mission, s, e, cap, n); }
bool fm_mission_status(ls_iq_mission_status_t *s)
{ return fm_mission_snapshot(s, NULL, 0, NULL); }
int64_t __wrap_esp_timer_get_time(void) { return INT64_C(9007199254740993); }

static bool collect(void *context, const char *line, size_t n)
{
    output_t *o = context;
    LS_CHECK(n > 0 && n <= LS_OBSERVATION_LINE_BYTES);
    LS_CHECK(line[0] == '{' && line[n - 1] == '\n');
    LS_CHECK(n + o->used < sizeof(o->text));
    if (o->refuse) return false;
    memcpy(o->text + o->used, line, n); o->used += n; o->text[o->used] = 0;
    o->lines++;
    if (o->nested && o->lines == 1) {
        inner = (output_t){0};
        LS_CHECK(ls_observation_export(collect, &inner));
    }
    return true;
}
static void fixture(void)
{
    journal = (scan_journal_status_t){.initialized=true, .session_active=true,
        .storage_ok=true, .queue_dropped=7, .records_written=14, .write_errors=2,
        .bytes_written=UINT64_C(9007199254740993)};
    strcpy(journal.path, "/sdcard/lakeshark/scan/scan_native.jsonl");
    call = (call_entry_t){.duration_ms=1250, .bytes=40044, .drops=73, .kept=true};
    strcpy(call.path, "/sdcard/lakeshark/calls/20261025/FM_1.wav");
    call.meta=(call_meta_t){.time=1792960000, .hz=154785000, .talkgroup=42,
        .source=1234, .rate=16000, .gps=true, .lat=40.123456, .lon=-74.654321};
    call_disappeared=false; calls=1; calls_read=0;
    allocation_failed=false;
    memset(&mission, 0, sizeof(mission));
    iq=(ls_iq_observation_t){0};
    ls_iq_observation_begin(&iq,256000,154785000);
    LS_CHECK(ls_iq_observation_deliver(&iq,16384,500000));
    output=(output_t){0};
}
static void has(const char *part) { LS_CHECK_MSG(strstr(output.text,part)!=NULL,"missing %s",part); }

LS_CASE(observation_receipt_preserves_native_fields_without_inventing_links)
{
    fixture();
    LS_CHECK(ls_observation_export_workspace_bytes() <= 14336);
    LS_CHECK(ls_observation_export(collect,&output));
    LS_CHECK(output.lines==4 && calls_read==1);
    /* DETECTOR is an SD app: the board has no detector history to export. */
    has("\"detector_snapshot_available\":false,\"detector_recent_total\":null");
    LS_CHECK(!strstr(output.text,"\"type\":\"detector_event\""));
    has("\"scope\":\"bounded_ram_snapshots\"");
    has("\"atomic_across_stores\":false");
    has("\"uptime_us\":\"9007199254740993\"");
    has("\"path\":\"/sdcard/lakeshark/scan/scan_native.jsonl\"");
    has("\"records_written\":\"14\",\"queue_dropped\":\"7\",\"write_errors\":\"2\"");
    has("\"path\":\"/sdcard/lakeshark/calls/20261025/FM_1.wav\"");
    has("\"source_unit_id\":\"1234\""); has("\"artifact_bytes\":\"40044\"");
    has("\"recorder_drops\":null"); /* Struct zero/default is not per-file evidence. */
    has("\"artifact_verified\":false,\"receiver\":null,\"decoder_build\":null");
    has("\"delivered_u8_iq_bytes\":\"16384\",\"delivered_iq_pairs\":\"8192\"");
    has("\"nominal_sample_duration_us\":\"32000\"");
    has("\"rf_coverage_us\":null,\"lost_sample_pairs\":null,\"sample_frequency_verified\":false");
    LS_CHECK(!strstr(output.text,"PRIVATE") && !strstr(output.text,"40.123456"));
    LS_CHECK(!strstr(output.text,"latitude") && !strstr(output.text,"longitude"));
}

LS_CASE(observation_receipt_unknown_and_known_zero_remain_distinct)
{
    fixture(); journal=(scan_journal_status_t){0}; iq=(ls_iq_observation_t){0};
    calls=0;
    LS_CHECK(ls_observation_export(collect,&output));
    has("\"detector_snapshot_available\":false,\"detector_recent_total\":null");
    has("\"records_written\":null,\"queue_dropped\":null");
    has("\"delivered_u8_iq_bytes\":null,\"delivered_iq_pairs\":null");
    has("\"nominal_sample_duration_us\":null");
    LS_CHECK(!strstr(output.text,"\"type\":\"detector_event\""));
    fixture(); calls=0; journal.records_written=0; journal.queue_dropped=0;
    ls_iq_observation_begin(&iq,256000,0);
    LS_CHECK(ls_observation_export(collect,&output));
    has("\"records_written\":\"0\",\"queue_dropped\":\"0\"");
    has("\"delivered_u8_iq_bytes\":\"0\",\"delivered_iq_pairs\":\"0\"");
    has("\"nominal_sample_duration_us\":\"0\",\"first_delivery_us\":null");
}

LS_CASE(observation_receipt_is_bounded_and_escapes_native_paths)
{
    fixture(); calls=CALL_RECENT_MAX;
    strcpy(call.path,"/sdcard/quote\"back\\line\n.wav");
    LS_CHECK(ls_observation_export(collect,&output));
    LS_CHECK(output.lines==11 && calls_read==8);
    has("quote\\\"back\\\\line\\u000a.wav");
    fixture(); memset(call.path,1,sizeof(call.path)); call.path[sizeof(call.path)-1]=0;
    LS_CHECK(!ls_observation_export(collect,&output));
    has("\"type\":\"error\",\"reason\":\"record_bounds\",\"partial\":true");
    fixture(); memset(call.path,'X',sizeof(call.path));
    LS_CHECK(!ls_observation_export(collect,&output)); has("record_bounds");
}

LS_CASE(observation_receipt_handles_refresh_contention_output_failure_and_reentry)
{
    fixture(); call_disappeared=true;
    LS_CHECK(!ls_observation_export(collect,&output));
    has("\"reason\":\"call_snapshot_changed\",\"partial\":true");
    fixture(); output.refuse=true;
    LS_CHECK(!ls_observation_export(collect,&output));
    LS_CHECK(output.lines==0 && calls_read==0);
    fixture(); output.nested=true;
    LS_CHECK(ls_observation_export(collect,&output));
    LS_CHECK(inner.lines==4 && output.lines==4);
    LS_CHECK(!strcmp(inner.text,output.text));
    LS_CHECK(!ls_observation_export(NULL,NULL));
    char *bad[]={"observations","start"};
    LS_CHECK(ls_observation_command(2,bad)==1);
}

LS_CASE(observation_receipt_allocation_failure_is_explicit_without_internal_fallback)
{
    fixture(); allocation_failed=true;
    bool ok=ls_observation_export(collect,&output);
    allocation_failed=false;
    LS_CHECK(!ok && output.lines==1 && calls_read==0);
    has("\"reason\":\"psram_unavailable\",\"partial\":true");
}

LS_CASE(mission_receipt_is_explicit_v2_and_preserves_sample_control_distinction)
{
    fixture(); LS_CHECK(fm_mission_start("field \"A\""));
    LS_CHECK(ls_iq_observation_deliver(&iq, 16384, 700000));
    ls_iq_mission_observe(&mission, &iq, 700000);
    ls_iq_observation_close(&iq, LS_IQ_OBS_TUNE);
    ls_iq_mission_boundary(&mission, &iq, 800000);
    LS_CHECK(fm_mission_stop());
    LS_CHECK(ls_observation_mission_export(collect, &output));
    LS_CHECK(output.lines == 5);
    has("\"schema\":2,\"type\":\"mission_session\"");
    has("\"name\":\"field \\\"A\\\"\",\"active\":false");
    has("\"started_us\":\"600000\",\"stopped_us\":\"900000\"");
    has("\"delivered_u8_iq_bytes\":\"16384\",\"delivered_blocks\":\"1\"");
    has("\"nominal_sample_duration_us\":\"32000\"");
    has("\"kind\":\"boundary\",\"at_us\":\"800000\",\"last_delivery_us\":null");
    has("\"wall_interval_observation\":\"unknown\",\"rf_coverage_us\":null");
    LS_CHECK(!strstr(output.text, "\"schema\":1"));
    output = (output_t){0};
    LS_CHECK(ls_observation_export(collect, &output));
    LS_CHECK(output.lines == 4 && !strstr(output.text, "mission_session"));
}
LS_CASE(mission_receipt_bounds_large_integer_bits_and_explicit_failure)
{
    fixture(); LS_CHECK(!ls_observation_mission_export(collect, &output));
    has("\"schema\":2,\"type\":\"error\",\"reason\":\"mission_unavailable\"");
    fixture(); LS_CHECK(fm_mission_start("0123456789012345678901234567890"));
    for (unsigned i = 0; i < 40; ++i) {
        ls_iq_observation_begin(&iq, UINT32_MAX, UINT64_MAX);
        LS_CHECK(ls_iq_observation_deliver(&iq, 16384, UINT64_C(9007199254740993) + i));
        ls_iq_mission_observe(&mission, &iq, UINT64_C(9007199254740993) + i);
        ls_iq_observation_close(&iq, LS_IQ_OBS_MODE);
        ls_iq_mission_boundary(&mission, &iq, UINT64_C(9007199254740993) + i);
    }
    LS_CHECK(ls_observation_mission_export(collect, &output));
    LS_CHECK(output.lines == 33);
    has("\"overwritten_entries\":\"49\"");
    has("\"configured_center_hz\":\"18446744073709551615\"");
    fixture(); allocation_failed = true;
    LS_CHECK(!ls_observation_mission_export(collect, &output));
    allocation_failed = false; has("\"schema\":2,\"type\":\"error\",\"reason\":\"psram_unavailable\"");
    fixture(); LS_CHECK(fm_mission_start("refusal")); output.refuse = true;
    LS_CHECK(!ls_observation_mission_export(collect, &output)); LS_CHECK(output.lines == 0);
}
