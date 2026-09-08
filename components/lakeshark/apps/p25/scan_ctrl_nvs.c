/* LS-670: NVS wrapper for scan_ctrl.
 *
 * scan_ctrl.c is pure logic and knows nothing about NVS - the bench compiles
 * it as-is. This file binds the serialised blob to the same "sdr-tool" NVS
 * namespace the rest of the persisted settings use, so a factory-reset of the
 * settings partition clears the scan lists at the same time.
 *
 * Kept in its own translation unit so scan_ctrl.c stays host-buildable and
 * the ESP-IDF include of nvs.h does not spread to the bench. */

#include "scan_ctrl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "ls_nvs_safe.h"

#include <string.h>

static const char *TAG    = "p25scan";
static const char *NS     = "sdr-tool";
static const char *BLOBK  = "p25scanv1";

/* Sized to hold every possible mutation: header + max lockout + max allow. */
#define P25_SCAN_NVS_MAX (12 + 4 + 4 + \
                          P25_SCAN_LOCKOUT_MAX * 2 + \
                          P25_SCAN_ALLOW_MAX   * 2)

/* LS-671: NVS must not run on a task whose stack is in PSRAM - see
 * components/lakeshark/core/ls_nvs_safe.h. Serialising stays here; only the
 * flash access moves. */

typedef struct { uint8_t *buf; size_t len; size_t out_len; } scan_blob_t;

static esp_err_t scan_read(void *ctx)
{
    scan_blob_t *b = (scan_blob_t *)ctx;
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    b->out_len = b->len;
    err = nvs_get_blob(h, BLOBK, b->buf, &b->out_len);
    nvs_close(h);
    return err;
}

static esp_err_t scan_write(void *ctx)
{
    scan_blob_t *b = (scan_blob_t *)ctx;
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, BLOBK, b->buf, b->len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void p25_scan_persist_reload(void)
{
    uint8_t buf[P25_SCAN_NVS_MAX];
    scan_blob_t b = { .buf = buf, .len = sizeof(buf) };
    esp_err_t err = ls_nvs_call(scan_read, &b, 0);
    size_t sz = b.out_len;
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no stored scan blob (%d)", err);
        return;
    }
    if (!p25_scan_persist_load(&g_p25_scan, buf, sz)) {
        ESP_LOGW(TAG, "stored scan blob rejected (%u bytes) - keeping defaults",
                 (unsigned)sz);
        return;
    }
    ESP_LOGI(TAG, "scan reload: lockouts=%u allow=%u mode=%d hold=%u",
             (unsigned)g_p25_scan.lockout_count,
             (unsigned)g_p25_scan.allow_count,
             (int)g_p25_scan.list_mode,
             (unsigned)g_p25_scan.hold_tg);
}

void p25_scan_persist_save_now(void)
{
    uint8_t buf[P25_SCAN_NVS_MAX];
    size_t n = p25_scan_persist_save(&g_p25_scan, buf, sizeof(buf));
    if (n == 0) return;

    /* Serialising is pure memory work and stays on the caller. Only the flash
     * access moves - see LS-671 above. */
    scan_blob_t b = { .buf = buf, .len = n };
    esp_err_t err = ls_nvs_call(scan_write, &b, 0);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "scan save failed: %d", err);
}

/* ------------------------------------------------------ names file on SD */

#include <stdio.h>
#include <sys/stat.h>

#define P25_NAMES_PATH "/sdcard/p25_names.csv"

typedef struct { FILE *fp; } sd_ctx_t;

static int sd_read_line(void *ctx, char *buf, size_t buf_len)
{
    sd_ctx_t *s = (sd_ctx_t *)ctx;
    if (!s->fp) return 0;
    if (!fgets(buf, (int)buf_len, s->fp)) return 0;
    size_t k = strlen(buf);
    while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
    return (int)k;
}

/* Loads /sdcard/p25_names.csv into g_p25_scan.  Called on demand from the
 * console or from the P25 CONFIG tab - not on every boot, because the SD
 * may not be mounted yet and a slow probe there would delay a headless
 * boot for nothing.  Returns true if the file was opened (even if some
 * rows were rejected); false if it could not be opened. */
bool p25_scan_names_reload_from_sd(void)
{
    struct stat st;
    if (stat(P25_NAMES_PATH, &st) != 0) {
        ESP_LOGI(TAG, "no names file at %s", P25_NAMES_PATH);
        return false;
    }
    sd_ctx_t s = { fopen(P25_NAMES_PATH, "r") };
    if (!s.fp) {
        ESP_LOGW(TAG, "open %s failed", P25_NAMES_PATH);
        return false;
    }
    p25_scan_names_load(&g_p25_scan, sd_read_line, &s, (size_t)st.st_size);
    fclose(s.fp);
    if (g_p25_scan.names_refused_oversize) {
        ESP_LOGW(TAG, "names file too large (%ld bytes) - refused",
                 (long)st.st_size);
    } else {
        ESP_LOGI(TAG, "names loaded: %u ok, %u bad (first bad line: %d)",
                 (unsigned)g_p25_scan.names_loaded,
                 (unsigned)g_p25_scan.names_bad_lines,
                 (int)g_p25_scan.names_first_bad_line);
    }
    return true;
}
