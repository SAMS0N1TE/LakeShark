#ifndef LS_STATIC_TASK_H
#define LS_STATIC_TASK_H

/* A task created in static memory (xTaskCreateStatic*) that ends itself must
   not vTaskDelete(NULL): its TCB stays linked on the kernel's termination list
   until an idle task runs, and creating the task again in the same StaticTask_t
   before that re-initialises list items that are still linked. The kernel's
   lists break: one core loops in the scheduler, the UI starves, and the stall
   watchdog aborts (the 11-25 FM/P25 radio-switch freeze signature).

   So it ends with ls_static_task_park() (suspends itself), and whoever creates
   it again first calls ls_static_task_reap() on the old handle: a task that is
   Suspended is current on no core (eTaskGetState checks that under the kernel
   lock), so deleting it from outside unlinks it at once. */

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static inline void ls_static_task_park(void)
{
    vTaskSuspend(NULL);
}

/* Deletes the parked task in *h and clears *h. False when it has not parked
   within wait_ms: then it must not be created again (the storage is in use). */
static inline bool ls_static_task_reap(TaskHandle_t *h, uint32_t wait_ms)
{
    if (!h || !*h) return true;
    for (uint32_t waited = 0;; waited += 5) {
        if (eTaskGetState(*h) == eSuspended) {
            vTaskDelete(*h);
            *h = NULL;
            return true;
        }
        if (waited >= wait_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(5) ? pdMS_TO_TICKS(5) : 1);
    }
}

#endif
