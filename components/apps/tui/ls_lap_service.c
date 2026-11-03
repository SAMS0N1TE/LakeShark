/* See ls_lap_service.h. Screen task only, except ls_lap_service_request_stop
 * and ls_lap_services, which the console calls (a flag and a snapshot). */
#include "ls_lap_service.h"
#include "ls_lap_api.h"
#include "ls_lap_system.h"
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

static ls_lap_service_context_t *service_context(void) { return ls_lap_api_services(); }
void ls_lap_set_guard(const ls_lap_guard_t *g) { service_context()->guard = g; }
bool ls_lap_code_ok(const void *fn) {
    if (!fn) return false;
    return !service_context()->guard || !service_context()->guard->code_ok || service_context()->guard->code_ok((uintptr_t)fn);
}


void ls_lap_call_begin(void) {
    if (service_context()->depth++) return;
    service_context()->outer = !ls_lap_api_inside();
    if (!service_context()->outer) return;
    if (service_context()->guard && service_context()->guard->begin) service_context()->guard->begin();
    else ls_lap_api_callback(true);
}
void ls_lap_call_end(void) {
    if (!service_context()->depth || --service_context()->depth || !service_context()->outer) return;
    if (service_context()->guard && service_context()->guard->end) service_context()->guard->end();
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



void ls_lap_service_begin(const lap_app_t *app) {
    memset(&service_context()->state, 0, sizeof(service_context()->state));
    atomic_store(&service_context()->stop_wanted, false);
    service_context()->state.shown = true;
    if (!app) return;
    service_context()->state.granted = app->api_major == 2 && app->api_minor >= 1 && (app->flags & LAP_CAP_SERVICE);
    snprintf(service_context()->state.id, sizeof(service_context()->state.id), "%s", app->id);
    snprintf(service_context()->state.name, sizeof(service_context()->state.name), "%s", app->name);
    service_context()->state.hue = app->color;
}
void ls_lap_service_end(void) { ls_lap_service_begin(NULL); }
bool ls_lap_service_on(void) { return service_context()->state.running; }
void ls_lap_service_set_shown(bool shown) { service_context()->state.shown = shown; }
bool ls_lap_service_shown(void) { return service_context()->state.shown; }
const char *ls_lap_service_why(void) { return service_context()->state.why; }

void (*ls_lap_service_off(const char *why))(void) {
    void (*stopped)(void) = service_context()->state.running ? service_context()->state.stopped : NULL;
    if (why) snprintf(service_context()->state.why, sizeof(service_context()->state.why), "%s", why);
    service_context()->state.running = service_context()->state.stop = false;
    service_context()->state.tick = service_context()->state.stopped = NULL;
    service_context()->state.strikes = 0;
    return stopped;
}

int ls_lap_service_poll(void) {
    ls_lap_system_frame(); /* viewer lifecycle also reaches hidden services */
    if (!service_context()->state.running) return 0;
    if (service_context()->state.stop) { service_context()->state.why[0] = 0; return -1; }        /* its own wish: no notice */
    const int64_t t0 = now_us();
    if (t0 < service_context()->state.next_us) return 0;
    /* Due: the next one a period after this one was due, not after it ran,
       so the rate holds; a board that fell far behind starts afresh. */
    service_context()->state.next_us += (int64_t)service_context()->state.period_ms * 1000;
    if (service_context()->state.next_us <= t0) service_context()->state.next_us = t0 + (int64_t)service_context()->state.period_ms * 1000;
    void (*tick)(void) = service_context()->state.tick;
    if (!ls_lap_code_ok((const void *)tick)) {
        snprintf(service_context()->state.why, sizeof(service_context()->state.why), "bad tick");
        return -1;
    }
    ls_lap_call_begin();
    tick();
    ls_lap_call_end();
    const int64_t dt = now_us() - t0;
    const uint32_t us = dt < 0 ? 0 : dt > 0x7FFFFFFF ? 0x7FFFFFFFu : (uint32_t)dt;
    service_context()->state.st.ticks++;
    service_context()->state.st.last_us = us;
    if (us > service_context()->state.st.worst_us) service_context()->state.st.worst_us = us;
    if (us > LAP_SERVICE_BUDGET_US) {
        service_context()->state.st.over++;
        service_context()->state.strikes++;
    } else
        service_context()->state.strikes = 0;
    if (service_context()->state.running && (us > 10u * LAP_SERVICE_BUDGET_US || service_context()->state.strikes >= LS_LAP_SVC_STRIKES)) {
        LS_SNPRINTF(t_ls_lap_service_too_slow_lu_us, service_context()->state.why, sizeof(service_context()->state.why), "too slow (%lu us a tick)", (unsigned long)us);
        return -1;
    }
    if (service_context()->state.stop) { service_context()->state.why[0] = 0; return -1; }        /* its own wish: no notice */
    return 1;
}

bool ls_lap_service_request_stop(const char *id) {
    bool found=false;
    for(unsigned i=0;i<LS_LAP_CONTEXTS;i++) {
        ls_lap_service_context_t *c=ls_lap_instance_service_context(i);
        if(c && c->state.running && (!id||!strcmp(id,c->state.id))) {
            atomic_store(&c->stop_wanted,true); found=true;
            if(id) break;
        }
    }
    return found;
}
bool ls_lap_service_take_stop(void) {
    if (!atomic_exchange(&service_context()->stop_wanted, false)) return false;
    return service_context()->state.running;
}

int ls_lap_services(ls_lap_service_info_t *out, int max) {
    int n=0;
    if(!out||max<1) return 0;
    for(unsigned i=0;i<LS_LAP_CONTEXTS&&n<max;i++) {
        ls_lap_service_context_t *c=ls_lap_instance_service_context(i);
        if(!c||!c->state.running) continue;
        ls_lap_service_info_t *v=&out[n++]; memset(v,0,sizeof(*v));
        v->running=true; v->shown=c->state.shown; v->hue=c->state.hue;
        v->period_ms=c->state.period_ms; v->stats=c->state.st;
        snprintf(v->id,sizeof(v->id),"%s",c->state.id); snprintf(v->name,sizeof(v->name),"%s",c->state.name);
    }
    return n;
}

int lap_service_limit(void) { return CONFIG_LS_LAP_SERVICES_MAX; }
int lap_service_list(lap_service_info_t *out,int max) {
    if(!ls_lap_api_inside()||max<1||max>64||!ls_lap_api_readable(out,(size_t)max*sizeof(*out))) return LAP_SVC_EINVAL;
    int n=0;
    for(unsigned i=0;i<LS_LAP_CONTEXTS&&n<max;i++) if(ls_lap_instance_service_info(i,&out[n])) n++;
    return n;
}
int lap_service_query(const char *id,lap_service_info_t *out) {
    char name[32]; unsigned j=0;
    if(!id||!ls_lap_api_inside()||!ls_lap_api_readable(out,sizeof(*out))) return LAP_SVC_EINVAL;
    for(;j<sizeof(name);j++) { if(!ls_lap_api_readable(id+j,1)) return LAP_SVC_EINVAL; name[j]=id[j]; if(!name[j]) break; }
    if(!j||j==sizeof(name)) return LAP_SVC_EINVAL;
    for(unsigned i=0;i<LS_LAP_CONTEXTS;i++) {
        lap_service_info_t v;
        if(ls_lap_instance_service_info(i,&v)&&!strcmp(name,v.id)) { *out=v; return 1; }
    }
    memset(out,0,sizeof(*out)); return 0;
}

void ls_lap_service_notice(const char *body) {
#if DEVICE
    static const uint8_t HUE[8] = {TUI_WHITE, TUI_RED, TUI_GREEN, TUI_YELLOW, TUI_BLUE, TUI_MAGENTA, TUI_CYAN, TUI_WHITE};
    ls_notice_t n = {0};
    snprintf(n.title, sizeof(n.title), "%.11s SERVICE", service_context()->state.name);
    snprintf(n.body, sizeof(n.body), "%s", body ? body : "");
    n.screen = -1;
    n.hue = n.accent = HUE[service_context()->state.hue & 7];
    n.quiet = n.steady = true;
    ls_notify_post_quiet(&n);
#else
    ls_lap_hub_host_t *h = ls_lap_hub_host();
    h->notices++;
    snprintf(h->last_title, sizeof(h->last_title), "%.11s SERVICE", service_context()->state.name);
    snprintf(h->last_body, sizeof(h->last_body), "%s", body ? body : "");
#endif
}

/* ---- the app's calls ------------------------------------------------------------ */

int lap_service_start(const lap_service_t *svc) {
    lap_service_t c;
    if (!ls_lap_copy_input(&c, svc, sizeof(c))) return LAP_SVC_EINVAL;
    if (!service_context()->state.granted) return LAP_SVC_EACCES;
    if(!service_context()->state.running) {
        unsigned n=0; lap_service_info_t v;
        for(unsigned i=0;i<LS_LAP_CONTEXTS;i++) if(ls_lap_instance_service_info(i,&v)) n++;
        if(n>=CONFIG_LS_LAP_SERVICES_MAX) return LAP_SVC_EBUSY;
    }
    if (!ls_lap_code_ok((const void *)c.tick) || (c.stopped && !ls_lap_code_ok((const void *)c.stopped)))
        return LAP_SVC_EINVAL;
    uint32_t p = c.period_ms ? c.period_ms : 100u;
    if (p < LS_LAP_SVC_MIN_MS) p = LS_LAP_SVC_MIN_MS;
    if (p > LS_LAP_SVC_MAX_MS) p = LS_LAP_SVC_MAX_MS;
    if (!service_context()->state.running) {
        memset(&service_context()->state.st, 0, sizeof(service_context()->state.st));
        service_context()->state.strikes = 0;
        service_context()->state.why[0] = 0;
        service_context()->state.next_us = now_us() + (int64_t)p * 1000;
    }
    service_context()->state.tick = c.tick;
    service_context()->state.stopped = c.stopped;
    service_context()->state.period_ms = p;
    service_context()->state.running = true;
    service_context()->state.stop = false;
    return LAP_SVC_OK;
}
void lap_service_stop(void) {
    if (!ls_lap_api_inside() || !service_context()->state.running) return;
    if (!service_context()->state.shown) service_context()->state.stop = true;             /* ls_lap.c unloads after the tick */
    else ls_lap_service_off("stopped by the app");
}
bool lap_service_running(void) { return ls_lap_api_inside() && service_context()->state.running && !service_context()->state.stop; }
bool lap_screen_open(void) { return ls_lap_api_inside() && service_context()->state.shown; }
int lap_service_stats(lap_service_stats_t *out) {
    if (!ls_lap_api_inside() || !ls_lap_api_readable(out, sizeof(*out))) return LAP_SVC_EINVAL;
    *out = service_context()->state.st;
    return service_context()->state.running ? 1 : 0;
}
