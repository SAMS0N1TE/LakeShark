/*
 * e3d private: the platform layer. Everything the engine needs from the
 * system is here, for three backends:
 *
 *   E3D_HOST      a desktop build (one thread, the C library)
 *   ESP_PLATFORM  an ESP-IDF component (FreeRTOS, heap_caps, esp_timer)
 *   otherwise     a PAPP for the ESP32_P4_Launcher (the PAPP SDK)
 *
 * E3D_PORT_PAPP forces the PAPP backend.
 *
 *   E3D_FAST                     code that runs from internal SRAM
 *   e3d_port_time_us()           a microsecond clock
 *   e3d_port_alloc_sram(n)       64-aligned, internal HP SRAM, or NULL
 *   e3d_port_alloc_any(n)        64-aligned, anywhere
 *   e3d_port_free_aligned(p)     for both of the above
 *   e3d_port_sram_free()         free internal heap, in bytes
 *   e3d_port_dma_free()          free DMA-capable internal heap (the SD card and Wi-Fi need some)
 *   e3d_port_ext_alloc/free      the engine's own heap (PSRAM when there is one),
 *                                16-byte aligned: PIE loads and stores need it
 *   E3D_PORT_THREADS             1 when there is a second core to run on
 *   e3d_port_sem_* / thread_*    a binary semaphore and a thread with join
 *   E3D_LOG(fmt, ...)            a printf-style log line (no prefix or newline)
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* The internal heap can also hand out the P4's LP SRAM (0x5010_8000), which
 * is uncached and far slower than PSRAM through the caches: blocks there are
 * given back. HP SRAM (L2MEM) is 0x4FF0_0000 .. 0x4FFC_0000. */
static inline bool e3d_port_is_hp_sram(const void *p, size_t n)
{
    return (uintptr_t)p >= 0x4FF00000u && (uintptr_t)p + n <= 0x4FFC0000u;
}

/* ════════════════════════════════════════════════════════════════════════ */
#if defined(E3D_HOST)

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define E3D_FAST
#define E3D_PORT_THREADS 0
#define E3D_LOG(fmt, ...) fprintf(stderr, "e3d: " fmt "\n", ##__VA_ARGS__)

static inline int64_t e3d_port_time_us(void) { return (int64_t)clock() * 1000000 / CLOCKS_PER_SEC; }
static inline void *e3d_port_alloc_sram(size_t n) { (void)n; return NULL; }
static inline void *e3d_port_alloc_any(size_t n) { return malloc(n); }
static inline void e3d_port_free_aligned(void *p) { free(p); }
static inline size_t e3d_port_sram_free(void) { return 0; }
static inline size_t e3d_port_dma_free(void) { return 0; }
static inline void *e3d_port_ext_alloc(size_t n) { return malloc(n); }
static inline void e3d_port_ext_free(void *p) { free(p); }

/* ════════════════════════════════════════════════════════════════════════ */
#elif defined(ESP_PLATFORM) && !defined(E3D_PORT_PAPP)

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define E3D_FAST IRAM_ATTR
#define E3D_PORT_THREADS (CONFIG_FREERTOS_NUMBER_OF_CORES > 1)
#define E3D_LOG(fmt, ...) ESP_LOGW("e3d", fmt, ##__VA_ARGS__)

static inline int64_t e3d_port_time_us(void) { return esp_timer_get_time(); }

static inline void *e3d_port_alloc_sram(size_t n)
{
    void *p = heap_caps_aligned_alloc(64, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p && !e3d_port_is_hp_sram(p, n)) {
        heap_caps_free(p);
        p = NULL;
    }
    return p;
}

static inline void *e3d_port_alloc_any(size_t n)
{
    void *p = e3d_port_alloc_sram(n);
    return p ? p : heap_caps_aligned_alloc(64, n, MALLOC_CAP_8BIT);
}

static inline void e3d_port_free_aligned(void *p) { heap_caps_free(p); }
static inline size_t e3d_port_sram_free(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
static inline size_t e3d_port_dma_free(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA); }

static inline void *e3d_port_ext_alloc(size_t n)
{
    void *p = heap_caps_aligned_alloc(16, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_aligned_alloc(16, n, MALLOC_CAP_8BIT);
}
static inline void e3d_port_ext_free(void *p) { heap_caps_free(p); }

typedef SemaphoreHandle_t e3d_port_sem;
static inline e3d_port_sem e3d_port_sem_create(void) { return xSemaphoreCreateBinary(); }
static inline void e3d_port_sem_give(e3d_port_sem s) { xSemaphoreGive(s); }
/* ms < 0: forever */
static inline bool e3d_port_sem_take(e3d_port_sem s, int ms)
{
    return xSemaphoreTake(s, ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(ms)) == pdTRUE;
}
static inline void e3d_port_sem_destroy(e3d_port_sem s) { vSemaphoreDelete(s); }

/* A FreeRTOS task may not return: the trampoline signals the joiner and
 * deletes itself. */
typedef struct {
    int (*fn)(void *);
    void *arg;
    SemaphoreHandle_t exited;
} e3d_port_thread_s;
typedef e3d_port_thread_s *e3d_port_thread;

static inline void e3d_port_trampoline(void *p)
{
    e3d_port_thread t = p;
    t->fn(t->arg);
    xSemaphoreGive(t->exited);
    vTaskDelete(NULL);
}

static inline e3d_port_thread e3d_port_thread_start(int (*fn)(void *), void *arg, int core, int stack)
{
    e3d_port_thread t = heap_caps_calloc(1, sizeof(*t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!t)
        return NULL;
    t->fn = fn;
    t->arg = arg;
    t->exited = xSemaphoreCreateBinary();
    /* the priority of the caller, so both halves of a frame get the same share */
    UBaseType_t prio = uxTaskPriorityGet(NULL);
    if (!t->exited || xTaskCreatePinnedToCore(e3d_port_trampoline, "e3d_worker", stack, t, prio, NULL,
                                              core) != pdPASS) {
        if (t->exited)
            vSemaphoreDelete(t->exited);
        heap_caps_free(t);
        return NULL;
    }
    return t;
}

static inline void e3d_port_thread_join(e3d_port_thread t)
{
    xSemaphoreTake(t->exited, portMAX_DELAY);
    vSemaphoreDelete(t->exited);
    heap_caps_free(t);
}

static inline bool e3d_port_threads_available(void) { return true; }

/* ════════════════════════════════════════════════════════════════════════ */
#else   /* PAPP */

#include "papp/papp.h"

#define E3D_FAST PAPP_FAST
#define E3D_PORT_THREADS 1
#define E3D_LOG(fmt, ...) papp_log("e3d: " fmt "\n", ##__VA_ARGS__)

static inline int64_t e3d_port_time_us(void) { return papp_time_us(); }

static inline void *e3d_port_alloc_sram(size_t n)
{
    void *p = papp_alloc(n, PAPP_MEM_FAST, 64);
    if (p && !e3d_port_is_hp_sram(p, n)) {
        papp_free(p);
        p = NULL;
    }
    return p;
}

static inline void *e3d_port_alloc_any(size_t n)
{
    void *p = e3d_port_alloc_sram(n);
    return p ? p : papp_alloc(n, PAPP_MEM_ANY, 64);
}

static inline void e3d_port_free_aligned(void *p) { papp_free(p); }
static inline size_t e3d_port_sram_free(void) { return papp_mem_free_size(PAPP_MEM_FAST); }
static inline size_t e3d_port_dma_free(void) { return papp_mem_free_size(PAPP_MEM_FAST | PAPP_MEM_DMA); }

/* On the launcher, malloc puts blocks under 1 KB in internal SRAM */
static inline void *e3d_port_ext_alloc(size_t n) { return papp_alloc(n, PAPP_MEM_LARGE, 16); }
static inline void e3d_port_ext_free(void *p) { papp_free(p); }

typedef papp_sem_t *e3d_port_sem;
static inline e3d_port_sem e3d_port_sem_create(void) { return papp_sem_create(0, 1); }
static inline void e3d_port_sem_give(e3d_port_sem s) { papp_sem_give(s); }
static inline bool e3d_port_sem_take(e3d_port_sem s, int ms)
{
    return papp_sem_take(s, ms < 0 ? PAPP_WAIT_FOREVER : (uint32_t)ms);
}
static inline void e3d_port_sem_destroy(e3d_port_sem s) { papp_sem_destroy(s); }

typedef papp_thread_t *e3d_port_thread;
static inline e3d_port_thread e3d_port_thread_start(int (*fn)(void *), void *arg, int core, int stack)
{
    papp_thread_cfg_t cfg = { .name = "e3d_worker", .stack_size = (uint32_t)stack,
                              .prio = PAPP_PRIO_NORMAL, .core = (int8_t)core };
    return papp_thread_create(fn, arg, &cfg);
}
static inline void e3d_port_thread_join(e3d_port_thread t) { papp_thread_join(t, PAPP_WAIT_FOREVER, NULL); }
static inline bool e3d_port_threads_available(void) { return papp_if_thrd() != NULL; }

#endif
