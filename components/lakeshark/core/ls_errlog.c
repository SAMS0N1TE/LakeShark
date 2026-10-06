/* The error log's logic: the last-words ring and the record slots. No SDK
   calls; ls_errlog_esp.c supplies the store, the RTC ring and the hook. */

/* gmtime_r on the bench's MinGW as on newlib */
#define _POSIX_THREAD_SAFE_FUNCTIONS 1

#include "ls_errlog.h"

#include "esp_attr.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TAIL_MAGIC 0x4C534557u   /* "LSEW" */

/* ------------------------------------------------------------ last words */

void ls_errlog_tail_reset(ls_errlog_tail_t *t)
{
    if (!t) return;
    t->head = 0;
    t->used = 0;
    t->magic = TAIL_MAGIC;
}

bool ls_errlog_tail_valid(const ls_errlog_tail_t *t)
{
    return t && t->magic == TAIL_MAGIC && t->head < LS_ERRLOG_TAIL_BYTES &&
           t->used <= LS_ERRLOG_TAIL_BYTES;
}

static void put(ls_errlog_tail_t *t, char c)
{
    t->buf[t->head] = c;
    t->head = (t->head + 1) % LS_ERRLOG_TAIL_BYTES;
    if (t->used < LS_ERRLOG_TAIL_BYTES) t->used++;
}

void ls_errlog_tail_add(ls_errlog_tail_t *t, const char *line)
{
    if (!t || !line || !ls_errlog_tail_valid(t)) return;
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    if (!len) return;
    /* a line longer than the ring keeps its end */
    if (len > LS_ERRLOG_TAIL_BYTES - 1) {
        line += len - (LS_ERRLOG_TAIL_BYTES - 1);
        len = LS_ERRLOG_TAIL_BYTES - 1;
    }
    for (size_t i = 0; i < len; i++) put(t, line[i] == '\n' ? ' ' : line[i]);
    put(t, '\n');
}

/* the byte `i` from the oldest one held */
static char at(const ls_errlog_tail_t *t, uint32_t i)
{
    return t->buf[(t->head + LS_ERRLOG_TAIL_BYTES - t->used + i) % LS_ERRLOG_TAIL_BYTES];
}

size_t ls_errlog_tail_copy(const ls_errlog_tail_t *t, char *out, size_t n)
{
    if (!out || n == 0) return 0;
    out[0] = '\0';
    if (!ls_errlog_tail_valid(t) || t->used == 0) return 0;
    uint32_t start = 0;
    /* a ring that wrapped begins mid-line */
    if (t->used == LS_ERRLOG_TAIL_BYTES) {
        while (start < t->used && at(t, start) != '\n') start++;
        start++;
    }
    /* the newest whole lines that fit */
    if (t->used > start && t->used - start > n - 1) {
        start = t->used - (uint32_t)(n - 1);
        while (start < t->used && at(t, start) != '\n') start++;
        start++;
    }
    size_t k = 0;
    for (uint32_t i = start; i < t->used && k + 1 < n; i++) out[k++] = at(t, i);
    out[k] = '\0';
    return k;
}

/* ---------------------------------------------------------------- records */

static const ls_errlog_store_t *s_store;
static uint32_t s_total;
static bool     s_total_known;
EXT_RAM_BSS_ATTR static ls_errlog_rec_t s_newest;
static int      s_newest_state;          /* 0 not read yet, 1 held, -1 none */

void ls_errlog_init(const ls_errlog_store_t *store)
{
    s_store = store;
    s_total_known = false;
    s_newest_state = 0;
}

uint32_t ls_errlog_total(void)
{
    if (!s_total_known && s_store && s_store->get_seq) {
        s_total = s_store->get_seq();
        s_total_known = true;
    }
    return s_total_known ? s_total : 0;
}

static int slot_of(uint32_t seq) { return (int)((seq - 1) % LS_ERRLOG_RECORDS); }

bool ls_errlog_save(ls_errlog_rec_t *rec)
{
    if (!s_store || !s_store->save || !s_store->set_seq || !rec) return false;
    const uint32_t seq = ls_errlog_total() + 1;
    rec->seq = seq;
    rec->fw[sizeof(rec->fw) - 1] = rec->reason[sizeof(rec->reason) - 1] = '\0';
    rec->crumb[sizeof(rec->crumb) - 1] = rec->dump[sizeof(rec->dump) - 1] = '\0';
    rec->trail[sizeof(rec->trail) - 1] = '\0';
    rec->tail[sizeof(rec->tail) - 1] = '\0';
    s_total_known = false;                 /* whatever happens next */
    if (!s_store->save(slot_of(seq), rec) || !s_store->set_seq(seq)) return false;
    s_total = seq;
    s_total_known = true;
    s_newest = *rec;
    s_newest_state = 1;
    return true;
}

int ls_errlog_count(void)
{
    const uint32_t total = ls_errlog_total();
    return (int)(total < LS_ERRLOG_RECORDS ? total : LS_ERRLOG_RECORDS);
}

bool ls_errlog_get(int i, ls_errlog_rec_t *out)
{
    if (!out || !s_store || !s_store->load || i < 0 || i >= ls_errlog_count()) return false;
    const uint32_t seq = ls_errlog_total() - (uint32_t)i;
    if (!s_store->load(slot_of(seq), out)) return false;
    /* a slot left from before a clear, or a write that never finished */
    return out->seq == seq;
}

bool ls_errlog_clear(void)
{
    if (!s_store || !s_store->set_seq) return false;
    bool ok = true;
    if (s_store->erase)
        for (int s = 0; s < LS_ERRLOG_RECORDS; s++) ok = s_store->erase(s) && ok;
    s_newest_state = -1;
    s_total_known = false;
    if (!s_store->set_seq(0)) return false;
    s_total = 0;
    s_total_known = true;
    return ok;
}

const ls_errlog_rec_t *ls_errlog_newest(void)
{
    if (s_newest_state == 0) s_newest_state = ls_errlog_get(0, &s_newest) ? 1 : -1;
    return s_newest_state == 1 ? &s_newest : NULL;
}

/* ------------------------------------------------------------------- text */

void ls_errlog_when(const ls_errlog_rec_t *r, char *out, size_t n)
{
    if (!out || n == 0) return;
    if (!r || r->wall <= 0) {
        snprintf(out, n, "no clock");
        return;
    }
    /* the board keeps UTC and has no time zone, so it says so */
    const time_t t = (time_t)r->wall;
    struct tm tm;
    if (!gmtime_r(&t, &tm) || !strftime(out, n, "%Y-%m-%d %H:%M:%S UTC", &tm))
        snprintf(out, n, "%lld", (long long)r->wall);
}

static size_t append(char *buf, size_t n, size_t used, const char *fmt, ...)
{
    if (used >= n) return used;
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(buf + used, n - used, fmt, ap);
    va_end(ap);
    if (w < 0) return used;
    return (size_t)w >= n - used ? n - 1 : used + (size_t)w;
}

size_t ls_errlog_format(const ls_errlog_rec_t *r, char *buf, size_t n)
{
    if (!buf || n == 0) return 0;
    buf[0] = '\0';
    if (!r) return 0;
    char when[32];
    ls_errlog_when(r, when, sizeof(when));
    size_t k = append(buf, n, 0, "#%lu  %s  %s  (fw %s)\n", (unsigned long)r->seq, when,
                      r->reason[0] ? r->reason : "?", r->fw[0] ? r->fw : "?");
    if (r->crumb[0]) k = append(buf, n, k, "%s\n", r->crumb);
    if (r->trail[0]) k = append(buf, n, k, "trail: %s\n", r->trail);
    if (r->dump[0])  k = append(buf, n, k, "%s\n", r->dump);
    if (r->tail[0])  k = append(buf, n, k, "last words:\n%s", r->tail);
    else             k = append(buf, n, k, "last words: none kept\n");
    return k;
}
