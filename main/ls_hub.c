/* ls_hub: transport, identity, the hub task and the console verbs. See core/ls_hub.h. */
#include "ls_hub.h"
#include "ls_hub_int.h"
#include "ls_fetch.h"
#include "ls_nvs_safe.h"
#include "ls_ota.h"
#include "ls_version.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_console.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

#define CHUNK 8192
#define HUB_TIMEOUT_MS 15000
/* A hub answer is never bigger than a 25 MB locker file, and a call never takes longer than these,
   however slowly the bytes come: a hostile or broken server cannot fill the card or hold the task. */
#define HUB_DL_MAX (26u * 1024u * 1024u)
#define HUB_FILE_US (10LL * 60 * 1000000)
#define HUB_MEM_US (40LL * 1000000)

typedef struct { uint8_t op; uint8_t flag; char a[110]; } job_t;

static EXT_RAM_BSS_ATTR struct { bool ok; uint8_t did[8], sec[16]; uint32_t rep_seq; char dhex[17], auth[16 * 2 + 1 + 32 + 1]; } s_id;
static EXT_RAM_BSS_ATTR ls_hub_status_t s_st;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_q;
static TaskHandle_t s_task;

/* ---- identity ---- */
static esp_err_t id_job(void *ctx)
{
    const bool save = ctx != NULL;
    nvs_handle_t h;
    uint8_t b[28];
    size_t n = sizeof(b);
    if (nvs_open("lshub", NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    if (nvs_get_blob(h, "id", b, &n) != ESP_OK || n != sizeof(b)) {
        esp_fill_random(b, 24);
        memset(b + 24, 0, 4);
        if (nvs_set_blob(h, "id", b, sizeof(b)) != ESP_OK) { nvs_close(h); return ESP_FAIL; }
    } else if (save) {
        memcpy(b + 24, &s_id.rep_seq, 4);
        nvs_set_blob(h, "id", b, sizeof(b));
    }
    nvs_commit(h);
    nvs_close(h);
    memcpy(s_id.did, b, 8);
    memcpy(s_id.sec, b + 8, 16);
    memcpy(&s_id.rep_seq, b + 24, 4);
    return ESP_OK;
}

static bool ensure_id(void)
{
    if (s_id.ok) return true;
    if (ls_nvs_run(id_job, NULL, 0) != ESP_OK) return false;
    for (int i = 0; i < 8; i++) sprintf(s_id.dhex + i * 2, "%02x", s_id.did[i]);
    strcpy(s_id.auth, s_id.dhex);
    strcat(s_id.auth, ".");
    for (int i = 0; i < 16; i++) sprintf(s_id.auth + strlen(s_id.auth), "%02x", s_id.sec[i]);
    s_id.ok = true;
    return true;
}

bool ls_hub_device_id(char out[17])
{
    if (!ensure_id()) { out[0] = 0; return false; }
    memcpy(out, s_id.dhex, 17);
    return true;
}

uint32_t hub_rep_seq(void) { return ensure_id() ? s_id.rep_seq : 0; }

void hub_rep_seq_set(uint32_t v)
{
    if (!ensure_id()) return;
    s_id.rep_seq = v;
    ls_nvs_run(id_job, &s_id, 0);
}

/* ---- status ---- */
void ls_hub_status(ls_hub_status_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_st;
    taskEXIT_CRITICAL(&s_mux);
}

void hub_set(ls_hub_state_t st, const char *what, const char *fmt, ...)
{
    char t[sizeof(s_st.text)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    taskENTER_CRITICAL(&s_mux);
    s_st.state = st;
    strlcpy(s_st.what, what, sizeof(s_st.what));
    strlcpy(s_st.text, t, sizeof(s_st.text));
    if (st == LS_HUB_BUSY) s_st.done = s_st.total = 0;
    s_st.seq++;
    taskEXIT_CRITICAL(&s_mux);
}

void hub_progress(uint32_t done, uint32_t total)
{
    s_st.done = done;
    s_st.total = total;
}

bool hub_online(void) { return ls_fetch_online(); }
bool ls_hub_online(void) { return ls_fetch_online(); }

/* ---- transport ---- */
bool hub_json_str(const char *j, const char *key, char *out, size_t n)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(j, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
    return true;
}

int hub_call(const hub_req_t *q, uint32_t *got_out)
{
    int status = -2;
    uint32_t got = 0;
    char url[200], part[96];
    FILE *f = NULL, *uf = NULL;
    uint8_t *chunk = NULL;
    esp_http_client_handle_t c = NULL;
    if (got_out) *got_out = 0;
    if (!ensure_id()) return -3;
    if (!hub_online()) return -1;
    snprintf(url, sizeof(url), LS_HUB_URL "%s", q->path);
    esp_http_client_config_t cfg = { .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = HUB_TIMEOUT_MS,
                                     .buffer_size = 1536, .buffer_size_tx = 1024 };
    c = esp_http_client_init(&cfg);
    if (!c) return -3;
    esp_http_client_set_method(c, !strcmp(q->method, "POST") ? HTTP_METHOD_POST : !strcmp(q->method, "PUT") ? HTTP_METHOD_PUT
                                  : !strcmp(q->method, "DELETE") ? HTTP_METHOD_DELETE : HTTP_METHOD_GET);
    esp_http_client_set_header(c, "X-Dev", s_id.dhex);
    char bearer[sizeof(s_id.auth) + 8];
    snprintf(bearer, sizeof(bearer), "Bearer %s", s_id.auth);
    esp_http_client_set_header(c, "Authorization", bearer);
    esp_http_client_set_header(c, "User-Agent", "LakeShark/1");
    if (q->note && *q->note) esp_http_client_set_header(c, "X-Note", q->note);
    esp_http_client_set_header(c, "Content-Type", q->ctype ? q->ctype : "application/octet-stream");
    uint32_t blen = q->blen;
    if (q->upfile) {
        uf = fopen(q->upfile, "rb");
        if (!uf) { status = -3; goto out; }
        fseek(uf, 0, SEEK_END);
        blen = (uint32_t)ftell(uf);
        fseek(uf, 0, SEEK_SET);
    }
    const bool sends = strcmp(q->method, "GET") && strcmp(q->method, "DELETE");
    if (esp_http_client_open(c, sends ? (int)blen : 0) != ESP_OK) goto out;
    if (sends && blen) {
        if (q->upfile) {
            chunk = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
            if (!chunk) { status = -3; goto out; }
            uint32_t sent = 0;
            while (sent < blen) {
                const size_t n = fread(chunk, 1, CHUNK, uf);
                if (!n || esp_http_client_write(c, (const char *)chunk, (int)n) != (int)n) goto out;
                sent += (uint32_t)n;
                hub_progress(sent, blen);
            }
        } else if (esp_http_client_write(c, q->body, (int)blen) != (int)blen) {
            goto out;
        }
    }
    esp_http_client_fetch_headers(c);
    status = esp_http_client_get_status_code(c);
    const int64_t t0 = esp_timer_get_time();
    if (q->outfile && status == 200 && esp_http_client_get_content_length(c) > (int64_t)HUB_DL_MAX) status = -2;
    if (q->outfile && status == 200) {
        snprintf(part, sizeof(part), "%s.part", q->outfile);
        f = fopen(part, "wb");
        if (!f) { status = -3; goto out; }
        chunk = chunk ? chunk : heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
        if (!chunk) { status = -3; goto out; }
        for (;;) {
            const int n = esp_http_client_read(c, (char *)chunk, CHUNK);
            if (n < 0 || (n == 0 && !esp_http_client_is_complete_data_received(c))) { status = -2; break; }
            if (n == 0) break;
            if (got + (uint32_t)n > HUB_DL_MAX || esp_timer_get_time() - t0 > HUB_FILE_US) { status = -2; break; }
            if (fwrite(chunk, 1, (size_t)n, f) != (size_t)n) { status = -3; break; }
            got += (uint32_t)n;
            hub_progress(got, (uint32_t)esp_http_client_get_content_length(c));
        }
        if (fclose(f)) status = -3;
        f = NULL;
        if (status == 200) { remove(q->outfile); if (rename(part, q->outfile)) status = -3; }
        if (status != 200) remove(part);
    } else if (q->out && q->ocap) {
        for (;;) {
            if (got + 1 >= q->ocap) break;
            if (esp_timer_get_time() - t0 > HUB_MEM_US) { status = -2; break; }
            const int n = esp_http_client_read(c, q->out + got, (int)(q->ocap - 1 - got));
            if (n < 0 || (n == 0 && !esp_http_client_is_complete_data_received(c))) { status = -2; break; }   /* cut off: not a 200 */
            if (n == 0) break;
            got += (uint32_t)n;
        }
        q->out[got] = 0;
    }
out:
    if (f) fclose(f);
    if (uf) fclose(uf);
    heap_caps_free(chunk);
    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
    if (got_out) *got_out = got;
    return status;
}

/* ---- task ---- */
static void hub_task(void *arg)
{
    (void)arg;
    for (;;) {
        job_t j;
        const uint32_t w = chat_poll_wait_ms();
        if (xQueueReceive(s_q, &j, w ? pdMS_TO_TICKS(w) : portMAX_DELAY) != pdTRUE) {
            if (hub_online() && !ls_ota_rf_busy()) chat_run(HOP_CHAT_POLL, "");
            continue;
        }
        switch ((hub_op_t)j.op) {
        case HOP_REPORT: report_run(j.a, j.flag); break;
        case HOP_PAIR: case HOP_LIST: case HOP_UP: case HOP_PULL: case HOP_UNPAIR: case HOP_DEL: case HOP_GET: locker_run((hub_op_t)j.op, j.a); break;
        case HOP_CHAT_SEND: case HOP_CHAT_POLL: chat_run((hub_op_t)j.op, j.a); break;
        default: break;
        }
    }
}

bool hub_submit(hub_op_t op, const char *a, bool flag)
{
    static const char *const names[] = { "", "report", "pair", "list", "upload", "pull", "unpair", "chat", "chat", "delete", "get" };
    if (!s_q) s_q = xQueueCreate(4, sizeof(job_t));
    if (!s_q) return false;
    if (!s_task && xTaskCreatePinnedToCoreWithCaps(hub_task, "ls_hub", 16384, NULL, 2, &s_task, tskNO_AFFINITY,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_task = NULL;
        return false;
    }
    job_t j = { .op = op, .flag = flag };
    if (a) strlcpy(j.a, a, sizeof(j.a));
    if (op != HOP_CHAT_POLL && op != HOP_NONE) hub_set(LS_HUB_BUSY, names[op], "working");
    if (xQueueSend(s_q, &j, 0) != pdTRUE) { hub_set(LS_HUB_FAIL, names[op], "busy, try again"); return false; }
    return true;
}

bool hub_wait(uint32_t timeout_ms)
{
    for (uint32_t t = 0; t < timeout_ms; t += 100) {
        ls_hub_status_t st;
        ls_hub_status(&st);
        if (st.state != LS_HUB_BUSY) return st.state == LS_HUB_OK;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

void ls_hub_wake(void) { if (s_q) { job_t j = { .op = HOP_NONE }; xQueueSend(s_q, &j, 0); } }

/* ---- console ---- */
static int run_verb(int argc, char **argv, void (*fn)(int, char **))
{
    fn(argc, argv);
    return 0;
}
static int report_cmd(int c, char **v) { return run_verb(c, v, report_console); }
static int locker_cmd(int c, char **v) { return run_verb(c, v, locker_console); }
static int chat_cmd(int c, char **v) { return run_verb(c, v, chat_console); }

void ls_hub_console_register(void)
{
    static const esp_console_cmd_t cmds[] = {
        { .command = "report", .help = "Bug report to the hub: 'report send [note]', 'report preview [gps]', 'report id'.",
          .hint = "send|preview|id", .func = report_cmd },
        { .command = "locker", .help = "Wi-Fi file locker: 'locker pair' (6-digit code for the website or lslocker.py), 'code', 'list', "
          "'up <sd path>', 'pull' (to /sdcard/inbox), 'unpair'.", .hint = "pair|code|list|up|pull|unpair", .func = locker_cmd },
        { .command = "chat", .help = "Anonymous chat: 'chat join global|here|area|<us|z6-17-26>', 'send <text>', 'read', 'who', 'leave'.",
          .hint = "join|send|read|who|leave", .func = chat_cmd },
    };
    for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) esp_console_cmd_register(&cmds[i]);
}
