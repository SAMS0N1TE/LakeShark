/* Where each busy task last was, kept through a watchdog reset.

   A system watchdog reset (ESP_RST_WDT) writes no coredump and leaves
   nothing but the reset reason, so a freeze on battery, with no console,
   could not be placed. Each busy loop stamps its slot with a short tag as it
   goes; the slots live in RTC memory, which a watchdog reset does not clear.
   After a reset that was not asked for, boot prints the trail left by the
   run that died, and `trail` shows it again, next to the live one. */

#ifndef LS_TRAIL_H
#define LS_TRAIL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_TRAIL_TUI,       /* the screen being drawn, or the key being handled */
    LS_TRAIL_FIELD,     /* the field worker's step */
    LS_TRAIL_DFS,       /* the FIND source's step */
    LS_TRAIL_MIXRF,     /* the keyboard radios' loop */
    LS_TRAIL_LINK,      /* the Flipper link's last command */
    LS_TRAIL_SLOTS
} ls_trail_slot_t;

/* Cheap enough for a busy loop: copies at most 11 characters. */
void ls_trail(ls_trail_slot_t slot, const char *tag);

/* Once at boot: keeps the dead run's trail for printing, then starts over. */
void ls_trail_boot(void);
/* Prints the previous run's trail (if kept) and the live one. */
void ls_trail_print(void);

#ifdef __cplusplus
}
#endif
#endif
