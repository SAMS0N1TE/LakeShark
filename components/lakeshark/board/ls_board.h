#ifndef LS_BOARD_H
#define LS_BOARD_H

/*LS-001  The board layer.  Three things live here and nowhere else:

     variants/<board>.h   physical facts - pins, panel, flash size
     this file            which variant is selected, plus shared P4 pins
     ls_caps.h            LS_HAS_* capabilities, all derived

   The rule the rest of the tree lives by: no file outside this directory may
   test CONFIG_LS_BOARD_*.  Test an LS_HAS_* capability instead.  Adding a
   board is then one header and one Kconfig entry - never a branch.
   See docs/PORTING.md and board/TEMPLATE.h. */

#include "sdkconfig.h"

/* ---- Which board.  LS_BOARD_SELECTED says whether the choice was explicit;
   main.cpp uses it to shout when an sdkconfig has no CONFIG_LS_BOARD_* line
   at all and silently fell back to the NANO pin map.  Deriving it here means
   adding a board does not require editing that diagnostic. */
#if defined(CONFIG_LS_BOARD_P4_WIFI6)
#include "variants/p4_wifi6.h"
#define LS_BOARD_SELECTED 1
#elif defined(CONFIG_LS_BOARD_P4_TOUCH_LCD_4B)
#include "variants/p4_touch_lcd_4b.h"
#define LS_BOARD_SELECTED 1
#elif defined(CONFIG_LS_BOARD_P4_TOUCH_LCD_43)
#include "variants/p4_touch_lcd_43.h"
#define LS_BOARD_SELECTED 1
#elif defined(CONFIG_LS_BOARD_P4_NANO)
#include "variants/p4_nano.h"
#define LS_BOARD_SELECTED 1
#else
/*LS-727  No board chosen.  Fall back to the NANO so a stray build still
   links, but mark it so the boot log can say so in the loudest terms. */
#include "variants/p4_nano.h"
#define LS_BOARD_SELECTED 0
#endif

/* ---- Shared ESP32-P4 wiring.  Identical on every board carried so far, so
   it lives here rather than being copied into each variant.  A variant that
   differs simply defines the pin itself - these only fill in the gaps. */

#ifndef LS_BOARD_I2C_SDA_GPIO
#define LS_BOARD_I2C_SDA_GPIO    7
#endif
#ifndef LS_BOARD_I2C_SCL_GPIO
#define LS_BOARD_I2C_SCL_GPIO    8
#endif

#ifndef LS_BOARD_I2S_MCLK_GPIO
#define LS_BOARD_I2S_MCLK_GPIO   13
#endif
#ifndef LS_BOARD_I2S_BCK_GPIO
#define LS_BOARD_I2S_BCK_GPIO    12
#endif
#ifndef LS_BOARD_I2S_WS_GPIO
#define LS_BOARD_I2S_WS_GPIO     10
#endif
#ifndef LS_BOARD_I2S_DOUT_GPIO
#define LS_BOARD_I2S_DOUT_GPIO   9
#endif
#ifndef LS_BOARD_I2S_DIN_GPIO
#define LS_BOARD_I2S_DIN_GPIO    11
#endif
#ifndef LS_BOARD_PA_EN_GPIO
#define LS_BOARD_PA_EN_GPIO      53
#endif

#ifndef LS_BOARD_C6_EN_GPIO
#define LS_BOARD_C6_EN_GPIO      54
#endif

#ifndef LS_BOARD_SDIO_CLK_GPIO
#define LS_BOARD_SDIO_CLK_GPIO   18
#endif
#ifndef LS_BOARD_SDIO_CMD_GPIO
#define LS_BOARD_SDIO_CMD_GPIO   19
#endif
#ifndef LS_BOARD_SDIO_D0_GPIO
#define LS_BOARD_SDIO_D0_GPIO    14
#endif
#ifndef LS_BOARD_SDIO_D1_GPIO
#define LS_BOARD_SDIO_D1_GPIO    15
#endif
#ifndef LS_BOARD_SDIO_D2_GPIO
#define LS_BOARD_SDIO_D2_GPIO    16
#endif
#ifndef LS_BOARD_SDIO_D3_GPIO
#define LS_BOARD_SDIO_D3_GPIO    17
#endif

#ifndef LS_BOARD_BOOT_BTN_GPIO
#define LS_BOARD_BOOT_BTN_GPIO   35
#endif

/*LS-002  A build may override the variant's VBUS pin from sdkconfig - that
   is how lcd43_gui.defaults asserts -1.  Applied after the variant so the
   explicit build-time choice wins. */
#ifdef CONFIG_LS_VBUS_EN_GPIO
#undef  LS_BOARD_VBUS_EN_GPIO
#define LS_BOARD_VBUS_EN_GPIO    CONFIG_LS_VBUS_EN_GPIO
#endif

#include "ls_caps.h"

#endif
