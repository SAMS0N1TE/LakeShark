/* app_registry.c - app dispatch, page state, UART key parser, deferred switch.
 *
 * The switch worker is the single place where on_exit / on_enter run.
 * Callers (TUI key handler, settings page) just enqueue an index and
 * return immediately. The worker drains the queue serially so a fast
 * 'A A A' key-mash collapses to one final settled state instead of
 * racing multiple half-completed teardowns.
 */

#include "app_registry.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "app";

#define MAX_APPS 4
static const app_t *s_apps[MAX_APPS];
static int          s_app_count   = 0;
static int          s_current_app = 0;
static page_t       s_page        = PAGE_MAIN;

/* Deferred-switch plumbing. */
static QueueHandle_t s_switch_q          = NULL;
static volatile bool s_switch_in_flight  = false;

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

/* The actual switch, run on the worker task only. */
static void do_switch(int idx)
{
    if (idx < 0 || idx >= s_app_count) return;
    if (idx == s_current_app)          return;

    s_switch_in_flight = true;

    int prev = s_current_app;
    const app_t *old = s_apps[prev];
    const app_t *nu  = s_apps[idx];

    ESP_LOGI(TAG, "switching '%s' -> '%s'",
             old ? old->name : "?", nu ? nu->name : "?");

    /* Tear down old app first. This may block for up to a few seconds
     * while the app's RX task drains. That's fine - we're on a dedicated
     * worker, not the TUI task. */
    if (old && old->on_exit) old->on_exit();

    /* Flip the active index *between* exit and enter. While the switch
     * is in flight, app_current() still reports the outgoing app, which
     * is the safer default for any code that snapshots state. */
    s_current_app = idx;
    s_page        = PAGE_MAIN;

    if (nu && nu->on_enter) nu->on_enter();

    /* Publish the switched event so subscribers (event_stream output,
     * external tooling) see a clean transition. */
    event_t e = { 0 };
    e.kind = EVT_APP_SWITCHED;
    if (old && old->name) strncpy(e.u.sw.from, old->name, EVT_APP_NAME_MAX);
    if (nu  && nu->name)  strncpy(e.u.sw.to,   nu->name,  EVT_APP_NAME_MAX);
    event_bus_publish(&e);

    s_switch_in_flight = false;
    ESP_LOGI(TAG, "switched to '%s'", nu ? nu->name : "?");
}

static void switch_worker(void *arg)
{
    (void)arg;
    int target;
    while (1) {
        if (xQueueReceive(s_switch_q, &target, portMAX_DELAY) == pdTRUE) {
            /* Coalesce: if more requests are queued, jump straight to
             * the latest. Avoids visible flicker if the user mashes 'A'. */
            int latest;
            while (xQueueReceive(s_switch_q, &latest, 0) == pdTRUE) {
                target = latest;
            }
            do_switch(target);
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
    if (idx == s_current_app && !s_switch_in_flight) return;
    if (!s_switch_q) {
        /* Worker not started yet (very early boot path) - run inline.
         * This only happens during initial app registration, where the
         * caller is single-threaded anyway. */
        do_switch(idx);
        return;
    }
    /* Non-blocking send: if queue is full it just drops, which is fine
     * because the latest request will still be picked up by the
     * coalescing loop in switch_worker. */
    xQueueSend(s_switch_q, &idx, 0);
}

void app_cycle_next(void)
{
    if (s_app_count <= 1) return;
    app_switch_to((s_current_app + 1) % s_app_count);
}

page_t page_current(void)    { return s_page; }
void   page_set(page_t p)    { if (p < PAGE_COUNT) s_page = p; }
void   page_cycle_next(void) { s_page = (s_page + 1) % PAGE_COUNT; }

/* UART delivers arrow keys as 3-byte sequences (ESC '[' A/B/C/D). The
 * polling loop calls uart_read_bytes with timeout 0, so we feed bytes
 * through this state machine one at a time. key_flush_timeout() recovers
 * from a stalled mid-sequence (bare ESC or interrupted CSI). */
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
