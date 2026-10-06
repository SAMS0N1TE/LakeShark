#ifndef LS_ERRLOG_H
#define LS_ERRLOG_H

/* Errors that outlive the run they happened in.

   A hardware watchdog reset writes no coredump, and the panic crumb that
   names the frozen core lives only until the next power-off: in the field a
   crash was "it rebooted" until a laptop came out. So every abnormal reset
   (panic, watchdog, brownout) is written to flash at the next boot as a
   record: when, which firmware, why, both cores' crumbs, the coredump's one
   line if one was written, and the last error and warning lines the run
   printed before it went ("last words", kept in memory a reset does not
   clear). DIAG lists them; `crumb log` prints them.

   No SDK calls here: the store struct is the seam so the bench drives every
   branch without a chip (ls_errlog_esp.c wires NVS, RTC memory and the log
   hook). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_ERRLOG_RECORDS     4     /* kept in flash, newest first          */
#define LS_ERRLOG_TAIL_BYTES  1024  /* last words kept in RTC memory        */
#define LS_ERRLOG_REC_TAIL    480   /* of them, saved with a record         */
#define LS_ERRLOG_CRUMB_MAX   256
#define LS_ERRLOG_TRAIL_MAX   128
#define LS_ERRLOG_DUMP_MAX    96
#define LS_ERRLOG_FW_MAX      32
#define LS_ERRLOG_REASON_MAX  32

typedef struct {
    uint32_t seq;                          /* 1 for the first record ever    */
    int64_t  wall;                         /* unix seconds at the save; 0 = no clock */
    char     fw[LS_ERRLOG_FW_MAX];         /* firmware that saved it         */
    char     reason[LS_ERRLOG_REASON_MAX]; /* why the run before ended       */
    char     crumb[LS_ERRLOG_CRUMB_MAX];   /* both cores' crumbs, one line each */
    char     trail[LS_ERRLOG_TRAIL_MAX];   /* where each busy loop last was  */
    char     dump[LS_ERRLOG_DUMP_MAX];     /* coredump one-liner, or empty   */
    char     tail[LS_ERRLOG_REC_TAIL];     /* last words, oldest line first  */
} ls_errlog_rec_t;

/* The last error and warning lines, a byte ring that survives a reset. */
typedef struct {
    uint32_t magic;
    uint32_t head;                         /* next byte to write             */
    uint32_t used;                         /* bytes held, up to the size     */
    char     buf[LS_ERRLOG_TAIL_BYTES];
} ls_errlog_tail_t;

void   ls_errlog_tail_reset(ls_errlog_tail_t *t);
/* True when t holds a ring this firmware wrote (not power-on noise). */
bool   ls_errlog_tail_valid(const ls_errlog_tail_t *t);
/* One line; a missing newline is added. */
void   ls_errlog_tail_add(ls_errlog_tail_t *t, const char *line);
/* The newest whole lines that fit in out (n bytes with the NUL), oldest
   first. Returns the length written. */
size_t ls_errlog_tail_copy(const ls_errlog_tail_t *t, char *out, size_t n);

/* Where records live: slot 0..LS_ERRLOG_RECORDS-1 and a counter. */
typedef struct {
    bool (*load)(int slot, ls_errlog_rec_t *out);
    bool (*save)(int slot, const ls_errlog_rec_t *rec);
    bool (*erase)(int slot);
    uint32_t (*get_seq)(void);
    bool (*set_seq)(uint32_t seq);
} ls_errlog_store_t;

void ls_errlog_init(const ls_errlog_store_t *store);

/* Write rec as the next record (its seq is assigned). */
bool ls_errlog_save(ls_errlog_rec_t *rec);

/* Records held, and the i-th newest (0 = latest). */
int  ls_errlog_count(void);
bool ls_errlog_get(int i, ls_errlog_rec_t *out);
/* Records saved since the last clear, including those since overwritten.
   Read from the store once, then kept, so a screen may ask every frame. */
uint32_t ls_errlog_total(void);
bool ls_errlog_clear(void);
/* The newest record, read once and kept; NULL when there is none. */
const ls_errlog_rec_t *ls_errlog_newest(void);

/* "2026-10-05 07:12:33 UTC" or "no clock" */
void   ls_errlog_when(const ls_errlog_rec_t *r, char *out, size_t n);
/* The whole record as console text. */
size_t ls_errlog_format(const ls_errlog_rec_t *r, char *buf, size_t n);

/* Device glue (ls_errlog_esp.c). */
/* Before anything logs: start this run's last words, keeping the run
   before's for the save below. */
void ls_errlog_early(void);
/* After NVS, the wall clock and ls_crash_boot_setup(): record the run
   before if it ended badly (an abnormal reset, or panicked: a core entered
   the panic handler). crumb is the panic crumb's text, may be empty. */
void ls_errlog_boot(bool panicked, const char *crumb);
/* This run's last words so far (DIAG shows them live). */
size_t ls_errlog_live(char *out, size_t n);
/* `crumb log` and `crumb clear` print through this. */
void ls_errlog_print_all(void);

#ifdef __cplusplus
}
#endif

#endif
