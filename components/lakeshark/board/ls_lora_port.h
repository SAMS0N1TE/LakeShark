/* Where the LoRa radio is wired: the board's own socket, or a module on the
   feature (expansion) header.

   A header module shares the radio SPI bus that already reaches the header
   (the keyboard's MixRF radios use it too) and brings its own chip select,
   BUSY and reset lines, and for an LR20xx its own RF switch wiring, interrupt
   DIO and TCXO voltage. The choice is saved and taken at the next boot; until
   anything is saved the board's own values are used, so nothing changes for
   a board left alone.

   Header pins that the keyboard expansion also uses turn the keyboard and the
   MixRF radios off while header mode is in force: one connector cannot carry
   both. */

#ifndef LS_LORA_PORT_H
#define LS_LORA_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_LORA_PORT_RFSW_MAX 7     /* LR20xx DIO5..DIO11 */
#define LS_LORA_PORT_RST_BOARD (-2) /* the board's own (expander) reset line */
#define LS_LORA_PORT_NONE (-1)

typedef struct {
    bool header;            /* false: the board's socket                       */
    int8_t cs, busy;        /* GPIOs                                           */
    int8_t rst;             /* GPIO, or LS_LORA_PORT_RST_BOARD                 */
    int8_t irq_gpio;        /* GPIO wired to the interrupt DIO, or NONE (the
                               driver polls the IRQ word over SPI either way)  */
    int8_t irq_dio;         /* LR20xx DIO that carries the interrupt, or NONE  */
    uint8_t tcxo_code;      /* SetTcxoMode supply code, 0 1.6 V .. 7 3.3 V     */
    uint8_t rfsw_n;
    struct { uint8_t dio, config; } rfsw[LS_LORA_PORT_RFSW_MAX];
} ls_lora_port_t;

/* The board's own socket, as its variant header describes it. */
void ls_lora_port_board(ls_lora_port_t *out);

/* What this boot uses: the saved choice when one was saved and is valid,
   the board's socket otherwise. Read once and kept. */
const ls_lora_port_t *ls_lora_port(void);
bool ls_lora_port_is_header(void);

/* The saved choice and saving one. A saved onboard choice keeps the header
   pins last given (header false) so switching back needs no retyping; it
   uses the board's own lines. False, with the board's port, when nothing
   valid is saved. */
bool ls_lora_port_saved(ls_lora_port_t *out);
/* Flip the saved choice between onboard and header, keeping the pins. With
   no header pins of its own saved, header means the board's CON1 module
   where the board has one; elsewhere it refuses. */
bool ls_lora_port_toggle(char *why, size_t n);
/* True when `p` has the board's CON1 module lines ('lora port header con1'). */
bool ls_lora_port_is_con1(const ls_lora_port_t *p);
/* ... and the older T-LR2021 V0.2 switch table ('lora port header v02'). */
bool ls_lora_port_is_v02(const ls_lora_port_t *p);
/* ... and the V0.3 / V1.0 table ('lora port header con1'). CON1 pins with
   neither table is a custom table. */
bool ls_lora_port_is_v03(const ls_lora_port_t *p);
/* True when both would drive the radio the same way at a boot: the same
   choice and, for a header port, the same pins, interrupt, TCXO and table. */
bool ls_lora_port_same(const ls_lora_port_t *a, const ls_lora_port_t *b);
/* Swap the saved CON1 module's switch table between V0.3/V1.0 and V0.2.
   Refuses, and changes nothing, when the saved pins are custom. */
bool ls_lora_port_next_module(char *why, size_t n);
/* True when `p` selects the board's own socket (its chip select), header
   flag or not: header mode on those lines is still the onboard radio. */
bool ls_lora_port_on_socket(const ls_lora_port_t *p);
bool ls_lora_port_save(const ls_lora_port_t *p, char *why, size_t n);

/* GPIOs of `p` that the keyboard expansion also uses, as a mask; 0 when the
   keyboard can stay. Only a header port counts. */
uint64_t ls_lora_port_keyboard_pins(const ls_lora_port_t *p);
/* True when this boot's port takes pins the keyboard needs; the keypad and
   MixRF starts refuse and say why. */
bool ls_lora_port_blocks_keyboard(void);

/* Pins exist, differ from each other and from what the board itself uses,
   and the RF switch table never routes high-band transmit. */
bool ls_lora_port_check(const ls_lora_port_t *p, char *why, size_t n);

/* "header cs=33 busy=32 rst=26 irq=31 dio=9 tcxo=1.8 rfsw=5:02,6:08" (any
   order, after a base) or "onboard". Words start from `base`. */
bool ls_lora_port_parse(int argc, char **argv, const ls_lora_port_t *base,
                        ls_lora_port_t *out, char *why, size_t n);

/* One line for the console and the settings page. */
void ls_lora_port_describe(const ls_lora_port_t *p, char *out, size_t n);

/* Two words for NVS, and back. Unpack refuses a version it does not know. */
void ls_lora_port_pack(const ls_lora_port_t *p, uint64_t *a, uint64_t *b);
bool ls_lora_port_unpack(uint64_t a, uint64_t b, ls_lora_port_t *out);

/* TCXO supply in tenths of a volt for a SetTcxoMode code, and back (-1). */
int ls_lora_port_tcxo_dv(uint8_t code);
int ls_lora_port_tcxo_code(int dv);

/* `lora port ...` */
int ls_lora_port_command(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* LS_LORA_PORT_H */
