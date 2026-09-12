#include "app_registry.h"
#include "radio_endpoint.h"
#include "event_bus.h"
/**/
#include "usb_autoreboot_pref.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <ctype.h>
#include <string.h>

static const char *TAG = "app";

#define MAX_APPS 4
static const app_t *s_apps[MAX_APPS];
static int          s_app_count   = 0;
static int          s_current_app = 0;
static page_t       s_page        = PAGE_MAIN;

static QueueHandle_t s_switch_q          = NULL;
static volatile bool s_switch_in_flight  = false;

static volatile bool s_parked = true;
/**/
static volatile bool s_parked_by_fault = false;

/**/
void app_set_usb_autoreboot(bool en) { usb_autoreboot_pref_set(en); }
bool app_usb_autoreboot(void)        { return usb_autoreboot_pref_get(); }

#define APP_REQ_PARK    (-2)
#define APP_REQ_UNPARK  (-3)
#define APP_REQ_RECOVER (-4)
#define APP_REQ_UI_PARK (-5)
static bool s_ui_held;
static uint32_t s_ui_token;
static uint32_t s_ui_ack;
static uint32_t s_ui_failed;

typedef struct {
    int kind;
    int index;
    uint32_t token;
    char endpoint_id[LS_RADIO_ENDPOINT_ID_MAX];
} app_request_t;

#define RECOVER_MAGIC 0x52534352u
#define RECOVER_LOOP_MAX 2
static RTC_NOINIT_ATTR uint32_t s_recover_magic;
static RTC_NOINIT_ATTR char     s_recover_app[24];
static RTC_NOINIT_ATTR uint32_t s_recover_count;
static bool s_booted_from_recovery = false;

static bool recovery_remember(const char *name)
{
    int up_s = (int)(esp_timer_get_time() / 1000000);
    if (s_booted_from_recovery && up_s < 30) s_recover_count++;
    else                                     s_recover_count = 1;

    if (s_recover_count >= RECOVER_LOOP_MAX) {
        s_recover_count = 0;
        s_recover_magic = 0;
        return false;
    }
    if (!name) name = "";
    strncpy(s_recover_app, name, sizeof(s_recover_app) - 1);
    s_recover_app[sizeof(s_recover_app) - 1] = 0;
    s_recover_magic = RECOVER_MAGIC;
    return true;
}

const char *app_recovery_take(void)
{
    if (s_recover_magic != RECOVER_MAGIC) return NULL;
    s_recover_magic = 0;
    if (!s_recover_app[0]) return NULL;
    s_booted_from_recovery = true;
    return s_recover_app;
}

int app_register(const app_t *desc)
{
    if (s_app_count >= MAX_APPS || !desc || !desc->name) return -1;
    s_apps[s_app_count] = desc;
    return s_app_count++;
}

int          app_current_index(void)     { return s_current_app; }
const app_t *app_current(void)           { return s_app_count > 0 ? s_apps[s_current_app] : NULL; }
const app_t *app_at(int idx)             { return (idx >= 0 && idx < s_app_count) ? s_apps[idx] : NULL; }
int          app_count(void)             { return s_app_count; }
bool         app_switch_in_progress(void){ return s_switch_in_flight; }

static bool stop_app(const app_t *app)
{
    if (!app) return true;
    if (app->on_stop) return app->on_stop();
    if (app->on_exit) app->on_exit();
    return true;
}

static void do_park(void)
{
    if (s_parked) return;
    s_switch_in_flight = true;
    const app_t *cur = s_apps[s_current_app];
    ESP_LOGI(TAG, "parking radio app '%s'", cur ? cur->name : "?");
    if (!stop_app(cur)) {
        /* a timed-out backend still owns its task/session.  Parking
         * it optimistically allowed a later unpark/switch to start a second
         * radio owner while the first one was live. */
        ESP_LOGE(TAG, "park aborted: '%s' did not stop", cur ? cur->name : "?");
        s_switch_in_flight = false;
        return;
    }
    /* the app stop callback owns joining its task and releasing its
     * radio session. Parking an app is not a USB-pipe teardown operation. */
    s_parked = true;
    /**/
    s_parked_by_fault = false;
    s_switch_in_flight = false;
}

static void do_unpark(void)
{
    if (!s_parked) return;
    s_switch_in_flight = true;
    const app_t *cur = s_apps[s_current_app];
    ESP_LOGI(TAG, "unparking radio app '%s'", cur ? cur->name : "?");
    s_parked = false;
    /**/
    s_parked_by_fault = false;
    s_page   = PAGE_MAIN;
    if (cur && cur->on_enter) cur->on_enter();
    s_switch_in_flight = false;
}

static void do_switch(int idx)
{
    if (idx < 0 || idx >= s_app_count) return;

    if (idx == s_current_app && !s_parked) return;

    s_switch_in_flight = true;

    int prev = s_current_app;
    const app_t *old = s_apps[prev];
    const app_t *nu  = s_apps[idx];

    ESP_LOGI(TAG, "switching '%s' -> '%s'%s",
             old ? old->name : "?", nu ? nu->name : "?",
             s_parked ? " (from parked)" : "");

    if (idx != s_current_app && !s_parked && !stop_app(old)) {
        ESP_LOGE(TAG, "switch aborted: '%s' did not stop",
                 old ? old->name : "?");
        s_switch_in_flight = false;
        return;
    }

    s_parked      = false;
    s_current_app = idx;
    s_page        = PAGE_MAIN;

    if (nu && nu->on_enter) nu->on_enter();

    event_t e = { 0 };
    e.kind = EVT_APP_SWITCHED;
    if (old && old->name) strncpy(e.u.sw.from, old->name, EVT_APP_NAME_MAX);
    if (nu  && nu->name)  strncpy(e.u.sw.to,   nu->name,  EVT_APP_NAME_MAX);
    event_bus_publish(&e);

    s_switch_in_flight = false;
    ESP_LOGI(TAG, "switched to '%s'", nu ? nu->name : "?");
}

static bool endpoint_owned_by_app(const ls_radio_endpoint_info_t *info,
                                  const app_t *app)
{
    if (!info || !app || !app->name || !info->leased) return false;
    char normalized[LS_RADIO_OWNER_MAX];
    size_t n = 0;
    for (const char *p = app->name; *p && n + 1 < sizeof(normalized); ++p) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) normalized[n++] = (char)tolower(c);
    }
    normalized[n] = '\0';
    return strcmp(normalized, info->owner) == 0;
}

static void do_recover(const char *endpoint_id)
{
    if (!endpoint_id || !endpoint_id[0]) return;
    ls_radio_endpoint_info_t info;
    if (ls_radio_endpoint_get(endpoint_id, &info) != LS_RADIO_OK ||
        !info.present) {
        ESP_LOGW(TAG, "recovery ignored: endpoint '%s' is not present",
                 endpoint_id);
        return;
    }
    const app_t *cur = s_apps[s_current_app];
    bool restart_app = !s_parked && endpoint_owned_by_app(&info, cur);
    s_switch_in_flight = true;
    ESP_LOGW(TAG, "recovering endpoint '%s'%s", endpoint_id,
             restart_app ? " after releasing the current app session" : "");
    if (restart_app && !stop_app(cur)) {
        ESP_LOGE(TAG, "recovery aborted: '%s' did not stop",
                 cur ? cur->name : "?");
        s_switch_in_flight = false;
        return;
    }

    ls_radio_err_t error = ls_radio_endpoint_recover(endpoint_id);
    if (error == LS_RADIO_OK) {
        if (restart_app && cur && cur->on_enter) cur->on_enter();
        /**/
        s_parked_by_fault = false;
        s_switch_in_flight = false;
        ESP_LOGW(TAG, "endpoint recovery complete for '%s'", endpoint_id);
        return;
    }

    if (restart_app && app_usb_autoreboot() &&
        recovery_remember(cur ? cur->name : "")) {
        ESP_LOGE(TAG, "endpoint '%s' recovery failed (%s) -- fast-rebooting into '%s'",
                 endpoint_id, ls_radio_err_name(error),
                 cur ? cur->name : "?");
        vTaskDelay(pdMS_TO_TICKS(30));
        esp_restart();
    }

    ESP_LOGE(TAG, "endpoint '%s' recovery failed: %s (auto-reboot %s)",
             endpoint_id, ls_radio_err_name(error),
             app_usb_autoreboot() ? "loop-broken" : "off");
    if (restart_app) {
        s_parked = true;
        /**/
        s_parked_by_fault = true;
    }
    s_switch_in_flight = false;
}

/**/
bool app_parked_by_fault(void) { return s_parked && s_parked_by_fault; }

/**/
bool app_switch_in_flight(void) { return s_switch_in_flight; }

bool app_switch_service(uint32_t wait_ticks)
{
    app_request_t request;
    if (!s_switch_q) return false;
    if (xQueueReceive(s_switch_q, &request, wait_ticks) != pdTRUE) return false;
    app_request_t latest;
    while (xQueueReceive(s_switch_q, &latest, 0) == pdTRUE) {
        /* the unload fence must never be coalesced away by
         * a stale radio request from the screen being destroyed. */
        if (latest.kind == APP_REQ_UI_PARK || request.kind != APP_REQ_UI_PARK)
            request = latest;
    }
    if (request.kind == APP_REQ_UI_PARK) {
        do_park();
        /* Publish the generation with the result. A late completion from an
         * earlier timeout cannot acknowledge a newer request that arrived
         * while on_stop was running. */
        if (s_parked)
            __atomic_store_n(&s_ui_ack, request.token, __ATOMIC_RELEASE);
        else
            __atomic_store_n(&s_ui_failed, request.token, __ATOMIC_RELEASE);
        return true;
    }
    if (__atomic_load_n(&s_ui_held, __ATOMIC_ACQUIRE)) return true;
    if      (request.kind == APP_REQ_PARK)    do_park();
    else if (request.kind == APP_REQ_UNPARK)  do_unpark();
    else if (request.kind == APP_REQ_RECOVER) do_recover(request.endpoint_id);
    else do_switch(request.index);
    return true;
}

static void switch_worker(void *arg)
{
    (void)arg;
    while (1) (void)app_switch_service(portMAX_DELAY);
}

void app_switch_worker_start(void)
{
    if (s_switch_q) return;
    s_switch_q = xQueueCreate(4, sizeof(app_request_t));
    if (!s_switch_q) {
        ESP_LOGE(TAG, "switch queue alloc failed");
        return;
    }
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        /* Left at 4096 deliberately. The idle high-water mark says
           3632 bytes are never touched, but this worker runs the apps'
           enter/exit callbacks, so a quiet moment is not its worst case, and
           test_app_registry_lifecycle pins name, depth and caps together as
           the contract from /. Not worth 1.5 KB. */
        switch_worker, "appsw", 4096, NULL, 3, NULL, 0,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        /* leaving a successfully allocated queue behind when the
         * internal-stack worker could not be created made every later radio
         * transition look accepted while no task existed to stop/join/start
         * either backend.  Delete the orphan queue so the existing direct
         * lifecycle path remains available under this allocation failure. */
        ESP_LOGE(TAG, "switch worker start failed - using direct lifecycle");
        vQueueDelete(s_switch_q);
        s_switch_q = NULL;
    }
}

void app_switch_to(int idx)
{
    if (__atomic_load_n(&s_ui_held, __ATOMIC_ACQUIRE)) return;
    if (idx < 0 || idx >= s_app_count) return;

    if (idx == s_current_app && !s_switch_in_flight && !s_parked) return;
    if (!s_switch_q) {

        do_switch(idx);
        return;
    }

    app_request_t request = {.kind = idx, .index = idx};
    xQueueSend(s_switch_q, &request, 0);
}

void app_cycle_next(void)
{
    if (s_app_count <= 1) return;
    app_switch_to((s_current_app + 1) % s_app_count);
}

uint32_t app_ui_park_request(void)
{
    __atomic_store_n(&s_ui_held, true, __ATOMIC_RELEASE);
    uint32_t token = __atomic_add_fetch(&s_ui_token, 1, __ATOMIC_ACQ_REL);
    if (!token) token = __atomic_add_fetch(&s_ui_token, 1, __ATOMIC_ACQ_REL);
    app_request_t request = {.kind = APP_REQ_UI_PARK, .token = token};
    if (!s_switch_q || xQueueSend(s_switch_q, &request, 0) != pdTRUE) {
        __atomic_store_n(&s_ui_failed, token, __ATOMIC_RELEASE);
        return 0;
    }
    return token;
}

int app_ui_park_status(uint32_t token)
{
    if (!token || token != __atomic_load_n(&s_ui_token, __ATOMIC_ACQUIRE)) return -1;
    if (token == __atomic_load_n(&s_ui_failed, __ATOMIC_ACQUIRE)) return -1;
    return token == __atomic_load_n(&s_ui_ack, __ATOMIC_ACQUIRE) ? 1 : 0;
}

void app_ui_park_release(void)
{
    __atomic_store_n(&s_ui_held, false, __ATOMIC_RELEASE);
}

bool app_parked(void) { return s_parked; }

void app_park(void)
{
    if (__atomic_load_n(&s_ui_held, __ATOMIC_ACQUIRE)) return;
    if (s_parked) return;
    if (!s_switch_q) { do_park(); return; }
    app_request_t request = {.kind = APP_REQ_PARK};
    xQueueSend(s_switch_q, &request, 0);
}

void app_unpark(void)
{
    if (__atomic_load_n(&s_ui_held, __ATOMIC_ACQUIRE)) return;
    if (!s_parked) return;
    if (!s_switch_q) { do_unpark(); return; }
    app_request_t request = {.kind = APP_REQ_UNPARK};
    xQueueSend(s_switch_q, &request, 0);
}

void app_request_recover(const char *endpoint_id)
{
    if (__atomic_load_n(&s_ui_held, __ATOMIC_ACQUIRE)) return;
    if (!endpoint_id || !endpoint_id[0]) return;
    if (!s_switch_q) {
        do_recover(endpoint_id);
        return;
    }
    app_request_t request = {.kind = APP_REQ_RECOVER};
    strncpy(request.endpoint_id, endpoint_id,
            sizeof(request.endpoint_id) - 1);
    xQueueSend(s_switch_q, &request, 0);
}

page_t page_current(void)    { return s_page; }
void   page_set(page_t p)    { if (p < PAGE_COUNT) s_page = p; }
void   page_cycle_next(void) { s_page = (s_page + 1) % PAGE_COUNT; }

typedef enum { KP_IDLE = 0, KP_ESC, KP_CSI } kp_state_t;
static kp_state_t s_kp_state = KP_IDLE;
static int        s_kp_ticks = 0;

tui_key_t key_feed(uint8_t b)
{
    s_kp_ticks = 0;
    switch (s_kp_state) {
    case KP_IDLE:
        if (b == 0x1b) { s_kp_state = KP_ESC; return TK_NONE; }
        if (b == 0x08 || b == 0x7f) return TK_BKSP;
        if (b == '\r' || b == '\n') return TK_ENTER;
        return (tui_key_t)b;
    case KP_ESC:
        if (b == '[') { s_kp_state = KP_CSI; return TK_NONE; }
        s_kp_state = KP_IDLE;
        return TK_ESC;
    case KP_CSI:
        s_kp_state = KP_IDLE;
        switch (b) {
            case 'A': return TK_UP;
            case 'B': return TK_DOWN;
            case 'C': return TK_RIGHT;
            case 'D': return TK_LEFT;
            default:  return TK_NONE;
        }
    }
    return TK_NONE;
}

tui_key_t key_flush_timeout(void)
{
    if (s_kp_state == KP_IDLE) return TK_NONE;
    if (++s_kp_ticks < 3) return TK_NONE;
    s_kp_state = KP_IDLE;
    s_kp_ticks = 0;
    return TK_ESC;
}
