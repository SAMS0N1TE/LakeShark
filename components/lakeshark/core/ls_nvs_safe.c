/* see ls_nvs_safe.h for why this exists. */

#include "ls_nvs_safe.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "ls_flash_task.h"

static const char *TAG = "ls_nvs";

typedef struct {
    ls_nvs_fn_t       fn;
    void             *ctx;
    esp_err_t         err;
    SemaphoreHandle_t done;
} ls_nvs_job_t;

static DRAM_ATTR StackType_t s_worker_stack[LS_NVS_WORKER_STACK_BYTES]
    __attribute__((aligned(16)));
static DRAM_ATTR StaticTask_t s_worker_tcb;
static DRAM_ATTR StaticSemaphore_t s_worker_lock_storage;
static SemaphoreHandle_t s_worker_lock;

static void ls_nvs_worker(void *arg)
{
    ls_nvs_job_t *j = (ls_nvs_job_t *)arg;
    j->err = j->fn ? j->fn(j->ctx) : ESP_ERR_INVALID_ARG;
    xSemaphoreGive(j->done);
    /* Suspend after publishing the result so the caller can safely delete the
       transient task before another dispatch reuses its static storage. */
    vTaskSuspend(NULL);
}

esp_err_t ls_nvs_init(void)
{
    if (s_worker_lock) return ESP_OK;
    s_worker_lock = xSemaphoreCreateMutexStatic(&s_worker_lock_storage);
    if (!s_worker_lock) {
        ESP_LOGE(TAG, "static NVS dispatch lock initialization failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ls_nvs_call(ls_nvs_fn_t fn, void *ctx, unsigned stack_bytes)
{
    if (!fn) return ESP_ERR_INVALID_ARG;
    if (!s_worker_lock) {
        ESP_LOGE(TAG, "NVS worker used before ls_nvs_init - not persisted");
        return ESP_ERR_INVALID_STATE;
    }
    if (stack_bytes == 0) stack_bytes = LS_NVS_WORKER_STACK_BYTES;
    if (stack_bytes > sizeof(s_worker_stack)) return ESP_ERR_INVALID_SIZE;

    if (xSemaphoreTake(s_worker_lock, portMAX_DELAY) != pdTRUE)
        return ESP_ERR_INVALID_STATE;

    /* The job lives on the caller's stack and the caller blocks until the
       worker is finished with it, so it cannot go out of scope underneath.
       The worker only ever touches it before giving the semaphore. */
    ls_nvs_job_t j = { .fn = fn, .ctx = ctx, .err = ESP_FAIL };
    StaticSemaphore_t done_storage;

    j.done = xSemaphoreCreateBinaryStatic(&done_storage);
    if (!j.done) {
        xSemaphoreGive(s_worker_lock);
        return ESP_ERR_NO_MEM;
    }

    /* INTERNAL alone includes P4 TCM (0x30100000-0x30102000), but
       IDF 5.5.4's cache-off assertion accepts DRAM (0x4ff00000-0x4ffc0000),
       not TCM. The complete statically reserved stack range is checked by
       ls_flash_task_create_static before every dispatch. */
    TaskHandle_t worker = ls_flash_task_create_static(
        ls_nvs_worker, "ls_nvs", stack_bytes, &j, 5, s_worker_stack,
        &s_worker_tcb, tskNO_AFFINITY);
    if (!worker) {
        vSemaphoreDelete(j.done);
        xSemaphoreGive(s_worker_lock);
        ESP_LOGW(TAG, "cache-safe static NVS worker unavailable - not persisted");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(j.done, portMAX_DELAY);
    ls_flash_task_delete_static(worker);
    vSemaphoreDelete(j.done);
    xSemaphoreGive(s_worker_lock);
    return j.err;
}
