/* See ls_rlog.h. Plain stdio, because the card is a VFS mount on the
   board and a directory on the bench, and this file wants to be the same
   code in both. */
#include "ls_rlog.h"

#include <stdio.h>
#include <string.h>

/* The whole reason this is its own function.

   The ring holds `count` records and the next append lands at `head`, so the
   OLDEST is head - count, wrapped. Writing that as a subtraction on unsigned
   indices is how it goes wrong: head=0 count=24 underflows to a very large
   number and the modulo hides it by producing a plausible slot. The capacity
   is added first so the intermediate never goes below zero. */
uint32_t ls_rlog_slot_of(uint32_t head, uint32_t count, uint32_t capacity,
                         uint32_t i)
{
    if (!capacity) return 0;
    if (count > capacity) count = capacity;
    return (head + capacity - count + i) % capacity;
}

static bool write_header(ls_rlog_t *lg, FILE *f)
{
    ls_rlog_header_t h = {
        .magic = LS_RLOG_MAGIC, .version = LS_RLOG_VERSION,
        .rec_size = lg->rec_size, .capacity = lg->capacity,
        .count = lg->count, .head = lg->head,
    };
    if (fseek(f, 0, SEEK_SET) != 0) return false;
    return fwrite(&h, sizeof(h), 1, f) == 1;
}

/* Create the file at full size so a later append cannot fail for want of
   space on a card that filled up in between. A log that reserves what it
   needs on the day it is made is one that keeps working on the day the card
   is nearly full, which is the day it matters. */
static bool create(ls_rlog_t *lg)
{
    FILE *f = fopen(lg->path, "wb");
    if (!f) return false;

    lg->count = 0;
    lg->head = 0;
    bool ok = write_header(lg, f);

    if (ok) {
        static const unsigned char zero[64] = { 0 };
        size_t left = (size_t)lg->rec_size * lg->capacity;
        while (ok && left) {
            const size_t n = left < sizeof(zero) ? left : sizeof(zero);
            ok = fwrite(zero, 1, n, f) == n;
            left -= n;
        }
    }
    fclose(f);
    return ok;
}

bool ls_rlog_open(ls_rlog_t *lg, const char *path,
                  uint32_t rec_size, uint32_t capacity)
{
    if (!lg || !path || !*path || !rec_size || !capacity) return false;

    memset(lg, 0, sizeof(*lg));
    snprintf(lg->path, sizeof(lg->path), "%s", path);
    lg->rec_size = rec_size;
    lg->capacity = capacity;

    FILE *f = fopen(lg->path, "r+b");
    if (f) {
        ls_rlog_header_t h;
        const bool read_ok = fread(&h, sizeof(h), 1, f) == 1;
        fclose(f);

        /* Geometry has to agree or the file is replaced. Reading one
           struct's bytes as another's produces records that look real. */
        if (read_ok && h.magic == LS_RLOG_MAGIC &&
            h.version == LS_RLOG_VERSION &&
            h.rec_size == rec_size && h.capacity == capacity &&
            h.count <= capacity && h.head < capacity) {
            lg->count = h.count;
            lg->head = h.head;
            lg->open = true;
            return true;
        }
    }

    if (!create(lg)) return false;
    lg->open = true;
    return true;
}

bool ls_rlog_append(ls_rlog_t *lg, const void *rec)
{
    if (!lg || !lg->open || !rec) return false;

    FILE *f = fopen(lg->path, "r+b");
    if (!f) return false;

    const long off = (long)sizeof(ls_rlog_header_t) +
                     (long)lg->head * (long)lg->rec_size;
    bool ok = fseek(f, off, SEEK_SET) == 0 &&
              fwrite(rec, lg->rec_size, 1, f) == 1;

    if (ok) {
        lg->head = (lg->head + 1) % lg->capacity;
        if (lg->count < lg->capacity) lg->count++;
        /* The record lands before the header that points at it. A power cut
           between the two loses the newest entry and leaves the rest
           readable, which is the right way round to fail. */
        ok = write_header(lg, f);
    }
    fclose(f);
    return ok;
}

int ls_rlog_read(ls_rlog_t *lg, void *out, int max)
{
    if (!lg || !lg->open || !out || max <= 0) return 0;

    FILE *f = fopen(lg->path, "rb");
    if (!f) return 0;

    int want = (int)lg->count;
    if (want > max) want = max;

    /* The newest `max` when there are more than asked for: a chat wants the
       end of the conversation, not its beginning. */
    const uint32_t skip = lg->count - (uint32_t)want;

    unsigned char *dst = (unsigned char *)out;
    int got = 0;
    for (int i = 0; i < want; i++) {
        const uint32_t slot = ls_rlog_slot_of(lg->head, lg->count,
                                              lg->capacity, skip + (uint32_t)i);
        const long off = (long)sizeof(ls_rlog_header_t) +
                         (long)slot * (long)lg->rec_size;
        if (fseek(f, off, SEEK_SET) != 0) break;
        if (fread(dst + (size_t)i * lg->rec_size, lg->rec_size, 1, f) != 1) break;
        got++;
    }
    fclose(f);
    return got;
}

int ls_rlog_read_at(ls_rlog_t *lg, int i, void *out)
{
    if (!lg || !lg->open || !out) return 0;
    if (i < 0 || (uint32_t)i >= lg->count) return 0;

    FILE *f = fopen(lg->path, "rb");
    if (!f) return 0;

    const uint32_t slot = ls_rlog_slot_of(lg->head, lg->count, lg->capacity,
                                          (uint32_t)i);
    const long off = (long)sizeof(ls_rlog_header_t) +
                     (long)slot * (long)lg->rec_size;
    int got = 0;
    if (fseek(f, off, SEEK_SET) == 0 &&
        fread(out, lg->rec_size, 1, f) == 1)
        got = 1;
    fclose(f);
    return got;
}

bool ls_rlog_replace(ls_rlog_t *lg, const void *recs, int n)
{
    if (!lg || !lg->open || n < 0) return false;
    if (!recs && n) return false;
    if ((uint32_t)n > lg->capacity) return false;

    FILE *f = fopen(lg->path, "r+b");
    if (!f) return false;

    bool ok = fseek(f, (long)sizeof(ls_rlog_header_t), SEEK_SET) == 0;
    if (ok && n)
        ok = fwrite(recs, lg->rec_size, (size_t)n, f) == (size_t)n;

    if (ok) {
        lg->count = (uint32_t)n;
        lg->head  = (lg->capacity && (uint32_t)n == lg->capacity)
                  ? 0 : (uint32_t)n;
        ok = write_header(lg, f);
    }
    fclose(f);
    return ok;
}

int ls_rlog_count(const ls_rlog_t *lg)
{
    return (lg && lg->open) ? (int)lg->count : 0;
}

bool ls_rlog_clear(ls_rlog_t *lg)
{
    if (!lg || !lg->open) return false;
    FILE *f = fopen(lg->path, "r+b");
    if (!f) return false;
    lg->count = 0;
    lg->head = 0;
    const bool ok = write_header(lg, f);
    fclose(f);
    return ok;
}

void ls_rlog_close(ls_rlog_t *lg)
{
    if (lg) lg->open = false;
}
