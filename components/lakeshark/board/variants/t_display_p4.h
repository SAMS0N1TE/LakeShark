/* LilyGO T-Display-P4. */

#ifndef LS_VARIANT_T_DISPLAY_P4_H
#define LS_VARIANT_T_DISPLAY_P4_H

#define LS_BOARD_NAME            "T-Display-P4"
#define LS_BOARD_FLASH_MB        16

/* ------------------------------------------------------------- overrides */

/* I2S to the ES8311.  MCLK 13 and BCK 12 match the shared default; WS and
   DOUT do not, and inheriting them silently swaps the word-clock with the
   data line.  vendor gpio::es8311: kWsLrck = 9, kDacData = 10, kAdcData = 11. */
#define LS_BOARD_I2S_WS_GPIO     9
#define LS_BOARD_I2S_DOUT_GPIO   10

#define LS_BOARD_I2S_DIN_GPIO    11

/* The ES8311's CONTROL interface is on I2C port 2 (SDA 20 / SCL 21),
   not the port 1 the shared LS_BOARD_I2C_* default describes.  Port 1 (7/8)
   is right for the XL9535, the touch controllers and the RTC, so the shared
   default stays and the codec gets the second bus - do not "fix" this by
   moving the shared pins.
     vendor gpio::i2c: kPort1Sda = 7,  kPort1Scl = 8
                       kPort2Sda = 20, kPort2Scl = 21 */
#define LS_BOARD_I2C2_SDA_GPIO   20
#define LS_BOARD_I2C2_SCL_GPIO   21

#define LS_BOARD_IO_EXPANDER_ADDR   0x20
#define LS_BOARD_IO_EXPANDER_BUS    LS_I2C_PRIMARY

#define LS_BOARD_XL_PWR_3V3_EN      LS_XL9535_IO0   /* LOW_EN       */
#define LS_BOARD_XL_RF_SW_VCTL      LS_XL9535_IO1   /* PWDN         */
#define LS_BOARD_XL_SCREEN_RST      LS_XL9535_IO2   /* DISPLAY_RSTN */
#define LS_BOARD_XL_TOUCH_RST       LS_XL9535_IO3   /* TP_RSTN      */
#define LS_BOARD_XL_TOUCH_INT       LS_XL9535_IO4   /* TP_INT       */
#define LS_BOARD_XL_ETH_PHY_RST     LS_XL9535_IO5   /* PHY_RSTN     */
#define LS_BOARD_XL_AUDIO_PWR_EN    LS_XL9535_IO6   /* PWR_EN       */
#define LS_BOARD_XL_SENSOR_INT      LS_XL9535_IO7   /* Sensor_INT   */
#define LS_BOARD_XL_USB_PHY_EN      LS_XL9535_IO10  /* VCC_EN       */
#define LS_BOARD_XL_GPS_WAKE        LS_XL9535_IO11  /* GPS_WAKE_UP  */
#define LS_BOARD_XL_RTC_INT         LS_XL9535_IO12  /* Time_INT     */
#define LS_BOARD_XL_C6_WAKE         LS_XL9535_IO13  /* C6_WAKEUP    */
#define LS_BOARD_XL_C6_EN           LS_XL9535_IO14  /* C6_ESP_EN    */
#define LS_BOARD_XL_SD_PWR_EN       LS_XL9535_IO15  /* SD_PWEN      */
#define LS_BOARD_XL_RADIO_RST       LS_XL9535_IO16  /* LORA_RSTN    */

/* The ES8311 control interface is on the secondary bus. ls_audio_hw
   drives it directly; the Waveshare BSP must never initialize this codec. */
#define LS_BOARD_CODEC_I2C_BUS   LS_I2C_SECONDARY
#define LS_BOARD_CODEC_I2C_ADDR  0x18

/* PA enable and C6 enable are XL9535 outputs (IO6 and IO14), not
   GPIOs.  The shared ls_board.h defaults are 53 and 54, which on this board
   are exposed 2x8P header pins - and with the keyboard expansion fitted they
   are the nRF24L01+'s CE and CS.  Pinning both to -1 stops the shared
   defaults being inherited and driven; the expander drives the real ones. */
#define LS_BOARD_PA_EN_GPIO      -1
#define LS_BOARD_C6_EN_GPIO      -1

/*/ SD uses IDF slot 0 on 43/44/39-42. The shared default
   pins 18/19/14-17 belong to C6 on slot 1. ls_sdcard mounts the card on
   slot 0 so both buses can run together. */
#define LS_BOARD_SDIO_CLK_GPIO   43
#define LS_BOARD_SDIO_CMD_GPIO   44
#define LS_BOARD_SDIO_D0_GPIO    39
#define LS_BOARD_SDIO_D1_GPIO    40
#define LS_BOARD_SDIO_D2_GPIO    41
#define LS_BOARD_SDIO_D3_GPIO    42

/* Boot button: 35, matching the shared default.  Note the vendor header also
   maps 35 as ip101::kRmiiTxd1 - the two are mutually exclusive, and Ethernet
   is not something LakeShark brings up. */

/* ------------------------------------------------------------- display */

/* Backlight is a direct GPIO on both SKUs.  PT4103 EN, and its internal soft
   start caps PWM at 1 kHz - the vendor says so explicitly, so do not raise it.
     vendor gpio::pt4103::kEn = 51, device::pt4103::kPwmFrequencyHz = 1000 */
#define LS_BOARD_LCD_BL_GPIO     51
#define LS_BOARD_LCD_BL_PWM_MAX_HZ 1000

/* Panel and touch reset/interrupt are XL9535 outputs, not GPIOs:
     kScreenRst = IO2, kTouchRst = IO3, kTouchInt = IO4
   so LS_BOARD_LCD_RST_GPIO / LS_BOARD_TOUCH_*_GPIO cannot be defined here at
   all.  Bringing a panel up on this board needs the expander driver first. */

/* Two SKUs, chosen in menuconfig (LS_TDP4_PANEL). The panel and its touch
   share the 2-lane DSI bus, the XL9535 resets and I2C port 1 either way. */

#if defined(CONFIG_LS_TDP4_PANEL_HI8561)
/* TFT SKU. Vendor lilygo_device_driver t_display_p4/config.h, hi8561:
   540x1168, hsync 28 hbp 26 hfp 20, vsync 2 vbp 22 vfp 200, 2 lanes at
   1000 Mbps, touch (same die) at 0x68 on port 1. The IMU's 0x68 is on the
   secondary bus, so the two do not collide. */
#define LS_BOARD_LCD_H_RES       540
#define LS_BOARD_PANEL_HI8561    1
#define LS_BOARD_LCD_V_RES       1168
/* Unmeasured on glass: a smaller margin than the AMOLED's until someone
   checks how far the bezel covers the corners. */
#define LS_BOARD_LCD_CORNER_R    40
#define LS_BOARD_LCD_DSI_LANES   2
#define LS_BOARD_LCD_DSI_MBPS    1000
/* The vendor runs 60 MHz (~70 Hz). Scanout competes with capture for PSRAM
   the same way it does on the AMOLED, so start from the clocks that proved
   out there: 48 MHz is ~56 Hz here, 40 MHz ~47 Hz. */
#define LS_BOARD_LCD_DPI_CLK_MHZ 48
#define LS_BOARD_LCD_DPI_PERF_CLK_MHZ 40
#define LS_BOARD_LCD_HSYNC       28
#define LS_BOARD_LCD_HBP         26
#define LS_BOARD_LCD_HFP         20
#define LS_BOARD_LCD_VSYNC       2
#define LS_BOARD_LCD_VBP         22
#define LS_BOARD_LCD_VFP         200
#define LS_BOARD_TOUCH_I2C_ADDR  0x68
#else
/* 4.1" AMOLED SKU. */

#define LS_BOARD_LCD_H_RES       568
#define LS_BOARD_PANEL_RM69A10   1
#define LS_BOARD_LCD_V_RES       1232
/* Keep text inside the rounded glass; matches the renderer's panel mask. */
#define LS_BOARD_LCD_CORNER_R    72
#define LS_BOARD_LCD_DSI_LANES   2
#define LS_BOARD_LCD_DSI_MBPS    1000
/* 60 MHz produced whole-screen flashes and 37 ms refresh gaps on the
   20 ms frame schedule under normal load. 48 MHz gives scanout headroom
   while retaining ~40 Hz; 40 MHz stopped flashes but felt slightly slower. */
#define LS_BOARD_LCD_DPI_CLK_MHZ 48
/* High-rate capture + DSP + SD share PSRAM with scanout. The previously
   exercised 40 MHz timing gives ~33 Hz and 17% less scanout demand. */
#define LS_BOARD_LCD_DPI_PERF_CLK_MHZ 40
#define LS_BOARD_LCD_HSYNC       50
#define LS_BOARD_LCD_HBP         150
#define LS_BOARD_LCD_HFP         50
#define LS_BOARD_LCD_VSYNC       40
#define LS_BOARD_LCD_VBP         120
#define LS_BOARD_LCD_VFP         80
#define LS_BOARD_TOUCH_I2C_ADDR  0x5D
#endif

/* Neither SKU is 480x800.  's waterfall ring buffer and the refresh counters are sized off LS_BOARD_LCD_*, and this is a 1232-line
   panel in portrait - both want re-checking before anyone trusts a
   frame-timing number here. */

/* ------------------------------------------------------------- radios */

#define LS_BOARD_SPI_SCLK_GPIO   2
#define LS_BOARD_SPI_MOSI_GPIO   3
#define LS_BOARD_SPI_MISO_GPIO   4

/* SX1262 LoRa on the base board, SPI port 1 (SCLK 2 / MOSI 3 / MISO 4).
   CS and BUSY are direct GPIOs; RST and DIO1 are XL9535 IO16 and IO17, so
     vendor gpio::radio: kCs = 24, kBusy = 6
     vendor gpio::xl9535: kRadioRst = IO16, kRadioDio1 = IO17 */

#define LS_BOARD_LORA_CS_GPIO    24
#define LS_BOARD_LORA_BUSY_GPIO  6

/* SX1262 DIO1 is expander pin IO17. */

#define LS_BOARD_XL_RADIO_DIO1   LS_XL9535_IO17

/* The same socket may carry an LR20xx (LR2021) module. ls_lora_start() probes
   for one first and falls back to the SX1262.

   What LilyGO's own sources confirm for the LR2021 carrier (github.com/
   Xinyuan-LilyGO/T-Display-P4, branch v1-debug-lr2021: the
   radiolib_lr2021_send_receive and sx1262_lora_send_receive examples, and the
   board's pin configuration header in its private_library component):
     - CS 24, BUSY 6, SCLK 2 / MOSI 3 / MISO 4: the example reuses the SX1262
       macros above, so the socket and its reset (XL9535 IO16) are shared.
     - IRQ: Lr2021.irqDioNum = 11, and the example polls XL9535 IO17 for it, so
       expander IO17 is LR2021 DIO11 (on the SX1262 it is DIO1).
     - RF switch: the example drives it from LR2021 DIO6/7/8/10 through
       setRfSwitchTable. STBY is all low; LF receive (1090 MHz ADS-B included)
       needs DIO8 high; HF receive is DIO6 + DIO10; DIO8 is also the LF transmit
       state and DIO7 + DIO10 the HF transmit state. Without DIO8 the front end
       is off.
     - Antenna: SKY13453 VCTL is XL9535 IO1. The SX1262 example sets it HIGH
       under the comment "use the RF1 antenna by default" (lines 63-65 of
       that example's main source); the LR2021 example sets it HIGH too. ls_board_hw.c already parks it HIGH and calls that the
       internal antenna. LilyGO lilygo_device_driver t_display_p4/v1/driver.cpp
       sets IO1 HIGH for the internal antenna and LOW for the external MMCX1.
   LilyGO lilygo_device_driver src/device/t_display_p4/v1/driver.cpp,
   InitLr2021, selects TCXO 3.3 V with 32768 ticks before calibration and
   DCDC. The module therefore supports DCDC with its SIMO inductor fitted.
   LilyGO reports the T-LR2021 v0.3 TCXO supply is always on; v1 has no
   TCXO power pin. This driver defaults to TCXO and LDO for sensitivity;
   both clock and regulator modes are runtime settings. It transmits on the LF
   path only, below 1 GHz: LilyGO's example warns that an HF power above 12
   damages the 2.4 GHz front-end module, and the driver never writes one. */
#define LS_BOARD_LORA_MAY_BE_LR20XX 1

/* LR2021 DIO that carries the interrupt to expander IO17. The driver maps it
   and still polls the IRQ word over SPI; nothing reads IO17 for an LR2021. */
#define LS_BOARD_LR20XX_IRQ_DIO     11

/* RF switch DIOs, { dio, config }. config is the SetDioRfSwitchConfig byte:
   bit 0 standby, 1 Rx LF, 2 Tx LF, 3 Rx HF, 4 Tx HF, set where the DIO is
   high in that mode. DIO8 carries both LF states, as in LilyGO's table. The
   Tx HF bits (DIO7 and DIO10 on this carrier) are left clear on purpose, so
   the HF transmit state routes nothing to the antenna; the driver refuses a
   table that sets one. */
#define LS_BOARD_LR20XX_RFSW_TABLE     { 6, 0x08 },   /* high in Rx HF            */     { 7, 0x00 },   /* high only in Tx HF       */     { 8, 0x06 },   /* high in Rx LF and Tx LF  */     { 10, 0x08 }   /* high in Rx HF            */

/* VCTL level that selects RF1, the antenna the factory firmware defaults to. */
#define LS_BOARD_LR20XX_RF1_VCTL_LEVEL true

#define LS_BOARD_GPS_UART_NUM    2
#define LS_BOARD_GPS_RX_GPIO     22
#define LS_BOARD_GPS_TX_GPIO     23
#define LS_BOARD_GPS_BAUD        115200

/* SKY13453 RF switch - XL9535 IO1 (VCTL).  Same story: not a GPIO. */

/* ------------------------------------------- keyboard expansion (optional) */

/* The keyboard board hangs off the two expansion headers and brings its own
   I2C bus, made from header pins - it is not the shared port 1 or port 2:
     vendor keyboard_expansion::gpio::i2c: kPort3Sda = ext 1x4P2 IO46
                                           kPort3Scl = ext 1x4P2 IO45 */
#define LS_BOARD_KEYBOARD_I2C_SDA_GPIO  46
#define LS_BOARD_KEYBOARD_I2C_SCL_GPIO  45

/* T-MixRF, vendor keyboard expansion configuration. Power is XL9555 IO0. */
#define LS_BOARD_MIX_CC_CS 36
#define LS_BOARD_MIX_CC_GDO0 25
#define LS_BOARD_MIX_NRF_CS 54
#define LS_BOARD_MIX_NRF_CE 53
#define LS_BOARD_MIX_NFC_CS 27

/* TCA8418 keypad matrix, 10 columns x 7 rows.  Address is fixed at 0x34 and
   the interrupt is a direct GPIO, so the keypad is the one part of this
   expansion that works without the expander driver.  Reset is XL9555 IO6,
   which is only needed to recover a wedged controller.
     vendor keyboard_expansion device::tca8418: kI2cAddress = 0x34,
       kKeypadScanWidth = 10, kKeypadScanHeight = 7
     vendor keyboard_expansion gpio::tca8418: kInt = ext 1x4P1 IO48
     vendor keyboard_expansion gpio::xl9555: kTca8418Rst = IO6 */
#define LS_BOARD_KEYBOARD_I2C_ADDR   0x34
#define LS_BOARD_KEYPAD_INT_GPIO     48
#define LS_BOARD_KEYPAD_COLS         10
#define LS_BOARD_KEYPAD_ROWS         7

/* SY7200A keyboard backlight enable, direct GPIO.
     vendor keyboard_expansion gpio::sy7200a::kEn = ext 1x4P1 IO47 */
#define LS_BOARD_KEYPAD_BL_GPIO      47

/* #define LS_BOARD_CC1101_CS_GPIO   36 */
/* #define LS_BOARD_CC1101_GDO0_GPIO 25 */
/* #define LS_BOARD_CC1101_GDO2_GPIO 33 */
/* #define LS_BOARD_NRF24_CS_GPIO    54 */
/* #define LS_BOARD_NRF24_CE_GPIO    53 */
/* #define LS_BOARD_NRF24_IRQ_GPIO   32 */
/* #define LS_BOARD_NFC_CS_GPIO      27 */

/* Note the collision: nRF24 CS is header IO54 and CE is header IO53 - the
   same two pins the shared ls_board.h defaults use for C6 enable and PA
   enable.  With the keyboard fitted, driving those shared defaults does not
   just poke a header, it drives a radio's chip select.  Another reason the
   PA/C6 defaults must not be inherited on this board. */

/**/
/* PCF8563 RTC @ 0x51 on the primary I2C bus, from the vendor header
   (device::pcf8563::kI2cAddress) and confirmed by an `i2c` bus scan on this
   board on 2026-09-09, which answers 20 51 55 5d on SDA7/SCL8.

   This was held commented out on purpose until there was a driver behind it,
   because LS_HAS_RTC gating on an address alone claims a capability that
   does not exist. ls_rtc.c is that driver, so the define is now real. */
#define LS_BOARD_RTC_I2C_ADDR    0x51

/* BQ27220 fuel gauge @ 0x55 on the primary bus, from the vendor
   config (cpp_bus_driver bq27220.h, kDeviceI2cAddressDefault) and confirmed
   by an `i2c` scan on this board on 2026-09-09, which answers 20 51 55 5d.
   Defined here in the same change that teaches ls_gauge.c to read it. */
#define LS_BOARD_GAUGE_I2C_ADDR  0x55

/* ICM20948 nine-axis sensor @ 0x68 on the SECONDARY bus. */

#define LS_BOARD_IMU_I2C_ADDR    0x68

/* The vibration motor's driver, and what it is driving. */

#define LS_BOARD_HAPTIC_I2C_ADDR 0x58
#define LS_BOARD_HAPTIC_F0_HZ    177

/* HOW THE SENSOR IS GLUED DOWN, as ONE ANGLE. */

#define LS_BOARD_IMU_MOUNT_DEG   90

/* Flipper control-head UART - the vendor header has no dedicated pins for
   it, and the free header IO are listed under gpio::ext.  Measure against
   the board before writing a scan list; guessing here costs a wiring
   session, not a compile error. */
/* #define LS_BOARD_LINK_RX_GPIO    ??  TODO measure */
/* #define LS_BOARD_LINK_TX_GPIO    ??  TODO measure */
/* #define LS_BOARD_LINK_SCAN_PINS  ??  TODO measure */

#endif
