/* SD -> ESP-Hosted OTA. No UART, host reset or bootloader writes. */
#include "ls_c6_update.h"
#include "ls_c6_core.h"
#include "ls_ota.h"
#include "ls_board.h"
#include "ls_gauge.h"
#include "ls_wifi.h"
#include "ls_fetch.h"
#include <sys/stat.h>
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
#include "esp_system.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ls_textpack.h"

static bool s_busy, s_latched, s_auto, s_restart_after;
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
    if (r->stage <= LS_C6_WRITE && r->sent && percent == last_percent) return;
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
    if (s_restart_after && result.stage == LS_C6_DONE) {
        /* Radios stay latched off until a P4 reboot; restart so Wi-Fi and BLE return. */
        puts("c6: new radio firmware verified; restarting the board in 4 s");
        vTaskDelay(pdMS_TO_TICKS(4000));
        esp_restart();
    }
done:
    if (file) fclose(file);
    free(data);
    /* Keep the shared P4 updater lease after shutdown, including a late SDK
     * reconnect after timeout. Neither updater may run again before reboot. */
    if (!__atomic_load_n(&s_latched, __ATOMIC_ACQUIRE)) ls_ota_c6_release();
    __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
    ls_task_retire_self();
}

/* `restart`: reboot the board after a good write, so Wi-Fi and BLE return. */
static ls_act_status_t start(const char *path, bool force, bool probe, bool restart)
{
#if !LS_HAS_C6_OTA
    (void)path; (void)force; (void)probe; (void)restart;
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
    s_restart_after = restart;
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

/* ---- Automatic update: no cable, no manual SD copy ------------------------
   Image from the card if one is there, else downloaded (Wi-Fi) to the card
   from the signed manifest at data.terminalbay.com/c6/. The whole file is
   on the card, sha256-checked against the signed manifest, before the first
   byte goes to the C6; the install itself is start() above, unchanged. */
#define C6_MANIFEST "https://data.terminalbay.com/c6/manifest.txt"
#define C6_BIN_URL  "https://data.terminalbay.com/c6/network_adapter.bin"

static void auto_say(ls_c6_stage_t st, size_t sent, size_t size, const char *msg)
{
    ls_c6_result_t r = {.stage = st, .sent = sent, .size = size};
    portENTER_CRITICAL(&s_mux);
    memcpy(r.old_version, s_result.old_version, sizeof(r.old_version));
    portEXIT_CRITICAL(&s_mux);
    snprintf(r.message, sizeof(r.message), "%s", msg);
    report(NULL, &r);
}
static void dl_progress(uint32_t got, uint32_t total, void *ctx)
{
    (void)ctx;
    auto_say(LS_C6_VALIDATE, got, total, "Downloading C6 firmware. Keep power on.");
}
static bool plain_version(const char *v)
{
    unsigned a, b, c; char t;
    return sscanf(v, "%u.%u.%u%c", &a, &b, &c, &t) == 3;
}
static bool hex32(const char *h, uint8_t out[32])
{
    for (int i = 0; i < 32; ++i) {
        unsigned v;
        char two[3] = {h[2 * i], h[2 * i + 1], 0};
        if (!two[0] || !two[1] || sscanf(two, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}
/* Version of the validated image on the card, "" when none or not valid. */
static void sd_image_version(char out[32])
{
    out[0] = 0;
    struct stat st;
    if (stat(LS_C6_DEFAULT_PATH, &st) || st.st_size < 24 || st.st_size > (long)LS_C6_SLOT_SIZE) return;
    uint8_t *d = heap_caps_malloc((size_t)st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    FILE *f = d ? fopen(LS_C6_DEFAULT_PATH, "rb") : NULL;
    if (f && fread(d, 1, (size_t)st.st_size, f) == (size_t)st.st_size) {
        ls_c6_image_t img;
        if (!ls_c6_validate(d, (size_t)st.st_size, LS_C6_SLOT_SIZE, hash, &img)) snprintf(out, 32, "%s", img.version);
    }
    if (f) fclose(f);
    free(d);
}
static const char *manifest_field(const char *m, const char *key, char *out, size_t cap)
{
    size_t k = strlen(key);
    while (m && *m) {
        if (!strncmp(m, key, k) && m[k] == '=') {
            size_t n = strcspn(m + k + 1, "\r\n");
            if (n >= cap) return NULL;
            memcpy(out, m + k + 1, n); out[n] = 0;
            return out;
        }
        m = strchr(m, '\n');
        if (m) ++m;
    }
    return NULL;
}

static void auto_worker(void *arg)
{
    (void)arg;
    char run[32] = "", target[32] = "", num[16], sha[80];
    char *man = NULL;
    uint32_t size = 0;
    uint8_t digest[32];
    bool force = false;
    int e = version(NULL, run);
    if (e) { auto_say(LS_C6_FAILED, 0, 0, "Cannot read the C6 version; is the C6 up? Nothing changed."); goto out; }
    portENTER_CRITICAL(&s_mux);
    memcpy(s_result.old_version, run, sizeof(run));
    portEXIT_CRITICAL(&s_mux);
    auto_say(LS_C6_VALIDATE, 0, 0, "Checking the C6 radio firmware");
    sd_image_version(target);
    bool have_sd = target[0] != 0;
    if (!have_sd) {
        if (!ls_fetch_online()) {
            auto_say(LS_C6_FAILED, 0, 0, "No Wi-Fi and no image on the SD card. Join Wi-Fi in SETTINGS, or copy network_adapter.bin to /sdcard/lakeshark/c6/. Nothing changed.");
            goto out;
        }
        man = heap_caps_calloc(1, 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ls_fetch_req_t q = {.url = C6_MANIFEST, .buf = man, .cap = 1024, .timeout_s = 30, .flags = LS_FETCH_SIGNED};
        ls_fetch_result_t fr = man ? ls_fetch_run(&q, NULL, NULL) : LS_FETCH_SD;
        if (fr != LS_FETCH_OK || !manifest_field(man, "version", target, sizeof(target)) ||
            !manifest_field(man, "size", num, sizeof(num)) || !manifest_field(man, "sha256", sha, sizeof(sha)) ||
            (size = (uint32_t)strtoul(num, NULL, 10)) < 24 || size > LS_C6_SLOT_SIZE || !hex32(sha, digest)) {
            auto_say(LS_C6_FAILED, 0, 0, fr == LS_FETCH_OK ? "Manifest unusable. Nothing changed." :
                     "Cannot fetch the signed C6 manifest. Nothing changed.");
            goto out;
        }
    }
    if (!strcmp(run, target)) { auto_say(LS_C6_IDLE, 0, 0, "C6 radio firmware is up to date"); goto out; }
    if (!ls_c6_version_allowed(target, run, false)) {
        if (plain_version(run)) { auto_say(LS_C6_IDLE, 0, 0, "C6 radio firmware is newer than the bundled image; left alone"); goto out; }
        force = true; /* factory or unnumbered build: the bundled image replaces it */
    }
    if (!have_sd) {
        ls_fetch_req_t q = {.url = C6_BIN_URL, .dest = LS_C6_DEFAULT_PATH, .bytes = size, .sha256 = digest,
                            .timeout_s = 300, .flags = LS_FETCH_RESUME, .progress = dl_progress};
        ls_fetch_result_t fr = ls_fetch_run(&q, NULL, NULL);
        if (fr != LS_FETCH_OK) {
            char m[96];
            snprintf(m, sizeof(m), "Download failed: %s. Nothing changed; try again.", ls_fetch_result_text(fr));
            auto_say(LS_C6_FAILED, 0, 0, m);
            goto out;
        }
    }
    /* Image is whole and verified on the card. Hand over to the normal install. */
    ls_act_status_t rc = start(LS_C6_DEFAULT_PATH, force, false, true);
    if (rc != LS_ACT_OK) {
        auto_say(LS_C6_FAILED, 0, 0, "Install did not start (busy or battery). Image kept on the SD card.");
    }
out:
    free(man);
    s_auto = false;
    ls_task_retire_self();
}

static ls_act_status_t auto_start(void)
{
#if !LS_HAS_C6_OTA
    return LS_ACT_UNAVAILABLE;
#else
    if (s_busy || s_auto || __atomic_load_n(&s_latched, __ATOMIC_ACQUIRE)) return LS_ACT_BUSY;
    s_auto = true;
    if (xTaskCreateWithCaps(auto_worker, "c6_auto", 12288, NULL, 3, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_auto = false;
        return LS_ACT_FAILED;
    }
    return LS_ACT_OK;
#endif
}
static ls_act_status_t auto_action(const ls_args_t *in, ls_val_t *out)
{
    (void)in; (void)out;
    return auto_start();
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
        .i = __atomic_load_n(&s_busy, __ATOMIC_ACQUIRE) || s_auto || __atomic_load_n(&s_latched, __ATOMIC_ACQUIRE)};
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
    return start(in->v[0].s, in->v[1].i != 0, false, false);
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
    return start(in->v[0].s, false, true, false);
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
    ls_action_register("c6.auto", "", LS_CAP_STORE | LS_CAP_POWER, auto_action,
                       "Update the C6 radio firmware by itself (SD image, else download); restarts after");
    ls_action_register("c6.update", "sb", LS_CAP_STORE | LS_CAP_POWER, action,
                       "Update C6 from SD (path, force); radios stay off until P4 reboot");
}

/* c6 flash PATH: stream an ESP-Hosted coprocessor image from the card with
   the SDK's own slave OTA, for boards without the T-Display updater (the
   Nano's ESP32-C5). Checks the image is for the coprocessor this build
   talks to, then activates it and restarts the board so both ends start
   clean. Blocking, on the console task; a dev and bench tool. */
#if defined(CONFIG_ESP_HOSTED_CP_TARGET_ESP32C5)
#define COP_CHIP_ID 23
#define COP_NAME "ESP32-C5"
#else
#define COP_CHIP_ID 13
#define COP_NAME "ESP32-C6"
#endif
static int cop_flash(const char *path)
{
    if (strncmp(path, "/sdcard/", 8) || strstr(path, "..")) { puts("c6 flash: use a /sdcard/ path"); return 1; }
    FILE *f = fopen(path, "rb");
    if (!f) { printf("c6 flash: cannot open %s\n", path); return 1; }
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    size_t n = buf ? fread(buf, 1, 4096, f) : 0;
    if (n < 64 || buf[0] != 0xE9 || buf[12] != COP_CHIP_ID || buf[13] != 0) {
        printf("c6 flash: %s is not an " COP_NAME " app image\n", path);
        fclose(f); heap_caps_free(buf); return 1;
    }
    esp_err_t e = esp_hosted_slave_ota_begin();
    if (e) {
        printf("c6 flash: OTA begin failed: %s (no update slot on the coprocessor?)\n", esp_err_to_name(e));
        fclose(f); heap_caps_free(buf); return 1;
    }
    size_t total = 0;
    while (n > 0) {
        e = esp_hosted_slave_ota_write(buf, (uint32_t)n);
        if (e) break;
        total += n;
        if ((total & 0x3FFFF) < n) printf("c6 flash: %u bytes\n", (unsigned)total);
        n = fread(buf, 1, 4096, f);
    }
    fclose(f); heap_caps_free(buf);
    if (e) { printf("c6 flash: write failed at %u: %s; the running image is untouched\n", (unsigned)total, esp_err_to_name(e)); return 1; }
    e = esp_hosted_slave_ota_end();
    if (e) { printf("c6 flash: image rejected by the coprocessor: %s\n", esp_err_to_name(e)); return 1; }
    e = esp_hosted_slave_ota_activate();
    /* The coprocessor restarts into the new image as it activates it, often
       before its answer gets back, so a failed reply here is the usual case
       and not a failure: the image was checked by ota_end above. */
    if (e == ESP_OK) printf("c6 flash: %u bytes written and activated; restarting the board\n", (unsigned)total);
    else printf("c6 flash: %u bytes written and checked; the coprocessor restarted before answering the activate (%s), "
                "which is normal. Restarting the board; 'c6 ver' and 'c6 mon 8' show the result\n",
                (unsigned)total, esp_err_to_name(e));
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return 0;
}

int ls_c6_update_command(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "flash")) return cop_flash(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "status")) {
        ls_c6_result_t r;
        portENTER_CRITICAL(&s_mux); r = s_result; portEXIT_CRITICAL(&s_mux);
        printf("c6: %u/%u bytes (%u%%): %s\n", (unsigned)r.sent, (unsigned)r.size,
               r.size ? (unsigned)(r.sent * 100 / r.size) : 0,
               r.message[0] ? r.message : "Idle; no update attempted");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "update") && !strcmp(argv[2], "auto")) {
        ls_act_status_t rc = auto_start();
        printf("c6: %s\n", rc == LS_ACT_OK ? "auto update started; c6 status shows progress" : ls_act_status_str(rc));
        return rc == LS_ACT_OK ? 0 : 1;
    }
    if (argc >= 2 && argc <= 4 && !strcmp(argv[1], "update")) {
        const char *path = argc >= 3 ? argv[2] : LS_C6_DEFAULT_PATH;
        bool force = argc == 4 && !strcmp(argv[3], "force");
        if (argc == 4 && !force) goto usage;
        if (argc == 3 && !strcmp(path, "force")) { path = LS_C6_DEFAULT_PATH; force = true; }
        ls_act_status_t rc = start(path, force, false, false);
        printf("c6: %s\n", rc == LS_ACT_OK ? "update queued; c6 status shows progress" : ls_act_status_str(rc));
        return rc == LS_ACT_OK ? 0 : 1;
    }
usage:
    puts("c6 update auto | c6 update [" LS_C6_DEFAULT_PATH "] [force] | c6 status | c6 flash /sdcard/IMAGE.bin\n"
         "Cable recovery only if the C6 no longer boots: docs/TDP4_FIRST_FLASH.md");
    return 1;
}
