/* The error log on the chip: last words in RTC memory (a reset does not
   clear it), a hook on the log for the error and warning lines, the records
   in NVS (namespace "errlog"), and the boot step that writes one. */

#include "ls_errlog.h"
#include "ls_crash.h"
#include "ls_log_line.h"
#include "ls_nvs_safe.h"
#include "ls_trail.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "ls_textpack.h"

static const char *TAG = "errlog";
#define NS "errlog"

/* ------------------------------------------------------------ last words */

RTC_NOINIT_ATTR static ls_errlog_tail_t s_rtc_tail;
/* Internal RAM is the scarce kind here; nothing below needs it. */
EXT_RAM_BSS_ATTR static char s_prev[LS_ERRLOG_REC_TAIL];   /* the run before's */
EXT_RAM_BSS_ATTR static char s_line[192];
static size_t s_line_len;
static bool s_line_open;                /* s_line_task holds s_line_mutex */
static TaskHandle_t s_line_task;
static ls_log_line_t s_line_of;
static portMUX_TYPE s_tail_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_line_mutex;
static StaticSemaphore_t s_line_mutex_buf;
static vprintf_like_t s_next_vprintf;

/* One call's share of an error or warning line, into s_line. Under Log V2
   the line comes in pieces (ls_log_line.h): the task whose prefix took the
   mutex keeps it until its newline, and the whole line goes into the ring. */
static void keep_piece(const char *fmt, va_list args)
{
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();
    const bool mine = s_line_open && s_line_task == me;
    if (ls_log_line_start(fmt)) {
        if (!mine) {
            if (!s_line_mutex || xSemaphoreTake(s_line_mutex, 0) != pdTRUE) return;
            s_line_open = true;
            s_line_task = me;
        }
        s_line_len = 0;
    } else if (!mine) {
        return;
    }
    const size_t room = sizeof(s_line) - s_line_len;
    va_list copy;
    va_copy(copy, args);
    const int n = vsnprintf(s_line + s_line_len, room, fmt, copy);
    va_end(copy);
    if (n > 0) s_line_len += (size_t)n < room ? (size_t)n : room - 1;
    const bool ended = s_line_len && s_line[s_line_len - 1] == '\n';
    if (!LS_LOG_LINE_IN_PIECES || ended || s_line_len >= sizeof(s_line) - 1) {
        portENTER_CRITICAL_SAFE(&s_tail_lock);
        ls_errlog_tail_add(&s_rtc_tail, s_line);
        portEXIT_CRITICAL_SAFE(&s_tail_lock);
        s_line_open = false;
        xSemaphoreGive(s_line_mutex);
    }
}

/* Every log line passes here. Error and warning lines are copied into the
   ring first; a line beginning while another is being copied is not kept,
   rather than wait. */
static int errlog_vprintf(const char *fmt, va_list args)
{
    const char level = ls_log_line_level(&s_line_of, fmt, args);
    if (level == 'E' || level == 'W') keep_piece(fmt, args);
    return s_next_vprintf ? s_next_vprintf(fmt, args) : vprintf(fmt, args);
}

void ls_errlog_early(void)
{
    if (ls_errlog_tail_valid(&s_rtc_tail))
        ls_errlog_tail_copy(&s_rtc_tail, s_prev, sizeof(s_prev));
    else
        s_prev[0] = '\0';
    ls_errlog_tail_reset(&s_rtc_tail);
    if (!s_line_mutex) s_line_mutex = xSemaphoreCreateMutexStatic(&s_line_mutex_buf);
    const vprintf_like_t prev = esp_log_set_vprintf(errlog_vprintf);
    if (prev != errlog_vprintf) s_next_vprintf = prev;     /* never itself */
}

size_t ls_errlog_live(char *out, size_t n)
{
    portENTER_CRITICAL_SAFE(&s_tail_lock);
    const size_t k = ls_errlog_tail_copy(&s_rtc_tail, out, n);
    portEXIT_CRITICAL_SAFE(&s_tail_lock);
    return k;
}

/* ------------------------------------------------------------- NVS store */

typedef struct { int slot; ls_errlog_rec_t *rec; const ls_errlog_rec_t *crec; uint32_t seq; bool ok; } job_t;

static void key_of(int slot, char key[8]) { snprintf(key, 8, "r%d", slot); }

static esp_err_t load_job(void *ctx)
{
    job_t *j = ctx;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_FAIL;
    char key[8];
    key_of(j->slot, key);
    size_t len = sizeof(*j->rec);
    j->ok = nvs_get_blob(h, key, j->rec, &len) == ESP_OK && len == sizeof(*j->rec);
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t save_job(void *ctx)
{
    job_t *j = ctx;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    char key[8];
    key_of(j->slot, key);
    j->ok = nvs_set_blob(h, key, j->crec, sizeof(*j->crec)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t erase_job(void *ctx)
{
    job_t *j = ctx;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    char key[8];
    key_of(j->slot, key);
    const esp_err_t e = nvs_erase_key(h, key);
    j->ok = (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t get_seq_job(void *ctx)
{
    job_t *j = ctx;
    nvs_handle_t h;
    j->seq = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;   /* nothing yet */
    nvs_get_u32(h, "seq", &j->seq);
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t set_seq_job(void *ctx)
{
    job_t *j = ctx;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    j->ok = nvs_set_u32(h, "seq", j->seq) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ESP_OK;
}

static bool st_load(int slot, ls_errlog_rec_t *out)
{
    job_t j = { .slot = slot, .rec = out };
    return ls_nvs_run(load_job, &j, 0) == ESP_OK && j.ok;
}
static bool st_save(int slot, const ls_errlog_rec_t *rec)
{
    job_t j = { .slot = slot, .crec = rec };
    return ls_nvs_run(save_job, &j, 0) == ESP_OK && j.ok;
}
static bool st_erase(int slot)
{
    job_t j = { .slot = slot };
    return ls_nvs_run(erase_job, &j, 0) == ESP_OK && j.ok;
}
static uint32_t st_get_seq(void)
{
    job_t j = { 0 };
    ls_nvs_run(get_seq_job, &j, 0);
    return j.seq;
}
static bool st_set_seq(uint32_t seq)
{
    job_t j = { .seq = seq };
    return ls_nvs_run(set_seq_job, &j, 0) == ESP_OK && j.ok;
}

static const ls_errlog_store_t s_nvs_store = { st_load, st_save, st_erase, st_get_seq, st_set_seq };

/* ------------------------------------------------------------------ boot */

static const char *reset_name(esp_reset_reason_t r, bool *bad)
{
    *bad = true;
    switch (r) {
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "hardware watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    default:               *bad = false; return "normal";
    }
}

void ls_errlog_boot(bool panicked, const char *crumb)
{
    ls_errlog_init(&s_nvs_store);
    bool bad = false;
    const char *why = reset_name(esp_reset_reason(), &bad);
    /* the panic handler ran even if the reset reads otherwise */
    if (!bad && !panicked) return;

    EXT_RAM_BSS_ATTR static ls_errlog_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    const time_t now = time(NULL);
    rec.wall = now > 1700000000 ? (int64_t)now : 0;      /* 0 until the RTC set the clock */
    snprintf(rec.fw, sizeof(rec.fw), "%s", esp_app_get_description()->version);
    snprintf(rec.reason, sizeof(rec.reason), "%s", bad ? why : "panic");
    if (crumb) snprintf(rec.crumb, sizeof(rec.crumb), "%s", crumb);
    ls_trail_text(rec.trail, sizeof(rec.trail));
    /* only a panic writes a dump; after a watchdog reset a stored one is older */
    if (esp_reset_reason() == ESP_RST_PANIC && ls_crash_present())
        LS_SNPUTS(t_ls_errlog_esp_coredump_written_crash_reads, rec.dump, sizeof(rec.dump), "coredump written: 'crash' reads it");
    snprintf(rec.tail, sizeof(rec.tail), "%s", s_prev);
    if (ls_errlog_save(&rec))
        ESP_LOGW(TAG, "the run before ended (%s): saved as error #%lu ('crumb log', DIAG)",
                 rec.reason, (unsigned long)rec.seq);
    else
        ESP_LOGE(TAG, "the run before ended (%s) and the record could not be saved", rec.reason);
}

void ls_errlog_print_all(void)
{
    const int n = ls_errlog_count();
    LS_PRINTF(t_ls_errlog_esp_errors_saved_d_of, "errors saved: %d of %lu since the last clear (newest first)\n", n,
           (unsigned long)ls_errlog_total());
    EXT_RAM_BSS_ATTR static ls_errlog_rec_t rec;
    EXT_RAM_BSS_ATTR static char text[1200];
    for (int i = 0; i < n; i++) {
        if (!ls_errlog_get(i, &rec)) {
            LS_PRINTF(t_ls_errlog_esp_record_d_unreadable, "(record %d unreadable)\n", i);
            continue;
        }
        ls_errlog_format(&rec, text, sizeof(text));
        fputs(text, stdout);
        fputs("\n", stdout);
    }
}
