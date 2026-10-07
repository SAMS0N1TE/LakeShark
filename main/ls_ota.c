/* Updates over WiFi.

   The board fetches a small signed manifest from the update server, checks
   the signature against the key it was built with, and offers the release
   only when it is later than the one running. Installing streams the image
   into the other app slot, hashes it on the way, and switches slots only
   when the hash is the manifest's and IDF accepts the image. The new build
   must run for VALID_AFTER_S and have drawn MIN_FRAMES screens before it marks
   itself good; until both are true it keeps checking. A crash, a reset or a
   power cut sooner and the bootloader goes back to the build before it.

   The manifest must name the channel its URL asks for (stable.txt is
   "stable"), and any one of the keys in ls_ota_key.h may have signed it. A
   download has a total time limit and a stall limit, and any failure aborts
   the slot.

   The download runs on a PSRAM stack, and every step that touches flash is
   handed to ls_nvs_run's worker, whose stack is DRAM: the P4 asserts on a
   flash write from a PSRAM or TCM stack, and it has no DRAM to spare for a
   stack big enough for TLS.

   An erase or a write turns the flash cache off on both cores, and the
   screen's code and framebuffer are behind that cache: a 4 KB sector erase
   alone is tens of milliseconds with every task stopped. Back to back they
   would leave the page frozen for most of the install, so each piece of the
   image waits for the screen's next frame before it is written: the stall
   lands right after a frame and the animation keeps an even beat.

   When the image is in, the board waits to be restarted: a button, or
   RESTART_WAIT_S seconds. The screen is put out before the reset so the
   glass is black through it, not blue.

   The board also looks, quietly, on its own: one background check a few
   minutes after WiFi first connects and one a day after that (the timing is
   ls_ota_bg_* in ls_ota_core, the setting is Settings > DEVICE > Check for
   updates). It reads only the manifest, never the image, never installs, and
   a failure of any kind is silent. It does nothing while an RF app is
   receiving, a job is under way or the heap is low, and tries again ten
   minutes later. A newer verified build lights a dot on HOME and raises one
   UPDATE pop-up per build; installing stays a tap on the UPDATE page. */

#include "ls_ota.h"
#include "ls_ota_key.h"
#include "ls_ota_core.h"
#include "ls_wifi.h"
#include "tui/ls_value.h"
#include "tui/ls_action.h"
#include "tui/ls_tui.h"
#include "tui/ls_app.h"
#include "tui/ls_notify.h"
#include "tui/ls_tui_screen.h"
#include "ls_nvs_safe.h"
#include "ls_task_reap.h"
#include "settings.h"
#include "ls_panel.h"

#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_console.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ota";

#define DEFAULT_URL   "https://" LS_OTA_HOST_PRIMARY "/tdp4/stable.txt"
/* The default until 2026-10: a board that saved it keeps the default, not the URL. */
#define OLD_DEFAULT_URL "https://" LS_OTA_HOST_FALLBACK "/tdp4/stable.txt"
#define BOARD_NAME    "T-Display-P4"
#define VALID_AFTER_S 20
#define MIN_FRAMES    50        /* screens drawn since boot before a trial build is kept */
#define RECHECK_S     3         /* how often a trial that is not healthy yet looks again */
#define DOWNLOAD_MAX_S 900      /* the whole image, 15 min */
#define STALL_S       30        /* no bytes for this long ends the download */
#define WIFI_WAIT_MS  20000
#define CHUNK         4096
#define FRAME_WAIT_MS 120       /* most a write waits for the screen's next frame */
#define RESTART_WAIT_S 10       /* the written build waits this long for a tap */
#define BLANK_FADE_MS  240
#define TASK_STACK    12288     /* PSRAM: TLS and the HTTP client */

enum { JOB_CHECK, JOB_INSTALL, JOB_FORCE, JOB_BG };

static volatile ls_ota_stage_t s_state = LS_OTA_ST_IDLE;
static volatile int     s_percent;
static volatile uint32_t s_got;              /* bytes of the image so far */
static int64_t          s_trial_end_us;    /* when this boot's trial ends, 0 when none */
static volatile bool    s_busy;
static volatile bool    s_restart_now;     /* a tap or ENTER: restart without waiting */
static volatile int64_t s_restart_at_us;   /* when the wait ends, 0 once it has */
static char             s_reason[48];
static ls_ota_manifest_t s_man;            /* from the last good check */
static ls_ota_verdict_t s_verdict;
static bool             s_nothing;         /* the last check found no manifest on the server */
static char             s_url[200] = DEFAULT_URL;
static char             s_failed[32];      /* the build that last rolled back, or "" */
static unsigned         s_stack_left;      /* the last job's unused stack, for sizing */
static volatile bool    s_bg;              /* a background check is running: silent, state untouched */
static volatile bool    s_notify;          /* a newer verified build is known: the HOME dot */
static char             s_announced[48];   /* the last build raised as a pop-up, kept in NVS */
static char             s_note[LS_NOTIFY_BODY]; /* the pop-up waiting for the UI */
static volatile bool    s_note_pending;
static ls_ota_bg_t      s_sched = { true, -1, -1, -1, 0 };
static unsigned         s_bg_blockers;     /* what last stopped a due check */
static int              s_bg_runs;

static const char *running_build(void) { return esp_app_get_description()->version; }

/* "2.9.0-g0123456789ab" -> "2.9.0" */
static const char *short_version(const char *build)
{
    static char v[24];
    snprintf(v, sizeof(v), "%s", build);
    char *g = strstr(v, "-g");
    if (g) *g = 0;
    return v;
}

static void fail(const char *why)
{
    /* A background check says nothing: the page keeps what it showed. */
    if (s_bg) { ESP_LOGI(TAG, "background check: %s", why); return; }
    snprintf(s_reason, sizeof(s_reason), "%s", why);
    s_state = LS_OTA_ST_FAILED;
    ESP_LOGW(TAG, "%s", why);
}

/* ---------------------------------------------------------------- the URL */

static esp_err_t url_load(void *ctx)
{
    (void)ctx;
    nvs_handle_t h;
    char buf[sizeof(s_url)];
    size_t n = sizeof(buf);
    if (nvs_open("ls_ota", NVS_READONLY, &h) != ESP_OK) return ESP_OK;
    if (nvs_get_str(h, "url", buf, &n) == ESP_OK && !strncmp(buf, "https://", 8)
        && strcmp(buf, OLD_DEFAULT_URL))
        snprintf(s_url, sizeof(s_url), "%s", buf);
    n = sizeof(s_announced);
    if (nvs_get_str(h, "seen", s_announced, &n) != ESP_OK) s_announced[0] = 0;
    nvs_close(h);
    return ESP_OK;
}

/* The build last raised as a pop-up, so a reboot does not raise it again. */
static esp_err_t seen_save(void *ctx)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("ls_ota", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_str(h, "seen", (const char *)ctx);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* NULL puts the default back. */
static esp_err_t url_save(void *ctx)
{
    const char *u = ctx;
    nvs_handle_t h;
    esp_err_t e = nvs_open("ls_ota", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = u ? nvs_set_str(h, "url", u) : nvs_erase_key(h, "url");
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* --------------------------------------------------------------- the wire */

static bool wifi_up(void)
{
    char ip[20];
    ls_wifi_sta_ip(ip, sizeof(ip));
    return ls_wifi_sta_connected() && ip[0];
}

static bool wifi_ready(void)
{
    char ip[20];
    ls_wifi_sta_ip(ip, sizeof(ip));
    if (ls_wifi_sta_connected() && ip[0]) return true;
    s_state = LS_OTA_ST_WIFI;
    if (!ls_wifi_sta_running()) {
        const esp_err_t e = ls_wifi_sta_autojoin();
        if (e == ESP_ERR_NOT_FOUND) { fail("join WiFi first"); return false; }
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { fail("WiFi would not start"); return false; }
    }
    for (int waited = 0; waited < WIFI_WAIT_MS; waited += 250) {
        ls_wifi_sta_ip(ip, sizeof(ip));
        if (ls_wifi_sta_connected() && ip[0]) return true;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    fail("WiFi did not connect");
    return false;
}

/* Opened and past the headers, following up to three redirects, every one
   of them to https. *status is -1 when the server could not be reached and
   -2 for a redirect that leaves https; the handle is NULL for both. */
static esp_http_client_handle_t open_url(const char *url, int timeout_ms, int64_t *length, int *status)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = timeout_ms,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    int bad = -1;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { *status = bad; return NULL; }
    for (int hop = 0; hop < 4; hop++) {
        if (esp_http_client_open(c, 0) != ESP_OK) break;
        *length = esp_http_client_fetch_headers(c);
        *status = esp_http_client_get_status_code(c);
        if (*status < 300 || *status >= 400) return c;
        if (hop == 3 || esp_http_client_set_redirection(c) != ESP_OK) break;
        char next[sizeof(s_url)];
        if (esp_http_client_get_url(c, next, sizeof(next)) != ESP_OK || strncmp(next, "https://", 8)) {
            bad = -2;
            break;
        }
        esp_http_client_close(c);
    }
    *status = bad;
    esp_http_client_cleanup(c);
    return NULL;
}

/* open_url, and for a URL on one of the two update hosts, one more try on the
   other when the first cannot be reached or does not answer 200. A redirect
   off https is final. When both fail the first's *status is reported with a
   NULL handle, so the caller can tell unreachable (< 0) from a plain 404. */
static esp_http_client_handle_t open_url_fallback(const char *url, int timeout_ms, int64_t *length, int *status)
{
    esp_http_client_handle_t c = open_url(url, timeout_ms, length, status);
    char alt[sizeof(s_url)];
    if ((c && *status == 200) || *status == -2 || !ls_ota_other_host(url, alt, sizeof(alt))) return c;
    const int first = *status;
    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
    ESP_LOGW(TAG, "%s failed (%d), trying %s", url, first, alt);
    int64_t len2 = 0;
    int st2 = 0;
    esp_http_client_handle_t c2 = open_url(alt, timeout_ms, &len2, &st2);
    if (c2 && st2 == 200) { *length = len2; *status = st2; return c2; }
    if (c2) { esp_http_client_close(c2); esp_http_client_cleanup(c2); }
    *status = first;
    return NULL;
}

static void shut(esp_http_client_handle_t c)
{
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
}

/* -------------------------------------------------------------- the check */

typedef struct {
    const uint8_t *hash;
    const ls_ota_manifest_t *m;
} sig_ctx_t;

static bool key_accepts(size_t key, void *ctx)
{
    const sig_ctx_t *c = ctx;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    const char *pem = LS_OTA_PUBLIC_KEYS[key];
    const bool ok =
        mbedtls_pk_parse_public_key(&pk, (const unsigned char *)pem, strlen(pem) + 1) == 0
        && mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, c->hash, 32, c->m->sig, c->m->sig_len) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}

static bool signed_by_us(const char *text, const ls_ota_manifest_t *m)
{
    uint8_t hash[32];
    if (mbedtls_sha256((const unsigned char *)text, m->signed_len, hash, 0) != 0) return false;
    sig_ctx_t c = { hash, m };
    return ls_ota_verify_any(LS_OTA_PUBLIC_KEY_COUNT, key_accepts, &c);
}

/* No manifest for the channel is not a failure: nothing newer exists, so the
   board is up to date. Ends the check the way "not newer" does. */
static bool nothing_published(void)
{
    s_notify = false;
    /* A background check leaves the page alone, unless it was offering a
       build the server no longer has. */
    if (s_bg && s_state != LS_OTA_ST_AVAILABLE) {
        ESP_LOGI(TAG, "background check: nothing published");
        return true;
    }
    memset(&s_man, 0, sizeof(s_man));
    s_verdict = LS_OTA_CURRENT;
    s_nothing = true;
    s_state = LS_OTA_ST_CURRENT;
    ESP_LOGI(TAG, "nothing published yet, running %s", running_build());
    return true;
}

/* A newer verified build is known. `post` raises the one pop-up for it; either
   way the build is remembered as announced, so a manual check that found it
   first does not leave it to be announced later. */
static void notice_found(const ls_ota_manifest_t *m, bool post)
{
    s_notify = true;
    if (!ls_ota_notice_new(m->build, s_announced)) return;
    char b[sizeof(s_announced)];
    snprintf(b, sizeof(b), "%s", m->build);
    if (ls_nvs_run(seen_save, b, 3072) != ESP_OK) return;      /* not kept: try again next check */
    snprintf(s_announced, sizeof(s_announced), "%s", b);
    if (post && ls_ota_toast_text(m->version, s_note, sizeof(s_note)))
        __atomic_store_n(&s_note_pending, true, __ATOMIC_RELEASE);
}

static bool do_check(void)
{
    if (s_bg ? !wifi_up() : !wifi_ready()) return false;
    if (!s_bg) {
        s_state = LS_OTA_ST_CHECKING;
        s_nothing = false;
    }

    int64_t length = 0;
    int status = 0;
    esp_http_client_handle_t c = open_url_fallback(s_url, 20000, &length, &status);
    if (!c) {
        if (status == 404 || status == 410) return nothing_published();
        char w[32];
        snprintf(w, sizeof(w), "update server answered %d", status);
        fail(status == -2 ? "redirect not on https" : status < 0 ? "can't reach the update server" : w);
        return false;
    }
    char *text = malloc(LS_OTA_MAX_TEXT + 1);
    if (!text) { shut(c); fail("no memory"); return false; }
    size_t n = 0;
    int r = 0;
    while (n < LS_OTA_MAX_TEXT && (r = esp_http_client_read(c, text + n, LS_OTA_MAX_TEXT - n)) > 0)
        n += (size_t)r;
    /* The image's relative URL is beside the manifest as it was finally
       served, which a redirect may have moved. */
    char base[sizeof(s_url)];
    if (esp_http_client_get_url(c, base, sizeof(base)) != ESP_OK) snprintf(base, sizeof(base), "%s", s_url);
    shut(c);
    text[n] = 0;

    ls_ota_manifest_t m;
    ls_ota_err_t e = LS_OTA_ERR_MAGIC;
    bool ok = false, none = false;
    if (r < 0)                                   fail("can't reach the update server");
    else if ((e = ls_ota_parse(text, n, base, &m)) != LS_OTA_OK) {
        /* The site's own page where a manifest should be is no manifest. */
        char w[48];
        snprintf(w, sizeof(w), "manifest: %s", ls_ota_err_text(e));
        if (e == LS_OTA_ERR_MAGIC) none = true;
        else fail(w);
    }
    else if (!signed_by_us(text, &m))            fail("manifest not signed by LakeShark");
    else if (strcmp(m.board, BOARD_NAME))        fail("manifest is for another board");
    else if (!ls_ota_channel_ok(&m, s_url))      fail("manifest is for another channel");
    else                                         ok = true;
    free(text);
    if (none) return nothing_published();
    if (!ok) return false;

    const ls_ota_verdict_t found = ls_ota_judge(running_build(), &m);
    /* A build that already failed to start here is offered, never announced. */
    const bool news = found == LS_OTA_NEWER && strcmp(m.build, s_failed);
    if (s_bg) {
        if (found == LS_OTA_NEWER) {
            s_man = m;
            s_verdict = found;
            s_nothing = false;
            s_state = LS_OTA_ST_AVAILABLE;
        } else if (s_state == LS_OTA_ST_AVAILABLE) {    /* no longer offered */
            s_man = m;
            s_verdict = found;
            s_state = LS_OTA_ST_CURRENT;
        }
        s_notify = false;
        if (news) notice_found(&m, true);
        ESP_LOGI(TAG, "background check: server has %s (%s)", m.build, found == LS_OTA_NEWER ? "newer" : "not newer");
        return true;
    }
    s_man = m;
    s_verdict = found;
    s_state = s_verdict == LS_OTA_NEWER ? LS_OTA_ST_AVAILABLE : LS_OTA_ST_CURRENT;
    s_notify = false;
    if (news) notice_found(&m, false);
    ESP_LOGI(TAG, "server has %s (%s), running %s", m.build, s_verdict == LS_OTA_NEWER ? "newer" : "not newer",
             running_build());
    return true;
}

/* ------------------------------------------------------------ the install */

/* The slot is written from ls_nvs_run's worker, whose stack is DRAM: the
   download runs on a PSRAM stack, and a flash write from there asserts. Each
   piece is copied onto that stack first, because IDF writes a buffer outside
   DRAM 32 bytes at a time. */
typedef struct {
    const esp_partition_t *part;
    esp_ota_handle_t       h;
    const uint8_t         *buf;
    size_t                 len;
} slot_job_t;

static esp_err_t slot_begin(void *ctx)
{
    slot_job_t *j = ctx;
    j->part = esp_ota_get_next_update_partition(NULL);
    if (!j->part) return ESP_ERR_NOT_FOUND;
    return esp_ota_begin(j->part, OTA_WITH_SEQUENTIAL_WRITES, &j->h);
}

static esp_err_t slot_write(void *ctx)
{
    slot_job_t *j = ctx;
    uint8_t page[512];
    for (size_t at = 0; at < j->len; at += sizeof(page)) {
        const size_t n = j->len - at < sizeof(page) ? j->len - at : sizeof(page);
        memcpy(page, j->buf + at, n);
        const esp_err_t e = esp_ota_write(j->h, page, n);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}

static esp_err_t slot_abort(void *ctx) { return esp_ota_abort(((slot_job_t *)ctx)->h); }
static esp_err_t slot_end(void *ctx)   { return esp_ota_end(((slot_job_t *)ctx)->h); }
static esp_err_t slot_boot(void *ctx)  { return esp_ota_set_boot_partition(((slot_job_t *)ctx)->part); }

static void do_install(bool force)
{
    if (!do_check()) return;
    if (s_nothing || (s_state != LS_OTA_ST_AVAILABLE && !force)) return;

    int64_t length = 0;
    int status = 0;
    esp_http_client_handle_t c = open_url_fallback(s_man.url, 8000, &length, &status);
    if (!c) {
        fail(status == -2 ? "redirect not on https" : status < 0 ? "can't reach the update server"
                                                                  : "image not on the server");
        return;
    }
    /* No length (chunked, or IDF's 0 for unknown) is not a wrong length: the
       byte count and the hash below still decide. */
    if (status != 200 || (length > 0 && length != (int64_t)s_man.size)) {
        shut(c);
        fail(status != 200 ? "image not on the server" : "image size differs");
        return;
    }
    uint8_t *buf = malloc(CHUNK);
    if (!buf) { shut(c); fail("no memory"); return; }

    slot_job_t j = { 0 };
    esp_err_t e = ls_nvs_run(slot_begin, &j, 0);
    if (e != ESP_OK || s_man.size > j.part->size) {
        if (e == ESP_OK) ls_nvs_run(slot_abort, &j, 0);
        free(buf);
        shut(c);
        fail(e == ESP_ERR_NOT_FOUND ? "no update slot: install once by USB"
             : e != ESP_OK ? "could not open the slot" : "image too big for the slot");
        return;
    }

    s_percent = 0;
    s_got = 0;
    s_state = LS_OTA_ST_DOWNLOADING;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    uint32_t got = 0, seen = ls_tui_present_count();
    bool longer = false;
    const char *why = NULL;                 /* set by the first thing that goes wrong */
    const int64_t t0 = esp_timer_get_time();
    int64_t last_data = t0;
    while (got < s_man.size) {
        const int64_t now = esp_timer_get_time();
        if (now - t0 > (int64_t)DOWNLOAD_MAX_S * 1000000) { why = "download took too long"; break; }
        if (now - last_data > (int64_t)STALL_S * 1000000) { why = "download stalled"; break; }
        const int r = esp_http_client_read(c, (char *)buf, CHUNK);
        if (r == -ESP_ERR_HTTP_EAGAIN) continue;    /* nothing yet: the stall limit decides */
        if (r <= 0) break;
        if (got + (uint32_t)r > s_man.size) { longer = true; break; }
        mbedtls_sha256_update(&sha, buf, (size_t)r);
        j.buf = buf;
        j.len = (size_t)r;
        /* Start each erase right after a frame went out, so the stall lands
           in the render loop's sleep and the frames keep an even beat. */
        for (int waited = 0; ls_tui_present_count() == seen && waited < FRAME_WAIT_MS; waited++)
            vTaskDelay(1);
        if ((e = ls_nvs_run(slot_write, &j, 0)) != ESP_OK) break;
        seen = ls_tui_present_count();
        got += (uint32_t)r;
        s_percent = (int)((uint64_t)got * 100u / s_man.size);
        s_got = got;
        last_data = esp_timer_get_time();   /* a slow flash write is not a stalled server */
    }
    shut(c);
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    free(buf);

    if (!why && e != ESP_OK)                        why = "flash write failed";
    else if (!why && (longer || got != s_man.size)) why = "download cut short";
    if (!why) {
        s_state = LS_OTA_ST_VERIFYING;
        if (memcmp(digest, s_man.sha256, sizeof(digest))) why = "image hash wrong";
    }
    if (why) {                                      /* every failure gives the slot back */
        ls_nvs_run(slot_abort, &j, 0);
        fail(why);
        return;
    }
    if ((e = ls_nvs_run(slot_end, &j, 0)) != ESP_OK) {
        fail(e == ESP_ERR_OTA_VALIDATE_FAILED ? "image rejected" : "could not finish the slot");
        return;
    }
    if (ls_nvs_run(slot_boot, &j, 0) != ESP_OK) { fail("could not switch slots"); return; }
    ESP_LOGI(TAG, "%s written to %s; restart in %d s", s_man.build, j.part->label, RESTART_WAIT_S);
    s_restart_now = false;
    s_restart_at_us = esp_timer_get_time() + (int64_t)RESTART_WAIT_S * 1000000;
    s_state = LS_OTA_ST_RESTARTING;
    while (!s_restart_now && esp_timer_get_time() < s_restart_at_us)
        vTaskDelay(pdMS_TO_TICKS(50));
    /* From here the page says it is restarting, then the glass goes dark. */
    s_restart_at_us = 0;
    vTaskDelay(pdMS_TO_TICKS(150));
    ls_panel_blank(BLANK_FADE_MS);
    vTaskDelay(pdMS_TO_TICKS(80));      /* a frame or two with the panel off */
    esp_restart();
}

/* ------------------------------------------------------------- the worker */

static void ota_worker(void *arg)
{
    const int job = (int)(intptr_t)arg;
    if (job == JOB_CHECK)   do_check();
    else if (job == JOB_BG) {
        s_bg = true;
        do_check();
        s_bg = false;
    }
    else                    do_install(job == JOB_FORCE);
    s_stack_left = uxTaskGetStackHighWaterMark(NULL);
    __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
    ls_task_retire_self();
}

typedef enum { JOB_STARTED, JOB_BUSY, JOB_NO_MEMORY } start_t;

/* One job at a time, claimed in one step: the Settings row and the console
   can ask at the same moment. The worker's stack is PSRAM: WiFi, TLS and
   hashing never touch flash, and the board has no 8 KB of DMA-capable DRAM
   to spare (the radios' buffers live there). Its flash work goes through
   ls_nvs_run. */
static start_t start_job(int job)
{
    if (__atomic_exchange_n(&s_busy, true, __ATOMIC_ACQ_REL)) return JOB_BUSY;
    if (xTaskCreatePinnedToCoreWithCaps(ota_worker, "ota", TASK_STACK, (void *)(intptr_t)job, 3, NULL,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
        if (job != JOB_BG) fail("no memory for the update task");
        return JOB_NO_MEMORY;
    }
    return JOB_STARTED;
}

/* ------------------------------------------------ the background check */

/* An RF app is streaming or receiving when the receiver is claimed (P25, FM,
   ADS-B, REC, an SDR or LR2021 job) or when any app's directory lamp is lit:
   the same `live` the HOME tiles use, which covers EXPERIMENTS and SUB-GHZ
   watch. MESH and GPS are lit whenever their stack is up, so they say nothing
   about a streaming receiver and are left out. */
static bool rf_busy(void)
{
    if (ls_tui_radio_claimed()) return true;
    for (int i = 0; i < ls_app_count(); i++) {
        const ls_app_t *a = ls_app_at(i);
        if (!a || !a->live || !a->id) continue;
        if (!strcmp(a->id, "mesh") || !strcmp(a->id, "gps")) continue;
        if (a->live()) return true;
    }
    return false;
}

/* Every BG_TICK_S, on the esp_timer task: cheap unless a check is due, and
   it only ever starts the ordinary check job. */
#define BG_TICK_S 15

static void bg_tick(void *arg)
{
    (void)arg;
    s_sched.enabled = settings_get_update_check();
    if (!s_sched.enabled) return;                       /* off: nothing at all */
    const int64_t now_s = esp_timer_get_time() / 1000000;
    const bool up = wifi_up();
    if (s_sched.wifi_up_s < 0) {
        if (up) s_sched.wifi_up_s = now_s;              /* the first check is a few minutes on */
        return;
    }
    if (ls_ota_bg_next_s(&s_sched) > now_s) return;
    s_bg_blockers = ls_ota_bg_blockers(__atomic_load_n(&s_busy, __ATOMIC_ACQUIRE), rf_busy(), up,
                                       heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                                       heap_caps_get_free_size(MALLOC_CAP_DMA));
    if (ls_ota_bg_act(&s_sched, now_s, s_bg_blockers) != LS_OTA_BG_RUN) {
        s_sched.last_defer_s = now_s;
        return;
    }
    if (start_job(JOB_BG) != JOB_STARTED) {
        s_bg_blockers |= LS_OTA_BG_BUSY_JOB;
        s_sched.last_defer_s = now_s;
        return;
    }
    s_sched.last_run_s = now_s;
    s_sched.last_defer_s = -1;
    s_bg_runs++;
}

/* The pop-up, raised once per build from the UI's own frame poll: tapping it
   opens UPDATE. A system notice, so it is drawn in its own colour. */
static bool ota_notice(ls_notice_t *out)
{
    if (!__atomic_exchange_n(&s_note_pending, false, __ATOMIC_ACQ_REL)) return false;
    snprintf(out->title, sizeof(out->title), "UPDATE");
    snprintf(out->body, sizeof(out->body), "%s", s_note);
    out->hue = TUI_MAGENTA;
    out->accent = TUI_MAGENTA;
    const ls_app_t *a = ls_app_by_id("update");
    out->screen = a && a->screen ? ls_tui_screen_index_of(a->screen) : -1;
    return true;
}

static esp_err_t mark_valid(void *ctx)
{
    (void)ctx;
    return esp_ota_mark_app_valid_cancel_rollback();
}

/* A build on its first boot after an update proves itself by running: it is
   marked good only when VALID_AFTER_S has passed and the screen has drawn
   MIN_FRAMES since boot. A build that is up but not drawing is looked at
   again every RECHECK_S, never kept on time alone. ls_nvs_call's worker has
   its own DRAM stack, so keeping it needs no stack of its own. */
static esp_timer_handle_t s_valid_timer;

static void valid_timer(void *arg)
{
    (void)arg;
    if (ls_tui_present_count() < MIN_FRAMES) {
        ESP_LOGW(TAG, "%s has drawn %u frames: still on trial", running_build(),
                 (unsigned)ls_tui_present_count());
        /* If the timer cannot be re-armed the build stays on trial, and the
           next reset rolls it back. */
        esp_timer_start_once(s_valid_timer, (uint64_t)RECHECK_S * 1000000u);
        return;
    }
    s_trial_end_us = 0;
    if (ls_nvs_call(mark_valid, NULL, 3072) == ESP_OK)
        ESP_LOGI(TAG, "%s ran %d s, drew %u frames: kept", running_build(), VALID_AFTER_S,
                 (unsigned)ls_tui_present_count());
}

static esp_err_t boot_state(void *ctx)
{
    url_load(NULL);
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    *(bool *)ctx = run && esp_ota_get_state_partition(run, &st) == ESP_OK
                   && st == ESP_OTA_IMG_PENDING_VERIFY;
    /* The build the bootloader last gave up on, so a check can say so
       instead of offering it as if it were new. */
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t d;
    if (bad && esp_ota_get_partition_description(bad, &d) == ESP_OK)
        snprintf(s_failed, sizeof(s_failed), "%s", d.version);
    return ESP_OK;
}

/* ------------------------------------------------- value, action, console */

/* Whole seconds before the written build restarts, 0 once it has begun to. */
static long restart_left(void)
{
    const int64_t at = s_restart_at_us, left = at - esp_timer_get_time();
    return s_state == LS_OTA_ST_RESTARTING && at && left > 0 ? (long)((left + 999999) / 1000000) : 0;
}

static bool v_state(ls_val_t *out)
{
    static char text[64];
    const char *run = short_version(running_build());
    switch (s_state) {
    case LS_OTA_ST_IDLE:        snprintf(text, sizeof(text), "%s: tap CHECK", run); break;
    case LS_OTA_ST_WIFI:        snprintf(text, sizeof(text), "joining WiFi"); break;
    case LS_OTA_ST_CHECKING:    snprintf(text, sizeof(text), "checking"); break;
    case LS_OTA_ST_CURRENT:     snprintf(text, sizeof(text), s_nothing ? "%s is the latest, nothing published yet"
                                  : s_verdict == LS_OTA_OLDER ? "%s, newer than the server" : "%s is the latest", run); break;
    case LS_OTA_ST_AVAILABLE:   snprintf(text, sizeof(text), strcmp(s_man.build, s_failed)
                                  ? "%s ready: tap INSTALL" : "%s did not start: tap INSTALL to retry",
                                  s_man.version); break;
    case LS_OTA_ST_DOWNLOADING: snprintf(text, sizeof(text), "downloading %d%%", s_percent); break;
    case LS_OTA_ST_VERIFYING:   snprintf(text, sizeof(text), "checking the image"); break;
    case LS_OTA_ST_RESTARTING: {
        const long left = restart_left();
        if (left > 0) snprintf(text, sizeof(text), "restart in %ld s: tap RESTART", left);
        else          snprintf(text, sizeof(text), "restarting");
        break;
    }
    case LS_OTA_ST_FAILED:      snprintf(text, sizeof(text), "%s", s_reason); break;
    case LS_OTA_ST_UPDATED:     snprintf(text, sizeof(text), "updated to %s", run); break;
    }
    out->kind = LS_VAL_TEXT;
    out->s = text;
    return true;
}

/* What the update screen draws from, beside ota.state's one line. */
static bool v_stage(ls_val_t *o) { o->kind = LS_VAL_INT; o->i = s_state; return true; }
static bool v_got(ls_val_t *o)   { o->kind = LS_VAL_INT; o->i = (long)s_got; return true; }
static bool v_size(ls_val_t *o)  { o->kind = LS_VAL_INT; o->i = (long)s_man.size; return true; }
static bool v_build(ls_val_t *o) { o->kind = LS_VAL_TEXT; o->s = s_man.build; return true; }
static bool v_notes(ls_val_t *o) { o->kind = LS_VAL_TEXT; o->s = s_man.notes; return true; }
static bool v_notify(ls_val_t *o) { o->kind = LS_VAL_INT; o->i = s_notify ? 1 : 0; return true; }
static bool v_running(ls_val_t *o) { o->kind = LS_VAL_TEXT; o->s = running_build(); return true; }
static bool v_failed(ls_val_t *o)  { o->kind = LS_VAL_TEXT; o->s = s_failed; return true; }
static bool v_restart(ls_val_t *o) { o->kind = LS_VAL_INT; o->i = restart_left(); return true; }
static bool v_trial(ls_val_t *o)
{
    const int64_t left = s_trial_end_us - esp_timer_get_time();
    o->kind = LS_VAL_INT;
    o->i = s_trial_end_us && left > 0 ? (long)((left + 999999) / 1000000) : 0;
    return true;
}

/* The UPDATE button: check, or install what the check found. */
static ls_act_status_t a_step(const ls_args_t *in, ls_val_t *out)
{
    (void)in;
    (void)out;
    if (s_state == LS_OTA_ST_RESTARTING) {
        /* The new build is written and waiting: this is RESTART. */
        if (!restart_left()) return LS_ACT_BUSY;
        s_restart_now = true;
        return LS_ACT_OK;
    }
    if (__atomic_load_n(&s_busy, __ATOMIC_ACQUIRE)) return LS_ACT_BUSY;
    switch (start_job(s_state == LS_OTA_ST_AVAILABLE ? JOB_INSTALL : JOB_CHECK)) {
    case JOB_STARTED: return LS_ACT_OK;
    case JOB_BUSY:    return LS_ACT_BUSY;
    default:          return LS_ACT_FAILED;
    }
}

void ls_ota_publish(void)
{
    static bool done;
    if (done) return;
    done = true;
    ls_value_publish("ota.state", NULL, v_state);
    ls_value_publish("ota.stage", NULL, v_stage);
    ls_value_publish("ota.got", "B", v_got);
    ls_value_publish("ota.size", "B", v_size);
    ls_value_publish("ota.build", NULL, v_build);
    ls_value_publish("ota.notes", NULL, v_notes);
    ls_value_publish("ota.notify", NULL, v_notify);
    ls_value_publish("ota.running", NULL, v_running);
    ls_value_publish("ota.failed", NULL, v_failed);
    ls_value_publish("ota.trial", "s", v_trial);
    ls_value_publish("ota.restart", "s", v_restart);
    ls_action_register("ota.step", "", LS_CAP_STORE | LS_CAP_POWER, a_step,
                       "check for an update, install the one found, or restart into it");
    bool trial = false;
    ls_nvs_run(boot_state, &trial, 3072);
    ls_notify_add_probe(ota_notice);
    s_sched.seed = esp_random();
    static esp_timer_handle_t bg_timer;
    const esp_timer_create_args_t bg_args = { .callback = bg_tick, .name = "ota_bg" };
    if (esp_timer_create(&bg_args, &bg_timer) == ESP_OK)
        esp_timer_start_periodic(bg_timer, (uint64_t)BG_TICK_S * 1000000u);
    if (!trial) return;
    s_state = LS_OTA_ST_UPDATED;
    s_trial_end_us = esp_timer_get_time() + (int64_t)VALID_AFTER_S * 1000000;
    const esp_timer_create_args_t args = { .callback = valid_timer, .name = "ota_valid" };
    if (esp_timer_create(&args, &s_valid_timer) != ESP_OK
        || esp_timer_start_once(s_valid_timer, (uint64_t)VALID_AFTER_S * 1000000u) != ESP_OK) {
        s_trial_end_us = 0;
        /* Without the timer the trial could never end, and the next restart
           would throw away a build that got this far: keep it now. */
        ls_nvs_run(mark_valid, NULL, 3072);
    }
}

typedef struct {
    const esp_partition_t *run, *boot, *next;
    esp_ota_img_states_t   st;
    bool                   have_st;
} slots_t;

static esp_err_t read_slots(void *ctx)
{
    slots_t *s = ctx;
    s->run = esp_ota_get_running_partition();
    s->boot = esp_ota_get_boot_partition();
    s->next = esp_ota_get_next_update_partition(NULL);
    s->have_st = s->run && esp_ota_get_state_partition(s->run, &s->st) == ESP_OK;
    return ESP_OK;
}

static const char *st_name(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "on trial";
    case ESP_OTA_IMG_VALID:          return "kept";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "rolled back";
    default:                         return "no record";
    }
}

static void print_state(void)
{
    ls_val_t v;
    v_state(&v);
    printf("ota: %s\n", v.s);
}

/* A span of seconds for the console: "45 s", "7 m", "23 h 57 m". */
static void span_text(char *out, size_t cap, int64_t secs)
{
    if (secs < 120)       snprintf(out, cap, "%lld s", (long long)secs);
    else if (secs < 3600) snprintf(out, cap, "%lld m", (long long)(secs / 60));
    else                  snprintf(out, cap, "%lld h %lld m", (long long)(secs / 3600), (long long)((secs / 60) % 60));
}

static int ota_cmd(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "";
    if (!strcmp(sub, "check") || !strcmp(sub, "install")) {
        const bool install = !strcmp(sub, "install");
        const bool force = install && argc > 2 && !strcmp(argv[2], "force");
        const start_t started = start_job(install ? (force ? JOB_FORCE : JOB_INSTALL) : JOB_CHECK);
        if (started != JOB_STARTED) {
            printf("ota: %s\n", started == JOB_BUSY ? "busy" : s_reason);
            return 1;
        }
        if (install) {
            printf("ota: installing; `ota` shows progress\n");
            return 0;
        }
        for (int i = 0; i < 900 && __atomic_load_n(&s_busy, __ATOMIC_ACQUIRE); i++)
            vTaskDelay(pdMS_TO_TICKS(100));
        if (__atomic_load_n(&s_busy, __ATOMIC_ACQUIRE)) {
            printf("ota: still checking after 90 s; `ota` shows the result\n");
            return 1;
        }
        print_state();
        if (s_state == LS_OTA_ST_AVAILABLE || (s_state == LS_OTA_ST_CURRENT && !s_nothing))
            printf("ota: server %s, %lu bytes, %s\n", s_man.build, (unsigned long)s_man.size, s_man.url);
        return s_state == LS_OTA_ST_FAILED;
    }
    if (!strcmp(sub, "url")) {
        if (argc > 2) {
            if (__atomic_load_n(&s_busy, __ATOMIC_ACQUIRE)) {
                printf("ota: busy; change the manifest when the job is done\n");
                return 1;
            }
            const bool def = !strcmp(argv[2], "default") || !strcmp(argv[2], OLD_DEFAULT_URL);
            if (!def && strncmp(argv[2], "https://", 8)) {
                printf("ota: the manifest must be on https\n");
                return 1;
            }
            if (ls_nvs_run(url_save, def ? NULL : argv[2], 3072) != ESP_OK) {
                printf("ota: could not save\n");
                return 1;
            }
            snprintf(s_url, sizeof(s_url), "%s", def ? DEFAULT_URL : argv[2]);
            s_state = LS_OTA_ST_IDLE;
        }
        printf("ota: manifest %s\n", s_url);
        return 0;
    }
    if (!strcmp(sub, "restart")) {
        if (!restart_left()) { printf("ota: nothing written is waiting to restart\n"); return 1; }
        s_restart_now = true;
        printf("ota: restarting\n");
        return 0;
    }
    if (!strcmp(sub, "valid")) {
        const esp_err_t e = ls_nvs_run(mark_valid, NULL, 3072);
        printf("ota: mark valid: %s\n", esp_err_to_name(e));
        return e != ESP_OK;
    }
    if (*sub) {
        printf("usage: ota [check | install [force] | restart | url [URL|default] | valid]\n");
        return 1;
    }
    slots_t s = { 0 };
    ls_nvs_run(read_slots, &s, 3072);
    char ip[20];
    ls_wifi_sta_ip(ip, sizeof(ip));
    printf("ota: running %s from %s (%s); boots %s; next %s\n", running_build(),
           s.run ? s.run->label : "?", s.have_st ? st_name(s.st) : "no record",
           s.boot ? s.boot->label : "?", s.next ? s.next->label : "none");
    printf("ota: manifest %s\n", s_url);
    if (s_failed[0]) printf("ota: last build that did not start: %s\n", s_failed);
    printf("ota: wifi %s %s; task stack left %u; internal free %u\n",
           ls_wifi_sta_connected() ? "up" : "down", ip, s_stack_left,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    print_state();
    const int64_t now_s = esp_timer_get_time() / 1000000;
    s_sched.enabled = settings_get_update_check();
    const int64_t next = ls_ota_bg_next_s(&s_sched);
    printf("ota: background check %s", s_sched.enabled ? "Daily" : "Off");
    if (!s_sched.enabled)    printf(", no background traffic\n");
    else if (next < 0)       printf(", waits for WiFi to connect\n");
    else if (next > now_s) {
        char in[24], at[24], up[24];
        span_text(in, sizeof(in), next - now_s);
        span_text(at, sizeof(at), next);
        span_text(up, sizeof(up), now_s);
        printf(", next in %s (at uptime %s), uptime now %s\n", in, at, up);
    }
    else                     printf(", due now, waiting for a clear moment\n");
    printf("ota: background runs %d, last stopped by%s%s%s%s%s; newer build %s; announced %s\n", s_bg_runs,
           s_bg_blockers & LS_OTA_BG_BUSY_JOB ? " job" : "", s_bg_blockers & LS_OTA_BG_BUSY_RF ? " rf" : "",
           s_bg_blockers & LS_OTA_BG_LOW_HEAP ? " heap" : "", s_bg_blockers & LS_OTA_BG_NO_WIFI ? " wifi" : "",
           s_bg_blockers ? "" : " nothing", s_notify ? "known" : "none", s_announced[0] ? s_announced : "none");
    return 0;
}

void ls_ota_console_register(void)
{
    static const esp_console_cmd_t c = {
        .command = "ota",
        .help = "Updates over WiFi: 'ota' states, 'ota check', 'ota install [force]', "
                "'ota restart', 'ota url [URL|default]', 'ota valid'",
        .hint = "[check|install [force]|restart|url [URL|default]|valid]",
        .func = ota_cmd,
        .argtable = NULL,
    };
    esp_console_cmd_register(&c);
}
