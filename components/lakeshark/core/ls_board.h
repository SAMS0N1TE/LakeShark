#ifndef LS_BOARD_H
#define LS_BOARD_H

/*LS-001*/

#include "sdkconfig.h"

#if defined(CONFIG_LS_BOARD_P4_WIFI6)
#define LS_BOARD_NAME            "ESP32-P4-WIFI6"
#elif defined(CONFIG_LS_BOARD_P4_TOUCH_LCD_4B)
#define LS_BOARD_NAME            "ESP32-P4-WIFI6-Touch-LCD-4B"
#elif defined(CONFIG_LS_BOARD_P4_TOUCH_LCD_43)
#define LS_BOARD_NAME            "ESP32-P4-WIFI6-Touch-LCD-4.3"
#else
#define CONFIG_LS_BOARD_P4_NANO  1
#define LS_BOARD_NAME            "ESP32-P4-NANO"
#endif

#define LS_BOARD_I2C_SDA_GPIO    7
#define LS_BOARD_I2C_SCL_GPIO    8

#define LS_BOARD_I2S_MCLK_GPIO   13
#define LS_BOARD_I2S_BCK_GPIO    12
#define LS_BOARD_I2S_WS_GPIO     10
#define LS_BOARD_I2S_DOUT_GPIO   9
#define LS_BOARD_I2S_DIN_GPIO    11
#define LS_BOARD_PA_EN_GPIO      53

#define LS_BOARD_C6_EN_GPIO      54

#define LS_BOARD_SDIO_CLK_GPIO   18
#define LS_BOARD_SDIO_CMD_GPIO   19
#define LS_BOARD_SDIO_D0_GPIO    14
#define LS_BOARD_SDIO_D1_GPIO    15
#define LS_BOARD_SDIO_D2_GPIO    16
#define LS_BOARD_SDIO_D3_GPIO    17

#define LS_BOARD_BOOT_BTN_GPIO   35

/*LS-002*/
#ifdef CONFIG_LS_VBUS_EN_GPIO
#define LS_BOARD_VBUS_EN_GPIO    CONFIG_LS_VBUS_EN_GPIO
#endif

#if defined(CONFIG_LS_BOARD_P4_NANO)

#ifndef LS_BOARD_VBUS_EN_GPIO
#define LS_BOARD_VBUS_EN_GPIO    46
#endif
#define LS_BOARD_HAS_DISPLAY     0
#define LS_BOARD_HAS_POWER_BTN   0
#define LS_BOARD_FLASH_MB        16

/*LS-003*/
#define LS_BOARD_LINK_RX_GPIO    33
#define LS_BOARD_LINK_TX_GPIO    32
#define LS_BOARD_LINK_SCAN_PINS \
    { 33, 32, 45, 47, 48, 0, 1, 2, 3, 6, 4, 5, 20, 21, 22, 23, 36 }

#elif defined(CONFIG_LS_BOARD_P4_WIFI6)

#ifndef LS_BOARD_VBUS_EN_GPIO
#define LS_BOARD_VBUS_EN_GPIO    46
#endif
#define LS_BOARD_HAS_DISPLAY     0
#define LS_BOARD_HAS_POWER_BTN   0
#define LS_BOARD_FLASH_MB        32

/*LS-003*/
#define LS_BOARD_LINK_RX_GPIO    33
#define LS_BOARD_LINK_TX_GPIO    32
#define LS_BOARD_LINK_SCAN_PINS \
    { 33, 32, 27, 26, 23, 22, 21, 20, 2, 3, 4, 5, 28, 29, 30, 31, 46, 47, 48, 49, 50, 51, 52 }

#elif defined(CONFIG_LS_BOARD_P4_TOUCH_LCD_4B)

#ifndef LS_BOARD_VBUS_EN_GPIO
#define LS_BOARD_VBUS_EN_GPIO    (-1)
#endif
#define LS_BOARD_HAS_DISPLAY     1
#define LS_BOARD_HAS_POWER_BTN   0
#define LS_BOARD_FLASH_MB        32

#define LS_BOARD_LCD_H_RES       720
#define LS_BOARD_LCD_V_RES       720
#define LS_BOARD_LCD_BL_GPIO     26
#define LS_BOARD_LCD_RST_GPIO    27
#define LS_BOARD_TOUCH_RST_GPIO  23
#define LS_BOARD_TOUCH_INT_GPIO  (-1)

#define LS_BOARD_LINK_RX_GPIO    33
#define LS_BOARD_LINK_TX_GPIO    32
#define LS_BOARD_LINK_SCAN_PINS \
    { 33, 32, 20, 21, 22, 2, 3, 4, 5 }

#else

/*LS-902*/
#ifndef LS_BOARD_VBUS_EN_GPIO
#define LS_BOARD_VBUS_EN_GPIO    (-1)
#endif
#define LS_BOARD_HAS_DISPLAY     1
#define LS_BOARD_HAS_POWER_BTN   1
#define LS_BOARD_FLASH_MB        32

#define LS_BOARD_LCD_H_RES       480
#define LS_BOARD_LCD_V_RES       800
#define LS_BOARD_LCD_BL_GPIO     26
#define LS_BOARD_LCD_RST_GPIO    27
#define LS_BOARD_TOUCH_RST_GPIO  23
#define LS_BOARD_TOUCH_INT_GPIO  (-1)

#define LS_BOARD_LINK_RX_GPIO    33
#define LS_BOARD_LINK_TX_GPIO    32
#define LS_BOARD_LINK_SCAN_PINS \
    { 33, 32, 20, 21, 22, 2, 3, 4, 5 }

#endif

#define LS_BOARD_HAS_VBUS_CTRL   (LS_BOARD_VBUS_EN_GPIO >= 0)

#endif
