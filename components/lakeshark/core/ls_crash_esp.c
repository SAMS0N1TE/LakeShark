/* LS-767: Explicit crash parsing faulted after a valid checksum, overwriting
 * the original dump. Never call the IDF ELF parser, even on explicit reads.
 * Fixed-size raw header inspection only; preserve full partition for offline. */
#include "ls_crash.h"
#include <string.h>
#include "esp_core_dump.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "sdkconfig.h"
static const char *TAG = "ls_crash";
static ls_crash_read_result_t esp_source_read(ls_crash_summary_t *out)
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
    return ls_crash_inspect_header(header, n, out);
#else
    return LS_CRASH_READ_NOT_FOUND;
#endif
}
static bool esp_source_present(void)
{
    ls_crash_summary_t metadata;
    return esp_source_read(&metadata) != LS_CRASH_READ_NOT_FOUND;
}
static bool esp_source_erase(void)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    /* Only explicit crash clear can reach this write. */
    esp_err_t e = esp_core_dump_image_erase();
    if (e != ESP_OK && e != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "coredump erase failed: %s", esp_err_to_name(e));
        return false;
    }
#endif
    return true;
}
static const ls_crash_source_t s_esp_source = {
    .present = esp_source_present, .read = esp_source_read, .erase = esp_source_erase,
};
void ls_crash_boot_setup(void)
{
    ls_crash_init(&s_esp_source, __DATE__ " " __TIME__);
    char notice[192];
    if (ls_crash_boot_notice(notice, sizeof(notice)) > 0)
        ESP_LOGW(TAG, "%s", notice);
}
