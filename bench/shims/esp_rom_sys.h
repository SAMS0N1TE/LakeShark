#ifndef LS_SHIM_ESP_ROM_SYS_H
#define LS_SHIM_ESP_ROM_SYS_H

#include <stdint.h>

void esp_rom_delay_us(uint32_t us);

/* The bench has no ROM, and a trace marker is only ever diagnostic, so it
   goes to stdout. Declared here rather than left to the caller because
   firmware code that traces must still link on the host. */
#include <stdio.h>
#define esp_rom_printf(...) ((void)printf(__VA_ARGS__))

#endif
