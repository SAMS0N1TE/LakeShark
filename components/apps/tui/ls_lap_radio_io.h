/* Firmware-owned radio-I/O worker; never invokes app code. */
#ifndef LS_LAP_RADIO_IO_H
#define LS_LAP_RADIO_IO_H
#include "ls_lap_radio.h"
typedef struct {
    int (*modes)(int radio, lap_radio_modes_t *out);
    int (*open)(int radio, const lap_packet_config_t *cfg, bool edges);
    int (*read)(void *data, int cap, lap_packet_meta_t *meta);
    int (*rssi)(int16_t *dbm);
    void (*close)(void);
    int (*tx)(int radio, const lap_packet_config_t *cfg, int format, const void *data, size_t bytes,
              uint32_t rate, unsigned vga, bool amp);
    int (*tx_poll)(void); /* 0 pending, 1 completed, negative failure */
    void (*tx_cancel)(void);
    uint32_t (*nfc_caps)(void);
    int (*nfc_open)(void);
    int (*nfc_read)(lap_nfc_event_t *out, void *data, size_t cap);
    int (*nfc_write)(const void *data, size_t len);
    void (*nfc_close)(void);
    int (*channels)(const char *bank, uint32_t first, lap_channel_t *rows, int count, bool write);
    /* True while the app's session must stay off the radio: its service runs
       with the screen closed and another owner wants the LoRa chip, or the
       screen opened or closed under a held lease. The worker closes an open
       session and waits, queued, without counting tries. NULL: never. */
    bool (*step_aside)(int radio);
} ls_lap_io_backend_t;
const ls_lap_io_backend_t *ls_lap_io_backend(void);
void ls_lap_io_begin(const lap_app_t *app);
void ls_lap_io_end(void);
bool ls_lap_io_radio_busy(void);
bool ls_lap_rx_physical_busy(void);
void ls_lap_io_file_fault(int error);
void ls_lap_io_tx_interrupt(void);
bool ls_lap_io_service(void);
uint32_t ls_lap_io_caps(void);
/* Bus-free list snapshot, safe on worker or callback. */
int ls_lap_radio_snapshot(int index, lap_radio_info_t *out);
/* Capture contexts: nonblocking, copies bounded chunks; overflow is counted. */
void ls_lap_io_tap(int format, const void *data, size_t bytes, uint64_t hz, uint32_t rate,
                   uint32_t flags);
/* Simulator only; same words used by LAPSIM_SCRIPT without frame prefix. */
bool ls_lap_io_sim_command(const char *command);
#endif
