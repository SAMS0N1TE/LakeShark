/* Board-private: what ls_lora.c (the dispatcher) shares with its two backends,
   ls_lora_sx126x.c and ls_lora_lr20xx.c. Nothing outside board/ includes this. */

#ifndef LS_LORA_PRIV_H
#define LS_LORA_PRIV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"

#include "ls_lora.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One entry per public ls_lora_* call that depends on the part. A backend that
   does not implement a call still fills the slot, with a function that returns
   ESP_ERR_NOT_SUPPORTED (or the matching neutral value). Never NULL except
   `stop`, `fsk_retune`, `fsk_stream_read`, the modes_* slots and the ook_* slots. A backend must never send another family's opcodes. */
typedef struct {
    ls_lora_chip_t kind;
    const char *name;                       /* default name; see name_fn */
    uint32_t (*caps)(void);
    esp_err_t (*status)(uint8_t *out);
    esp_err_t (*read_reg)(uint16_t addr, uint8_t *buf, size_t len);
    esp_err_t (*configure)(const ls_lora_cfg_t *cfg);
    const ls_lora_cfg_t *(*cfg)(void);
    esp_err_t (*receive)(void);
    bool      (*is_receiving)(void);
    esp_err_t (*send)(const uint8_t *data, size_t len);
    bool      (*send_done)(void);
    int       (*poll)(uint8_t *buf, size_t max, float *rssi_dbm, float *snr_db);
    esp_err_t (*rssi_inst)(float *dbm);
    uint32_t  (*airtime_ms)(int len);
    esp_err_t (*fsk_begin)(const ls_fsk_cfg_t *cfg);
    int       (*fsk_poll)(uint8_t *buf, size_t size, float *rssi_dbm);
    esp_err_t (*fsk_send)(const uint8_t *data, size_t len);
    esp_err_t (*fsk_receive)(void);
    esp_err_t (*fsk_end)(void);
    bool      (*fsk_active)(void);
    /* NULL where the part cannot move a session in place: the dispatcher
       answers ESP_ERR_NOT_SUPPORTED. */
    esp_err_t (*fsk_retune)(uint32_t freq_hz);
    /* NULL where the part has no stream sessions: the dispatcher answers -1. */
    int       (*fsk_stream_read)(uint8_t *buf, size_t size, bool *restarted);
    esp_err_t (*scan_begin)(uint32_t min_hz, uint32_t max_hz);
    int       (*scan_sweep)(float *dbm, int n);
    int       (*scan_pass)(float *dbm, int n, bool *row_done);
    esp_err_t (*scan_end)(void);
    bool      (*scanning)(void);
    void      (*scan_profile)(ls_lora_scan_prof_t *out);
    /* The Mode S frame source. NULL where the part has none: the dispatcher
       answers ESP_ERR_NOT_SUPPORTED. */
    esp_err_t (*modes_begin)(uint32_t freq_hz, int gain_step);
    int       (*modes_poll)(uint8_t *buf, size_t size, float *rssi_dbm);
    esp_err_t (*modes_set_gain)(int gain_step);
    esp_err_t (*modes_end)(void);
    bool      (*modes_active)(void);
    /* Live tuning of the session; NULL where the part has none. */
    esp_err_t (*modes_tuning)(ls_lora_modes_tuning_t *out);
    esp_err_t (*modes_set_boost)(int boost);
    esp_err_t (*modes_set_bw)(uint32_t hz, uint32_t *chosen_hz);
    esp_err_t (*modes_set_thresh)(int level_db, bool automatic);
    esp_err_t (*modes_read_thresh)(int *raw);
    /* The OOK session with the numbers left open; NULL where there is none. */
    esp_err_t (*ook_begin)(const ls_ook_cfg_t *cfg);
    int       (*ook_poll)(uint8_t *buf, size_t size, float *rssi_dbm);
    esp_err_t (*ook_end)(void);
    bool      (*ook_active)(void);
    void      (*diagnostics)(void);         /* after a successful start */
    void      (*stop)(void);                /* drop backend state; may be NULL */
    const char *(*name_fn)(void);           /* optional refinement of name */
} ls_lora_ops_t;

/* The SX126x LoRa bandwidth ladder, shared by the sweep plan (ls_lora.c) and
   the SX126x backend. */
typedef struct { uint32_t hz; uint8_t code; } ls_lora_bw_rung_t;
extern const ls_lora_bw_rung_t ls_lora_sx126x_bw_ladder[];
extern const int ls_lora_sx126x_bw_ladder_n;

/* ---- hardware the dispatcher owns (fitted boards only) ------------------- */

spi_device_handle_t ls_lora_hw_dev(void);

/* The one lock over the SPI device. Every transaction sequence takes it: a
   whole command, including both frames of an LR20xx read and the BUSY waits
   between them, since another task's frame between the halves is read as the
   answer. The dispatcher takes it in each public entry point that reaches the
   chip, and the backends take it again around the commands they issue, so a
   helper reached from anywhere is covered. It is recursive: the same task may
   hold it several times over. Task context only. Keep a hold to one command
   sequence; never across a wait that is not a single command's BUSY wait. */
void ls_lora_hw_lock(void);
void ls_lora_hw_unlock(void);
/* Whether the calling task holds it. */
bool ls_lora_hw_locked_by_me(void);
/* spi_device_transmit on the socket's device, and the only way a backend
   reaches it. Called without the lock held it still serialises that one
   transaction, and counts the call: the host bench asserts the count stays
   zero, because a sequence that reaches here unguarded is one a second task
   can interleave with. */
esp_err_t ls_lora_hw_transmit(spi_transaction_t *t);
/* Command lock must cover fill, transfer and reply copy. */
esp_err_t ls_lora_hw_transfer_bytes(const uint8_t *tx, uint8_t *rx, size_t n);
unsigned  ls_lora_hw_unguarded(void);
/* One word-aligned internal DMA buffer, reserved before probing and retained
   across stop/start. Holds 259 wire bytes plus alignment padding. NULL until
   allocation succeeds; access is protected by the command lock. */
uint8_t *ls_lora_hw_pkt(void);
/* BUSY low within `timeout_ms`, spinning briefly and then yielding. */
bool ls_lora_hw_wait_not_busy(int timeout_ms);
int  ls_lora_hw_busy_level(void);
/* A pulse on the part's NRESET and the wait for it to come back up, for a
   backend whose part has stopped taking commands. The part then holds none of
   what it was told; the backend reprograms it. Takes the lock itself. */
esp_err_t ls_lora_hw_reset(void);

/* ---- SX126x backend ------------------------------------------------------ */

extern const ls_lora_ops_t ls_lora_sx126x_ops;
/* SetStandby then GetStatus. The status byte is not judged here. */
esp_err_t ls_lora_sx126x_probe(uint8_t *status);

/* ---- LR20xx backend ------------------------------------------------------ */

extern const ls_lora_ops_t ls_lora_lr20xx_ops;

#ifdef __cplusplus
}
#endif

#endif /* LS_LORA_PRIV_H */
