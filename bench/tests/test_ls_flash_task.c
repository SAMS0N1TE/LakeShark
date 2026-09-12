/* LS_TEST_SOURCES: ${FW}/components/lakeshark/core/ls_flash_task.c
 *                  ${FW}/components/lakeshark/core/ls_nvs_safe.c
 *                  ${FW}/main/ble_link_host_task.c */

/* P4 TCM carries MALLOC_CAP_INTERNAL but cache_utils.c does not accept
 * a TCM stack while flash cache is disabled. then found that allocating
 * the safe NVS stack on demand could fail during startup. These cases pin the
 * static NVS lifecycle and the fixed NimBLE DRAM-checked stack. */

#include "ls_test.h"

#include "ble_link_host_task.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/task.h"
#include "ls_flash_task.h"
#include "ls_nvs_safe.h"

static int s_callback_count;

static esp_err_t nvs_callback(void *ctx)
{
    ++s_callback_count;
    return *(esp_err_t *)ctx;
}

static void inert_task(void *ctx) { (void)ctx; }

LS_CASE(nvs_dispatch_lifecycle_reuses_one_bounded_static_stack)
{
    ls_shim_task_reset();
    ls_shim_task_execute_on_create(pdTRUE);
    s_callback_count = 0;
    esp_err_t want = 37;

    LS_EQ_INT(ls_nvs_init(), ESP_OK);
    LS_EQ_INT(ls_nvs_init(), ESP_OK);
    LS_EQ_INT(ls_nvs_call(nvs_callback, &want, 0), want);
    StackType_t *reserved = ls_shim_task_last_static_stack();
    LS_EQ_INT(ls_nvs_call(nvs_callback, &want, 0), want);

    LS_EQ_INT(s_callback_count, 2);
    LS_EQ_UINT(ls_shim_task_static_create_count(), 2);
    LS_EQ_UINT(ls_shim_task_delete_count(), 2);
    LS_CHECK(reserved != NULL);
    LS_CHECK(ls_shim_task_last_static_stack() == reserved);
    LS_EQ_UINT(ls_shim_task_last_stack_depth(), LS_NVS_WORKER_STACK_BYTES);
    LS_EQ_UINT(LS_NVS_WORKER_STACK_BYTES, 3072);
    LS_EQ_STR(ls_shim_task_last_name(), "ls_nvs");
    LS_EQ_UINT(ls_shim_task_delete_with_caps_count(), 0);
}

LS_CASE(nvs_dispatch_failure_releases_storage_for_retry)
{
    LS_EQ_INT(ls_nvs_init(), ESP_OK);
    ls_shim_task_reset();
    ls_shim_task_fail_create(pdTRUE);
    s_callback_count = 0;
    esp_err_t want = ESP_OK;

    LS_EQ_INT(ls_nvs_call(nvs_callback, &want, 2048), ESP_ERR_NO_MEM);
    LS_EQ_UINT(ls_shim_task_last_stack_depth(), 2048);
    LS_EQ_INT(s_callback_count, 0);
    LS_EQ_UINT(ls_shim_task_delete_count(), 0);
    LS_EQ_UINT(ls_shim_task_delete_with_caps_count(), 0);

    ls_shim_task_reset();
    ls_shim_task_execute_on_create(pdTRUE);
    LS_EQ_INT(ls_nvs_call(nvs_callback, &want, 2048), ESP_OK);
    LS_EQ_INT(s_callback_count, 1);
    LS_EQ_UINT(ls_shim_task_delete_count(), 1);

    LS_EQ_INT(ls_nvs_call(nvs_callback, &want,
                          LS_NVS_WORKER_STACK_BYTES + 1),
              ESP_ERR_INVALID_SIZE);
    LS_EQ_UINT(ls_shim_task_static_create_count(), 1);
}

typedef struct {
    int entered;
    int completed;
    esp_err_t result;
} callback_state_t;

static esp_err_t completing_callback(void *ctx)
{
    callback_state_t *state = (callback_state_t *)ctx;
    state->entered++;
    state->completed = 0x686;
    return state->result;
}

LS_CASE(nvs_dispatch_returns_only_after_callback_completion)
{
    LS_EQ_INT(ls_nvs_init(), ESP_OK);
    ls_shim_task_reset();
    ls_shim_task_execute_on_create(pdTRUE);
    callback_state_t state = { .result = 91 };

    LS_EQ_INT(ls_nvs_call(completing_callback, &state, 0), state.result);
    LS_EQ_INT(state.entered, 1);
    LS_EQ_INT(state.completed, 0x686);
    LS_EQ_UINT(ls_shim_task_static_create_count(), 1);
    LS_EQ_UINT(ls_shim_task_delete_count(), 1);
}

LS_CASE(nimble_host_owns_exact_dram_checked_stack)
{
    ls_shim_task_reset();
    TaskHandle_t task = ble_link_host_task_start(inert_task, 21, 0);

    LS_CHECK(task != NULL);
    LS_EQ_UINT(ls_shim_task_last_stack_depth(), BLE_LINK_HOST_STACK_BYTES);
    LS_EQ_UINT(BLE_LINK_HOST_STACK_BYTES, 5120);
    LS_EQ_STR(ls_shim_task_last_name(), "nimble_host");
    LS_CHECK(ls_shim_task_last_static_stack() != NULL);
}

LS_CASE(static_flash_task_rejects_non_dram_stack)
{
    ls_shim_task_reset();
    StackType_t stack[128];
    StaticTask_t tcb;
    ls_shim_ptr_in_dram_result = false;

    LS_CHECK(ls_flash_task_create_static(inert_task, "bad", sizeof(stack),
                                         NULL, 1, stack, &tcb, 0) == NULL);
    LS_EQ_UINT(ls_shim_task_last_stack_depth(), 0);
}
