/* The error log's chip side against fakes: the log hook (every line still
   reaches the console, only errors and warnings are kept), the boot step
   (which resets are saved, with the run before's last words), and the
   records' trip through NVS. */

#include "ls_test.h"
#include "ls_errlog.h"
#include "ls_nvs_safe.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------- the console */

static char s_console[8192];

static int console_vprintf(const char *fmt, va_list ap)
{
    const size_t used = strlen(s_console);
    return vsnprintf(s_console + used, sizeof(s_console) - used, fmt, ap);
}

static vprintf_like_t s_vprintf = console_vprintf;

vprintf_like_t esp_log_set_vprintf(vprintf_like_t func)
{
    const vprintf_like_t old = s_vprintf;
    s_vprintf = func;
    return old;
}

void esp_log_write(esp_log_level_t level, const char *tag, const char *format, ...)
{
    (void)level; (void)tag;
    va_list ap;
    va_start(ap, format);
    s_vprintf(format, ap);
    va_end(ap);
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle)) n++;
    return n;
}

/* ------------------------------------------------- the rest of the chip */

static esp_reset_reason_t s_reason = ESP_RST_POWERON;
esp_reset_reason_t esp_reset_reason(void) { return s_reason; }
void esp_restart(void) {}

static const esp_app_desc_t s_desc = { .version = "2.8.2-bench" };
const esp_app_desc_t *esp_app_get_description(void) { return &s_desc; }

static bool s_dump;
bool ls_crash_present(void) { return s_dump; }

esp_err_t ls_nvs_run(ls_nvs_fn_t fn, void *ctx, unsigned stack_bytes)
{
    (void)stack_bytes;
    return fn(ctx);
}

/* ---------------------------------------------------------------- NVS */

typedef struct {
    char key[16];
    unsigned char data[1400];
    size_t len;
    bool used;
} kv_t;

static kv_t s_kv[16];
static bool s_ns_made;               /* the namespace exists once written */

static void nvs_wipe(void)
{
    memset(s_kv, 0, sizeof(s_kv));
    s_ns_made = false;
}

static kv_t *find(const char *key)
{
    for (int i = 0; i < 16; i++)
        if (s_kv[i].used && !strcmp(s_kv[i].key, key)) return &s_kv[i];
    return NULL;
}

static kv_t *make(const char *key)
{
    kv_t *k = find(key);
    for (int i = 0; !k && i < 16; i++)
        if (!s_kv[i].used) k = &s_kv[i];
    if (!k) return NULL;
    k->used = true;
    snprintf(k->key, sizeof(k->key), "%s", key);
    s_ns_made = true;
    return k;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (strcmp(name, "errlog")) return ESP_FAIL;
    if (mode == NVS_READONLY && !s_ns_made) return ESP_ERR_NVS_NOT_FOUND;
    *out = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)h;
    const kv_t *k = find(key);
    if (!k) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < k->len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, k->data, k->len);
    *len = k->len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len)
{
    (void)h;
    kv_t *k = make(key);
    if (!k || len > sizeof(k->data)) return ESP_FAIL;
    memcpy(k->data, value, len);
    k->len = len;
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out)
{
    (void)h;
    const kv_t *k = find(key);
    if (!k) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(out, k->data, sizeof(*out));
    return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t value)
{
    return nvs_set_blob(h, key, &value, sizeof(value));
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h;
    kv_t *k = find(key);
    if (!k) return ESP_ERR_NVS_NOT_FOUND;
    k->used = false;
    return ESP_OK;
}

/* ------------------------------------------------------------- a boot */

/* What app_main does, as far as the error log is concerned. */
static void boot(esp_reset_reason_t why, bool panicked, const char *crumb)
{
    s_reason = why;
    s_console[0] = '\0';
    ls_errlog_early();
    ls_errlog_boot(panicked, crumb);
}

LS_CASE(every_line_still_reaches_the_console_and_only_errors_and_warnings_are_kept)
{
    nvs_wipe();
    boot(ESP_RST_POWERON, false, "");
    s_console[0] = '\0';
    ESP_LOGI("tune", "info %d", 1);
    ESP_LOGW("p25", "stream quiet %d s", 2);
    ESP_LOGE("usb", "transfer failed %d", 3);
    LS_CHECK(strstr(s_console, "I (0) tune: info 1\n") != NULL);
    LS_CHECK(strstr(s_console, "W (0) p25: stream quiet 2 s\n") != NULL);
    LS_CHECK(strstr(s_console, "E (0) usb: transfer failed 3\n") != NULL);
    char live[512];
    ls_errlog_live(live, sizeof(live));
    LS_EQ_STR(live, "W (0) p25: stream quiet 2 s\nE (0) usb: transfer failed 3\n");
}

LS_CASE(a_second_boot_never_hooks_the_log_to_itself)
{
    nvs_wipe();
    boot(ESP_RST_POWERON, false, "");
    boot(ESP_RST_SW, false, "");
    s_console[0] = '\0';
    ESP_LOGE("once", "printed once");
    LS_EQ_INT(count_of(s_console, "printed once"), 1);
}

LS_CASE(a_restart_that_was_asked_for_saves_nothing)
{
    nvs_wipe();
    boot(ESP_RST_POWERON, false, "");
    ESP_LOGE("x", "an error before a restart");
    boot(ESP_RST_SW, false, "");
    LS_EQ_INT(ls_errlog_count(), 0);
    boot(ESP_RST_POWERON, false, "");
    LS_EQ_INT(ls_errlog_count(), 0);
}

LS_CASE(a_watchdog_reset_is_saved_with_the_run_before_s_last_words)
{
    nvs_wipe();
    s_dump = true;                            /* an older dump, not this reset's */
    boot(ESP_RST_POWERON, false, "");
    ESP_LOGW("p25", "stream quiet");
    ESP_LOGE("usb", "transfer stuck");
    boot(ESP_RST_TASK_WDT, false, "");
    ESP_LOGE("new", "this run, not the one before");
    LS_EQ_INT(ls_errlog_count(), 1);
    ls_errlog_rec_t r;
    LS_CHECK(ls_errlog_get(0, &r));
    LS_EQ_STR(r.reason, "task watchdog");
    LS_EQ_STR(r.fw, "2.8.2-bench");
    LS_EQ_STR(r.tail, "W (0) p25: stream quiet\nE (0) usb: transfer stuck\n");
    LS_EQ_STR(r.dump, "");
    LS_CHECK(r.wall > 0);
    LS_CHECK(strstr(s_console, "saved as error #1") != NULL);
    s_dump = false;
}

LS_CASE(a_panic_names_its_dump_and_keeps_the_crumb)
{
    nvs_wipe();
    s_dump = true;
    boot(ESP_RST_POWERON, false, "");
    boot(ESP_RST_PANIC, true, "core1 int mcause 8000001e pc 4ff0abcd tval 0 ra 4ff0abcd sp 4ff3a000\n");
    ls_errlog_rec_t r;
    LS_CHECK(ls_errlog_get(0, &r));
    LS_EQ_STR(r.reason, "panic");
    LS_CHECK(strstr(r.crumb, "core1 int mcause 8000001e") != NULL);
    LS_CHECK(r.dump[0] != '\0');
    s_dump = false;
}

LS_CASE(a_panic_only_the_crumb_saw_is_still_saved)
{
    nvs_wipe();
    boot(ESP_RST_POWERON, false, "");
    boot(ESP_RST_SW, true, "core0 exc mcause 7 pc 40000000 tval 0 ra 0 sp 0\n");
    LS_EQ_INT(ls_errlog_count(), 1);
    ls_errlog_rec_t r;
    LS_CHECK(ls_errlog_get(0, &r));
    LS_EQ_STR(r.reason, "panic");
}

LS_CASE(records_come_back_after_a_power_cycle_and_a_clear_empties_them)
{
    nvs_wipe();
    boot(ESP_RST_POWERON, false, "");
    boot(ESP_RST_BROWNOUT, false, "");
    boot(ESP_RST_INT_WDT, false, "");
    boot(ESP_RST_POWERON, false, "");          /* the slots are flash, not RTC */
    LS_EQ_INT(ls_errlog_count(), 2);
    ls_errlog_rec_t r;
    LS_CHECK(ls_errlog_get(0, &r));
    LS_EQ_STR(r.reason, "interrupt watchdog");
    LS_CHECK(ls_errlog_get(1, &r));
    LS_EQ_STR(r.reason, "brownout");
    LS_CHECK(find("r0") != NULL && find("r1") != NULL && find("seq") != NULL);
    LS_CHECK(ls_errlog_clear());
    LS_EQ_INT(ls_errlog_count(), 0);
    LS_CHECK(find("r0") == NULL && find("r1") == NULL);
}

LS_CASE(a_record_from_a_build_with_another_layout_reads_as_unreadable)
{
    nvs_wipe();
    boot(ESP_RST_POWERON, false, "");
    boot(ESP_RST_WDT, false, "");
    kv_t *k = find("r0");
    LS_CHECK(k != NULL);
    k->len -= 8;                              /* an older, smaller record */
    ls_errlog_rec_t r;
    LS_CHECK(!ls_errlog_get(0, &r));
}
