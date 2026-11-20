/* Force-included into every module source (build_modules.py).
   A module's statics all live in PSRAM with the rest of its image, so the
   placement attributes mean nothing there; left alone, .ext_ram.bss would be
   written into the file as zeros. Module code must not run with the flash
   cache off (no interrupt handlers): it is in PSRAM. */
#ifndef LS_MOD_PRELUDE_H
#define LS_MOD_PRELUDE_H
#ifndef __ASSEMBLER__
#include "esp_attr.h"
#undef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#undef EXT_RAM_NOINIT_ATTR
#define EXT_RAM_NOINIT_ATTR
#undef DRAM_ATTR
#define DRAM_ATTR
#undef IRAM_ATTR
#define IRAM_ATTR
/* Spinlocks (portMUX_TYPE) would be PSRAM statics: every critical section of
   module code takes one firmware spinlock in internal RAM instead. */
#include "freertos/FreeRTOS.h"
#undef HZ /* sys/param.h leak: sources use HZ as an identifier */
void ls_mod_crit_enter(void);
void ls_mod_crit_exit(void);
#undef portENTER_CRITICAL
#undef portEXIT_CRITICAL
#undef portENTER_CRITICAL_SAFE
#undef portEXIT_CRITICAL_SAFE
#undef portENTER_CRITICAL_ISR
#undef portEXIT_CRITICAL_ISR
#define portENTER_CRITICAL(m) ((void)(m), ls_mod_crit_enter())
#define portEXIT_CRITICAL(m) ((void)(m), ls_mod_crit_exit())
#define portENTER_CRITICAL_SAFE(m) portENTER_CRITICAL(m)
#define portEXIT_CRITICAL_SAFE(m) portEXIT_CRITICAL(m)
#define portENTER_CRITICAL_ISR(m) portENTER_CRITICAL(m)
#define portEXIT_CRITICAL_ISR(m) portEXIT_CRITICAL(m)
#endif
#endif
