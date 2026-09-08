/* Copy this to variants/<your_board>.h to add a board, then:
     1. add a CONFIG_LS_BOARD_<YOUR_BOARD> choice entry to main/Kconfig.projbuild
     2. add the #elif arm to ls_board.h that includes your file
     3. add boards/<your_board>.defaults
     4. add the board to the CI matrix in .github/workflows/build-matrix.yml
   Nothing else in the tree should need to change.  If you find yourself
   wanting to test CONFIG_LS_BOARD_<YOUR_BOARD> outside this directory, add a
   capability to ls_caps.h and test that instead - see docs/PORTING.md.

   Declare PHYSICAL FACTS ONLY here: pins, panel size, flash size.  Never
   declare an LS_HAS_* flag; ls_caps.h derives those from what you write.
   Omit anything the board does not have - absence is how a capability is
   switched off.  Never guess a pin number: a wrong GPIO is worse than an
   absent feature, and can damage hardware.  Leave it out and say so. */
#ifndef LS_VARIANT_TEMPLATE_H
#define LS_VARIANT_TEMPLATE_H

#define LS_BOARD_NAME            "MY-BOARD"      /* shown at every boot */
#define LS_BOARD_FLASH_MB        16              /* picks the partition table */

/* USB VBUS switch.  A real GPIO here buys power-cycle recovery of a wedged
   dongle - LakeShark's one advantage over library-style USB stacks.  Use
   (-1) when the port is hard-powered; that derives LS_HAS_VBUS_CTRL = 0. */
#define LS_BOARD_VBUS_EN_GPIO    (-1)

/* Display.  Omit this whole block on a headless board: defining
   LS_BOARD_LCD_H_RES is what derives LS_HAS_DISPLAY. */
/* #define LS_BOARD_LCD_H_RES       480 */
/* #define LS_BOARD_LCD_V_RES       800 */
/* #define LS_BOARD_LCD_BL_GPIO     26  */
/* #define LS_BOARD_LCD_RST_GPIO    27  */

/* Touch.  Omit unless the panel has a controller; defining
   LS_BOARD_TOUCH_RST_GPIO is what derives LS_HAS_TOUCH. */
/* #define LS_BOARD_TOUCH_RST_GPIO  23  */
/* #define LS_BOARD_TOUCH_INT_GPIO  (-1) */

/* Keyboard matrix controller.  Its I2C address derives LS_HAS_KEYBOARD. */
/* #define LS_BOARD_KEYBOARD_I2C_ADDR  0x34 */

/* Control-head UART to the Flipper.  LS_BOARD_LINK_SCAN_PINS is the ordered
   list the link probes when autodetecting which pins are actually wired. */
#define LS_BOARD_LINK_RX_GPIO    33
#define LS_BOARD_LINK_TX_GPIO    32
#define LS_BOARD_LINK_SCAN_PINS \
    { 33, 32 }

/* Any of the shared ESP32-P4 pins in ls_board.h (I2C, I2S, SDIO, C6 enable,
   BOOT button) may be overridden by defining them here - ls_board.h fills in
   only what a variant leaves undefined. */

#endif
