/* Early board hardware bring-up. */

#ifndef LS_BOARD_HW_H
#define LS_BOARD_HW_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the IO expander and drive every rail and reset the board
   declares.  Returns ESP_OK on a board without one. */
esp_err_t ls_board_hw_early_init(void);
esp_err_t ls_board_hw_touch_reset(void);

/* True when this board needs an expander and it answered.  A board that
   declares none reports true, because nothing is missing. */
bool ls_board_hw_ready(void);

/* ESP32-C6 enable, wherever it happens to live. */

esp_err_t ls_board_hw_c6_enable(bool on);

/* Release EN to the board pull-up so the physical reset button can drive it. */
esp_err_t ls_board_hw_c6_release(void);

/* 1 enabled, 0 held in reset, -1 when the board cannot tell. */
int ls_board_hw_c6_state(void);

/* Which antenna the SKY13453 is pointing at. */

esp_err_t ls_board_hw_antenna_external(bool external);

/* True when MMCX1 is selected. False for the internal antenna, and false on
   a board that has no switch - which is correct, since such a board's signal
   goes wherever it is soldered. */
bool ls_board_hw_antenna_is_external(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_BOARD_HW_H */
