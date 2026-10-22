/* Receive-only link evidence and strength history. No payloads or bearings. */
#ifndef LS_DRONE_WATCH_H
#define LS_DRONE_WATCH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define LS_DW_ENTRIES 32
#define LS_DW_HISTORY 32
typedef enum { LS_DW_ELRS900, LS_DW_ELRS24, LS_DW_HOPPER900,
    LS_DW_WIDE24, LS_DW_ANALOG58, LS_DW_RID, LS_DW_SOURCES } ls_dw_source_t;
typedef enum { LS_DW_POSSIBLE, LS_DW_LIKELY, LS_DW_CONFIRMED } ls_dw_confidence_t;
typedef enum { LS_DW_STEADY, LS_DW_APPROACHING, LS_DW_RECEDING } ls_dw_trend_t;
typedef struct { int64_t us; float dbm; } ls_dw_rssi_t;
typedef struct {
    uint64_t id, centre_hz;
    ls_dw_source_t source;
    unsigned band_mhz;
    char channel[16], identity[41]; /* RID aircraft Basic ID only; no operator fields */
    float rssi_dbm, peak_dbm, db_per_10s, hits_per_second;
    bool calibrated; /* false: SDR relative dB, never pass off dBFS as dBm */
    ls_dw_trend_t trend;
    ls_dw_confidence_t confidence;
    int64_t first_us, last_us;
    unsigned history_count;
    ls_dw_rssi_t history[LS_DW_HISTORY]; /* oldest first */
} ls_drone_watch_entry_t;
typedef struct {
    bool enabled, alerts;
    uint32_t sources, cycle_ms, dropped;
    bool running[LS_DW_SOURCES];
    char reason[LS_DW_SOURCES][96], config[96];
} ls_drone_watch_status_t;
/* Starts a static PSRAM-stack RF worker on core 0. No screen registration. */
bool ls_drone_watch_start(void);
/* Nonblocking snapshots: SIZE_MAX / false means busy; preserve previous view. */
size_t ls_drone_watch_snapshot(ls_drone_watch_entry_t *out, size_t cap);
bool ls_drone_watch_status(ls_drone_watch_status_t *out);
bool ls_drone_watch_read(uint64_t id, ls_drone_watch_entry_t *out);
const char *ls_drone_watch_source(ls_dw_source_t source);
const char *ls_drone_watch_label(ls_dw_source_t source);
bool ls_drone_watch_enable(bool on);
bool ls_drone_watch_source_enable(ls_dw_source_t source, bool on);
bool ls_drone_watch_alerts(bool on);
/* Where each source last actually measured: lo..hi MHz at `us` (0 = never).
 * For drawing scan progress from real measurements; lock free. */
typedef struct { uint32_t lo_mhz, hi_mhz; int64_t us; } ls_dw_cursor_t;
void ls_drone_watch_cursor(ls_dw_cursor_t out[LS_DW_SOURCES]);
/* Live entries, cheap enough for a home tile every frame. */
unsigned ls_drone_watch_count(void);
/* DRONES is on screen: the watch may pause MeshCore to use the LoRa radio.
 * In the background it waits for the mesh instead. */
void ls_drone_watch_foreground(bool on);
/* Callback runs on the RF worker, after the bounded detector queue accepts.
 * Must be nonblocking; entry is call-local. NULL removes it. */
typedef void (*ls_drone_watch_notice_fn)(uint64_t id, bool approaching, void *ctx);
void ls_drone_watch_notice_hook(ls_drone_watch_notice_fn fn, void *ctx);
void ls_drone_watch_legacy(bool enabled); /* detector's old ELRS900 request */
void ls_drone_watch_step(int64_t now_us); /* worker only; bounded radio pass */
int ls_drone_watch_command(int argc, char **argv);
/* Producer API; all calls below are serialized on the RF worker. Useful for
 * recorded/fake feeds too. Packet activity is preamble/header only. */
void ls_drone_watch_lora900(unsigned band, unsigned channel, float dbm, int64_t us);
void ls_drone_watch_activity(ls_dw_source_t source, uint64_t hz, float dbm, int64_t us);
/* Contiguous RSSI bins, each pass's actual measurement timestamp, not a timer.
 * Sequential sweeps are heuristic evidence, never proof of modulation. */
void ls_drone_watch_spectrum(ls_dw_source_t source, const float *dbm, size_t n,
    uint32_t lo_hz, uint32_t hi_hz, int64_t us);
/* Envelope power, >=2 ms at >=100 kS/s. No video demodulation or pixels. */
bool ls_drone_watch_video(const float *power, size_t n, uint32_t sample_rate);
void ls_drone_watch_analog(unsigned channel, const float *power, size_t n,
    uint32_t sample_rate, float level, bool calibrated, int64_t us);
/* Strict v1 numeric JSON schema, validated atomically; worker only, idle. */
bool ls_drone_watch_configure(const char *json, size_t len);
#ifdef __cplusplus
}
#endif
#endif
