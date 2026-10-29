/* SD -> ESP-Hosted OTA. No UART, host reset or bootloader writes. */
#include "ls_c6_update.h"
#include "ls_c6_core.h"
#include "ls_ota.h"
#include "ls_board.h"
#include "ls_gauge.h"
#include "ls_wifi.h"
#include "ble_link.h"
#include "ls_task_reap.h"
#include "tui/ls_action.h"
#include "tui/ls_value.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "esp_hosted_event.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ls_textpack.h"

static bool s_busy, s_latched;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static ls_c6_result_t s_result;
static bool s_probe;
EXT_RAM_BSS_ATTR static char s_image[32];
EXT_RAM_BSS_ATTR static char s_path[192];
static bool s_force;
static unsigned s_boots, s_before_activate;
static esp_event_handler_instance_t s_event;
static bool s_reconnected;
static int s_reconnect_error;

static void report(void *ctx, const ls_c6_result_t *r)
{
    (void)ctx;
    static unsigned last_percent;
    unsigned percent = r->size ? (unsigned)(r->sent * 100 / r->size) : 0;
    portENTER_CRITICAL(&s_mux);
    s_result = *r;
    portEXIT_CRITICAL(&s_mux);
    if (r->stage == LS_C6_WRITE && percent == last_percent) return;
    last_percent = percent;
    printf("c6: %u/%u bytes (%u%%): %s\n", (unsigned)r->sent, (unsigned)r->size,
           percent, r->message);
}

static void preflight_failure(const char *message)
{
    ls_c6_result_t r = {.stage = LS_C6_FAILED};
    portENTER_CRITICAL(&s_mux);
    memcpy(r.old_version, s_result.old_version, sizeof(r.old_version));
    portEXIT_CRITICAL(&s_mux);
    snprintf(r.message, sizeof(r.message), "%s; old image still active", message);
    report(NULL, &r);
}

static int hash(const uint8_t *data, size_t size, uint8_t digest[32])
{
    return mbedtls_sha256(data, size, digest, 0);
}

static int power(void *ctx)
{
    (void)ctx;
#if LS_HAS_GAUGE
    ls_gauge_t gauge;
    int e = ls_gauge_read(&gauge); /* fresh read on worker, not cached UI data */
    if (e) return e;
    if (!gauge.present || gauge.percent < 20) return ESP_ERR_INVALID_STATE;
#endif
    return ESP_OK;
}

static int version(void *ctx, char out[32])
{
    (void)ctx;
    esp_hosted_coprocessor_fwver_t v = {0};
    int e = esp_hosted_get_coprocessor_fwversion(&v);
    if (!e) snprintf(out, 32, "%lu.%lu.%lu", (unsigned long)v.major1,
                     (unsigned long)v.minor1, (unsigned long)v.patch1);
    return e;
}

static int stop(void *ctx)
{
    (void)ctx;
    __atomic_store_n(&s_latched, true, __ATOMIC_RELEASE);
    /* Both gates must latch even if one shutdown fails. Wi-Fi's AP teardown
     * must never bring the BLE head back while OTA owns the coprocessor. */
    int b = ble_link_c6_quiesce();
    int w = ls_wifi_c6_quiesce();
    return b ? b : w;
}

static int begin(void *ctx) { (void)ctx; return esp_hosted_slave_ota_begin(); }
static int write_chunk(void *ctx, const uint8_t *data, size_t size)
{
    (void)ctx;
    return esp_hosted_slave_ota_write((uint8_t *)data, (uint32_t)size);
}
static int end(void *ctx) { (void)ctx; return esp_hosted_slave_ota_end(); }
static int activate(void *ctx)
{
    (void)ctx;
    s_before_activate = __atomic_load_n(&s_boots, __ATOMIC_ACQUIRE);
    return esp_hosted_slave_ota_activate();
}
static uint64_t millis(void *ctx) { (void)ctx; return (uint64_t)esp_timer_get_time() / 1000; }

static void boot_event(void *ctx, esp_event_base_t base, int32_t id, void *data)
{
    (void)ctx; (void)base; (void)id; (void)data;
    __atomic_add_fetch(&s_boots, 1u, __ATOMIC_RELEASE);
}

static void reconnect_worker(void *arg)
{
    (void)arg;
    /* Hosted's SDIO counters must be reset after slave reboot. All radio
     * clients are latched off and the NimBLE host has acknowledged stop.
     * This helper owns no caller-stack pointers: SDK cleanup/connect may
     * outlive the UI's deadline. Never kill it while it holds SDK locks. */
    int e = esp_hosted_deinit();
    if (!e) e = esp_hosted_init();
    if (!e) e = esp_hosted_connect_to_slave();
    s_reconnect_error = e;
    __atomic_store_n(&s_reconnected, true, __ATOMIC_RELEASE);
    ls_task_retire_self();
}

static int wait_boot(void *ctx, unsigned timeout_ms, char out[32])
{
    uint64_t until = millis(ctx) + timeout_ms;
    /* Let the slave's two-second activation timer expire before rebuilding
     * the link. This does not reset/restart P4 or resume any radio users. */
    vTaskDelay(pdMS_TO_TICKS(3000));
    if (xTaskCreateWithCaps(reconnect_worker, "c6_reconnect", 6144, NULL, 3, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    /* get_fwversion has a one-second RPC timeout. Reserve it in the budget.
     * An old live image answering before its 2-second restart is not success. */
    while (millis(ctx) + 1200 < until) {
        if (__atomic_load_n(&s_reconnected, __ATOMIC_ACQUIRE)) {
            if (s_reconnect_error) return s_reconnect_error;
            if (__atomic_load_n(&s_boots, __ATOMIC_ACQUIRE) != s_before_activate &&
                version(ctx, out) == ESP_OK) return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_ERR_TIMEOUT;
}

static void worker(void *arg)
{
    (void)arg;
    uint8_t *data = NULL;
    if (s_probe) {
        ls_c6_result_t r = {.stage = LS_C6_VALIDATE};
        int e = version(NULL, r.old_version);
        snprintf(r.message, sizeof(r.message), e ? "C6 version unavailable; inspecting SD" : "C6 version read; inspecting SD");
        report(NULL, &r);
    }
    FILE *file = fopen(s_path, "rb");
    if (!file) { preflight_failure("Cannot open SD image"); goto done; }
    setvbuf(file, NULL, _IONBF, 0);
    if (fseek(file, 0, SEEK_END)) { preflight_failure("Cannot seek SD image"); goto done; }
    long length = ftell(file);
    if (length < 24 || length > LS_C6_SLOT_SIZE) {
        preflight_failure("Invalid size or image exceeds 0x1e0000 slot"); goto done;
    }
    if (fseek(file, 0, SEEK_SET)) { preflight_failure("Cannot rewind SD image"); goto done; }
    data = heap_caps_malloc((size_t)length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) { preflight_failure("Not enough PSRAM for immutable image"); goto done; }
    for (size_t pos = 0; pos < (size_t)length;) {
        size_t n = (size_t)length - pos;
        if (n > LS_C6_CHUNK) n = LS_C6_CHUNK;
        if (fread(data + pos, 1, n, file) != n) { preflight_failure("SD read failed"); goto done; }
        pos += n;
        vTaskDelay(1);
    }
    if (fgetc(file) != EOF || ferror(file)) { preflight_failure("SD image changed/read failed"); goto done; }
    fclose(file); file = NULL;
    if (s_probe) {
        /* Read-only preflight shares the immutable SD snapshot and validator.
         * No stop/begin/write/end/activate callback is reachable here. */
        ls_c6_result_t r = {.stage = LS_C6_IDLE, .size = (size_t)length};
        ls_c6_image_t image;
        const char *why = ls_c6_validate(data, (size_t)length, LS_C6_SLOT_SIZE, hash, &image);
        int e = version(NULL, r.old_version);
        if (why) { preflight_failure(why); goto done; }
        portENTER_CRITICAL(&s_mux);
        snprintf(s_image, sizeof(s_image), "%s", image.version);
        portEXIT_CRITICAL(&s_mux);
        if (e) { preflight_failure("Cannot read running C6 version"); goto done; }
        if (power(NULL)) { preflight_failure("Battery below 20% or gauge unavailable"); goto done; }
        snprintf(r.message, sizeof(r.message), "SD / C6 target / SHA-256 / power checked. Install rechecks all inputs.");
        report(NULL, &r);
        goto done;
    }
    int e = esp_event_handler_instance_register(ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_CP_INIT,
                                                boot_event, NULL, &s_event);
    if (e) { preflight_failure("Cannot monitor C6 reboot"); goto done; }
    const ls_c6_transport_t io = {
        .power = power, .version = version, .stop = stop, .begin = begin,
        .write = write_chunk, .end = end, .activate = activate, .wait = wait_boot,
        .millis = millis, .report = report
    };
    ls_c6_result_t result;
    ls_c6_run(data, (size_t)length, s_force, hash, &io, &result);
    esp_event_handler_instance_unregister(ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_CP_INIT, s_event);
done:
    if (file) fclose(file);
    free(data);
    /* Keep the shared P4 updater lease after shutdown, including a late SDK
     * reconnect after timeout. Neither updater may run again before reboot. */
    if (!__atomic_load_n(&s_latched, __ATOMIC_ACQUIRE)) ls_ota_c6_release();
    __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
    ls_task_retire_self();
}

static ls_act_status_t start(const char *path, bool force, bool probe)
{
#if !LS_HAS_C6_OTA
    (void)path; (void)force; (void)probe;
    preflight_failure("C6 OTA requires provisioned T-Display-P4");
    return LS_ACT_UNAVAILABLE;
#else
    if (__atomic_exchange_n(&s_busy, true, __ATOMIC_ACQ_REL)) return LS_ACT_BUSY;
    if (__atomic_load_n(&s_latched, __ATOMIC_ACQUIRE)) {
        LS_PUTS(msg_ls_c6_update_c6_radios_inhibited_reboot, "c6: radios inhibited; reboot P4 before another update. See c6 status for boot state.");
        __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
        return LS_ACT_BUSY;
    }
    if (probe) { portENTER_CRITICAL(&s_mux); s_image[0] = 0; portEXIT_CRITICAL(&s_mux); }
    if (!path || !*path) path = LS_C6_DEFAULT_PATH;
    if (strlen(path) >= sizeof(s_path) || strncmp(path, "/sdcard/", 8) || strstr(path, "/../")) {
        preflight_failure("Use a /sdcard/ path under 192 bytes, without ..");
        __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
        return LS_ACT_BADARG;
    }
    snprintf(s_path, sizeof(s_path), "%s", path);
    if (!ls_ota_c6_acquire()) {
        preflight_failure("P4 updater busy or P4 image still on trial");
        __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
        return LS_ACT_BUSY;
    }
    s_force = force;
    s_probe = probe;
    ls_c6_result_t r = {.stage = LS_C6_VALIDATE};
    snprintf(r.message, sizeof(r.message), "Reading SD image; validating before C6 writes");
    report(NULL, &r);
    if (xTaskCreateWithCaps(worker, "c6_update", 6144, NULL, 3, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        preflight_failure("Cannot allocate PSRAM worker");
        ls_ota_c6_release();
        __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
        return LS_ACT_FAILED;
    }
    return LS_ACT_OK;
#endif
}

static bool state(ls_val_t *out)
{
    EXT_RAM_BSS_ATTR static char text[240]; /* only read by UI/value caller, worker never writes it */
    ls_c6_result_t r;
    portENTER_CRITICAL(&s_mux);
    r = s_result;
    portEXIT_CRITICAL(&s_mux);
    snprintf(text, sizeof(text), "%u/%u bytes (%u%%) | %s", (unsigned)r.sent,
             (unsigned)r.size, r.size ? (unsigned)(r.sent * 100 / r.size) : 0,
             r.message[0] ? r.message : "Ready: SD image, provisioned C6 layout required");
    *out = (ls_val_t){.kind = LS_VAL_TEXT, .s = text};
    return true;
}
static bool busy(ls_val_t *out)
{
    *out = (ls_val_t){.kind = LS_VAL_BOOL,
        .i = __atomic_load_n(&s_busy, __ATOMIC_ACQUIRE) || __atomic_load_n(&s_latched, __ATOMIC_ACQUIRE)};
    return true;
}
static bool percent(ls_val_t *out)
{
    portENTER_CRITICAL(&s_mux);
    long value = s_result.size ? (long)(s_result.sent * 100 / s_result.size) : 0;
    portEXIT_CRITICAL(&s_mux);
    *out = (ls_val_t){.kind = LS_VAL_INT, .i = value};
    return true;
}
static ls_act_status_t action(const ls_args_t *in, ls_val_t *out)
{
    (void)out;
    return start(in->v[0].s, in->v[1].i != 0, false);
}
/* Bounded cache reads only; SD and Hosted RPC stay on c6_update. */
#define C6_NUMBER(fn, expr) \
static bool fn(ls_val_t *out) { \
    portENTER_CRITICAL(&s_mux); long v = (expr); portEXIT_CRITICAL(&s_mux); \
    *out = (ls_val_t){.kind = LS_VAL_INT, .i = v}; return true; }
C6_NUMBER(stage_value, s_result.message[0] ? (long)s_result.stage : LS_C6_IDLE)
C6_NUMBER(sent_value, s_result.sent)
C6_NUMBER(size_value, s_result.size)
C6_NUMBER(boot_value, s_result.boot)
#define C6_TEXT(fn, field) \
static bool fn(ls_val_t *out) { \
    EXT_RAM_BSS_ATTR static char text[32]; portENTER_CRITICAL(&s_mux); \
    snprintf(text, sizeof(text), "%s", field); portEXIT_CRITICAL(&s_mux); \
    *out = (ls_val_t){.kind = LS_VAL_TEXT, .s = text}; return true; }
C6_TEXT(old_value, s_result.old_version)
C6_TEXT(new_value, s_result.new_version)
C6_TEXT(image_value, s_image)
static ls_act_status_t inspect(const ls_args_t *in, ls_val_t *out)
{
    (void)out;
    return start(in->v[0].s, false, true);
}
void ls_c6_publish(void)
{
    ls_value_publish("c6.stage", NULL, stage_value);
    ls_value_publish("c6.sent", "B", sent_value);
    ls_value_publish("c6.size", "B", size_value);
    ls_value_publish("c6.boot", NULL, boot_value);
    ls_value_publish("c6.old", NULL, old_value);
    ls_value_publish("c6.new", NULL, new_value);
    ls_value_publish("c6.image", NULL, image_value);
    ls_action_register("c6.inspect", "s", LS_CAP_STORE | LS_CAP_POWER, inspect,
                       "Read-only SD image and C6 preflight");
    ls_value_publish("c6.state", NULL, state);
    ls_value_publish("c6.busy", NULL, busy);
    ls_value_publish("c6.percent", "%", percent);
    ls_action_register("c6.update", "sb", LS_CAP_STORE | LS_CAP_POWER, action,
                       "Update C6 from SD (path, force); radios stay off until P4 reboot");
}

int ls_c6_update_command(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "status")) {
        ls_c6_result_t r;
        portENTER_CRITICAL(&s_mux); r = s_result; portEXIT_CRITICAL(&s_mux);
        printf("c6: %u/%u bytes (%u%%): %s\n", (unsigned)r.sent, (unsigned)r.size,
               r.size ? (unsigned)(r.sent * 100 / r.size) : 0,
               r.message[0] ? r.message : "Idle; no update attempted");
        return 0;
    }
    if (argc >= 2 && argc <= 4 && !strcmp(argv[1], "update")) {
        const char *path = argc >= 3 ? argv[2] : LS_C6_DEFAULT_PATH;
        bool force = argc == 4 && !strcmp(argv[3], "force");
        if (argc == 4 && !force) goto usage;
        if (argc == 3 && !strcmp(path, "force")) { path = LS_C6_DEFAULT_PATH; force = true; }
        ls_act_status_t rc = start(path, force, false);
        printf("c6: %s\n", rc == LS_ACT_OK ? "update queued; c6 status shows progress" : ls_act_status_str(rc));
        return rc == LS_ACT_OK ? 0 : 1;
    }
usage:
    puts("c6 update [" LS_C6_DEFAULT_PATH "] [force] | c6 status\n"
         "Recovery: USB-UART on C6 header; see docs/TDP4_FIRST_FLASH.md");
    return 1;
}
