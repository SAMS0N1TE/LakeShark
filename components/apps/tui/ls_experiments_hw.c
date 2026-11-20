/* The board's half of ls_experiments.h: who may have the LoRa socket, taking
   it from MeshCore, and the worker task that runs the experiment. */
#include "esp_heap_caps.h"
#include "ls_experiments.h"

#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ls_field.h"
#include "ls_lora.h"
#include "ls_mesh.h"

const char *ls_exp_hw_radio_busy(void)
{
    if (!ls_lora_present()) return "No LoRa chip answered in the socket";
    if (ls_field_owned()) return "LoRa Labs owns the radio; turn DIRECT off first";
    /* P25 or ADS-B left listening on the chip gives it up to the take below. */
    const bool bg = ls_mesh_radio_bg();
    if (!bg && ls_lora_modes_active()) return "ADS-B has the LoRa chip in a Mode S session";
    if (ls_lora_scanning()) return "A sweep is using the LoRa chip";
    if (!bg && ls_lora_fsk_active()) return "An FSK session is using the LoRa chip";
    if (ls_lora_ook_active()) return "An OOK session is using the LoRa chip";
    return NULL;
}

/* A mesh node transmits for tens of milliseconds at a time, so the first
   ask often lands mid-frame: wait it out for a second, as `lora scan`
   does. */
bool ls_exp_hw_radio_take(void)
{
    const int64_t deadline = esp_timer_get_time() + 1000000;
    while (!ls_mesh_radio_hold(true) && esp_timer_get_time() < deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (ls_mesh_radio_held()) return true;
    ls_mesh_radio_hold(false);
    return false;
}

void ls_exp_hw_radio_give(void) { ls_mesh_radio_hold(false); }

/* The stack in PSRAM and the TCB in DRAM, created once and never deleted:
   a dynamic WithCaps task would take its TCB from the internal heap, which
   is nearly gone by the time the TUI runs. */
#define EXP_STACK_WORDS (6144 / sizeof(StackType_t))
/* Allocated at first wake, not static: built as an app module (ls_mod), this
   file's statics live in PSRAM, and FreeRTOS asserts on a PSRAM TCB. */
static StackType_t *s_stack;
static StaticTask_t *s_tcb; /* mod-lint: ok pointer only, the TCB is heap_caps INTERNAL */
static TaskHandle_t s_task;
/* 0 none yet, 1 being created, 2 running, 3 could not be created. */
static int s_task_state;

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        if (ls_exp_service()) vTaskDelay(pdMS_TO_TICKS(2));
        else ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

bool ls_exp_hw_wake(void)
{
    int expect = 0;
    if (__atomic_compare_exchange_n(&s_task_state, &expect, 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (!s_tcb) s_tcb = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); /* mod-lint: ok internal TCB */
        if (!s_stack) s_stack = heap_caps_malloc(EXP_STACK_WORDS * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        s_task = s_tcb && s_stack
                     ? xTaskCreateStaticPinnedToCore(worker, /* mod-lint: ok internal TCB, PSRAM stack never writes flash */ "exp", EXP_STACK_WORDS, NULL, 2,
                                                     s_stack, s_tcb, tskNO_AFFINITY)
                     : NULL;
        __atomic_store_n(&s_task_state, s_task ? 2 : 3, __ATOMIC_RELEASE);
    }
    while (__atomic_load_n(&s_task_state, __ATOMIC_ACQUIRE) == 1) vTaskDelay(1);
    if (!s_task) return false;
    xTaskNotifyGive(s_task);
    return true;
}
