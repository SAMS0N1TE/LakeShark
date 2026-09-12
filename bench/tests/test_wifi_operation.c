#include "ls_test.h"
#include "ls_wifi_operation.h"

enum { BUSY = -99, DRIVER_FAILURE = -7 };

static ls_wifi_operation_t operation;
static int radio_running;
static int joins;
static int reads;
static int events;

static int join(void *context)
{
    (void)context;
    joins++;
    radio_running = 1;
    events = events * 10 + 3;
    return 0;
}

static int read_credentials(void *context)
{
    (void)context;
    reads++;
    return 0;
}

static int scan_with_console_interleaving(void *context)
{
    (void)context;
    radio_running = 1;
    events = events * 10 + 1;
    /* The exact defect: a UI-owned transient scan overlaps a console join.
     * No newer owner may start before this scan's stop/restore completes. */
    LS_EQ_INT(ls_wifi_operation_run(&operation, join, NULL, BUSY), BUSY);
    LS_EQ_INT(joins, 0);
    LS_EQ_INT(ls_wifi_operation_run(&operation, read_credentials, NULL, BUSY), BUSY);
    LS_EQ_INT(reads, 0);
    radio_running = 0;
    events = events * 10 + 2;
    return 0;
}

LS_CASE(scan_cleanup_cannot_stop_a_later_console_join)
{
    operation = (ls_wifi_operation_t){0};
    radio_running = joins = reads = events = 0;
    LS_EQ_INT(ls_wifi_operation_run(&operation, scan_with_console_interleaving,
                                    NULL, BUSY), 0);
    LS_EQ_INT(events, 12);
    LS_EQ_INT(radio_running, 0);
    LS_EQ_INT(ls_wifi_operation_run(&operation, join, NULL, BUSY), 0);
    LS_EQ_INT(events, 123);
    LS_EQ_INT(joins, 1);
    LS_EQ_INT(radio_running, 1);
    LS_EQ_INT(ls_wifi_operation_run(&operation, read_credentials, NULL, BUSY), 0);
    LS_EQ_INT(reads, 1);
}

static int fail_driver(void *context)
{
    (void)context;
    return DRIVER_FAILURE;
}

LS_CASE(driver_failure_releases_operation_and_busy_never_executes_a_callback)
{
    operation = (ls_wifi_operation_t){0};
    LS_EQ_INT(ls_wifi_operation_run(&operation, fail_driver, NULL, BUSY), DRIVER_FAILURE);
    LS_CHECK(ls_wifi_operation_try(&operation));
    LS_CHECK(!ls_wifi_operation_try(&operation));
    LS_EQ_INT(ls_wifi_operation_run(&operation, NULL, NULL, BUSY), BUSY);
    ls_wifi_operation_end(&operation);
    LS_EQ_INT(ls_wifi_operation_run(&operation, fail_driver, NULL, BUSY), DRIVER_FAILURE);
}

static int internal_leave(void *context)
{
    int *calls = context;
    LS_CHECK(!ls_wifi_operation_try(&operation));
    (*calls)++;
    return 0;
}

static int internal_join(void *context)
{
    /* Composite operations call their internal helpers without reacquiring. */
    LS_EQ_INT(internal_leave(context), 0);
    return internal_leave(context);
}

LS_CASE(composite_operation_retains_ownership_through_nested_helpers)
{
    operation = (ls_wifi_operation_t){0};
    int calls = 0;
    LS_EQ_INT(ls_wifi_operation_run(&operation, internal_join, &calls, BUSY), 0);
    LS_EQ_INT(calls, 2);
    LS_CHECK(ls_wifi_operation_try(&operation));
    ls_wifi_operation_end(&operation);
}

static int init_result;
static int init_calls;

static int initialize_nvs(void)
{
    init_calls++;
    return init_result;
}

LS_CASE(nvs_initialization_errors_are_propagated_without_destructive_recovery)
{
    bool ready = false;
    init_calls = 0;
    const int errors[] = {0x110d, 0x1110, -1};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        init_result = errors[i];
        LS_EQ_INT(ls_wifi_nvs_prepare(&ready, initialize_nvs), errors[i]);
        LS_CHECK(!ready);
        LS_EQ_INT(init_calls, i + 1);
    }
    init_result = 0;
    LS_EQ_INT(ls_wifi_nvs_prepare(&ready, initialize_nvs), 0);
    LS_CHECK(ready);
    LS_EQ_INT(init_calls, 4);
    init_result = -1;
    LS_EQ_INT(ls_wifi_nvs_prepare(&ready, initialize_nvs), 0);
    LS_EQ_INT(init_calls, 4);
}
