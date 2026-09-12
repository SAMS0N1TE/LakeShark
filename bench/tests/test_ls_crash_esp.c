#include "ls_test.h"
#include "ls_crash.h"
#include "ls_nvs_safe.h"
#include "esp_partition.h"
#include <string.h>
static esp_partition_t part = {0x10000, 65536};
static uint8_t bytes[20];
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
    LS_CHECK(p == &part); LS_EQ_INT(off, 0);
    LS_CHECK(n <= 20 && n <= part.size);
    reads++;
    if (read_error) return ESP_FAIL;
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
