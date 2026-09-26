/* Every radio as a signal level COMPASS can find a transmitter by.

   A source is chosen and, where it tunes, given a frequency or a target
   (a Wi-Fi network, a Bluetooth device, a mesh node). The field worker
   keeps a fresh level from it; the screen pairs each new level with the
   heading and hands both to ls_df. A radio this board cannot aim with says
   why, rather than being left out of the list.

   Two slots can run at once on radios that share no hardware, and a
   tunable radio can scan a list of channels. An SDR measures every channel
   that falls inside one capture together and retunes only for the rest;
   the nRF24 hears all its channels in one sweep; the SX1262 and CC1101
   retune to each channel for a dwell. Every level comes out of one queue,
   tagged with its slot and channel. */

#ifndef LS_DF_SOURCES_H
#define LS_DF_SOURCES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_DFS_MESH, LS_DFS_LORA, LS_DFS_RTL, LS_DFS_HACKRF, LS_DFS_CC1101,
    LS_DFS_NRF24, LS_DFS_WIFI, LS_DFS_BLE, LS_DFS_NFC, LS_DFS_GPS, LS_DFS_COUNT
} ls_dfs_t;

#define LS_DFS_SLOTS    2
#define LS_DFS_CHANNELS 8

typedef struct {
    ls_dfs_t source;
    bool active;          /* selected and running */
    bool fresh;           /* a level in the last 1.5 s */
    bool tunable;         /* takes a frequency */
    bool targets;         /* takes a chosen network, device or node */
    float level;
    const char *unit;
    uint32_t freq_hz;     /* the first channel */
    uint32_t updates;     /* bumps once per new level */
    char target[40];
    char status[96];
    int channels;         /* 1 unless scanning */
    int channel;          /* the channel being dwelt on */
    uint32_t freqs[LS_DFS_CHANNELS];
} ls_dfs_status_t;

enum { LS_DFS_READ_OVERLOAD = 1 };

typedef struct {
    int64_t us;
    uint32_t freq_hz;
    float level;
    uint8_t slot, channel, flags;
} ls_dfs_reading_t;

const char *ls_dfs_name(ls_dfs_t s);
/* NULL when the source can be used now, otherwise why not. */
const char *ls_dfs_unavailable(ls_dfs_t s);
/* NULL when `s` can run in `slot` beside the other slot's radio. */
const char *ls_dfs_conflict(int slot, ls_dfs_t s);
/* Whether the radio can tune to `hz`. */
bool ls_dfs_in_range(ls_dfs_t s, uint32_t hz);

/* Slot 0. */
bool ls_dfs_select(ls_dfs_t s, uint32_t freq_hz);
bool ls_dfs_tune(uint32_t freq_hz);
void ls_dfs_stop(void);
void ls_dfs_poll(ls_dfs_status_t *out);

bool ls_dfs_select_slot(int slot, ls_dfs_t s, uint32_t freq_hz);
void ls_dfs_stop_slot(int slot);
void ls_dfs_poll_slot(int slot, ls_dfs_status_t *out);
/* The channels a tunable slot scans, one at least; each dwells dwell_ms
   where it must be retuned to. Channels the radio cannot reach are left
   out; returns how many were kept. */
int  ls_dfs_set_channels(int slot, const uint32_t *hz, int n, uint32_t dwell_ms);
/* Levels heard since the last call, oldest first. */
int  ls_dfs_take(ls_dfs_reading_t *out, int max);

/* Targets for slot 0, strongest or most recent first. */
int  ls_dfs_target_count(void);
bool ls_dfs_target_label(int i, char *label, size_t lcap, char *detail, size_t dcap);
bool ls_dfs_target_pick(int i);   /* -1 follows everything the source hears */

/* The field worker runs this; it may block on a radio briefly. */
void ls_dfs_step(void);

#ifdef __cplusplus
}
#endif
#endif
