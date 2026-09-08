#include "ls_cpu_busy.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <string.h>

bool ls_cpu_busy_from_samples(uint32_t previous_idle,
                              uint32_t current_idle,
                              uint64_t elapsed_us,
                              int *busy_pct)
{
    if (!busy_pct || elapsed_us == 0) return false;

    /* LS-673: FreeRTOS's run-time counter is uint32_t. Unsigned subtraction
       deliberately gives the correct idle delta across one counter wrap. */
    uint32_t idle_delta = current_idle - previous_idle;
    uint64_t idle_pct = (uint64_t)idle_delta * 100u / elapsed_us;
    *busy_pct = idle_pct >= 100u ? 0 : 100 - (int)idle_pct;
    return true;
}

bool ls_cpu_busy(int *core0_pct, int *core1_pct)
{
    static uint32_t s_last_idle[2];
    static int64_t s_last_us;
    static bool s_have_sample;
    static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

    if (!core0_pct || !core1_pct) return false;

    UBaseType_t capacity = uxTaskGetNumberOfTasks();
    if (capacity == 0) return false;

    /* uxTaskGetSystemState writes this snapshot while the scheduler is
       suspended, so keep it in internal RAM rather than PSRAM. */
    TaskStatus_t *tasks = heap_caps_malloc(sizeof(*tasks) * capacity,
                                           MALLOC_CAP_INTERNAL);
    if (!tasks) return false;

    UBaseType_t count = uxTaskGetSystemState(tasks, capacity, NULL);
    uint32_t idle[2] = { 0, 0 };
    bool found[2] = { false, false };
    for (UBaseType_t i = 0; i < count; ++i) {
        if (tasks[i].pcTaskName &&
            strncmp(tasks[i].pcTaskName, "IDLE", 4) == 0) {
            BaseType_t core = tasks[i].xCoreID;
            if (core == 0 || core == 1) {
                idle[core] += tasks[i].ulRunTimeCounter;
                found[core] = true;
            }
        }
    }
    heap_caps_free(tasks);

    if (!found[0] || !found[1]) return false;

    int64_t now = esp_timer_get_time();
    bool valid = false;

    portENTER_CRITICAL(&s_lock);
    if (s_have_sample && now > s_last_us) {
        uint64_t elapsed_us = (uint64_t)(now - s_last_us);
        valid = ls_cpu_busy_from_samples(s_last_idle[0], idle[0], elapsed_us,
                                         core0_pct) &&
                ls_cpu_busy_from_samples(s_last_idle[1], idle[1], elapsed_us,
                                         core1_pct);
    }
    s_last_idle[0] = idle[0];
    s_last_idle[1] = idle[1];
    s_last_us = now;
    s_have_sample = true;
    portEXIT_CRITICAL(&s_lock);

    return valid;
}
