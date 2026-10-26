#pragma once
#include "FreeRTOS.h"
TaskHandle_t xTaskGetCurrentTaskHandle(void);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task);
unsigned uxTaskPriorityGet(TaskHandle_t task);
int xPortGetCoreID(void);
void vTaskSuspend(TaskHandle_t task);
void vTaskDelete(TaskHandle_t task);
unsigned ulTaskNotifyTake(int clear,unsigned wait);
void xTaskNotifyGive(TaskHandle_t task);
TaskHandle_t xTaskCreateStaticPinnedToCore(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,StackType_t *memory,StaticTask_t *tcb,int core);
