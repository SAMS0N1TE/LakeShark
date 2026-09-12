/* Waveshare ESP32-P4-WIFI6-Touch-LCD-4B, the "Smart 86 Box".
   Physical facts only. */
#ifndef LS_VARIANT_P4_TOUCH_LCD_4B_H
#define LS_VARIANT_P4_TOUCH_LCD_4B_H

#define LS_BOARD_NAME            "ESP32-P4-WIFI6-Touch-LCD-4B"
#define LS_BOARD_FLASH_MB        32
/* existing Waveshare battery-divider measurement, GPIO20. */
#define LS_BOARD_BATTERY_ADC_CHANNEL 4
#define LS_BOARD_BATTERY_DIVIDER 3

/* No software VBUS control; the port is hard-powered. */
#define LS_BOARD_VBUS_EN_GPIO    (-1)

#define LS_BOARD_LCD_H_RES       720
#define LS_BOARD_LCD_V_RES       720
#define LS_BOARD_LCD_BL_GPIO     26
#define LS_BOARD_LCD_RST_GPIO    27
#define LS_BOARD_TOUCH_RST_GPIO  23
#define LS_BOARD_TOUCH_INT_GPIO  (-1)

/**/
#define LS_BOARD_LINK_RX_GPIO    33
#define LS_BOARD_LINK_TX_GPIO    32
#define LS_BOARD_LINK_SCAN_PINS \
    { 33, 32, 20, 21, 22, 2, 3, 4, 5 }

#endif
