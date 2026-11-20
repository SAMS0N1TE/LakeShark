/* Wiring that can be changed on the board: which GPIO each add-on part
   (GPS, a LoRa / LR2021 module, a nine-axis IMU, the Flipper head) is on.

   A board opts in with LS_BOARD_PINS_RUNTIME in its variant. Its compile
   time pins are then only the defaults; the saved table in NVS wins, and a
   change takes effect at the next boot. On every other board ls_pin()
   answers the compile time pin and ls_pin_set() refuses. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_PIN_GPS_RX,      /* module TX -> this pin */
    LS_PIN_GPS_TX,      /* module RX <- this pin */
    LS_PIN_SPI_SCLK,
    LS_PIN_SPI_MOSI,
    LS_PIN_SPI_MISO,
    LS_PIN_LORA_CS,
    LS_PIN_LORA_BUSY,
    LS_PIN_LORA_RST,
    LS_PIN_LORA_IRQ,    /* SX1262 DIO1 / LR2021 IRQ DIO line */
    LS_PIN_IMU_SDA,     /* same as the board bus = share that bus */
    LS_PIN_IMU_SCL,
    LS_PIN_LINK_RX,     /* Flipper TX -> this pin */
    LS_PIN_LINK_TX,
    LS_PIN_COUNT
} ls_pin_id_t;

#define LS_PIN_NONE (-1)

typedef struct {
    const char *key;    /* console and NVS name, e.g. "gps.rx" */
    const char *group;  /* GPS, LORA, IMU, LINK */
    const char *label;  /* short, for the screen */
    bool optional;      /* may be NONE */
} ls_pin_info_t;

/* True when this board's pins can be changed at all. */
bool ls_pins_runtime(void);

/* The pin this boot uses, LS_PIN_NONE when the part is not wired. */
int  ls_pin(ls_pin_id_t id);
/* The pin the next boot will use (what is saved), and the board default. */
int  ls_pin_next(ls_pin_id_t id);
int  ls_pin_default(ls_pin_id_t id);
const ls_pin_info_t *ls_pin_info(ls_pin_id_t id);
int  ls_pin_find(const char *key);          /* -1 when unknown */

/* Save one pin for the next boot. False with a reason when the GPIO is out
   of range, belongs to the board itself or is taken by another part. */
bool ls_pin_set(ls_pin_id_t id, int gpio, char *why, size_t n);
/* Checks a pin without saving it. */
bool ls_pin_check(ls_pin_id_t id, int gpio, char *why, size_t n);

/* IMU I2C address (7-bit). */
uint8_t ls_pins_imu_addr(void);
uint8_t ls_pins_imu_addr_next(void);
bool    ls_pins_set_imu_addr(uint8_t addr);
/* Saves a whole table (LS_PIN_COUNT entries) and the IMU address at once,
   so parts can swap pins. False with a reason, nothing saved, on a clash. */
bool    ls_pins_apply(const int *table, uint8_t imu_addr, char *why, size_t n);
/* Checks one entry of an unsaved table (the clash check reads that table). */
bool    ls_pins_check_in(const int *table, ls_pin_id_t id, char *why, size_t n);
/* The board drives this GPIO itself (audio, SD, C5 link, board I2C...). */
bool    ls_pin_board_own(int gpio);

/* Forget every saved pin: the board defaults come back at the next boot. */
bool ls_pins_reset(void);
/* True when anything saved differs from what this boot runs on. */
bool ls_pins_pending(void);

/* Reads the table. Called once early in boot; any ls_pin() before it
   answers the default. Safe from any task. */
void ls_pins_load(void);
/* True once the saved table has been read (NVS was up). */
bool ls_pins_ready(void);
/* Why the saved pins were not used this boot (a conflict, or a failed start before), or NULL. */
const char *ls_pins_rejected(void);

/* `pin` console command: pin | pin set <key> <gpio|none> | pin imu <addr>
   | pin reset. */
int  ls_pins_cmd(int argc, char **argv);

#ifdef __cplusplus
}
#endif
