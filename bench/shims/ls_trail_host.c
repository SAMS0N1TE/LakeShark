/* Host builds keep no trail: there is no RTC memory and no watchdog. */
#include "ls_trail.h"
void ls_trail(ls_trail_slot_t slot, const char *tag) { (void)slot; (void)tag; }
void ls_trail_boot(void) {}
void ls_trail_print(void) {}
