#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* All BLE board profiles configure this exact NimBLE host stack size.  The
 * compile-time assertion in ble_link.c prevents Kconfig from outgrowing it. */
#define BLE_LINK_HOST_STACK_BYTES 5120u

TaskHandle_t ble_link_host_task_start(TaskFunction_t fn,
                                      UBaseType_t priority,
                                      BaseType_t core_id);
void ble_link_host_task_exit(void);

#ifdef __cplusplus
}
#endif
