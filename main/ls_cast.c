/* SPDX-License-Identifier: GPL-3.0-or-later
 * 'tui cast': the panel framebuffer as JPEG over Wi-Fi.
 *
 * The serial recorder ('tui rec') sends the grid's cells and tops out near
 * 200 KB/s on the CH343, so a screen that repaints a picture every frame
 * (WARDRIVE's 3D replay is ~600 KB a frame) arrives a strip at a time.
 * This sends the finished framebuffer instead, compressed by the P4's
 * JPEG encoder, which reads it by DMA: no copy and almost no CPU. Only the
 * rows the screen redrew since the last frame are sent, as up to four
 * bands; the first frame, a turn of the screen and an idle second send
 * the whole frame.
 *
 * Wire format, TCP port 7878 on the Wi-Fi station address only:
 *   client -> board: the token printed by 'tui cast on', then '\n'
 *   board -> client: "OK <width> <height> 2\n", "NO\n" (wrong token) or
 *                    "BUSY\n" (someone else is watching)
 *   then pieces:     u32 word, u32 ms, u16 y, u16 rows, JPEG (little endian)
 *                    word = JPEG length (low 27 bits) | last << 27 | turn << 28
 *                    The JPEG is framebuffer rows y..y+rows, full width.
 *                    last: the frame is complete with this piece.
 *                    turn: how the grid sits on the native portrait
 *                    framebuffer, 0 portrait, 1 clockwise, 2 counter-clockwise.
 * At most the asked frame rate; frames are skipped, never queued.
 *
 * The tasks' stacks are in PSRAM: they never touch NVS or flash.
 */
#include "ls_cast.h"
#include "ls_panel.h"
#include "tui/ls_tui.h"

#include "driver/jpeg_encode.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAST_PORT        7878
#define CAST_LIFE_US     (10LL * 60 * 1000000)
#define CAST_OUT_BYTES   (448 * 1024)   /* each of two */
#define CAST_BUFS        2
#define CAST_PAD         128      /* before each JPEG: its header, and cache alignment */
#define CAST_HDR         12
#define CAST_STACK       6144
#define CAST_IDLE_RESEND_US 1000000
#define CAST_MAX_ROWS    2048
#define CAST_BANDS       4
#define CAST_BAND_GAP    48       /* rows: closer runs go as one band */

typedef struct { uint8_t *hdr; uint32_t n; } cast_piece_t;
typedef struct { uint8_t *buf; int fd; int count; uint32_t bytes; cast_piece_t piece[CAST_BANDS]; } cast_job_t;

static TaskHandle_t s_task, s_tx_task;
static QueueHandle_t s_tx_q, s_free_q;
static size_t s_buf_cap;
static volatile bool s_tx_failed, s_drained = true;
static volatile bool s_on;
static volatile bool s_want;
static volatile bool s_active;
static volatile bool s_busy;          /* the task is inside a session */
static volatile int s_turn;
static volatile int s_fps = 20;
static volatile int s_quality = 60;
static volatile int s_rate_kbs = 1500;
static volatile int64_t s_deadline_us;
static char s_token[9];
static uint32_t s_ip;               /* network order */
static uint32_t s_client_ip;

/* Rows redrawn since the last capture, and the ones the capture took.
   Both are written by the UI task only (present, then the hook). */
static EXT_RAM_BSS_ATTR uint8_t s_acc[CAST_MAX_ROWS / 8];
static EXT_RAM_BSS_ATTR uint8_t s_cap[CAST_MAX_ROWS / 8];

/* Status, written by the tasks. */
static uint32_t s_frames, s_refused, s_errors, s_sessions, s_full_frames;
static uint32_t s_last_size;
static float s_fps_now, s_avg_kb, s_enc_ms, s_send_ms, s_avg_rows;
static const char *s_why_off = "never started";

static uint32_t wifi_ip(void)
{
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    if (nif && esp_netif_is_netif_up(nif) && esp_netif_get_ip_info(nif, &ip) == ESP_OK)
        return ip.ip.addr;
    return 0;
}

bool ls_cast_active(void) { return s_active; }

/* ls_panel: rows y0..y1-1 of the framebuffer were just written back. */
void ls_cast_rows(int y0, int y1)
{
    if (!s_on) return;
    if (y0 < 0) y0 = 0;
    if (y1 > CAST_MAX_ROWS) y1 = CAST_MAX_ROWS;
    for (int y = y0; y < y1;) {
        if (!(y & 7) && y + 8 <= y1) { s_acc[y >> 3] = 0xff; y += 8; continue; }
        s_acc[y >> 3] |= (uint8_t)(1u << (y & 7));
        y++;
    }
}

static volatile uint32_t s_ui_changed;   /* presents that changed the glass */

void ls_cast_presented(bool clean)
{
    if (!clean) s_ui_changed++;
    if (!s_on) return;
    s_turn = ls_tui_native_turn();
    if (s_want && s_task) {
        memcpy(s_cap, s_acc, sizeof(s_cap));
        memset(s_acc, 0, sizeof(s_acc));
        s_want = false;
        xTaskNotifyGive(s_task);
    }
}

static bool send_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        int k = send(fd, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

/* At most s_rate_kbs, two segments at a time. The C6 drops a packet
   (esp_hosted flow control) whenever its Wi-Fi queue fills, and a 32 KB
   window sent in one burst fills it: TCP then sits out a 1.5 s
   retransmit timer. Fed in small steps the queue never fills. */
static bool send_paced(int fd, const uint8_t *p, size_t n)
{
    const size_t step = 2 * 1440;
    int64_t due = esp_timer_get_time();
    while (n) {
        const size_t k = n < step ? n : step;
        if (!send_all(fd, p, k)) return false;
        p += k;
        n -= k;
        const int rate = s_rate_kbs;
        due += (int64_t)k * 1000000 / ((int64_t)(rate > 0 ? rate : 1) * 1024);
        const int64_t now = esp_timer_get_time();
        if (due - now >= 1000) vTaskDelay(pdMS_TO_TICKS((due - now) / 1000));
        else if (now - due > 20000) due = now;   /* the link was slower: no debt */
    }
    return true;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put_u16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

/* The token line, within 3 s. */
static bool token_ok(int fd)
{
    char line[40];
    size_t n = 0;
    const int64_t until = esp_timer_get_time() + 3000000;
    while (n < sizeof(line) - 1 && esp_timer_get_time() < until) {
        char c;
        int k = recv(fd, &c, 1, 0);
        if (k <= 0) break;
        if (c == '\n' || c == '\r') { if (n) break; continue; }
        line[n++] = c;
    }
    line[n] = 0;
    if (n != 8) return false;
    unsigned diff = 0;
    for (int i = 0; i < 8; i++) diff |= (unsigned)(line[i] ^ s_token[i]);
    return diff == 0;
}

static void refuse_busy(int lfd)
{
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(lfd, &rs);
    struct timeval tv = {0, 0};
    if (select(lfd + 1, &rs, NULL, NULL, &tv) > 0) {
        int c = accept(lfd, NULL, NULL);
        if (c >= 0) { send(c, "BUSY\n", 5, 0); close(c); }
    }
}

/* The sender: frames go out from here while the next one is encoded. */
static void tx_task(void *arg)
{
    (void)arg;
    cast_job_t j;
    int64_t win_start = esp_timer_get_time();
    uint32_t win_frames = 0;
    for (;;) {
        if (xQueueReceive(s_tx_q, &j, portMAX_DELAY) != pdTRUE) continue;
        if (!s_tx_failed) {
            const int64_t t0 = esp_timer_get_time();
            bool ok = true;
            for (int i = 0; ok && i < j.count; i++)
                ok = send_paced(j.fd, j.piece[i].hdr, j.piece[i].n + CAST_HDR);
            if (!ok) {
                s_tx_failed = true;
            } else {
                const int64_t sent = esp_timer_get_time();
                s_frames++;
                win_frames++;
                s_last_size = j.bytes;
                const float a = s_frames < 8 ? 0.5f : 0.1f;
                s_avg_kb += a * ((float)j.bytes / 1024.0f - s_avg_kb);
                s_send_ms += a * ((float)(sent - t0) / 1000.0f - s_send_ms);
                if (s_frames == 1) {
                    win_start = sent;
                    win_frames = 0;
                } else if (sent - win_start >= 2000000) {
                    s_fps_now = (float)win_frames * 1e6f / (float)(sent - win_start);
                    win_start = sent;
                    win_frames = 0;
                }
            }
        }
        xQueueSend(s_free_q, &j.buf, 0);
    }
}

/* The captured rows as up to CAST_BANDS bands, 8-row aligned for the
   encoder's 8x8 blocks. Returns how many; 0 when nothing changed. */
static int bands(int h, int y0s[CAST_BANDS], int y1s[CAST_BANDS])
{
    int n = 0, y = 0;
    int a[64], b[64];
    while (y < h && n < 64) {
        while (y < h && !(s_cap[y >> 3] & (1u << (y & 7)))) y = (y & 7) || s_cap[y >> 3] ? y + 1 : y + 8;
        if (y >= h) break;
        const int start = y;
        while (y < h && (s_cap[y >> 3] & (1u << (y & 7)))) y++;
        int lo = start & ~7, hi = (y + 7) & ~7;
        if (hi > h) hi = h;
        if (n && lo - b[n - 1] < CAST_BAND_GAP) b[n - 1] = hi;
        else { a[n] = lo; b[n] = hi; n++; }
    }
    if (y < h && n == 64) b[n - 1] = h;   /* too fragmented: to the bottom */
    while (n > CAST_BANDS) {              /* join the closest pair */
        int best = 1;
        for (int i = 2; i < n; i++)
            if (a[i] - b[i - 1] < a[best] - b[best - 1]) best = i;
        b[best - 1] = b[best];
        for (int i = best; i + 1 < n; i++) { a[i] = a[i + 1]; b[i] = b[i + 1]; }
        n--;
    }
    for (int i = 0; i < n; i++) { y0s[i] = a[i]; y1s[i] = b[i]; }
    return n;
}

/* One watcher, until it goes, the cast is turned off, or time is up.
   Two buffers: one is sent while the next frame is encoded into the other;
   with both busy the frame is skipped, never queued. */
static void stream(int lfd, int fd, jpeg_encoder_handle_t enc)
{
    ls_panel_fb_t fb;
    if (!ls_panel_fb(&fb) || !fb.pixels) return;
    char hello[40];
    int hn = snprintf(hello, sizeof(hello), "OK %d %d 2\n", fb.width, fb.height);
    if (!send_all(fd, (const uint8_t *)hello, (size_t)hn)) return;

    int64_t last_send = 0, next_due = esp_timer_get_time();
    int last_turn = -9;
    s_tx_failed = false;
    s_active = true;
    while (s_on && !s_tx_failed && esp_timer_get_time() < s_deadline_us && s_ip == wifi_ip()) {
        refuse_busy(lfd);
        int64_t now = esp_timer_get_time();
        if (now < next_due) {
            int ms = (int)((next_due - now) / 1000);
            vTaskDelay(pdMS_TO_TICKS(ms > 0 ? ms : 1));
        }
        uint8_t *buf = NULL;
        if (xQueueReceive(s_free_q, &buf, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        const int fps = s_fps;
        next_due = esp_timer_get_time() + 1000000 / (fps > 0 ? fps : 1);
        /* Right after the UI task's next present, so the frame is whole
           and the UI has a draw phase ahead before it writes again. */
        s_want = true;
        const bool presented = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500)) != 0;
        if (!presented) {
            s_want = false;
            (void)ulTaskNotifyTake(pdTRUE, 0);
        }
        const int64_t cap = esp_timer_get_time();
        if (!ls_panel_fb(&fb) || !fb.pixels || fb.height > CAST_MAX_ROWS) {
            xQueueSend(s_free_q, &buf, 0);
            break;
        }
        const int turn = s_turn == 1 ? 1 : s_turn == -1 ? 2 : 0;
        int y0[CAST_BANDS], y1[CAST_BANDS];
        int nb = presented && turn == last_turn ? bands(fb.height, y0, y1) : 0;
        if (!nb) {
            if (presented && turn == last_turn && cap - last_send < CAST_IDLE_RESEND_US) {
                xQueueSend(s_free_q, &buf, 0);
                continue;
            }
            nb = 1; y0[0] = 0; y1[0] = fb.height;   /* whole frame */
            s_full_frames++;
        }
        last_turn = turn;

        cast_job_t j = { .buf = buf, .fd = fd, .count = 0, .bytes = 0 };
        size_t off = 0;
        int rows = 0;
        bool bad = false;
        for (int i = 0; i < nb && !bad; i++) {
            const int h = y1[i] - y0[i];
            jpeg_encode_cfg_t cfg = {
                .height = (uint32_t)h,
                .width = fb.width,
                .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
                /* 568 is not a multiple of 16, so 4:2:2 and 4:2:0 MCUs do not
                   fit the row; 4:4:4 uses 8x8 and fits 568x1232 exactly. */
                .sub_sample = JPEG_DOWN_SAMPLING_YUV444,
                .image_quality = (uint32_t)s_quality,
            };
            uint8_t *jpg = buf + off + CAST_PAD;
            uint32_t n = 0;
            if (off + CAST_PAD + 4096 > s_buf_cap ||
                jpeg_encoder_process(enc, &cfg, (const uint8_t *)(fb.pixels + (size_t)y0[i] * fb.width),
                                     (uint32_t)fb.width * h * 2, jpg,
                                     (uint32_t)(s_buf_cap - off - CAST_PAD), &n) != ESP_OK ||
                n == 0 || n >= (1u << 27)) {
                bad = true;
                break;
            }
            uint8_t *hdr = jpg - CAST_HDR;
            put_u32(hdr, n | (uint32_t)(i == nb - 1) << 27 | (uint32_t)turn << 28);
            put_u32(hdr + 4, (uint32_t)(cap / 1000));
            put_u16(hdr + 8, (unsigned)y0[i]);
            put_u16(hdr + 10, (unsigned)h);
            j.piece[j.count].hdr = hdr;
            j.piece[j.count].n = n;
            j.count++;
            j.bytes += n;
            rows += h;
            off += CAST_PAD + ((n + CAST_PAD - 1) & ~(size_t)(CAST_PAD - 1));
        }
        const int64_t enc_done = esp_timer_get_time();
        if (bad) {
            s_errors++;
            last_turn = -9;          /* the next one goes whole */
            xQueueSend(s_free_q, &buf, 0);
            continue;
        }
        const float a = s_frames < 8 ? 0.5f : 0.1f;
        s_enc_ms += a * ((float)(enc_done - cap) / 1000.0f - s_enc_ms);
        s_avg_rows += a * ((float)rows - s_avg_rows);
        xQueueSend(s_tx_q, &j, portMAX_DELAY);
        last_send = enc_done;
    }
    /* Both buffers back before the socket closes under the sender. */
    s_tx_failed = true;
    for (int i = 0; i < 60 && uxQueueMessagesWaiting(s_free_q) < CAST_BUFS; i++)
        vTaskDelay(pdMS_TO_TICKS(100));
    s_drained = uxQueueMessagesWaiting(s_free_q) == CAST_BUFS;
    s_active = false;
}

static int listen_on(uint32_t ip)
{
    if (!ip || wifi_ip() != ip) return -1;   /* no socket before the netif is up */
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(CAST_PORT);
    a.sin_addr.s_addr = ip;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 1)) { close(fd); return -1; }
    return fd;
}

static void session(void)
{
    s_why_off = "could not start";
    jpeg_encoder_handle_t enc = NULL;
    jpeg_encode_engine_cfg_t ecfg = { .intr_priority = 0, .timeout_ms = 300 };
    if (jpeg_new_encoder_engine(&ecfg, &enc) != ESP_OK) { s_why_off = "no JPEG encoder"; return; }
    jpeg_encode_memory_alloc_cfg_t mcfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    uint8_t *out[CAST_BUFS] = {0};
    size_t cap = 0;
    for (int i = 0; i < CAST_BUFS; i++) out[i] = (uint8_t *)jpeg_alloc_encoder_mem(CAST_OUT_BYTES, &mcfg, &cap);
    if (!out[0] || !out[1]) {
        free(out[0]); free(out[1]); jpeg_del_encoder_engine(enc);
        s_why_off = "no PSRAM for the frame buffers";
        return;
    }
    s_buf_cap = cap;
    s_drained = true;
    xQueueReset(s_tx_q);
    xQueueReset(s_free_q);
    for (int i = 0; i < CAST_BUFS; i++) xQueueSend(s_free_q, &out[i], 0);
    int lfd = listen_on(s_ip);
    if (lfd < 0) {
        free(out[0]); free(out[1]); jpeg_del_encoder_engine(enc);
        s_why_off = "could not listen on Wi-Fi";
        return;
    }
    s_why_off = "turned off";
    /* Modem sleep holds the PC's ACKs until the next beacon (pings of
       85-290 ms), which caps a TCP stream near 50 KB/s. Awake for the
       cast, then back to what it was. */
    wifi_ps_type_t ps_was = WIFI_PS_MIN_MODEM;
    const bool ps_known = esp_wifi_get_ps(&ps_was) == ESP_OK;
    const esp_err_t ps_err = ps_known && ps_was != WIFI_PS_NONE ? esp_wifi_set_ps(WIFI_PS_NONE) : ESP_OK;
    if (ps_err != ESP_OK) printf("tui cast: Wi-Fi stays in power save (%s): expect a slow stream\n", esp_err_to_name(ps_err));
    while (s_on) {
        if (esp_timer_get_time() >= s_deadline_us) { s_why_off = "10 minutes are up"; break; }
        if (wifi_ip() != s_ip) { s_why_off = "Wi-Fi went down"; break; }
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(lfd, &rs);
        struct timeval tv = {0, 250000};
        if (select(lfd + 1, &rs, NULL, NULL, &tv) <= 0) continue;
        struct sockaddr_in peer;
        socklen_t pl = sizeof(peer);
        int c = accept(lfd, (struct sockaddr *)&peer, &pl);
        if (c < 0) continue;
        const struct timeval rt = {.tv_sec = 3};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &rt, sizeof(rt));
        if (!token_ok(c)) {
            send(c, "NO\n", 3, 0);
            close(c);
            s_refused++;
            printf("tui cast: refused a client with the wrong token\n");
            continue;
        }
        s_client_ip = peer.sin_addr.s_addr;
        s_sessions++;
        stream(lfd, c, enc);
        shutdown(c, SHUT_RDWR);
        close(c);
        s_client_ip = 0;
    }
    close(lfd);
    if (ps_known && ps_was != WIFI_PS_NONE && ps_err == ESP_OK) esp_wifi_set_ps(ps_was);
    /* A sender still stuck in a send keeps its buffer: leaked, not freed under it. */
    if (s_drained) { free(out[0]); free(out[1]); }
    jpeg_del_encoder_engine(enc);
}

static void cast_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_on) continue;
        s_busy = true;
        session();
        s_on = false;
        s_want = false;
        s_active = false;
        printf("tui cast: off (%s), %lu frames sent\n", s_why_off, (unsigned long)s_frames);
        s_busy = false;
    }
}

/* The UI loop over one second: how often it comes round and how many of
   those frames changed the glass. Casting should not move these. */
static void print_ui_rate(void)
{
    const uint32_t p0 = ls_tui_present_count(), c0 = s_ui_changed;
    const int64_t t0 = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(1000));
    const float dt = (float)(esp_timer_get_time() - t0) / 1e6f;
    printf("tui cast: the screen ran %.1f frames/s, %.1f of them changed\n",
           (double)((float)(ls_tui_present_count() - p0) / dt), (double)((float)(s_ui_changed - c0) / dt));
}

static void print_status(void)
{
    if (!s_on) {
        printf("tui cast: off (%s); 'tui cast on [fps] [quality] [KB/s]' starts it\n", s_why_off);
        print_ui_rate();
        return;
    }
    const uint8_t *ip = (const uint8_t *)&s_ip;
    const int64_t left = (s_deadline_us - esp_timer_get_time()) / 1000000;
    printf("tui cast: on %u.%u.%u.%u:%d token %s, %d fps max, quality %d, %d KB/s max, off in %d:%02d\n",
           ip[0], ip[1], ip[2], ip[3], CAST_PORT, s_token, s_fps, s_quality, s_rate_kbs,
           (int)(left > 0 ? left / 60 : 0), (int)(left > 0 ? left % 60 : 0));
    if (s_active) {
        const uint8_t *c = (const uint8_t *)&s_client_ip;
        printf("tui cast: sending to %u.%u.%u.%u: %.1f fps, %.0f KB a frame (last %lu) over %.0f rows, "
               "encode %.1f ms, send %.1f ms, %lu frames (%lu whole)\n",
               c[0], c[1], c[2], c[3], (double)s_fps_now, (double)s_avg_kb, (unsigned long)(s_last_size / 1024),
               (double)s_avg_rows, (double)s_enc_ms, (double)s_send_ms, (unsigned long)s_frames,
               (unsigned long)s_full_frames);
    } else {
        printf("tui cast: waiting for a client\n");
    }
    if (s_refused || s_errors)
        printf("tui cast: %lu refused, %lu encode errors\n", (unsigned long)s_refused, (unsigned long)s_errors);
    print_ui_rate();
}

int ls_cast_cmd(int argc, char **argv)
{
    if (argc < 3) { print_status(); return 0; }
    if (!strcmp(argv[2], "off")) {
        if (!s_on) { printf("tui cast: already off\n"); return 0; }
        s_on = false;
        if (s_task) xTaskNotifyGive(s_task);
        for (int i = 0; i < 40 && s_busy; i++) vTaskDelay(pdMS_TO_TICKS(100));
        if (s_busy) printf("tui cast: turning off\n");
        return 0;
    }
    if (strcmp(argv[2], "on")) {
        printf("tui cast: 'tui cast on [fps 1..30] [quality 10..95] [KB/s 100..4000]', 'tui cast off', 'tui cast'\n");
        return 1;
    }
    int fps = argc >= 4 ? atoi(argv[3]) : 20;
    int q = argc >= 5 ? atoi(argv[4]) : 60;
    int rate = argc >= 6 ? atoi(argv[5]) : 1500;
    if (fps < 1) fps = 1;
    if (fps > 30) fps = 30;
    if (q < 10) q = 10;
    if (q > 95) q = 95;
    if (rate < 100) rate = 100;
    if (rate > 4000) rate = 4000;
    s_fps = fps;
    s_quality = q;
    s_rate_kbs = rate;
    if (s_on) {
        s_deadline_us = esp_timer_get_time() + CAST_LIFE_US;
        print_status();
        return 0;
    }
    /* A session still closing down would turn this one off as it ends. */
    for (int i = 0; i < 50 && s_busy; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (s_busy) { printf("tui cast: the last session is still closing - try again\n"); return 1; }
    const uint32_t ip = wifi_ip();
    if (!ip) { printf("tui cast: Wi-Fi is not connected - join a network first\n"); return 1; }
    ls_panel_fb_t fb;
    if (!ls_panel_fb(&fb) || !fb.pixels) { printf("tui cast: no framebuffer on this screen\n"); return 1; }
    if (!s_tx_q) {
        s_tx_q = xQueueCreate(CAST_BUFS, sizeof(cast_job_t));
        s_free_q = xQueueCreate(CAST_BUFS, sizeof(uint8_t *));
        if (!s_tx_q || !s_free_q) { printf("tui cast: no memory for the queues\n"); return 1; }
    }
    if (!s_tx_task) {
        if (xTaskCreatePinnedToCoreWithCaps(tx_task, "tui_cast_tx", 4096, NULL, 2, &s_tx_task,
                                            tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            s_tx_task = NULL;
            printf("tui cast: could not start the sender\n");
            return 1;
        }
    }
    if (!s_task) {
        if (xTaskCreatePinnedToCoreWithCaps(cast_task, "tui_cast", CAST_STACK, NULL, 2, &s_task,
                                            tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            s_task = NULL;
            printf("tui cast: could not start the task\n");
            return 1;
        }
    }
    snprintf(s_token, sizeof(s_token), "%08lx", (unsigned long)esp_random());
    s_ip = ip;
    s_frames = s_refused = s_errors = s_full_frames = 0;
    s_fps_now = s_avg_kb = s_enc_ms = s_send_ms = s_avg_rows = 0;
    memset(s_acc, 0, sizeof(s_acc));
    s_deadline_us = esp_timer_get_time() + CAST_LIFE_US;
    s_on = true;
    xTaskNotifyGive(s_task);
    print_status();
    return 0;
}
