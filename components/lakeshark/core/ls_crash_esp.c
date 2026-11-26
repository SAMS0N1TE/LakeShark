/* Explicit crash parsing faulted after a valid checksum, overwriting
 * the original dump. Never call the IDF ELF parser, even on explicit reads.
 * The raw header, then ls_crash_parse_elf's bounded reads (checksum first);
 * the full partition stays as it is for offline decoding. */
#include "ls_crash.h"
#include "ls_nvs_safe.h"
#include <string.h>
#include "esp_core_dump.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "sdkconfig.h"
static const char *TAG = "ls_crash";
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
static bool part_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    const esp_partition_t *part = ctx;
    return off <= part->size && len <= part->size - off &&
           esp_partition_read(part, off, buf, len) == ESP_OK;
}
/* Where code can be in the crashed image: flash text up to the running
   image's rodata, the ROM, and internal RAM's text. Taken from the running
   image; a dump from another build is still scanned, a little looser. */
#ifdef ESP_PLATFORM
#include "soc/soc.h"
extern char _instruction_reserved_start[], _rodata_reserved_start[];
extern char _iram_text_start[], _iram_text_end[];
static size_t code_ranges(ls_crash_range_t *r)
{
    r[0] = (ls_crash_range_t){ (uint32_t)(uintptr_t)_instruction_reserved_start,
                               (uint32_t)(uintptr_t)_rodata_reserved_start };
    r[1] = (ls_crash_range_t){ SOC_IROM_MASK_LOW, SOC_IROM_MASK_HIGH };
    r[2] = (ls_crash_range_t){ (uint32_t)(uintptr_t)_iram_text_start, (uint32_t)(uintptr_t)_iram_text_end };
    return 3;
}
#else
/* The bench: the P4's map, as the device would give it. */
static size_t code_ranges(ls_crash_range_t *r)
{
    r[0] = (ls_crash_range_t){ 0x40000020u, 0x40360020u };
    r[1] = (ls_crash_range_t){ 0x4fc00000u, 0x4fc20000u };
    r[2] = (ls_crash_range_t){ 0x4ff00330u, 0x4ff16a80u };
    return 3;
}
#endif
#endif
/* parse: the explicit read (crash, a report); presence alone never parses. */
static ls_crash_read_result_t read_metadata(ls_crash_summary_t *out, bool parse)
{
    memset(out, 0, sizeof(*out));
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) return LS_CRASH_READ_NOT_FOUND;
    out->partition_address = part->address;
    out->partition_size = part->size;
    uint8_t header[20];
    size_t n = part->size < sizeof(header) ? part->size : sizeof(header);
    if (!n || esp_partition_read(part, 0, header, n) != ESP_OK)
        return LS_CRASH_READ_UNAVAILABLE;
    const ls_crash_read_result_t r = ls_crash_inspect_header(header, n, out);
    if (r != LS_CRASH_READ_METADATA || !parse) return r;
    ls_crash_range_t code[3];
    return ls_crash_parse_elf(part_read, (void *)part, code, code_ranges(code), out);
#else
    return LS_CRASH_READ_NOT_FOUND;
#endif
}
typedef struct {
    ls_crash_summary_t *out;
    ls_crash_read_result_t result;
    bool parse;
} crash_read_job_t;

static esp_err_t read_on_flash_stack(void *ctx)
{
    crash_read_job_t *job = ctx;
    job->result = read_metadata(job->out, job->parse);
    return ESP_OK;
}

/* a 20-byte partition read from the REPL asserted on a TCM stack
 * (SP 0x30101090). Bounded reads still disable the flash cache. Use the
 * existing reserved DRAM worker; do not allocate another scarce stack. */
static ls_crash_read_result_t source_job(ls_crash_summary_t *out, bool parse)
{
    memset(out, 0, sizeof(*out));
    crash_read_job_t job = { .out = out, .result = LS_CRASH_READ_UNAVAILABLE, .parse = parse };
    if (ls_nvs_call(read_on_flash_stack, &job, 0) != ESP_OK)
        return LS_CRASH_READ_UNAVAILABLE;
    return job.result;
}
static ls_crash_read_result_t esp_source_read(ls_crash_summary_t *out)
{
    return source_job(out, true);
}
static bool esp_source_present(void)
{
    ls_crash_summary_t metadata;
    return source_job(&metadata, false) != LS_CRASH_READ_NOT_FOUND;
}
static esp_err_t erase_on_flash_stack(void *ctx)
{
    (void)ctx;
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    /* Only explicit crash clear can reach this write. */
    esp_err_t e = esp_core_dump_image_erase();
    if (e != ESP_OK && e != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "coredump erase failed: %s", esp_err_to_name(e));
        return e;
    }
#endif
    return ESP_OK;
}
static bool esp_source_erase(void)
{
    return ls_nvs_call(erase_on_flash_stack, NULL, 0) == ESP_OK;
}
static const ls_crash_source_t s_esp_source = {
    .present = esp_source_present, .read = esp_source_read, .erase = esp_source_erase,
};
void ls_crash_boot_setup(void)
{
    /* Also used by early safe-mode startup, before the backend is running. */
    ls_nvs_init();
    ls_crash_init(&s_esp_source, __DATE__ " " __TIME__);
    char notice[192];
    if (ls_crash_boot_notice(notice, sizeof(notice)) > 0)
        ESP_LOGW(TAG, "%s", notice);
}
