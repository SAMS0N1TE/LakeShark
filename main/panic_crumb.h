#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Moves the previous boot's panic record out of RTC memory; call early. */
void panic_crumb_boot(void);
void panic_crumb_register_command(void);
/* The run before's crumbs as text for the error log: a line per core that
   entered the panic handler, then the ticks. True when a core entered it. */
bool panic_crumb_text(char *out, size_t n);

#ifdef __cplusplus
}
#endif
