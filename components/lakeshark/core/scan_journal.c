#include "scan_journal.h"

#include "ls_sdcard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define JOURNAL_QUEUE_DEPTH 24
#define JOURNAL_LINE_MAX 512
#define JOURNAL_STACK 4096

static const char *TAG = "scanlog";
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static volatile bool s_accepting;
static volatile uint32_t s_sequence;
static volatile uint32_t s_dropped;
static volatile uint32_t s_session_number;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static scan_journal_status_t s_status;

static void publish_policy(const scan_journal_policy_t *policy,
                           const char *detail)
{
    portENTER_CRITICAL(&s_lock);
    s_status.session_active = policy->active;
    s_status.storage_ok = policy->storage_ok;
    s_status.records_written = policy->records;
    s_status.write_errors = policy->write_errors;
    s_status.bytes_written = policy->bytes;
    s_status.queue_dropped = __atomic_load_n(&s_dropped, __ATOMIC_ACQUIRE);
    if (detail) snprintf(s_status.detail, sizeof(s_status.detail), "%s", detail);
    portEXIT_CRITICAL(&s_lock);
}

static bool write_record(FILE *file, scan_journal_policy_t *policy,
                         const scan_journal_record_t *record)
{
    char line[JOURNAL_LINE_MAX];
    int n = scan_journal_format_json(record, line, sizeof(line));
    if (n < 0 || !scan_journal_policy_can_write(policy, (size_t)n + 1)) {
        policy->storage_ok = false;
        policy->write_errors++;
        publish_policy(policy, "journal full or free-space reserve reached; scanner continues");
        return false;
    }
    size_t a = fwrite(line, 1, (size_t)n, file);
    size_t b = a == (size_t)n ? fwrite("\n", 1, 1, file) : 0;
    bool ok = a == (size_t)n && b == 1 && fflush(file) == 0;
    scan_journal_policy_note_write(policy, (size_t)n + 1, ok ? (size_t)n + 1 : a + b);
    publish_policy(policy, ok ? "logging" : "SD write failed; scanner continues");
    return ok;
}

static FILE *open_session(const scan_journal_record_t *record,
                          scan_journal_policy_t *policy)
{
    portENTER_CRITICAL(&s_lock);
    s_status.path[0] = 0;
    portEXIT_CRITICAL(&s_lock);
    uint64_t total = 0, free_bytes = 0;
    bool available = ls_sdcard_mounted() && ls_sdcard_size(&total, &free_bytes);
    scan_journal_policy_begin(policy, available, free_bytes);
    if (!policy->storage_ok) {
        publish_policy(policy, available ? "SD too full; scanner continues" :
                                         "SD unavailable; scanner continues");
        return NULL;
    }
    if ((mkdir("/sdcard/lakeshark", 0775) != 0 && errno != EEXIST) ||
        (mkdir("/sdcard/lakeshark/scan", 0775) != 0 && errno != EEXIST)) {
        policy->storage_ok = false;
        policy->write_errors++;
        publish_policy(policy, "journal directory failed; scanner continues");
        return NULL;
    }
    char path[112];
    FILE *file = NULL;
    for (unsigned suffix = 0; suffix < 100 && !file; ++suffix) {
        snprintf(path, sizeof(path),
                 "/sdcard/lakeshark/scan/scan_%08llx_%012llx_%04lx_%02u.jsonl",
                 (unsigned long long)(uint64_t)record->unix_time,
                 (unsigned long long)record->uptime_ms,
                 (unsigned long)record->session_id, suffix);
        FILE *probe = fopen(path, "rb");
        if (probe) { fclose(probe); continue; }
        file = fopen(path, "wb");
    }
    if (!file) {
        policy->storage_ok = false;
        policy->write_errors++;
        publish_policy(policy, "journal open failed; scanner continues");
        return NULL;
    }
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.path, sizeof(s_status.path), "%s", path);
    portEXIT_CRITICAL(&s_lock);
    publish_policy(policy, "logging");
    return file;
}

static void journal_task(void *unused)
{
    (void)unused;
    FILE *file = NULL;
    scan_journal_policy_t policy = {0};
    uint32_t reported_drops = 0;
    uint32_t last_written_sequence = 0;
    uint32_t open_session_id = 0;
    scan_journal_record_t record;
    for (;;) {
        if (xQueueReceive(s_queue, &record, portMAX_DELAY) != pdTRUE) continue;

        uint32_t dropped = __atomic_load_n(&s_dropped, __ATOMIC_ACQUIRE);
        if (file && dropped != reported_drops) {
            scan_journal_record_t overflow = record;
            overflow.kind = SCAN_JOURNAL_QUEUE_OVERFLOW;
            overflow.session_id = open_session_id;
            overflow.sequence = last_written_sequence + 1;
            overflow.requested_hz = overflow.effective_hz = 0;
            overflow.effective_known = false;
            overflow.power_pct = -1;
            overflow.radio_error = 0;
            overflow.channel[0] = 0;
            snprintf(overflow.reason, sizeof(overflow.reason), "producer queue full");
            overflow.dropped_events = dropped - reported_drops;
            scan_journal_policy_note_drop(&policy, overflow.dropped_events);
            if (write_record(file, &policy, &overflow))
                last_written_sequence = overflow.sequence;
            reported_drops = dropped;
        }

        if (record.kind == SCAN_JOURNAL_SESSION_START) {
            if (file) fclose(file);
            scan_journal_policy_stop(&policy);
            open_session_id = record.session_id;
            last_written_sequence = 0;
            file = open_session(&record, &policy);
        }
        if (record.kind != SCAN_JOURNAL_SESSION_START &&
            record.session_id != open_session_id)
            continue;
        if (file && policy.storage_ok && write_record(file, &policy, &record))
            last_written_sequence = record.sequence;
        if (record.kind == SCAN_JOURNAL_SESSION_STOP) {
            if (file && fclose(file) != 0) {
                policy.write_errors++;
                policy.storage_ok = false;
            }
            file = NULL;
            open_session_id = 0;
            scan_journal_policy_stop(&policy);
            publish_policy(&policy, policy.storage_ok ? "session closed" :
                                                       "logging stopped; scanner continued");
        }
    }
}

static bool enqueue(const scan_journal_record_t *record)
{
    if (!record || !s_queue) return false;
    scan_journal_record_t copy = *record;
    copy.session_id = __atomic_load_n(&s_session_number, __ATOMIC_ACQUIRE);
    copy.sequence = __atomic_add_fetch(&s_sequence, 1, __ATOMIC_ACQ_REL);
    if (xQueueSend(s_queue, &copy, 0) == pdTRUE) return true;
    __atomic_add_fetch(&s_dropped, 1, __ATOMIC_ACQ_REL);
    portENTER_CRITICAL(&s_lock);
    s_status.queue_dropped = s_dropped;
    snprintf(s_status.detail, sizeof(s_status.detail), "event queue overflow; scanner continues");
    portEXIT_CRITICAL(&s_lock);
    return false;
}

void scan_journal_init(void)
{
    if (!s_queue) s_queue = xQueueCreate(JOURNAL_QUEUE_DEPTH, sizeof(scan_journal_record_t));
    if (s_queue && !s_task)
        (void)xTaskCreatePinnedToCore(journal_task, "scan_sd", JOURNAL_STACK,
                                     NULL, 1, &s_task, 0);
    portENTER_CRITICAL(&s_lock);
    s_status.initialized = s_queue && s_task;
    snprintf(s_status.detail, sizeof(s_status.detail), "%s",
             s_status.initialized ? "ready" : "worker unavailable; scanner continues");
    portEXIT_CRITICAL(&s_lock);
}

bool scan_journal_session_start(const scan_journal_record_t *record)
{
    if (!s_queue || !s_task) return false;
    (void)__atomic_add_fetch(&s_session_number, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&s_sequence, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_accepting, true, __ATOMIC_RELEASE);
    return enqueue(record);
}

bool scan_journal_emit(const scan_journal_record_t *record)
{
    if (!__atomic_load_n(&s_accepting, __ATOMIC_ACQUIRE)) return false;
    return enqueue(record);
}

bool scan_journal_session_stop(const scan_journal_record_t *record)
{
    if (!__atomic_exchange_n(&s_accepting, false, __ATOMIC_ACQ_REL)) return false;
    return enqueue(record);
}

void scan_journal_get_status(scan_journal_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}
