/* The error log on the chip: last words in RTC memory (a reset does not
   clear it), a hook on the log for the error and warning lines, the records
   in NVS (namespace "errlog"), and the boot step that writes one. */

#include "ls_errlog.h"
#include "ls_crash.h"
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

static const char *TAG = "errlog";
#define NS "errlog"

/* ------------------------------------------------------------ last words */

RTC_NOINIT_ATTR static ls_errlog_tail_t s_rtc_tail;
/* Internal RAM is the scarce kind here; nothing below needs it. */
EXT_RAM_BSS_ATTR static char s_prev[LS_ERRLOG_REC_TAIL];   /* the run before's */
EXT_RAM_BSS_ATTR static char s_line[192];
static portMUX_TYPE s_tail_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_line_mutex;
static StaticSemaphore_t s_line_mutex_buf;
static vprintf_like_t s_next_vprintf;

/* Every log line passes here. Error and warning lines (the format begins
   "E (" or "W (" with colours off) are copied into the ring first; a line
   arriving while another is being copied is not kept, rather than wait. */
static int errlog_vprintf(const char *fmt, va_list args)
{
    if (fmt && (fmt[0] == 'E' || fmt[0] == 'W') && fmt[1] == ' ' && fmt[2] == '(' &&
        s_line_mutex && xSemaphoreTake(s_line_mutex, 0) == pdTRUE) {
        va_list copy;
        va_copy(copy, args);
        vsnprintf(s_line, sizeof(s_line), fmt, copy);
        va_end(copy);
        portENTER_CRITICAL_SAFE(&s_tail_lock);
        ls_errlog_tail_add(&s_rtc_tail, s_line);
        portEXIT_CRITICAL_SAFE(&s_tail_lock);
        xSemaphoreGive(s_line_mutex);
    }
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
        snprintf(rec.dump, sizeof(rec.dump), "coredump written: 'crash' reads it");
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
    printf("errors saved: %d of %lu since the last clear (newest first)\n", n,
           (unsigned long)ls_errlog_total());
    EXT_RAM_BSS_ATTR static ls_errlog_rec_t rec;
    EXT_RAM_BSS_ATTR static char text[1200];
    for (int i = 0; i < n; i++) {
        if (!ls_errlog_get(i, &rec)) {
            printf("(record %d unreadable)\n", i);
            continue;
        }
        ls_errlog_format(&rec, text, sizeof(text));
        fputs(text, stdout);
        fputs("\n", stdout);
    }
}
