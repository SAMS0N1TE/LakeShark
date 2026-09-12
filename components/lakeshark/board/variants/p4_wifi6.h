/* Waveshare ESP32-P4-WIFI6.  Physical facts only. */
#ifndef LS_VARIANT_P4_WIFI6_H
#define LS_VARIANT_P4_WIFI6_H

#define LS_BOARD_NAME            "ESP32-P4-WIFI6"
#define LS_BOARD_FLASH_MB        32

/**/
#define LS_BOARD_VBUS_EN_GPIO    46

/**/
#define LS_BOARD_LINK_RX_GPIO    33
#define LS_BOARD_LINK_TX_GPIO    32
#define LS_BOARD_LINK_SCAN_PINS \
    { 33, 32, 27, 26, 23, 22, 21, 20, 2, 3, 4, 5, 28, 29, 30, 31, 46, 47, 48, 49, 50, 51, 52 }

#endif
