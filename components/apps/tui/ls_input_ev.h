/* Desktop input events: what a PC window, lsterm or a Bluetooth keyboard/mouse
 * hands the UI, with the edges the touch path drops (key up, hover, which button,
 * wheel steps, modifiers). Producers queue them; the UI task delivers them.
 *
 * Contract between the input side (ls_vt / ls_rterm / BLE HID) and the desk:
 * - ls_input_post() may be called from any task; it never blocks and wakes the UI.
 * - On the UI task each event goes to ls_desk_input() first. When the desk is not
 *   running or returns false, the event falls back to today's path
 *   (ls_tui_router_key, ls_tui_router_touch, ls_tui_touch_remote).
 * - Positions are grid cells plus grid pixels (the same pixels ls_tui_touch_remote takes).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ls_tui_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_IEV_KEY_DOWN,      /* key/ch set; clicks=0 is a producer auto-repeat */
    LS_IEV_KEY_UP,
    LS_IEV_PTR_MOVE,      /* hover or drag; buttons says which are held */
    LS_IEV_PTR_DOWN,      /* button says which one changed */
    LS_IEV_PTR_UP,
    LS_IEV_WHEEL,         /* wheel = signed steps, + is down */
    LS_IEV_RELEASE_ALL,   /* focus lost or source gone: drop every held key and button */
} ls_iev_kind_t;

enum { LS_IBTN_LEFT = 0, LS_IBTN_MIDDLE = 1, LS_IBTN_RIGHT = 2 };
enum { LS_IMOD_SHIFT = 1, LS_IMOD_ALT = 2, LS_IMOD_CTRL = 4, LS_IMOD_META = 8 };
enum { LS_ISRC_TERM = 0, LS_ISRC_BLE = 1, LS_ISRC_LOCAL = 2 };

typedef struct {
    uint8_t  kind;        /* ls_iev_kind_t */
    uint8_t  source;      /* LS_ISRC_* */
    uint8_t  mods;        /* LS_IMOD_* held at the time */
    uint8_t  button;      /* LS_IBTN_* for PTR_DOWN/UP */
    uint8_t  buttons;     /* bit per LS_IBTN_* held after this event */
    uint8_t  clicks;      /* 2 on second pointer press; KEY_DOWN repeat=0, initial=1 */
    int16_t  col, row;    /* pointer cell */
    int16_t  px, py;      /* pointer in grid pixels */
    int16_t  wheel;
    uint16_t key;         /* ls_tk_t for KEY_* */
    char     ch;          /* printable character for LS_TK_CHAR, else 0 */
    uint32_t seq;         /* producer sequence, echoed for the latency meter; 0 = none */
} ls_input_ev_t;

/* Queue an event for the UI task. False when the queue is full (a RELEASE_ALL follows). */
bool ls_input_post(const ls_input_ev_t *ev);

/* UI task: drain once before drawing. Returns number delivered (including reset).
 * applied_seq is sampled only AFTER the UI finishes drawing/presenting a frame. */
unsigned ls_input_drain(void);
uint32_t ls_input_applied_seq(void);
/* A new remote viewer discards the previous session's pending input and queues
 * a release/reset barrier. Sequence counters may restart. The returned epoch
 * must match applied_epoch at frame capture before echoing a sequence. */
uint32_t ls_input_begin_session(void);
uint32_t ls_input_applied_epoch(void);
/* Platform hook: give the same UI wake semaphore used by injected keys. */
void ls_input_wake(void);

/* `term ev <hex>`: 1..10 concatenated 24-byte records, no native struct padding.
 * Every record starts with version=1. All multibyte fields are little-endian:
 * offset  0:u8 version, 1:u8 kind, 2:u8 source, 3:u8 mods, 4:u8 button,
 *         5:u8 buttons (bit 0 left, 1 middle, 2 right), 6:u8 clicks,
 *         7:i16 col, 9:i16 row, 11:i16 px, 13:i16 py, 15:i16 wheel,
 *        17:u16 key, 19:u8 ch, 20:u32 seq. Cells/pixels are zero-based.
 * ch is one byte (the existing LS_TK_CHAR API). seq=0 disables measurement.
 * Malformed lines (including unknown versions/kinds) enqueue nothing. Valid
 * lines may overflow the queue, in which case ls_input_post's reset rule applies.
 * A cell update ends with optional 'L', u32 applied seq (little-endian), after
 * all G/P/C/X records, so old pages may stop at L without losing any cells.
 * `term caps` replies `term: caps ev1`; `term lat <p50> <p95>` reports integer ms.
 */
#define LS_INPUT_WIRE_VERSION 1
#define LS_INPUT_WIRE_SIZE 24
#define LS_INPUT_WIRE_MAX 10
bool ls_input_decode_hex(const char *hex);
void ls_input_encode(const ls_input_ev_t *ev, uint8_t out[LS_INPUT_WIRE_SIZE]);

void ls_input_latency_report(int p50_ms, int p95_ms);
bool ls_input_latency(int *p50_ms, int *p95_ms);

/* Desk hooks, weak in the core: a build without the desk answers false. */
bool ls_desk_active(void);
bool ls_desk_input(const ls_input_ev_t *ev);

#ifdef __cplusplus
}
#endif
