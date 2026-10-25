#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t StackType_t;
typedef struct { int n; } StaticTask_t;
typedef void *TaskHandle_t;
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(n) (n)
