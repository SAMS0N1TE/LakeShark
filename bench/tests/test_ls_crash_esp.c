#include "ls_test.h"
#include "ls_crash.h"
#include "ls_nvs_safe.h"
#include "esp_partition.h"
#include <string.h>
static esp_partition_t part = {0x10000, 65536};
static uint8_t bytes[20];
static uint8_t image[65536];   /* a whole stored dump, when use_image */
static bool use_image;
static int far_reads;           /* reads past the 20-byte header */
static int reads, erases;
static int read_error, erase_error;
static bool missing;
static bool on_flash_stack;
static esp_err_t dispatch_error;
esp_err_t ls_nvs_init(void) { return ESP_OK; }
esp_err_t ls_nvs_call(ls_nvs_fn_t fn, void *ctx, unsigned stack_bytes)
{
    (void)stack_bytes;
    if (dispatch_error) return dispatch_error;
    on_flash_stack = true;
    esp_err_t result = fn(ctx);
    on_flash_stack = false;
    return result;
}
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{ (void)type; (void)subtype; (void)label; return missing ? NULL : &part; }
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *out, size_t n)
{
    LS_CHECK(on_flash_stack);
    LS_CHECK(p == &part);
    LS_CHECK(off <= part.size && n <= part.size - off);
    reads++;
    if (off || n > 20) far_reads++;
    if (read_error) return ESP_FAIL;
    if (use_image) { memcpy(out, image + off, n); return ESP_OK; }
    LS_EQ_INT(off, 0); LS_CHECK(n <= 20);
    memcpy(out, bytes, n); return ESP_OK;
}
esp_err_t esp_core_dump_image_erase(void)
{ LS_CHECK(on_flash_stack); erases++; return erase_error ? ESP_FAIL : ESP_OK; }
static void reset(void)
{
    memset(bytes, 0, sizeof(bytes)); bytes[2] = 1; /* 64 KiB declared */
    bytes[4] = 0xff; bytes[7] = 0xfe; /* unknown version */
    part.size = 65536; missing = false;
    reads = erases = read_error = erase_error = 0;
    on_flash_stack = false; dispatch_error = ESP_OK;
    use_image = false; far_reads = 0;
}
LS_CASE(dispatch_failure_preserves_dump_without_accessing_flash)
{
    reset(); dispatch_error = ESP_FAIL; ls_crash_boot_setup();
    char buf[768]; ls_crash_format(buf, sizeof(buf));
    LS_CHECK(ls_crash_present());
    LS_CHECK(strstr(buf, "unavailable"));
    LS_CHECK(!ls_crash_erase());
    LS_EQ_INT(reads, 0); LS_EQ_INT(erases, 0);
}
LS_CASE(actual_adapter_repeated_reads_never_parse_or_erase)
{
    reset(); ls_crash_boot_setup();
    LS_CHECK(ls_crash_present()); LS_EQ_INT(reads, 1);
    for (int i = 0; i < 10; ++i) {
        char buf[768]; ls_crash_format(buf, sizeof(buf));
        LS_CHECK(strstr(buf, "parsing disabled"));
        LS_CHECK(strstr(buf, "0xfe0000ff"));
        LS_CHECK(strstr(buf, "checksum NOT checked"));
        LS_CHECK(strstr(buf, "matching crash ELF"));
    }
    LS_EQ_INT(reads, 11); LS_EQ_INT(erases, 0);
    erase_error = 1; LS_CHECK(!ls_crash_erase()); LS_CHECK(ls_crash_present());
    erase_error = 0; LS_CHECK(ls_crash_erase()); LS_CHECK(!ls_crash_present());
}
LS_CASE(corrupt_truncated_and_unreadable_remain_present)
{
    reset(); bytes[3] = 0x80; ls_crash_boot_setup();
    char buf[768]; ls_crash_format(buf, sizeof(buf));
    LS_CHECK(strstr(buf, "invalid size")); LS_CHECK(ls_crash_present());
    for (unsigned n = 0; n < 20; ++n) {
        part.size = n; ls_crash_boot_setup(); ls_crash_format(buf, sizeof(buf));
        LS_CHECK(ls_crash_present()); LS_CHECK(strstr(buf, "preserved"));
    }
    reset(); read_error = 1; ls_crash_boot_setup();
    ls_crash_format(buf, sizeof(buf)); LS_CHECK(ls_crash_present());
    LS_EQ_INT(erases, 0);
}
LS_CASE(empty_and_missing_do_not_claim_a_crash)
{
    reset(); memset(bytes, 0xff, 4); ls_crash_boot_setup();
    LS_CHECK(!ls_crash_present());
    missing = true; ls_crash_boot_setup(); LS_CHECK(!ls_crash_present());
    LS_EQ_INT(erases, 0);
}
LS_CASE(header_bounds_and_output_truncation)
{
    reset(); ls_crash_summary_t s = {0}; s.partition_size = 65536;
    LS_EQ_INT(ls_crash_inspect_header(NULL, 20, &s), LS_CRASH_READ_UNAVAILABLE);
    LS_EQ_INT(ls_crash_inspect_header(bytes, 20, NULL), LS_CRASH_READ_UNAVAILABLE);
    LS_EQ_INT(ls_crash_inspect_header(bytes, 20, &s), LS_CRASH_READ_METADATA);
    LS_CHECK(s.header_bounds_ok);
    memset(bytes, 0, 4);
    LS_EQ_INT(ls_crash_inspect_header(bytes, 20, &s), LS_CRASH_READ_METADATA);
    LS_CHECK(!s.header_bounds_ok);
    ls_crash_boot_setup();
    char buf[32]; memset(buf, 0x7f, sizeof(buf));
    LS_EQ_INT(ls_crash_format(buf, 0), 0); LS_EQ_INT(buf[0], 0x7f);
    LS_EQ_INT(ls_crash_format(buf, 1), 0); LS_EQ_INT(buf[0], 0);
    LS_CHECK(ls_crash_format(buf, sizeof(buf)) < sizeof(buf));
    LS_EQ_INT(buf[31], 0); LS_EQ_INT(erases, 0);
}

/* ---- a whole ELF core dump, as IDF writes one on the P4 (CRC32 build) ---- */
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static uint32_t zcrc(const uint8_t *p, size_t n)
{
    uint32_t c = ~0u;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}
static size_t note(uint8_t *p, const char *name, uint32_t type, const void *desc, uint32_t dsz)
{
    const uint32_t nsz = (uint32_t)strlen(name) + 1;
    put32(p, nsz); put32(p + 4, dsz); put32(p + 8, type);
    memcpy(p + 12, name, nsz);
    const size_t d = 12 + ((nsz + 3) & ~3u);
    memcpy(p + d, desc, dsz);
    return d + ((dsz + 3) & ~3u);
}
#define TCB_VA   0x4ff4cbc0u
#define STACK_VA 0x4ff4e710u
#define SP_VA    0x4ff4e7b0u
/* Fills image[] and returns its size. The crashed task's stack holds, from
   sp: a flash return address, a pointer into the dumped TCB (data), an odd
   word, the same flash address again, ROM and IRAM code, a data pointer
   into undumped RAM, and plain numbers. */
static uint32_t build_dump(void)
{
    memset(image, 0, sizeof(image));
    uint8_t *e = image + 24;                       /* the ELF */
    const uint32_t phnum = 4, phoff = 52;
    uint32_t at = phoff + phnum * 32;              /* data after the headers */
    /* PT_NOTE 0: the crashed task's registers, then another task's */
    const uint32_t n0 = at;
    uint8_t pr[204] = {0};
    put32(pr + 72, 0x4200abcdu & ~1u); put32(pr + 76, 0x4ff0c210u); put32(pr + 80, SP_VA);
    at += note(e + at, "CORE", 1, pr, sizeof(pr));
    put32(pr + 72, 0x40001000u); put32(pr + 80, 0x48f59800u);
    at += note(e + at, "CORE", 1, pr, sizeof(pr));
    const uint32_t n0sz = at - n0;
    /* PT_LOAD 1: the TCB, its name at 52 */
    const uint32_t l1 = at;
    memcpy(e + at + 52, "ls_tui", 7);
    at += 352;
    /* PT_LOAD 2: the stack */
    const uint32_t l2 = at;
    const uint32_t words[] = { 0x4000c6f8u, TCB_VA + 8, 0x4000c6f9u, 0x4000c6f8u, 0x4000c6f8u,
                               0x4fc01234u, 0x4ff0d55eu, 0x4ff61000u, 12345u, 0x4009512cu };
    const uint32_t sp_off = SP_VA - STACK_VA;
    for (unsigned i = 0; i < sizeof(words) / 4; i++) put32(e + at + sp_off + 4 * i, words[i]);
    put32(e + at + 4, 0x40099990u);                /* below sp: not scanned */
    at += 1536;
    /* PT_NOTE 3: the dump's info, extra info and panic reason */
    const uint32_t n3 = at;
    uint8_t info[72] = {0}; put32(info, 0x00120102u); memcpy(info + 4, "420b2209a", 9);
    at += note(e + at, "ESP_CORE_DUMP_INFO", 8266, info, sizeof(info));
    uint8_t extra[8] = {0}; put32(extra, TCB_VA);
    at += note(e + at, "ESP_EXTRA_INFO", 677, extra, sizeof(extra));
    at += note(e + at, "ESP_PANIC_DETAILS", 679, "assert failed: 0x4ff103be\nmore", 30);
    const uint32_t n3sz = at - n3;
    /* headers */
    e[0] = 0x7f; memcpy(e + 1, "ELF", 3); e[4] = 1; e[5] = 1; e[6] = 1;
    put16(e + 16, 4); put16(e + 18, 243); put32(e + 20, 1);
    put32(e + 28, phoff); put16(e + 40, 52); put16(e + 42, 32); put16(e + 44, (uint16_t)phnum);
    const uint32_t ph[4][3] = { { 4, n0, n0sz }, { 1, l1, 352 }, { 1, l2, 1536 }, { 4, n3, n3sz } };
    const uint32_t va[4] = { 0, TCB_VA, STACK_VA, 0 };
    for (unsigned i = 0; i < phnum; i++) {
        uint8_t *p = e + phoff + 32 * i;
        put32(p, ph[i][0]); put32(p + 4, ph[i][1]); put32(p + 8, va[i]); put32(p + 12, va[i]);
        put32(p + 16, ph[i][2]); put32(p + 20, ph[i][2]);
    }
    const uint32_t len = 24 + at + 4;
    put32(image, len); put32(image + 4, 0x00120102u); put32(image + 20, 103);
    put32(image + len - 4, zcrc(image, len - 4));
    use_image = true;
    return len;
}
static void reseal(uint32_t len) { put32(image + len - 4, zcrc(image, len - 4)); }

LS_CASE(a_stored_elf_dump_names_the_crash_without_the_idf_parser)
{
    reset(); build_dump(); ls_crash_boot_setup();
    LS_CHECK(ls_crash_present());
    LS_EQ_INT(far_reads, 0);                       /* presence: header only */
    char buf[1024]; ls_crash_format(buf, sizeof(buf));
    LS_CHECK(strstr(buf, "task \"ls_tui\" pc=0x4200abcc ra=0x4ff0c210"));
    LS_CHECK(strstr(buf, "reason: assert failed: 0x4ff103be\n"));
    LS_CHECK(strstr(buf, "elf-sha=420b2209a"));
    /* flash, ROM and IRAM code from sp up, once each in a row; not the TCB
       pointer, the odd word, undumped RAM, numbers, or anything below sp */
    LS_CHECK(strstr(buf, "backtrace (stack scan): 0x4000c6f8 0x4fc01234 0x4ff0d55e 0x4009512c\n"));
    LS_CHECK(!strstr(buf, "0x40099990"));
    LS_CHECK(strstr(buf, "tools/ls_crash_decode.py"));
    LS_CHECK(!strstr(buf, "more"));
    LS_EQ_INT(erases, 0);
}
LS_CASE(a_dump_whose_checksum_fails_is_kept_and_not_believed)
{
    reset(); const uint32_t len = build_dump();
    image[200] ^= 0x40;                            /* a flipped bit in a note */
    ls_crash_boot_setup();
    char buf[1024]; ls_crash_format(buf, sizeof(buf));
    LS_CHECK(strstr(buf, "dump not read (checksum mismatch)"));
    LS_CHECK(strstr(buf, "preserved")); LS_CHECK(!strstr(buf, "ls_tui"));
    LS_CHECK(ls_crash_present()); LS_EQ_INT(erases, 0);
    (void)len;
}
LS_CASE(corrupt_elf_headers_and_notes_stay_in_bounds)
{
    char buf[1024];
    /* each a checksum-valid image the parser must refuse or bound */
    struct { uint32_t off; uint32_t value; int width; const char *why; } bad[] = {
        { 24 + 44, 0xffff, 2, "not an ELF core" },          /* phnum huge      */
        { 24 + 28, 0x7ffffff0u, 4, "headers out of bounds" },/* phoff past end  */
        { 24 + 16, 2, 2, "not an ELF core" },               /* not ET_CORE     */
        { 24 + 0, 0, 1, "not a 32-bit LE ELF" },            /* magic           */
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reset(); const uint32_t len = build_dump();
        if (bad[i].width == 4) put32(image + bad[i].off, bad[i].value);
        else if (bad[i].width == 2) put16(image + bad[i].off, (uint16_t)bad[i].value);
        else image[bad[i].off] = (uint8_t)bad[i].value;
        reseal(len); ls_crash_boot_setup(); ls_crash_format(buf, sizeof(buf));
        LS_CHECK(strstr(buf, bad[i].why)); LS_CHECK(ls_crash_present());
    }
    /* a note claiming more than its segment, and a load segment past the end */
    reset(); uint32_t len = build_dump();
    put32(image + 24 + 52 + 32 * 4 + 4, 0x00fffff0u); reseal(len);
    ls_crash_boot_setup(); ls_crash_format(buf, sizeof(buf));
    LS_CHECK(strstr(buf, "dump not read (bad note)"));
    reset(); len = build_dump();
    put32(image + 24 + 52 + 32 * 1 + 4, 0x7fff0000u); reseal(len);
    ls_crash_boot_setup(); ls_crash_format(buf, sizeof(buf));
    LS_CHECK(strstr(buf, "task \"\" pc=0x4200abcc"));    /* TCB gone: no name  */
    /* a size that claims the whole partition reads only inside it */
    reset(); len = build_dump(); put32(image, 65536); put32(image + 65532, zcrc(image, 65532));
    ls_crash_boot_setup(); ls_crash_format(buf, sizeof(buf));
    LS_CHECK(strstr(buf, "crash: task") || strstr(buf, "dump not read"));
    LS_EQ_INT(erases, 0);
}
LS_CASE(the_caller_under_a_printf_and_stale_frames_is_still_listed)
{
    /* A console command crashed in printf: 30 stale return addresses from
       earlier UART writes sat between sp and the command's own, 1.2 KB up. */
    reset(); const uint32_t len = build_dump();
    uint8_t *stack = image + 24 + 52 + 4 * 32;
    while (memcmp(stack + 52, "ls_tui", 6)) stack++;          /* the TCB ... */
    stack += 352 + (SP_VA - STACK_VA);                       /* ... then sp */
    memset(stack, 0, 1300);
    for (unsigned i = 0; i < 30; i++) put32(stack + 4 * i, 0x40100000u + 0x40u * i);
    put32(stack + 1200, 0x40177770u);
    reseal(len); ls_crash_boot_setup();
    char buf[1024]; ls_crash_format(buf, sizeof(buf));
    LS_CHECK(strstr(buf, " 0x40100740 0x40177770\n"));
    LS_CHECK(!strstr(buf, "fault_code=0x00000000"));
}
