#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

bool ls_shim_ptr_in_dram_result = true;

static BaseType_t s_task_execute;
static BaseType_t s_task_fail;
static uint32_t s_task_stack_depth;
static UBaseType_t s_task_memory_caps;
static StackType_t *s_task_static_stack;
static char s_task_name[32];
static unsigned s_task_delete_caps_count;
static unsigned s_task_delete_count;
static unsigned s_task_static_create_count;
static unsigned s_task_notify_count;
static eTaskState s_task_state;
static BaseType_t s_task_core_id;
static UBaseType_t s_task_priority;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned head;
    unsigned tail;
    unsigned count;
    unsigned length;
    size_t item_size;
    unsigned char *data;
    bool dynamic;
} ls_host_queue_t;

static BaseType_t s_queue_fail_create;
static BaseType_t s_queue_fail_send;
static unsigned s_queue_delete_count;
static unsigned s_queue_static_create_count;

void ls_shim_queue_reset(void)
{
    s_queue_fail_create = pdFALSE;
    s_queue_fail_send = pdFALSE;
    s_queue_delete_count = 0;
    s_queue_static_create_count = 0;
}

void ls_shim_queue_fail_create(BaseType_t enabled)
{
    s_queue_fail_create = enabled;
}

void ls_shim_queue_fail_send(BaseType_t enabled)
{
    s_queue_fail_send = enabled;
}

unsigned ls_shim_queue_delete_count(void) { return s_queue_delete_count; }
unsigned ls_shim_queue_static_create_count(void)
{
    return s_queue_static_create_count;
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    if (s_queue_fail_create || length == 0 || item_size == 0) return NULL;
    ls_host_queue_t *queue = calloc(
        1, sizeof(*queue) + (size_t)length * (size_t)item_size);
    if (!queue) return NULL;
    queue->data = (unsigned char *)(queue + 1);
    queue->dynamic = true;
    if (pthread_mutex_init(&queue->lock, NULL) != 0 ||
        pthread_cond_init(&queue->changed, NULL) != 0) {
        free(queue);
        return NULL;
    }
    queue->length = length;
    queue->item_size = item_size;
    return (QueueHandle_t)queue;
}

QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size,
                                 uint8_t *storage, StaticQueue_t *queue_storage)
{
    if (length == 0 || item_size == 0 || !storage || !queue_storage)
        return NULL;
    _Static_assert(sizeof(ls_host_queue_t) <= sizeof(StaticQueue_t),
                   "host StaticQueue_t storage is too small");
    ls_host_queue_t *queue = (ls_host_queue_t *)queue_storage;
    memset(queue, 0, sizeof(*queue));
    if (pthread_mutex_init(&queue->lock, NULL) != 0 ||
        pthread_cond_init(&queue->changed, NULL) != 0) {
        return NULL;
    }
    queue->length = length;
    queue->item_size = item_size;
    queue->data = storage;
    queue->dynamic = false;
    ++s_queue_static_create_count;
    return (QueueHandle_t)queue;
}

BaseType_t xQueueSend(QueueHandle_t handle, const void *item, TickType_t ticks)
{
    (void)ticks;
    ls_host_queue_t *queue = (ls_host_queue_t *)handle;
    if (!queue || !item || s_queue_fail_send) return pdFALSE;
    pthread_mutex_lock(&queue->lock);
    if (queue->count == queue->length) {
        pthread_mutex_unlock(&queue->lock);
        return pdFALSE;
    }
    memcpy(&queue->data[(size_t)queue->head * queue->item_size], item,
           queue->item_size);
    queue->head = (queue->head + 1) % queue->length;
    ++queue->count;
    pthread_cond_signal(&queue->changed);
    pthread_mutex_unlock(&queue->lock);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t handle, void *item, TickType_t ticks)
{
    ls_host_queue_t *queue = (ls_host_queue_t *)handle;
    if (!queue || !item) return pdFALSE;
    pthread_mutex_lock(&queue->lock);
    if (queue->count == 0 && ticks == portMAX_DELAY) {
        while (queue->count == 0)
            pthread_cond_wait(&queue->changed, &queue->lock);
    }
    if (queue->count == 0) {
        pthread_mutex_unlock(&queue->lock);
        return pdFALSE;
    }
    memcpy(item, &queue->data[(size_t)queue->tail * queue->item_size],
           queue->item_size);
    queue->tail = (queue->tail + 1) % queue->length;
    --queue->count;
    pthread_mutex_unlock(&queue->lock);
    return pdTRUE;
}

BaseType_t xQueueReset(QueueHandle_t handle)
{
    ls_host_queue_t *queue = (ls_host_queue_t *)handle;
    if (!queue) return pdFALSE;
    pthread_mutex_lock(&queue->lock);
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    pthread_mutex_unlock(&queue->lock);
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t handle)
{
    ls_host_queue_t *queue = (ls_host_queue_t *)handle;
    if (!queue) return;
    pthread_cond_destroy(&queue->changed);
    pthread_mutex_destroy(&queue->lock);
    if (queue->dynamic) free(queue);
    ++s_queue_delete_count;
}

void ls_shim_task_reset(void)
{
    s_task_execute = pdFALSE;
    s_task_fail = pdFALSE;
    s_task_stack_depth = 0;
    s_task_memory_caps = 0;
    s_task_static_stack = NULL;
    s_task_name[0] = '\0';
    s_task_delete_caps_count = 0;
    s_task_delete_count = 0;
    s_task_static_create_count = 0;
    s_task_notify_count = 0;
    s_task_state = eReady;
    s_task_core_id = tskNO_AFFINITY;
    s_task_priority = 0;
    ls_shim_ptr_in_dram_result = true;
}

void ls_shim_task_execute_on_create(BaseType_t enabled) { s_task_execute = enabled; }
void ls_shim_task_fail_create(BaseType_t enabled) { s_task_fail = enabled; }
uint32_t ls_shim_task_last_stack_depth(void) { return s_task_stack_depth; }
UBaseType_t ls_shim_task_last_memory_caps(void) { return s_task_memory_caps; }
StackType_t *ls_shim_task_last_static_stack(void) { return s_task_static_stack; }
const char *ls_shim_task_last_name(void) { return s_task_name; }
unsigned ls_shim_task_delete_with_caps_count(void) { return s_task_delete_caps_count; }
unsigned ls_shim_task_delete_count(void) { return s_task_delete_count; }
unsigned ls_shim_task_static_create_count(void) { return s_task_static_create_count; }
unsigned ls_shim_task_notify_count(void) { return s_task_notify_count; }
void ls_shim_task_set_state(eTaskState state) { s_task_state = state; }
BaseType_t ls_shim_task_last_core_id(void) { return s_task_core_id; }
UBaseType_t ls_shim_task_last_priority(void) { return s_task_priority; }

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned count;
    unsigned max_count;
    bool dynamic;
} ls_host_semaphore_t;

static SemaphoreHandle_t create_semaphore(unsigned max_count,
                                          unsigned initial_count)
{
    ls_host_semaphore_t *sem = calloc(1, sizeof(*sem));
    if (!sem) return NULL;
    if (pthread_mutex_init(&sem->lock, NULL) != 0 ||
        pthread_cond_init(&sem->changed, NULL) != 0) {
        free(sem);
        return NULL;
    }
    sem->count = initial_count;
    sem->max_count = max_count;
    sem->dynamic = true;
    return (SemaphoreHandle_t)sem;
}

static SemaphoreHandle_t create_semaphore_static(StaticSemaphore_t *storage,
                                                  unsigned max_count,
                                                  unsigned initial_count)
{
    if (!storage || max_count == 0 || initial_count > max_count) return NULL;
    _Static_assert(sizeof(ls_host_semaphore_t) <= sizeof(StaticSemaphore_t),
                   "host StaticSemaphore_t storage is too small");
    ls_host_semaphore_t *sem = (ls_host_semaphore_t *)storage;
    memset(sem, 0, sizeof(*sem));
    if (pthread_mutex_init(&sem->lock, NULL) != 0 ||
        pthread_cond_init(&sem->changed, NULL) != 0) {
        return NULL;
    }
    sem->count = initial_count;
    sem->max_count = max_count;
    sem->dynamic = false;
    return (SemaphoreHandle_t)sem;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return create_semaphore(1, 1);
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return create_semaphore(1, 0);
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    return create_semaphore_static(storage, 1, 1);
}

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    return create_semaphore_static(storage, 1, 0);
}

SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max_count,
                                           UBaseType_t initial_count)
{
    if (max_count == 0 || initial_count > max_count) return NULL;
    return create_semaphore(max_count, initial_count);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks)
{
    ls_host_semaphore_t *sem = (ls_host_semaphore_t *)handle;
    if (!sem) return pdFALSE;
    pthread_mutex_lock(&sem->lock);
    int rc = 0;
    if (ticks == portMAX_DELAY) {
        while (sem->count == 0)
            pthread_cond_wait(&sem->changed, &sem->lock);
    } else if (ticks == 0) {
        if (sem->count == 0) rc = ETIMEDOUT;
    } else {
        struct timespec deadline;
        /* The Windows MinGW runtime used by the host gate does not
           expose C11 timespec_get. Use the pthread clock for its absolute
           condition-variable deadline, also supported by winpthreads. */
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += ticks / 1000;
        deadline.tv_nsec += (long)(ticks % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000L;
        }
        while (sem->count == 0 && rc == 0)
            rc = pthread_cond_timedwait(&sem->changed, &sem->lock,
                                        &deadline);
    }
    if (rc == 0 && sem->count != 0) --sem->count;
    pthread_mutex_unlock(&sem->lock);
    return rc == 0 ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    ls_host_semaphore_t *sem = (ls_host_semaphore_t *)handle;
    if (!sem) return pdFALSE;
    pthread_mutex_lock(&sem->lock);
    BaseType_t result = pdFALSE;
    if (sem->count < sem->max_count) {
        ++sem->count;
        pthread_cond_signal(&sem->changed);
        result = pdTRUE;
    }
    pthread_mutex_unlock(&sem->lock);
    return result;
}

void vSemaphoreDelete(SemaphoreHandle_t handle)
{
    ls_host_semaphore_t *sem = (ls_host_semaphore_t *)handle;
    if (!sem) return;
    bool dynamic = sem->dynamic;
    pthread_cond_destroy(&sem->changed);
    pthread_mutex_destroy(&sem->lock);
    if (dynamic) free(sem);
}

void vTaskDelay(TickType_t ticks)
{
    struct timespec delay = {
        .tv_sec = ticks / 1000,
        .tv_nsec = (long)(ticks % 1000) * 1000000L,
    };
    nanosleep(&delay, NULL);
}

UBaseType_t uxTaskGetNumberOfTasks(void)
{
    return 0;
}

UBaseType_t uxTaskGetSystemState(TaskStatus_t *tasks,
                                 UBaseType_t capacity,
                                 uint32_t *total_runtime)
{
    (void)tasks;
    (void)capacity;
    if (total_runtime) *total_runtime = 0;
    return 0;
}

BaseType_t xTaskCreatePinnedToCore(void (*task)(void *), const char *name,
                                   uint32_t stack_depth, void *arg,
                                   UBaseType_t priority,
                                   TaskHandle_t *created_task,
                                   BaseType_t core_id)
{
    s_task_stack_depth = stack_depth;
    snprintf(s_task_name, sizeof(s_task_name), "%s", name ? name : "");
    (void)arg;
    s_task_priority = priority;
    s_task_core_id = core_id;
    if (s_task_fail) return pdFAIL;
    if (created_task) *created_task = (TaskHandle_t)1;
    if (s_task_execute && task) task(arg);
    return pdPASS;
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t task,
                                          const char *name,
                                          uint32_t stack_depth, void *arg,
                                          UBaseType_t priority,
                                          TaskHandle_t *created_task,
                                          BaseType_t core_id,
                                          UBaseType_t memory_caps)
{
    (void)priority;
    (void)core_id;
    s_task_stack_depth = stack_depth;
    s_task_memory_caps = memory_caps;
    snprintf(s_task_name, sizeof(s_task_name), "%s", name ? name : "");
    if (s_task_fail) return pdFAIL;
    if (created_task) *created_task = (TaskHandle_t)(uintptr_t)0x1234;
    if (s_task_execute && task) task(arg);
    return pdPASS;
}

TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t task,
                                           const char *name,
                                           uint32_t stack_depth, void *arg,
                                           UBaseType_t priority,
                                           StackType_t *stack,
                                           StaticTask_t *tcb,
                                           BaseType_t core_id)
{
    (void)arg;
    s_task_priority = priority;
    s_task_core_id = core_id;
    s_task_stack_depth = stack_depth;
    s_task_static_stack = stack;
    ++s_task_static_create_count;
    snprintf(s_task_name, sizeof(s_task_name), "%s", name ? name : "");
    if (s_task_fail || !stack || !tcb) return NULL;
    TaskHandle_t created = (TaskHandle_t)tcb;
    s_task_state = eReady;
    if (s_task_execute && task) {
        s_task_state = eRunning;
        task(arg);
    }
    return created;
}

BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    if (!task) return pdFAIL;
    ++s_task_notify_count;
    return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t wait_ticks)
{
    (void)clear_on_exit;
    (void)wait_ticks;
    return 1;
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
    s_task_state = eDeleted;
    ++s_task_delete_count;
}
void vTaskDeleteWithCaps(TaskHandle_t task)
{
    (void)task;
    ++s_task_delete_caps_count;
}
void vTaskSuspend(TaskHandle_t task)
{
    (void)task;
    s_task_state = eSuspended;
}

eTaskState eTaskGetState(TaskHandle_t task)
{
    (void)task;
    return s_task_state;
}
