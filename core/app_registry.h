/*
 * app_registry.h - multi-tool app framework.
 *
 * Apps register an app_t descriptor at boot. The framework dispatches
 * lifecycle callbacks (enter/exit/sample/key) and keeps track of which
 * app is currently active. UI rendering is opt-in: when CONFIG_ENABLE_TUI
 * is on, the TUI calls draw_main() through this registry; when off, the
 * app runs headless and emits events instead.
 *
 * Switching: app_switch_to() / app_cycle_next() are *non-blocking*. They
 * enqueue a request that a dedicated worker task picks up and runs the
 * old->on_exit() / new->on_enter() pair on. This keeps the TUI task
 * responsive while an app drains its in-flight USB transfers, and gives
 * the apps a single well-defined thread to do their handshake on rather
 * than racing the caller's stack.
 *
 * Display chrome (header banner, page list) can be customised per-app
 * via the optional banner / signal_label / draw_signal fields. Apps
 * that don't set them inherit sensible defaults.
 */
#ifndef APP_REGISTRY_H
#define APP_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAGE_MAIN = 0,
    PAGE_SIGNAL,           /* per-app signal/spectrum view (was WATERFALL) */
    PAGE_LOG,
    PAGE_SETTINGS,
    PAGE_DIAG,             /* per-app radio/decode diagnostics */
    PAGE_COUNT
} page_t;

typedef enum {
    TK_NONE  = 0,
    TK_UP    = 0x81,
    TK_DOWN  = 0x82,
    TK_LEFT  = 0x83,
    TK_RIGHT = 0x84,
    TK_ENTER = '\r',
    TK_ESC   = 0x1b,
    TK_TAB   = '\t',
    TK_BKSP  = 0x7f,
} tui_key_t;

typedef struct app_s {
    const char *name;
    uint32_t    default_freq;
    uint32_t    default_rate;
    int         default_gain;

    /* Optional: header banner string. NULL -> framework default. */
    const char *banner;
    /* Optional: short label for the SIGNAL tab. NULL -> "SIGNAL". */
    const char *signal_label;
    /* Optional: short label for the DIAG tab. NULL -> "DIAG". */
    const char *diag_label;

    void (*on_enter)(void);
    void (*on_exit) (void);
    void (*on_sample)(uint8_t *iq, int len);
    void (*draw_main)(int top_row, int rows, int cols);
    /* Optional: replace the default Mode-S-style signal analyzer on
     * PAGE_SIGNAL. NULL -> framework draws its built-in spectrum view. */
    void (*draw_signal)(int top_row, int rows, int cols);
    /* Optional: render a radio/decode diagnostic page. NULL -> empty
     * page with a "no diagnostics for this app" placeholder. */
    void (*draw_diag)(int top_row, int rows, int cols);
    void (*on_key)(tui_key_t k);
} app_t;

int          app_register(const app_t *desc);
int          app_current_index(void);
const app_t *app_current(void);
const app_t *app_at(int index);
int          app_count(void);

/* Non-blocking switch. Returns immediately; the actual on_exit/on_enter
 * pair runs on a dedicated worker so the caller (typically the TUI key
 * task) is never frozen by an app's drain handshake. */
void         app_switch_to(int index);
void         app_cycle_next(void);
bool         app_switch_in_progress(void);

/* Called once during boot, after all apps are registered, to spin up
 * the switch worker task. Must be called before the first switch. */
void         app_switch_worker_start(void);

page_t       page_current(void);
void         page_set(page_t p);
void         page_cycle_next(void);

tui_key_t    key_feed(uint8_t byte);
tui_key_t    key_flush_timeout(void);

#ifdef __cplusplus
}
#endif

#endif
