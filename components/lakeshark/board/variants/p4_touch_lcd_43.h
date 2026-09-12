/* Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3.  Physical facts only.
   written from the product page, board not in hand at the time.
   SETTLED - this board has NO software VBUS control.  Do not guess
   GPIO46 here; on this board 46 is the audio PA enable in Waveshare's own
   06_I2SCodec example.  See README for the power-path notes. */
#ifndef LS_VARIANT_P4_TOUCH_LCD_43_H
#define LS_VARIANT_P4_TOUCH_LCD_43_H

#define LS_BOARD_NAME            "ESP32-P4-WIFI6-Touch-LCD-4.3"
#define LS_BOARD_FLASH_MB        32
/* existing Waveshare battery-divider measurement, GPIO20. */
#define LS_BOARD_BATTERY_ADC_CHANNEL 4
#define LS_BOARD_BATTERY_DIVIDER 3

/**/
#define LS_BOARD_VBUS_EN_GPIO    (-1)

#define LS_BOARD_LCD_H_RES       480
#define LS_BOARD_LCD_V_RES       800
#define LS_BOARD_LCD_BL_GPIO     26
#define LS_BOARD_LCD_RST_GPIO    27
#define LS_BOARD_TOUCH_RST_GPIO  23
#define LS_BOARD_TOUCH_INT_GPIO  (-1)

#define LS_BOARD_HAS_PWR_BTN     1

/**/
#define LS_BOARD_LINK_RX_GPIO    33
#define LS_BOARD_LINK_TX_GPIO    32
#define LS_BOARD_LINK_SCAN_PINS \
    { 33, 32, 20, 21, 22, 2, 3, 4, 5 }

#endif
