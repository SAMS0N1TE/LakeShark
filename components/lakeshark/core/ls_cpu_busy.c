#include "ls_cpu_busy.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

bool ls_cpu_busy_from_samples(uint32_t previous_idle,
                              uint32_t current_idle,
                              uint64_t elapsed_us,
                              int *busy_pct)
{
    if (!busy_pct || elapsed_us == 0) return false;

    /* FreeRTOS's run-time counter is uint32_t. Unsigned subtraction
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

    uint32_t idle[2] = { 0, 0 };
    /* uxTaskGetSystemState scans every stack with the SMP kernel locked.
     * PSRAM stack scans can delay LCD DMA restart. CPU telemetry needs only
     * two permanent idle TCBs, never task enumeration or stack watermarks. */
    for (BaseType_t core = 0; core < 2; ++core) {
        TaskHandle_t handle = xTaskGetIdleTaskHandleForCore(core);
        if (!handle) return false;
        TaskStatus_t info;
        vTaskGetInfo(handle, &info, pdFALSE, eRunning);
        idle[core] = info.ulRunTimeCounter;
    }

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
