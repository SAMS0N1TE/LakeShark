/* Formatter for a stored coredump summary, and the boot-notice /
   erase glue that goes with it. No SDK calls here: the source struct is
   the seam so the bench can drive every branch without an ESP chip. */

#include "ls_crash.h"

#include <stdarg.h>
#include <stdlib.h>
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

/* ------------------------------------------------------------ ELF dump */

#define CD_HDR           24u      /* core_dump_header_t ahead of the ELF   */
#define CD_EHDR          52u      /* Elf32_Ehdr                             */
#define CD_PHDR          32u      /* Elf32_Phdr                             */
#define CD_VER_ELF_CRC32 0x0102u  /* COREDUMP_VERSION_ELF_CRC32, low half  */
#define CD_MAX_PHDRS     512u
#define CD_MAX_LOADS     160u
#define CD_MAX_NOTES     1024u
#define CD_NOTE_PRSTATUS 1u       /* "CORE": the task's registers          */
#define CD_NOTE_INFO     8266u    /* "ESP_CORE_DUMP_INFO": ver + ELF sha   */
#define CD_NOTE_EXTRA    677u     /* "ESP_EXTRA_INFO": crashed TCB first   */
#define CD_NOTE_PANIC    679u     /* "ESP_PANIC_DETAILS": the reason text  */
#define CD_PR_REG        72u      /* riscv_prstatus: pc, ra, sp, ...       */
/* pcTaskName in IDF FreeRTOS's TCB_t on the P4 build (seen in a dump:
   ls_tui, tdp_touch, IDLE0 all at 52). Checked for a printable,
   terminated name before it is believed, so a layout change only loses
   the name. */
#define CD_TCB_NAME      52u
/* Bytes of the crashed stack looked at. A printf's frame is ~600 B on its
   own; in a console command crash the command's return address sat 1180 B
   above sp, after 25 stale FreeRTOS and UART return addresses. */
#define CD_STACK_SCAN    4096u
#define CD_CHUNK         512u

typedef struct { uint32_t lo, hi, off; } cd_load_t;
typedef struct {
    ls_crash_read_at_fn rd;
    void *ctx;
    uint32_t len;                 /* the image's own size, checksum incl.  */
    cd_load_t *load;
    unsigned n_load;
} cd_t;

static bool cd_read(const cd_t *d, uint32_t off, void *buf, uint32_t n)
{
    return off <= d->len && n <= d->len - off && d->rd(d->ctx, off, buf, n);
}

/* esp_rom_crc32_le(0, ...) over the header and data is zlib's crc32. */
static uint32_t cd_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* A dumped address: its offset in the image, the whole [va, va+n) inside one
   PT_LOAD. */
static bool cd_va(const cd_t *d, uint32_t va, uint32_t n, uint32_t *off)
{
    for (unsigned i = 0; i < d->n_load; i++) {
        const cd_load_t *l = &d->load[i];
        if (va >= l->lo && va < l->hi && n <= l->hi - va) { *off = l->off + (va - l->lo); return true; }
    }
    return false;
}
static bool cd_dumped(const cd_t *d, uint32_t va)
{
    for (unsigned i = 0; i < d->n_load; i++)
        if (va >= d->load[i].lo && va < d->load[i].hi) return true;
    return false;
}
static bool cd_code(const ls_crash_range_t *code, size_t n, uint32_t a)
{
    for (size_t i = 0; i < n; i++) if (a >= code[i].lo && a < code[i].hi) return true;
    return false;
}

/* Printable ASCII only (or lower-case hex only), up to the first NUL or
   line end. */
static void cd_text(char *dst, size_t cap, const uint8_t *src, size_t n, bool hex_only)
{
    size_t w = 0;
    for (size_t i = 0; i < n && w + 1 < cap; i++) {
        const char c = (char)src[i];
        if (!c || c == '\n' || c == '\r') break;
        if (hex_only ? !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) : (c < 0x20 || c > 0x7e)) break;
        dst[w++] = c;
    }
    dst[w] = '\0';
}

static const char *cd_parse(cd_t *d, uint8_t *buf, const ls_crash_range_t *code, size_t n_code,
                            ls_crash_summary_t *out)
{
    /* The checksum, over everything before it, before any field is used. */
    uint32_t crc = 0;
    for (uint32_t at = 0; at < d->len - 4;) {
        const uint32_t n = d->len - 4 - at < CD_CHUNK ? d->len - 4 - at : CD_CHUNK;
        if (!cd_read(d, at, buf, n)) return "read failed";
        crc = cd_crc32(crc, buf, n);
        at += n;
    }
    if (!cd_read(d, d->len - 4, buf, 4)) return "read failed";
    if (read_le32(buf) != crc) return "checksum mismatch";

    uint8_t eh[CD_EHDR];
    if (!cd_read(d, CD_HDR, eh, sizeof(eh))) return "read failed";
    if (eh[0] != 0x7f || memcmp(eh + 1, "ELF", 3) || eh[4] != 1 || eh[5] != 1) return "not a 32-bit LE ELF";
    const uint32_t e_type = (uint32_t)eh[16] | ((uint32_t)eh[17] << 8);
    const uint32_t phoff = read_le32(eh + 28);
    const uint32_t phent = (uint32_t)eh[42] | ((uint32_t)eh[43] << 8);
    const uint32_t phnum = (uint32_t)eh[44] | ((uint32_t)eh[45] << 8);
    if (e_type != 4 || phent != CD_PHDR || !phnum || phnum > CD_MAX_PHDRS) return "not an ELF core";
    const uint32_t elf_len = d->len - 4 - CD_HDR;
    if (phoff > elf_len || phnum * CD_PHDR > elf_len - phoff) return "headers out of bounds";

    /* Where each dumped address lives in the image. */
    d->n_load = 0;
    for (uint32_t i = 0; i < phnum; i++) {
        uint8_t ph[CD_PHDR];
        if (!cd_read(d, CD_HDR + phoff + i * CD_PHDR, ph, sizeof(ph))) return "read failed";
        const uint32_t off = read_le32(ph + 4), va = read_le32(ph + 8), sz = read_le32(ph + 16);
        if (read_le32(ph) != 1 || !sz || d->n_load >= CD_MAX_LOADS) continue;
        if (off > elf_len || sz > elf_len - off || va > UINT32_MAX - sz) continue;
        d->load[d->n_load++] = (cd_load_t){ va, va + sz, CD_HDR + off };
    }

    /* The notes: the first "CORE" is the task that crashed (IDF writes it
       first), then the dump's own info, extra info and panic reason. */
    bool have_regs = false;
    uint32_t sp = 0, tcb = 0;
    unsigned notes = 0;
    for (uint32_t i = 0; i < phnum; i++) {
        uint8_t ph[CD_PHDR];
        if (!cd_read(d, CD_HDR + phoff + i * CD_PHDR, ph, sizeof(ph))) return "read failed";
        if (read_le32(ph) != 4) continue;
        const uint32_t off = read_le32(ph + 4), sz = read_le32(ph + 16);
        if (off > elf_len || sz > elf_len - off) return "note out of bounds";
        uint32_t at = CD_HDR + off;
        const uint32_t end = at + sz;
        while (end - at >= 12 && notes++ < CD_MAX_NOTES) {
            uint8_t nh[12];
            if (!cd_read(d, at, nh, sizeof(nh))) return "read failed";
            const uint32_t nsz = read_le32(nh), dsz = read_le32(nh + 4), type = read_le32(nh + 8);
            if (nsz > 32 || dsz > 4096) return "bad note";
            const uint32_t desc = at + 12 + ((nsz + 3u) & ~3u);
            const uint32_t next = desc + ((dsz + 3u) & ~3u);
            if (next > end) return "bad note";
            char name[33] = "";
            if (!cd_read(d, at + 12, name, nsz)) return "read failed";
            name[nsz < 32 ? nsz : 32] = '\0';
            const uint32_t take = dsz < CD_CHUNK ? dsz : CD_CHUNK;
            if (!have_regs && type == CD_NOTE_PRSTATUS && !strcmp(name, "CORE") && dsz >= CD_PR_REG + 12) {
                if (!cd_read(d, desc + CD_PR_REG, buf, 12)) return "read failed";
                out->pc = read_le32(buf); out->ra = read_le32(buf + 4); sp = read_le32(buf + 8);
                have_regs = true;
            } else if (type == CD_NOTE_INFO && !strcmp(name, "ESP_CORE_DUMP_INFO") && dsz > 4) {
                if (!cd_read(d, desc, buf, take)) return "read failed";
                cd_text(out->elf_sha, sizeof(out->elf_sha), buf + 4, take - 4, true);
            } else if (type == CD_NOTE_EXTRA && !strcmp(name, "ESP_EXTRA_INFO") && dsz >= 4) {
                if (!cd_read(d, desc, buf, 4)) return "read failed";
                tcb = read_le32(buf);
            } else if (type == CD_NOTE_PANIC && !strcmp(name, "ESP_PANIC_DETAILS") && dsz) {
                if (!cd_read(d, desc, buf, take)) return "read failed";
                cd_text(out->reason, sizeof(out->reason), buf, take, false);
            }
            at = next;
        }
    }
    if (!have_regs) return "no task registers";

    uint32_t o;
    if (tcb && cd_va(d, tcb + CD_TCB_NAME, LS_CRASH_TASK_MAX, &o) && cd_read(d, o, buf, LS_CRASH_TASK_MAX) &&
        memchr(buf, 0, LS_CRASH_TASK_MAX)) {
        cd_text(out->task, sizeof(out->task), buf, LS_CRASH_TASK_MAX, false);
        if (strlen(out->task) != strlen((const char *)buf)) out->task[0] = '\0';
    }

    /* Code addresses on the crashed task's stack, from sp up: what a caller
       left as its return address. Data pointers into memory the dump holds
       are not code; neither is an odd address (RV32IC code is 2-aligned). */
    out->bt_scanned = true;
    if (sp & 3u) return NULL;
    for (unsigned i = 0; i < d->n_load; i++) {
        const cd_load_t *l = &d->load[i];
        if (sp < l->lo || sp >= l->hi) continue;
        uint32_t n = l->hi - sp;
        if (n > CD_STACK_SCAN) n = CD_STACK_SCAN;
        n &= ~3u;
        for (uint32_t at = 0; at < n && out->bt_depth < LS_CRASH_MAX_BT; at += CD_CHUNK) {
            const uint32_t k = n - at < CD_CHUNK ? n - at : CD_CHUNK;
            if (!cd_read(d, l->off + (sp - l->lo) + at, buf, k)) { out->bt_corrupted = true; break; }
            for (uint32_t w = 0; w < k && out->bt_depth < LS_CRASH_MAX_BT; w += 4) {
                const uint32_t a = read_le32(buf + w);
                if ((a & 1u) || !cd_code(code, n_code, a) || cd_dumped(d, a)) continue;
                if (a == out->pc || a == out->ra) continue;
                if (out->bt_depth && out->bt[out->bt_depth - 1] == a) continue;
                out->bt[out->bt_depth++] = a;
            }
        }
        break;
    }
    return NULL;
}

ls_crash_read_result_t ls_crash_parse_elf(ls_crash_read_at_fn read_at, void *ctx,
                                          const ls_crash_range_t *code, size_t n_code,
                                          ls_crash_summary_t *out)
{
    if (!out) return LS_CRASH_READ_UNAVAILABLE;
    out->parse_why = NULL;
    if (!read_at) { out->parse_why = "no reader"; return LS_CRASH_READ_METADATA; }
    if ((out->core_dump_version & 0xffffu) != CD_VER_ELF_CRC32) {
        out->parse_why = "dump format not read on the device";
        return LS_CRASH_READ_METADATA;
    }
    if (!out->header_bounds_ok || out->declared_size < CD_HDR + CD_EHDR + 4) {
        out->parse_why = "size out of bounds";
        return LS_CRASH_READ_METADATA;
    }
    cd_t d = { read_at, ctx, out->declared_size, malloc(CD_MAX_LOADS * sizeof(cd_load_t)), 0 };
    uint8_t *buf = malloc(CD_CHUNK);
    ls_crash_summary_t s = *out;
    const char *why = d.load && buf ? cd_parse(&d, buf, code, n_code, &s) : "no memory";
    free(buf);
    free(d.load);
    if (why) { out->parse_why = why; return LS_CRASH_READ_METADATA; }
    *out = s;
    return LS_CRASH_READ_OK;
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
        size_t m = 0;
        if (s.parse_why) m = append(buf, buflen, m, "crash: dump not read (%s)\n", s.parse_why);
        return append(buf, buflen, m,
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
    /* A dump read on the board has no fault code to give: say nothing
       rather than a line of zeros. */
    if (s.fault_code || s.fault_addr || !s.bt_scanned)
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
        n = append(buf, buflen, n, s.bt_scanned ? "       backtrace (stack scan):" : "       backtrace:");
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
    if (s.bt_scanned)
        n = append(buf, buflen, n,
                   "       decode: tools/ls_crash_decode.py <crash ELF> with these "
                   "lines; dump preserved, back up before 'crash clear'\n");
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
