#include "ls_task_reap.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "task_reap";

/* Tasks that have retired and wait, suspended, to be deleted. */
static QueueHandle_t s_queue;

static void reaper(void *arg)
{
    (void)arg;
    for (;;) {
        TaskHandle_t t;
        /* vTaskDeleteWithCaps suspends the task, waits until it is not
           running, deletes it and frees its stack and TCB. */
        if (xQueueReceive(s_queue, &t, portMAX_DELAY) == pdTRUE) vTaskDeleteWithCaps(t);
    }
}

void ls_task_reap_start(void)
{
    if (s_queue) return;
    QueueHandle_t q = xQueueCreate(8, sizeof(TaskHandle_t));
    if (!q) {
        ESP_LOGE(TAG, "no memory for the reaper's queue");
        return;
    }
    /* Published before the reaper starts, which reads it at once. */
    s_queue = q;
    /* Its stack in PSRAM: it never touches flash. */
    if (xTaskCreatePinnedToCoreWithCaps(reaper, "task_reap", 2048, NULL, 2, NULL, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_queue = NULL;
        vQueueDelete(q);
        ESP_LOGE(TAG, "no memory for the reaper");
    }
}

void ls_task_retire_self(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (s_queue && xQueueSend(s_queue, &self, portMAX_DELAY) == pdTRUE) {
        for (;;) vTaskSuspend(NULL);
    }
    vTaskDeleteWithCaps(NULL);
    for (;;) vTaskSuspend(NULL);
}
