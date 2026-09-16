#ifndef LS_SHIM_TASK_H
#define LS_SHIM_TASK_H
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *pcTaskName;
    uint32_t ulRunTimeCounter;
    BaseType_t xCoreID;
} TaskStatus_t;

typedef enum {
    eRunning = 0,
    eReady,
    eBlocked,
    eSuspended,
    eDeleted,
    eInvalid,
} eTaskState;

void vTaskDelay(TickType_t ticks);
TaskHandle_t xTaskGetIdleTaskHandleForCore(BaseType_t core);
void vTaskGetInfo(TaskHandle_t task, TaskStatus_t *info, BaseType_t scan_stack, eTaskState state);
void ls_shim_idle_runtime(uint32_t core0, uint32_t core1);
unsigned ls_shim_task_info_calls(void);
unsigned ls_shim_task_stack_scan_calls(void);
unsigned ls_shim_system_state_calls(void);
UBaseType_t uxTaskGetNumberOfTasks(void);
UBaseType_t uxTaskGetSystemState(TaskStatus_t *tasks,
                                 UBaseType_t capacity,
                                 uint32_t *total_runtime);
BaseType_t xTaskCreatePinnedToCore(void (*task)(void *), const char *name,
                                   uint32_t stack_depth, void *arg,
                                   UBaseType_t priority,
                                   TaskHandle_t *created_task,
                                   BaseType_t core_id);
BaseType_t xTaskCreate(TaskFunction_t task, const char *name,
                       uint32_t stack_depth, void *arg,
                       UBaseType_t priority, TaskHandle_t *created_task);
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t task,
                                          const char *name,
                                          uint32_t stack_depth, void *arg,
                                          UBaseType_t priority,
                                          TaskHandle_t *created_task,
                                          BaseType_t core_id,
                                          UBaseType_t memory_caps);
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t task,
                                           const char *name,
                                           uint32_t stack_depth, void *arg,
                                           UBaseType_t priority,
                                           StackType_t *stack,
                                           StaticTask_t *tcb,
                                           BaseType_t core_id);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t wait_ticks);
void vTaskDelete(TaskHandle_t task);
void vTaskDeleteWithCaps(TaskHandle_t task);
void vTaskSuspend(TaskHandle_t task);
eTaskState eTaskGetState(TaskHandle_t task);

/* Host-test controls and observations for task-placement contracts. */
void ls_shim_task_reset(void);
void ls_shim_task_execute_on_create(BaseType_t enabled);
void ls_shim_task_fail_create(BaseType_t enabled);
uint32_t ls_shim_task_last_stack_depth(void);
UBaseType_t ls_shim_task_last_memory_caps(void);
StackType_t *ls_shim_task_last_static_stack(void);
const char *ls_shim_task_last_name(void);
unsigned ls_shim_task_delete_with_caps_count(void);
unsigned ls_shim_task_delete_count(void);
unsigned ls_shim_task_static_create_count(void);
unsigned ls_shim_task_notify_count(void);
void ls_shim_task_set_state(eTaskState state);
void ls_shim_task_suspend_lag(unsigned reads);
unsigned ls_shim_task_running_delete_count(void);
unsigned ls_shim_task_state_read_count(void);
BaseType_t ls_shim_task_last_core_id(void);
UBaseType_t ls_shim_task_last_priority(void);

#ifdef __cplusplus
}
#endif
#endif
