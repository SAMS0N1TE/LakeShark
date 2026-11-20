/* See ls_fetch.h. One HTTPS path (OTA's open, its trust and host fallback),
   one queue task on a PSRAM stack. Nothing here writes flash. */
#include "ls_fetch.h"
#include "ls_ota.h"
#include "ls_ota_core.h"
#include "ls_wifi.h"
#include "ls_textpack.h"
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK     4096
#define STALL_US  30000000
#define STACK     16384         /* PSRAM: TLS and the HTTP client, as OTA's */

typedef struct {
    ls_fetch_req_t  q;
    char            url[224], fallback[160], dest[112];
    uint8_t         sha[32];
    ls_fetch_info_t info;
    volatile bool   cancel, used;
} job_t;

static EXT_RAM_BSS_ATTR job_t s_jobs[LS_FETCH_JOBS];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static int s_next_id;

bool ls_fetch_online(void)
{
    char ip[20];
    ls_wifi_sta_ip(ip, sizeof(ip));
    if (ls_wifi_sta_connected() && ip[0]) return true;
    esp_netif_t *eth = esp_netif_get_handle_from_ifkey("ETH_DEF");
    esp_netif_ip_info_t a;
    return eth && esp_netif_is_netif_up(eth) && esp_netif_get_ip_info(eth, &a) == ESP_OK && a.ip.addr;
}

const char *ls_fetch_result_text(ls_fetch_result_t r)
{
    static const char *const T[] = { "ok", "queued", "running", "no network", "unreachable", "http error",
        "wrong size", "hash mismatch", "bad signature", "card error", "timed out", "cancelled", "busy" };
    return (unsigned)r < sizeof(T) / sizeof(T[0]) ? T[r] : "?";
}

static int nib(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}

/* Hex pairs into out, at most cap bytes; returns how many, stops at the first non-pair. */
static size_t unhex(const char **s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    for (; n < cap && nib((*s)[0]) >= 0 && nib((*s)[1]) >= 0; *s += 2) out[n++] = (uint8_t)(nib((*s)[0]) << 4 | nib((*s)[1]));
    return n;
}

static bool hash_file(FILE *f, uint32_t len, uint8_t out[32])
{
    uint8_t *b = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool ok = b && !fseek(f, 0, SEEK_SET) && !mbedtls_sha256_starts(&sha, 0);
    while (ok && len) {
        const size_t n = len < CHUNK ? len : CHUNK;
        ok = fread(b, 1, n, f) == n && !mbedtls_sha256_update(&sha, b, n);
        len -= (uint32_t)n;
    }
    ok = ok && !mbedtls_sha256_finish(&sha, out);
    mbedtls_sha256_free(&sha);
    heap_caps_free(b);
    return ok;
}

/* The body (a file, or len bytes at mem) ends in a good sig= line. */
static bool sig_ok(FILE *f, const char *mem, uint32_t len, uint32_t *signed_len)
{
    char tail[260];
    const uint32_t n = len < sizeof(tail) - 1 ? len : sizeof(tail) - 1;
    if (f) { if (fseek(f, (long)(len - n), SEEK_SET) || fread(tail, 1, n, f) != n) return false; }
    else memcpy(tail, mem + len - n, n);
    tail[n] = 0;
    const char *s = NULL;
    for (const char *p = tail; (p = strstr(p, "\nsig=")); p += 5) s = p + 1;
    if (!s) return false;
    ls_ota_manifest_t m;
    const char *h = s + 4;
    m.sig_len = unhex(&h, m.sig, sizeof(m.sig));
    while (*h == '\r' || *h == '\n') h++;
    if (*h || !m.sig_len) return false;
    const uint32_t body = len - n + (uint32_t)(s - tail);
    uint8_t hash[32];
    if (f ? !hash_file(f, body, hash) : mbedtls_sha256((const unsigned char *)mem, body, hash, 0) != 0) return false;
    if (signed_len) *signed_len = body;
    return ls_ota_hash_signed_by_us(hash, &m);
}

/* A power cut between publish()'s two renames leaves only X.bak: the last
   good file goes back under its name. */
bool ls_fetch_recover(const char *dest)
{
    char bak[120];
    struct stat st;
    if (!stat(dest, &st)) return true;
    snprintf(bak, sizeof(bak), "%s.bak", dest);
    return !stat(bak, &st) && !rename(bak, dest);
}

bool ls_fetch_file_signed(const char *path, uint32_t *signed_len)
{
    struct stat st;
    ls_fetch_recover(path);
    FILE *f = !stat(path, &st) ? fopen(path, "rb") : NULL;
    if (!f) return false;
    const bool ok = sig_ok(f, NULL, (uint32_t)st.st_size, signed_len);
    fclose(f);
    return ok;
}

static void make_dirs(char *path)
{
    for (char *p = strchr(path + 1, '/'); p && (p = strchr(p + 1, '/'));) { *p = 0; mkdir(path, 0775); *p = '/'; }
}

/* FAT cannot rename over a file: the last good one steps aside as .bak and
   comes back if the new one cannot take its place. */
static bool publish(const char *part, const char *dest)
{
    char bak[120];
    struct stat st;
    snprintf(bak, sizeof(bak), "%s.bak", dest);
    const bool have = !stat(dest, &st);
    if (have) { remove(bak); if (rename(dest, bak)) return false; }
    if (rename(part, dest)) { if (have) (void)rename(bak, dest); return false; }
    if (have) remove(bak);
    return true;
}

/* Files being fetched right now: a second job for one of them would share its
   .part and corrupt both. */
static EXT_RAM_BSS_ATTR char s_busy[LS_FETCH_JOBS + 2][112];

static bool claim(const char *dest)
{
    bool ok = true;
    int free_slot = -1;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < LS_FETCH_JOBS + 2; i++) {
        if (!s_busy[i][0]) { if (free_slot < 0) free_slot = i; }
        else if (!strcmp(s_busy[i], dest)) ok = false;
    }
    if (ok && free_slot >= 0) snprintf(s_busy[free_slot], sizeof(s_busy[0]), "%s", dest);
    else if (ok) ok = false;
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

static void release(const char *dest)
{
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < LS_FETCH_JOBS + 2; i++)
        if (!strcmp(s_busy[i], dest)) { s_busy[i][0] = 0; break; }
    portEXIT_CRITICAL(&s_mux);
}

static ls_fetch_result_t run(const ls_fetch_req_t *q, job_t *j, uint32_t *got_out, int *http_out)
{
    ls_fetch_result_t r = LS_FETCH_OK;
    int status = -1;
    int64_t length = 0;
    uint32_t got = 0, from = 0, total = 0;
    const uint32_t cap = q->max_bytes ? q->max_bytes : LS_FETCH_MAX_DEFAULT;
    const int64_t deadline = esp_timer_get_time() + (int64_t)(q->timeout_s ? q->timeout_s : 120) * 1000000;
    const bool resume = (q->flags & LS_FETCH_RESUME) && q->sha256 && q->dest;
    bool keep = false, claimed = false, have_part = false;
    char part[120];
    FILE *f = NULL;
    esp_http_client_handle_t c = NULL;
    uint8_t *chunk = NULL;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);

    if (!q->url || (q->dest ? strncmp(q->dest, "/sdcard/", 8) || strlen(q->dest) > sizeof(part) - 6 : !q->buf || !q->cap)) {
        r = LS_FETCH_SD;
        goto out;
    }
    if (!ls_fetch_online()) { r = LS_FETCH_NO_NET; keep = true; goto out; }
    if (q->dest) {
        struct stat st;
        if (!claim(q->dest)) { r = LS_FETCH_BUSY; goto out; }
        claimed = true;
        ls_fetch_recover(q->dest);      /* so a failed fetch still leaves the last good file */
        snprintf(part, sizeof(part), "%s.part", q->dest);
        have_part = true;
        if (resume && !stat(part, &st) && st.st_size > 0 && (!q->bytes || (uint32_t)st.st_size < q->bytes)) from = (uint32_t)st.st_size;
        else { remove(part); make_dirs(part); }
    }
    c = ls_ota_open_from(q->url, q->token, from, deadline, &length, &status);
    if (!c && q->fallback && status != -3 && status != -2)
        c = ls_ota_open_from(q->fallback, q->token, from, deadline, &length, &status);
    if (!c) {
        r = status == -3 ? LS_FETCH_TIMEOUT : status < 0 ? LS_FETCH_UNREACHABLE : LS_FETCH_HTTP;
        keep = status < 0;
        goto out;
    }
    if (status == 200) from = 0;    /* no Range support there: the whole body again */
    total = length > 0 ? from + (uint32_t)length : q->bytes;
    if ((q->bytes && total != q->bytes) || total > cap || (!q->dest && total >= q->cap)) { r = LS_FETCH_SIZE; goto out; }
    chunk = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (q->dest) f = fopen(part, from ? "ab" : "wb");
    if (!chunk || (q->dest && !f) || mbedtls_sha256_starts(&sha, 0)) { r = LS_FETCH_SD; goto out; }
    got = from;
    for (int64_t last = esp_timer_get_time();;) {
        int64_t now = esp_timer_get_time();
        if (j && j->cancel) { r = LS_FETCH_CANCELLED; break; }
        if (now > deadline || now - last > STALL_US) { r = LS_FETCH_TIMEOUT; keep = true; break; }
        const int n = esp_http_client_read(c, (char *)chunk, CHUNK);
        if (n == -ESP_ERR_HTTP_EAGAIN) continue;
        if (n < 0 || (n == 0 && !esp_http_client_is_complete_data_received(c))) { r = LS_FETCH_UNREACHABLE; keep = true; break; }
        if (n == 0) break;
        last = esp_timer_get_time();
        if ((uint32_t)n > cap - got || (q->bytes && (uint32_t)n > q->bytes - got) || (!q->dest && got + (uint32_t)n >= q->cap)) {
            r = LS_FETCH_SIZE;
            break;
        }
        if (!f) memcpy(q->buf + got, chunk, (size_t)n);
        else if (fwrite(chunk, 1, (size_t)n, f) != (size_t)n) { r = LS_FETCH_SD; break; }
        if (!from) mbedtls_sha256_update(&sha, chunk, (size_t)n);
        got += (uint32_t)n;
        if (j) { j->info.got = got; j->info.total = total; }
        if (q->progress) q->progress(got, total, q->ctx);
    }
    if (r == LS_FETCH_OK && q->bytes && got != q->bytes) r = LS_FETCH_SIZE;
    if (r == LS_FETCH_OK && f && fflush(f)) r = LS_FETCH_SD;
    if (f && from && r == LS_FETCH_OK) {    /* a joined file is hashed whole, off the card */
        fclose(f);
        f = fopen(part, "rb");
        if (!f) r = LS_FETCH_SD;
    }
    if (r == LS_FETCH_OK && q->sha256) {
        uint8_t d[32];
        const bool ok = from ? hash_file(f, got, d) : !mbedtls_sha256_finish(&sha, d);
        if (!ok || memcmp(d, q->sha256, 32)) r = LS_FETCH_HASH;
    }
    if (f && fclose(f) && r == LS_FETCH_OK) r = LS_FETCH_SD;
    f = NULL;
    if (r == LS_FETCH_OK && (q->flags & LS_FETCH_SIGNED) &&
        !(q->dest ? ls_fetch_file_signed(part, NULL) : sig_ok(NULL, q->buf, got, NULL))) r = LS_FETCH_SIG;
    if (r == LS_FETCH_OK && q->dest && !publish(part, q->dest)) r = LS_FETCH_SD;
    if (r == LS_FETCH_OK && !q->dest) q->buf[got] = 0;
out:
    if (f) fclose(f);
    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
    mbedtls_sha256_free(&sha);
    heap_caps_free(chunk);
    if (have_part && r != LS_FETCH_OK && !(keep && resume)) remove(part);   /* a card error too: no dead partial on a full card */
    if (claimed) release(q->dest);
    if (got_out) *got_out = got;
    if (http_out) *http_out = status;
    return r;
}

ls_fetch_result_t ls_fetch_run(const ls_fetch_req_t *req, uint32_t *got, int *http)
{
    return req ? run(req, NULL, got, http) : LS_FETCH_SD;
}

static void task(void *arg)
{
    (void)arg;
    for (;;) {
        job_t *j = NULL;
        portENTER_CRITICAL(&s_mux);
        for (int i = 0; i < LS_FETCH_JOBS; i++) {
            job_t *k = &s_jobs[i];
            if (k->used && k->info.state == LS_FETCH_QUEUED && (!j || k->info.id < j->info.id)) j = k;
        }
        portEXIT_CRITICAL(&s_mux);
        if (!j) { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); continue; }
        const uint8_t fl = j->q.flags;
        const int64_t until = esp_timer_get_time() + (int64_t)(j->q.timeout_s ? j->q.timeout_s : 120) * 1000000;
        ls_fetch_result_t r = LS_FETCH_OK;
        while (!j->cancel) {
            const bool net = ls_fetch_online();
            const bool quiet = !(fl & LS_FETCH_BACKGROUND) ||
                (!ls_ota_rf_busy() && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= LS_OTA_BG_MIN_INTERNAL &&
                 heap_caps_get_free_size(MALLOC_CAP_DMA) >= LS_OTA_BG_MIN_DMA);
            if (net && quiet) break;
            if ((!net && !(fl & LS_FETCH_WAIT_NET)) || esp_timer_get_time() > until) { r = net ? LS_FETCH_BUSY : LS_FETCH_NO_NET; break; }
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        if (j->cancel) r = LS_FETCH_CANCELLED;
        if (r == LS_FETCH_OK) {
            j->info.state = LS_FETCH_RUNNING;
            r = run(&j->q, j, NULL, &j->info.http);
        }
        j->info.state = r;
        if (j->q.done) j->q.done(j->info.id, r, j->q.ctx);
        j->used = false;    /* the info stays readable until the slot is taken again */
    }
}

int ls_fetch_start(const ls_fetch_req_t *q)
{
    if (!q || !q->url || strlen(q->url) >= sizeof(s_jobs[0].url) ||
        (q->fallback && strlen(q->fallback) >= sizeof(s_jobs[0].fallback)) ||
        (q->dest && strlen(q->dest) >= sizeof(s_jobs[0].dest))) return 0;
    job_t *j = NULL;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < LS_FETCH_JOBS && !j; i++)
        if (!s_jobs[i].used) { j = &s_jobs[i]; j->used = true; j->info.state = LS_FETCH_RUNNING; j->info.id = 0; }
    const int id = j ? ++s_next_id : 0;
    static bool made;
    const bool make = j && !made;
    if (make) made = true;
    portEXIT_CRITICAL(&s_mux);
    if (!j) return 0;
    if (make && xTaskCreatePinnedToCoreWithCaps(task, "ls_fetch", STACK, NULL, 2, &s_task, tskNO_AFFINITY,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        made = false;
        j->used = false;
        return 0;
    }
    j->q = *q;
    j->q.url = strcpy(j->url, q->url);
    j->q.fallback = q->fallback ? strcpy(j->fallback, q->fallback) : NULL;
    j->q.dest = q->dest ? strcpy(j->dest, q->dest) : NULL;
    j->q.sha256 = q->sha256 ? memcpy(j->sha, q->sha256, 32) : NULL;
    j->q.token = NULL;
    j->cancel = false;
    memset(&j->info, 0, sizeof(j->info));
    const char *base = q->dest ? strrchr(q->dest, '/') : NULL;
    snprintf(j->info.name, sizeof(j->info.name), "%s", base ? base + 1 : "memory");
    j->info.id = id;
    j->info.state = LS_FETCH_QUEUED;
    if (s_task) xTaskNotifyGive(s_task);
    return id;
}

bool ls_fetch_info(int id, ls_fetch_info_t *out)
{
    for (int i = 0; i < LS_FETCH_JOBS; i++)
        if (id > 0 && s_jobs[i].info.id == id) { if (out) *out = s_jobs[i].info; return true; }
    return false;
}

bool ls_fetch_active(ls_fetch_info_t *out, unsigned *queued)
{
    bool any = false;
    unsigned n = 0;
    for (int i = 0; i < LS_FETCH_JOBS; i++) {
        if (!s_jobs[i].used) continue;
        if (s_jobs[i].info.state == LS_FETCH_QUEUED) n++;
        else if (s_jobs[i].info.state == LS_FETCH_RUNNING && s_jobs[i].info.id) { any = true; if (out) *out = s_jobs[i].info; }
    }
    if (queued) *queued = n;
    return any;
}

void ls_fetch_cancel(int id)
{
    for (int i = 0; i < LS_FETCH_JOBS; i++)
        if (s_jobs[i].used && (!id || s_jobs[i].info.id == id)) s_jobs[i].cancel = true;
}

static int fetch_cmd(int argc, char **argv)
{
    if (argc >= 3) {
        uint8_t d[32];
        const char *h = argc > 3 ? argv[3] : "";
        ls_fetch_req_t q = { .url = argv[1], .dest = argv[2], .flags = LS_FETCH_RESUME };
        if (unhex(&h, d, 32) == 32) q.sha256 = d;
        const int id = ls_fetch_start(&q);
        LS_PRINTF(t_ls_fetch_job, "fetch: job %d\n", id);
        return !id;
    }
    if (argc == 2) { ls_fetch_cancel(0); return 0; }
    LS_PRINTF(t_ls_fetch_online, "fetch: network %s\n", ls_fetch_online() ? "up" : "down");
    for (int i = 0; i < LS_FETCH_JOBS; i++) {
        const ls_fetch_info_t *n = &s_jobs[i].info;
        if (n->id) LS_PRINTF(t_ls_fetch_row, "%d %s: %s %u/%u http %d\n", n->id, n->name, ls_fetch_result_text(n->state),
                             (unsigned)n->got, (unsigned)n->total, n->http);
    }
    return 0;
}

void ls_fetch_console_register(void)
{
    static const esp_console_cmd_t c = {
        .command = "fetch",
        .help = LS_TXT(con_ls_fetch_help, "Downloader: bare 'fetch' lists jobs; 'fetch <https url> </sdcard/path> [sha256]' queues one "
                       "(resumed when a sha256 is given); 'fetch cancel' stops them all."),
        .hint = LS_TXT(con_ls_fetch_hint, "[<url> <path> [sha256]|cancel]"),
        .func = fetch_cmd,
    };
    esp_console_cmd_register(&c);
}
