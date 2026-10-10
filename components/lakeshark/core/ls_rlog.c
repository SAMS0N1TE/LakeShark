/* See ls_rlog.h. Plain stdio, because the card is a VFS mount on the
   board and a directory on the bench, and this file wants to be the same
   code in both. */
#include "ls_rlog.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

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

static bool finish(FILE *f)
{
    bool ok = fflush(f) == 0;
#if defined(LS_RLOG_TEST_SYNC)
    extern int ls_rlog_test_sync(int fd);
    if (ok) ok = ls_rlog_test_sync(fileno(f)) == 0;
#elif defined(_WIN32)
    if (ok) ok = _commit(_fileno(f)) == 0;
#else
    if (ok) ok = fsync(fileno(f)) == 0;
#endif
    if (fclose(f) != 0) ok = false;
    return ok;
}

/* Undo holds the old header and overwritten slot until append is durable. */
static bool undo(ls_rlog_t *lg, bool save)
{
    char path[80];
    snprintf(path, sizeof(path), "%s.undo", lg->path);
    char temp[80];
    snprintf(temp, sizeof(temp), "%s.undo.tmp", lg->path);
    FILE *j = fopen(save ? temp : path, save ? "wb" : "rb");
    if (!j) return !save && errno == ENOENT;
    FILE *f = fopen(lg->path, "r+b");
    if (!f) { fclose(j); return false; }
    ls_rlog_header_t h;
    bool ok = save ? fread(&h, sizeof(h), 1, f) == 1 :
                     fread(&h, sizeof(h), 1, j) == 1;
    ok = ok && h.magic == LS_RLOG_MAGIC && h.version == LS_RLOG_VERSION &&
         h.rec_size == lg->rec_size && h.capacity == lg->capacity &&
         h.head < h.capacity && h.count <= h.capacity;
    if (ok && !save) {
        ok = fseek(j, 0, SEEK_END) == 0 && ftell(j) == (long)sizeof(h) + h.rec_size &&
             fseek(j, sizeof(h), SEEK_SET) == 0;
    }
    if (ok && save) ok = fwrite(&h, sizeof(h), 1, j) == 1;
    if (ok) ok = fseek(f, (long)sizeof(h) + (long)h.head * h.rec_size, SEEK_SET) == 0;
    unsigned char buf[64];
    for (size_t left = ok ? h.rec_size : 0; ok && left;) {
        size_t n = left < sizeof(buf) ? left : sizeof(buf);
        ok = fread(buf, 1, n, save ? f : j) == n &&
             fwrite(buf, 1, n, save ? j : f) == n;
        left -= n;
    }
    if (ok && !save)
        ok = fseek(f, 0, SEEK_SET) == 0 && fwrite(&h, sizeof(h), 1, f) == 1;
    if (save) { if (!finish(j)) ok = false; fclose(f); }
    else { fclose(j); if (!finish(f)) ok = false; }
    if (ok && save) ok = rename(temp, path) == 0;
    if (ok && !save) {
        lg->head = h.head;
        lg->count = h.count;
        ok = remove(path) == 0;
    }
    if (!ok && save) remove(temp);
    return ok;
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
    if (!finish(f)) ok = false;
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

    if (!undo(lg, false)) return false;
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

    if (!undo(lg, false) || !undo(lg, true)) return false;
    FILE *f = fopen(lg->path, "r+b");
    if (!f) return false;
    ls_rlog_t next = *lg;
    const long off = (long)sizeof(ls_rlog_header_t) +
                     (long)lg->head * (long)lg->rec_size;
    bool ok = fseek(f, off, SEEK_SET) == 0 &&
              fwrite(rec, lg->rec_size, 1, f) == 1 && fflush(f) == 0;
    if (ok) {
        next.head = (next.head + 1) % next.capacity;
        if (next.count < next.capacity) next.count++;
        ok = write_header(&next, f);
    }
    if (!finish(f)) ok = false;
    if (ok) {
        char path[80];
        snprintf(path, sizeof(path), "%s.undo", lg->path);
        ok = remove(path) == 0;
    }
    if (ok) *lg = next;
    else (void)undo(lg, false);

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
    if (!undo(lg, false) || !undo(lg, true)) return false;
    FILE *f = fopen(lg->path, "r+b");
    if (!f) return false;
    ls_rlog_t next = *lg;
    next.count = 0;
    next.head = 0;
    bool ok = write_header(&next, f);
    if (!finish(f)) ok = false;
    if (ok) {
        char path[80];
        snprintf(path, sizeof(path), "%s.undo", lg->path);
        ok = remove(path) == 0;
    }
    if (ok) *lg = next;
    else (void)undo(lg, false);
    return ok;
}

void ls_rlog_close(ls_rlog_t *lg)
{
    if (lg) lg->open = false;
}
