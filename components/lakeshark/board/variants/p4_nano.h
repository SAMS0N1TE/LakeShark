/* Waveshare ESP32-P4-NANO.  Physical facts only - no capability flags, no
   policy.  ls_caps.h derives LS_HAS_* from what is declared here. */
#ifndef LS_VARIANT_P4_NANO_H
#define LS_VARIANT_P4_NANO_H

#define LS_BOARD_NAME            "ESP32-P4-NANO"
#define LS_BOARD_FLASH_MB        16

/**/
#define LS_BOARD_VBUS_EN_GPIO    46

/**/
#define LS_BOARD_LINK_RX_GPIO    33
#define LS_BOARD_LINK_TX_GPIO    32
#define LS_BOARD_LINK_SCAN_PINS \
    { 33, 32, 45, 47, 48, 0, 1, 2, 3, 6, 4, 5, 20, 21, 22, 23, 36 }

#endif
