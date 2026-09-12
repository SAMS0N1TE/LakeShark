/* LS_TEST_SOURCES: ${FW}/components/lakeshark/core/app_registry.c */
#include "ls_test.h"

#include "app_registry.h"
#include "event_bus.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "radio_endpoint.h"

#include <stdbool.h>
#include <string.h>

static char s_trace[32];
static size_t s_trace_len;
static event_t s_event;
static unsigned s_events;
static bool s_autoreboot;
static bool s_fm_stop_ok = true;
static void check_ui_fence(void);

static void trace(char c)
{
    if (s_trace_len + 1 < sizeof(s_trace)) {
        s_trace[s_trace_len++] = c;
        s_trace[s_trace_len] = '\0';
    }
}

static void fm_enter(void)  { trace('F'); }
static bool fm_stop(void)   { trace('f'); return s_fm_stop_ok; }
static void p25_enter(void) { trace('P'); }
static void p25_exit(void)  { trace('p'); }

static const app_t FM_APP = {
    .name = "FM",
    .on_enter = fm_enter,
    .on_stop = fm_stop,
};

static const app_t P25_APP = {
    .name = "P25",
    .on_enter = p25_enter,
    .on_exit = p25_exit,
};

void usb_autoreboot_pref_set(bool enabled) { s_autoreboot = enabled; }
bool usb_autoreboot_pref_get(void) { return s_autoreboot; }

void event_bus_publish(const event_t *event)
{
    if (event) s_event = *event;
    ++s_events;
}

ls_radio_err_t ls_radio_endpoint_get(const char *id,
                                     ls_radio_endpoint_info_t *info)
{
    (void)id;
    (void)info;
    return LS_RADIO_ERR_UNAVAILABLE;
}

ls_radio_err_t ls_radio_endpoint_recover(const char *id)
{
    (void)id;
    return LS_RADIO_ERR_UNAVAILABLE;
}

const char *ls_radio_err_name(ls_radio_err_t error)
{
    (void)error;
    return "injected";
}

void esp_restart(void) {}

LS_CASE(acars_to_p25_transition_and_worker_failure_fallback)
{
    LS_EQ_INT(app_register(&FM_APP), 0);
    LS_EQ_INT(app_register(&P25_APP), 1);

    app_switch_to(0);       /* AppACARS run -> lakeshark_acars_start. */
    app_park();             /* AppACARS pause -> lakeshark_acars_stop. */
    app_switch_to(1);       /* AppP25 resume -> lakeshark_select_p25. */

    LS_EQ_STR(s_trace, "FfP");
    LS_EQ_INT(app_current_index(), 1);
    LS_CHECK(!app_parked());
    LS_EQ_INT(s_events, 2);
    LS_EQ_INT(s_event.kind, EVT_APP_SWITCHED);
    LS_EQ_STR(s_event.u.sw.from, "FM");
    LS_EQ_STR(s_event.u.sw.to, "P25");

    s_trace_len = 0;
    s_trace[0] = '\0';
    ls_shim_task_reset();
    ls_shim_queue_reset();

    /* Queue allocation failure must preserve the same synchronous lifecycle
       fallback as worker allocation failure. */
    ls_shim_queue_fail_create(pdTRUE);
    app_switch_worker_start();
    app_switch_to(0);
    LS_EQ_STR(s_trace, "pF");
    LS_EQ_INT(app_current_index(), 0);
    LS_EQ_UINT(ls_shim_queue_delete_count(), 0);

    s_trace_len = 0;
    s_trace[0] = '\0';
    ls_shim_queue_reset();
    ls_shim_task_fail_create(pdTRUE);

    app_switch_worker_start();

    LS_EQ_STR(ls_shim_task_last_name(), "appsw");
    LS_EQ_UINT(ls_shim_task_last_stack_depth(), 4096);
    LS_EQ_UINT(ls_shim_task_last_memory_caps(),
               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    LS_EQ_UINT(ls_shim_queue_delete_count(), 1);

    app_switch_to(1);
    LS_EQ_STR(s_trace, "fP");
    LS_EQ_INT(app_current_index(), 1);
    LS_CHECK(!app_parked());

    app_switch_to(0);
    LS_EQ_STR(s_trace, "fPpF");
    LS_EQ_INT(app_current_index(), 0);

    /* A failed checked stop retains FM as the registry owner.  In
       particular P25 enter must not run until a later stop retry succeeds. */
    s_fm_stop_ok = false;
    app_switch_to(1);
    LS_EQ_STR(s_trace, "fPpFf");
    LS_EQ_INT(app_current_index(), 0);
    LS_CHECK(!app_parked());

    app_park();
    LS_EQ_STR(s_trace, "fPpFff");
    LS_CHECK(!app_parked());

    s_fm_stop_ok = true;
    app_switch_to(1);
    LS_EQ_STR(s_trace, "fPpFfffP");
    LS_EQ_INT(app_current_index(), 1);
    check_ui_fence();
}

static void check_ui_fence(void)
{
    /* No worker means fail closed, not a synchronous GUI stop. */
    unsigned before=s_trace_len;
    LS_EQ_UINT(app_ui_park_request(),0);
    app_switch_to(0); app_unpark();
    LS_EQ_UINT(s_trace_len,before);
    app_ui_park_release();
    ls_shim_task_reset(); ls_shim_queue_reset();
    app_switch_worker_start();
    app_switch_to(0); /* stale request before the fence */
    uint32_t first=app_ui_park_request();
    LS_CHECK(first!=0); LS_EQ_INT(app_ui_park_status(first),0);
    uint32_t latest=app_ui_park_request();
    LS_CHECK(latest!=first); LS_EQ_INT(app_ui_park_status(first),-1);
    app_switch_to(0); app_unpark();
    LS_CHECK(app_switch_service(0));
    LS_EQ_INT(app_ui_park_status(latest),1); LS_CHECK(app_parked());
    LS_EQ_INT(app_current_index(),1);
    LS_CHECK(!app_switch_service(0));
    app_ui_park_release(); app_switch_to(0); LS_CHECK(app_switch_service(0));
    LS_EQ_INT(app_current_index(),0);
    s_fm_stop_ok=false;
    uint32_t failed=app_ui_park_request(); LS_CHECK(app_switch_service(0));
    LS_EQ_INT(app_ui_park_status(failed),-1); LS_CHECK(!app_parked());
    s_fm_stop_ok=true;
    uint32_t retry=app_ui_park_request(); LS_CHECK(app_switch_service(0));
    LS_EQ_INT(app_ui_park_status(retry),1);
    ls_shim_queue_fail_send(pdTRUE);
    LS_EQ_UINT(app_ui_park_request(),0);
    app_ui_park_release();
    ls_shim_queue_fail_send(pdFALSE);
}
