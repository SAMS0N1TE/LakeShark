/* Task-safe desktop input. Only the UI drain mutates application state. */
#include "ls_input_ev.h"
#include "ls_tui_touch.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <stddef.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() taskENTER_CRITICAL(&s_lock)
#define UNLOCK() taskEXIT_CRITICAL(&s_lock)
#else
#include <stdatomic.h>
static atomic_flag s_lock = ATOMIC_FLAG_INIT;
#define LOCK() while (atomic_flag_test_and_set_explicit(&s_lock, memory_order_acquire)) {}
#define UNLOCK() atomic_flag_clear_explicit(&s_lock, memory_order_release)
#endif

#define INPUT_N 64
EXT_RAM_BSS_ATTR static ls_input_ev_t s_ring[INPUT_N];
static unsigned s_read, s_count;
/* An out-of-band reset cannot be displaced by a full ring or later producers.
 * Reject new events until it follows all previously accepted edges. */
static bool s_reset;
static uint32_t s_applied;
static bool s_begin;
static uint32_t s_epoch, s_applied_epoch;
static int s_p50, s_p95;
static int64_t s_latency_us;
static bool s_latency_valid;

__attribute__((weak)) bool ls_desk_active(void) { return false; }
__attribute__((weak)) bool ls_desk_input(const ls_input_ev_t *ev) { (void)ev; return false; }
__attribute__((weak)) void ls_input_wake(void) {}

uint32_t ls_input_begin_session(void)
{
    LOCK();
    s_read = s_count = 0; s_reset = false; s_begin = true;
    const uint32_t epoch = ++s_epoch;
    s_latency_valid = false;
    UNLOCK();
    ls_input_wake();
    return epoch;
}

bool ls_input_post(const ls_input_ev_t *ev)
{
    if (!ev || ev->kind > LS_IEV_CONTACT_UP) return false;
    LOCK();
    const bool room = s_count < INPUT_N && !s_reset;
    if (room) s_ring[(s_read + s_count++) % INPUT_N] = *ev;
    else s_reset = true;
    UNLOCK();
    ls_input_wake();                  /* hover, release and overflow wake too */
    return room;
}

extern void ls_lap_input_board(const ls_input_ev_t *) __attribute__((weak));
extern bool ls_lap_input_active(void) __attribute__((weak));
static void fallback(const ls_input_ev_t *e, bool native)
{
    static bool down, moved;
    static int col0, row0, col, row, px, py;
    if (native && ls_lap_input_board) ls_lap_input_board(e);
    switch (e->kind) {
    case LS_IEV_KEY_DOWN: ls_tui_router_key((ls_tk_t)e->key, e->ch); break;
    /* The native runtime has already received the release above. */
    case LS_IEV_KEY_UP: break;
    case LS_IEV_PTR_DOWN:
        if (e->button == LS_IBTN_RIGHT && (!ls_lap_input_active || !ls_lap_input_active())) ls_tui_router_key(LS_TK_ESC, 0);
        if (e->button != LS_IBTN_LEFT) break;
        down = true; moved = false; col0 = e->col; row0 = e->row;
        col = e->col; row = e->row; px = e->px; py = e->py;
        ls_tui_touch_remote(col, row, px, py, true);
        break;
    case LS_IEV_PTR_MOVE:
        if (!down) break;
        if (e->col != col0 || e->row != row0) moved = true;
        col = e->col; row = e->row; px = e->px; py = e->py;
        ls_tui_touch_remote(col, row, px, py, true);
        break;
    case LS_IEV_PTR_UP:
        if (e->button != LS_IBTN_LEFT || !down) break;
        down = false;
        /* Match VT deliver(): only MOVE makes a drag, otherwise tap origin. */
        ls_tui_touch_remote(moved ? e->col : col0, moved ? e->row : row0,
                            moved ? e->px : px, moved ? e->py : py, false);
        if (!moved) ls_tui_router_touch(col0, row0);
        break;
    case LS_IEV_WHEEL:
        for (int i = 0, n = e->wheel < 0 ? -(int)e->wheel : e->wheel; i < n; i++)
            ls_tui_router_key(e->wheel < 0 ? LS_TK_WHEEL_UP : LS_TK_WHEEL_DOWN, 0);
        break;
    case LS_IEV_FOCUS_LOST:
    case LS_IEV_PAUSE:
    case LS_IEV_RELEASE_ALL:
        down = false;
        ls_tui_touch_remote(col, row, px, py, false);
        break;
    default: break;
    }
}

unsigned ls_input_drain(void)
{
    unsigned n = 0;
    /* Bound a drain even if another core continuously produces input. */
    while (n < INPUT_N + 1) {
        ls_input_ev_t e;
        LOCK();
        if (s_begin) {
            /* UI-owned sequence reset: an already popped old event finishes
             * before this barrier, never after the new epoch is applied. */
            memset(&e, 0, sizeof(e)); e.kind = LS_IEV_RELEASE_ALL; s_begin = false;
            __atomic_store_n(&s_applied, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&s_applied_epoch, s_epoch, __ATOMIC_RELEASE);
        } else if (s_count) {
            e = s_ring[s_read]; s_read = (s_read + 1) % INPUT_N; s_count--;
        } else if (s_reset) {
            memset(&e, 0, sizeof(e)); e.kind = LS_IEV_RELEASE_ALL; s_reset = false;
        } else { UNLOCK(); break; }
        UNLOCK();
        if(e.native_pixels) {
            int gx,gy,c,r;
            if(!ls_tui_pixel_to_grid(e.px,e.py,&gx,&gy)) { n++; continue; }
            if(!ls_tui_pixel_to_cell(e.px,e.py,&c,&r))c=r=-1;
            e.px=gx;e.py=gy;e.col=c;e.row=r;e.native_pixels=false;
        }
        /* A screen may ask what its key() and touch() do not say. */
        static uint8_t clicks;
        if (e.kind == LS_IEV_PTR_DOWN && e.button == LS_IBTN_LEFT) clicks = e.clicks;
        ls_tui_input_now(e.kind <= LS_IEV_WHEEL ? e.mods : 0, clicks);
        if (!ls_desk_active() || !ls_desk_input(&e)) fallback(&e, true);
        /* Reset also releases a legacy finger left held before the desk opened. */
        else if (e.kind == LS_IEV_RELEASE_ALL) fallback(&e, false);
        ls_tui_input_now(0, 0);
        if (e.seq && (!s_applied || (int32_t)(e.seq - s_applied) > 0))
            __atomic_store_n(&s_applied, e.seq, __ATOMIC_RELEASE);
        n++;
    }
    return n;
}

uint32_t ls_input_applied_seq(void) { return __atomic_load_n(&s_applied, __ATOMIC_ACQUIRE); }
uint32_t ls_input_applied_epoch(void) { return __atomic_load_n(&s_applied_epoch, __ATOMIC_ACQUIRE); }

void ls_input_latency_report(int p50, int p95)
{
    if (p50 < 0 || p95 < p50 || p95 > 60000) return;
    const int64_t now = esp_timer_get_time();
    LOCK(); s_p50 = p50; s_p95 = p95; s_latency_us = now; s_latency_valid = true; UNLOCK();
}

bool ls_input_latency(int *p50, int *p95)
{
    const int64_t now = esp_timer_get_time();
    LOCK();
    const bool valid = s_latency_valid && now - s_latency_us < 10000000;
    if (valid) { if (p50) *p50 = s_p50; if (p95) *p95 = s_p95; }
    UNLOCK();
    return valid;
}

static unsigned u16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void ls_input_encode(const ls_input_ev_t *e, uint8_t b[LS_INPUT_WIRE_SIZE])
{
    b[0] = LS_INPUT_WIRE_VERSION; b[1] = e->kind; b[2] = e->source; b[3] = e->mods;
    b[4] = e->button; b[5] = e->buttons; b[6] = e->clicks;
    put16(b+7, e->col); put16(b+9, e->row); put16(b+11, e->px); put16(b+13, e->py);
    put16(b+15, e->wheel); put16(b+17, e->key); b[19] = (uint8_t)e->ch;
    put16(b+20, e->seq); put16(b+22, e->seq >> 16);
}
static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 :
           c >= 'A' && c <= 'F' ? c-'A'+10 : -1;
}
bool ls_input_decode_hex(const char *hex)
{
    if (!hex) return false;
    const size_t len = strlen(hex);
    if(len<2)return false;
    int version=hexval(hex[0])*16+hexval(hex[1]);
    if(version!=1 && version!=2)return false;
    size_t bytes=version==2?LS_INPUT_WIRE2_SIZE:LS_INPUT_WIRE_SIZE,stride=bytes*2;
    if (!len || len % stride || len > stride * (version==2?4:LS_INPUT_WIRE_MAX)) return false;
    /* Validate the entire line before posting its first edge. */
    for (size_t i = 0; i < len; i++) if (hexval(hex[i]) < 0) return false;
    for (size_t i = 0; i < len; i += stride)
        if ((hexval(hex[i]) * 16 + hexval(hex[i+1])) != version ||
            (hexval(hex[i+2]) * 16 + hexval(hex[i+3])) > (version==2?LS_IEV_CONTACT_UP:LS_IEV_RELEASE_ALL)) return false;
    if(version==2)for(size_t i=0;i<len;i+=stride) {
        uint8_t b[LS_INPUT_WIRE2_SIZE];
        for(unsigned j=0;j<sizeof(b);j++)b[j]=hexval(hex[i+j*2])*16+hexval(hex[i+j*2+1]);
        uint32_t id=u16(b+24)|(uint32_t)u16(b+26)<<16;
        if(b[2]!=LS_ISRC_TERM || b[30]>15 || b[31]!=1 || b[50] || b[51] ||
           (int8_t)b[48]<-1 || (int8_t)b[48]>1 || (int8_t)b[49]<-1 || (int8_t)b[49]>1 ||
           (b[1]>LS_IEV_RESIZE && id<0x30000) || id>=0x40000 ||
           (int16_t)u16(b+40)<0 || (int16_t)u16(b+42)<0)return false;
        for(unsigned a=0;a<4;a++)if(u16(b+32+a*2)==0x8000u)return false;
    }
    for (size_t i = 0; i < len; i += stride) {
        uint8_t b[LS_INPUT_WIRE2_SIZE]={0};
        for (unsigned j = 0; j < bytes; j++) b[j] = (uint8_t)(hexval(hex[i+j*2])*16 + hexval(hex[i+j*2+1]));
        ls_input_ev_t e = {
            .kind=b[1], .source=b[2], .mods=b[3], .button=b[4], .buttons=b[5], .clicks=b[6],
            .col=(int16_t)u16(b+7), .row=(int16_t)u16(b+9), .px=(int16_t)u16(b+11), .py=(int16_t)u16(b+13),
            .wheel=(int16_t)u16(b+15), .key=(uint16_t)u16(b+17), .ch=(char)b[19],
            .seq=(uint32_t)u16(b+20) | (uint32_t)u16(b+22)<<16,
        };
        if(version==2) {
            e.device=u16(b+24)|(uint32_t)u16(b+26)<<16;e.contact=u16(b+28);
            e.classes=b[30];e.transport=b[31];
            for(unsigned a=0;a<6;a++)e.axes[a]=(int16_t)u16(b+32+a*2);
            e.gamepad_buttons=u16(b+44)|(uint32_t)u16(b+46)<<16;
            e.dpad_x=(int8_t)b[48];e.dpad_y=(int8_t)b[49];
        }
        ls_input_post(&e);
    }
    return true;
}
