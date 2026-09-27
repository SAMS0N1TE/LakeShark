#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Moves the previous boot's panic record out of RTC memory; call early. */
void panic_crumb_boot(void);
void panic_crumb_register_command(void);

#ifdef __cplusplus
}
#endif
