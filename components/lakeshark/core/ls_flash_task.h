#pragma once

/* Tasks which call NVS or another SPI-flash API need a stronger stack
 * placement guarantee than MALLOC_CAP_INTERNAL provides on ESP32-P4. */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create a transient task whose stack is selected from cache-safe DRAM.
 * stack_bytes follows ESP-IDF's byte-count task API.  Delete the returned task
 * with ls_flash_task_delete(), not vTaskDelete(). */
BaseType_t ls_flash_task_create(TaskFunction_t fn, const char *name,
                                uint32_t stack_bytes, void *arg,
                                UBaseType_t priority, TaskHandle_t *created,
                                BaseType_t core_id);

void ls_flash_task_delete(TaskHandle_t task);

/* Create a task from caller-owned static storage after verifying the complete
 * stack range is in the DRAM range accepted while the flash cache is off. */
TaskHandle_t ls_flash_task_create_static(TaskFunction_t fn, const char *name,
                                         uint32_t stack_bytes, void *arg,
                                         UBaseType_t priority,
                                         StackType_t *stack,
                                         StaticTask_t *tcb,
                                         BaseType_t core_id);

/* Delete a suspended task created from caller-owned static storage. */
void ls_flash_task_delete_static(TaskHandle_t task);

#ifdef __cplusplus
}
#endif
