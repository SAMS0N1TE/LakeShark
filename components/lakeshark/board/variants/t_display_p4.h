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

/* SKU settled: the unit in hand is the 4.1" AMOLED. */

#define LS_BOARD_LCD_H_RES       568
#define LS_BOARD_PANEL_RM69A10   1
#define LS_BOARD_LCD_V_RES       1232
/* The corner radius of the glass, in pixels. A physical fact, so it
   lives here; the TUI needs it to know how far its corner cell has to stand
   off, and NOT how far its edges do - that distinction was worth six columns
   and four rows. 40 is what the LVGL chrome assumed and what the TUI
   inherited; 'tui corner <px>' sweeps it against the actual panel. */
#define LS_BOARD_LCD_CORNER_R    40
#define LS_BOARD_LCD_DSI_LANES   2
#define LS_BOARD_LCD_DSI_MBPS    1000
#define LS_BOARD_LCD_DPI_CLK_MHZ 60
#define LS_BOARD_LCD_HSYNC       50
#define LS_BOARD_LCD_HBP         150
#define LS_BOARD_LCD_HFP         50
#define LS_BOARD_LCD_VSYNC       40
#define LS_BOARD_LCD_VBP         120
#define LS_BOARD_LCD_VFP         80
#define LS_BOARD_TOUCH_I2C_ADDR  0x5D

/* Neither SKU is 480x800.  's waterfall ring buffer and the refresh counters are sized off LS_BOARD_LCD_*, and this is a 1232-line
   panel in portrait - both want re-checking before anyone trusts a
   frame-timing number here. */

/* ------------------------------------------------------------- radios */

#define LS_BOARD_SPI_SCLK_GPIO   2
#define LS_BOARD_SPI_MOSI_GPIO   3
#define LS_BOARD_SPI_MISO_GPIO   4

/* SX1262 LoRa on the base board, SPI port 1 (SCLK 2 / MOSI 3 / MISO 4).
   CS and BUSY are direct GPIOs; RST and DIO1 are XL9535 IO16 and IO17, so
   the part is NOT usable from a pin map alone and LS_HAS_LORA stays off
   until the expander driver lands.  Recorded so the next person does not
   have to re-derive it.
     vendor gpio::radio: kCs = 24, kBusy = 6
     vendor gpio::xl9535: kRadioRst = IO16, kRadioDio1 = IO17 */
/* Enabled. */

#define LS_BOARD_LORA_CS_GPIO    24
#define LS_BOARD_LORA_BUSY_GPIO  6

/* SX1262 DIO1 is expander pin IO17. */

#define LS_BOARD_XL_RADIO_DIO1   LS_XL9535_IO17

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
