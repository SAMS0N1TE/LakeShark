/* Derived board capabilities.  Include ls_board.h, not this file.

   Every LS_HAS_* below is DERIVED from a physical fact a variant header
   declared.  Nothing here is hand-set per board, and no variant may define an
   LS_HAS_* itself - that is the whole point.  A board gains a capability by
   declaring the pin or panel that provides it, and loses it by omitting one.

   Code outside board/ tests LS_HAS_*, never CONFIG_LS_BOARD_*.  That rule is
   what keeps board-specific logic from leaking into the radio and DSP paths,
   and it is what makes board number five cost one header instead of a fork.
   .github/workflows/lint-board-rule.yml enforces it. */
#ifndef LS_CAPS_H
#define LS_CAPS_H

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

#if defined(LS_BOARD_TOUCH_RST_GPIO)
#define LS_HAS_TOUCH         1
#else
#define LS_HAS_TOUCH         0
#endif

/*LS-907  One keyboard capability, from either physical fact.

   LS_HAS_KEYBOARD was derived twice in this file - once from
   LS_BOARD_KEYBOARD_I2C_ADDR and again, sixty lines later, from
   LS_BOARD_KEYPAD_INT_GPIO - and each block defined it unconditionally.
   That was silent only because no variant declared either macro, so both
   arrived at 0 and the redefinition was identical. The first variant to
   declare a keypad would have had the two disagree: 1 from one block, 0
   from the other, and the winner decided by which came last in the file.

   A keyboard controller is one capability. Either fact establishes it: the
   I2C address says the controller is on the bus, the interrupt line says
   its IRQ is wired somewhere the firmware can see. Code that needs the IRQ
   specifically must test LS_BOARD_KEYPAD_INT_GPIO, not this. */
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

/* The ESP32-C6 companion radio, held in reset until enabled. */
#if defined(LS_BOARD_C6_EN_GPIO) && (LS_BOARD_C6_EN_GPIO >= 0)
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

/* I2S codec for audio out. */
#if defined(LS_BOARD_I2S_BCK_GPIO)
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
   legal envelope. */
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
#if defined(LS_BOARD_RF_SW_GPIO) && (LS_BOARD_RF_SW_GPIO >= 0)
#define LS_HAS_RF_SWITCH     1
#else
#define LS_HAS_RF_SWITCH     0
#endif

/* Battery-backed real-time clock. When a variant declares an I2C address
   for one, this flips on and ls_time.c would seed the wall clock from it
   at boot - so a page written before WiFi comes up still gets a real
   stamp. A board carrying a PCF8563 would be the obvious first source;
   its I2C address is 0x51 but no driver has landed here yet. Do not
   write one from inside this task - LS-200 explicitly leaves it to a
   follow-up - just record that this is the eventual source of time
   across a reboot. */
#if defined(LS_BOARD_RTC_I2C_ADDR)
#define LS_HAS_RTC           1
#else
#define LS_HAS_RTC           0
#endif

/* Build-time policy, not a hardware fact: a GUI is drawn only when the board
   has a panel AND this build was not configured headless.  Kept separate
   from LS_HAS_DISPLAY on purpose - see the comment there. */
#if LS_HAS_DISPLAY && !defined(CONFIG_LAKESHARK_HEADLESS)
#define LS_USE_DISPLAY       1
#else
#define LS_USE_DISPLAY       0
#endif

#endif
