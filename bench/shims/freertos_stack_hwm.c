/* The host has no task stacks to measure: plenty left. Its own file, defined
   strongly: a suite that models a task's stack defines its own, and then this
   object is never pulled out of the harness library. (A weak definition inside
   freertos_shim.o does not link on mingw unless that object is already in the
   link, which is why lssim needed a --defsym.) */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{ (void)task; return 8192; }
