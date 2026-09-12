/* Formatter for a stored coredump summary, and the boot-notice /
   erase glue that goes with it. No SDK calls here: the source struct is
   the seam so the bench can drive every branch without an ESP chip. */

#include "ls_crash.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const ls_crash_source_t *s_src;
static char                     s_running[64];
static bool                     s_present_cache;

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

ls_crash_read_result_t ls_crash_inspect_header(const uint8_t *header,
                                             size_t len, ls_crash_summary_t *out)
{
    if (!out) return LS_CRASH_READ_UNAVAILABLE;
    out->present = true;
    out->header_bounds_ok = false;
    if (!header || len < 4) return LS_CRASH_READ_UNAVAILABLE;
    out->declared_size = read_le32(header);
    /* IDF BLANK_COREDUMP_SIZE; all other values remain evidence, even zero. */
    if (out->declared_size == UINT32_MAX) {
        out->present = false;
        return LS_CRASH_READ_NOT_FOUND;
    }
    if (len < 20) return LS_CRASH_READ_UNAVAILABLE;
    out->core_dump_version = read_le32(header + 4);
    out->header_bounds_ok = out->declared_size >= 20 &&
                           out->declared_size <= out->partition_size;
    return LS_CRASH_READ_METADATA;
}

static ls_crash_read_result_t source_read(ls_crash_summary_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_src || !s_src->read) return LS_CRASH_READ_NOT_FOUND;
    return s_src->read(out);
}

void ls_crash_init(const ls_crash_source_t *src, const char *running_id)
{
    s_src = src;
    if (running_id) {
        strncpy(s_running, running_id, sizeof(s_running) - 1);
        s_running[sizeof(s_running) - 1] = '\0';
    } else {
        s_running[0] = '\0';
    }

    s_present_cache = s_src && s_src->present && s_src->present();
}

bool ls_crash_present(void) { return s_present_cache; }

bool ls_crash_erase(void)
{
    if (!s_src || !s_src->erase) {
        /* Nothing to erase (nothing to talk to) is still "success" from
           the caller's point of view - the postcondition ("no stored
           dump") holds. */
        s_present_cache = false;
        return true;
    }
    bool ok = s_src->erase();
    if (ok) s_present_cache = false;
    return ok;
}

/* Bounded appender. Returns bytes written (never past buflen-1) and always
   NUL-terminates when buflen > 0. Never negative: a truncated write is not
   an error at a caller who is printing a report. */
static size_t append(char *buf, size_t buflen, size_t used, const char *fmt, ...)
{
    if (buflen == 0) return 0;
    if (used >= buflen - 1) { buf[buflen - 1] = '\0'; return used; }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + used, buflen - used, fmt, ap);
    va_end(ap);
    if (n < 0) { buf[used] = '\0'; return used; }

    size_t added = (size_t)n;
    if (added >= buflen - used) added = buflen - used - 1;
    return used + added;
}

size_t ls_crash_format(char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return 0;
    buf[0] = '\0';

    if (!s_present_cache) {
        return append(buf, buflen, 0, "no crash recorded\n");
    }

    ls_crash_summary_t s;
    ls_crash_read_result_t result = source_read(&s);
    if (result == LS_CRASH_READ_METADATA) {
        return append(buf, buflen, 0,
            "crash: stored dump preserved; on-device ELF parsing disabled\n"
            "partition address=0x%08lx size=0x%08lx; raw declared size=%lu "
            "version=0x%08lx (%s; checksum NOT checked)\n"
            "Export FULL coredump partition using a host flash reader; decode "
            "offline with ESP-IDF esp-coredump and matching crash ELF. "
            "Back up before 'crash clear'.\n",
            (unsigned long)s.partition_address, (unsigned long)s.partition_size,
            (unsigned long)s.declared_size, (unsigned long)s.core_dump_version,
            s.header_bounds_ok ? "size in bounds only" : "invalid size");
    }
    if (result == LS_CRASH_READ_QUARANTINED) {
        return append(buf, buflen, 0,
                      "crash: stored dump quarantined after a parser fault; "
                      "summary not read and dump preserved (use 'crash clear' "
                      "to erase)\n");
    }
    if (result == LS_CRASH_READ_UNAVAILABLE) {
        return append(buf, buflen, 0,
                      "crash: stored dump summary unavailable; dump preserved. "
                      "Export full partition and decode offline with matching "
                      "crash ELF; back up before clearing.\n");
    }
    if (result == LS_CRASH_READ_NOT_FOUND || !s.present) {
        s_present_cache = false;
        return append(buf, buflen, 0, "no crash recorded\n");
    }

    size_t n = 0;

    n = append(buf, buflen, n,
               "crash: task \"%s\" pc=0x%08lx ra=0x%08lx\n",
               s.task[0] ? s.task : "", (unsigned long)s.pc, (unsigned long)s.ra);
    n = append(buf, buflen, n,
               "       fault_code=0x%08lx fault_addr=0x%08lx  (dump v%lu)\n",
               (unsigned long)s.fault_code, (unsigned long)s.fault_addr,
               (unsigned long)s.core_dump_version);

    if (s.elf_sha[0]) {
        n = append(buf, buflen, n, "       elf-sha=%s\n", s.elf_sha);
    }
    if (s.reason[0]) {
        n = append(buf, buflen, n, "       reason: %s\n", s.reason);
    }

    if (s.bt_depth == 0) {
        n = append(buf, buflen, n,
                   "       backtrace: %s\n",
                   s.bt_corrupted ? "(corrupted)" : "(none captured)");
    } else {
        n = append(buf, buflen, n, "       backtrace:");
        for (unsigned i = 0; i < s.bt_depth && i < LS_CRASH_MAX_BT; i++) {
            n = append(buf, buflen, n, " 0x%08lx", (unsigned long)s.bt[i]);
        }
        if (s.bt_corrupted) n = append(buf, buflen, n, " (corrupted)");
        n = append(buf, buflen, n, "\n");
    }

    if (s_running[0]) {
        n = append(buf, buflen, n,
                   "       running now: %s  (elf-sha above identifies the "
                   "image that crashed)\n", s_running);
    }
    return n;
}

size_t ls_crash_boot_notice(char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return 0;
    buf[0] = '\0';
    if (!s_present_cache) return 0;
    return append(buf, buflen, 0,
                  "crash: a dump from a previous boot is waiting - "
                  "type 'crash' for safe metadata; back up before 'crash clear'");
}
