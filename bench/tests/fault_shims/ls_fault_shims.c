/* Fault-injection stand-ins: storage rooted in a temp dir, an HTTP server that
   misbehaves on request, the network/OTA seams, and a task pump. */
#include "ls_fault.h"
#include <errno.h>
#include <setjmp.h>
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef _WIN32
#include <direct.h>
#endif

#undef fopen
#undef fwrite
#undef fflush
#undef fclose
#undef rename
#undef remove
#undef stat
#undef mkdir
#undef opendir
#undef unlink

lf_fs_t lf_fs;
char lf_root[260];

void lf_fs_reset(void)
{
    memset(&lf_fs, 0, sizeof(lf_fs));
    lf_fs.write_budget = -1;
}

const char *lf_real(const char *p, char *buf, size_t n)
{
    if (!strncmp(p, "/sdcard", 7)) { snprintf(buf, n, "%s%s", lf_root, p + 7); return buf; }
    return p;
}

FILE *lf_fopen(const char *p, const char *m)
{
    char b[400];
    if (lf_fs.open_fail_substr && strstr(p, lf_fs.open_fail_substr)) return NULL;
    return fopen(lf_real(p, b, sizeof(b)), m);
}
size_t lf_fwrite(const void *b, size_t s, size_t n, FILE *f)
{
    if (lf_fs.write_budget >= 0 && (long)(s * n) > lf_fs.write_budget) {
        size_t k = s ? (size_t)lf_fs.write_budget / s : 0;
        if (k) fwrite(b, s, k, f);
        lf_fs.write_budget = 0;
        return k;
    }
    if (lf_fs.write_budget >= 0) lf_fs.write_budget -= (long)(s * n);
    return fwrite(b, s, n, f);
}
int lf_fflush(FILE *f) { return lf_fs.fflush_fail ? EOF : fflush(f); }
int lf_fclose(FILE *f) { int r = fclose(f); return lf_fs.fclose_fail ? EOF : r; }
int lf_rename(const char *a, const char *b)
{
    char x[400], y[400];
    lf_fs.rename_calls++;
    if (lf_fs.rename_fail_nth && --lf_fs.rename_fail_nth == 0) return -1;
    return rename(lf_real(a, x, sizeof(x)), lf_real(b, y, sizeof(y)));
}
int lf_remove(const char *p) { char b[400]; return remove(lf_real(p, b, sizeof(b))); }
int lf_unlink(const char *p) { char b[400]; return unlink(lf_real(p, b, sizeof(b))); }
int lf_stat(const char *p, struct stat *st) { char b[400]; return stat(lf_real(p, b, sizeof(b)), st); }
int lf_mkdir(const char *p)
{
    char b[400];
    if (lf_fs.mkdir_fail) return -1;
#ifdef _WIN32
    return _mkdir(lf_real(p, b, sizeof(b)));
#else
    return mkdir(lf_real(p, b, sizeof(b)), 0775);
#endif
}
DIR *lf_opendir(const char *p) { char b[400]; return opendir(lf_real(p, b, sizeof(b))); }

/* ---- network ---- */
size_t lf_heap_free;
lf_srv_t lf_srv;
bool lf_online = true;
bool lf_rf_busy;

void lf_net_reset(void)
{
    memset(&lf_srv, 0, sizeof(lf_srv));
    lf_srv.claim_len = -2;
    lf_srv.range_ok = true;
    lf_srv.drop_after = lf_srv.end_early = lf_srv.stall_after = -1;
    lf_online = true;
    lf_rf_busy = false;
    lf_heap_free = 1u << 20;
}

struct esp_http_client { const uint8_t *body; size_t blen, pos, end; long sent; };

struct esp_http_client *ls_ota_open_from(const char *url, const char *token, uint32_t from,
                                         int64_t deadline, int64_t *length, int *status)
{
    (void)token;
    lf_srv.opens++;
    if (lf_srv.nurls < 4) snprintf(lf_srv.urls[lf_srv.nurls++], sizeof(lf_srv.urls[0]), "%s", url);
    lf_srv.last_from = (int)from;
    if (strncmp(url, "https://", 8)) { *status = -2; return NULL; }
    if (esp_timer_get_time() > deadline) { *status = -3; return NULL; }
    if (lf_srv.open_status && (!lf_srv.fail_url || strstr(url, lf_srv.fail_url))) {
        *status = lf_srv.open_status;
        return NULL;
    }
    struct esp_http_client *c = calloc(1, sizeof(*c));
    c->body = lf_srv.body; c->blen = lf_srv.body_len;
    if (lf_srv.route && !lf_srv.route(url, &c->body, &c->blen)) { *status = 404; free(c); return NULL; }
    if (from && from > c->blen) { *status = 416; free(c); return NULL; }
    if (from && lf_srv.range_ok) { *status = 206; c->pos = from; }
    else { *status = 200; c->pos = 0; }
    c->end = c->blen;
    long claim = lf_srv.claim_len == -2 ? (long)(c->blen - c->pos) : lf_srv.claim_len;
    *length = claim;
    /* a real client stops at Content-Length */
    if (claim >= 0 && c->pos + (size_t)claim < c->end) c->end = c->pos + (size_t)claim;
    return c;
}

int esp_http_client_read(esp_http_client_handle_t c, char *buf, int n)
{
    if (lf_srv.on_read) lf_srv.on_read((size_t)c->sent);
    if (lf_srv.read_us) ls_shim_time_advance(lf_srv.read_us);
    if (lf_srv.stall_after >= 0 && c->sent >= lf_srv.stall_after) { ls_shim_time_advance(1000000); return -ESP_ERR_HTTP_EAGAIN; }
    if (lf_srv.drop_after >= 0 && c->sent >= lf_srv.drop_after) return -1;
    size_t left = c->end - c->pos;
    if (lf_srv.end_early >= 0) left = (size_t)lf_srv.end_early > (size_t)c->sent ? (size_t)lf_srv.end_early - (size_t)c->sent : 0;
    if (lf_srv.drop_after >= 0 && left > (size_t)(lf_srv.drop_after - c->sent)) left = (size_t)(lf_srv.drop_after - c->sent);
    if (left > (size_t)n) left = (size_t)n;
    if (left > c->end - c->pos) left = c->end - c->pos;
    memcpy(buf, c->body + c->pos, left);
    c->pos += left; c->sent += (long)left;
    return (int)left;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) { return c->pos >= c->end; }
esp_err_t esp_http_client_close(esp_http_client_handle_t c) { (void)c; return ESP_OK; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { lf_srv.cleanups++; free(c); return ESP_OK; }

size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return lf_heap_free; }
bool ls_ota_rf_busy(void) { return lf_rf_busy; }
/* "Signed" here: the DER is the first 8 bytes of the hash. */
#include "ls_ota_core.h"
bool ls_ota_hash_signed_by_us(const uint8_t hash[32], const ls_ota_manifest_t *m)
{
    return m->sig_len == 8 && !memcmp(m->sig, hash, 8);
}
bool ls_wifi_sta_connected(void) { return lf_online; }
void ls_wifi_sta_ip(char *out, int cap) { snprintf(out, (size_t)cap, "%s", lf_online ? "10.0.0.2" : ""); }
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *k) { (void)k; return NULL; }
bool esp_netif_is_netif_up(esp_netif_t *n) { (void)n; return false; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *i) { (void)n; (void)i; return ESP_FAIL; }
esp_err_t esp_console_cmd_register(const esp_console_cmd_t *c) { (void)c; return ESP_OK; }

/* ---- tasks, run on the test's own thread ----
   A task function is captured at create time. lf_task_pump_n runs it until it
   has gone round `allow` times (each ulTaskNotifyTake that returns is one
   round) and then abandons it at the next take, as if it had blocked. A wait
   inside one task (vTaskDelay) lets the downloader task run. */
typedef struct { char name[16]; TaskFunction_t fn; jmp_buf jb; int allow, takes; } lf_task_t;
static lf_task_t s_tasks[4];
static int s_ntasks;
static lf_task_t *s_cur;
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t task, const char *name, uint32_t depth, void *arg,
                                           UBaseType_t prio, TaskHandle_t *out, BaseType_t core, UBaseType_t caps)
{
    (void)depth; (void)arg; (void)prio; (void)core; (void)caps;
    lf_task_t *t = NULL;
    for (int i = 0; i < s_ntasks; i++) if (!strcmp(s_tasks[i].name, name)) t = &s_tasks[i];
    if (!t && s_ntasks < 4) t = &s_tasks[s_ntasks++];
    if (!t) return pdFAIL;
    snprintf(t->name, sizeof(t->name), "%s", name);
    t->fn = task;
    if (out) *out = (TaskHandle_t)(uintptr_t)0x1234;
    return pdPASS;
}
BaseType_t xTaskNotifyGive(TaskHandle_t t) { (void)t; return pdPASS; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    (void)clear; (void)wait;
    if (s_cur && s_cur->takes++ >= s_cur->allow) longjmp(s_cur->jb, 1);
    return 1;
}
void lf_task_pump_n(const char *name, int allow)
{
    for (int i = 0; i < s_ntasks; i++) {
        lf_task_t *t = &s_tasks[i], *prev = s_cur;
        if (strcmp(t->name, name) || !t->fn) continue;
        t->allow = allow; t->takes = 0;
        s_cur = t;
        if (setjmp(t->jb) == 0) t->fn(NULL);
        s_cur = prev;
        return;
    }
}
void lf_task_pump(void) { lf_task_pump_n("ls_fetch", 0); }
void vTaskDelay(TickType_t t)
{
    ls_shim_time_advance((int64_t)t * 1000);
    if (s_cur && strcmp(s_cur->name, "ls_fetch")) lf_task_pump_n("ls_fetch", 0);
}
