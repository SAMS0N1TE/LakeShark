#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TK21_U11 = 1u << 0, TK21_U10 = 1u << 1, TK21_U5 = 1u << 2,
    TK21_U7 = 1u << 3, TK21_U8 = 1u << 4, TK21_U6 = 1u << 5, TK21_KEY = 1u << 6
};

typedef struct {
    int x_gpio, y_gpio, buttons_gpio, key_gpio;
    bool invert_x, invert_y;
} tk21_pinout;

typedef struct {
    float x, y;             /* normalized to [-1, 1], after dead zone */
    uint8_t held, pressed;  /* pressed contains rising edges since the last poll */
} tk21_state;

typedef struct {
    int axis[2], center[2];  /* electrical X/Y before inversion */
    int ladder_mv;
} tk21_raw_state;

/* One controller per firmware. Initialize once with the stick released.
   The component owns the ADC oneshot units used by these pins. */
esp_err_t tk21_init(const tk21_pinout *pins);
tk21_state tk21_poll(void);
/* Latest filtered ADC counts and boot center, without a second ADC read. */
tk21_raw_state tk21_read_raw(void);
void tk21_log(void);

#ifdef __cplusplus
}
#endif
