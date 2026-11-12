/* NVS wrapper for scan_ctrl. */

#include "scan_ctrl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "ls_nvs_safe.h"

#include <stdio.h>
#include <string.h>

static const char *TAG    = "p25scan";
static const char *NS     = "sdr-tool";
static const char *BLOBK  = "p25scanv1";

/* Sized to hold every possible mutation: header + max lockout + max allow. */
#define P25_SCAN_NVS_MAX (12 + 4 + 4 + \
                          P25_SCAN_LOCKOUT_MAX * 2 + \
                          P25_SCAN_ALLOW_MAX   * 2 + 6)   /* + scope */

/* A roster scoped to a system lives under its own key, "p25r_" + WACN (5
 * hex) + SYSID (3 hex); WACN 00000 when only the SYSID was known. BLOBK
 * keeps the unscoped roster. A system with no key has no roster: every
 * call is followed there. */
typedef struct {
    uint8_t *buf; size_t len; size_t out_len;
    char key[16];
    bool erase;
} scan_blob_t;

static void roster_key(char out[16], uint32_t wacn, uint16_t sysid)
{
    snprintf(out, 16, "p25r_%05lX%03X", (unsigned long)(wacn & 0xfffffu),
             (unsigned)(sysid & 0xfffu));
}

static esp_err_t scan_read(void *ctx)
{
    scan_blob_t *b = (scan_blob_t *)ctx;
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    b->out_len = b->len;
    err = nvs_get_blob(h, b->key[0] ? b->key : BLOBK, b->buf, &b->out_len);
    nvs_close(h);
    return err;
}

static esp_err_t scan_write(void *ctx)
{
    scan_blob_t *b = (scan_blob_t *)ctx;
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    const char *key = b->key[0] ? b->key : BLOBK;
    if (b->erase) {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else {
        err = nvs_set_blob(h, key, b->buf, b->len);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool roster_read(const char *key, p25_scan_ctrl_t *sc)
{
    uint8_t buf[P25_SCAN_NVS_MAX];
    scan_blob_t b = { .buf = buf, .len = sizeof(buf) };
    snprintf(b.key, sizeof(b.key), "%s", key);
    if (ls_nvs_run(scan_read, &b, 0) != ESP_OK) return false;
    return p25_scan_persist_load_roster(sc, buf, b.out_len);
}

static void roster_erase(const char *key)
{
    scan_blob_t b = { .erase = true };
    snprintf(b.key, sizeof(b.key), "%s", key);
    (void)ls_nvs_run(scan_write, &b, 0);
}

/* p25_roster_load_fn: the roster stored for a newly named system. */
static void roster_load(p25_scan_ctrl_t *sc, uint32_t wacn, uint16_t sysid)
{
    char key[16];
    roster_key(key, wacn, sysid);
    if (roster_read(key, sc)) {
        ESP_LOGI(TAG, "roster %s: lockouts=%u allow=%u mode=%d", key,
                 (unsigned)sc->lockout_count, (unsigned)sc->allow_count,
                 (int)sc->list_mode);
        return;
    }
    /* A roster made when only the SYSID was known (a profile with no wacn=,
     * or a site that sent only RFSS_STS_BCST). Now the WACN is known it
     * moves to the full key. */
    char partial[16];
    roster_key(partial, 0, sysid);
    if (wacn && roster_read(partial, sc)) {
        p25_scan_roster_bind(sc, wacn, sysid);
        p25_scan_persist_save_now();
        roster_erase(partial);
        ESP_LOGI(TAG, "roster %s moved to %s", partial, key);
        return;
    }
    p25_scan_roster_clear(sc);
    ESP_LOGI(TAG, "roster %s: none stored, following every call", key);
}

/* p25_roster_save_fn: store under the new scope; the copy under the scope
 * it left (the unscoped slot, or a SYSID-only key) goes with it. */
static void roster_save(p25_scan_ctrl_t *sc, p25_system_id_t moved_from)
{
    p25_scan_persist_save_now();
    char key[16];
    if (moved_from.valid) {
        if (sc->roster_sys.valid && moved_from.wacn == sc->roster_sys.wacn &&
            moved_from.sysid == sc->roster_sys.sysid) return;
        roster_key(key, moved_from.wacn, moved_from.sysid);
    } else {
        snprintf(key, sizeof(key), "%s", BLOBK);
    }
    roster_erase(key);
    ESP_LOGI(TAG, "roster %s moved to its system's key", key);
}

void p25_scan_persist_reload(void)
{
    p25_scan_set_roster_loader(roster_load);
    p25_scan_set_roster_saver(roster_save);
    uint8_t buf[P25_SCAN_NVS_MAX];
    scan_blob_t b = { .buf = buf, .len = sizeof(buf) };
    esp_err_t err = ls_nvs_run(scan_read, &b, 0);
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
     * access moves - see above. */
    scan_blob_t b = { .buf = buf, .len = n };
    /* An unscoped roster saved now was made now: it belongs to the first
     * system that names itself (see roster_pending_bind). */
    if (!g_p25_scan.roster_sys.valid)
        g_p25_scan.roster_pending_bind = !p25_scan_roster_empty(&g_p25_scan);
    if (g_p25_scan.roster_sys.valid) {
        roster_key(b.key, g_p25_scan.roster_sys.wacn, g_p25_scan.roster_sys.sysid);
        /* An emptied roster leaves no key behind, so travelling to new
         * systems does not fill NVS. */
        b.erase = p25_scan_roster_empty(&g_p25_scan);
    }
    esp_err_t err = ls_nvs_run(scan_write, &b, 0);
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
