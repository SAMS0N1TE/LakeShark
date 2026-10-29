/* See ls_lap_service.h. Screen task only, except ls_lap_service_request_stop
 * and ls_lap_services, which the console calls (a flag and a snapshot). */
#include "ls_lap_service.h"
#include "ls_lap_api.h"
#include "ls_lap_radio.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "ls_textpack.h"
#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#endif
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "ls_notify.h"
#include "tuilib/tui_core.h"
#define DEVICE 1
#else
#define DEVICE 0
#endif

/* ---- the guard -------------------------------------------------------------- */

static const ls_lap_guard_t *guard;
void ls_lap_set_guard(const ls_lap_guard_t *g) { guard = g; }
bool ls_lap_code_ok(const void *fn) {
    if (!fn) return false;
    return !guard || !guard->code_ok || guard->code_ok((uintptr_t)fn);
}
static int depth;                       /* nested calls: only the outer one marks */
static bool outer;                      /* this begin opened the API context */
void ls_lap_call_begin(void) {
    if (depth++) return;
    outer = !ls_lap_api_inside();
    if (!outer) return;
    if (guard && guard->begin) guard->begin();
    else ls_lap_api_callback(true);
}
void ls_lap_call_end(void) {
    if (!depth || --depth || !outer) return;
    if (guard && guard->end) guard->end();
    else ls_lap_api_callback(false);
}

/* ---- the service -------------------------------------------------------------- */

#if !defined(ESP_PLATFORM) || defined(LS_LAP_HOST_TEST)
int64_t (*ls_lap_service_host_clock)(void);
#endif
#if !defined(ESP_PLATFORM)
extern uint32_t (*ls_lap_api_host_clock)(void);
#endif
static int64_t now_us(void) {
#if defined(LS_LAP_HOST_TEST)
    if (ls_lap_service_host_clock) return ls_lap_service_host_clock();
#endif
#if defined(ESP_PLATFORM)
    return esp_timer_get_time();
#else
    if (ls_lap_service_host_clock) return ls_lap_service_host_clock();
    return ls_lap_api_host_clock ? (int64_t)ls_lap_api_host_clock() * 1000 : 0;
#endif
}

static struct {
    bool granted, running, shown, stop;  /* stop: the app asked while hidden */
    void (*tick)(void), (*stopped)(void);
    uint32_t period_ms;
    int64_t next_us;
    int strikes;
    lap_service_stats_t st;
    char id[32], name[16], why[40];
    uint8_t hue;
} v;
static atomic_bool stop_wanted;
static char stop_id[32];

void ls_lap_service_begin(const lap_app_t *app) {
    memset(&v, 0, sizeof(v));
    atomic_store(&stop_wanted, false);
    v.shown = true;
    if (!app) return;
    v.granted = app->api_major == 2 && app->api_minor >= 1 && (app->flags & LAP_CAP_SERVICE);
    snprintf(v.id, sizeof(v.id), "%s", app->id);
    snprintf(v.name, sizeof(v.name), "%s", app->name);
    v.hue = app->color;
}
void ls_lap_service_end(void) { ls_lap_service_begin(NULL); }
bool ls_lap_service_on(void) { return v.running; }
void ls_lap_service_set_shown(bool shown) { v.shown = shown; }
bool ls_lap_service_shown(void) { return v.shown; }
const char *ls_lap_service_why(void) { return v.why; }

void (*ls_lap_service_off(const char *why))(void) {
    void (*stopped)(void) = v.running ? v.stopped : NULL;
    if (why) snprintf(v.why, sizeof(v.why), "%s", why);
    v.running = v.stop = false;
    v.tick = v.stopped = NULL;
    v.strikes = 0;
    return stopped;
}

int ls_lap_service_poll(void) {
    if (!v.running) return 0;
    if (v.stop) { v.why[0] = 0; return -1; }        /* its own wish: no notice */
    const int64_t t0 = now_us();
    if (t0 < v.next_us) return 0;
    /* Due: the next one a period after this one was due, not after it ran,
       so the rate holds; a board that fell far behind starts afresh. */
    v.next_us += (int64_t)v.period_ms * 1000;
    if (v.next_us <= t0) v.next_us = t0 + (int64_t)v.period_ms * 1000;
    void (*tick)(void) = v.tick;
    if (!ls_lap_code_ok((const void *)tick)) {
        snprintf(v.why, sizeof(v.why), "bad tick");
        return -1;
    }
    ls_lap_call_begin();
    tick();
    ls_lap_call_end();
    const int64_t dt = now_us() - t0;
    const uint32_t us = dt < 0 ? 0 : dt > 0x7FFFFFFF ? 0x7FFFFFFFu : (uint32_t)dt;
    v.st.ticks++;
    v.st.last_us = us;
    if (us > v.st.worst_us) v.st.worst_us = us;
    if (us > LAP_SERVICE_BUDGET_US) {
        v.st.over++;
        v.strikes++;
    } else
        v.strikes = 0;
    if (v.running && (us > 10u * LAP_SERVICE_BUDGET_US || v.strikes >= LS_LAP_SVC_STRIKES)) {
        LS_SNPRINTF(t_ls_lap_service_too_slow_lu_us, v.why, sizeof(v.why), "too slow (%lu us a tick)", (unsigned long)us);
        return -1;
    }
    if (v.stop) { v.why[0] = 0; return -1; }        /* its own wish: no notice */
    return 1;
}

bool ls_lap_service_request_stop(const char *id) {
    if (!v.running || (id && strcmp(id, v.id))) return false;
    snprintf(stop_id, sizeof(stop_id), "%s", v.id);
    atomic_store(&stop_wanted, true);
    return true;
}
bool ls_lap_service_take_stop(void) {
    if (!atomic_exchange(&stop_wanted, false)) return false;
    return v.running && !strcmp(stop_id, v.id);
}

int ls_lap_services(ls_lap_service_info_t *out, int max) {
    if (!out || max < 1 || !v.running) return 0;
    memset(out, 0, sizeof(*out));
    out->running = true;
    out->shown = v.shown;
    snprintf(out->id, sizeof(out->id), "%s", v.id);
    snprintf(out->name, sizeof(out->name), "%s", v.name);
    out->hue = v.hue;
    out->period_ms = v.period_ms;
    out->stats = v.st;
    return 1;
}

void ls_lap_service_notice(const char *body) {
#if DEVICE
    static const uint8_t HUE[8] = {TUI_WHITE, TUI_RED, TUI_GREEN, TUI_YELLOW, TUI_BLUE, TUI_MAGENTA, TUI_CYAN, TUI_WHITE};
    ls_notice_t n = {0};
    snprintf(n.title, sizeof(n.title), "%.11s SERVICE", v.name);
    snprintf(n.body, sizeof(n.body), "%s", body ? body : "");
    n.screen = -1;
    n.hue = n.accent = HUE[v.hue & 7];
    n.quiet = n.steady = true;
    ls_notify_post_quiet(&n);
#else
    ls_lap_hub_host_t *h = ls_lap_hub_host();
    h->notices++;
    snprintf(h->last_title, sizeof(h->last_title), "%.11s SERVICE", v.name);
    snprintf(h->last_body, sizeof(h->last_body), "%s", body ? body : "");
#endif
}

/* ---- the app's calls ------------------------------------------------------------ */

int lap_service_start(const lap_service_t *svc) {
    if (!ls_lap_api_inside() || !ls_lap_api_owns(svc, sizeof(*svc))) return LAP_SVC_EINVAL;
    if (!v.granted) return LAP_SVC_EACCES;
    const lap_service_t c = *svc;            /* copy-in */
    if (!ls_lap_code_ok((const void *)c.tick) || (c.stopped && !ls_lap_code_ok((const void *)c.stopped)))
        return LAP_SVC_EINVAL;
    uint32_t p = c.period_ms ? c.period_ms : 100u;
    if (p < LS_LAP_SVC_MIN_MS) p = LS_LAP_SVC_MIN_MS;
    if (p > LS_LAP_SVC_MAX_MS) p = LS_LAP_SVC_MAX_MS;
    if (!v.running) {
        memset(&v.st, 0, sizeof(v.st));
        v.strikes = 0;
        v.why[0] = 0;
        v.next_us = now_us() + (int64_t)p * 1000;
    }
    v.tick = c.tick;
    v.stopped = c.stopped;
    v.period_ms = p;
    v.running = true;
    v.stop = false;
    return LAP_SVC_OK;
}
void lap_service_stop(void) {
    if (!ls_lap_api_inside() || !v.running) return;
    if (!v.shown) v.stop = true;             /* ls_lap.c unloads after the tick */
    else ls_lap_service_off("stopped by the app");
}
bool lap_service_running(void) { return ls_lap_api_inside() && v.running && !v.stop; }
bool lap_screen_open(void) { return ls_lap_api_inside() && v.shown; }
int lap_service_stats(lap_service_stats_t *out) {
    if (!ls_lap_api_inside() || !ls_lap_api_owns(out, sizeof(*out))) return LAP_SVC_EINVAL;
    *out = v.st;
    return v.running ? 1 : 0;
}
