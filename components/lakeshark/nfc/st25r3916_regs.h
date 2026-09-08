/* ST25R3916 register map, from the ST25R3916 data sheet DS12484 rev 4.

   Only the registers this driver actually touches are named.  Written
   from the public data sheet and NFC Forum specifications - no RFAL
   source was consulted for this map. */

#ifndef LS_ST25R3916_REGS_H
#define LS_ST25R3916_REGS_H

/* SPI command bytes (data sheet §1.2.4 "SPI operating modes").

   The register space is split in two pages (A + B).  Space B is
   addressed by the SPACE_B command; every register in the private
   set below is in space A. */
#define ST25R3916_CMD_MODE_REG_READ     0x40   /* | 6-bit addr */
#define ST25R3916_CMD_MODE_REG_WRITE    0x00   /* | 6-bit addr */
#define ST25R3916_CMD_MODE_FIFO_LOAD    0x80
#define ST25R3916_CMD_MODE_FIFO_READ    0x9F
#define ST25R3916_CMD_MODE_DIRECT_CMD   0xC0   /* | 6-bit command */
#define ST25R3916_CMD_MODE_SPACE_B      0xFB

/* --- Space A registers (subset).  Data sheet §3, table 40. */
#define ST25R3916_REG_IO_CONF1          0x00
#define ST25R3916_REG_IO_CONF2          0x01
#define ST25R3916_REG_OP_CONTROL        0x02
#define ST25R3916_REG_MODE              0x03
#define ST25R3916_REG_BIT_RATE          0x04
#define ST25R3916_REG_ISO14443A_NFC     0x05
#define ST25R3916_REG_TX_DRIVER         0x0A
#define ST25R3916_REG_MASK_RX_TIMER     0x0C
#define ST25R3916_REG_NO_RESPONSE_TIMER 0x0E   /* +0x0F */
#define ST25R3916_REG_MAIN_IRQ_MASK     0x14
#define ST25R3916_REG_MAIN_IRQ          0x1A
#define ST25R3916_REG_FIFO_STATUS1      0x1E
#define ST25R3916_REG_FIFO_STATUS2      0x1F
#define ST25R3916_REG_NUM_TX_BYTES1     0x21
#define ST25R3916_REG_NUM_TX_BYTES2     0x22

/* --- Direct commands. */
#define ST25R3916_CMD_SET_DEFAULT       0xC1
#define ST25R3916_CMD_CLEAR_FIFO        0xC2
#define ST25R3916_CMD_TRANSMIT_WITH_CRC 0xC4
#define ST25R3916_CMD_TRANSMIT_NO_CRC   0xC5
#define ST25R3916_CMD_MASK_RX_ENABLE    0xD1
#define ST25R3916_CMD_MASK_RX_DISABLE   0xD0

/* --- Main IRQ bits (main IRQ, register 0x1A). */
#define ST25R3916_IRQ_TXE               0x08   /* end of transmit */
#define ST25R3916_IRQ_RXS               0x04   /* start of receive */
#define ST25R3916_IRQ_RXE               0x02   /* end of receive */

#endif
