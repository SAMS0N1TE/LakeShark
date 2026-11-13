#ifndef LS_BOOTLOG_H
#define LS_BOOTLOG_H

/* The boot log: what this run logged from the first line of app_main.

   "Black screen, nothing on boot" used to need a laptop on the serial port.
   Every log line is also copied into PSRAM: the first 40 KB whole (the boot
   itself), then the newest 8 KB. It is written to the SD card as
   lakeshark/bootlog.txt once the card is up and again when the boot has
   settled, after the file of the run before is renamed bootlog-prev.txt, so
   a board that came up black still leaves its log for the next boot (or a
   card reader) to show. BOOT LOG shows both; `bootlog` prints this run's.

   No SDK calls here (ls_bootlog_esp.c hooks the log and names the reset);
   plain stdio does the files, so the bench drives all of it. Nothing below
   takes internal RAM: every static is in PSRAM. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_BOOTLOG_HEAD      40960   /* kept whole from the start of the boot */
#define LS_BOOTLOG_TAIL      8192    /* then the newest lines, rolling        */
#define LS_BOOTLOG_TEXT_MAX  (LS_BOOTLOG_HEAD + LS_BOOTLOG_TAIL + 256)
#define LS_BOOTLOG_DIR       "/sdcard/lakeshark"
#define LS_BOOTLOG_FILE      "bootlog.txt"
#define LS_BOOTLOG_PREV      "bootlog-prev.txt"

/* One whole line (or more), n bytes. Never waits: a line arriving while
   another is being copied is counted and left out. False when left out. */
bool ls_bootlog_add(const char *text, size_t n);
/* Goes up with every line kept: a screen re-reads when it changes. */
uint32_t ls_bootlog_seq(void);
uint32_t ls_bootlog_skipped(void);
void ls_bootlog_skip(void);            /* the hook left a line out */

/* The log as text, oldest first, with a line where the middle was left out.
   Returns the length written; out is always terminated. */
size_t ls_bootlog_text(char *out, size_t cap);
bool   ls_bootlog_write(FILE *f);

/* Writes dir/bootlog.txt. The first save of a run first renames the file
   already there to bootlog-prev.txt. False without a card. */
bool   ls_bootlog_save(const char *dir);
/* Reads this run's saved file, or (prev) the run before's. 0 when there
   is none. */
size_t ls_bootlog_load(const char *dir, bool prev, char *out, size_t cap);

void ls_bootlog_reset(void);            /* the bench, between cases */

/* On the chip. Start: first thing in app_main; writes the identity and
   reset lines and hooks the log. Tick: from a slow loop; 20 s into the run
   it saves to the card once, from a task whose stack is in PSRAM. */
void ls_bootlog_start(void);
void ls_bootlog_tick(void);
void ls_bootlog_register_command(void);

#ifdef __cplusplus
}
#endif

#endif
