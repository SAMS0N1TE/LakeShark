/* NimBLE restores its NVS security database from its host task.  On
 * ESP32-P4 the ordinary allocator put this 5120-byte stack in TCM at
 * 0x30100000, which is internal but inaccessible to IDF's cache-off path. */

#include "ble_link_host_task.h"

#include "esp_attr.h"
#include "ls_flash_task.h"
#include "ls_static_task.h"

static StackType_t s_host_stack[BLE_LINK_HOST_STACK_BYTES]
    __attribute__((aligned(16)));
static StaticTask_t s_host_tcb;
static TaskHandle_t s_host_task;

static TaskHandle_t s_host_parked;   /* the last host task, parked when it ended */

TaskHandle_t ble_link_host_task_start(TaskFunction_t fn,
                                      UBaseType_t priority,
                                      BaseType_t core_id)
{
    if (s_host_task) return s_host_task;
    /* Bluetooth off and on again: the old task must be gone from the
       kernel's lists before its static TCB is reused (ls_static_task.h). */
    if (!ls_static_task_reap(&s_host_parked, 500)) return NULL;
    s_host_task = ls_flash_task_create_static(fn, "nimble_host",
                                               sizeof(s_host_stack), NULL,
                                               priority, s_host_stack,
                                               &s_host_tcb, core_id);
    return s_host_task;
}

void ble_link_host_task_exit(void)
{
    s_host_parked = xTaskGetCurrentTaskHandle();
    s_host_task = NULL;
    ls_static_task_park();   /* never vTaskDelete(NULL): ls_static_task.h */
}
