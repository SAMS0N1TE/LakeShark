#ifndef FLIPPER_LINK_H
#define FLIPPER_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FLIPPER_LINK_PROTO_VERSION 3

typedef struct {
    int      uart_num;
    int      rx_gpio;
    int      tx_gpio;
    uint32_t baud;
    int      telemetry_hz;
} flipper_link_cfg_t;

#define FLIPPER_LINK_CFG_DEFAULT() (flipper_link_cfg_t){ \
    .uart_num     = 2,                                   \
    .rx_gpio      = 33,                                  \
    .tx_gpio      = 32,                                  \
    .baud         = 115200,                              \
    .telemetry_hz = 5,                                   \
}

typedef struct {
    void        (*select_mode_by_name)(const char *name);
    const char *(*current_mode_name)(void);

    void        (*play_test_sound)(void);

    void        (*reboot)(void);
    void        (*c6_reset)(void);
    int         (*c6_up)(void);
    void        (*sdr_reset)(void);

    void        (*sdr_recover)(void);

    /* Returns whether the cycle actually STARTED.

       It was void, so the protocol answered "+OK sdr power cycling" and then
       the board logged that it cannot power cycle the dongle in software -
       two contradictory statements about the same press, one of them to the
       head that acts on it. This board has no VBUS switch on the USB host
       port, so on this board the answer is always no. */
    bool        (*sdr_power_cycle)(void);
    void        (*ble_enable)(bool on);

    void        (*sys_info)(char *out, size_t len);

    void        (*heap_stats)(uint32_t *internal, uint32_t *dma, uint32_t *psram);
    uint32_t    (*uptime_s)(void);

    bool        (*set_log_level)(const char *tag, const char *level);
    void        (*show_fm_mode)(int mode);
} flipper_link_host_t;

void flipper_link_set_host(const flipper_link_host_t *host);

esp_err_t flipper_link_start(const flipper_link_cfg_t *cfg,
                             const flipper_link_host_t *host);

void flipper_link_stop(void);
bool flipper_link_running(void);

esp_err_t flipper_link_reconfigure(const flipper_link_cfg_t *cfg);

void flipper_link_get_cfg(flipper_link_cfg_t *out);

void flipper_link_inject(const char *line, char *reply, size_t reply_len);

int flipper_link_snapshot(char *buf, size_t len);

int flipper_link_eq_snapshot(char *buf, size_t len);

int flipper_link_scan_rx(int dwell_ms);

int flipper_link_probe_rx(void);

void flipper_link_set_verbose(bool en);
bool flipper_link_verbose(void);

void flipper_link_stats(uint32_t *rx_lines, uint32_t *tx_lines, uint32_t *bad_lines);

int flipper_link_sdr_stall_s(void);

#ifdef __cplusplus
}
#endif

#endif
