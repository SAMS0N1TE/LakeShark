/* Derived board capabilities. */

#ifndef LS_CAPS_H
#define LS_CAPS_H

#if defined(LS_BOARD_BATTERY_ADC_CHANNEL) && defined(LS_BOARD_BATTERY_DIVIDER)
#define LS_HAS_BATTERY_ADC 1
#else
#define LS_HAS_BATTERY_ADC 0
#endif

#if defined(LS_BOARD_PANEL_RM69A10)
#define LS_HAS_COMPACT_UI 1
#else
#define LS_HAS_COMPACT_UI 0
#endif

#ifndef LS_BOARD_H
#error "include ls_board.h, not ls_caps.h directly"
#endif

/* A software-controlled VBUS switch.  This is what lets a wedged RTL dongle
   be power-cycled and re-enumerated without touching the hardware. */
#if defined(LS_BOARD_VBUS_EN_GPIO) && (LS_BOARD_VBUS_EN_GPIO >= 0)
#define LS_HAS_VBUS_CTRL     1
#else
#define LS_HAS_VBUS_CTRL     0
#endif

/* A panel is physically present.  This is a HARDWARE fact and is NOT the
   same question as "does this build draw a GUI" - a headless build on a
   board that has a panel is a legitimate configuration.  Use LS_USE_DISPLAY
   for the build question. */
#if defined(LS_BOARD_LCD_H_RES)
#define LS_HAS_DISPLAY       1
#else
#define LS_HAS_DISPLAY       0
#endif

/* Either kind of pin counts, the same way fixed the RF switch. */

#if defined(LS_BOARD_TOUCH_RST_GPIO) || defined(LS_BOARD_XL_TOUCH_RST)
#define LS_HAS_TOUCH         1
#else
#define LS_HAS_TOUCH         0
#endif

/* One keyboard capability, from either physical fact. */

#if defined(LS_BOARD_KEYBOARD_I2C_ADDR) ||     (defined(LS_BOARD_KEYPAD_INT_GPIO) && (LS_BOARD_KEYPAD_INT_GPIO >= 0))
#define LS_HAS_KEYBOARD      1
#else
#define LS_HAS_KEYBOARD      0
#endif

#if defined(LS_BOARD_HAS_PWR_BTN) && LS_BOARD_HAS_PWR_BTN
#define LS_HAS_POWER_BTN     1
#elif defined(LS_BOARD_PWR_BTN_GPIO) && (LS_BOARD_PWR_BTN_GPIO >= 0)
#define LS_HAS_POWER_BTN     1
#else
#define LS_HAS_POWER_BTN     0
#endif

/* The ESP32-C6 companion radio, held in reset until enabled.

   Same fault as LS_HAS_TOUCH above and the same fix. This board's C6 enable
   is expander IO14, LS_BOARD_C6_EN_GPIO is explicitly -1, and ls_board_hw.c
   releases the part through the expander at every boot - so the flag said
   there was no co-processor on a board that powers one up before the console
   appears. */
#if (defined(LS_BOARD_C6_EN_GPIO) && (LS_BOARD_C6_EN_GPIO >= 0)) || \
    defined(LS_BOARD_XL_C6_EN)
#define LS_HAS_C6            1
#else
#define LS_HAS_C6            0
#endif

/* SDIO bus to the C6 / SD card. */
#if defined(LS_BOARD_SDIO_CLK_GPIO)
#define LS_HAS_SDIO          1
#else
#define LS_HAS_SDIO          0
#endif

/* I2C GPIO expander holding rails and resets.

   A board declares one by giving both an address and a bus.  On the
   T-Display-P4 this is not an optional peripheral: the 3V3 rail, both
   resets, audio power, USB PHY power and the C6 enable are all behind it,
   so a build that cannot reach the expander reaches almost nothing. */
#if defined(LS_BOARD_IO_EXPANDER_ADDR) && defined(LS_BOARD_IO_EXPANDER_BUS)
#define LS_HAS_IO_EXPANDER   1
#else
#define LS_HAS_IO_EXPANDER   0
#endif

/* Second I2C bus.  The shared LS_BOARD_I2C_* pins are one bus; a
   board with a codec or a sensor on a separate bus declares this too. */
#if defined(LS_BOARD_I2C2_SDA_GPIO) && defined(LS_BOARD_I2C2_SCL_GPIO)
#define LS_HAS_I2C2          1
#else
#define LS_HAS_I2C2          0
#endif

/* I2S codec for audio out. */

#if defined(LS_BOARD_NO_AUDIO) && LS_BOARD_NO_AUDIO
#define LS_HAS_AUDIO         0
#elif defined(LS_BOARD_I2S_BCK_GPIO)
#define LS_HAS_AUDIO         1
#else
#define LS_HAS_AUDIO         0
#endif

/* UART to a Flipper control head. */
#if defined(LS_BOARD_LINK_RX_GPIO) && defined(LS_BOARD_LINK_TX_GPIO)
#define LS_HAS_LINK_UART     1
#else
#define LS_HAS_LINK_UART     0
#endif

/* LoRa transceiver (Semtech SX1262 / LR2021 class).  Long-range narrowband,
   its own ISM regime, TX-capable, on a bus and RF path distinct from any
   sub-GHz FSK radio the board might also carry.  Kept separate from
   LS_HAS_SUBGHZ_TX on purpose - different silicon, different bus, different
   legal envelope - see in variants/t_display_p4.h. */
#if defined(LS_BOARD_LORA_CS_GPIO) && (LS_BOARD_LORA_CS_GPIO >= 0)
#define LS_HAS_LORA          1
#else
#define LS_HAS_LORA          0
#endif

/* Sub-GHz TX-capable ISM transceiver (TI CC1101 class).  This is the
   capability the REC app cares about: on every board so far REC has only
   ever been able to receive and replay through a separate HackRF, so the
   presence of an on-board TX-capable ISM radio changes what REC can do. */
#if defined(LS_BOARD_CC1101_CS_GPIO) && (LS_BOARD_CC1101_CS_GPIO >= 0)
#define LS_HAS_SUBGHZ_TX     1
#else
#define LS_HAS_SUBGHZ_TX     0
#endif

/* NFC reader/writer (ST ST25R3916 class). */
#if defined(LS_BOARD_NFC_CS_GPIO) && (LS_BOARD_NFC_CS_GPIO >= 0)
#define LS_HAS_NFC           1
#else
#define LS_HAS_NFC           0
#endif

/* RF antenna switch (SKY13453 class).  When present, code must select the
   antenna path before transmitting - LakeShark otherwise transmits into
   whichever path the switch defaulted to at boot. */
/* Either kind of pin counts. */

#if (defined(LS_BOARD_RF_SW_GPIO) && (LS_BOARD_RF_SW_GPIO >= 0)) ||     defined(LS_BOARD_XL_RF_SW_VCTL)
#define LS_HAS_RF_SWITCH     1
#else
#define LS_HAS_RF_SWITCH     0
#endif

/* Battery-backed real-time clock. */

#if defined(LS_BOARD_RTC_I2C_ADDR)
#define LS_HAS_RTC           1
#else
#define LS_HAS_RTC           0
#endif

#if defined(LS_BOARD_GAUGE_I2C_ADDR)
#define LS_HAS_GAUGE         1
#else
#define LS_HAS_GAUGE         0
#endif

/* A nine-axis inertial sensor on I2C: accelerometer, gyroscope and
   magnetometer. Derived from the address alone, which is the whole of what
   distinguishes a board that has one - unlike LS_HAS_TOUCH, which derives
   from a reset GPIO this board cannot define because the reset is an
   expander pin, and therefore reads 0 on a board whose touch works. */
#if defined(LS_BOARD_IMU_I2C_ADDR)
#define LS_HAS_IMU           1
#else
#define LS_HAS_IMU           0
#endif

/* A haptic driver on I2C, and a motor on the other side of it.

   Derived from the address for the same reason LS_HAS_IMU is: the address is
   the whole of what distinguishes a board that has one. The resonant
   frequency is a separate board fact and has no default - a board that
   declares the part declares what it is driving. */
#if defined(LS_BOARD_HAPTIC_I2C_ADDR)
#define LS_HAS_HAPTIC        1
#else
#define LS_HAS_HAPTIC        0
#endif

/* A microphone into the codec's ADC. Derived from the data-in pin,
   because that is the whole of what a board needs to have one: the codec is
   already there for output and capture is the same part in a different
   mode. */
#if defined(LS_BOARD_I2S_DIN_GPIO)
#define LS_HAS_MIC           1
#else
#define LS_HAS_MIC           0
#endif

/* The Waveshare GUI uses main.cpp. The compact panel adapter uses the radio
   entry point with the same shell, so it also draws a GUI. */
#if LS_HAS_DISPLAY && (!defined(CONFIG_LAKESHARK_HEADLESS) || LS_HAS_COMPACT_UI)
#define LS_USE_DISPLAY       1
#else
#define LS_USE_DISPLAY       0
#endif

#endif
