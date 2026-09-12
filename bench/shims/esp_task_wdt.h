#ifndef LS_SHIM_ESP_TASK_WDT_H
#define LS_SHIM_ESP_TASK_WDT_H

#include "esp_err.h"

static inline esp_err_t esp_task_wdt_status(void *task)
{
    (void)task;
    return -1;
}

static inline esp_err_t esp_task_wdt_reset(void)
{
    return ESP_OK;
}

#endif
