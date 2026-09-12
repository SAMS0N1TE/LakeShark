/* TCA8418 keypad on the T-Display-P4 keyboard expansion. */

#ifndef LS_KEYPAD_H
#define LS_KEYPAD_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t row;      /* 0 .. LS_BOARD_KEYPAD_ROWS-1 */
    uint8_t col;      /* 0 .. LS_BOARD_KEYPAD_COLS-1 */
    bool    pressed;  /* false is the release of the same key */
} ls_keypad_event_t;

esp_err_t ls_keypad_start(void);

/* True once start() has found the part. */
bool ls_keypad_present(void);

/* Ask the bus again whether the keyboard is there. */

void ls_keypad_tick(void);

/* Next queued event, or false when the FIFO is empty.  Non-blocking. */
bool ls_keypad_read(ls_keypad_event_t *out);

/* Keyboard backlight, 0 disables.  Board declares the pin; boards without one
   return ESP_ERR_NOT_SUPPORTED. */
esp_err_t ls_keypad_backlight(bool on);

/* The backlight's switching rate and depth, changeable while it runs. */

esp_err_t ls_keypad_backlight_tune(uint32_t freq_hz, int duty_1024);
uint32_t  ls_keypad_backlight_freq(void);   /* 0 when it has never been on */
int       ls_keypad_backlight_duty(void);

/* Console helper: probe the bus and describe what answered. */
void ls_keypad_diagnostics(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_KEYPAD_H */
