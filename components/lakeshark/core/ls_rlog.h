/* A bounded log of fixed-size records, on the card. */

#ifndef LS_RLOG_H
#define LS_RLOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* On-card header. Little endian, which is the only endianness this project
   has ever run on; a card moved to a big-endian host reads the magic wrong
   and the log is treated as absent, which is the safe direction. */
#define LS_RLOG_MAGIC   0x474F4C53u   /* "SLOG" */
#define LS_RLOG_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t rec_size;     /* bytes per record, as the owner declared      */
    uint32_t capacity;     /* records the ring holds                        */
    uint32_t count;        /* records currently held, <= capacity           */
    uint32_t head;         /* index the NEXT append lands on                */
} ls_rlog_header_t;

typedef struct {
    char     path[64];
    uint32_t rec_size;
    uint32_t capacity;
    uint32_t count;
    uint32_t head;
    bool     open;         /* false when there is no card or no file        */
} ls_rlog_t;

/* Open or create. Returns false when there is nowhere to write, which
   is not an error worth shouting about on a board with no card in it - every
   other call then becomes a no-op and the owner keeps working from RAM.

   An existing file whose geometry disagrees with `rec_size` or `capacity` is
   REPLACED, not reinterpreted. The alternative is reading one struct's bytes
   as another's, which produces confident nonsense; a log that starts empty
   after a firmware change is the honest outcome. */
bool ls_rlog_open(ls_rlog_t *lg, const char *path,
                  uint32_t rec_size, uint32_t capacity);

/* Append one record. False when the log is not open or the write failed. */
bool ls_rlog_append(ls_rlog_t *lg, const void *rec);

/* Oldest first, which is how a chat and a track both read. Returns how many
   were written into `out`, at most `max` and at most what the ring holds. */
int ls_rlog_read(ls_rlog_t *lg, void *out, int max);

/* One record, by position, oldest first. */

int ls_rlog_read_at(ls_rlog_t *lg, int i, void *out);

/* How many records the log currently holds. */
int ls_rlog_count(const ls_rlog_t *lg);

/* Replace the whole contents in one go. */

bool ls_rlog_replace(ls_rlog_t *lg, const void *recs, int n);

/* Empty it, keeping the file and its geometry. */
bool ls_rlog_clear(ls_rlog_t *lg);

void ls_rlog_close(ls_rlog_t *lg);

/* The index arithmetic, exposed because it is where ring logs go
   wrong and it is pure.

   Given a ring that holds `count` of `capacity` records with the next append
   landing at `head`, which slot is the `i`th oldest? Off by one here does not
   fail: it silently returns the newest record as the oldest, or repeats one,
   and a chat that reads slightly out of order looks like a radio problem. */
uint32_t ls_rlog_slot_of(uint32_t head, uint32_t count, uint32_t capacity,
                         uint32_t i);

#ifdef __cplusplus
}
#endif

#endif /* LS_RLOG_H */
