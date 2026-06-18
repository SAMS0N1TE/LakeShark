
#ifndef APP_REGISTRY_H
#define APP_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAGE_MAIN = 0,
    PAGE_SIGNAL,
    PAGE_LOG,
    PAGE_SETTINGS,
    PAGE_DIAG,
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

    const char *banner;

    const char *signal_label;

    const char *diag_label;

    void (*on_enter)(void);
    void (*on_exit) (void);
    void (*on_sample)(uint8_t *iq, int len);
    void (*draw_main)(int top_row, int rows, int cols);

    void (*draw_signal)(int top_row, int rows, int cols);

    void (*draw_diag)(int top_row, int rows, int cols);
    void (*on_key)(tui_key_t k);
} app_t;

int          app_register(const app_t *desc);
int          app_current_index(void);
const app_t *app_current(void);
const app_t *app_at(int index);
int          app_count(void);

void         app_switch_to(int index);
void         app_cycle_next(void);
bool         app_switch_in_progress(void);

void         app_park(void);
void         app_unpark(void);
bool         app_parked(void);
void         app_request_recover(void);
void         app_set_usb_autoreboot(bool en);
bool         app_usb_autoreboot(void);
const char  *app_recovery_take(void);

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
