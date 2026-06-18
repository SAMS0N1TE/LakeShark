
#include "app_registry.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
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

static bool s_usb_autoreboot = false;
void app_set_usb_autoreboot(bool en) { s_usb_autoreboot = en; }
bool app_usb_autoreboot(void)        { return s_usb_autoreboot; }

#define APP_REQ_PARK    (-2)
#define APP_REQ_UNPARK  (-3)
#define APP_REQ_RECOVER (-4)

extern int rtlsdr_dev_reopen(void);

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

static void do_park(void)
{
    if (s_parked) return;
    s_switch_in_flight = true;
    const app_t *cur = s_apps[s_current_app];
    ESP_LOGI(TAG, "parking radio app '%s'", cur ? cur->name : "?");
    if (cur && cur->on_exit) cur->on_exit();
    s_parked = true;
    s_switch_in_flight = false;
}

static void do_unpark(void)
{
    if (!s_parked) return;
    s_switch_in_flight = true;
    const app_t *cur = s_apps[s_current_app];
    ESP_LOGI(TAG, "unparking radio app '%s'", cur ? cur->name : "?");
    s_parked = false;
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

    if (idx != s_current_app && old && old->on_exit && !s_parked) old->on_exit();

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

static void do_recover(void)
{
    if (s_parked) return;
    const app_t *cur = s_apps[s_current_app];
    s_switch_in_flight = true;
    ESP_LOGW(TAG, "USB pipe wedged -- recovering radio app '%s'", cur ? cur->name : "?");
    if (cur && cur->on_exit)  cur->on_exit();

    if (rtlsdr_dev_reopen() == 0) {
        if (cur && cur->on_enter) cur->on_enter();
        s_switch_in_flight = false;
        ESP_LOGW(TAG, "USB recovery complete for '%s'", cur ? cur->name : "?");
        return;
    }

    if (s_usb_autoreboot && recovery_remember(cur ? cur->name : "")) {
        ESP_LOGE(TAG, "in-place recovery impossible -- fast-rebooting into '%s'",
                 cur ? cur->name : "?");
        vTaskDelay(pdMS_TO_TICKS(30));
        esp_restart();
    }

    ESP_LOGE(TAG, "USB wedge unrecoverable -- radio parked (auto-reboot %s); replug dongle",
             s_usb_autoreboot ? "loop-broken" : "off");
    s_parked = true;
    s_switch_in_flight = false;
}

static void switch_worker(void *arg)
{
    (void)arg;
    int target;
    while (1) {
        if (xQueueReceive(s_switch_q, &target, portMAX_DELAY) == pdTRUE) {

            int latest;
            while (xQueueReceive(s_switch_q, &latest, 0) == pdTRUE) {
                target = latest;
            }
            if      (target == APP_REQ_PARK)    do_park();
            else if (target == APP_REQ_UNPARK)  do_unpark();
            else if (target == APP_REQ_RECOVER) do_recover();
            else                                do_switch(target);
        }
    }
}

void app_switch_worker_start(void)
{
    if (s_switch_q) return;
    s_switch_q = xQueueCreate(4, sizeof(int));
    if (!s_switch_q) {
        ESP_LOGE(TAG, "switch queue alloc failed");
        return;
    }
    xTaskCreatePinnedToCore(switch_worker, "appsw", 4096, NULL, 3, NULL, 0);
}

void app_switch_to(int idx)
{
    if (idx < 0 || idx >= s_app_count) return;

    if (idx == s_current_app && !s_switch_in_flight && !s_parked) return;
    if (!s_switch_q) {

        do_switch(idx);
        return;
    }

    xQueueSend(s_switch_q, &idx, 0);
}

void app_cycle_next(void)
{
    if (s_app_count <= 1) return;
    app_switch_to((s_current_app + 1) % s_app_count);
}

bool app_parked(void) { return s_parked; }

void app_park(void)
{
    if (s_parked) return;
    if (!s_switch_q) { do_park(); return; }
    int req = APP_REQ_PARK;
    xQueueSend(s_switch_q, &req, 0);
}

void app_unpark(void)
{
    if (!s_parked) return;
    if (!s_switch_q) { do_unpark(); return; }
    int req = APP_REQ_UNPARK;
    xQueueSend(s_switch_q, &req, 0);
}

void app_request_recover(void)
{
    if (s_parked || !s_switch_q) return;
    int req = APP_REQ_RECOVER;
    xQueueSend(s_switch_q, &req, 0);
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
