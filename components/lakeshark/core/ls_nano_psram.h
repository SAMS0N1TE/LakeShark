#pragma once

/* Internal RAM is the scarce pool on the Nano (modules are compiled in, so
 * their statics land there). LS_NANO_EXT_BSS puts a static task stack in PSRAM
 * on the Nano only. Use it only for stacks of tasks that never write flash or
 * NVS directly (flash and NVS work goes through ls_nvs_run), and keep the
 * StaticTask_t itself in internal RAM. Other boards keep the stack where it was. */

#include "esp_attr.h"
#include "sdkconfig.h"

#if CONFIG_LS_NANO_RAM_BUDGET
#define LS_NANO_EXT_BSS EXT_RAM_BSS_ATTR
#else
#define LS_NANO_EXT_BSS
#endif
