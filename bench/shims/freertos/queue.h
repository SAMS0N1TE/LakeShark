#ifndef LS_SHIM_QUEUE_H
#define LS_SHIM_QUEUE_H
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size,
                                 uint8_t *storage, StaticQueue_t *queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item,
                      TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item,
                         TickType_t ticks);
BaseType_t xQueueReset(QueueHandle_t queue);
void vQueueDelete(QueueHandle_t queue);

/* Host-test controls for deterministic allocation/fill failure injection. */
void ls_shim_queue_reset(void);
void ls_shim_queue_fail_create(BaseType_t enabled);
void ls_shim_queue_fail_send(BaseType_t enabled);
unsigned ls_shim_queue_delete_count(void);
unsigned ls_shim_queue_static_create_count(void);

#ifdef __cplusplus
}
#endif
#endif
