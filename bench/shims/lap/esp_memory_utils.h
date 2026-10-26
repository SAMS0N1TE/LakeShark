#pragma once
#include <stdbool.h>
bool esp_ptr_external_ram(const void *p);
bool esp_ptr_in_dram(const void *p);
