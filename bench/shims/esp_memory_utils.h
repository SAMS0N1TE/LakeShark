#ifndef LS_SHIM_ESP_MEMORY_UTILS_H
#define LS_SHIM_ESP_MEMORY_UTILS_H

#include <stdbool.h>

extern bool ls_shim_ptr_in_dram_result;
static inline bool esp_ptr_in_dram(const void *p)
{
    return p != NULL && ls_shim_ptr_in_dram_result;
}

#endif
