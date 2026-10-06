/* Host builds keep no trail: there is no RTC memory and no watchdog. */
#include "ls_trail.h"
void ls_trail(ls_trail_slot_t slot, const char *tag) { (void)slot; (void)tag; }
void ls_trail_boot(void) {}
void ls_trail_print(void) {}
size_t ls_trail_text(char *out, size_t n) { if (out && n) out[0] = '\0'; return 0; }
