/* Included only when compiling the P4 IDF FreeRTOS tasks/port sources.
 * A cache-enabled test in an IRAM tick hook races cache suspension by the
 * other core. Keep its task/name lookups in IRAM, along with the scheduler
 * suspend/resume closure used by the flash IPC protocol. Section attributes
 * on these declarations are inherited by the definitions; IDF's flash
 * mappings match .text.<symbol>, not these .iram1 sections.
 */
#pragma once
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void IRAM_ATTR vTaskSuspendAll(void);
BaseType_t IRAM_ATTR xTaskResumeAll(void);
TaskHandle_t IRAM_ATTR xTaskGetCurrentTaskHandleForCore(BaseType_t core);
char *IRAM_ATTR pcTaskGetName(TaskHandle_t task);
void IRAM_ATTR vPortYield(void);
