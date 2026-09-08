#include "radio_decode_worker.h"

#include "esp_attr.h"

/* LS-726: a permanent 16 KiB FM stack remained reserved while P25 allocated
 * another 16 KiB cache-safe DSD stack.  LCD validation reached 19 DMA bytes,
 * so the successful FM-start reservation made the mutually exclusive P25
 * worker unsafe to start.
 *
 * Keep one fixed cache-safe stack and recreate only its small static TCB to
 * preserve FM/core-1 and DSD/core-0 placement.  Reuse is permitted only after
 * eTaskGetState reports eSuspended; deleting a non-running static task removes
 * its TCB synchronously, so the next create cannot overlap the old stack. */
static DRAM_ATTR StackType_t s_decode_stack[LS_RADIO_DECODE_STACK_BYTES]
    __attribute__((aligned(16)));
static DRAM_ATTR StaticTask_t s_decode_tcb;
static TaskHandle_t s_decode_task;
static TaskFunction_t s_run;

static void decode_worker(void *arg)
{
    (void)arg;
    TaskFunction_t run = s_run;
    if (run) run(NULL);

    /* release() observes the kernel state, not an optimistic software flag.
     * There must be no stack access after this suspension point. */
    vTaskSuspend(NULL);
}

bool ls_radio_decode_worker_start(TaskFunction_t run, const char *name,
                                  UBaseType_t priority, BaseType_t core_id)
{
    if (!run || !name || s_decode_task) return false;

    s_run = run;
    TaskHandle_t task = xTaskCreateStaticPinnedToCore(
        decode_worker, name, sizeof(s_decode_stack), NULL, priority,
        s_decode_stack, &s_decode_tcb, core_id);
    if (!task) {
        s_run = NULL;
        return false;
    }
    s_decode_task = task;
    return true;
}

bool ls_radio_decode_worker_release(unsigned wait_iterations,
                                    uint32_t wait_ms)
{
    TaskHandle_t task = s_decode_task;
    if (!task) return true;

    for (unsigned i = 0;; ++i) {
        if (eTaskGetState(task) == eSuspended) {
            vTaskDelete(task);
            s_decode_task = NULL;
            s_run = NULL;
            return true;
        }
        if (i >= wait_iterations) return false;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}
