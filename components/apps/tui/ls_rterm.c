/* See ls_rterm.h. */

#include "ls_rterm.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "ls_vt.h"
#include "ls_theme.h"
#include "ls_tui.h"

#define TERM_PORT      2323
#define TERM_CELLS_MAX 8192           /* the largest grid any font and rotation gives, with room */
#define TERM_FRAME_US  50000          /* 20 frames a second at most */
#define SERIAL_IDLE_US 10000000       /* a serial session with no input or ping for this long ends */
#define TERM_AHEAD     2              /* updates the client may not yet have drawn */
#define TERM_ACK_US    500000         /* an answer this late was lost: stop waiting for it */
#define AUTH_US        60000000
#define NVS_NS         "lsterm"

typedef struct {
    bool on, need_enter;
    int cols, rows;
    ls_vt_screen scr;
    ls_vt_cell *shown;
    ls_vt_input in;
    int64_t last_in_us;
    int64_t start_us;                 /* no frames before this: the PC is still changing rate */
    /* Flow control, when the client asks for it ('term serial <baud> ack'):
       each screen's update ends in an empty frame, the client answers
       'term ack' once its terminal has drawn it, and no more than
       TERM_AHEAD go unanswered. A terminal slower than the link would
       otherwise fall further behind the longer a busy screen runs. */
    bool acks;
    int unacked;
    int64_t ack_wait_us;              /* when the oldest unanswered one went */
} session_t;

static void (*s_key)(int, char);
static void (*s_tap)(int, int);
static void (*s_ser_write)(const char *, size_t);
static void (*s_ser_wait)(void);
static void (*s_ser_baud)(int);

static SemaphoreHandle_t s_mu;
static TaskHandle_t s_task;
static session_t s_ser, s_net;
static volatile bool s_want;
/* The snapshot handoff: release when one side is done with the buffers, acquire
   before the other touches them (two cores, RISC-V's weak ordering). */
static bool s_ready;
static bool s_input_new;              /* a key or tap went to the UI since the last capture */

/* The snapshot, written by the UI task while !s_ready and read by the worker while s_ready. */
static tui_cell *s_snap;
static uint8_t *s_glass;
static size_t s_glass_n;
static uint32_t *s_top, *s_bot;
static ls_vt_cell *s_now;
static uint16_t s_pal[16];
static int s_cols, s_rows;
static tui_rect s_img;
static int64_t s_last_cap;
static char *s_frame;                 /* one serial frame being built */

/* Network. Only the worker touches the sockets. */
static volatile bool s_net_enabled;
static char s_pass[24];
static int s_listen = -1, s_client = -1;
static bool s_authed;
static int s_tries, s_iac;
static char s_line[32];
static int s_line_n;
static int64_t s_client_since, s_listen_retry;
static uint32_t s_client_ip;

static vprintf_like_t s_prev_vprintf;

void ls_rterm_set_input(void (*key)(int k, char ch), void (*tap)(int col, int row))
{
    s_key = key;
    s_tap = tap;
}

void ls_rterm_set_serial(void (*write)(const char *, size_t), void (*wait_tx)(void), void (*set_baud)(int))
{
    s_ser_write = write;
    s_ser_wait = wait_tx;
    s_ser_baud = set_baud;
}

bool ls_rterm_active(void) { return s_want; }

static void update_want(void) { s_want = s_ser.on || s_net.on; }

/* --- the frame --------------------------------------------------------- */

void ls_rterm_capture(void)
{
    if (!s_want || __atomic_load_n(&s_ready, __ATOMIC_ACQUIRE) || !s_snap) return;
    const int64_t now = esp_timer_get_time();
    /* The rate cap is for screens that change by themselves; the frame
       answering a key or a click goes at once. */
    const bool answer = __atomic_exchange_n(&s_input_new, false, __ATOMIC_ACQ_REL);
    if (!answer && now - s_last_cap < TERM_FRAME_US) return;
    tui_surface *sf = ls_tui_surface();
    if (!sf || !sf->front || (size_t)sf->w * sf->h > TERM_CELLS_MAX) return;
    s_cols = sf->w;
    s_rows = sf->h;
    memcpy(s_snap, sf->front, (size_t)s_cols * s_rows * sizeof(tui_cell));
    size_t gn = 0;
    const uint8_t *g = ls_tui_glass_now(&gn);
    s_glass_n = g && gn <= (TERM_CELLS_MAX + 7) / 8 ? gn : 0;
    if (s_glass_n) memcpy(s_glass, g, s_glass_n);
    tui_rect r;
    const uint16_t *src = NULL;
    int w = 0, h = 0, x0 = 0, y0 = 0;
    uint32_t serial = 0;
    s_img.w = 0;
    if (ls_tui_image_now(&r, &src, &w, &h, &serial) && src && r.x >= 0 && r.y >= 0 &&
        r.x + r.w <= s_cols && r.y + r.h <= s_rows) {
        ls_tui_image_ring_offset(&x0, &y0);
        ls_vt_sample_image(r, s_cols, src, w, h, x0, y0, s_top, s_bot);
        s_img = r;
    }
    const ls_tui_theme_t *th = ls_tui_theme_effective(ls_tui_get_theme(), ls_tui_daylight());
    if (th) memcpy(s_pal, th->palette, sizeof(s_pal));
    s_last_cap = now;
    __atomic_store_n(&s_ready, true, __ATOMIC_RELEASE);
    if (s_task) xTaskNotifyGive(s_task);
}

/* --- output ------------------------------------------------------------ */

static uint16_t crc16(const char *p, size_t n)
{
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= (uint16_t)((uint8_t)*p++ << 8);
        for (int i = 0; i < 8; i++) c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1);
    }
    return c;
}

/* Not snprintf: this runs on the console task too, and printf's stack is
   most of what that task has left. */
static void hex4(char *p, unsigned v)
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 3; i >= 0; i--, v >>= 4) p[i] = digits[v & 15];
}

/* Serial frames: 0x1E, four hex digits of length, the bytes, 0x1F, four hex
   digits of CRC-16. Whatever else the console prints lies between frames,
   and the reader drops it. One write each, so no other task splits one. */
static void serial_out(void *ctx, const char *b, size_t n)
{
    (void)ctx;
    if (!s_ser_write || !s_frame) return;
    while (n) {
        const size_t k = n > 4000 ? 4000 : n;
        s_frame[0] = 0x1E;
        hex4(s_frame + 1, (unsigned)k);
        memcpy(s_frame + 5, b, k);
        s_frame[5 + k] = 0x1F;
        hex4(s_frame + 6 + k, crc16(b, k));
        s_ser_write(s_frame, k + 10);
        b += k;
        n -= k;
    }
}

static void net_close(void)
{
    if (s_client >= 0) {
        shutdown(s_client, SHUT_RDWR);
        close(s_client);
    }
    s_client = -1;
    s_authed = false;
    s_net.on = false;
    update_want();
}

static void net_out(void *ctx, const char *b, size_t n)
{
    (void)ctx;
    while (n && s_client >= 0) {
        const int k = send(s_client, b, n, 0);
        if (k <= 0) { net_close(); return; }
        b += k;
        n -= (size_t)k;
    }
}

static void net_say(const char *s) { net_out(NULL, s, strlen(s)); }

static void render(session_t *s, ls_vt_out_fn out)
{
    const int64_t now = esp_timer_get_time();
    if (!s->on || now < s->start_us) return;
    if (s->acks && s->unacked >= TERM_AHEAD) {
        if (now - s->ack_wait_us < TERM_ACK_US) return;   /* the next one diffs against what it has */
        s->unacked = 0;
    }
    if (s->cols != s_cols || s->rows != s_rows) {      /* first frame, or the screen turned */
        s->cols = s_cols;
        s->rows = s_rows;
        ls_vt_screen_init(&s->scr, s_cols, s_rows, s->shown);
        s->need_enter = true;
    }
    if (s->need_enter) {
        ls_vt_enter(out, NULL, s->cols, s->rows);
        s->scr.full = true;
        s->need_enter = false;
    }
    if (ls_vt_render(&s->scr, s_now, out, NULL) && s->acks) {
        static const char end[] = "\x1e" "0000" "\x1f" "ffff";   /* empty frame: CRC-16 of nothing */
        if (s_ser_write) s_ser_write(end, sizeof(end) - 1);
        if (!s->unacked++) s->ack_wait_us = now;
    }
}

/* --- input ------------------------------------------------------------- */

static void deliver(session_t *s, const ls_vt_event *ev, int n, void (*quit)(void))
{
    for (int i = 0; i < n; i++) {
        switch (ev[i].type) {
        case LS_VT_EV_KEY:
            __atomic_store_n(&s_input_new, true, __ATOMIC_RELEASE);
            if (s_key) s_key(ev[i].key, ev[i].ch);
            break;
        case LS_VT_EV_TAP:
            if (s_tap && ev[i].col >= 0 && ev[i].row >= 0 && ev[i].col < s->cols && ev[i].row < s->rows) {
                __atomic_store_n(&s_input_new, true, __ATOMIC_RELEASE);
                s_tap(ev[i].col, ev[i].row);
            }
            break;
        case LS_VT_EV_REDRAW: s->scr.full = true; break;
        case LS_VT_EV_QUIT:   if (quit) quit(); return;
        default: break;
        }
    }
}

/* --- serial ------------------------------------------------------------ */

static int term_vprintf(const char *fmt, va_list ap)
{
    /* Log lines from other tasks would sit between frames anyway; errors only. */
    return fmt[0] == 'E' && s_prev_vprintf ? s_prev_vprintf(fmt, ap) : 0;
}

static void serial_end(void)
{
    if (!s_ser.on) return;
    ls_vt_leave(serial_out, NULL);
    s_ser.on = false;
    update_want();
    if (s_ser_wait) s_ser_wait();
    if (s_ser_baud) s_ser_baud(CONFIG_ESP_CONSOLE_UART_BAUDRATE);
    if (s_prev_vprintf) esp_log_set_vprintf(s_prev_vprintf);
    s_prev_vprintf = NULL;
    printf("\nterm: serial end\n");
}

/* --- network ----------------------------------------------------------- */

static void net_listen(int64_t now)
{
    if (s_listen >= 0 || now < s_listen_retry) return;
    s_listen_retry = now + 2000000;
    /* No network up means no TCP/IP stack either, and a socket call then
       asserts inside lwIP (a board with no Wi-Fi saved). Wait for one. */
    esp_netif_t *nif = esp_netif_get_default_netif();
    if (!nif || !esp_netif_is_netif_up(nif)) return;
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return;
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(TERM_PORT),
                            .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 1)) { close(fd); return; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    s_listen = fd;
}

/* Telnet's option talk is dropped, so `telnet board 2323` and a raw socket
   both work. 1: after IAC, 2: after IAC + a verb, 3: inside a subnegotiation. */
static size_t strip_iac(uint8_t *b, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t x = b[i];
        switch (s_iac) {
        case 0: if (x == 0xFF) s_iac = 1; else b[o++] = x; break;
        case 1: s_iac = x == 0xFF ? (b[o++] = x, 0) : x == 0xFA ? 3 : (x >= 0xFB ? 2 : 0); break;
        case 2: s_iac = 0; break;
        case 3: if (x == 0xF0) s_iac = 0; break;
        }
    }
    return o;
}

static void net_quit(void)
{
    ls_vt_leave(net_out, NULL);
    net_close();
}

static void net_password(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n && s_client >= 0; i++) {
        if (b[i] == '\r' || b[i] == '\n') {
            if (!s_line_n) continue;
            s_line[s_line_n] = 0;
            /* Same time whatever the guess, so its length leaks nothing. */
            unsigned diff = (unsigned)strlen(s_line) ^ (unsigned)strlen(s_pass);
            for (size_t k = 0; k < sizeof(s_line) - 1; k++)
                diff |= (unsigned)(uint8_t)s_line[k] ^ (unsigned)(uint8_t)(k < sizeof(s_pass) ? s_pass[k] : 0);
            s_line_n = 0;
            memset(s_line, 0, sizeof(s_line));
            if (!diff) {
                s_authed = true;
                memset(&s_net.in, 0, sizeof(s_net.in));
                s_net.cols = s_net.rows = 0;
                s_net.on = true;
                s_net.last_in_us = esp_timer_get_time();
                update_want();
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (++s_tries >= 3) { net_say("\r\nno\r\n"); net_close(); return; }
            net_say("\r\nwrong password\r\npassword: ");
        } else if (b[i] >= 0x20 && b[i] < 0x7F && s_line_n < (int)sizeof(s_line) - 1) {
            s_line[s_line_n++] = (char)b[i];
        } else if ((b[i] == 0x7F || b[i] == 0x08) && s_line_n) {
            s_line_n--;
        }
    }
}

static void net_service(int64_t now)
{
    if (!s_net_enabled) {
        if (s_client >= 0) net_quit();
        if (s_listen >= 0) { close(s_listen); s_listen = -1; }
        return;
    }
    net_listen(now);
    if (s_listen >= 0) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        const int c = accept(s_listen, (struct sockaddr *)&peer, &plen);
        if (c >= 0) {
            if (s_client >= 0) {
                static const char busy[] = "LakeShark: someone else is on the terminal\r\n";
                send(c, busy, sizeof(busy) - 1, 0);
                close(c);
            } else {
                s_client = c;
                s_client_ip = peer.sin_addr.s_addr;
                const struct timeval tv = {.tv_sec = 2};
                setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                const int one = 1;
                setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                s_authed = false;
                s_tries = s_iac = s_line_n = 0;
                s_client_since = now;
                /* IAC WILL ECHO, IAC WILL SUPPRESS-GO-AHEAD: a telnet client
                   sends each key as it is typed and echoes nothing. */
                static const char hello[] = "\xff\xfb\x01\xff\xfb\x03" "LakeShark terminal\r\npassword: ";
                net_out(NULL, hello, sizeof(hello) - 1);
            }
        }
    }
    if (s_client < 0) return;
    uint8_t buf[256];
    const int n = recv(s_client, buf, sizeof(buf), MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { net_close(); return; }
    if (n > 0) {
        const size_t k = strip_iac(buf, (size_t)n);
        if (!s_authed) { net_password(buf, k); return; }
        ls_vt_event ev[32];
        const int m = ls_vt_input_feed(&s_net.in, buf, k, ev, 32);
        s_net.last_in_us = now;
        deliver(&s_net, ev, m, net_quit);
    } else if (s_authed && s_net.in.n && now - s_net.last_in_us > 40000) {
        ls_vt_event ev[2];
        deliver(&s_net, ev, ls_vt_input_idle(&s_net.in, ev, 2), net_quit);
    } else if (!s_authed && now - s_client_since > AUTH_US) {
        net_close();
    }
}

/* --- the worker -------------------------------------------------------- */

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        const int64_t now = esp_timer_get_time();
        xSemaphoreTake(s_mu, portMAX_DELAY);
        if (__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE)) {
            const ls_vt_frame f = {
                .cols = s_cols, .rows = s_rows, .cells = s_snap, .palette = s_pal,
                .glass = s_glass_n ? s_glass : NULL, .image = s_img, .img_top = s_top, .img_bot = s_bot,
            };
            ls_vt_resolve(&f, false, s_now);
            render(&s_ser, serial_out);
            render(&s_net, net_out);
            __atomic_store_n(&s_ready, false, __ATOMIC_RELEASE);
        }
        if (s_ser.on && now - s_ser.last_in_us > SERIAL_IDLE_US) serial_end();
        if (s_ser.on && s_ser.in.n && now - s_ser.last_in_us > 40000) {
            ls_vt_event ev[2];
            deliver(&s_ser, ev, ls_vt_input_idle(&s_ser.in, ev, 2), serial_end);
        }
        net_service(now);
        xSemaphoreGive(s_mu);
    }
}

/* Buffers and the task, once. All of it in PSRAM but the task's control block. */
static bool start_once(void);

/* The boot path (ls_rterm_start) and a 'term' command typed during boot can
   both get here: one of them allocates, the other waits for it. */
static bool ensure_started(void)
{
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    static int state;                 /* 0 not started, 1 starting, 2 running */
    taskENTER_CRITICAL(&mux);
    const int was = state;
    if (!was) state = 1;
    taskEXIT_CRITICAL(&mux);
    if (was == 2) return true;
    if (was == 1) {
        while (__atomic_load_n(&state, __ATOMIC_ACQUIRE) == 1) vTaskDelay(pdMS_TO_TICKS(10));
        return __atomic_load_n(&state, __ATOMIC_ACQUIRE) == 2;
    }
    const bool ok = start_once();
    __atomic_store_n(&state, ok ? 2 : 0, __ATOMIC_RELEASE);
    return ok;
}

static bool start_once(void)
{
    if (!s_mu) s_mu = xSemaphoreCreateMutex();
    const size_t cells = TERM_CELLS_MAX;
    s_snap = heap_caps_calloc(cells, sizeof(tui_cell), MALLOC_CAP_SPIRAM);
    s_glass = heap_caps_calloc((cells + 7) / 8, 1, MALLOC_CAP_SPIRAM);
    s_top = heap_caps_calloc(cells, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_bot = heap_caps_calloc(cells, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_now = heap_caps_calloc(cells, sizeof(ls_vt_cell), MALLOC_CAP_SPIRAM);
    s_ser.shown = heap_caps_calloc(cells, sizeof(ls_vt_cell), MALLOC_CAP_SPIRAM);
    s_net.shown = heap_caps_calloc(cells, sizeof(ls_vt_cell), MALLOC_CAP_SPIRAM);
    s_frame = heap_caps_malloc(4096 + 16, MALLOC_CAP_SPIRAM);
    StaticTask_t *tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_TCM);
    if (!tcb) tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    StackType_t *stack = heap_caps_malloc(6144, MALLOC_CAP_SPIRAM);
    if (s_mu && s_snap && s_glass && s_top && s_bot && s_now && s_ser.shown && s_net.shown && s_frame && tcb && stack)
        s_task = xTaskCreateStaticPinnedToCore(worker, "ls_term", 6144, NULL, 2, stack, tcb, 0);
    if (!s_task) {
        heap_caps_free(s_snap); heap_caps_free(s_glass); heap_caps_free(s_top); heap_caps_free(s_bot);
        heap_caps_free(s_now); heap_caps_free(s_ser.shown); heap_caps_free(s_net.shown);
        heap_caps_free(s_frame); heap_caps_free(tcb); heap_caps_free(stack);
        s_snap = NULL; s_glass = NULL; s_top = s_bot = NULL; s_now = NULL;
        s_ser.shown = s_net.shown = NULL; s_frame = NULL;
        return false;
    }
    return true;
}

/* --- settings ---------------------------------------------------------- */

static void load_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t on = 0;
    nvs_get_u8(h, "net", &on);
    size_t len = sizeof(s_pass);
    if (nvs_get_str(h, "pass", s_pass, &len) != ESP_OK) s_pass[0] = 0;
    nvs_close(h);
    s_net_enabled = on && s_pass[0];
}

static bool save_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_u8(h, "net", s_net_enabled ? 1 : 0) == ESP_OK &&
              nvs_set_str(h, "pass", s_pass) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void ls_rterm_start(void)
{
    load_settings();
    if (s_net_enabled && !ensure_started()) printf("term: no memory for the network terminal\n");
}

/* --- console ----------------------------------------------------------- */

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int ls_rterm_console(int argc, char **argv)
{
    const char *verb = argc >= 2 ? argv[1] : "";
    /* The serial session's own traffic first, and silent: anything printed
       would be one more line for the PC to step over. */
    if (!strcmp(verb, "in") || !strcmp(verb, "ping") || !strcmp(verb, "redraw") || !strcmp(verb, "ack")) {
        if (!s_ser.on || !s_mu) return 0;
        xSemaphoreTake(s_mu, portMAX_DELAY);
        s_ser.last_in_us = esp_timer_get_time();
        if (!strcmp(verb, "redraw")) s_ser.scr.full = true;
        if (!strcmp(verb, "ack") && s_ser.unacked > 0) {
            s_ser.unacked--;
            s_ser.ack_wait_us = s_ser.last_in_us;
        }
        if (!strcmp(verb, "in") && argc >= 3) {
            uint8_t b[128];
            size_t n = 0;
            for (const char *p = argv[2]; p[0] && p[1] && n < sizeof(b); p += 2) {
                const int hi = hexval(p[0]), lo = hexval(p[1]);
                if (hi < 0 || lo < 0) break;
                b[n++] = (uint8_t)(hi << 4 | lo);
            }
            ls_vt_event ev[32];
            const int m = ls_vt_input_feed(&s_ser.in, b, n, ev, 32);
            deliver(&s_ser, ev, m, serial_end);
        }
        xSemaphoreGive(s_mu);
        return 0;
    }
    if (!strcmp(verb, "serial")) {
        const int baud = argc >= 3 ? atoi(argv[2]) : 921600;
        if (baud < 115200 || baud > 4000000) { printf("term: serial [115200-4000000]\n"); return 1; }
        if (!s_ser_write || !s_ser_baud || !s_key) { printf("term: no TUI on this board\n"); return 1; }
        if (s_ser.on) { printf("term: serial is already on\n"); return 1; }
        if (!ensure_started()) { printf("term: no memory\n"); return 1; }
        int cols = 0, rows = 0;
        ls_tui_geometry(&cols, &rows, NULL, NULL);
        printf("term: serial begin cols %d rows %d baud %d\n", cols, rows, baud);
        fflush(stdout);
        if (s_ser_wait) s_ser_wait();
        xSemaphoreTake(s_mu, portMAX_DELAY);
        s_prev_vprintf = esp_log_set_vprintf(term_vprintf);
        s_ser_baud(baud);
        memset(&s_ser.in, 0, sizeof(s_ser.in));
        s_ser.cols = s_ser.rows = 0;
        s_ser.last_in_us = esp_timer_get_time();
        s_ser.start_us = s_ser.last_in_us + 300000;
        s_ser.acks = argc >= 4 && !strcmp(argv[3], "ack");
        s_ser.unacked = 0;
        s_ser.on = true;
        update_want();
        xSemaphoreGive(s_mu);
        return 0;
    }
    if (!strcmp(verb, "off")) {
        if (!s_mu) return 0;
        xSemaphoreTake(s_mu, portMAX_DELAY);
        serial_end();
        xSemaphoreGive(s_mu);
        return 0;
    }
    if (!strcmp(verb, "net")) {
        const char *what = argc >= 3 ? argv[2] : "";
        if (!strcmp(what, "on")) {
            if (!s_pass[0]) {
                static const char abc[] = "abcdefghjkmnpqrstuvwxyz23456789";
                for (int i = 0; i < 10; i++) s_pass[i] = abc[esp_random() % (sizeof(abc) - 1)];
                s_pass[10] = 0;
            }
            if (!ensure_started()) { printf("term: no memory\n"); return 1; }
            s_net_enabled = true;
            if (!save_settings()) printf("term: could not save the setting\n");
            printf("term: network terminal on, port %d, password %s (not encrypted: use it on your own network)\n",
                   TERM_PORT, s_pass);
            return 0;
        }
        if (!strcmp(what, "off")) {
            s_net_enabled = false;
            save_settings();
            printf("term: network terminal off\n");
            return 0;
        }
        if (!strcmp(what, "pass") && argc >= 4) {
            if (strlen(argv[3]) < 6 || strlen(argv[3]) >= sizeof(s_pass)) {
                printf("term: a password is 6 to %d characters\n", (int)sizeof(s_pass) - 1);
                return 1;
            }
            snprintf(s_pass, sizeof(s_pass), "%s", argv[3]);
            save_settings();
            printf("term: password changed\n");
            return 0;
        }
        if (!strcmp(what, "kick")) {
            s_net_enabled = false;                 /* the worker closes it; back on after */
            vTaskDelay(pdMS_TO_TICKS(100));
            load_settings();
            printf("term: disconnected\n");
            return 0;
        }
        if (!strcmp(what, "show")) { printf("term: password %s\n", s_pass[0] ? s_pass : "(none)"); return 0; }
    }
    if (verb[0]) {
        printf("term: serial [baud] | off | net on|off|pass <p>|show|kick\n");
        return 1;
    }
    const uint32_t ip = ntohl(s_client_ip);
    printf("term: serial %s, network %s on port %d, %s\n", s_ser.on ? "on" : "off",
           s_net_enabled ? "on" : "off", TERM_PORT,
           s_client < 0 ? "nobody connected" : s_authed ? "a client is watching" : "a client is at the password");
    if (s_client >= 0)
        printf("term: client %lu.%lu.%lu.%lu\n", (unsigned long)(ip >> 24), (unsigned long)(ip >> 16 & 255),
               (unsigned long)(ip >> 8 & 255), (unsigned long)(ip & 255));
    return 0;
}
