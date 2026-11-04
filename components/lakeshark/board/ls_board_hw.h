/* Early board hardware bring-up. */

#ifndef LS_BOARD_HW_H
#define LS_BOARD_HW_H

#include <stdbool.h>
#include <stdint.h>

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

/* The GPS module awake (true) or in its standby (false), where the board
   wires its WAKE_UP pin. Standby keeps the module's time and ephemeris, so
   the next fix is a hot start; tracking draws about 23 mA, standby about
   20 uA. ESP_ERR_NOT_SUPPORTED on a board without the pin. */
esp_err_t ls_board_hw_gps_wake(bool awake);

/* 1 enabled, 0 held in reset, -1 when the board cannot tell. */
int ls_board_hw_c6_state(void);

/* Which antenna the SKY13453 is pointing at. */

esp_err_t ls_board_hw_antenna_external(bool external);
/* Same as ls_board_hw_antenna_external(true); kept for older callers. */
esp_err_t ls_board_hw_antenna_confirm_external(void);
/* Always true: the route never blocks a transmit. Kept for older callers. */
bool ls_board_hw_antenna_tx_allowed(void);

/* MMCX1 is always receive-only on a fitted LR2021/LR20xx. The legacy
   transmit option cannot override that physical port constraint. On other
   supported socket routes false selects receive-only (the default). With
   MMCX1 selected and receive only, the LoRa driver brackets each transmit
   with tx_begin (switch to internal; an error means do not transmit) and
   tx_end (back to MMCX1 once the part reports it done), under its lock.
   ls_board_hw_antenna_is_external keeps saying the operator's choice. */
void ls_board_hw_antenna_set_mmcx_tx(bool tx);
bool ls_board_hw_antenna_mmcx_tx(void);
esp_err_t ls_board_hw_antenna_tx_begin(void);
void ls_board_hw_antenna_tx_end(void);
/* Since boot: transmits moved to internal, and returns to MMCX1. */
void ls_board_hw_antenna_tx_counts(uint32_t *moved, uint32_t *returned);

/* True when MMCX1 is selected. False for the internal antenna, and false on
   a board that has no switch - which is correct, since such a board's signal
   goes wherever it is soldered. */
bool ls_board_hw_antenna_is_external(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_BOARD_HW_H */
