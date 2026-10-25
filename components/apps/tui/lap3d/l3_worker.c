/* One shared PSRAM-stack core-0 helper; host execution is deterministic
 * single-core. */
#include "l3_private.h"
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static EXT_RAM_BSS_ATTR struct {
    TaskHandle_t handle;
    l3_scene *scene;
    atomic_int state; /* 0 idle/cancelled, 1 queued, 2 reading a scene */
    unsigned scenes;
    bool attempted;
} helper;
static __attribute__((noinline)) void worker(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int queued = 1;
        if (!atomic_compare_exchange_strong_explicit(&helper.state, &queued, 2,
                                                     memory_order_acquire, memory_order_relaxed))
            continue;
        l3_scene *s = helper.scene;
        for (;;) {
            int band = atomic_fetch_add(&s->next_band, 1);
            if (band >= s->bands)
                break;
            l3_raster_band(s, band, 1);
            atomic_fetch_add_explicit(&s->completed, 1, memory_order_release);
        }
        atomic_store_explicit(&helper.state, 0, memory_order_release);
    }
}
void l3_worker_acquire(void) {
    helper.scenes++;
    /* Setup belongs to scene creation, never a recorded/rendered frame. */
    if (!helper.attempted) {
        helper.attempted = true;
        xTaskCreatePinnedToCoreWithCaps(worker, "lap3d", 4096, NULL, tskIDLE_PRIORITY + 1,
                                        &helper.handle, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
}
void l3_worker_release(void) {
    if (helper.scenes && !--helper.scenes) {
        if (helper.handle)
            vTaskDeleteWithCaps(helper.handle);
        helper.handle = NULL;
        helper.attempted = false;
    }
}
void l3_worker_start(l3_scene *s) {
    s->stats.cores = helper.handle ? 2 : 1;
    if (helper.handle) {
        helper.scene = s;
        atomic_store_explicit(&helper.state, 1, memory_order_release);
        xTaskNotifyGive(helper.handle);
    }
}
void l3_worker_finish(l3_scene *s) {
    (void)s; /* Caller has consumed all unclaimed bands; only a claimed band's
                tail remains. */
    int queued = 1;
    /* A starved core 0 need not wake just to acknowledge an empty job. */
    if (atomic_compare_exchange_strong_explicit(&helper.state, &queued, 0, memory_order_acq_rel,
                                                memory_order_relaxed))
        return;
    /* A band takes well under a millisecond, so poll briefly; if core 0 was
       taken from the helper mid-band, sleep rather than spin, so this core's
       idle task (and its watchdog) still run. */
    for (int spins = 0; helper.handle && atomic_load_explicit(&helper.state, memory_order_acquire) == 2;
         spins++)
        if (spins < 200)
            esp_rom_delay_us(5);
        else
            vTaskDelay(1);
}
#else
void l3_worker_acquire(void) {}
void l3_worker_release(void) {}
void l3_worker_start(l3_scene *s) { s->stats.cores = 1; }
void l3_worker_finish(l3_scene *s) { (void)s; }
#endif
