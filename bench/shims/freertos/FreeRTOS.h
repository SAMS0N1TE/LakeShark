/* Host shim. Synchronization used by portable firmware logic is implemented
   with pthreads in freertos_shim.c; task creation and firmware scheduling are
   intentionally outside the host bench. */
#ifndef LS_SHIM_FREERTOS_H
#define LS_SHIM_FREERTOS_H

#include <stdlib.h>

#include <stdint.h>
#include <stddef.h>

typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef unsigned UBaseType_t;
typedef void *   TaskHandle_t;
typedef void   (*TaskFunction_t)(void *);
typedef uint8_t  StackType_t;
typedef struct { uintptr_t opaque[48]; } StaticTask_t;
typedef struct { uintptr_t opaque[16]; } StaticSemaphore_t;
typedef struct { uintptr_t opaque[32]; } StaticQueue_t;
typedef void *   QueueHandle_t;
typedef void *   SemaphoreHandle_t;
typedef void *   EventGroupHandle_t;
typedef void *   StreamBufferHandle_t;
typedef void *   TimerHandle_t;
typedef int      portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux)  ((void)(mux))

#define pdTRUE          1
#define pdFALSE         0
#define pdPASS          1
#define pdFAIL          0
#define portMAX_DELAY   0xFFFFFFFFu
#define tskNO_AFFINITY  (-1)
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portTICK_PERIOD_MS 1

static inline void *pvPortMalloc(size_t n) { return malloc(n); }
static inline void  vPortFree(void *p)     { free(p); }

#endif
