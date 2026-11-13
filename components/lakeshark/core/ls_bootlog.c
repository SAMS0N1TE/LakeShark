/* The boot log's buffer and files; see ls_bootlog.h. */

#include "ls_bootlog.h"

#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_attr.h"

#define NOTE_MAX 96

/* Writers are serialised by `busy`, taken without waiting. A reader never
   holds a writer up: the head only grows, and the rolling tail is copied
   under a sequence count (odd while a writer is in it) and copied again if
   it moved. */
typedef struct {
    atomic_flag busy;                   /* a writer is in                 */
    atomic_flag filing;                 /* snap and the files are in use  */
    atomic_uint tail_seq;
    uint32_t head_len;
    uint32_t tail_total;                /* bytes ever sent to the tail    */
    uint32_t seq, skipped;
    bool rolled;                        /* this run renamed the old file  */
    char head[LS_BOOTLOG_HEAD];
    char tail[LS_BOOTLOG_TAIL];
    char snap[LS_BOOTLOG_TAIL];
} bootlog_t;

/* No initializer: PSRAM .bss, zeroed before app_main. */
EXT_RAM_BSS_ATTR static bootlog_t B;

void ls_bootlog_reset(void) { memset(&B, 0, sizeof(B)); }
uint32_t ls_bootlog_seq(void) { return B.seq; }
uint32_t ls_bootlog_skipped(void) { return B.skipped; }
void ls_bootlog_skip(void) { B.skipped++; }

bool ls_bootlog_add(const char *text, size_t n)
{
    if (!text || !n) return false;
    if (atomic_flag_test_and_set(&B.busy)) { B.skipped++; return false; }
    if (!B.tail_total && n <= LS_BOOTLOG_HEAD - B.head_len) {
        memcpy(B.head + B.head_len, text, n);
        B.head_len += (uint32_t)n;
    } else {
        if (n > LS_BOOTLOG_TAIL) { text += n - LS_BOOTLOG_TAIL; n = LS_BOOTLOG_TAIL; }
        atomic_fetch_add(&B.tail_seq, 1);
        const size_t at = B.tail_total % LS_BOOTLOG_TAIL;
        const size_t first = n < LS_BOOTLOG_TAIL - at ? n : LS_BOOTLOG_TAIL - at;
        memcpy(B.tail + at, text, first);
        memcpy(B.tail, text + first, n - first);
        B.tail_total += (uint32_t)n;
        atomic_fetch_add(&B.tail_seq, 1);
    }
    B.seq++;
    atomic_flag_clear(&B.busy);
    return true;
}

/* The tail in order, whole lines only, into dst (LS_BOOTLOG_TAIL of room).
   Returns its length; *dropped is what was logged between head and it. */
static size_t tail_copy(char *dst, uint32_t *dropped)
{
    uint32_t total = 0;
    for (int tries = 0; tries < 16; tries++) {
        const unsigned s0 = atomic_load(&B.tail_seq);
        total = B.tail_total;
        if (total > LS_BOOTLOG_TAIL) {
            const size_t start = total % LS_BOOTLOG_TAIL;
            memcpy(dst, B.tail + start, LS_BOOTLOG_TAIL - start);
            memcpy(dst + LS_BOOTLOG_TAIL - start, B.tail, start);
        } else {
            memcpy(dst, B.tail, total);
        }
        if (!(s0 & 1u) && atomic_load(&B.tail_seq) == s0) break;
    }
    *dropped = 0;
    if (total <= LS_BOOTLOG_TAIL) return total;
    size_t skip = 0;                    /* the rest of the line the roll cut */
    while (skip < LS_BOOTLOG_TAIL && dst[skip] != '\n') skip++;
    if (skip < LS_BOOTLOG_TAIL) skip++;
    memmove(dst, dst + skip, LS_BOOTLOG_TAIL - skip);
    *dropped = total - LS_BOOTLOG_TAIL + (uint32_t)skip;
    return LS_BOOTLOG_TAIL - skip;
}

static int dropped_note(char *out, uint32_t dropped)
{
    return dropped ? snprintf(out, NOTE_MAX, "... %lu bytes left out here: the newest lines follow ...\n",
                              (unsigned long)dropped) : 0;
}

static int skipped_note(char *out)
{
    return B.skipped ? snprintf(out, NOTE_MAX, "(%lu lines not kept: two tasks logged at once)\n",
                                (unsigned long)B.skipped) : 0;
}

size_t ls_bootlog_text(char *out, size_t cap)
{
    if (!out || !cap) return 0;
    size_t n = B.head_len;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, B.head, n);
    /* the tail is put in order in the caller's buffer: one too small for
       that gets the start of the boot only */
    if (cap - 1 - n >= LS_BOOTLOG_TAIL + 2 * NOTE_MAX) {
        uint32_t dropped = 0;
        char *tail = out + n + NOTE_MAX;
        const size_t t = tail_copy(tail, &dropped);
        n += (size_t)dropped_note(out + n, dropped);
        memmove(out + n, tail, t);
        n += t;
        n += (size_t)skipped_note(out + n);
    }
    out[n] = '\0';
    return n;
}

static void write_held(FILE *f)
{
    char note[NOTE_MAX];
    uint32_t dropped = 0;
    fwrite(B.head, 1, B.head_len, f);
    const size_t t = tail_copy(B.snap, &dropped);
    int k = dropped_note(note, dropped);
    if (k) fwrite(note, 1, (size_t)k, f);
    if (t) fwrite(B.snap, 1, t, f);
    k = skipped_note(note);
    if (k) fwrite(note, 1, (size_t)k, f);
}

bool ls_bootlog_write(FILE *f)
{
    if (!f || atomic_flag_test_and_set(&B.filing)) return false;
    write_held(f);
    atomic_flag_clear(&B.filing);
    return true;
}

static void path_of(const char *dir, bool prev, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", dir, prev ? LS_BOOTLOG_PREV : LS_BOOTLOG_FILE);
}

bool ls_bootlog_save(const char *dir)
{
    char cur[96], old[96];
    /* one save at a time, and never while the log is being printed: taken
       before the file is touched, so a busy moment cannot empty it */
    if (!dir || atomic_flag_test_and_set(&B.filing)) return false;
    path_of(dir, false, cur, sizeof(cur));
    path_of(dir, true, old, sizeof(old));
#ifdef _WIN32
    mkdir(dir);
#else
    mkdir(dir, 0775);
#endif
    bool ok = true;
    if (!B.rolled) {
        FILE *was = fopen(cur, "rb");
        if (was) {
            fclose(was);
            remove(old);
            ok = rename(cur, old) == 0;
        }
    }
    FILE *f = ok ? fopen(cur, "wb") : NULL;
    if (f) {
        B.rolled = true;                /* the card is there: never roll twice */
        write_held(f);
        if (ferror(f)) ok = false;
        if (fclose(f) != 0) ok = false;
    } else {
        ok = false;
    }
    atomic_flag_clear(&B.filing);
    return ok;
}

size_t ls_bootlog_load(const char *dir, bool prev, char *out, size_t cap)
{
    char path[96];
    if (!dir || !out || !cap) return 0;
    out[0] = '\0';
    /* until this run has saved, the file there is still the run before's */
    path_of(dir, prev && B.rolled, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
    return n;
}
