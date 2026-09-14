#include "scan_import.h"
#include "scan_engine.h"
#include "ls_nvs_safe.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

typedef struct { const char *path; char *message; size_t capacity; } request_t;
static EXT_RAM_BSS_ATTR scan_channel_t staged[SCAN_MAX_CHANNELS];
static int staged_count;
static bool staged_valid;
static int64_t staged_until;
void scan_import_begin(void) { staged_count = 0; staged_valid = true; staged_until = esp_timer_get_time() + 60000000; }
void scan_import_abort(void) { staged_valid = false; staged_count = 0; }
bool scan_import_add(const char *line)
{
    if (!staged_valid || esp_timer_get_time() > staged_until || staged_count >= SCAN_MAX_CHANNELS ||
        !scan_import_line(line, &staged[staged_count])) { staged_valid = false; return false; }
    for (int i = 0; i < staged_count; ++i)
        if (staged[i].freq_hz == staged[staged_count].freq_hz && staged[i].mode == staged[staged_count].mode &&
            staged[i].zone == staged[staged_count].zone) { staged_valid = false; return false; }
    ++staged_count; staged_until = esp_timer_get_time() + 60000000; return true;
}
static esp_err_t commit_worker(void *unused)
{
    (void)unused;
    scan_engine_stop();
    return scan_channels_replace(staged, staged_count) ? ESP_OK : ESP_FAIL;
}
bool scan_import_commit(char *message, size_t capacity)
{
    bool ok = staged_valid && staged_count > 0 && esp_timer_get_time() <= staged_until &&
              ls_nvs_call(commit_worker, NULL, 0) == ESP_OK;
    snprintf(message, capacity, ok ? "Imported %d channels; scan stopped" : "Import failed; list retained", staged_count);
    scan_import_abort(); return ok;
}
static esp_err_t import_worker(void *arg)
{
    request_t *r = arg;
    FILE *f = fopen(r->path, "rb");
    if (!f) { snprintf(r->message, r->capacity, "Import file not found"); return ESP_FAIL; }
    if (fseek(f, 0, SEEK_END) || ftell(f) > 4 * 1024 * 1024 || ftell(f) <= 0 || fseek(f, 0, SEEK_SET)) {
        fclose(f); snprintf(r->message, r->capacity, "Import must be 1 byte..4 MB"); return ESP_FAIL;
    }
    scan_channel_t *rows = heap_caps_calloc(SCAN_MAX_CHANNELS, sizeof(*rows), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rows) { fclose(f); snprintf(r->message, r->capacity, "No import memory"); return ESP_ERR_NO_MEM; }
    char line[192]; int count = 0, number = 0; bool ok = true;
    long offset = ftell(f);
    while (fgets(line, sizeof(line), f)) {
        ++number;
        long bytes = ftell(f) - offset;
        offset += bytes;
        if (bytes < 1 || bytes >= (long)sizeof(line) || memchr(line, 0, (size_t)bytes)) { ok = false; break; }
        if (!strchr(line, '\n') && !feof(f)) { ok = false; break; }
        if (number == 1) { if (strcmp(line, "LSCAN1\n") && strcmp(line, "LSCAN1\r\n")) { ok = false; break; } continue; }
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (count == SCAN_MAX_CHANNELS || !scan_import_line(line, &rows[count])) { ok = false; break; }
        for (int i = 0; i < count; ++i)
            if (rows[i].freq_hz == rows[count].freq_hz && rows[i].mode == rows[count].mode && rows[i].zone == rows[count].zone)
                ok = false;
        if (!ok) break;
        ++count;
    }
    ok = ok && !ferror(f) && count > 0;
    fclose(f);
    if (ok) {
        scan_engine_stop();
        ok = scan_channels_replace(rows, count);
        snprintf(r->message, r->capacity, ok ? "Imported %d channels; scan stopped" : "Import save failed; list retained", count);
    } else snprintf(r->message, r->capacity, "Rejected import at line %d; list retained", number);
    heap_caps_free(rows);
    return ok ? ESP_OK : ESP_FAIL;
}
bool scan_import_file(const char *path, char *message, size_t capacity)
{
    if (!message || !capacity) return false;
    if (!path || strncmp(path, "/sdcard/", 8) || strstr(path, "..") || strlen(path) > 96) {
        snprintf(message, capacity, "Use an SD path without '..'"); return false;
    }
    request_t request = {path, message, capacity};
    snprintf(message, capacity, "Import worker unavailable");
    esp_err_t err = ls_nvs_call(import_worker, &request, 0);
    return err == ESP_OK;
}
