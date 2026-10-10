#include "ls_wifi.h"
#include "rec_state.h"   /* rec_dir() */
#include "rec_watch.h"
#include "ls_wifi_sta_core.h"
#include "ls_wifi_operation.h"
#include "ls_wifi_file_stream.h"
#include "ls_hub_transfer.h"
#include "ble_link.h"
#include "ls_time.h"
#include "tui/ls_field.h"
#include "ls_board.h"   /* LS_USE_DISPLAY */
#include "esp_attr.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ls_nvs_safe.h"
#include "bsp/esp-bsp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "ls_wifi";
static EXT_RAM_BSS_ATTR ls_wifi_scan_ap_t s_scan_cache[64];
static int s_scan_cache_count;
static int64_t s_scan_cache_us;

#define LS_AP_SSID  "LakeShark"
#define LS_AP_PASS  "sharkbait"          /* WPA2 needs >= 8 chars */
#define LS_AP_CHAN  6
#define LS_AP_MAXC  2

/**/
/* Files come off the SD card. If it is not mounted there is nothing useful to
   serve, and saying so beats an empty listing that looks like a broken
   server. */
#ifndef BSP_SD_MOUNT_POINT
#define BSP_SD_MOUNT_POINT "/sdcard"
#endif
static const char *ROOT = BSP_SD_MOUNT_POINT;

/**/

#define LS_WIFI_NVS_NAMESPACE "ls_wifi"
#define LS_WIFI_NVS_KEY_SSID  "ssid"
#define LS_WIFI_NVS_KEY_PASS  "pass"

static volatile bool  s_ap_running  = false;
static volatile bool  s_sta_running = false;      /* STA netif+config is up */
static volatile bool  s_sta_connected = false;    /* got IP */
static volatile int   s_sta_reason = 0;
static bool           s_stack_up    = false;
static bool           s_events_bound = false;
static bool           s_nvs_flash_ready = false;

static httpd_handle_t s_httpd    = NULL;
/* Defined below; started by either the SoftAP or the station. */
static esp_err_t httpd_ensure_started(void);
static esp_netif_t   *s_netif_ap  = NULL;
static esp_netif_t   *s_netif_sta = NULL;
static char           s_ip_ap[16]  = "192.168.4.1";
static char           s_ip_sta[16] = "0.0.0.0";
static char           s_sta_ssid[LS_WIFI_SSID_MAX_LEN + 1] = "";

static esp_timer_handle_t s_reconnect_timer = NULL;
static uint32_t           s_reconnect_ms    = LS_WIFI_BACKOFF_MIN_MS;
static ls_wifi_operation_t s_operation;

static esp_err_t sta_leave_locked(void);
static esp_err_t sta_autojoin_excluding_locked(const char *excluded);
static void deferred_worker(void *arg);
static void wipe(void *p, size_t n);
static _Atomic bool s_roam_pending, s_commit_pending, s_roam_used, s_cleanup_pending;
static _Atomic bool s_sta_gave_up;
/* Shared only while s_operation is owned. No new large internal buffers. */
static int s_link_saved_count = -1;
static EXT_RAM_BSS_ATTR ls_wifi_saved_t s_saved[LS_WIFI_SAVED_MAX];
static EXT_RAM_BSS_ATTR ls_wifi_visible_t s_visible_saved[LS_WIFI_SAVED_MAX];

static bool sd_present(void)
{
    DIR *d = opendir(ROOT);
    if (!d) return false;
    closedir(d);
    return true;
}

/* ---------------------------------------------------------------- http --- */

static esp_err_t send_chunk(httpd_req_t *r, const char *s)
{
    return httpd_resp_send_chunk(r, s, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_index(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    send_chunk(r,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<style>body{background:#0A0C0D;color:#C8D0D6;font:15px system-ui;margin:0;padding:16px}"
        "h1{color:#84D2CE;font-size:18px}a{color:#C8D0D6}"
        "li{margin:6px 0}form{margin:16px 0;padding:12px;background:#171A1C;border-radius:6px}"
        "input[type=submit]{background:#21262A;color:#C8D0D6;border:1px solid #515A61;"
        "padding:8px 14px;border-radius:4px}</style>"
        "<h1>LakeShark files</h1>"
        "<form method=post enctype=multipart/form-data action=/ul>"
        "<input type=file name=f><input type=submit value=Upload></form><ul>");

    if (!sd_present()) {
        send_chunk(r, "<li><i>no SD card mounted - nothing to serve</i></li>");
    } else {
        DIR *d = opendir(ROOT);
        struct dirent *e;
        char line[320];
        int n = 0;
        while (d && (e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char full[280];
            struct stat st;
            snprintf(full, sizeof(full), "%s/%s", ROOT, e->d_name);
            long sz = (stat(full, &st) == 0) ? (long)st.st_size : 0;
            snprintf(line, sizeof(line),
                     "<li><a href='/dl?f=%s'>%s</a> <small>%ld B</small></li>",
                     e->d_name, e->d_name, sz);
            send_chunk(r, line);
            n++;
        }
        if (d) closedir(d);

        /* and the captures directory, which is where anything the
           device produces itself actually lands. */
        const char *cap = rec_dir();
        if (cap && strcmp(cap, ROOT) != 0) {
            DIR *cd = opendir(cap);
            if (cd) {
                bool any = false;
                while ((e = readdir(cd)) != NULL) {
                    if (e->d_name[0] == '.') continue;
                    if (!any) { send_chunk(r, "</ul><h1>captures</h1><ul>"); any = true; }
                    char cfull[280];
                    struct stat cst;
                    snprintf(cfull, sizeof(cfull), "%s/%s", cap, e->d_name);
                    long csz = (stat(cfull, &cst) == 0) ? (long)cst.st_size : 0;
                    snprintf(line, sizeof(line),
                             "<li><a href='/dl?f=%s'>%s</a> <small>%ld B</small></li>",
                             e->d_name, e->d_name, csz);
                    send_chunk(r, line);
                    n++;
                }
                closedir(cd);
            }
        }
        if (!n) send_chunk(r, "<li><i>card is empty</i></li>");
    }
    send_chunk(r, "</ul>");
    return httpd_resp_send_chunk(r, NULL, 0);
}

/* Reject anything with a path separator: this server is rooted at the card and
   must not be talked out of it. */
static bool name_ok(const char *n)
{
    if (!n || !*n) return false;
    if (strstr(n, "..")) return false;
    if (strchr(n, '/') || strchr(n, '\\')) return false;
    return true;
}

static int dl_send(void *context, const char *data, size_t size)
{
    return httpd_resp_send_chunk((httpd_req_t *)context, data, size);
}

static esp_err_t h_dl(httpd_req_t *r)
{
    char q[160], name[128];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "f", name, sizeof(name)) != ESP_OK ||
        !name_ok(name)) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_FAIL;
    }
    /* Captures and screenshots live in rec_dir(), a subdirectory of the
       SD root, and this only ever looked at the root. So `shot` printed "fetch
       it with 'wifi on'" and the file it had just written was not on the page.
       Try both. */
    char full[280];
    snprintf(full, sizeof(full), "%s/%s", ROOT, name);
    FILE *f = fopen(full, "rb");
    if (!f) {
        snprintf(full, sizeof(full), "%s/%s", rec_dir(), name);
        f = fopen(full, "rb");
    }
    if (!f) { httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "no such file"); return ESP_FAIL; }

    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        fclose(f);
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot size file");
        return ESP_FAIL;
    }

    httpd_resp_set_type(r, "application/octet-stream");

    /* 2 KB chunks meant 562 separate chunked writes for a 1.15 MB
       screenshot, and the per-chunk cost dominates over the co-processor's
       Wi-Fi link: a 34 KB file moved at 53 KB/s while a screenshot managed
       108 KB in 60 s and stalled. Pull bigger blocks. The buffer is only
       touched by this task - no DMA, no ISR - so it belongs in PSRAM rather
       than the scarce internal heap; the small static is the fallback when
       PSRAM cannot serve it. */
    enum { DL_CHUNK = 16 * 1024 };
    static char small[2048];
    char  *buf = heap_caps_malloc(DL_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t cap = buf ? (size_t)DL_CHUNK : sizeof(small);
    if (!buf) buf = small;

    uint64_t sent = 0;
    int64_t started = esp_timer_get_time();
    errno = 0;
    ls_wifi_file_result_t result = ls_wifi_file_stream(
        f, buf, cap, (uint64_t)st.st_size, dl_send, r, &sent);
    int transfer_errno = errno;
    if (buf != small) heap_caps_free(buf);
    fclose(f);
    if (result != LS_WIFI_FILE_OK) {
        ESP_LOGE(TAG, "download %s: result=%d sent=%llu/%llu errno=%d elapsed_ms=%lld",
                 name, (int)result, (unsigned long long)sent,
                 (unsigned long long)st.st_size, transfer_errno,
                 (long long)((esp_timer_get_time() - started) / 1000));
        /* Returning failure closes the incomplete chunked response. Do not
         * append an HTTP error or retry a chunk that may be partly on wire. */
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "download %s: complete bytes=%llu elapsed_ms=%lld",
             name, (unsigned long long)sent,
             (long long)((esp_timer_get_time() - started) / 1000));
    return ESP_OK;
}

/**/
/* Minimal multipart parse: find the filename, skip to the blank line after the
   part headers, then stream the body to disk until the trailing boundary. This
   is deliberately not a general multipart parser - it handles one file per
   POST, which is what the form sends. */
static esp_err_t h_ul(httpd_req_t *r)
{
    if (!sd_present()) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD card");
        return ESP_FAIL;
    }

    static char buf[2048];
    int  remaining = r->content_len;
    FILE *f = NULL;
    bool  in_body = false;
    char  boundary[80] = {0};

    while (remaining > 0) {
        int want = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int got  = httpd_req_recv(r, buf, want);
        if (got <= 0) { if (f) fclose(f); return ESP_FAIL; }
        remaining -= got;

        if (!in_body) {
            /* First chunk carries the part headers. */
            char *fn = strstr(buf, "filename=\"");
            char *hdr_end = strstr(buf, "\r\n\r\n");
            if (fn && hdr_end) {
                fn += 10;
                char *q = strchr(fn, '"');
                if (q) {
                    char name[128];
                    int len = (int)(q - fn);
                    if (len > 0 && len < (int)sizeof(name)) {
                        memcpy(name, fn, len); name[len] = 0;
                        if (name_ok(name)) {
                            char full[280];
                            snprintf(full, sizeof(full), "%s/%s", ROOT, name);
                            f = fopen(full, "wb");
                            ESP_LOGW(TAG, "upload -> %s", full);
                        }
                    }
                }
                /* First boundary line is everything up to the first CRLF. */
                char *b_end = strstr(buf, "\r\n");
                if (b_end && (b_end - buf) < (int)sizeof(boundary)) {
                    int bl = (int)(b_end - buf);
                    memcpy(boundary, buf, bl); boundary[bl] = 0;
                }
                int off = (int)(hdr_end - buf) + 4;
                in_body = true;
                if (f && got > off) fwrite(buf + off, 1, got - off, f);
                continue;
            }
            if (!f && remaining <= 0) break;
            continue;
        }
        if (f) fwrite(buf, 1, got, f);
    }

    if (f) {
        /* Trim the trailing CRLF + closing boundary the form appends. */
        long end = ftell(f);
        long trim = (long)strlen(boundary) + 6;
        if (end > trim) {
            fflush(f);
            int fd = fileno(f);
            if (fd >= 0) { fflush(f); ftruncate(fd, end - trim); }
        }
        fclose(f);
    }
    httpd_resp_set_status(r, "303 See Other");
    httpd_resp_set_hdr(r, "Location", "/");
    return httpd_resp_send(r, NULL, 0);
}

/* ---------------------------------------------------------------- nvs ---- */

/**/
/* Credentials use their own namespace; forgetting never touches radio settings. */
static esp_err_t nvs_open_creds(nvs_open_mode_t mode, nvs_handle_t *out)
{
    esp_err_t error = ls_wifi_nvs_prepare(&s_nvs_flash_ready, nvs_flash_init);
    if (error != ESP_OK) return error;
    return nvs_open(LS_WIFI_NVS_NAMESPACE, mode, out);
}

#include "ls_wifi_store.inc"

static esp_err_t store_run(int op, const char *ssid, const char *pass, char *password)
{
    /* s_operation serializes this cache. Remember failures too: a broken NVS
     * must not trigger initialization/migration on every one-second UI poll.
     * An explicit save/forget/password request still attempts storage again. */
    static bool loaded;
    static esp_err_t load_error;
    if (op == 0 && loaded) return load_error;
    store_job_t job = {op, ssid, pass, password};
    esp_err_t e = ls_nvs_run(store_job, &job, 0);
    s_link_saved_count = -1;
    if(e==ESP_OK || e==ESP_ERR_NOT_FOUND) {
        s_link_saved_count=0;
        for(int i=0;i<LS_WIFI_SAVED_MAX;i++) s_link_saved_count+=s_saved[i].ssid[0]!=0;
    }
    loaded = true;
    load_error = e == ESP_ERR_NOT_FOUND ? ESP_OK : e;
    return e;
}

static int saved_list_locked(char names[][33], int cap)
{
    if (!names || cap <= 0 || store_run(0, NULL, NULL, NULL) != ESP_OK) return -1;
    bool used[LS_WIFI_SAVED_MAX] = {0};
    int count = 0;
    while (count < cap) {
        int best = -1;
        for (int i = 0; i < LS_WIFI_SAVED_MAX; ++i)
            if (!used[i] && s_saved[i].ssid[0] &&
                (best < 0 || s_saved[i].sequence > s_saved[best].sequence)) best = i;
        if (best < 0) break;
        memcpy(names[count++], s_saved[best].ssid, 33); used[best] = true;
    }
    return count;
}

/* ------------------------------------------------------------- events ---- */

static void reconnect_cb(void *arg)
{
    (void)arg;
    if (!ls_wifi_operation_try(&s_operation)) {
        /* The timer already exists; retry without blocking its task or
         * racing a scan/join. A concurrent scheduled retry is sufficient. */
        esp_timer_start_once(s_reconnect_timer, 1000000ULL);
        return;
    }
    if (s_commit_pending || s_roam_pending || s_cleanup_pending) {
        if (xTaskCreateWithCaps(deferred_worker, "wifi_deferred", 6144, NULL, 4,
                NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            ls_wifi_operation_end(&s_operation);
            esp_timer_start_once(s_reconnect_timer, 1000000ULL);
        }
        return; /* worker owns s_operation, including during scan/NVS */
    }
    if (!s_sta_running || s_sta_connected || s_sta_gave_up) {
        ls_wifi_operation_end(&s_operation); return;
    }
    ESP_LOGI(TAG, "wifi: reconnecting to stored network");
    esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK && e != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(e));
    }
    ls_wifi_operation_end(&s_operation);
}

/* Consecutive failed attempts since the last success or explicit join. */
static int s_sta_attempts;
/* A join's credentials, saved once it has an address: a mistyped password
   or a tap on the wrong network must not replace the network that works. */
static char s_pending_ssid[LS_WIFI_SSID_MAX_LEN + 1];
static char s_pending_pass[LS_WIFI_PASS_MAX_LEN + 1];
static _Atomic bool s_pending_save;

/* Volatile stores, so the compiler cannot drop a wipe of a buffer that is
   not read again. */
static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

static void pending_drop(void)
{
    s_pending_save = false;
    wipe(s_pending_pass, sizeof(s_pending_pass));
}

/* Caller owns s_operation. Drain a recorded GOT_IP before a subsequent
 * console/UI operation can replace or wipe that successful join's credentials.
 * A quick disconnect after GOT_IP still counts as a successful join. */
static void pending_commit_locked(void)
{
    if (!__atomic_exchange_n(&s_commit_pending, false, __ATOMIC_ACQ_REL) || !s_pending_save) return;
    esp_err_t e = store_run(1, s_pending_ssid, s_pending_pass, NULL);
    if (e != ESP_OK) ESP_LOGW(TAG, "credential save failed: %s", esp_err_to_name(e));
    pending_drop();
}

static void schedule_reconnect(void)
{
    if (!s_sta_running) return;
    if (!ls_wifi_should_retry(s_sta_attempts, s_sta_reason)) {
        const char *why = ls_wifi_reason_text(s_sta_reason);
        ESP_LOGW(TAG, "wifi: giving up after %d attempts (%s); "
                      "join again to retry",
                 s_sta_attempts, why ? why : "unknown reason");
        if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
        s_sta_gave_up = true;
        s_cleanup_pending = true;
        if (s_reconnect_timer) {
            if (!s_roam_used) s_roam_pending = true;
            esp_timer_start_once(s_reconnect_timer, 1000ULL);
        }
        return;
    }
    s_sta_attempts++;
    if (!s_reconnect_timer) {
        const esp_timer_create_args_t args = {
            .callback = &reconnect_cb,
            .name     = "ls_wifi_reconn",
        };
        if (esp_timer_create(&args, &s_reconnect_timer) != ESP_OK) {
            ESP_LOGE(TAG, "esp_timer_create for reconnect failed");
            return;
        }
    }
    esp_timer_stop(s_reconnect_timer);
    /* Backoff formula lives in ls_wifi_sta_core.c so the bench can pin
       it. See test_wifi_sta.c. */
    s_reconnect_ms = ls_wifi_backoff_next(s_reconnect_ms,
                                          LS_WIFI_BACKOFF_MIN_MS,
                                          LS_WIFI_BACKOFF_MAX_MS);
    ESP_LOGI(TAG, "wifi: retry in %u ms", (unsigned)s_reconnect_ms);
    esp_timer_start_once(s_reconnect_timer, (uint64_t)s_reconnect_ms * 1000ULL);
}

static void wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            /* Do not connect from here. ls_wifi_sta_join() owns the
               initial connect once it has set the config; connecting on
               STA_START would race a caller that has not yet written a valid
               SSID. */
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
            /* Log the reason code, not the SSID or password. */
            ESP_LOGW(TAG, "wifi: disconnected (reason=%d)", ev ? ev->reason : -1);
            s_sta_connected = false;
            s_sta_reason = ev ? ev->reason : -1;
            strncpy(s_ip_sta, "0.0.0.0", sizeof(s_ip_sta));
            /* Give the server's memory back when no interface is left
               to serve; internal RAM is the scarce resource on this board. */
            if (s_httpd && !s_ap_running) {
                if (httpd_stop(s_httpd) == ESP_OK) s_httpd = NULL;
            }
            schedule_reconnect();
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (!s_sta_running) return; /* late IP after an explicit leave */
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip_sta, sizeof(s_ip_sta), IPSTR, IP2STR(&ev->ip_info.ip));
        s_sta_connected = true;
        s_sta_reason = 0;
        s_reconnect_ms  = ls_wifi_backoff_reset(LS_WIFI_BACKOFF_MIN_MS);
        /* Connected, so the attempt budget starts over. */
        s_sta_attempts = 0;
        s_sta_gave_up = false;
        if (s_pending_save) {
            s_commit_pending = true;
            /* The timer only starts a worker; it never writes NVS. */
            if (s_reconnect_timer) {
                esp_timer_stop(s_reconnect_timer);
                esp_timer_start_once(s_reconnect_timer, 1000ULL);
            }
        }
        /* Serve the file browser over the station too, so captures can
           be pulled without dropping the BLE head to raise the SoftAP.
           Headless boards only: on a board with a screen the page is not
           wanted, and its 4 KB task stack was the internal RAM the serial
           console needed once the station had joined. */
#if LS_USE_DISPLAY
        ESP_LOGW(TAG, "wifi: joined \"%s\" - %s", s_sta_ssid, s_ip_sta);
#else
        if (httpd_ensure_started() == ESP_OK)
            ESP_LOGW(TAG, "wifi: joined \"%s\" - http://%s/", s_sta_ssid, s_ip_sta);
        else
            ESP_LOGW(TAG, "wifi: joined \"%s\" - %s (file server unavailable)",
                     s_sta_ssid, s_ip_sta);
#endif

        ls_time_sntp_start();
    }
}

/* --------------------------------------------------------- wifi stack ---- */

static esp_err_t ensure_stack(void)
{
#if defined(CONFIG_LS_C6_LINK) && !CONFIG_LS_C6_LINK
    return ESP_ERR_NOT_SUPPORTED;
#elif !defined(CONFIG_LS_C6_LINK)
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (s_stack_up) return ESP_OK;

    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&cfg);
    if (e != ESP_OK && e != ESP_ERR_WIFI_INIT_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(e));
        return e;
    }

    if (!s_events_bound) {
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            &wifi_event_cb, NULL, NULL);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            &wifi_event_cb, NULL, NULL);
        s_events_bound = true;
    }
    s_stack_up = true;
    return ESP_OK;
}

static wifi_mode_t compose_mode(void)
{
    if (s_ap_running && s_sta_running) return WIFI_MODE_APSTA;
    if (s_ap_running)                  return WIFI_MODE_AP;
    if (s_sta_running)                 return WIFI_MODE_STA;
    return WIFI_MODE_NULL;
}

/* --------------------------------------------------------- SoftAP path ---- */

static esp_err_t ap_start_locked(void)
{
#if LS_USE_DISPLAY
    /* The SoftAP exists to serve the file page, which is for headless
       boards only - see httpd_ensure_started(). */
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (s_ap_running) return ESP_OK;

    /**/
    /* Take the C6 off BLE first. Both go through the same SDIO link and
       documents a boot-loop when that link is unhappy; one user at a
       time is the cheap way to stay out of it. The AP path serves HTTP, which
       is heavy enough that keeping BLE running alongside it has never been
       tested and is not being introduced here. */
    ESP_LOGW(TAG, "stopping the BLE head - WiFi and BLE share the C6");
    ble_link_stop();

    esp_err_t e = ensure_stack();
    if (e != ESP_OK) return e;

    if (!s_netif_ap) s_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, LS_AP_SSID, sizeof(ap.ap.ssid) - 1);
    strncpy((char *)ap.ap.password, LS_AP_PASS, sizeof(ap.ap.password) - 1);
    ap.ap.ssid_len       = strlen(LS_AP_SSID);
    ap.ap.channel        = LS_AP_CHAN;
    ap.ap.max_connection = LS_AP_MAXC;
    ap.ap.authmode       = WIFI_AUTH_WPA2_PSK;

    s_ap_running = true;
    if ((e = esp_wifi_set_mode(compose_mode())) != ESP_OK) { s_ap_running = false; return e; }
    e = esp_wifi_set_config(WIFI_IF_AP, &ap);
    wipe(&ap, sizeof(ap));
    if (e != ESP_OK) { s_ap_running = false; return e; }
    if ((e = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(e));
        s_ap_running = false;
        return e;
    }

    esp_netif_ip_info_t ip;
    if (s_netif_ap && esp_netif_get_ip_info(s_netif_ap, &ip) == ESP_OK)
        snprintf(s_ip_ap, sizeof(s_ip_ap), IPSTR, IP2STR(&ip.ip));

    if ((e = httpd_ensure_started()) != ESP_OK) {
        esp_wifi_stop();
        s_ap_running = false;
        return e;
    }

    ESP_LOGW(TAG, "AP \"%s\" - http://%s/  (BLE is off)",
             LS_AP_SSID, s_ip_ap);
    return ESP_OK;
}

/* Read-only diagnostics work with the keyboard attached and do not retune a radio. */
static esp_err_t h_field_debug(httpd_req_t *req)
{
    EXT_RAM_BSS_ATTR static ls_field_state_t field;
    ls_field_snapshot(&field);
    char reply[512];
    int n = snprintf(reply, sizeof(reply),
        "{\"ready\":%s,\"direct\":%s,\"busy\":%s,\"mode\":%d,"
        "\"hz\":%lu,\"rx\":%lu,\"bad\":%lu,\"tx\":%lu,"
        "\"sequence\":%lu,\"queue_drops\":%lu,\"journal_entries\":%d,"
        "\"recording\":%s,\"gps_valid\":%s,\"imu_valid\":%s}",
        field.ready ? "true" : "false", field.direct ? "true" : "false", field.busy ? "true" : "false",
        field.mode, (unsigned long)field.config.freq_hz, (unsigned long)field.rx,
        (unsigned long)field.bad, (unsigned long)field.tx, (unsigned long)field.sequence,
        (unsigned long)field.drops, field.journal_count, field.recording ? "true" : "false",
        field.sample.gps_valid ? "true" : "false", field.sample.imu_valid ? "true" : "false");
    if (n < 0 || n >= (int)sizeof(reply)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, reply, n);
}

static esp_err_t h_watch_debug(httpd_req_t *req)
{
    EXT_RAM_BSS_ATTR static rec_watch_status_t watch;
    rec_watch_snapshot(&watch);
    rec_hub_status_t rx;rec_get_hub_status(&rx);
    char reply[384];
    int n=snprintf(reply,sizeof(reply),
        "{\"ready\":%s,\"enabled\":%s,\"streaming\":%s,\"patterns\":%d,"
        "\"captures\":%lu,\"skipped\":%lu,\"alerts_queued\":%lu,"
        "\"alerts_refused\":%lu,\"alerts_limited\":%lu,\"archive_max_bytes\":532480}",
        watch.ready?"true":"false",watch.enabled?"true":"false",rx.receiver_streaming?"true":"false",
        watch.count,(unsigned long)watch.received,(unsigned long)watch.dropped,
        (unsigned long)watch.alert_sent,(unsigned long)watch.alert_failed,(unsigned long)watch.alert_suppressed);
    if(n<0 || n>=(int)sizeof(reply))return ESP_FAIL;
    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return httpd_resp_send(req,reply,n);
}

static esp_err_t h_hub_info(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "{\"protocol\":\"lakeshark-hub-v1\",\"map_upload\":true}");
}

static esp_err_t h_hub_map(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Connection", "close");
    char query[128], name[65], expected_text[16];
    if (req->content_len < 127 || req->content_len > 1024 * 1024 * 1024 ||
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
        !ls_hub_map_name(name) ||
        httpd_req_get_hdr_value_str(req, "X-LakeShark-CRC32", expected_text, sizeof(expected_text)) != ESP_OK ||
        strlen(expected_text) != 8) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid map name, size or checksum");
    }
    char *end;
    for (int i = 0; i < 8; ++i) {
        char c = expected_text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid checksum");
    }
    uint32_t expected = (uint32_t)strtoul(expected_text, &end, 16);
    if (*end || !sd_present())
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No SD card or invalid checksum");
    char folder[160], final[240], partial[250];
    snprintf(folder, sizeof(folder), "%s/maps", ROOT);
    if (mkdir(folder, 0775) && errno != EEXIST)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create maps directory");
    snprintf(final, sizeof(final), "%s/%s", folder, name);
    snprintf(partial, sizeof(partial), "%s.partial", final);
    struct stat existing;
    if (stat(final, &existing) == 0) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Map already exists; choose a new name");
    }
    uint8_t *buffer = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No transfer buffer");
    FILE *file = fopen(partial, "wb");
    bool ok = file != NULL;
    uint32_t crc = 0;
    size_t received = 0;
    uint8_t header[127] = {0};
    while (ok && received < req->content_len) {
        size_t want = req->content_len - received;
        if (want > 4096) want = 4096;
        int got = httpd_req_recv(req, (char *)buffer, want);
        if (got <= 0) { ok = false; break; }
        if (received < sizeof(header)) {
            size_t n = sizeof(header) - received;
            if (n > (size_t)got) n = (size_t)got;
            memcpy(header + received, buffer, n);
        }
        crc = ls_hub_crc32(crc, buffer, (size_t)got);
        ok = fwrite(buffer, 1, (size_t)got, file) == (size_t)got;
        received += (size_t)got;
    }
    if (file && fclose(file) != 0) ok = false;
    free(buffer);
    ok = ok && received == req->content_len && crc == expected && ls_hub_map_header(header, received);
    if (!ok || rename(partial, final) != 0) {
        unlink(partial);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Map transfer failed; existing maps kept");
    }
    char reply[120];
    snprintf(reply, sizeof(reply), "{\"bytes\":%lu,\"crc32\":\"%08lx\",\"saved\":true}",
             (unsigned long)received, (unsigned long)crc);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, reply);
}

static esp_err_t httpd_ensure_started(void)
{
#if LS_USE_DISPLAY
    /* The file page is for headless boards only. Refused here, where every
       caller passes, because gating two of the three callers left the
       server running at join time. */
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (s_httpd) return ESP_OK;

    httpd_config_t hc   = HTTPD_DEFAULT_CONFIG();
    /* Chunked responses enter newlib's formatter before the handler body.
       A 2560-byte stack reached within 160 bytes of its base while formatting
       the response header and corrupted the task control block.  Audio's
       queue/player stack now live in PSRAM, so reserve a real safety margin
       here while this task is still created before USB fragments the heap. */
    hc.stack_size       = 4096;
    hc.max_uri_handlers = 8;
    hc.lru_purge_enable = true;

    esp_err_t e = httpd_start(&s_httpd, &hc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(e));
        s_httpd = NULL;
        return e;
    }
    static const httpd_uri_t u_index = { .uri = "/",    .method = HTTP_GET,  .handler = h_index };
    static const httpd_uri_t u_dl    = { .uri = "/dl",  .method = HTTP_GET,  .handler = h_dl };
    static const httpd_uri_t u_ul    = { .uri = "/ul",  .method = HTTP_POST, .handler = h_ul };
    httpd_register_uri_handler(s_httpd, &u_index);
    httpd_register_uri_handler(s_httpd, &u_dl);
    httpd_register_uri_handler(s_httpd, &u_ul);
    static const httpd_uri_t debug = {.uri="/debug/field", .method=HTTP_GET, .handler=h_field_debug};
    httpd_register_uri_handler(s_httpd, &debug);
    static const httpd_uri_t watch={.uri="/debug/rec",.method=HTTP_GET,.handler=h_watch_debug};
    httpd_register_uri_handler(s_httpd,&watch);
    static const httpd_uri_t hub_info = {.uri="/hub/info", .method=HTTP_GET, .handler=h_hub_info};
    static const httpd_uri_t hub_map = {.uri="/hub/map", .method=HTTP_POST, .handler=h_hub_map};
    httpd_register_uri_handler(s_httpd, &hub_info);
    httpd_register_uri_handler(s_httpd, &hub_map);
    return ESP_OK;
}

static esp_err_t ap_stop_locked(void)
{
    if (!s_ap_running) return ESP_OK;
    esp_err_t error;
    /* Only tear the server down if nothing else is serving it. The
       station keeps it alive, so stopping the SoftAP (to bring BLE back) no
       longer takes the file browser with it. */
    if (s_httpd && !s_sta_connected) {
        error = httpd_stop(s_httpd);
        if (error != ESP_OK) return error;
        s_httpd = NULL;
    }

    /* Only stop the radio if STA is not still using it. */
    if (!s_sta_running) {
        error = esp_wifi_stop();
    } else {
        error = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (error != ESP_OK) return error;
    s_ap_running = false;
    ESP_LOGW(TAG, "WiFi AP off - restarting the BLE head");
    return ble_link_start();
}

bool ls_wifi_running(void) { return s_ap_running; }

/* ------------------------------------------------------------- STA API ---- */

/* Everything below is new. AP path above must remain byte-compatible
   with it still stops BLE and it still serves the same three URIs. */

static bool sta_apply_config(const char *ssid, const char *pass)
{
    wifi_config_t sta = {0};
    /* IDF's SSID field is 32 bytes, not a 31-character C string. */
    ls_wifi_copy_ssid(sta.sta.ssid, ssid);
    if (pass && *pass) {
        strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password) - 1);
        sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    sta.sta.pmf_cfg.capable  = true;
    sta.sta.pmf_cfg.required = false;

    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &sta);
    wipe(&sta, sizeof(sta));
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(STA): %s", esp_err_to_name(e));
        return false;
    }
    return true;
}

static esp_err_t sta_bring_up(void)
{
    esp_err_t e = ensure_stack();
    if (e != ESP_OK) return e;

    if (!s_netif_sta) s_netif_sta = esp_netif_create_default_wifi_sta();

    /* Was ANY interface already active? If yes, esp_wifi is started and
       set_mode() alone brings STA in. Otherwise we own the start. */
    bool wifi_was_started = s_ap_running || s_sta_running;
    bool prev_sta         = s_sta_running;
    s_sta_running = true;

    if ((e = esp_wifi_set_mode(compose_mode())) != ESP_OK) {
        s_sta_running = prev_sta;
        ESP_LOGE(TAG, "esp_wifi_set_mode: %s", esp_err_to_name(e));
        return e;
    }
    if (!wifi_was_started) {
        if ((e = esp_wifi_start()) != ESP_OK) {
            s_sta_running = prev_sta;
            ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(e));
            return e;
        }
    }
    return ESP_OK;
}

static esp_err_t sta_join_locked(const char *ssid, const char *pass)
{
    if (!ls_wifi_ssid_valid(ssid)) return ESP_ERR_INVALID_ARG;
    if (!ls_wifi_pass_valid(pass ? pass : "")) return ESP_ERR_INVALID_ARG;

    /* changing networks while associated otherwise fails set_config
     * or leaves the previous IP on screen. Cancel the old association first. */
    if (s_sta_running) {
        esp_err_t leave = sta_leave_locked();
        if (leave != ESP_OK) return leave;
    }
    if (!s_reconnect_timer) {
        const esp_timer_create_args_t args = {.callback = reconnect_cb, .name = "ls_wifi_reconn"};
        esp_err_t timer = esp_timer_create(&args, &s_reconnect_timer);
        if (timer != ESP_OK) return timer;
    }
    s_sta_reason = 0;
    /* Here rather than only in ls_wifi_sta_join: the settings screen and the
       Flipper head rejoin through autojoin, which reaches this and not that,
       so without this a rejoin after the budget ran out gives up again on
       its first disconnect. */
    s_sta_attempts = 0;
    s_sta_gave_up = false;

    strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
    s_sta_ssid[sizeof(s_sta_ssid) - 1] = '\0';

    esp_err_t e = sta_bring_up();
    if (e != ESP_OK) return e;

    /* Set config after mode is set but esp_wifi_connect() must not race with
       an unwritten config, so this must land before we call it below. */
    if (!sta_apply_config(ssid, pass ? pass : "")) {
        sta_leave_locked();
        return ESP_FAIL;
    }

    strncpy(s_pending_ssid, ssid, sizeof(s_pending_ssid) - 1);
    s_pending_ssid[sizeof(s_pending_ssid) - 1] = '\0';
    strncpy(s_pending_pass, pass ? pass : "", sizeof(s_pending_pass) - 1);
    s_pending_pass[sizeof(s_pending_pass) - 1] = '\0';
    s_pending_save = true;

    s_reconnect_ms = ls_wifi_backoff_reset(LS_WIFI_BACKOFF_MIN_MS);

    /* Starting the server after GOT_IP raced USB enumeration, when the
       internal heap was already fragmented below one task-stack-sized block.
       The listener is valid before an address is assigned, so reserve its
       measured-small worker while startup still has contiguous RAM.  A web
       failure must not prevent the station itself from connecting. */
    esp_err_t web = httpd_ensure_started();
    if (web != ESP_OK && web != ESP_ERR_NOT_SUPPORTED)
        ESP_LOGW(TAG, "wifi: file server reservation failed: %s",
                 esp_err_to_name(web));

    ESP_LOGW(TAG, "wifi: joining stored network");
    e = esp_wifi_connect();
    if (e != ESP_OK) sta_leave_locked();
    return e;
}

static esp_err_t sta_leave_locked(void)
{
    bool was_running = s_sta_running;
    /* Cancel intent before draining a successful join: a late event must
     * neither schedule another retry nor publish credentials for a leave. */
    s_sta_running = false;
    pending_commit_locked();
    s_roam_pending = false;
    s_commit_pending = false;
    s_cleanup_pending = false;
    pending_drop();
    if (!was_running) return ESP_OK;
    if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
    esp_err_t e = esp_wifi_disconnect();
    if (e != ESP_OK && e != ESP_ERR_WIFI_NOT_CONNECT) {
        s_sta_running = true;
        return e;
    }
    s_sta_connected = false;
    strncpy(s_ip_sta, "0.0.0.0", sizeof(s_ip_sta));

    /* Stop SNTP too. The synced flag stays set - a lost network
       does not un-know what the time was. */
    ls_time_sntp_stop();

    if (!s_ap_running) {
        e = esp_wifi_stop();
    } else {
        e = esp_wifi_set_mode(compose_mode());
    }
    if (e != ESP_OK) { s_sta_running = true; return e; }
    ESP_LOGW(TAG, "wifi: station mode off");
    return ESP_OK;
}

static esp_err_t sta_forget_locked(const char *ssid)
{
    if (ssid && !ls_wifi_ssid_valid(ssid)) return ESP_ERR_INVALID_ARG;
    if (!ssid || !strcmp(ssid, s_sta_ssid)) {
        esp_err_t e = sta_leave_locked();
        if (e != ESP_OK) return e;
    }
    esp_err_t e = store_run(2, ssid, NULL, NULL);
    if (e == ESP_OK && (!ssid || !strcmp(ssid, s_sta_ssid))) s_sta_ssid[0] = 0;
    return e;
}

static int sta_scan_locked(ls_wifi_scan_ap_t *out, int cap, bool passive);
static esp_err_t sta_autojoin_excluding_locked(const char *excluded)
{
    esp_err_t rc = store_run(0, NULL, NULL, NULL);
    if (rc != ESP_OK) return rc;
    int count = 0;
    for (int i = 0; i < LS_WIFI_SAVED_MAX; ++i) if (s_saved[i].ssid[0]) count++;
    if (!count) return ESP_ERR_NOT_FOUND;
    int choice = ls_wifi_saved_candidate(s_saved, LS_WIFI_SAVED_MAX, NULL, 0, excluded);
    if (choice < 0) return ESP_ERR_NOT_FOUND;
    if (count > 1) {
        /* Scan also reduces ALL results to the strongest BSSID per saved
         * SSID, even when that SSID is beyond the UI's first 16 rows. */
        ls_wifi_scan_ap_t *aps = heap_caps_malloc(16 * sizeof(*aps),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!aps) return ESP_ERR_NO_MEM;
        int n = sta_scan_locked(aps, 16, false);
        choice = ls_wifi_saved_candidate(s_saved, LS_WIFI_SAVED_MAX,
            s_visible_saved, LS_WIFI_SAVED_MAX, excluded);
        free(aps);
        if (n < 0) return ESP_FAIL;
    }
    char ssid[33], pass[64] = {0};
    memcpy(ssid, s_saved[choice].ssid, sizeof(ssid));
    rc = store_run(3, ssid, NULL, pass);
    if (rc == ESP_OK) rc = sta_join_locked(ssid, pass);
    wipe(pass, sizeof(pass));
    return rc;
}

static esp_err_t sta_autojoin_locked(void)
{
    return sta_autojoin_excluding_locked(NULL);
}

static void deferred_worker(void *arg)
{
    (void)arg;
    pending_commit_locked();
    if (s_cleanup_pending) {
        s_cleanup_pending = false;
        if (s_sta_gave_up) pending_drop();
    }
    if (s_roam_pending) {
        s_roam_pending = false;
        if (s_sta_running && !s_sta_connected && s_sta_gave_up && !s_roam_used) {
            s_roam_used = true; /* remains armed only by explicit join/connect */
            char failed[33]; memcpy(failed, s_sta_ssid, sizeof(failed));
            pending_drop();
            esp_err_t e = sta_autojoin_excluding_locked(failed);
            if (e != ESP_OK && e != ESP_ERR_NOT_FOUND)
                ESP_LOGW(TAG, "wifi roam: %s", esp_err_to_name(e));
        }
    }
    ls_wifi_operation_end(&s_operation);
    vTaskDeleteWithCaps(NULL);
}

static int sta_scan_locked(ls_wifi_scan_ap_t *out, int cap, bool passive)
{
    if (!out || cap <= 0) return -1;
    if (passive && (s_sta_running || s_ap_running)) return -1;
    if (ensure_stack() != ESP_OK) return -1;

    /* Scan requires WiFi to be started; enable STA transiently if idle. Do
       not touch the running-flag - this is a one-shot. */
    bool started_for_scan = false;
    if (!s_ap_running && !s_sta_running) {
        if (!s_netif_sta) s_netif_sta = esp_netif_create_default_wifi_sta();
        if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return -1;
        if (esp_wifi_start() != ESP_OK) return -1;
        started_for_scan = true;
    } else if (!s_sta_running) {
        /* AP is up; put us into APSTA transiently. */
        if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) return -1;
    }

    wifi_scan_config_t sc = {0};
    if (passive) { sc.scan_type = WIFI_SCAN_TYPE_PASSIVE; sc.show_hidden = true; }
    esp_err_t e = esp_wifi_scan_start(&sc, /* block=*/true);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(e));
        if (started_for_scan) esp_wifi_stop();
        else if (!s_sta_running && s_ap_running) esp_wifi_set_mode(WIFI_MODE_AP);
        return -1;
    }

    uint16_t n = 0;
    e = esp_wifi_scan_get_ap_num(&n);
    s_scan_cache_count = 0;
    s_scan_cache_us = esp_timer_get_time();
    memset(s_visible_saved, 0, sizeof(s_visible_saved));
    if (e != ESP_OK || !n) {
        esp_wifi_clear_ap_list();
        if (started_for_scan) esp_wifi_stop();
        else if (!s_sta_running && s_ap_running) esp_wifi_set_mode(WIFI_MODE_AP);
        return e == ESP_OK ? 0 : -1;
    }
    /* the console scan overflowed its 4 KiB stack in esp_log's
     * formatter with the AP record array live. Keep scan records off stacks. */
    wifi_ap_record_t *recs = heap_caps_malloc(
        sizeof(*recs) * n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!recs) {
        esp_wifi_clear_ap_list();
        if (started_for_scan) esp_wifi_stop();
        else if (!s_sta_running && s_ap_running) esp_wifi_set_mode(WIFI_MODE_AP);
        return -1;
    }
    e = esp_wifi_scan_get_ap_records(&n, recs);
    if (e != ESP_OK) {
        heap_caps_free(recs);
        if (started_for_scan) esp_wifi_stop();
        else if (!s_sta_running && s_ap_running) esp_wifi_set_mode(WIFI_MODE_AP);
        return -1;
    }

    int written = 0;
    for (int i = 0; i < n; i++) {
        if (s_scan_cache_count < 64) {
            ls_wifi_scan_ap_t *a = &s_scan_cache[s_scan_cache_count++];
            memset(a, 0, sizeof(*a));
            memcpy(a->ssid, recs[i].ssid, 32);
            memcpy(a->bssid, recs[i].bssid, 6);
            a->rssi = recs[i].rssi; a->channel = recs[i].primary;
            a->auth = recs[i].authmode; a->secure = a->auth != WIFI_AUTH_OPEN;
        }
        for (int j = 0; j < LS_WIFI_SAVED_MAX; ++j) {
            if (!s_saved[j].ssid[0] || strncmp((const char *)recs[i].ssid, s_saved[j].ssid, 33)) continue;
            if (!s_visible_saved[j].ssid[0] || recs[i].rssi > s_visible_saved[j].rssi) {
                memcpy(s_visible_saved[j].ssid, s_saved[j].ssid, 33);
                s_visible_saved[j].rssi = recs[i].rssi;
            }
        }
        if (written >= cap) continue;
        strncpy(out[written].ssid, (const char *)recs[i].ssid,
                sizeof(out[written].ssid) - 1);
        out[written].ssid[sizeof(out[written].ssid) - 1] = '\0';
        /* Hidden and control-character names cannot be selected by this API;
         * don't let newline SSIDs corrupt a dropdown's option mapping. */
        if (!passive && !ls_wifi_ssid_valid(out[written].ssid)) continue;
        memcpy(out[written].bssid, recs[i].bssid, 6);
        out[written].auth = recs[i].authmode;
        out[written].rssi   = recs[i].rssi;
        out[written].secure = (recs[i].authmode != WIFI_AUTH_OPEN);
        out[written].channel = recs[i].primary;
        written++;
    }

    heap_caps_free(recs);

    if (started_for_scan) esp_wifi_stop();
    else if (!s_sta_running && s_ap_running) esp_wifi_set_mode(WIFI_MODE_AP);
    return written;
}

bool ls_wifi_sta_running(void)   { return s_sta_running; }
bool ls_wifi_sta_connected(void) { return s_sta_connected; }

/**/
void ls_wifi_sta_ip(char *out, int cap)
{
    if (!out || cap <= 0) return;
    if (!s_sta_connected) { out[0] = 0; return; }
    snprintf(out, (size_t)cap, "%s", s_ip_sta);
}

static void sta_status_locked(char *buf, int cap)
{
    if (!buf || cap <= 0) return;
    if (!s_sta_running) {
        esp_err_t error = store_run(0, NULL, NULL, NULL);
        int n = 0, single = 0;
        for (int i = 0; i < LS_WIFI_SAVED_MAX; ++i)
            if (s_saved[i].ssid[0]) { n++; single = i; }
        if (error != ESP_OK)
            snprintf(buf, cap, "Saved networks unavailable: %s", esp_err_to_name(error));
        else if (n == 1) snprintf(buf, cap, "Disconnected; saved \"%s\"", s_saved[single].ssid);
        else snprintf(buf, cap, "Disconnected; %d saved", n);
        return;
    }
    if (s_sta_connected) {
        snprintf(buf, cap, "Connected: %s  IP %s", s_sta_ssid, s_ip_sta);
    } else if (s_sta_gave_up) {
        const char *why = ls_wifi_reason_text(s_sta_reason);
        snprintf(buf, cap, "Stopped trying \"%s\"; %s - join again to retry",
                 s_sta_ssid, why ? why : "unknown reason");
    } else {
        ls_wifi_connecting_status(buf, (size_t)cap, s_sta_ssid, s_sta_reason, s_reconnect_ms);
    }
}

static void status_locked(char *buf, int cap)
{
    if (!buf || cap <= 0) return;
    char sta[96];
    sta_status_locked(sta, sizeof(sta));
    if (s_ap_running) {
        snprintf(buf, cap, "AP \"%s\" http://%s/ sd=%s BLE OFF | %s",
                 LS_AP_SSID, s_ip_ap,
                 sd_present() ? "yes" : "NO CARD", sta);
    } else {
        snprintf(buf, cap, "wifi ap off (BLE head active) | %s", sta);
    }
}

static _Atomic bool s_survey;

typedef enum {
    WIFI_OP_AP_START, WIFI_OP_AP_STOP, WIFI_OP_JOIN, WIFI_OP_LEAVE,
    WIFI_OP_FORGET, WIFI_OP_FORGET_ALL, WIFI_OP_SAVE, WIFI_OP_LIST, WIFI_OP_AUTOJOIN, WIFI_OP_SCAN, WIFI_OP_STA_STATUS,
    WIFI_OP_STATUS, WIFI_OP_STA_INFO, WIFI_OP_SURVEY, WIFI_OP_SURVEY_BEGIN,
} wifi_operation_kind_t;

typedef struct {
    wifi_operation_kind_t kind;
    const char *ssid;
    const char *password;
    void *out;
    int capacity;
} wifi_operation_request_t;

static int dispatch_operation(void *context)
{
    const wifi_operation_request_t *r = context;
    /* Read-only updates never perform a deferred credential write. GOT_IP's
     * existing deferred worker owns it; mutating commands drain it before
     * replacing the successful join's credentials. */
    if (r->kind == WIFI_OP_AP_START || r->kind == WIFI_OP_AP_STOP ||
        r->kind == WIFI_OP_JOIN || r->kind == WIFI_OP_LEAVE ||
        r->kind == WIFI_OP_FORGET || r->kind == WIFI_OP_FORGET_ALL ||
        r->kind == WIFI_OP_SAVE || r->kind == WIFI_OP_AUTOJOIN)
        pending_commit_locked();
    if (s_survey && (r->kind == WIFI_OP_AP_START || r->kind == WIFI_OP_JOIN ||
                     r->kind == WIFI_OP_AUTOJOIN || r->kind == WIFI_OP_SCAN))
        return ESP_ERR_INVALID_STATE;
    switch (r->kind) {
    case WIFI_OP_SURVEY_BEGIN:
        if (s_sta_running || s_ap_running || s_survey) return ESP_ERR_INVALID_STATE;
        s_survey = true; return ESP_OK;
    case WIFI_OP_AP_START: return ap_start_locked();
    case WIFI_OP_AP_STOP: return ap_stop_locked();
    case WIFI_OP_JOIN:
        if (!ls_wifi_ssid_valid(r->ssid) || !ls_wifi_pass_valid(r->password ? r->password : ""))
            return ESP_ERR_INVALID_ARG;
        s_roam_used = false; s_roam_pending = false;
        return sta_join_locked(r->ssid, r->password);
    case WIFI_OP_LEAVE: return sta_leave_locked();
    case WIFI_OP_FORGET:
        if (!r->ssid && !s_sta_ssid[0]) return ESP_ERR_NOT_FOUND;
        return sta_forget_locked(r->ssid ? r->ssid : s_sta_ssid);
    case WIFI_OP_FORGET_ALL: return sta_forget_locked(NULL);
    case WIFI_OP_SAVE:
        if (!ls_wifi_ssid_valid(r->ssid) || !ls_wifi_pass_valid(r->password ? r->password : ""))
            return ESP_ERR_INVALID_ARG;
        return store_run(1, r->ssid, r->password ? r->password : "", NULL);
    case WIFI_OP_LIST: return saved_list_locked(r->out, r->capacity);
    case WIFI_OP_AUTOJOIN:
        s_roam_used = false; s_roam_pending = false;
        return sta_autojoin_locked();
    case WIFI_OP_SCAN: return sta_scan_locked(r->out, r->capacity, false);
    case WIFI_OP_SURVEY: return s_survey ? sta_scan_locked(r->out, r->capacity, true) : -1;
    case WIFI_OP_STA_STATUS: sta_status_locked(r->out, r->capacity); return ESP_OK;
    case WIFI_OP_STATUS: status_locked(r->out, r->capacity); return ESP_OK;
    case WIFI_OP_STA_INFO: {
        if (!r->out || !s_sta_connected) return ESP_ERR_INVALID_STATE;
        wifi_ap_record_t ap;
        esp_err_t rc = esp_wifi_sta_get_ap_info(&ap);
        if (rc != ESP_OK) return rc;
        ls_wifi_scan_ap_t *out = r->out;
        snprintf(out->ssid, sizeof(out->ssid), "%.32s", ap.ssid);
        out->rssi = ap.rssi;
        out->secure = ap.authmode != WIFI_AUTH_OPEN;
        out->channel = ap.primary;
        memcpy(out->bssid, ap.bssid, 6); out->auth = ap.authmode;
        return ESP_OK;
    }
    }
    return ESP_ERR_INVALID_ARG;
}

static int perform(wifi_operation_kind_t kind, const char *ssid,
                   const char *password, void *out, int capacity)
{
    wifi_operation_request_t request = {kind, ssid, password, out, capacity};
    return ls_wifi_operation_run(&s_operation, dispatch_operation, &request,
                                 ESP_ERR_INVALID_STATE);
}

esp_err_t ls_wifi_start(void)
{
    return perform(WIFI_OP_AP_START, NULL, NULL, NULL, 0);
}

esp_err_t ls_wifi_stop(void)
{
    return perform(WIFI_OP_AP_STOP, NULL, NULL, NULL, 0);
}

esp_err_t ls_wifi_sta_join(const char *ssid, const char *password)
{
    /* An explicit join is the operator saying "it is there now", which
       is the one thing that can be known from outside. Re-arms the
       budget so giving up is never permanent. */
    return perform(WIFI_OP_JOIN, ssid, password, NULL, 0);
}

esp_err_t ls_wifi_sta_leave(void)
{
    return perform(WIFI_OP_LEAVE, NULL, NULL, NULL, 0);
}

esp_err_t ls_wifi_sta_forget(void)
{
    return perform(WIFI_OP_FORGET, NULL, NULL, NULL, 0);
}

esp_err_t ls_wifi_sta_save(const char *ssid, const char *pass)
{
    return perform(WIFI_OP_SAVE, ssid, pass, NULL, 0);
}

esp_err_t ls_wifi_sta_forget_ssid(const char *ssid)
{
    if (!ls_wifi_ssid_valid(ssid)) return ESP_ERR_INVALID_ARG;
    return perform(WIFI_OP_FORGET, ssid, NULL, NULL, 0);
}

esp_err_t ls_wifi_sta_forget_all(void)
{
    return perform(WIFI_OP_FORGET_ALL, NULL, NULL, NULL, 0);
}

int ls_wifi_saved_list(char names[][33], int cap)
{
    int rc = perform(WIFI_OP_LIST, NULL, NULL, names, cap);
    return rc >= 0 && rc <= LS_WIFI_SAVED_MAX ? rc : -1;
}

esp_err_t ls_wifi_sta_autojoin(void)
{
    return perform(WIFI_OP_AUTOJOIN, NULL, NULL, NULL, 0);
}

int ls_wifi_sta_scan(ls_wifi_scan_ap_t *out, int cap)
{
    int result = perform(WIFI_OP_SCAN, NULL, NULL, out, cap);
    return result == ESP_ERR_INVALID_STATE ? -1 : result;
}

int ls_wifi_scan_cached(ls_wifi_scan_ap_t *out, int cap, int64_t *seen_us)
{
    /* Hold the same gate across copying records and their timestamp. */
    if (!ls_wifi_operation_try(&s_operation)) return -1;
    int n = -1;
    if (out && cap > 0) {
        n = 0;
        if (s_scan_cache_us && esp_timer_get_time() - s_scan_cache_us <= 15000000) {
            n = cap < s_scan_cache_count ? cap : s_scan_cache_count;
            memcpy(out, s_scan_cache, n * sizeof(s_scan_cache[0]));
        }
    }
    if (seen_us) *seen_us = s_scan_cache_us;
    ls_wifi_operation_end(&s_operation);
    return n;
}

esp_err_t ls_wifi_sta_info(ls_wifi_scan_ap_t *out)
{
    return perform(WIFI_OP_STA_INFO, NULL, NULL, out, 0);
}

void ls_wifi_sta_status(char *buf, int cap)
{
    if (!buf || cap <= 0) return;
    if (perform(WIFI_OP_STA_STATUS, NULL, NULL, buf, cap) == ESP_ERR_INVALID_STATE)
        snprintf(buf, cap, "Wi-Fi busy; retry shortly");
}

void ls_wifi_status(char *buf, int cap)
{
    if (!buf || cap <= 0) return;
    if (perform(WIFI_OP_STATUS, NULL, NULL, buf, cap) == ESP_ERR_INVALID_STATE)
        snprintf(buf, cap, "Wi-Fi busy; retry shortly");
}

int ls_wifi_survey_scan(ls_wifi_scan_ap_t *out, int cap)
{
    int rc = perform(WIFI_OP_SURVEY, NULL, NULL, out, cap);
    return rc == ESP_ERR_INVALID_STATE ? -1 : rc;
}

esp_err_t ls_wifi_survey_mode(bool active)
{
    if (!active) { s_survey = false; return ESP_OK; }
    return perform(WIFI_OP_SURVEY_BEGIN, NULL, NULL, NULL, 0);
}

void ls_wifi_link_status(ls_wifi_link_status_t *out)
{
    memset(out,0,sizeof(*out));out->saved=-1;
    if(!ls_wifi_operation_try(&s_operation)) return;
    out->connected=s_sta_connected;out->saved=s_link_saved_count;
    if(out->connected) {
        snprintf(out->ssid,sizeof(out->ssid),"%s",s_sta_ssid);
        snprintf(out->ip,sizeof(out->ip),"%s",s_ip_sta);
    }
    ls_wifi_operation_end(&s_operation);
}
