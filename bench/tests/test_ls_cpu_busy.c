/* LS_TEST_SOURCES: components/lakeshark/core/ls_cpu_busy.c */

#include "ls_test.h"
#include "ls_cpu_busy.h"

#include <limits.h>
#include "freertos/task.h"
#include "esp_timer.h"

LS_CASE(cpu_busy_reads_only_idle_counters_without_stack_scans)
{
    int core0 = -1, core1 = -1;
    ls_shim_time_set(1000);
    ls_shim_idle_runtime(100, 200);
    LS_CHECK(!ls_cpu_busy(&core0, &core1));
    ls_shim_time_advance(1000);
    ls_shim_idle_runtime(350, 700);
    LS_CHECK(ls_cpu_busy(&core0, &core1));
    LS_EQ_INT(core0, 75);
    LS_EQ_INT(core1, 50);
    LS_EQ_INT(ls_shim_task_info_calls(), 4);
    LS_EQ_INT(ls_shim_task_stack_scan_calls(), 0);
    LS_EQ_INT(ls_shim_system_state_calls(), 0);
}

LS_CASE(cpu_busy_uses_wall_clock_elapsed)
{
    int busy = -1;
    LS_CHECK(ls_cpu_busy_from_samples(1000u, 1250u, 1000u, &busy));
    LS_EQ_INT(busy, 75);
}

LS_CASE(cpu_busy_idle_counter_wraps)
{
    int busy = -1;
    LS_CHECK(ls_cpu_busy_from_samples(UINT32_MAX - 49u, 50u, 200u, &busy));
    LS_EQ_INT(busy, 50);
}

LS_CASE(cpu_busy_rejects_zero_elapsed)
{
    int busy = 37;
    LS_CHECK(!ls_cpu_busy_from_samples(100u, 200u, 0u, &busy));
    LS_EQ_INT(busy, 37);
}

LS_CASE(cpu_busy_clamps_idle_delta_larger_than_elapsed)
{
    int busy = -1;
    LS_CHECK(ls_cpu_busy_from_samples(100u, 1201u, 1000u, &busy));
    LS_EQ_INT(busy, 0);
}
