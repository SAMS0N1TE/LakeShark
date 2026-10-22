/* Waveshare ESP32-P4-NANO.  Physical facts only - no capability flags, no
   policy.  ls_caps.h derives LS_HAS_* from what is declared here. */
#ifndef LS_VARIANT_P4_NANO_H
#define LS_VARIANT_P4_NANO_H

#define LS_BOARD_NAME            "ESP32-P4-NANO"
#define LS_BOARD_FLASH_MB        16
/* ES8311 speaker codec only; the BSP's ES7210 microphone part is not fitted. */
#define LS_BOARD_NO_MIC          1

/* TF slot on IDF slot 0, 43/44/39-42 (Waveshare BSP esp32_p4_nano.h). The
   shared default 18/19/14-17 is the C6 link on slot 1. Those pins sit on the
   VDD_IO_5 rail that the chip's LDO channel 4 feeds; nothing else powers it
   on this headless board, so ls_sdcard turns it on. */
#define LS_BOARD_SDIO_CLK_GPIO   43
#define LS_BOARD_SDIO_CMD_GPIO   44
#define LS_BOARD_SDIO_D0_GPIO    39
#define LS_BOARD_SDIO_D1_GPIO    40
#define LS_BOARD_SDIO_D2_GPIO    41
#define LS_BOARD_SDIO_D3_GPIO    42
#define LS_BOARD_SDCARD_LDO_CHAN 4

/* The codec, a DSI panel's adapter (0x45) and its GT911 all sit on SDA7/SCL8,
   where the vendored BSP already runs I2C1; ls_i2c shares that bus. */
#define LS_BOARD_I2C_FROM_BSP    1

/* MIPI-DSI connector, 2 lanes, no reset or backlight pin: a Waveshare DSI
   panel is chosen on the console ('panel') and driven by ls_panel_ws.c. */
#if defined(CONFIG_LS_PANEL_RUNTIME) && CONFIG_LS_PANEL_RUNTIME
#define LS_BOARD_PANEL_RUNTIME   1
#endif

/**/
#define LS_BOARD_VBUS_EN_GPIO    46

/**/
#define LS_BOARD_LINK_RX_GPIO    33
#define LS_BOARD_LINK_TX_GPIO    32
#define LS_BOARD_LINK_SCAN_PINS \
    { 33, 32, 45, 47, 48, 0, 1, 2, 3, 6, 4, 5, 20, 21, 22, 23, 36 }

#endif
