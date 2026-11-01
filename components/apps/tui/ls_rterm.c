/* See ls_rterm.h. */

#include "ls_rterm.h"
#include "ls_input_ev.h"
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
#include "ls_ws_tx.h"
#include "ls_theme.h"
#include "ls_tui.h"
#include "ls_tui_screen.h"
#include "ls_tui_touch.h"
#include "miniz.h"
#include "ls_textpack.h"
#include "ls_rterm_visual.h"
#include "ls_map.h"

#define TERM_PORT      2323
#if CONFIG_LS_PANEL_RUNTIME
#define TERM_CELLS_MAX 43008          /* 384 x 112; C/X offsets and counts remain u16 */
#else
#define TERM_CELLS_MAX 8192
#endif
#define TERM_FRAME_US  50000          /* 20 frames a second at most */
#define TERM_CELLS_FRAME_US 15000     /* the cell feed: every frame the screen draws */
#define TERM_STACK     16384
#define SERIAL_IDLE_US 10000000       /* a serial session with no input or ping for this long ends */
#define SIZE_WAIT_US   500000         /* how long a network client has to say its size before the first frame */
#define TERM_AHEAD     2              /* updates the client may not yet have drawn */
#define TERM_ACK_US    5000000        /* an answer this late was lost: stop waiting for it */
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
    visual_view visual;
    uint32_t latency_seq;             /* last completed UI input sequence sent */
    uint32_t input_epoch;             /* excludes snapshots from an old viewer */
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
static bool (*s_resize)(int, int, const char *);
static void (*s_tap)(int, int);
static void (*s_ser_write)(const char *, size_t);
static void (*s_ser_wait)(void);
static void (*s_ser_baud)(int);

/* One encoded serial update, never a backlog. The worker fills it under s_mu,
 * then writes it outside s_mu. 64 bytes/cell bounds even worst-case VT colors. */
static char *s_serial_tx;
static size_t s_serial_n, s_serial_cap;
static char *s_serial_inflight; /* UART writer owns this until it reacquires s_mu. */
static bool s_serial_over, s_serial_stop;
/* A line for the PC on the serial feed (ls_rterm_viewer_line), written after an
 * update. A printf from another task lands inside an update's bytes when the
 * screen is changing, and the PC drops it with the update. */
static char s_say[48];
static void serial_stage(const char *b, size_t n)
{
    if (s_serial_over || n > s_serial_cap - s_serial_n) { s_serial_over = true; return; }
    memcpy(s_serial_tx + s_serial_n, b, n); s_serial_n += n;
}

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
static uint32_t s_snap_seq;
static uint32_t s_snap_epoch;
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
    ls_ws_tx tx;
    const uint8_t *body;              /* HTTP flash asset, sent incrementally */
    size_t body_n;
    bool closing;
    int64_t tx_progress;
} webconn_t;
static volatile bool s_web_enabled, s_web_kick;
static int s_web_listen = -1, s_web_viewer = -1;
static webconn_t s_wc[WEB_CONNS];
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
#if CONFIG_LS_PANEL_RUNTIME
extern const uint8_t web_font_start[] asm("_binary_lsfont_bin_gz_start");
extern const uint8_t web_font_end[] asm("_binary_lsfont_bin_gz_end");
extern const uint8_t web_mono_start[] asm("_binary_dejavu_mono_woff2_start");
extern const uint8_t web_mono_end[] asm("_binary_dejavu_mono_woff2_end");
extern const uint8_t web_bold_start[] asm("_binary_dejavu_mono_bold_woff2_start");
extern const uint8_t web_bold_end[] asm("_binary_dejavu_mono_bold_woff2_end");
#endif
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

void ls_rterm_set_resize(bool (*resize)(int cols, int rows, const char *font)) { s_resize = resize; }

void ls_rterm_set_serial(void (*write)(const char *, size_t), void (*wait_tx)(void), void (*set_baud)(int))
{
    s_ser_write = write;
    s_ser_wait = wait_tx;
    s_ser_baud = set_baud;
}

bool ls_rterm_active(void) { return s_want; }

void ls_rterm_viewer_line(const char *line)
{
    bool queued = false;
    if (s_mu) {
        xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
        if (s_ser.on && !s_serial_stop) { snprintf(s_say, sizeof(s_say), "%s", line); queued = true; }
        xSemaphoreGiveRecursive(s_mu);
    }
    if (queued) { if (s_task) xTaskNotifyGive(s_task); return; }
    printf("%s\n", line);
    fflush(stdout);
}

static bool
visual_stroke(int kind, const tui_surface *sf, tui_rect area,
    int dx, int dy, int x0, int y0, int x1, int y1, uint8_t attr);

/* The centres of the exact dot/slot rectangles the board rasterizes, including
   odd cell sizes. Quarter-pixel units preserve half-pixel centres on the wire. */
static int visual_axis(int dot,int per_cell,int cell,bool braille) {
    int slot=dot%per_cell,a=slot*cell/per_cell,b=(slot+1)*cell/per_cell;
    if(braille) {int d=(b-a+1)/2;a+=(b-a-d)/2;b=a+d;}
    return dot/per_cell*cell*4+(a+b)*2;
}

static void update_want(void) {
    s_want = s_ser.on || s_net.on || s_web.on || s_ssh.on;
    bool pixels=(s_ser.on && s_ser.cells && s_ser.visual.pixels) ||
                (s_web.on && s_web.cells && s_web.visual.pixels);
    bool vectors=(s_ser.on && s_ser.cells && s_ser.visual.vectors) ||
                 (s_web.on && s_web.cells && s_web.visual.vectors);
    if(vectors && !__atomic_load_n(&s_visual_vectors,__ATOMIC_ACQUIRE))
        __atomic_store_n(&s_trace_new,true,__ATOMIC_RELEASE);
    __atomic_store_n(&s_visual_pixels,pixels,__ATOMIC_RELEASE);
    __atomic_store_n(&s_visual_vectors,vectors,__ATOMIC_RELEASE);
    __atomic_store_n(&tui_stroke_hook,vectors ? visual_stroke : NULL,__ATOMIC_RELEASE);
    /* A viewer joined or changed what it takes: one capture even on a still
       screen, which capture_locked otherwise skips. */
    __atomic_store_n(&s_input_new,true,__ATOMIC_RELEASE);
}

static bool visual_stroke(int kind, const tui_surface *sf, tui_rect area,
    int dx, int dy, int x0, int y0, int x1, int y1, uint8_t attr)
{
    if (!__atomic_load_n(&s_visual_vectors,__ATOMIC_ACQUIRE)) return false;
    tui_surface *root = ls_tui_surface();
    /* If enabled halfway through a draw, keep ordinary dots until the next
       complete root frame. Never publish paths left from a previous viewer. */
    if(kind && __atomic_load_n(&s_trace_new,__ATOMIC_ACQUIRE)) return false;
    if (kind == 0) {
        if (sf != root) return false;
        __atomic_store_n(&s_trace_new,false,__ATOMIC_RELEASE);
        size_t n = (size_t)root->w * root->h;
        if (n != s_dot_count) {
            heap_caps_free(s_dots);
            s_dots = heap_caps_calloc(n, sizeof(*s_dots), MALLOC_CAP_SPIRAM);
            s_dot_count = s_dots ? n : 0;
        }
        if (!s_lines) s_lines = heap_caps_malloc(VECTOR_CAP * sizeof(*s_lines), MALLOC_CAP_SPIRAM);
        if (!s_frame_lines) s_frame_lines = heap_caps_malloc(VECTOR_CAP * sizeof(*s_frame_lines), MALLOC_CAP_SPIRAM);
        if (s_dots) memset(s_dots, 0, s_dot_count * sizeof(*s_dots));
        s_line_count = s_line_dropped = 0;
        return false;
    }
    if (!s_dot_count || !s_lines || !s_frame_lines || dx <= 0 || dy <= 0) return false;
    int ox=0,oy=0,cw=0,ch=0; ls_tui_get_origin(&ox,&oy); ls_tui_geometry(NULL,NULL,&cw,&ch);
    int cx=area.x + x0/dx + ox, cy=area.y + y0/dy + oy;
    if (x0<0 || y0<0 || cx<0 || cy<0 || cx>=root->w || cy>=root->h) return false;
    if (kind == 2) {
        visual_dot *d=&s_dots[(size_t)cy*root->w+cx];
        if (d->attr != attr || d->glyph != y1) d->bits=0;
        d->attr=attr; d->glyph=(uint16_t)y1; d->bits|=(uint8_t)x1;
        return true;
    }
    int ex=area.x+x1/dx+ox, ey=area.y+y1/dy+oy;
    if(x1<0 || y1<0 || ex<0 || ey<0 || ex>=root->w || ey>=root->h) return false;
    if (s_line_count == VECTOR_CAP) { s_line_dropped++; return false; }
    s_lines[s_line_count++]=(visual_line){
        (uint16_t)((area.x+ox)*cw*4+visual_axis(x0,dx,cw,dy==4)),
        (uint16_t)((area.y+oy)*ch*4+visual_axis(y0,dy,ch,dy==4)),
        (uint16_t)((area.x+ox)*cw*4+visual_axis(x1,dx,cw,dy==4)),
        (uint16_t)((area.y+oy)*ch*4+visual_axis(y1,dy,ch,dy==4)),attr};
    return true;
}

static void visual_unavailable(void)
{
    heap_caps_free(s_picture.src);heap_caps_free(s_picture.masks);heap_caps_free(s_picture.dirty_lo);heap_caps_free(s_picture.dirty_hi);heap_caps_free(s_picture.palette_lut);heap_caps_free(s_picture.palette);
    s_picture=(visual_picture){.ready=true,.unavailable=true,.generation=++s_picture_generation};
}

static void visual_source_copy(uint16_t *dst,const uint16_t *src,size_t bytes)
{
    memcpy(dst,src,bytes);s_picture.copy_bytes+=(uint32_t)bytes;
}

/* Called only by the UI, before publishing the snapshot. Copy the source,
   not a scaled maximum-size canvas. Ring offsets are applied by the encoder.
   Masks use exactly the glass renderer's coverage and halo rules. */
static void visual_capture(tui_rect r, const uint16_t *src, int w, int h,
                           int x0, int y0, uint32_t serial, int64_t now,bool input)
{
    s_picture.ready=false;
    s_picture.copy_bytes=0;
    bool vectors=__atomic_load_n(&s_visual_vectors,__ATOMIC_ACQUIRE);
    bool pixels=__atomic_load_n(&s_visual_pixels,__ATOMIC_ACQUIRE);
    if(!pixels) {heap_caps_free(s_picture.src);heap_caps_free(s_picture.masks);heap_caps_free(s_picture.dirty_lo);heap_caps_free(s_picture.dirty_hi);heap_caps_free(s_picture.palette_lut);heap_caps_free(s_picture.palette);s_picture=(visual_picture){0};}
    uint32_t pan_epoch=0;int32_t pan_left=0,pan_top=0;uint16_t pan_ground=0;
    bool pan=ls_tui_image_pan_state(&pan_epoch,&pan_left,&pan_top,&pan_ground);
    if(!pan && (s_picture.dirty_lo || s_picture.palette_lut || s_picture.palette)) {
        heap_caps_free(s_picture.dirty_lo);heap_caps_free(s_picture.dirty_hi);
        heap_caps_free(s_picture.palette_lut);heap_caps_free(s_picture.palette);
        s_picture.dirty_lo=s_picture.dirty_hi=s_picture.palette_lut=s_picture.palette=NULL;
        s_picture.palette_n=0;s_picture.palette_generation=0;s_picture.palette_overflow=false;
    }
    if(input && !pan && (pixels || vectors)) {
        /* Moving an unchanged image needs no PSRAM copy. Keep its placement
           current even while input takes priority over pixel payloads. */
        if(pixels && src==s_picture.identity && w==s_picture.sw && h==s_picture.sh &&
           s_picture.src && r.w>0 && r.h>0) {
            s_picture.rect=r;s_picture.w=r.w*s_cell_w;s_picture.h=r.h*s_cell_h;
        }
        s_frame_line_count=0;s_vectors_hash=0;return;
    }
    if(!vectors) {
        heap_caps_free(s_lines);heap_caps_free(s_frame_lines);heap_caps_free(s_dots);heap_caps_free(s_frame_dots);
        s_lines=s_frame_lines=NULL;s_dots=s_frame_dots=NULL;s_dot_count=s_frame_dot_count=0;s_line_count=0;
    }
    if (vectors && s_frame_dot_count!=s_dot_count) {
        heap_caps_free(s_frame_dots);
        s_frame_dots=heap_caps_calloc(s_dot_count,sizeof(*s_frame_dots),MALLOC_CAP_SPIRAM);
        s_frame_dot_count=s_frame_dots?s_dot_count:0;
    }
    if (vectors && !__atomic_load_n(&s_trace_new,__ATOMIC_ACQUIRE) &&
        s_frame_dots && s_frame_lines && s_dot_count==(size_t)s_cols*s_rows) {
        memcpy(s_frame_dots,s_dots,s_dot_count*sizeof(*s_dots));
        memcpy(s_frame_lines,s_lines,s_line_count*sizeof(*s_lines));
        s_frame_line_count=s_line_count;
        s_vectors_hash=visual_hash(s_frame_dots,s_dot_count*sizeof(*s_frame_dots)) ^
            visual_hash(s_frame_lines,s_frame_line_count*sizeof(*s_frame_lines)) ^
            visual_hash(s_snap,s_dot_count*sizeof(*s_snap)) ^ visual_hash(s_pal,sizeof(s_pal));
    } else { s_frame_line_count=0; s_vectors_hash=0; }
    if (!pixels) {
        return;
    }
    if(now-s_picture_us<(pan?16000:PICTURE_US)) return;
    s_picture_us=now;
    if (!src || w<=0 || h<=0 || r.w<=0 || r.h<=0) {
        heap_caps_free(s_picture.src); heap_caps_free(s_picture.masks);heap_caps_free(s_picture.dirty_lo);heap_caps_free(s_picture.dirty_hi);heap_caps_free(s_picture.palette_lut);heap_caps_free(s_picture.palette);
        s_picture=(visual_picture){.ready=true}; return;
    }
    if(w>65535 || h>65535 || (uint64_t)w*h>4194304) {visual_unavailable();return;}
    const size_t bytes=(size_t)w*h*2;
    uint32_t epoch=0,columns=0,rows=0;
    bool ring=ls_tui_image_ring_state(&epoch,&columns,&rows);
    bool same_source=s_picture.identity==src && s_picture.sw==w && s_picture.sh==h && s_picture.src;
    const bool changed=!same_source || s_picture.serial!=serial || s_picture.ring!=ring ||
        pan!=s_picture.pan || (pan && (s_picture.pan_epoch!=pan_epoch || s_picture.pan_left!=pan_left || s_picture.pan_top!=pan_top)) ||
        (ring && (s_picture.ring_epoch!=epoch || s_picture.ring_columns!=columns || s_picture.ring_rows!=rows));
    if (bytes!=s_picture.bytes) {
        heap_caps_free(s_picture.src); s_picture.src=heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM);
        s_picture.bytes=s_picture.src?bytes:0;
        s_picture.identity=NULL;
    }
    if (!s_picture.src) {visual_unavailable();return;}
    s_picture.copy_bytes=0;
    if(pan && (!s_picture.dirty_lo || !s_picture.dirty_hi || s_picture.sh!=h)) {
        heap_caps_free(s_picture.dirty_lo);heap_caps_free(s_picture.dirty_hi);
        s_picture.dirty_lo=heap_caps_calloc(h,sizeof(uint16_t),MALLOC_CAP_SPIRAM);
        s_picture.dirty_hi=heap_caps_calloc(h,sizeof(uint16_t),MALLOC_CAP_SPIRAM);
    }
    if(pan && (!s_picture.dirty_lo || !s_picture.dirty_hi)) {visual_unavailable();return;}
    if(changed && pan) {
        int64_t dx=(int64_t)pan_left-s_picture.pan_left,dy=(int64_t)pan_top-s_picture.pan_top;
        bool reuse=same_source && s_picture.pan && pan_epoch==s_picture.pan_epoch &&
            dx>-w && dx<w && dy>-h && dy<h &&
            dx>=INT16_MIN && dx<=INT16_MAX && dy>=INT16_MIN && dy<=INT16_MAX && !ring;
        unsigned palette_before=s_picture.palette_n;
        if(!s_picture.palette_lut)s_picture.palette_lut=heap_caps_malloc(65536u*sizeof(uint16_t),MALLOC_CAP_SPIRAM);
        if(!s_picture.palette)s_picture.palette=heap_caps_malloc(256u*sizeof(uint16_t),MALLOC_CAP_SPIRAM);
        if(s_picture.palette_lut && s_picture.palette && (!reuse || !s_picture.palette_n)) {
            memset(s_picture.palette_lut,255,65536u*sizeof(uint16_t));
            s_picture.palette_n=0;s_picture.palette_overflow=false;
        }
        s_picture.pan_base=reuse?s_picture.generation:0;
        s_picture.pan_dx=reuse?(int32_t)dx:0;s_picture.pan_dy=reuse?(int32_t)dy:0;
        if(reuse && (dx || dy)) {
            for(int j=0;j<h;j++) {
                int y=dy<0?h-1-j:j,sy=y+(int)dy;
                uint16_t *out=s_picture.src+(size_t)y*w;
                int lo=dx<0?-(int)dx:0,hi=dx>0?w-(int)dx:w;
                if(sy>=0 && sy<h) {
                    memmove(out+lo,s_picture.src+(size_t)sy*w+lo+(int)dx,(size_t)(hi-lo)*2);
                    for(int x=0;x<lo;x++)out[x]=pan_ground;
                    for(int x=hi;x<w;x++)out[x]=pan_ground;
                } else for(int x=0;x<w;x++)out[x]=pan_ground;
            }
        }
        if(s_picture.palette_lut && s_picture.palette && !s_picture.palette_overflow)visual_palette_add(pan_ground);
        for(int y=0;y<h;y++) {
            uint16_t *out=s_picture.src+(size_t)y*w;const uint16_t *in=src+(size_t)y*w;
            int lo=0,hi=w;
            if(reuse) {while(lo<hi && out[lo]==in[lo])lo++;while(hi>lo && out[hi-1]==in[hi-1])hi--;}
            s_picture.dirty_lo[y]=(uint16_t)lo;s_picture.dirty_hi[y]=(uint16_t)hi;
            if(hi>lo)visual_source_copy(out+lo,in+lo,(size_t)(hi-lo)*2);
            if(s_picture.palette_lut && s_picture.palette && !s_picture.palette_overflow)
                for(int x=lo;x<hi;x++)if(!visual_palette_add(in[x]))break;
        }
        if(s_picture.palette_n && !s_picture.palette_overflow) {
            if(!reuse || palette_before!=s_picture.palette_n || !s_picture.palette_generation)
                s_picture.palette_generation=++s_palette_generation;
        } else s_picture.palette_generation=0;
        s_picture.generation=++s_picture_generation;
        ls_map_trace("capture pan",(int32_t)s_picture.generation,reuse);
    } else if(changed) {
        uint32_t dx=columns-s_picture.ring_columns,dy=rows-s_picture.ring_rows;
        bool known=same_source && ring && s_picture.ring && epoch==s_picture.ring_epoch;
        if(known && !dx && dy>0 && dy<(uint32_t)h && x0==s_picture.x0 &&
           y0==(s_picture.y0+h-(int)dy)%h) {
            for(uint32_t i=0;i<dy;i++) {
                size_t off=(size_t)((y0+(int)i)%h)*w;
                visual_source_copy(s_picture.src+off,src+off,(size_t)w*2);
            }
            s_picture.partial_axes|=2;
        } else if(known && !dy && dx>0 && dx<(uint32_t)w && y0==s_picture.y0 &&
                  x0==(s_picture.x0+(int)dx)%w) {
            int first=w-s_picture.x0;if(first>(int)dx) first=(int)dx;
            for(int y=0;y<h;y++) {
                size_t off=(size_t)y*w;
                visual_source_copy(s_picture.src+off+s_picture.x0,src+off+s_picture.x0,(size_t)first*2);
                if(first<(int)dx) visual_source_copy(s_picture.src+off,src+off,(dx-first)*2);
            }
            s_picture.partial_axes|=1;
        } else {
            visual_source_copy(s_picture.src,src,bytes);
            s_picture.full_epoch=++s_picture_full_epoch;s_picture.partial_axes=0;
        }
        s_picture.generation=++s_picture_generation;
        ls_map_trace("capture",(int32_t)s_picture.generation,0);
    }
    s_picture.pan=pan;s_picture.pan_epoch=pan_epoch;s_picture.pan_left=pan_left;s_picture.pan_top=pan_top;s_picture.pan_ground=pan_ground;
    s_picture.ring=ring;s_picture.ring_epoch=epoch;s_picture.ring_columns=columns;s_picture.ring_rows=rows;
    s_picture.identity=src; s_picture.serial=serial;
    s_picture.sw=w;s_picture.sh=h;s_picture.rect=r;
    s_picture.w=r.w*s_cell_w;s_picture.h=r.h*s_cell_h;s_picture.x0=x0;s_picture.y0=y0;
    size_t count=0;
    tui_rect visible=tui_rect_intersect(r,tui_rect_make(0,0,s_cols,s_rows));
    for(int y=visible.y;y<visible.y+visible.h;y++) for(int x=visible.x;x<visible.x+visible.w;x++) {
        size_t i=(size_t)y*s_cols+x;
        if(s_glass_n && (s_glass[i>>3]>>(i&7)&1) && (uint16_t)s_snap[i].ch!=0xB0) count++;
    }
    const size_t mask_bytes=((size_t)s_cell_w*s_cell_h+1)/2;
    const size_t n=3+count*(5+mask_bytes);
    if (n!=s_picture.masks_n) {
        heap_caps_free(s_picture.masks);s_picture.masks=heap_caps_malloc(n,MALLOC_CAP_SPIRAM);
        s_picture.masks_n=s_picture.masks?n:0;
    }
    if (!s_picture.masks) {visual_unavailable();return;}
    uint8_t *p=s_picture.masks;*p++='O';*p++=(uint8_t)count;*p++=(uint8_t)(count>>8);
    uint8_t cov[32*32];
    for(int y=visible.y;y<visible.y+visible.h;y++) for(int x=visible.x;x<visible.x+visible.w;x++) {
        size_t i=(size_t)y*s_cols+x;
        if(!s_glass_n || !(s_glass[i>>3]>>(i&7)&1) || (uint16_t)s_snap[i].ch==0xB0) continue;
        bool halo=false;ls_tui_glass_mask((uint16_t)s_snap[i].ch,cov,&halo);
        uint16_t glyph=(uint16_t)s_snap[i].ch;if(glyph>=0xff00) glyph&=255;
        *p++=(uint8_t)i;*p++=(uint8_t)(i>>8);*p++=halo;
        *p++=(uint8_t)glyph;*p++=(uint8_t)(glyph>>8);
        for(size_t j=0;j<mask_bytes;j++) {
            size_t a=j*2,b=a+1;*p++=cov[a] | (b<(size_t)s_cell_w*s_cell_h?cov[b]<<4:0);
        }
    }
    s_picture.masks_hash=visual_hash(s_picture.masks,n);
    s_picture.unavailable=false;
    s_picture.ready=true;
}

/* All feed storage is protected by s_mu, including the UI capture. A grid
 * change is a transaction: allocate every replacement first, then publish it.
 * Failure leaves the previous grid and its diff baselines intact. */
static size_t s_buffer_cells;
static bool s_grid_oom;
static bool grid_buffers(size_t cells)
{
    if (cells > TERM_CELLS_MAX) return false;
    const bool watching = s_ser.on || s_net.on || s_web.on || s_ssh.on;
    if (!watching) cells = 0;
    /* A ready SSH update may already be in chan_out on another task. Keep its
     * queue until taken; resize it on the next capture, never underneath it. */
    const bool ssh_busy = __atomic_load_n(&s_sshq_ready, __ATOMIC_ACQUIRE);
    const size_t tx = cells * 64 + 16384;
    const size_t chunk = cells ? 4010 : 0; /* streamed chunks, never a whole frame */
    struct slot { void *old, *next; size_t bytes; bool change; } b[] = {
        {s_snap, NULL, cells * sizeof(*s_snap), false},
        {s_glass, NULL, (cells + 7) / 8, false},
        {s_top, NULL, cells * sizeof(*s_top), false},
        {s_bot, NULL, cells * sizeof(*s_bot), false},
        {s_now, NULL, cells * sizeof(*s_now), false},
        {s_ser.shown, NULL, s_ser.on ? cells * sizeof(ls_vt_cell) : 0, false},
        {s_net.shown, NULL, s_net.on ? cells * sizeof(ls_vt_cell) : 0, false},
        {s_web.shown, NULL, s_web.on ? cells * sizeof(ls_vt_cell) : 0, false},
        {s_ssh.shown, NULL, s_ssh.on ? cells * sizeof(ls_vt_cell) : 0, false},
        {s_serial_tx, NULL, s_ser.on ? tx : 0, false},
        {s_sshq, NULL, ssh_busy ? s_sshq_cap : s_ssh.on ? tx : 0, false},
        {s_frame, NULL, chunk, false},
    };
    bool changed = false;
    for (size_t i = 0; i < sizeof(b)/sizeof(b[0]); i++) {
        b[i].change = i == 10 ? (!ssh_busy && s_sshq_cap != b[i].bytes) :
            cells != s_buffer_cells || (!!b[i].old != !!b[i].bytes);
        if (!b[i].change) { b[i].next = b[i].old; continue; }
        changed = true;
        if (b[i].bytes) b[i].next = heap_caps_calloc(1, b[i].bytes, MALLOC_CAP_SPIRAM);
        if (b[i].bytes && !b[i].next) {
            for (size_t j = 0; j < i; j++) if (b[j].change) heap_caps_free(b[j].next);
            return false;
        }
    }
    for (size_t i = 0; i < sizeof(b)/sizeof(b[0]); i++)
        if (b[i].change && b[i].old != s_serial_inflight) heap_caps_free(b[i].old);
    s_snap = b[0].next; s_glass = b[1].next; s_top = b[2].next; s_bot = b[3].next;
    s_now = b[4].next;
    s_ser.shown = b[5].next; s_net.shown = b[6].next;
    s_web.shown = b[7].next; s_ssh.shown = b[8].next;
    s_serial_tx = b[9].next; s_serial_cap = b[9].bytes;
    s_sshq = b[10].next; s_sshq_cap = b[10].bytes;
    s_frame = b[11].next;
    s_buffer_cells = cells;
    if (changed) {
        s_serial_n = 0;
        s_ser.unacked = 0;
        if (!ssh_busy) s_sshq_n = 0;
        s_ser.cols = s_net.cols = s_web.cols = s_ssh.cols = 0;
        s_now_cols = s_now_rows = 0;
        __atomic_store_n(&s_ready, false, __ATOMIC_RELEASE);
    }
    if (!cells) {
        heap_caps_free(s_z); s_z = NULL;
        heap_caps_free(s_zpay); s_zpay = NULL;
    }
    return true;
}

bool ls_rterm_prepare_grid(int cols, int rows)
{
    if (cols < 1 || rows < 1 || (size_t)cols * rows > TERM_CELLS_MAX) return false;
    if (!s_mu) return true; /* no feed has ever been opened */
    xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
    const bool ok = grid_buffers((size_t)cols * rows);
    s_grid_oom = !ok;
    xSemaphoreGiveRecursive(s_mu);
    return ok;
}

/* Called under s_mu before publishing a new session. Saved large grids may
 * no longer fit beside the current app: try smaller virtual grids on attach. */
static bool session_buffers(session_t *session)
{
    int cols = 0, rows = 0;
    ls_tui_geometry(&cols, &rows, NULL, NULL);
    if (cols > 0 && rows > 0 && grid_buffers((size_t)cols * rows)) return true;
    while (cols > 40 || rows > 12) {
        cols = cols * 3 / 4; if (cols < 40) cols = 40;
        rows = rows * 3 / 4; if (rows < 12) rows = 12;
        if (s_resize && s_resize(cols, rows, "")) return true;
    }
    session->on = false;
    update_want();
    return false;
}

/* --- the frame --------------------------------------------------------- */

static void capture_locked(void)
{
    if (__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE)) return;
    if (!s_want) {
        visual_capture((tui_rect){0},NULL,0,0,0,0,0,0,false);
        return;
    }
    const int64_t now = esp_timer_get_time();
    /* The rate cap is for screens that change by themselves; the frame
       answering a key or a click goes at once. */
    const uint32_t applied = ls_input_applied_seq();
    const uint32_t epoch = ls_input_applied_epoch();
    const bool answer = __atomic_exchange_n(&s_input_new, false, __ATOMIC_ACQ_REL) || applied != s_snap_seq || epoch != s_snap_epoch;
    /* Cells cost the board next to nothing to send: with only cell viewers on,
       every frame the screen draws can go (a 40 ms frame against the 50 ms cap
       sent every other one). */
    const bool cells_only = !s_net.on && !s_ssh.on && (!s_ser.on || s_ser.cells) && (!s_web.on || s_web.cells);
    if (!answer && now - s_last_cap < (cells_only ? TERM_CELLS_FRAME_US : TERM_FRAME_US)) return;
    tui_surface *sf = ls_tui_surface();
    if (!sf || !sf->front || (size_t)sf->w * sf->h > TERM_CELLS_MAX) return;
    if (!grid_buffers((size_t)sf->w * sf->h)) {
        int c = sf->w, r = sf->h;
        while (c > 40 || r > 12) {
            c = c * 3 / 4; if (c < 40) c = 40;
            r = r * 3 / 4; if (r < 12) r = 12;
            if (s_resize && s_resize(c, r, "")) break;
        }
        return;
    }
    size_t gn = 0;
    const uint8_t *g = ls_tui_glass_now(&gn);
    /* Nothing new on the screen (cells, glass, picture) and no input to answer:
       nothing to send. A static MAP cost the UI ~6 ms a frame here, every frame. */
    {
        static struct { const uint16_t *src; uint32_t serial, pan_epoch, lines_hash; tui_rect r;
                        int x0, y0; int32_t pan_left, pan_top; size_t lines; bool valid; } last;
        tui_rect ir = {0}; const uint16_t *isrc = NULL; int iw = 0, ih = 0, ix0 = 0, iy0 = 0; uint32_t iser = 0;
        if (ls_tui_image_now(&ir, &isrc, &iw, &ih, &iser)) ls_tui_image_ring_offset(&ix0, &iy0);
        uint32_t pe = 0; int32_t pl = 0, pt = 0; uint16_t pg = 0;
        ls_tui_image_pan_state(&pe, &pl, &pt, &pg);
        const bool vectors = __atomic_load_n(&s_visual_vectors, __ATOMIC_ACQUIRE);
        const uint32_t lh = vectors && s_lines ? visual_hash(s_lines, s_line_count * sizeof(*s_lines)) : 0;
        /* Same picture as the last capture, and, with a picture viewer, as the
           one the picture layer last TOOK: its own rate cap can pass a new one
           over, and a later identical frame must still deliver it. */
        const bool pixels = __atomic_load_n(&s_visual_pixels, __ATOMIC_ACQUIRE);
        const bool same_image = last.valid && last.src == isrc && last.serial == iser && last.x0 == ix0 &&
            last.y0 == iy0 && !memcmp(&last.r, &ir, sizeof(ir)) && last.pan_epoch == pe &&
            last.pan_left == pl && last.pan_top == pt && last.lines == s_line_count && last.lines_hash == lh &&
            (!pixels || (!isrc ? !s_picture.identity
                               : s_picture.identity == isrc && s_picture.serial == iser && s_picture.x0 == ix0 &&
                                 s_picture.y0 == iy0 && !memcmp(&s_picture.rect, &ir, sizeof(ir))));
        const ls_tui_theme_t *th = ls_tui_theme_effective(ls_tui_get_theme(), ls_tui_daylight());
        const size_t gl = g && gn <= (s_buffer_cells + 7) / 8 ? gn : 0;
        if (!answer && same_image && s_cols == sf->w && s_rows == sf->h && gl == s_glass_n &&
            (!th || !memcmp(s_pal, th->palette, sizeof(s_pal))) && ls_tui_font_at(s_font_i) == ls_tui_font() &&
            (!gl || !memcmp(s_glass, g, gl)) &&
            !memcmp(s_snap, sf->front, (size_t)s_cols * s_rows * sizeof(tui_cell))) {
            s_last_cap = now;
            return;
        }
        last.src = isrc; last.serial = iser; last.r = ir; last.x0 = ix0; last.y0 = iy0;
        last.pan_epoch = pe; last.pan_left = pl; last.pan_top = pt;
        last.lines = s_line_count; last.lines_hash = lh; last.valid = true;
    }
    s_cols = sf->w;
    s_rows = sf->h;
    memcpy(s_snap, sf->front, (size_t)s_cols * s_rows * sizeof(tui_cell));
    s_glass_n = g && gn <= (s_buffer_cells + 7) / 8 ? gn : 0;
    if (s_glass_n) memcpy(s_glass, g, s_glass_n);
    tui_rect r={0};
    const uint16_t *src = NULL;
    int w = 0, h = 0, x0 = 0, y0 = 0;
    uint32_t serial = 0;
    s_img.w = 0;
    bool have_image=ls_tui_image_now(&r, &src, &w, &h, &serial);
    if(have_image) ls_tui_image_ring_offset(&x0,&y0);
    if (have_image && src && r.x >= 0 && r.y >= 0 &&
        r.x + r.w <= s_cols && r.y + r.h <= s_rows) {
        /* Sampling every cell of a picture cost ~10 ms a frame on the Nano with
           MAP up, for a picture that had not changed: only a new one is sampled. */
        static struct { const uint16_t *src; uint32_t serial; tui_rect r; int w, h, x0, y0, cols, rows; void *top; } done;
        if (done.src != src || done.serial != serial || memcmp(&done.r, &r, sizeof(r)) || done.w != w || done.h != h ||
            done.x0 != x0 || done.y0 != y0 || done.cols != s_cols || done.rows != s_rows || done.top != (void *)s_top) {
            ls_vt_sample_image(r, s_cols, src, w, h, x0, y0, s_top, s_bot);
            done.src = src; done.serial = serial; done.r = r; done.w = w; done.h = h;
            done.x0 = x0; done.y0 = y0; done.cols = s_cols; done.rows = s_rows; done.top = (void *)s_top;
        }
        s_img = r;
    }
    const ls_tui_theme_t *th = ls_tui_theme_effective(ls_tui_get_theme(), ls_tui_daylight());
    if (th) memcpy(s_pal, th->palette, sizeof(s_pal));
    for (s_font_i = 0; s_font_i < ls_tui_font_count() - 1; s_font_i++)
        if (ls_tui_font_at(s_font_i) == ls_tui_font()) break;
    ls_tui_geometry(NULL, NULL, &s_cell_w, &s_cell_h);
    visual_capture(r, have_image ? src : NULL, w, h, x0, y0, serial, now,answer);
    if (answer) ls_map_trace("answer frame", (int32_t)applied, 0);
    s_snap_seq = applied;             /* this snapshot was drawn after UI drain */
    s_snap_epoch = epoch;
    s_last_cap = now;
    __atomic_store_n(&s_ready, true, __ATOMIC_RELEASE);
    if (s_task) xTaskNotifyGive(s_task);
}

void ls_rterm_capture(void)
{
    if (!s_mu || !s_want) return;
    /* A slow network/UART consumer must never park the UI task. */
    if (xSemaphoreTakeRecursive(s_mu, 0) != pdTRUE) return;
    capture_locked();
    xSemaphoreGiveRecursive(s_mu);
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
     'L' u32 completed input sequence
   Only after 'term visual P V' negotiates pix1/vec1: I (source rows and
   mapping), O (exact glass coverage), V (paths and owned dot bits). See
   bench/docs/desktop-pixels.md; the legacy records above are unchanged.
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

static ls_vt_cell cell_feed_key(const session_t *s,size_t i,int c,int r)
{
    ls_vt_cell key=cell_key(i,c,r);
    /* I owns picture colors. A neutral X placeholder avoids a second,
       potentially incompressible half-block picture even on the first frame.
       Glyph/kind changes remain immediate; pixels off sends ordinary X. */
    if(s->visual.pixels && !s_picture.unavailable && (key.cp>>24)) key.fg=key.bg=0;
    return key;
}

#include "ls_rterm_visual_encode.h"

static size_t cells_render(session_t *s, ls_vt_out_fn out)
{
    cell_sink k = {.out = out};
    /* G resets the viewer's cell arrays, even when the cell counts stay put. */
    const bool full = s->scr.full || s->cfont != s_font_i;
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
    for (int i = 0; i < n;) {
        ls_vt_cell key = cell_feed_key(s, (size_t)i, i % cols, i / cols);
        if (!full && same(&key, &s->shown[i])) { i++; continue; }
        const bool special = key.cp >> 24;
        int last = i, j = i;
        if (!special) {
            for (; j < n; j++) {
                const ls_vt_cell kj = cell_feed_key(s, (size_t)j, j % cols, j / cols);
                if (kj.cp >> 24) break;
                if (full || !same(&kj, &s->shown[j])) last = j;
                else if (j - last > 2) break;
            }
            const int m = last - i + 1;
            cs_u8(&k, 'C'); cs_u16(&k, (unsigned)i); cs_u16(&k, (unsigned)m);
            for (int t = i; t <= last; t++) {
                const ls_vt_cell kt = cell_feed_key(s, (size_t)t, t % cols, t / cols);
                cs_u16(&k, kt.cp & 0xFFFF); cs_u8(&k, kt.cp >> 16 & 0xFF);
                s->shown[t] = kt;
            }
        } else {
            for (; j < n; j++) {
                const ls_vt_cell kj = cell_feed_key(s, (size_t)j, j % cols, j / cols);
                if (!(kj.cp >> 24) || (!full && same(&kj, &s->shown[j]))) break;
                last = j;
            }
            const int m = last - i + 1;
            cs_u8(&k, 'X'); cs_u16(&k, (unsigned)i); cs_u16(&k, (unsigned)m);
            for (int t = i; t <= last; t++) {
                const ls_vt_cell kt = cell_feed_key(s, (size_t)t, t % cols, t / cols);
                cs_u16(&k, kt.cp & 0xFFFF); cs_u8(&k, kt.cp >> 16 & 0xFF); cs_u8(&k, kt.cp >> 24);
                cs_u16(&k, kt.fg); cs_u16(&k, kt.bg);
                s->shown[t] = kt;
            }
        }
        i = last + 1;
    }
    const bool latency = s_snap_seq && s->input_epoch == s_snap_epoch && s->latency_seq != s_snap_seq;
    if (latency) {
        /* Append after cells: old pages stop at unknown records. */
        cs_u8(&k, 'L'); cs_u16(&k, s_snap_seq); cs_u16(&k, s_snap_seq >> 16);
        s->latency_seq = s_snap_seq;
    }
    visual_render(s, &k, full, latency);
    cs_flush(&k);
    /* Palette-only updates also need an end frame and an acknowledgement. */
    return k.total;
}

static void render(session_t *s, ls_vt_out_fn out)
{
    const int64_t now = esp_timer_get_time();
    if (!s->on || !s->shown || now < s->start_us || !s_now_cols) return;
    if (s->acks && s->unacked >= TERM_AHEAD) {
        if (now - s->ack_wait_us < TERM_ACK_US) return;   /* the next one diffs against what it has */
        s->unacked = 0;
    }
    s_sink = s->write;
    const bool z = s->z && z_ready();
    if (z) {
        /* Fast settings: an update is tens of kilobytes of escape codes, and
           greedy matching with few probes still takes most of it out. */
        tdefl_init(s_z, z_put, NULL,
                   picture_deflate_probes(&s->visual) | TDEFL_GREEDY_PARSING_FLAG);
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
static void ws_say(int i, const char *text);

static void session_word(session_t *s, const char *verb, const char *arg, void (*quit)(void))
{
    s->last_in_us = esp_timer_get_time();
    if (!strcmp(verb, "visual") && arg) {
        if(!visual_options(&s->visual,s->cells,s->z,arg)) return;
        s->scr.full=true;s->visual.last_us=0;update_want();
        __atomic_store_n(&s_input_new,true,__ATOMIC_RELEASE);
        return;
    }
    if (!strcmp(verb, "ev") && arg) ls_input_decode_hex(arg);
    if (!strcmp(verb, "lat") && arg) {
        int p50, p95;
        if (sscanf(arg, "%d %d", &p50, &p95) == 2) ls_input_latency_report(p50, p95);
    }
    if (!strcmp(verb, "redraw")) s->scr.full = true;
    if (!strcmp(verb, "size") && arg) {          /* the viewer's window: a grid to fill it */
        int c = 0, r = 0;
        char font[12] = "";
        if (sscanf(arg, "%d %d %11s", &c, &r, font) >= 2 && s_resize) {
            s_grid_oom = false;
            const bool ok = s_resize(c, r, font);
            char reply[64];
            snprintf(reply, sizeof(reply), "term: size %dx%d %s", c, r,
                     ok ? "regrid" : s_grid_oom ? "nomem" : "kept");
            if (s == &s_web) ws_say(s_web_viewer, reply);
            else printf("%s\n", reply);
        }
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
    /* May be requested from the console while UART output is blocking. Only
     * the worker ends the feed/changes baud, after that output completes. */
    s_serial_stop = true;
    if (s_task) xTaskNotifyGive(s_task);
}

/* --- network ----------------------------------------------------------- */

static esp_netif_t *network_netif(void)
{
    /* The default may still be waiting for DHCP while another interface is
     * already usable. Never create a socket before a netif has an address. */
    esp_netif_t *nif = esp_netif_get_default_netif();
    const char *keys[] = {"ETH_DEF", "WIFI_STA_DEF", "WIFI_AP_DEF"};
    for (size_t i = 0; i <= sizeof(keys) / sizeof(keys[0]); i++) {
        esp_netif_ip_info_t ip = {0};
        if (nif && esp_netif_is_netif_up(nif) &&
            esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr) return nif;
        if (i < sizeof(keys) / sizeof(keys[0])) nif = esp_netif_get_handle_from_ifkey(keys[i]);
    }
    return NULL;
}

static int tcp_listen(int port, int backlog)
{
    /* No network up means no TCP/IP stack either, and a socket call then
       asserts inside lwIP (a board with no Wi-Fi saved). Wait for one. */
    if (!network_netif()) return -1;
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
                if (!session_buffers(&s_net)) { net_close(); return; }
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
    heap_caps_free(c->tx.data);
    c->tx = (ls_ws_tx){0};
    c->body = NULL; c->body_n = 0; c->closing = false;
    if (s_web_viewer == i) {
        const ls_input_ev_t release = {.kind = LS_IEV_RELEASE_ALL};
        ls_input_post(&release);
        s_web_viewer = -1;
        s_web.on = false;
        update_want();
    }
}

static void *web_resize(void *p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }

static bool web_queue(int i, const void *b, size_t n)
{
    if (s_wc[i].fd < 0) return false;
    if (!ls_ws_tx_pending(&s_wc[i].tx)) s_wc[i].tx_progress = esp_timer_get_time();
    if (ls_ws_tx_push(&s_wc[i].tx, b, n, web_resize)) return true;
    /* Overflow/OOM must not leave half a delta in the viewer's baseline. */
    wc_close(i);
    return false;
}

static bool ws_send(int i, int op, const void *b, size_t n)
{
    uint8_t head[10];
    size_t h = ls_ws_header(head, op, n);
    return web_queue(i, head, h) && web_queue(i, b, n);
}

static int web_send_now(void *ctx, const void *b, size_t n)
{
    webconn_t *c = ctx;
    int k = send(c->fd, b, n, MSG_DONTWAIT);
    if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    return k > 0 ? k : -1;
}

static void web_flush(int i, int64_t now)
{
    webconn_t *c = &s_wc[i];
    size_t before = ls_ws_tx_pending(&c->tx);
    int result = ls_ws_tx_flush(&c->tx, web_send_now, c, 65536);
    if (result < 0) { wc_close(i); return; }
    if (ls_ws_tx_pending(&c->tx) < before) c->tx_progress = now;
    if (result && c->body_n) {
        size_t n = c->body_n > 65536 ? 65536 : c->body_n;
        int k = web_send_now(c, c->body, n);
        if (k < 0) { wc_close(i); return; }
        c->body += k; c->body_n -= (size_t)k;
        if (k) c->tx_progress = now;
    }
    if ((c->closing && !ls_ws_tx_pending(&c->tx) && !c->body_n) ||
        ((ls_ws_tx_pending(&c->tx) || c->body_n) && now - c->tx_progress > WEB_HEAD_US)) wc_close(i);
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
        if (++c->tries >= 5) c->closing = true;
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
        /* Finish any partially sent frame, then the taken notice, then close.
         * Closing here would discard the queued notice and the old page
         * would reconnect and steal the viewer back from the new one. */
        s_wc[old].closing = true;
        const ls_input_ev_t release = {.kind = LS_IEV_RELEASE_ALL};
        ls_input_post(&release);
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
    s_web.visual.pixels = s_web.visual.vectors = false;
    s_web.cfont = -1;
    s_web.latency_seq = 0;
    s_web.unacked = 0;
    s_web.write = web_write;
    s_web.on = true;
    if (!session_buffers(&s_web)) { ws_say(i, "term: no memory"); return; }
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
        c->closing = true;
        return;
    }
    if (m->op == LS_WS_PING) { ws_send(i, LS_WS_PONG, m->data, m->n); return; }
    if (m->op != LS_WS_TEXT) return;
    char line[512];
    if (m->n >= sizeof(line)) return; /* never apply a truncated edge batch */
    const size_t n = m->n;
    memcpy(line, m->data, n);
    line[n] = 0;
    if (!c->authed) { web_auth(i, line, now); return; }
    if (strncmp(line, "term ", 5)) return;
    char *verb = line + 5, *arg = strchr(verb, ' ');
    if (arg) *arg++ = 0;
    if (!strcmp(verb, "web")) { web_begin(i, arg, now); return; }
    if (!strcmp(verb, "caps") && i == s_web_viewer) {
        s_web.input_epoch = ls_input_begin_session(); s_web.latency_seq = 0;
        /* Older pages match ev1's whole line, rather than tokenizing it. */
        ws_say(i, "term: caps ev1");
        ws_say(i, "term: caps ev1 pix1 vec1"); return;
    }
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
    if (web_queue(i, head, (size_t)n)) s_wc[i].closing = true;
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
        if (!web_queue(i, head, (size_t)n)) return;
        c->ws = true;
        c->n = 0;                     /* a browser sends nothing more until it has the 101 */
        c->since = c->last = now;
        return;
    }
    const web_file_t files[] = {
        {"/", "text/html; charset=utf-8", "no-cache", web_page_start, web_page_end, true},
        {"/screen.html", "text/html; charset=utf-8", "no-cache", web_page_start, web_page_end, true},
#if CONFIG_LS_PANEL_RUNTIME
        /* screen.js explicitly inflates this resource. Do NOT add gzip
         * Content-Encoding: the browser would then decompress it twice. */
        {"/lsfont.bin.gz", "application/gzip", "no-cache", web_font_start, web_font_end, false},
        {"/dejavu-mono.woff2", "font/woff2", "max-age=86400", web_mono_start, web_mono_end, false},
        {"/dejavu-mono-bold.woff2", "font/woff2", "max-age=86400", web_bold_start, web_bold_end, false},
#endif
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
    if (!web_queue(i, head, (size_t)n)) return;
    if (strcmp(method, "HEAD")) { c->body = f->start; c->body_n = size; }
    c->closing = true;
}

static void web_read(int i, int64_t now)
{
    webconn_t *c = &s_wc[i];
    if (c->closing) return;
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
        if (c->closing) break;
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
    esp_netif_t *nif = network_netif();
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
    if (!s_web_enabled || !network_netif()) {
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
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        s_wc[i] = (webconn_t){.fd = fd, .ip = peer.sin_addr.s_addr, .since = now, .last = now, .rx = s_wc[i].rx};
    }
    for (int i = 0; i < WEB_CONNS; i++) {
        if (s_wc[i].fd >= 0) web_read(i, now);
        if (s_wc[i].fd >= 0) web_flush(i, now);
    }
}

/* --- ssh --------------------------------------------------------------- */

static void ssh_out(void *ctx, const char *b, size_t n)
{
    (void)ctx;
    if (s_sshq_over) return;
    if (n > s_sshq_cap - s_sshq_n) { s_sshq_over = true; return; }
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
    xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
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
    if (!session_buffers(&s_ssh)) s_ssh_quit = true;
    update_want();
    const bool ask = !s_ssh.told;
    xSemaphoreGiveRecursive(s_mu);
    return ask;
}

void rterm_ssh_size(int cols, int rows)
{
    xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
    s_ssh.told = true;
    client_size(&s_ssh, cols, rows);
    xSemaphoreGiveRecursive(s_mu);
}

void rterm_ssh_input(const uint8_t *b, size_t n)
{
    xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
    while (n) {
        const size_t k = n > 128 ? 128 : n;
        ls_vt_event ev[32];
        const int m = ls_vt_input_feed(&s_ssh.in, b, k, ev, 32);
        s_ssh.last_in_us = esp_timer_get_time();
        deliver(&s_ssh, ev, m, ssh_quit);
        b += k;
        n -= k;
    }
    xSemaphoreGiveRecursive(s_mu);
}

bool rterm_ssh_quit(void) { return __atomic_exchange_n(&s_ssh_quit, false, __ATOMIC_ACQ_REL); }

void rterm_ssh_end(void)
{
    xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
    s_ssh.on = false;
    update_want();
    s_sshq_n = 0;
    __atomic_store_n(&s_sshq_ready, false, __ATOMIC_RELEASE);
    xSemaphoreGiveRecursive(s_mu);
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
        xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
        s_serial_n = 0; s_serial_over = false;
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
            if (!s_serial_stop) render(&s_ser, frames_out);
            render(&s_net, net_out);
            /* One queued update, no backlog. When it drains, diff the
             * newest snapshot against the last encoded baseline. */
            if (s_web_viewer >= 0 && !ls_ws_tx_pending(&s_wc[s_web_viewer].tx))
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
        const bool stop_serial = s_serial_stop && s_ser.on;
        if (stop_serial) {
            s_sink = serial_stage;
            ls_vt_leave(frames_out, NULL);
            const ls_input_ev_t release = {.kind = LS_IEV_RELEASE_ALL};
            ls_input_post(&release);
        }
        /* If the bounded staging buffer ever fills, send none of this delta;
         * repair from a full baseline next time instead of sending half a frame. */
        if (s_serial_over) { s_serial_n = 0; s_ser.scr.full = true; s_ser.latency_seq = 0; s_ser.unacked = 0; }
        if (!s_want) grid_buffers(0);
        char *tx = s_serial_tx;
        const size_t tx_n = s_serial_n;
        s_serial_inflight = tx;
        char say[sizeof(s_say) + 2] = "";
        if (s_say[0] && s_ser.on && !stop_serial) { snprintf(say, sizeof(say), "\n%s\n", s_say); s_say[0] = 0; }
        xSemaphoreGiveRecursive(s_mu);
        if (tx_n && s_ser_write) s_ser_write(tx, tx_n);
        if (say[0] && s_ser_write) s_ser_write(say, strlen(say));
        xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
        if (tx != s_serial_tx) heap_caps_free(tx);
        s_serial_inflight = NULL;
        xSemaphoreGiveRecursive(s_mu);
        if (stop_serial) {
            if (s_ser_wait) s_ser_wait();
            if (s_ser_baud) s_ser_baud(CONFIG_ESP_CONSOLE_UART_BAUDRATE);
            xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
            s_ser.on = false; s_serial_stop = false; update_want();
            if (s_prev_vprintf) esp_log_set_vprintf(s_prev_vprintf);
            s_prev_vprintf = NULL;
            xSemaphoreGiveRecursive(s_mu);
            printf("\nterm: serial end\n");
        }
    }
}

/* The listener/task once; viewer storage follows the active grid. */
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
    if (!s_mu) s_mu = xSemaphoreCreateRecursiveMutex();
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
    if (s_mu && rx && tcb && stack)
        s_task = xTaskCreateStaticPinnedToCore(worker, "ls_term", TERM_STACK, NULL, 2, stack, tcb, 0);
    if (!s_task) {
        heap_caps_free(tcb); heap_caps_free(stack);
        for (int i = 0; i < WEB_CONNS; i++) { heap_caps_free(s_wc[i].rx); s_wc[i].rx = NULL; }
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
    if (!strcmp(verb, "caps")) {
        if (s_mu) {
            xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
            s_ser.input_epoch = ls_input_begin_session(); s_ser.latency_seq = 0;
            xSemaphoreGiveRecursive(s_mu);
        }
        printf("term: caps ev1\nterm: caps ev1 pix1 vec1\n"); return 0;
    }
    /* Desktop input bypasses the encoder/session mutex entirely. */
    if (!strcmp(verb, "ev")) {
        if (argc == 3) ls_input_decode_hex(argv[2]);
        return 0;
    }
    if (!strcmp(verb, "lat")) {
        if (argc == 4) ls_input_latency_report(atoi(argv[2]), atoi(argv[3]));
        return 0;
    }
    /* The serial session's own traffic first, and silent: anything printed
       would be one more line for the PC to step over. */
    if (!strcmp(verb, "visual")) {
        if(!s_ser.on || !s_mu) return 0;
        char arg[24];snprintf(arg,sizeof(arg),"%s %s",argc>=3?argv[2]:"0",argc>=4?argv[3]:"0");
        xSemaphoreTake(s_mu,portMAX_DELAY);session_word(&s_ser,verb,arg,serial_end);xSemaphoreGive(s_mu);
        return 0;
    }
    if (!strcmp(verb, "in") || !strcmp(verb, "ping") || !strcmp(verb, "redraw") || !strcmp(verb, "ack")) {
        if (!s_ser.on || !s_mu) return 0;
        xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
        session_word(&s_ser, verb, argc >= 3 ? argv[2] : NULL, serial_end);
        xSemaphoreGiveRecursive(s_mu);
        return 0;
    }
    if (!strcmp(verb, "size")) {                     /* 'term size C R': the viewer's window */
        char arg[48];
        snprintf(arg, sizeof(arg), "%s %s %s", argc >= 3 ? argv[2] : "", argc >= 4 ? argv[3] : "", argc >= 5 ? argv[4] : "");
        if (s_mu) xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
        session_word(&s_ser, verb, arg, serial_end);
        if (s_mu) xSemaphoreGiveRecursive(s_mu);
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
        xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
        s_ser.on = true;
        const bool admitted = session_buffers(&s_ser);
        if (!admitted) {
            xSemaphoreGiveRecursive(s_mu);
            printf("term: no memory\n"); return 1;
        }
        LS_PRINTF(t_ls_rterm_term_serial_begin_cols, "term: serial begin cols %d rows %d baud %d%s\n", cols, rows, baud, want_cells ? " cells" : "");
        fflush(stdout);
        if (s_ser_wait) s_ser_wait();
        s_prev_vprintf = esp_log_set_vprintf(term_vprintf);
        s_ser_baud(baud);
        memset(&s_ser.in, 0, sizeof(s_ser.in));
        s_ser.cols = s_ser.rows = 0;
        s_ser.last_in_us = esp_timer_get_time();
        s_ser.start_us = s_ser.last_in_us + 300000;
        s_ser.acks = want_ack;
        s_ser.z = want_z;
        s_ser.cells = want_cells;
        s_ser.visual.pixels = s_ser.visual.vectors = false;
        s_ser.cfont = -1;
        s_ser.latency_seq = 0;
        s_ser.unacked = 0;
        s_ser.write = serial_stage;
        s_serial_stop = false;
        s_ser.on = true;
        update_want();
        xSemaphoreGiveRecursive(s_mu);
        return 0;
    }
    if (!strcmp(verb, "off")) {
        if (!s_mu) return 0;
        xSemaphoreTakeRecursive(s_mu, portMAX_DELAY);
        serial_end();
        xSemaphoreGiveRecursive(s_mu);
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
            esp_netif_t *nif = network_netif();
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
