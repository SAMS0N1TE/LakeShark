/* ESP32-P4 TCM is MALLOC_CAP_INTERNAL but is not accepted by
 * esp_task_stack_is_sane_cache_disabled().  Flash callers must select DRAM,
 * not merely internal memory. */

#include "ls_flash_task.h"

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

#define LS_FLASH_STACK_CAPS \
    (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)

BaseType_t ls_flash_task_create(TaskFunction_t fn, const char *name,
                                uint32_t stack_bytes, void *arg,
                                UBaseType_t priority, TaskHandle_t *created,
                                BaseType_t core_id)
{
    return xTaskCreatePinnedToCoreWithCaps(fn, name, stack_bytes, arg, priority,
                                           created, core_id,
                                           LS_FLASH_STACK_CAPS);
}

void ls_flash_task_delete(TaskHandle_t task)
{
    vTaskDeleteWithCaps(task);
}

TaskHandle_t ls_flash_task_create_static(TaskFunction_t fn, const char *name,
                                         uint32_t stack_bytes, void *arg,
                                         UBaseType_t priority,
                                         StackType_t *stack,
                                         StaticTask_t *tcb,
                                         BaseType_t core_id)
{
    if (!fn || !stack || !tcb || stack_bytes == 0) return NULL;

    const unsigned char *first = (const unsigned char *)stack;
    const unsigned char *last = first + stack_bytes - 1;
    if (!esp_ptr_in_dram(first) || !esp_ptr_in_dram(last)) return NULL;

    return xTaskCreateStaticPinnedToCore(fn, name, stack_bytes, arg, priority,
                                         stack, tcb, core_id);
}

void ls_flash_task_delete_static(TaskHandle_t task)
{
    vTaskDelete(task);
}
