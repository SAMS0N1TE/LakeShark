#include "ls_lap_context.h"
/* API 2.4 Wi-Fi, HTTPS and time. IO worker owns every blocking operation. */
#include "ls_lap_net.h"
#include "ls_lap_net_audio.h"
#include "ls_lap_ble_peripheral.h"
#include <ctype.h>
#include <stdlib.h>
#if LAP_DEVICE
#include "../../../../main/ls_wifi.h"
#include "core/ls_time.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#endif
static atomic_flag lock = ATOMIC_FLAG_INIT;
static unsigned serial;
static LS_LAP_POOL atomic_int p4_op_cancel_slots[LS_LAP_CONTEXTS][4], p4_http_cancel_slots[LS_LAP_CONTEXTS][2], p4_op_published_slots[LS_LAP_CONTEXTS][4], p4_http_published_slots[LS_LAP_CONTEXTS][2];
#define p4_http_cancel (p4_http_cancel_slots[ls_lap_context_index()])
#define p4_http_published (p4_http_published_slots[ls_lap_context_index()])
#define p4_op_cancel (p4_op_cancel_slots[ls_lap_context_index()])
#define p4_op_published (p4_op_published_slots[ls_lap_context_index()])

static int next_id(void) {
    serial = serial % 0x0fffffff + 1;
    return (int)serial;
}
static EXT_RAM_BSS_ATTR struct {
    int id, kind;
    unsigned epoch;
    lap_operation_t status;
    char ssid[33], pass[65];
    bool saved, on, issued;
    uint32_t started, sync_before;
    lap_wifi_ap_t aps[16];
    int count;
} p4_ops_slots[LS_LAP_CONTEXTS][4];
#define p4_ops (p4_ops_slots[ls_lap_context_index()])

static EXT_RAM_BSS_ATTR lap_wifi_status_t wifi;
static lap_time_status_t ts;
#if !LAP_DEVICE
static int sim_wifi_error, sim_http_error, sim_status = 200;
static bool sim_initialized;
static char sim_saved[4][33] = {"SIM WLAN"};
static unsigned sim_saved_count = 1;
static char sim_body[2048] = "LakeShark HTTPS fixture\n";
#endif
static int operation(int kind, const char *ssid, const char *pass, bool on) {
    int e = ls_lap_av_access(LAP_HW_WIFI, true);
    if (e)
        return e;
    char name[33] = {0}, key[65] = {0};
    if (ssid && (!ls_lap_av_string(name, sizeof(name), ssid) || !name[0]))
        return LAP_IO_EINVAL;
    if (pass && !ls_lap_av_string(key, sizeof(key), pass))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int h = LAP_IO_EBUSY;
    for (int i = 0; i < 4; i++)
        if (!p4_ops[i].id) {
            atomic_store(&p4_op_published[i], 0);
            memset(&p4_ops[i], 0, sizeof(p4_ops[i]));
            p4_ops[i].id = h = next_id() * 4 + i;
            atomic_store(&p4_op_published[i], h);
            p4_ops[i].kind = kind;
            p4_ops[i].started = ls_lap_av_now();
            p4_ops[i].epoch = atomic_load(&ls_lap_av_epoch);
            p4_ops[i].status = av_status(LAP_OPERATION_PENDING, 0);
            memcpy(p4_ops[i].ssid, name, sizeof(name));
            memcpy(p4_ops[i].pass, key, sizeof(key));
            p4_ops[i].saved = pass == NULL;
            p4_ops[i].on = on;
            break;
        }
    /* Avoid passwords remaining in the caller-side temporary. */
    volatile char *erase = key;
    for (size_t i = 0; i < sizeof(key); i++)
        erase[i] = 0;
    av_give(&lock);
    ls_lap_av_wake();
    return h;
}
int lap_wifi_scan_start(void) { return operation(1, NULL, NULL, false); }
int lap_wifi_connect(const char *ssid, const char *pass) {
    if (!ssid)
        return LAP_IO_EINVAL;
    return operation(2, ssid, pass, false);
}
int lap_wifi_disconnect(void) { return operation(3, NULL, NULL, false); }
int lap_wifi_ap_request(bool on) { return operation(4, NULL, NULL, on); }
int lap_time_sync_request(void) { return operation(5, NULL, NULL, false); }
static int op_index(int h) {
    for (int i = 0; i < 4; i++)
        if (p4_ops[i].id == h && p4_ops[i].epoch == atomic_load(&ls_lap_av_epoch) &&
            atomic_load(&p4_op_cancel[i]) != h)
            return i;
    return -1;
}
int lap_operation_status(int h, lap_operation_t *out) {
    int e = ls_lap_av_access(0, true);
    if (e)
        return e;
    if (!ls_lap_api_readable(out, sizeof(*out)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int i = op_index(h);
    if (i >= 0)
        *out = p4_ops[i].status;
    av_give(&lock);
    return i < 0 ? LAP_IO_ECLOSED : 0;
}
void lap_operation_close(int h) {
    if (!ls_lap_api_inside())
        return;
    if (h > 0)
        av_close(&p4_op_cancel[(unsigned)h % 4], &p4_op_published[(unsigned)h % 4], h);
    ls_lap_av_wake();
}
int lap_wifi_status(lap_wifi_status_t *out) {
    int e = ls_lap_av_access(LAP_HW_WIFI, true);
    if (e)
        return e;
    if (!ls_lap_api_readable(out, sizeof(*out)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    *out = wifi;
    out->size = sizeof(*out);
    out->version = 1;
    av_give(&lock);
    ls_lap_av_wake();
    return 0;
}
int lap_time_status(lap_time_status_t *out) {
    int e = ls_lap_av_access(0, false);
    if (e)
        return e;
    if (!ls_lap_api_readable(out, sizeof(*out)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
#if LAP_DEVICE
    ts.synced = ls_time_is_synced();
    ts.source = ls_time_source();
    ts.last_sync_ms = ls_time_last_sync_ms();
#endif
    *out = ts;
    out->size = sizeof(*out);
    out->version = 1;
    out->uncertainty_ms = UINT32_MAX;
    if (!out->source)
        out->last_sync_ms = UINT32_MAX;
    av_give(&lock);
    return 0;
}
int lap_wifi_scan_result(int h, int first, lap_wifi_ap_t *out, int max) {
    int e = ls_lap_av_access(LAP_HW_WIFI, true);
    if (e)
        return e;
    if (first < 0 || max < 0 || max > 16 || (max && !ls_lap_api_readable(out, max * sizeof(*out))))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int i = op_index(h), n = 0;
    if (i < 0)
        n = LAP_IO_ECLOSED;
    else if (p4_ops[i].kind != 1)
        n = LAP_IO_EINVAL;
    else if (p4_ops[i].status.state != LAP_OPERATION_DONE)
        n = LAP_IO_EAGAIN;
    else if (p4_ops[i].status.error)
        n = p4_ops[i].status.error;
    else if (first < p4_ops[i].count) {
        n = p4_ops[i].count - first;
        if (n > max)
            n = max;
        memcpy(out, p4_ops[i].aps + first, n * sizeof(*out));
    }
    av_give(&lock);
    return n;
}
#define HTTP_RING 8192u
static EXT_RAM_BSS_ATTR struct http {
    int id;
    unsigned epoch;
    lap_http_request_t req;
    uint8_t body[4096];
    lap_http_status_t status;
    char headers[2048];
    unsigned header_len;
    uint8_t ring[HTTP_RING];
    unsigned head, tail;
    uint32_t started;
#if LAP_DEVICE
    esp_http_client_handle_t client;
#else
    unsigned fixture_offset;
    char fixture[2048];
#endif
} p4_https_slots[LS_LAP_CONTEXTS][2];
#define p4_https (p4_https_slots[ls_lap_context_index()])

static int http_index(int h) {
    for (int i = 0; i < 2; i++)
        if (p4_https[i].id == h && p4_https[i].epoch == atomic_load(&ls_lap_av_epoch) &&
            atomic_load(&p4_http_cancel[i]) != h)
            return i;
    return -1;
}
static bool terminated(const char *s, size_t n) { return memchr(s, 0, n) != NULL; }
static bool valid_headers(const char *s) {
    while (*s) {
        const char *end = strstr(s, "\r\n"), *colon = strchr(s, ':');
        if (!end || !colon || colon == s || colon > end || end - colon < 2)
            return false;
        for (const char *p = s; p < colon; p++)
            if (!isalnum((unsigned char)*p) && *p != '-')
                return false;
        for (const char *p = colon + 1; p < end; p++)
            if ((unsigned char)*p < 32 && *p != '\t')
                return false;
        char key[64];
        size_t n = colon - s;
        if (n >= sizeof(key))
            return false;
        for (size_t i = 0; i < n; i++)
            key[i] = tolower((unsigned char)s[i]);
        key[n] = 0;
        if (!strcmp(key, "host") || !strcmp(key, "content-length") || !strcmp(key, "transfer-encoding") ||
            !strcmp(key, "connection"))
            return false;
        s = end + 2;
    }
    return true;
}
int lap_http_open(const lap_http_request_t *r) {
    int e = ls_lap_av_access(LAP_HW_WIFI, true);
    if (e)
        return e;
    if (!ls_lap_api_readable(r, sizeof(*r)))
        return LAP_IO_EINVAL;
    lap_http_request_t c = *r;
    if (c.size != sizeof(c) || c.version != 1 || c.method > LAP_HTTP_PATCH || c.reserved[0] ||
        c.reserved[1] || c.reserved[2] || c.timeout_ms < 100 || c.timeout_ms > 30000 || !c.response_limit ||
        c.response_limit > 1048576 || c.body_size > 4096 || !terminated(c.url, sizeof(c.url)) ||
        !terminated(c.headers, sizeof(c.headers)) || !valid_headers(c.headers) ||
        (c.body_size && !ls_lap_api_readable(c.body, c.body_size)))
        return LAP_IO_EINVAL;
    const char *authority = !strncmp(c.url, "https://", 8)  ? c.url + 8
                            : !strncmp(c.url, "http://", 7) ? c.url + 7
                                                            : NULL;
    if (!authority || !*authority || *authority == '/' || strchr(c.url, '@') || strchr(c.url, '#'))
        return LAP_IO_EINVAL;
    for (const char *p = c.url; *p; p++)
        if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127)
            return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int h = LAP_IO_EBUSY;
    for (int i = 0; i < 2; i++)
        if (!p4_https[i].id) {
            struct http *q = &p4_https[i];
            memset(q, 0, sizeof(*q));
            q->id = h = next_id() * 2 + i;
            atomic_store(&p4_http_published[i], h);
            q->epoch = atomic_load(&ls_lap_av_epoch);
            q->req = c;
            if (c.body_size)
                memcpy(q->body, c.body, c.body_size);
            q->req.body = NULL;
            q->status =
                (lap_http_status_t){.size = sizeof(lap_http_status_t), .version = 1, .content_length = -1};
            q->started = ls_lap_av_now();
            break;
        }
    av_give(&lock);
    ls_lap_av_wake();
    return h;
}
int lap_http_status(int h, lap_http_status_t *out) {
    int e = ls_lap_av_access(0, true);
    if (e)
        return e;
    if (!ls_lap_api_readable(out, sizeof(*out)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int i = http_index(h);
    if (i >= 0) {
        *out = p4_https[i].status;
        out->queued = p4_https[i].head - p4_https[i].tail;
    }
    av_give(&lock);
    return i < 0 ? LAP_IO_ECLOSED : 0;
}
int lap_http_read(int h, void *out, size_t cap) {
    int e = ls_lap_av_access(0, true);
    if (e)
        return e;
    if (!cap || cap > HTTP_RING || (cap && !ls_lap_api_readable(out, cap)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int i = http_index(h), n = LAP_IO_ECLOSED;
    if (i >= 0) {
        struct http *q = &p4_https[i];
        unsigned k = q->head - q->tail;
        if (k > cap)
            k = cap;
        for (unsigned j = 0; j < k; j++)
            ((uint8_t *)out)[j] = q->ring[(q->tail + j) % HTTP_RING];
        q->tail += k;
        n = k ? (int)k : q->status.state == LAP_OPERATION_DONE ? q->status.error : LAP_IO_EAGAIN;
    }
    av_give(&lock);
    ls_lap_av_wake();
    return n;
}
int lap_http_header(int h, const char *name, char *out, size_t cap) {
    char key[64];
    int e = ls_lap_av_access(0, true);
    if (e)
        return e;
    if (!ls_lap_av_string(key, sizeof(key), name) || !key[0] || !cap || cap > 2048 ||
        !ls_lap_api_readable(out, cap))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int i = http_index(h), r = LAP_IO_ECLOSED;
    if (i >= 0) {
        struct http *q = &p4_https[i];
        r = q->status.state == LAP_OPERATION_PENDING ? LAP_IO_EAGAIN : 0;
        out[0] = 0;
        for (char *p = q->headers; *p;) {
            char *end = strstr(p, "\r\n"), *colon = strchr(p, ':');
            if (!end || !colon || colon > end)
                break;
            size_t n = colon - p;
            bool match = n == strlen(key);
            for (size_t j = 0; j < n && match; j++)
                if (tolower((unsigned char)p[j]) != tolower((unsigned char)key[j]))
                    match = false;
            if (match) {
                char *v = colon + 1;
                while (v < end && *v == ' ')
                    v++;
                n = end - v;
                if (n >= cap)
                    r = LAP_IO_ELIMIT;
                else {
                    memcpy(out, v, n);
                    out[n] = 0;
                    r = n;
                }
                break;
            }
            p = end + 2;
        }
    }
    av_give(&lock);
    return r;
}
void lap_http_close(int h) {
    if (!ls_lap_api_inside())
        return;
    if (h > 0)
        av_close(&p4_http_cancel[(unsigned)h % 2], &p4_http_published[(unsigned)h % 2], h);
    ls_lap_av_wake();
}
#if LAP_DEVICE
static esp_err_t http_event(esp_http_client_event_t *event) {
    struct http *q = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER) {
        size_t a = strlen(event->header_key), b = strlen(event->header_value);
        if (a + b + 4 >= sizeof(q->headers) - q->header_len)
            q->status.error = LAP_IO_ELIMIT;
        else
            q->header_len += snprintf(q->headers + q->header_len, sizeof(q->headers) - q->header_len,
                                      "%s: %s\r\n", event->header_key, event->header_value);
    }
    return ESP_OK;
}
#endif
static void http_finish(struct http *q, int error) {
    if (error)
        q->status.error = error;
    q->status.state = LAP_OPERATION_DONE;
#if LAP_DEVICE
    if (q->client) {
        esp_http_client_cleanup(q->client);
        q->client = NULL;
    }
#endif
}
static bool network_process(void) {
    if (!av_take(&lock))
        return true;
    unsigned epoch = atomic_load(&ls_lap_av_epoch);
    bool busy = false;
#if !LAP_DEVICE
    if (!sim_initialized && (ls_lap_sys_caps() & LAP_HW_WIFI)) {
        sim_initialized = true;
        wifi.connected = 1;
        strcpy(wifi.ssid, "SIM WLAN");
        strcpy(wifi.ip, "192.0.2.2");
        wifi.rssi = -42;
        wifi.channel = 6;
    }
#endif
    wifi.size = sizeof(wifi);
    wifi.version = 1;
    if (!wifi.connected)
        wifi.rssi = -127;
#if LAP_DEVICE
    ls_wifi_link_status_t link;
    ls_wifi_link_status(&link);
    wifi.connected = link.connected;
    snprintf(wifi.ssid, sizeof(wifi.ssid), "%s", link.ssid);
    snprintf(wifi.ip, sizeof(wifi.ip), "%s", link.ip);
    wifi.saved_count = link.saved < 0 ? 0 : link.saved;
    wifi.ap_running = ls_wifi_running();
    wifi.rssi = -127;
    /* sta_info has the firmware operation gate: worker only. */
    ls_wifi_scan_ap_t ap;
    if (wifi.connected && ls_wifi_sta_info(&ap) == ESP_OK) {
        wifi.rssi = ap.rssi;
        wifi.channel = ap.channel;
    }
    ts.synced = ls_time_is_synced();
    ts.source = ls_time_source();
    ts.last_sync_ms = ls_time_last_sync_ms();
#endif
    for (int i = 0; i < 4; i++)
        if (p4_ops[i].id) {
            if (p4_ops[i].epoch != epoch || atomic_load(&p4_op_cancel[i]) == p4_ops[i].id) {
                atomic_store(&p4_op_published[i], 0);
                memset(&p4_ops[i], 0, sizeof(p4_ops[i]));
                continue;
            }
            if (p4_ops[i].status.state == LAP_OPERATION_DONE)
                continue;
            busy = true;
            int error = 0;
            if (ls_lap_av_now() - p4_ops[i].started < 40)
                continue;
#if LAP_DEVICE
            if (p4_ops[i].issued) {
                bool complete = p4_ops[i].kind == 2 ? wifi.connected && !strcmp(wifi.ssid, p4_ops[i].ssid)
                                                 : ls_time_last_sync_ms() != p4_ops[i].sync_before;
                if (complete) {
                    p4_ops[i].status.state = LAP_OPERATION_DONE;
                    continue;
                }
                if (ls_lap_av_now() - p4_ops[i].started >= 30000 ||
                    (p4_ops[i].kind == 2 && ls_wifi_sta_gave_up())) {
                    p4_ops[i].status.state = LAP_OPERATION_DONE;
                    p4_ops[i].status.error = p4_ops[i].kind == 2 ? LAP_IO_EAUTH : LAP_IO_ETIMEOUT;
                }
                continue;
            }
#endif
#if LAP_DEVICE
            esp_err_t r = ESP_OK;
            switch (p4_ops[i].kind) {
            case 1: {
                static EXT_RAM_BSS_ATTR ls_wifi_scan_ap_t aps[16];
                int n = ls_wifi_sta_scan(aps, 16);
                if (n < 0 || n > 16) {
                    error = n < 0 ? LAP_IO_EBUSY : LAP_IO_EIO;
                    break;
                }
                p4_ops[i].count = n;
                for (int j = 0; j < n; j++) {
                    lap_wifi_ap_t *a = &p4_ops[i].aps[j];
                    memset(a, 0, sizeof(*a));
                    snprintf(a->ssid, sizeof(a->ssid), "%s", aps[j].ssid);
                    a->rssi = aps[j].rssi;
                    a->channel = aps[j].channel;
                    a->secure = aps[j].secure;
                }
                break;
            }
            case 2:
                r = p4_ops[i].saved ? ls_wifi_sta_join_saved(p4_ops[i].ssid)
                                 : ls_wifi_sta_join(p4_ops[i].ssid, p4_ops[i].pass);
                break;
            case 3:
                r = ls_wifi_sta_leave();
                break;
            case 4:
                r = p4_ops[i].on ? ls_wifi_start() : ls_wifi_stop();
                break;
            case 5:
                if (!ls_wifi_sta_connected())
                    error = LAP_IO_EIO;
                else {
                    p4_ops[i].sync_before = ls_time_last_sync_ms();
                    ls_time_sntp_stop();
                    ls_time_sntp_start();
                }
                break;
            }
            if (r != ESP_OK)
                error = r == ESP_ERR_INVALID_STATE ? LAP_IO_EBUSY : LAP_IO_EIO;
#else
            error = sim_wifi_error;
            if (!error)
                switch (p4_ops[i].kind) {
                case 1:
                    p4_ops[i].count = 1;
                    snprintf(p4_ops[i].aps[0].ssid, sizeof(p4_ops[i].aps[0].ssid), "SIM WLAN");
                    p4_ops[i].aps[0].rssi = -42;
                    p4_ops[i].aps[0].channel = 6;
                    p4_ops[i].aps[0].secure = true;
                    break;
                case 2: {
                    bool known = false;
                    for (unsigned j = 0; j < sim_saved_count; j++)
                        if (!strcmp(sim_saved[j], p4_ops[i].ssid))
                            known = true;
                    if (p4_ops[i].saved && !known) {
                        error = LAP_IO_EAUTH;
                        break;
                    }
                    if (!p4_ops[i].saved && !known) {
                        unsigned j = sim_saved_count < 4 ? sim_saved_count++ : 3;
                        snprintf(sim_saved[j], sizeof(sim_saved[j]), "%s", p4_ops[i].ssid);
                    }
                    wifi.saved_count = sim_saved_count;
                    wifi.connected = 1;
                    wifi.rssi = -42;
                    wifi.channel = 6;
                    snprintf(wifi.ssid, sizeof(wifi.ssid), "%s", p4_ops[i].ssid);
                    strcpy(wifi.ip, "192.0.2.2");
                    break;
                }
                case 3:
                    wifi.connected = 0;
                    wifi.ssid[0] = wifi.ip[0] = 0;
                    wifi.rssi = -127;
                    break;
                case 4:
                    wifi.ap_running = p4_ops[i].on;
                    break;
                case 5:
                    if (!wifi.connected) {
                        error = LAP_IO_EIO;
                        break;
                    }
                    ts.synced = 1;
                    ts.source = 2;
                    ts.last_sync_ms = ls_lap_av_now();
                    break;
                }
#endif
            volatile char *p = p4_ops[i].pass;
            for (unsigned j = 0; j < sizeof(p4_ops[i].pass); j++)
                p[j] = 0;
#if LAP_DEVICE
            if (!error && (p4_ops[i].kind == 2 || p4_ops[i].kind == 5)) {
                p4_ops[i].issued = true;
                continue;
            }
#endif
            p4_ops[i].status.state = LAP_OPERATION_DONE;
            p4_ops[i].status.error = error;
            p4_ops[i].status.transferred = p4_ops[i].count;
        }
    for (int i = 0; i < 2; i++)
        if (p4_https[i].id) {
            struct http *q = &p4_https[i];
            if (q->epoch != epoch || atomic_load(&p4_http_cancel[i]) == q->id) {
                http_finish(q, 0);
                atomic_store(&p4_http_published[i], 0);
                memset(q, 0, sizeof(*q));
                continue;
            }
            if (q->status.state == LAP_OPERATION_DONE)
                continue;
            busy = true;
            if (ls_lap_av_now() - q->started >= q->req.timeout_ms) {
                http_finish(q, LAP_IO_ETIMEOUT);
                continue;
            }
#if LAP_DEVICE
            if (!q->client) {
                /* TLS transport buffers must remain internal on ESP hosted/Nano. */
                if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 24576) {
                    http_finish(q, LAP_IO_EBUSY);
                    continue;
                }
                esp_http_client_config_t cfg = {.url = q->req.url,
                                                .timeout_ms = 1000,
                                                .crt_bundle_attach = esp_crt_bundle_attach,
                                                .disable_auto_redirect = true,
                                                .event_handler = http_event,
                                                .user_data = q,
                                                .buffer_size = 1024,
                                                .buffer_size_tx = 1024};
                q->client = esp_http_client_init(&cfg);
                if (!q->client) {
                    http_finish(q, LAP_IO_EIO);
                    continue;
                }
                static const esp_http_client_method_t methods[] = {HTTP_METHOD_GET,  HTTP_METHOD_POST,
                                                                   HTTP_METHOD_PUT,  HTTP_METHOD_DELETE,
                                                                   HTTP_METHOD_HEAD, HTTP_METHOD_PATCH};
                esp_http_client_set_method(q->client, methods[q->req.method]);
                char headers[512];
                memcpy(headers, q->req.headers, sizeof(headers));
                for (char *p = headers; *p;) {
                    char *end = strstr(p, "\r\n"), *colon = strchr(p, ':');
                    *colon = 0;
                    *end = 0;
                    esp_http_client_set_header(q->client, p, colon + 1);
                    p = end + 2;
                }
                if (esp_http_client_open(q->client, q->req.body_size) != ESP_OK) {
                    http_finish(q, !strncmp(q->req.url, "https:", 6) ? LAP_IO_ETLS : LAP_IO_EIO);
                    continue;
                }
                unsigned sent = 0;
                while (sent < q->req.body_size) {
                    int n = esp_http_client_write(q->client, (char *)q->body + sent, q->req.body_size - sent);
                    if (n <= 0)
                        break;
                    sent += n;
                }
                if (sent != q->req.body_size) {
                    http_finish(q, LAP_IO_EIO);
                    continue;
                }
                int64_t length = esp_http_client_fetch_headers(q->client);
                if (length < 0) {
                    http_finish(q, LAP_IO_EIO);
                    continue;
                }
                q->status.content_length =
                    esp_http_client_is_chunked_response(q->client) || length > INT32_MAX ? -1
                                                                                         : (int32_t)length;
                q->status.status_code = esp_http_client_get_status_code(q->client);
                q->status.state = LAP_OPERATION_READY;
                if (q->status.error || length > q->req.response_limit) {
                    http_finish(q, LAP_IO_ELIMIT);
                    continue;
                }
                if (q->req.method == LAP_HTTP_HEAD) {
                    http_finish(q, 0);
                    continue;
                }
                esp_http_client_set_timeout_ms(q->client, 20);
            }
            unsigned room = HTTP_RING - (q->head - q->tail);
            if (room < 512)
                continue;
            uint8_t chunk[512];
            int n = esp_http_client_read(q->client, (char *)chunk, sizeof(chunk));
            if (n < 0) {
                http_finish(q, LAP_IO_EIO);
                continue;
            }
            if (!n) {
                if (esp_http_client_is_complete_data_received(q->client))
                    http_finish(q, 0);
                continue;
            }
#else
            if (!wifi.connected) {
                http_finish(q, LAP_IO_EIO);
                continue;
            }
            if (sim_http_error) {
                http_finish(q, sim_http_error);
                continue;
            }
            if (q->status.state == LAP_OPERATION_PENDING) {
                q->status.state = LAP_OPERATION_READY;
                q->status.status_code = sim_status;
                snprintf(q->fixture, sizeof(q->fixture), "%s", sim_body);
                q->status.content_length = strlen(q->fixture);
                strcpy(q->headers, "Content-Type: text/plain\r\nX-Simulator: fixture\r\n");
            }
            unsigned room = HTTP_RING - (q->head - q->tail);
            if (room < 512)
                continue;
            uint8_t chunk[512];
            int n = strlen(q->fixture) - q->fixture_offset;
            if (n > 128)
                n = 128;
            memcpy(chunk, q->fixture + q->fixture_offset, n);
            q->fixture_offset += n;
            if (!n || q->req.method == LAP_HTTP_HEAD) {
                http_finish(q, 0);
                continue;
            }
#endif
            if (q->status.received + (unsigned)n > q->req.response_limit) {
                http_finish(q, LAP_IO_ELIMIT);
                continue;
            }
            for (int j = 0; j < n; j++)
                q->ring[q->head++ % HTTP_RING] = chunk[j];
            q->status.received += n;
        }
    av_give(&lock);
    return busy;
}
void ls_lap_av_script(const char *c) {
#if !LAP_DEVICE
    ls_lap_sound_script(c);
    ls_lap_ble_script(c);
    ls_lap_peripheral_script(c);
    ls_lap_mdns_script(c);
    ls_lap_udp_script(c);
    if (!av_take(&lock))
        return;
    if (!strncmp(c, "wifierror=", 10))
        sim_wifi_error = atoi(c + 10);
    else if (!strncmp(c, "httperror=", 10))
        sim_http_error = atoi(c + 10);
    else if (!strncmp(c, "httpstatus=", 11))
        sim_status = atoi(c + 11);
    else if (!strncmp(c, "httpbody=", 9))
        snprintf(sim_body, sizeof(sim_body), "%s", c + 9);
    else if (!strncmp(c, "wifidown", 8)) {
        wifi.connected = 0;
        wifi.ip[0] = wifi.ssid[0] = 0;
        wifi.rssi = -127;
    }
    av_give(&lock);
#else
    (void)c;
#endif
}

/* DNS/TLS handshakes and Wi-Fi scans must not stall the mic/mixer/UDP worker.
   One lazy firmware-owned task, 8 KiB PSRAM stack; only its TCB is internal.
   Wi-Fi credential/NVS persistence retains the existing internal-stack owner.
   No application task creation API or NimBLE task is introduced. */
#if LAP_DEVICE
static TaskHandle_t network_workers[LS_LAP_CONTEXTS];
#define network_worker network_workers[ls_lap_context_index()]
static atomic_bool network_busies[LS_LAP_CONTEXTS];
#define network_busy network_busies[ls_lap_context_index()]
static atomic_bool network_deads[LS_LAP_CONTEXTS];
#define network_dead network_deads[ls_lap_context_index()]
static void network_task(void *arg) {
    ls_lap_instance_bind(arg);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(atomic_load(&network_busy) ? 20 : 1000));
        atomic_store(&network_busy, network_process());
        if(!atomic_load(&ls_lap_av_active)&&!atomic_load(&network_busy)) {
            atomic_store(&network_dead,true);
            vTaskSuspend(NULL); /* IO owner joins and frees this stack/TCB on the same core. */
        }
    }
}
#endif
bool ls_lap_wifi_http_service(void) {
#if LAP_DEVICE
    if(network_worker&&atomic_load(&network_dead)) {
        vTaskDeleteWithCaps(network_worker); network_worker=NULL;
        atomic_store(&network_dead,false);
    }
    if (!network_worker && atomic_load(&ls_lap_av_active) && ls_lap_net_allowed() &&
        (ls_lap_sys_caps() & LAP_HW_WIFI)) {
        /* Do not consume Nano's internal task stack budget. */
        if (xTaskCreatePinnedToCoreWithCaps(network_task, "lap_net", 8192, ls_lap_instance_current(), 2, &network_worker,
                                0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            if (av_take(&lock)) {
                for (int i = 0; i < 4; i++)
                    if (p4_ops[i].id) {
                        volatile char *p = p4_ops[i].pass;
                        for (unsigned j = 0; j < sizeof(p4_ops[i].pass); j++)
                            p[j] = 0;
                        p4_ops[i].status = av_status(LAP_OPERATION_DONE, LAP_IO_EBUSY);
                        if (p4_ops[i].epoch != atomic_load(&ls_lap_av_epoch) ||
                            atomic_load(&p4_op_cancel[i]) == p4_ops[i].id) {
                            atomic_store(&p4_op_published[i], 0);
                            memset(&p4_ops[i], 0, sizeof(p4_ops[i]));
                        }
                    }
                for (int i = 0; i < 2; i++)
                    if (p4_https[i].id) {
                        http_finish(&p4_https[i], LAP_IO_EBUSY);
                        if (p4_https[i].epoch != atomic_load(&ls_lap_av_epoch) ||
                            atomic_load(&p4_http_cancel[i]) == p4_https[i].id) {
                            atomic_store(&p4_http_published[i], 0);
                            memset(&p4_https[i], 0, sizeof(p4_https[i]));
                        }
                    }
                av_give(&lock);
            }
            return false;
        }
    }
    if (network_worker)
        xTaskNotifyGive(network_worker);
    return atomic_load(&network_busy);
#else
    return network_process();
#endif
}

#include "ls_lap_usage.h"
ls_lap_usage_t ls_lap_http_usage(unsigned s) {
    ls_lap_usage_t v={sizeof(p4_ops_slots[s])+sizeof(p4_https_slots[s]),0};
    for(int i=0;i<4;i++) v.handles+=atomic_load(&p4_op_published_slots[s][i])>0;
    for(int i=0;i<2;i++) v.handles+=atomic_load(&p4_http_published_slots[s][i])>0;
    return v;
}
