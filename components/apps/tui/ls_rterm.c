/* See ls_rterm.h. */

#include "ls_rterm.h"
#include "ls_rterm_ssh.h"

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
#include "ls_nvs_safe.h"
#include "sdkconfig.h"

#include "ls_mdns.h"
#include "ls_vt.h"
#include "ls_ws.h"
#include "ls_theme.h"
#include "ls_tui.h"
#include "ls_tui_screen.h"
#include "ls_tui_touch.h"
#include "miniz.h"
#include "ls_textpack.h"

#define TERM_PORT      2323
#define TERM_CELLS_MAX 8192           /* the largest grid any font and rotation gives, with room */
#define TERM_FRAME_US  50000          /* 20 frames a second at most */
#define TERM_CELLS_FRAME_US 15000     /* the cell feed: every frame the screen draws */
#define TERM_STACK     16384
#define SERIAL_IDLE_US 10000000       /* a serial session with no input or ping for this long ends */
#define SIZE_WAIT_US   500000         /* how long a network client has to say its size before the first frame */
#define TERM_AHEAD     2              /* updates the client may not yet have drawn */
#define TERM_ACK_US    500000         /* an answer this late was lost: stop waiting for it */
#define AUTH_US        60000000
#define NVS_NS         "lsterm"
#define WEB_PORT       80
#define WEB_CONNS      3              /* the socket and two requests; the rest wait their turn */
#define WEB_RX         2048           /* a request's head: a phone's is under 1 KB */
#define WEB_HEAD_US    5000000        /* a connection that has not asked by now never will */
#define WEB_IDLE_US    15000000       /* a screen nothing came from for this long was put away */

typedef struct {
    bool on, need_enter;
    int cols, rows;
    ls_vt_screen scr;
    ls_vt_cell *shown;
    ls_vt_input in;
    int64_t last_in_us;
    int64_t key_us;                   /* the last 'in' bytes: acks every frame must not hold a lone Esc */
    int64_t start_us;                 /* no frames before this: the PC is still changing rate */
    /* Flow control, when the client asks for it ('term serial <baud> ack'):
       each screen's update ends in an empty frame, the client answers
       'term ack' once its terminal has drawn it, and no more than
       TERM_AHEAD go unanswered. A terminal slower than the link would
       otherwise fall further behind the longer a busy screen runs. */
    bool acks;
    bool z;                           /* 'term serial <baud> ack z': each update deflated */
    int unacked;
    int64_t ack_wait_us;              /* when the oldest unanswered one went */
    /* The client's window, when it has said (network only): one smaller
       than the grid shows part of it, ls_vt_screen_view. */
    int term_cols, term_rows;
    bool told;                        /* it gave its size itself (NAWS, SSH's pty): no cursor reports */
    bool repaint;                     /* the view moved: draw it again from the last frame */
    /* Cells instead of terminal text ('term serial <baud> ack z cells', 'term web
       ack z cells'): for a viewer that draws the board's own glyphs (web/screen.js
       with lsfont.bin). See cells_render for the records. */
    bool cells;
    uint16_t cpal[16];                /* the palette it was last sent */
    int cfont;                        /* the font it was last told, -1 none */
    /* The mouse as a finger, once the client has shown it sends releases
       (lsterm does; a bare telnet may not): a press holds the finger down,
       moving drags it (a game's stick, a slider), and a release that never
       left the cell is the tap. Until then a press is the tap. */
    bool mouse_up;
    bool m_down, m_moved;
    int m_col, m_row;
    void (*write)(const char *, size_t);   /* where its frames go: the UART, the web screen's socket */
} session_t;

static void (*s_key)(int, char);
static bool (*s_resize)(int, int);
static void (*s_tap)(int, int);
static void (*s_ser_write)(const char *, size_t);
static void (*s_ser_wait)(void);
static void (*s_ser_baud)(int);

static SemaphoreHandle_t s_mu;
static TaskHandle_t s_task;
static session_t s_ser, s_net, s_web, s_ssh;
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
static int s_font_i, s_cell_w, s_cell_h;  /* the font and cell size s_snap was drawn in */
static int s_now_cols, s_now_rows;    /* the grid s_now was resolved for; the worker's */
static uint16_t s_pal[16];
static int s_cols, s_rows;
static tui_rect s_img;
static int64_t s_last_cap;
static char *s_frame;                 /* one serial frame being built */
static void (*s_sink)(const char *, size_t);   /* where frames() sends it */
/* Deflated updates for the serial client that asks for them. A browser
   cannot change the link's rate without reopening the port, which restarts
   this board, so it stays at 115200; an update's escape codes deflate about
   tenfold. The compressor's state is large and lives in PSRAM, made once. */
static tdefl_compressor *s_z;
static char *s_zpay;
static size_t s_zpay_n, s_zin;

/* Network. Only the worker touches the sockets. */
static volatile bool s_net_enabled, s_kick;
static char s_pass[24];
static int s_listen = -1, s_client = -1;
static bool s_authed;
static int s_tries;
static ls_vt_telnet s_tn;
static char s_line[32];
static int s_line_n;
static int64_t s_client_since, s_listen_retry;
static uint32_t s_client_ip;

/* The web screen: the same framed updates as the USB screen, each frame
   one WebSocket message, for a browser on the network (web/screen.html).
   A few connections at once: the screen's socket and the page's requests,
   each answered and closed. */
typedef struct {
    int fd;
    bool ws, authed;
    int tries;
    uint32_t ip;
    int64_t since, last;
    size_t n;
    uint8_t *rx;                      /* a request's head, or WebSocket frames in */
} webconn_t;
static volatile bool s_web_enabled, s_web_kick;
static int s_web_listen = -1, s_web_viewer = -1;
static webconn_t s_wc[WEB_CONNS];
static uint8_t *s_wsout;              /* one frame out: its header and a serial frame */
static char s_token[33];              /* what a browser keeps instead of the password */
static int64_t s_web_lock, s_web_listen_retry;
static uint32_t s_web_ip;
/* lakeshark.local, while the web screen is on (ls_mdns.h). */
static int s_mdns = -1;
static uint32_t s_mdns_ip;            /* the address it answers with, network order */
static int s_mdns_say;                /* announcements still to send */
static int64_t s_mdns_next, s_mdns_retry;

extern const uint8_t web_page_start[] asm("_binary_screen_html_gz_start");
extern const uint8_t web_page_end[] asm("_binary_screen_html_gz_end");
extern const uint8_t web_icon180_start[] asm("_binary_icon_180_png_start");
extern const uint8_t web_icon180_end[] asm("_binary_icon_180_png_end");
extern const uint8_t web_icon192_start[] asm("_binary_icon_192_png_start");
extern const uint8_t web_icon192_end[] asm("_binary_icon_192_png_end");
extern const uint8_t web_icon512_start[] asm("_binary_icon_512_png_start");
extern const uint8_t web_icon512_end[] asm("_binary_icon_512_png_end");

/* SSH (ls_rterm_ssh.c). The worker renders an update into this buffer and
   hands it over with s_sshq_ready; the SSH task sends it and hands it back.
   While it is away the worker renders none, and the next diffs against
   what went: an update waits for a slow link rather than piling up. */
#define SSHQ_MAX (256 * 1024)
static char *s_sshq;
static size_t s_sshq_n, s_sshq_cap;
static bool s_sshq_over, s_sshq_ready;
static bool s_ssh_quit;

static vprintf_like_t s_prev_vprintf;

void ls_rterm_set_input(void (*key)(int k, char ch), void (*tap)(int col, int row))
{
    s_key = key;
    s_tap = tap;
}

void ls_rterm_set_resize(bool (*resize)(int cols, int rows)) { s_resize = resize; }

void ls_rterm_set_serial(void (*write)(const char *, size_t), void (*wait_tx)(void), void (*set_baud)(int))
{
    s_ser_write = write;
    s_ser_wait = wait_tx;
    s_ser_baud = set_baud;
}

bool ls_rterm_active(void) { return s_want; }

static void update_want(void) { s_want = s_ser.on || s_net.on || s_web.on || s_ssh.on; }

/* --- the frame --------------------------------------------------------- */

void ls_rterm_capture(void)
{
    if (!s_want || __atomic_load_n(&s_ready, __ATOMIC_ACQUIRE) || !s_snap) return;
    const int64_t now = esp_timer_get_time();
    /* The rate cap is for screens that change by themselves; the frame
       answering a key or a click goes at once. */
    const bool answer = __atomic_exchange_n(&s_input_new, false, __ATOMIC_ACQ_REL);
    /* Cells cost the board next to nothing to send: with only cell viewers on,
       every frame the screen draws can go (a 40 ms frame against the 50 ms cap
       sent every other one). */
    const bool cells_only = !s_net.on && !s_ssh.on && (!s_ser.on || s_ser.cells) && (!s_web.on || s_web.cells);
    if (!answer && now - s_last_cap < (cells_only ? TERM_CELLS_FRAME_US : TERM_FRAME_US)) return;
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
    s_font_i = ls_tui_font_index();
    ls_tui_geometry(NULL, NULL, &s_cell_w, &s_cell_h);
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
   and the reader drops it. One write each, so no other task splits one.
   The web screen gets the same frames, so one page reads both. */
static void frames(char start, const char *b, size_t n)
{
    if (!s_sink || !s_frame) return;
    while (n) {
        const size_t k = n > 4000 ? 4000 : n;
        s_frame[0] = start;
        hex4(s_frame + 1, (unsigned)k);
        memcpy(s_frame + 5, b, k);
        s_frame[5 + k] = 0x1F;
        hex4(s_frame + 6 + k, crc16(b, k));
        s_sink(s_frame, k + 10);
        b += k;
        n -= k;
    }
}

static void frames_out(void *ctx, const char *b, size_t n)
{
    (void)ctx;
    frames(0x1E, b, n);
}

/* Deflated: the same frames with 0x1C in front, carrying pieces of one raw
   deflate stream per update; the empty frame that ends the update says the
   stream is complete. */
static mz_bool z_put(const void *buf, int len, void *user)
{
    (void)user;
    const char *b = buf;
    while (len > 0) {
        const size_t k = (size_t)len < 4000 - s_zpay_n ? (size_t)len : 4000 - s_zpay_n;
        memcpy(s_zpay + s_zpay_n, b, k);
        s_zpay_n += k;
        b += k;
        len -= (int)k;
        if (s_zpay_n == 4000) { frames(0x1C, s_zpay, s_zpay_n); s_zpay_n = 0; }
    }
    return MZ_TRUE;
}

static void z_out(void *ctx, const char *b, size_t n)
{
    (void)ctx;
    s_zin += n;
    tdefl_compress_buffer(s_z, b, n, TDEFL_NO_FLUSH);
}

static bool z_ready(void)
{
    if (!s_z) s_z = heap_caps_malloc(sizeof(*s_z), MALLOC_CAP_SPIRAM);
    if (!s_zpay) s_zpay = heap_caps_malloc(4000, MALLOC_CAP_SPIRAM);
    return s_z && s_zpay;
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

/* --- the cell feed ---------------------------------------------------------

   Each update is a run of records, little-endian, through the same framing
   (and deflate) as the terminal text:
     'G' u16 cols, u16 rows, u8 font, u8 cell w, u8 cell h   the grid (first, and when it changes)
     'P' 16 x u16 RGB565                                     the palette (first, and when it changes)
     'C' u16 first, u16 n, n x (u16 glyph, u8 attr)          cells as the board stores them
     'X' u16 first, u16 n, n x (u16 glyph, u8 attr, u8 kind, u16 c1, u16 c2)
         kind 1: a picture cell, top and bottom colour; kind 2: a glyph on glass, ground c1
   A 'C' run carries up to two unchanged cells to save a header. */
typedef struct { uint8_t b[512]; size_t n; ls_vt_out_fn out; size_t total; } cell_sink;
static void cs_flush(cell_sink *k) { if (k->n) { k->out(NULL, (const char *)k->b, k->n); k->total += k->n; k->n = 0; } }
static void cs_put(cell_sink *k, const void *p, size_t n)
{
    const uint8_t *s = p;
    while (n) {
        if (k->n == sizeof(k->b)) cs_flush(k);
        size_t m = sizeof(k->b) - k->n < n ? sizeof(k->b) - k->n : n;
        memcpy(k->b + k->n, s, m);
        k->n += m; s += m; n -= m;
    }
}
static void cs_u8(cell_sink *k, unsigned v) { uint8_t b = (uint8_t)v; cs_put(k, &b, 1); }
static void cs_u16(cell_sink *k, unsigned v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; cs_put(k, b, 2); }
static uint16_t rgb565(uint32_t c) { return (uint16_t)((c >> 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 3 & 0x1F)); }

/* A cell as sent: glyph, attr and kind in cp; colours in fg/bg. */
static ls_vt_cell cell_key(size_t i, int c, int r)
{
    const tui_cell *cell = &s_snap[i];
    int ch = cell->ch;
    if (ch < 0) ch &= 0xFF;
    ls_vt_cell o = {(uint32_t)(uint16_t)ch | (uint32_t)cell->attr << 16, 0, 0};
    const bool in_img = s_img.w > 0 && tui_rect_contains(s_img, c, r);
    if (!in_img) return o;
    if (ch == 0xB0) { o.cp |= 1u << 24; o.fg = rgb565(s_top[i]); o.bg = rgb565(s_bot[i]); }
    else if (s_glass_n && (s_glass[i >> 3] >> (i & 7) & 1)) {
        const uint32_t a = s_top[i], b = s_bot[i];
        const uint32_t m = ((a >> 16 & 255) + (b >> 16 & 255)) / 2 << 16 | ((a >> 8 & 255) + (b >> 8 & 255)) / 2 << 8 |
                           ((a & 255) + (b & 255)) / 2;
        o.cp |= 2u << 24; o.fg = rgb565(m);
    }
    return o;
}
static bool same(const ls_vt_cell *a, const ls_vt_cell *b) { return a->cp == b->cp && a->fg == b->fg && a->bg == b->bg; }

static size_t cells_render(session_t *s, ls_vt_out_fn out)
{
    cell_sink k = {.out = out};
    const bool full = s->scr.full;
    s->scr.full = false;
    if (full || s->cfont != s_font_i) {
        cs_u8(&k, 'G'); cs_u16(&k, (unsigned)s->cols); cs_u16(&k, (unsigned)s->rows);
        cs_u8(&k, (unsigned)s_font_i); cs_u8(&k, (unsigned)s_cell_w); cs_u8(&k, (unsigned)s_cell_h);
        s->cfont = s_font_i;
    }
    if (full || memcmp(s->cpal, s_pal, sizeof(s->cpal))) {
        cs_u8(&k, 'P');
        for (int i = 0; i < 16; i++) cs_u16(&k, s_pal[i]);
        memcpy(s->cpal, s_pal, sizeof(s->cpal));
    }
    const int cols = s->cols, n = s->cols * s->rows;
    size_t cells = 0;
    for (int i = 0; i < n;) {
        ls_vt_cell key = cell_key((size_t)i, i % cols, i / cols);
        if (!full && same(&key, &s->shown[i])) { i++; continue; }
        const bool special = key.cp >> 24;
        int last = i, j = i;
        if (!special) {
            for (; j < n; j++) {
                const ls_vt_cell kj = cell_key((size_t)j, j % cols, j / cols);
                if (kj.cp >> 24) break;
                if (full || !same(&kj, &s->shown[j])) last = j;
                else if (j - last > 2) break;
            }
            const int m = last - i + 1;
            cs_u8(&k, 'C'); cs_u16(&k, (unsigned)i); cs_u16(&k, (unsigned)m);
            for (int t = i; t <= last; t++) {
                const ls_vt_cell kt = cell_key((size_t)t, t % cols, t / cols);
                cs_u16(&k, kt.cp & 0xFFFF); cs_u8(&k, kt.cp >> 16 & 0xFF);
                s->shown[t] = kt;
            }
        } else {
            for (; j < n; j++) {
                const ls_vt_cell kj = cell_key((size_t)j, j % cols, j / cols);
                if (!(kj.cp >> 24) || (!full && same(&kj, &s->shown[j]))) break;
                last = j;
            }
            const int m = last - i + 1;
            cs_u8(&k, 'X'); cs_u16(&k, (unsigned)i); cs_u16(&k, (unsigned)m);
            for (int t = i; t <= last; t++) {
                const ls_vt_cell kt = cell_key((size_t)t, t % cols, t / cols);
                cs_u16(&k, kt.cp & 0xFFFF); cs_u8(&k, kt.cp >> 16 & 0xFF); cs_u8(&k, kt.cp >> 24);
                cs_u16(&k, kt.fg); cs_u16(&k, kt.bg);
                s->shown[t] = kt;
            }
        }
        cells += (size_t)(last - i + 1);
        i = last + 1;
    }
    cs_flush(&k);
    return cells ? k.total : 0;
}

static void render(session_t *s, ls_vt_out_fn out)
{
    const int64_t now = esp_timer_get_time();
    if (!s->on || now < s->start_us || !s_now_cols) return;
    if (s->acks && s->unacked >= TERM_AHEAD) {
        if (now - s->ack_wait_us < TERM_ACK_US) return;   /* the next one diffs against what it has */
        s->unacked = 0;
    }
    s_sink = s->write;
    const bool z = s->z && z_ready();
    if (z) {
        /* Fast settings: an update is tens of kilobytes of escape codes, and
           greedy matching with few probes still takes most of it out. */
        tdefl_init(s_z, z_put, NULL, 32 | TDEFL_GREEDY_PARSING_FLAG);
        s_zpay_n = 0;
        s_zin = 0;
        out = z_out;
    }
    s->repaint = false;
    if (s->cols != s_now_cols || s->rows != s_now_rows) {   /* first frame, or the screen turned */
        s->cols = s_now_cols;
        s->rows = s_now_rows;
        ls_vt_screen_init(&s->scr, s->cols, s->rows, s->shown);
        ls_vt_screen_resize(&s->scr, s->term_cols, s->term_rows);
        s->scr.quadrants = s != &s_ser;   /* a network client's font is anyone's guess */
        s->need_enter = true;
    }
    if (s->need_enter) {
        if (!s->cells) ls_vt_enter(out, NULL, s->cols, s->rows);
        s->scr.full = true;
        s->need_enter = false;
    }
    const size_t drawn = s->cells ? cells_render(s, out) : ls_vt_render(&s->scr, s_now, out, NULL);
    if (z && s_zin) {
        tdefl_compress_buffer(s_z, NULL, 0, TDEFL_FINISH);
        if (s_zpay_n) frames(0x1C, s_zpay, s_zpay_n);
        s_zpay_n = 0;
    }
    if (drawn && s->acks) {
        static const char end[] = "\x1e" "0000" "\x1f" "ffff";   /* empty frame: CRC-16 of nothing */
        if (s->write) s->write(end, sizeof(end) - 1);
        if (!s->unacked++) s->ack_wait_us = now;
    }
}

/* --- input ------------------------------------------------------------- */

/* The client's window, from NAWS or a cursor report. The first frame waits
   for it; a change later (a phone turned, its keyboard shown) redraws. */
static void client_size(session_t *s, int cols, int rows)
{
    s->term_cols = cols;
    s->term_rows = rows;
    if (s->cols && ls_vt_screen_resize(&s->scr, cols, rows)) s->repaint = true;
    if (s->start_us > esp_timer_get_time()) s->start_us = 0;
}

/* The middle of a cell in grid px, as the remote finger. */
static void finger(int col, int row, bool down)
{
    int cols, rows, cw = 10, ch = 17;
    ls_tui_geometry(&cols, &rows, &cw, &ch);
    ls_tui_touch_remote(col, row, col * cw + cw / 2, row * ch + ch / 2, down);
}

static void deliver(session_t *s, const ls_vt_event *ev, int n, void (*quit)(void))
{
    int w = 0, h = 0, col = 0, row = 0;
    for (int i = 0; i < n; i++) {
        switch (ev[i].type) {
        case LS_VT_EV_KEY:
            ls_vt_screen_view(&s->scr, &w, &h);
            if (ev[i].wheel && s->cols && h < s->rows) {   /* the wheel moves a view that does not fit */
                ls_vt_screen_pan(&s->scr, 0, ev[i].key == LS_TK_UP ? -3 : 3);
                s->repaint = s->scr.full;
                break;
            }
            __atomic_store_n(&s_input_new, true, __ATOMIC_RELEASE);
            if (s_key) s_key(ev[i].wheel ? (ev[i].key == LS_TK_UP ? LS_TK_WHEEL_UP : LS_TK_WHEEL_DOWN) : ev[i].key, ev[i].ch);
            break;
        case LS_VT_EV_TAP:
            if (!s->cols) break;
            if (!ls_vt_screen_tap(&s->scr, ev[i].col, ev[i].row, &col, &row)) {
                s->repaint = s->scr.full;            /* the bar: the view moved, or nothing did */
                break;
            }
            if (col >= s->cols || row >= s->rows) break;
            __atomic_store_n(&s_input_new, true, __ATOMIC_RELEASE);
            if (s->mouse_up) {
                s->m_down = true;
                s->m_moved = false;
                s->m_col = col;
                s->m_row = row;
                finger(col, row, true);
            } else if (s_tap) {
                s_tap(col, row);
            }
            break;
        case LS_VT_EV_DRAG:
            if (!s->m_down) break;
            col = ev[i].col + s->scr.x0;
            row = ev[i].row + s->scr.y0;
            col = col < 0 ? 0 : col >= s->cols ? s->cols - 1 : col;
            row = row < 0 ? 0 : row >= s->rows ? s->rows - 1 : row;
            if (col != s->m_col || row != s->m_row) s->m_moved = true;
            __atomic_store_n(&s_input_new, true, __ATOMIC_RELEASE);
            finger(col, row, true);
            break;
        case LS_VT_EV_UP:
            s->mouse_up = true;
            if (!s->m_down) break;
            s->m_down = false;
            col = ev[i].col + s->scr.x0;
            row = ev[i].row + s->scr.y0;
            finger(s->m_moved ? col : s->m_col, s->m_moved ? row : s->m_row, false);
            if (!s->m_moved && s_tap) s_tap(s->m_col, s->m_row);
            __atomic_store_n(&s_input_new, true, __ATOMIC_RELEASE);
            break;
        case LS_VT_EV_SIZE:
            if (!s->told) client_size(s, ev[i].col, ev[i].row);   /* NAWS or the pty, when it came, knows better */
            break;
        case LS_VT_EV_REDRAW: s->scr.full = true; break;
        case LS_VT_EV_QUIT:   if (quit) quit(); return;
        default: break;
        }
    }
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* A watching client's own words, the same over USB and the web screen:
   'in <hex>' keys and taps, 'ack' an update drawn, 'redraw', 'ping'. */
static void session_word(session_t *s, const char *verb, const char *arg, void (*quit)(void))
{
    s->last_in_us = esp_timer_get_time();
    if (!strcmp(verb, "redraw")) s->scr.full = true;
    if (!strcmp(verb, "size") && arg) {          /* the viewer's window: a grid to fill it */
        int c = 0, r = 0;
        if (sscanf(arg, "%d %d", &c, &r) == 2 && s_resize)
            LS_PRINTF(t_ls_rterm_term_size_dx_d, "term: size %dx%d %s\n", c, r, s_resize(c, r) ? "regrid" : "kept");
    }
    if (!strcmp(verb, "ack") && s->unacked > 0) {
        s->unacked--;
        s->ack_wait_us = s->last_in_us;
    }
    if (!strcmp(verb, "in") && arg) {
        s->key_us = s->last_in_us;
        uint8_t b[128];
        size_t n = 0;
        for (const char *p = arg; p[0] && p[1] && n < sizeof(b); p += 2) {
            const int hi = hexval(p[0]), lo = hexval(p[1]);
            if (hi < 0 || lo < 0) break;
            b[n++] = (uint8_t)(hi << 4 | lo);
        }
        ls_vt_event ev[32];
        const int m = ls_vt_input_feed(&s->in, b, n, ev, 32);
        deliver(s, ev, m, quit);
    }
}

/* Same time whatever the guess, so its length leaks nothing. The secret
   is a buffer of cap bytes, zero after its end. */
static bool secret_is(const char *guess, const char *secret, size_t cap)
{
    char g[40] = {0};
    snprintf(g, sizeof(g), "%s", guess);
    unsigned diff = (unsigned)strlen(g) ^ (unsigned)strlen(secret);
    for (size_t k = 0; k < cap && k < sizeof(g); k++) diff |= (unsigned)(uint8_t)g[k] ^ (unsigned)(uint8_t)secret[k];
    return !diff;
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
    s_sink = s_ser_write;
    ls_vt_leave(frames_out, NULL);
    s_ser.on = false;
    update_want();
    if (s_ser_wait) s_ser_wait();
    if (s_ser_baud) s_ser_baud(CONFIG_ESP_CONSOLE_UART_BAUDRATE);
    if (s_prev_vprintf) esp_log_set_vprintf(s_prev_vprintf);
    s_prev_vprintf = NULL;
    printf("\nterm: serial end\n");
}

/* --- network ----------------------------------------------------------- */

static int tcp_listen(int port, int backlog)
{
    /* No network up means no TCP/IP stack either, and a socket call then
       asserts inside lwIP (a board with no Wi-Fi saved). Wait for one. */
    esp_netif_t *nif = esp_netif_get_default_netif();
    if (!nif || !esp_netif_is_netif_up(nif)) return -1;
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port),
                            .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, backlog)) { close(fd); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

/* A connection's options: a send that cannot go in 2 s means the client
   has gone, and keys and frames go as they are written. */
static void client_opts(int fd)
{
    const struct timeval tv = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static void net_listen(int64_t now)
{
    if (s_listen >= 0 || now < s_listen_retry) return;
    s_listen_retry = now + 2000000;
    s_listen = tcp_listen(TERM_PORT, 1);
}

static void net_quit(void)
{
    ls_vt_leave(net_out, NULL);
    net_close();
}

bool rterm_pass_ok(const char *guess)
{
    /* Same time whatever the guess, so its length leaks nothing. */
    const size_t gn = strnlen(guess, 64);
    unsigned diff = (unsigned)gn ^ (unsigned)strlen(s_pass);
    for (size_t k = 0; k < 64; k++)
        diff |= (unsigned)(uint8_t)(k < gn ? guess[k] : 0) ^ (unsigned)(uint8_t)(k < sizeof(s_pass) ? s_pass[k] : 0);
    return !diff && s_pass[0];
}

static void net_password(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n && s_client >= 0; i++) {
        if (b[i] == '\r' || b[i] == '\n') {
            if (!s_line_n) continue;
            s_line[s_line_n] = 0;
            const bool right = rterm_pass_ok(s_line);
            s_line_n = 0;
            memset(s_line, 0, sizeof(s_line));
            if (right) {
                s_authed = true;
                memset(&s_net.in, 0, sizeof(s_net.in));
                s_net.cols = s_net.rows = 0;
                s_net.term_cols = s_tn.sized ? s_tn.cols : 0;
                s_net.term_rows = s_tn.sized ? s_tn.rows : 0;
                s_net.told = s_tn.sized;
                s_net.repaint = false;
                s_net.last_in_us = esp_timer_get_time();
                /* A client that gave no NAWS may still answer where the cursor
                   stops; the first frame waits a moment for either. */
                s_net.start_us = s_tn.sized ? 0 : s_net.last_in_us + SIZE_WAIT_US;
                if (!s_tn.sized) {
                    s_net.in.want_size = true;
                    ls_vt_ask_size(net_out, NULL);
                }
                s_net.on = true;
                update_want();
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (++s_tries >= 3) { net_say("\r\nno\r\n"); net_close(); return; }
            net_say(LS_TEXT(t_ls_rterm_wrong_password_password, "\r\nwrong password\r\npassword: "));
        } else if (b[i] >= 0x20 && b[i] < 0x7F && s_line_n < (int)sizeof(s_line) - 1) {
            s_line[s_line_n++] = (char)b[i];
        } else if ((b[i] == 0x7F || b[i] == 0x08) && s_line_n) {
            s_line_n--;
        }
    }
}

static void net_service(int64_t now)
{
    if (s_kick) {
        s_kick = false;
        if (s_client >= 0) net_quit();
    }
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
                client_opts(c);
                s_authed = false;
                s_tries = s_line_n = 0;
                memset(&s_tn, 0, sizeof(s_tn));
                s_client_since = now;
                /* IAC WILL ECHO, IAC WILL SUPPRESS-GO-AHEAD: a telnet client
                   sends each key as it is typed and echoes nothing. IAC DO
                   NAWS: and says its window size, now and when it changes,
                   so that a phone gets a screen that fits it. */
                static const char hello[] = "\xff\xfb\x01\xff\xfb\x03" LS_VT_TELNET_DO_NAWS
                                            "LakeShark terminal\r\npassword: ";
                net_out(NULL, hello, sizeof(hello) - 1);
            }
        }
    }
    if (s_client < 0) return;
    uint8_t buf[256];
    const int n = recv(s_client, buf, sizeof(buf), MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { net_close(); return; }
    if (n > 0) {
        const size_t k = ls_vt_telnet_strip(&s_tn, buf, (size_t)n);
        if (!s_authed) { net_password(buf, k); return; }
        s_net.told = s_tn.sized;
        if (s_tn.sized && (s_tn.cols != s_net.term_cols || s_tn.rows != s_net.term_rows))
            client_size(&s_net, s_tn.cols, s_tn.rows);
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

/* --- the web screen ------------------------------------------------------ */

static bool send_all(int fd, const void *b, size_t n)
{
    const char *p = b;
    while (n) {
        const int k = send(fd, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

static void wc_close(int i)
{
    webconn_t *c = &s_wc[i];
    if (c->fd >= 0) {
        shutdown(c->fd, SHUT_RDWR);
        close(c->fd);
    }
    c->fd = -1;
    c->n = 0;
    c->ws = c->authed = false;
    if (s_web_viewer == i) {
        s_web_viewer = -1;
        s_web.on = false;
        update_want();
    }
}

static bool ws_send(int i, int op, const void *b, size_t n)
{
    if (s_wc[i].fd < 0 || n > 4096) return false;
    const size_t h = ls_ws_header(s_wsout, op, n);
    memcpy(s_wsout + h, b, n);
    if (send_all(s_wc[i].fd, s_wsout, h + n)) return true;
    wc_close(i);
    return false;
}

static void ws_say(int i, const char *text) { ws_send(i, LS_WS_TEXT, text, strlen(text)); }

/* The screen's frames, one message each. */
static void web_write(const char *b, size_t n)
{
    if (s_web_viewer >= 0) ws_send(s_web_viewer, LS_WS_BINARY, b, n);
}

static void web_quit(void)
{
    if (s_web_viewer >= 0) wc_close(s_web_viewer);
}

static void make_token(void)
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) s_token[i] = digits[esp_random() & 15];
    s_token[32] = 0;
}

static bool save_safe(void);

/* 'token <t>' from a browser that has been here, 'login <password>' from
   one that has not; either way the answer to keep is 'ok <token>'. One
   guess a second, whoever makes it. */
static void web_auth(int i, const char *line, int64_t now)
{
    webconn_t *c = &s_wc[i];
    if (now < s_web_lock) { ws_say(i, "wait"); return; }
    bool right = false;
    if (!strncmp(line, "token ", 6)) right = s_token[0] && secret_is(line + 6, s_token, sizeof(s_token));
    else if (!strncmp(line, "login ", 6)) right = s_pass[0] && secret_is(line + 6, s_pass, sizeof(s_pass));
    if (!right) {
        s_web_lock = now + 1000000;
        ws_say(i, "no");
        if (++c->tries >= 5) wc_close(i);
        return;
    }
    /* One token for every browser, kept across restarts; a new password ends it. */
    if (!s_token[0]) {
        make_token();
        save_safe();
    }
    c->authed = true;
    char msg[40];
    snprintf(msg, sizeof(msg), "ok %s", s_token);
    ws_say(i, msg);
}

/* 'term web [ack] [z]': start the screen on this socket. The newest wins:
   a screen already open is most likely a phone put away. Updates always
   wait for their acks; a page that cannot inflate leaves the z off. */
static void web_begin(int i, const char *arg, int64_t now)
{
    if (s_web_viewer >= 0 && s_web_viewer != i) {
        const int old = s_web_viewer;
        ws_say(old, "term: web taken");           /* so that page waits rather than taking it back */
        wc_close(old);
    }
    s_web_viewer = i;
    memset(&s_web.in, 0, sizeof(s_web.in));
    s_web.cols = s_web.rows = 0;
    s_web.term_cols = s_web.term_rows = 0;
    s_web.repaint = false;
    s_web.last_in_us = now;
    s_web.start_us = 0;
    s_web.acks = true;
    s_web.z = arg && strstr(arg, "z");
    s_web.cells = arg && strstr(arg, "cells");
    s_web.cfont = -1;
    s_web.unacked = 0;
    s_web.write = web_write;
    s_web.on = true;
    s_web_ip = s_wc[i].ip;
    update_want();
    __atomic_store_n(&s_input_new, true, __ATOMIC_RELEASE);   /* the first frame now, not at the rate cap */
    int cols = 0, rows = 0;
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    char msg[48];
    LS_SNPRINTF(t_ls_rterm_term_web_begin_cols, msg, sizeof(msg), "term: web begin cols %d rows %d%s", cols, rows, s_web.cells ? " cells" : "");
    ws_say(i, msg);
}

static void ws_message(int i, const ls_ws_msg *m, int64_t now)
{
    webconn_t *c = &s_wc[i];
    if (m->op == LS_WS_CLOSE) {
        ws_send(i, LS_WS_CLOSE, m->data, m->n >= 2 ? 2 : 0);
        wc_close(i);
        return;
    }
    if (m->op == LS_WS_PING) { ws_send(i, LS_WS_PONG, m->data, m->n); return; }
    if (m->op != LS_WS_TEXT) return;
    char line[200];
    const size_t n = m->n < sizeof(line) - 1 ? m->n : sizeof(line) - 1;
    memcpy(line, m->data, n);
    line[n] = 0;
    if (!c->authed) { web_auth(i, line, now); return; }
    if (strncmp(line, "term ", 5)) return;
    char *verb = line + 5, *arg = strchr(verb, ' ');
    if (arg) *arg++ = 0;
    if (!strcmp(verb, "web")) { web_begin(i, arg, now); return; }
    if (i == s_web_viewer) session_word(&s_web, verb, arg, web_quit);
}

typedef struct {
    const char *path, *type, *cache;
    const uint8_t *start, *end;
    bool gz;
} web_file_t;

static const char MANIFEST[] =
    "{\"name\":\"LakeShark\",\"short_name\":\"LakeShark\",\"start_url\":\"/\",\"scope\":\"/\","
    "\"display\":\"standalone\",\"background_color\":\"#000000\",\"theme_color\":\"#000000\","
    "\"icons\":[{\"src\":\"/icon-192.png\",\"sizes\":\"192x192\",\"type\":\"image/png\"},"
    "{\"src\":\"/icon-512.png\",\"sizes\":\"512x512\",\"type\":\"image/png\"}]}";

static void http_reply(int i, const char *status)
{
    char head[160];
    const int n = LS_SNPRINTF(t_ls_rterm_http_1_1_s, head, sizeof(head), "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
    send_all(s_wc[i].fd, head, (size_t)n);
    wc_close(i);
}

/* A whole request head is in: answer it and close, or become a WebSocket. */
static void http_answer(int i, int64_t now)
{
    webconn_t *c = &s_wc[i];
    const char *req = (const char *)c->rx;
    char method[8], path[48], v[64];
    if (!ls_http_request(req, method, sizeof(method), path, sizeof(path)) ||
        (strcmp(method, "GET") && strcmp(method, "HEAD"))) {
        http_reply(i, "400 Bad Request");
        return;
    }
    if (!strcmp(path, "/ws")) {
        char key[64], acc[29], head[192];
        if (!ls_http_header(req, "Upgrade", v, sizeof(v)) || !ls_http_has_token(v, "websocket") ||
            !ls_http_header(req, "Sec-WebSocket-Key", key, sizeof(key))) {
            http_reply(i, "400 Bad Request");
            return;
        }
        ls_ws_accept(key, acc);
        const int n = LS_SNPRINTF(t_ls_rterm_http_1_1_101, head, sizeof(head),
                               "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                               "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
        if (!send_all(c->fd, head, (size_t)n)) { wc_close(i); return; }
        c->ws = true;
        c->n = 0;                     /* a browser sends nothing more until it has the 101 */
        c->since = c->last = now;
        return;
    }
    const web_file_t files[] = {
        {"/", "text/html; charset=utf-8", "no-cache", web_page_start, web_page_end, true},
        {"/manifest.webmanifest", "application/manifest+json", "no-cache",
         (const uint8_t *)MANIFEST, (const uint8_t *)MANIFEST + sizeof(MANIFEST) - 1, false},
        {"/icon-180.png", "image/png", "max-age=86400", web_icon180_start, web_icon180_end, false},
        {"/icon-192.png", "image/png", "max-age=86400", web_icon192_start, web_icon192_end, false},
        {"/icon-512.png", "image/png", "max-age=86400", web_icon512_start, web_icon512_end, false},
    };
    const web_file_t *f = !strcmp(path, "/index.html") ? &files[0] : NULL;
    for (size_t k = 0; k < sizeof(files) / sizeof(files[0]) && !f; k++)
        if (!strcmp(path, files[k].path)) f = &files[k];
    if (!f) { http_reply(i, "404 Not Found"); return; }
    const size_t size = (size_t)(f->end - f->start);
    /* The page's tag: the CRC of what it inflates to, from the gzip trailer. */
    char etag[16] = "";
    if (f->gz && size > 8) {
        const uint8_t *t = f->end - 8;
        snprintf(etag, sizeof(etag), "\"%02x%02x%02x%02x\"", t[3], t[2], t[1], t[0]);
        if (ls_http_header(req, "If-None-Match", v, sizeof(v)) && !strcmp(v, etag)) {
            http_reply(i, "304 Not Modified");
            return;
        }
    }
    char head[320];
    const int n = LS_SNPRINTF(t_ls_rterm_http_1_1_200, head, sizeof(head),
                           "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n%s%s%s%s"
                           "Cache-Control: %s\r\nConnection: close\r\n\r\n",
                           f->type, (unsigned)size, f->gz ? "Content-Encoding: gzip\r\n" : "",
                           etag[0] ? "ETag: " : "", etag, etag[0] ? "\r\n" : "", f->cache);
    if (send_all(c->fd, head, (size_t)n) && strcmp(method, "HEAD")) send_all(c->fd, f->start, size);
    wc_close(i);
}

static void web_read(int i, int64_t now)
{
    webconn_t *c = &s_wc[i];
    const int k = recv(c->fd, c->rx + c->n, WEB_RX - 1 - c->n, MSG_DONTWAIT);
    if (k == 0 || (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { wc_close(i); return; }
    if (k > 0) {
        c->n += (size_t)k;
        c->last = now;
    }
    if (!c->ws) {
        c->rx[c->n] = 0;
        if (strstr((const char *)c->rx, "\r\n\r\n")) http_answer(i, now);
        else if (c->n >= WEB_RX - 1 || now - c->since > WEB_HEAD_US) wc_close(i);
        return;
    }
    size_t at = 0;
    for (;;) {
        ls_ws_msg m;
        const int used = ls_ws_parse(c->rx + at, c->n - at, WEB_RX - 16, &m);
        if (used < 0) { wc_close(i); return; }
        if (!used) break;
        at += (size_t)used;
        ws_message(i, &m, now);
        if (c->fd < 0) return;
    }
    memmove(c->rx, c->rx + at, c->n - at);
    c->n -= at;
    if (!c->authed ? now - c->since > AUTH_US : now - c->last > WEB_IDLE_US) wc_close(i);
}

static void mdns_close(void)
{
    if (s_mdns >= 0) close(s_mdns);
    s_mdns = -1;
    s_mdns_ip = 0;
}

static void mdns_send(const uint8_t *b, size_t n, const struct sockaddr_in *to)
{
    sendto(s_mdns, b, n, 0, (const struct sockaddr *)to, sizeof(*to));
}

/* Answer for the name, and say it twice, a second apart, whenever the
   address is new (RFC 6762, 8.3), so a phone's cache lets the old go. */
static void mdns_service(int64_t now)
{
    esp_netif_t *nif = esp_netif_get_default_netif();
    esp_netif_ip_info_t info = {0};
    if (!nif || !esp_netif_is_netif_up(nif) || esp_netif_get_ip_info(nif, &info) != ESP_OK || !info.ip.addr) {
        mdns_close();
        return;
    }
    if (s_mdns >= 0 && info.ip.addr != s_mdns_ip) mdns_close();   /* the group was joined on the old address */
    if (s_mdns < 0) {
        if (now < s_mdns_retry) return;
        s_mdns_retry = now + 2000000;
        const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) return;
        const int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        const struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(LS_MDNS_PORT),
                                      .sin_addr.s_addr = htonl(INADDR_ANY)};
        struct ip_mreq mr = {.imr_multiaddr.s_addr = htonl(LS_MDNS_GROUP), .imr_interface.s_addr = info.ip.addr};
        const uint8_t ttl = 255;
        struct in_addr via = {.s_addr = info.ip.addr};
        if (bind(fd, (const struct sockaddr *)&a, sizeof(a)) ||
            setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) ||
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) ||
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &via, sizeof(via))) {
            close(fd);
            return;
        }
        s_mdns = fd;
        s_mdns_ip = info.ip.addr;
        s_mdns_say = 2;
        s_mdns_next = now;
    }
    const struct sockaddr_in group = {.sin_family = AF_INET, .sin_port = htons(LS_MDNS_PORT),
                                      .sin_addr.s_addr = htonl(LS_MDNS_GROUP)};
    uint8_t in[512], out[96];
    for (int k = 0; k < 4; k++) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        const int n = recvfrom(s_mdns, in, sizeof(in), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
        if (n <= 0) break;
        const bool legacy = from.sin_port != htons(LS_MDNS_PORT);
        bool unicast = false;
        const size_t m = ls_mdns_answer(in, (size_t)n, LS_RTERM_WEB_NAME, s_mdns_ip, legacy, out, sizeof(out), &unicast);
        if (m) mdns_send(out, m, legacy || unicast ? &from : &group);
    }
    if (s_mdns_say && now >= s_mdns_next) {
        const size_t m = ls_mdns_announce(LS_RTERM_WEB_NAME, s_mdns_ip, out, sizeof(out));
        if (m) mdns_send(out, m, &group);
        s_mdns_say--;
        s_mdns_next = now + 1000000;
    }
}

static void web_service(int64_t now)
{
    if (s_web_kick) {                 /* every browser past the login, the screen's too */
        s_web_kick = false;
        for (int i = 0; i < WEB_CONNS; i++)
            if (s_wc[i].authed) wc_close(i);
    }
    if (!s_web_enabled) {
        for (int i = 0; i < WEB_CONNS; i++)
            if (s_wc[i].fd >= 0) wc_close(i);
        if (s_web_listen >= 0) { close(s_web_listen); s_web_listen = -1; }
        mdns_close();
        return;
    }
    mdns_service(now);
    if (s_web_listen < 0 && now >= s_web_listen_retry) {
        s_web_listen_retry = now + 2000000;
        s_web_listen = tcp_listen(WEB_PORT, 4);
    }
    for (int i = 0; i < WEB_CONNS && s_web_listen >= 0; i++) {
        if (s_wc[i].fd >= 0) continue;
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        const int fd = accept(s_web_listen, (struct sockaddr *)&peer, &plen);
        if (fd < 0) break;
        client_opts(fd);
        s_wc[i] = (webconn_t){.fd = fd, .ip = peer.sin_addr.s_addr, .since = now, .last = now, .rx = s_wc[i].rx};
    }
    for (int i = 0; i < WEB_CONNS; i++)
        if (s_wc[i].fd >= 0) web_read(i, now);
}

/* --- ssh --------------------------------------------------------------- */

static void ssh_out(void *ctx, const char *b, size_t n)
{
    (void)ctx;
    if (s_sshq_over) return;
    if (s_sshq_n + n > s_sshq_cap) {
        size_t cap = s_sshq_cap ? s_sshq_cap : 16384;
        while (cap < s_sshq_n + n) cap *= 2;
        char *p = cap <= SSHQ_MAX ? heap_caps_realloc(s_sshq, cap, MALLOC_CAP_SPIRAM) : NULL;
        if (!p) { s_sshq_over = true; return; }
        s_sshq = p;
        s_sshq_cap = cap;
    }
    memcpy(s_sshq + s_sshq_n, b, n);
    s_sshq_n += n;
}

static void ssh_render(void)
{
    if (!s_ssh.on || __atomic_load_n(&s_sshq_ready, __ATOMIC_ACQUIRE)) return;
    render(&s_ssh, ssh_out);
    if (s_sshq_over) {                       /* no room: none of it goes, all of it next time */
        s_sshq_n = 0;
        s_sshq_over = false;
        s_ssh.scr.full = s_ssh.need_enter = true;
    } else if (s_sshq_n) {
        __atomic_store_n(&s_sshq_ready, true, __ATOMIC_RELEASE);
    }
}

static void ssh_quit(void) { s_ssh_quit = true; }

bool rterm_ssh_begin(int cols, int rows)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    memset(&s_ssh.in, 0, sizeof(s_ssh.in));
    s_ssh.cols = s_ssh.rows = 0;
    s_ssh.told = cols > 0 && rows > 0;
    s_ssh.term_cols = s_ssh.told ? cols : 0;
    s_ssh.term_rows = s_ssh.told ? rows : 0;
    s_ssh.repaint = false;
    s_ssh.last_in_us = esp_timer_get_time();
    /* No pty (ssh -T): ask where the cursor stops, as for telnet without NAWS. */
    s_ssh.start_us = s_ssh.told ? 0 : s_ssh.last_in_us + SIZE_WAIT_US;
    s_ssh.in.want_size = !s_ssh.told;
    s_sshq_n = 0;
    s_sshq_over = s_ssh_quit = false;
    __atomic_store_n(&s_sshq_ready, false, __ATOMIC_RELEASE);
    s_ssh.on = true;
    update_want();
    const bool ask = !s_ssh.told;
    xSemaphoreGive(s_mu);
    return ask;
}

void rterm_ssh_size(int cols, int rows)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_ssh.told = true;
    client_size(&s_ssh, cols, rows);
    xSemaphoreGive(s_mu);
}

void rterm_ssh_input(const uint8_t *b, size_t n)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    while (n) {
        const size_t k = n > 128 ? 128 : n;
        ls_vt_event ev[32];
        const int m = ls_vt_input_feed(&s_ssh.in, b, k, ev, 32);
        s_ssh.last_in_us = esp_timer_get_time();
        deliver(&s_ssh, ev, m, ssh_quit);
        b += k;
        n -= k;
    }
    xSemaphoreGive(s_mu);
}

bool rterm_ssh_quit(void) { return __atomic_exchange_n(&s_ssh_quit, false, __ATOMIC_ACQ_REL); }

void rterm_ssh_end(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_ssh.on = false;
    update_want();
    s_sshq_n = 0;
    __atomic_store_n(&s_sshq_ready, false, __ATOMIC_RELEASE);
    xSemaphoreGive(s_mu);
}

bool rterm_ssh_take(const char **b, size_t *n)
{
    if (!__atomic_load_n(&s_sshq_ready, __ATOMIC_ACQUIRE)) return false;
    *b = s_sshq;
    *n = s_sshq_n;
    return true;
}

void rterm_ssh_taken(void)
{
    s_sshq_n = 0;
    __atomic_store_n(&s_sshq_ready, false, __ATOMIC_RELEASE);
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
            /* Terminal text needs the cells resolved; the cell feed sends them as they are. */
            if ((s_ser.on && !s_ser.cells) || s_net.on || (s_web.on && !s_web.cells) || s_ssh.on)
                ls_vt_resolve(&f, false, s_now);
            s_now_cols = s_cols;
            s_now_rows = s_rows;
            render(&s_ser, frames_out);
            render(&s_net, net_out);
            render(&s_web, frames_out);
            ssh_render();
            __atomic_store_n(&s_ready, false, __ATOMIC_RELEASE);
        }
        /* A view that moved with nothing new on the screen: the frame it has, again. */
        if (s_net.repaint) render(&s_net, net_out);
        if (s_ssh.repaint) ssh_render();
        if (s_ssh.on && s_ssh.in.n && now - s_ssh.last_in_us > 40000) {
            ls_vt_event ev[2];
            deliver(&s_ssh, ev, ls_vt_input_idle(&s_ssh.in, ev, 2), ssh_quit);
        }
        if (s_ser.on && now - s_ser.last_in_us > SERIAL_IDLE_US) serial_end();
        if (s_ser.on && s_ser.in.n && now - s_ser.key_us > 40000) {
            ls_vt_event ev[2];
            deliver(&s_ser, ev, ls_vt_input_idle(&s_ser.in, ev, 2), serial_end);
        }
        if (s_web.on && s_web.in.n && now - s_web.key_us > 40000) {   /* a lone Esc is the key, not a sequence's start */
            ls_vt_event ev[2];
            deliver(&s_web, ev, ls_vt_input_idle(&s_web.in, ev, 2), web_quit);
        }
        net_service(now);
        web_service(now);
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
    s_web.shown = heap_caps_calloc(cells, sizeof(ls_vt_cell), MALLOC_CAP_SPIRAM);
    s_ssh.shown = heap_caps_calloc(cells, sizeof(ls_vt_cell), MALLOC_CAP_SPIRAM);
    s_frame = heap_caps_malloc(4096 + 16, MALLOC_CAP_SPIRAM);
    s_wsout = heap_caps_malloc(4096 + 16, MALLOC_CAP_SPIRAM);
    bool rx = true;
    for (int i = 0; i < WEB_CONNS; i++) {
        s_wc[i].fd = -1;
        s_wc[i].rx = heap_caps_malloc(WEB_RX, MALLOC_CAP_SPIRAM);
        rx = rx && s_wc[i].rx;
    }
    StaticTask_t *tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_TCM);
    if (!tcb) tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    /* 16 KB: the ROM deflate for serial updates wants most of it (as for tui png). */
    StackType_t *stack = heap_caps_malloc(TERM_STACK, MALLOC_CAP_SPIRAM);
    if (s_mu && s_snap && s_glass && s_top && s_bot && s_now && s_ser.shown && s_net.shown && s_web.shown &&
        s_ssh.shown && s_frame && s_wsout && rx && tcb && stack)
        s_task = xTaskCreateStaticPinnedToCore(worker, "ls_term", TERM_STACK, NULL, 2, stack, tcb, 0);
    if (!s_task) {
        heap_caps_free(s_snap); heap_caps_free(s_glass); heap_caps_free(s_top); heap_caps_free(s_bot);
        heap_caps_free(s_now); heap_caps_free(s_ser.shown); heap_caps_free(s_net.shown); heap_caps_free(s_web.shown);
        heap_caps_free(s_ssh.shown);
        heap_caps_free(s_frame); heap_caps_free(s_wsout); heap_caps_free(tcb); heap_caps_free(stack);
        for (int i = 0; i < WEB_CONNS; i++) { heap_caps_free(s_wc[i].rx); s_wc[i].rx = NULL; }
        s_snap = NULL; s_glass = NULL; s_top = s_bot = NULL; s_now = NULL;
        s_ser.shown = s_net.shown = s_web.shown = s_ssh.shown = NULL; s_frame = NULL; s_wsout = NULL;
        return false;
    }
    return true;
}

bool rterm_ensure_started(void) { return ensure_started(); }

/* --- settings ---------------------------------------------------------- */

static void load_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t on = 0, web = 0;
    nvs_get_u8(h, "net", &on);
    nvs_get_u8(h, "web", &web);
    size_t len = sizeof(s_pass);
    if (nvs_get_str(h, "pass", s_pass, &len) != ESP_OK) s_pass[0] = 0;
    len = sizeof(s_token);
    if (nvs_get_str(h, "wtok", s_token, &len) != ESP_OK) s_token[0] = 0;
    nvs_close(h);
    s_net_enabled = on && s_pass[0];
    s_web_enabled = web && s_pass[0];
}

static bool save_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_u8(h, "net", s_net_enabled ? 1 : 0) == ESP_OK &&
              nvs_set_u8(h, "web", s_web_enabled ? 1 : 0) == ESP_OK &&
              nvs_set_str(h, "pass", s_pass) == ESP_OK && nvs_set_str(h, "wtok", s_token) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

/* From the screen task, whose stack may be in PSRAM: NVS runs elsewhere. */
static esp_err_t save_job(void *arg)
{
    (void)arg;
    return save_settings() ? ESP_OK : ESP_FAIL;
}
static bool save_safe(void) { return ls_nvs_run(save_job, NULL, 0) == ESP_OK; }

/* A new password: browsers that signed in with the old one sign in again. */
static void password_changed(void)
{
    s_token[0] = 0;
    s_web_kick = true;
}

/* Ten letters and digits that cannot be mistaken for each other. */
static void make_password(char out[24])
{
    static const char abc[] = "abcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < 10; i++) out[i] = abc[esp_random() % (sizeof(abc) - 1)];
    out[10] = 0;
}

void ls_rterm_status(ls_rterm_status_t *o)
{
    if (!o) return;
    memset(o, 0, sizeof(*o));
    o->net_on = s_net_enabled;
    o->port = TERM_PORT;
    o->client = s_client >= 0;
    o->authed = s_authed;
    o->client_ip = s_client_ip;
    o->serial_on = s_ser.on;
    o->serial_z = s_ser.z;
    o->web_on = s_web_enabled;
    o->web_port = WEB_PORT;
    o->web_client = s_web.on;
    o->web_ip = s_web_ip;
    o->web_named = s_mdns >= 0;
    snprintf(o->pass, sizeof(o->pass), "%s", s_pass);
    rterm_ssh_status(o);
}

bool ls_rterm_net_set(bool on)
{
    if (on) {
        if (!s_pass[0]) make_password(s_pass);
        if (!ensure_started()) return false;
    }
    s_net_enabled = on;
    return save_safe();
}

bool ls_rterm_web_set(bool on)
{
    if (on) {
        if (!s_pass[0]) make_password(s_pass);
        if (!ensure_started()) return false;
    }
    s_web_enabled = on;
    return save_safe();
}

bool rterm_pass_ensure(void)
{
    if (s_pass[0]) return true;
    make_password(s_pass);
    return save_safe();
}

bool ls_rterm_password_set(const char *p)
{
    char fresh[24];
    if (!p) { make_password(fresh); p = fresh; }
    else if (strlen(p) < 6 || strlen(p) >= sizeof(s_pass)) return false;
    snprintf(s_pass, sizeof(s_pass), "%s", p);
    password_changed();
    return save_safe();
}

void ls_rterm_kick(void)
{
    s_kick = true;
    rterm_ssh_kick();
}
void ls_rterm_web_kick(void) { s_web_kick = true; }

void ls_rterm_start(void)
{
    load_settings();
    if ((s_net_enabled || s_web_enabled) && !ensure_started()) printf("term: no memory for the network terminal\n");
    rterm_ssh_load();
}

/* --- console ----------------------------------------------------------- */

int ls_rterm_console(int argc, char **argv)
{
    const char *verb = argc >= 2 ? argv[1] : "";
    /* The serial session's own traffic first, and silent: anything printed
       would be one more line for the PC to step over. */
    if (!strcmp(verb, "in") || !strcmp(verb, "ping") || !strcmp(verb, "redraw") || !strcmp(verb, "ack")) {
        if (!s_ser.on || !s_mu) return 0;
        xSemaphoreTake(s_mu, portMAX_DELAY);
        session_word(&s_ser, verb, argc >= 3 ? argv[2] : NULL, serial_end);
        xSemaphoreGive(s_mu);
        return 0;
    }
    if (!strcmp(verb, "size")) {                     /* 'term size C R': the viewer's window */
        char arg[24];
        snprintf(arg, sizeof(arg), "%s %s", argc >= 3 ? argv[2] : "", argc >= 4 ? argv[3] : "");
        session_word(&s_ser, verb, arg, serial_end);
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
        bool want_ack = false, want_z = false, want_cells = false;
        for (int a = 3; a < argc; a++) {
            want_ack |= !strcmp(argv[a], "ack");
            want_z |= !strcmp(argv[a], "z");
            want_cells |= !strcmp(argv[a], "cells");
        }
        LS_PRINTF(t_ls_rterm_term_serial_begin_cols, "term: serial begin cols %d rows %d baud %d%s\n", cols, rows, baud, want_cells ? " cells" : "");
        fflush(stdout);
        if (s_ser_wait) s_ser_wait();
        xSemaphoreTake(s_mu, portMAX_DELAY);
        s_prev_vprintf = esp_log_set_vprintf(term_vprintf);
        s_ser_baud(baud);
        memset(&s_ser.in, 0, sizeof(s_ser.in));
        s_ser.cols = s_ser.rows = 0;
        s_ser.last_in_us = esp_timer_get_time();
        s_ser.start_us = s_ser.last_in_us + 300000;
        s_ser.acks = want_ack;
        s_ser.z = want_z;
        s_ser.cells = want_cells;
        s_ser.cfont = -1;
        s_ser.unacked = 0;
        s_ser.write = s_ser_write;
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
            if (!s_pass[0]) make_password(s_pass);
            if (!ensure_started()) { printf("term: no memory\n"); return 1; }
            s_net_enabled = true;
            if (!save_settings()) printf("term: could not save the setting\n");
            LS_PRINTF(t_ls_rterm_term_network_terminal_on, "term: network terminal on, port %d, password %s (not encrypted: use it on your own network)\n",
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
                LS_PRINTF(t_ls_rterm_term_a_password_is, "term: a password is 6 to %d characters\n", (int)sizeof(s_pass) - 1);
                return 1;
            }
            snprintf(s_pass, sizeof(s_pass), "%s", argv[3]);
            password_changed();
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
    if (!strcmp(verb, "web")) {
        const char *what = argc >= 3 ? argv[2] : "";
        if (!strcmp(what, "on")) {
            if (!s_pass[0]) make_password(s_pass);
            if (!ensure_started()) { printf("term: no memory\n"); return 1; }
            s_web_enabled = true;
            if (!save_settings()) printf("term: could not save the setting\n");
            esp_netif_ip_info_t ip = {0};
            esp_netif_t *nif = esp_netif_get_default_netif();
            if (nif) esp_netif_get_ip_info(nif, &ip);
            printf("term: web screen on, http://" LS_RTERM_WEB_NAME ".local/ or http://" IPSTR "/ password %s "
                   "(not encrypted: use it on your own network)\n", IP2STR(&ip.ip), s_pass);
            return 0;
        }
        if (!strcmp(what, "off")) {
            s_web_enabled = false;
            save_settings();
            printf("term: web screen off\n");
            return 0;
        }
        if (!strcmp(what, "kick")) {
            s_web_kick = true;
            printf("term: web screens signed out\n");
            return 0;
        }
    }
    if (!strcmp(verb, "ssh")) return rterm_ssh_console(argc, argv);
    if (verb[0]) {
        printf("term: serial [baud] | off | net on|off|pass <p>|show|kick | web on|off|kick | ssh ...\n");
        return 1;
    }
    const uint32_t ip = ntohl(s_client_ip);
    LS_PRINTF(t_ls_rterm_term_serial_s_network, "term: serial %s, network %s on port %d, %s\n", s_ser.on ? "on" : "off",
           s_net_enabled ? "on" : "off", TERM_PORT,
           s_client < 0 ? "nobody connected" : s_authed ? "a client is watching" : "a client is at the password");
    if (s_client >= 0)
        LS_PRINTF(t_ls_rterm_term_client_lu_lu, "term: client %lu.%lu.%lu.%lu\n", (unsigned long)(ip >> 24), (unsigned long)(ip >> 16 & 255),
               (unsigned long)(ip >> 8 & 255), (unsigned long)(ip & 255));
    const uint32_t wip = ntohl(s_web_ip);
    LS_PRINTF(t_ls_rterm_term_web_screen_s, "term: web screen %s on port %d%s, ", s_web_enabled ? "on" : "off", WEB_PORT,
           s_mdns >= 0 ? " as " LS_RTERM_WEB_NAME ".local" : "");
    if (s_web.on)
        LS_PRINTF(t_ls_rterm_lu_lu_lu_lu, "%lu.%lu.%lu.%lu is watching\n", (unsigned long)(wip >> 24), (unsigned long)(wip >> 16 & 255),
               (unsigned long)(wip >> 8 & 255), (unsigned long)(wip & 255));
    else printf("nobody watching\n");
    return 0;
}
