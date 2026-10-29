/* What the board's watchers see, held for the native app that watches
   (SDK 2.2 lap_seen_*): SWEEP's Bluetooth and Wi-Fi categories, Remote ID,
   the LoRa drone-link listener, drone watch, REC's watch and CATCH, the
   pager decoder and P25. DETECTOR is that app.

   Producers run on their own tasks. Each call is one short critical section
   into a 64-entry PSRAM queue and never waits; with no app watching the
   source it is one test and nothing is kept. An address never leaves this
   file: it becomes a pseudonym keyed for this boot. */
#ifndef LS_SEEN_H
#define LS_SEEN_H
#include <stdbool.h>
#include <stdint.h>
#include "ls_det_signal.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Equal to the SDK's LAP_SEEN_* bits (ls_lap_events.c checks). The decoder
   bits (REC, CATCH, POCSAG, P25) are in ls_det_signal.h. */
enum { LS_SEEN_RID = 1, LS_SEEN_BLE = 2, LS_SEEN_WIFI = 4, LS_SEEN_LORA = 8, LS_SEEN_DRONE = 32 };
#define LS_SEEN_ALL (LS_SEEN_RID | LS_SEEN_BLE | LS_SEEN_WIFI | LS_SEEN_LORA | LS_SEEN_DRONE | DET_SIGNALS)
/* And to LAP_SEEN_F_*. */
enum { LS_SEEN_F_VALIDATED = 1, LS_SEEN_F_FLOOD = 2, LS_SEEN_F_NEW = 4, LS_SEEN_F_EMERGENCY = 8, LS_SEEN_F_APPROACHING = 16 };
#define LS_SEEN_QUEUE 64
typedef struct {
    uint16_t source;
    uint8_t category, section;  /* SWEEP's (ls_sweep_app.h); 255 none */
    int8_t rssi;
    uint8_t flags;              /* LS_SEEN_F_* */
    uint16_t band;
    uint64_t entity;
    uint32_t a, b, hits_per_min;
    int64_t at_us;              /* esp_timer time it was seen */
    char text[64];
} ls_seen_t;

/* The sources the loaded app watches; 0 when none does. */
uint32_t ls_seen_wanted(void);
/* From lap_seen_watch, and 0 when the app closes. Changing the sources
   drops what waits. */
void ls_seen_want(uint32_t sources);
/* Wireless worker: keep running while an app watches or the drone-link
   listener is still winding down. */
bool ls_seen_pending(void);
void ls_seen_tick(void);
/* Screen task: the next sighting, oldest first. */
bool ls_seen_take(ls_seen_t *out);
uint32_t ls_seen_dropped(void);

/* Producers. Nonblocking and bounded; a full queue counts a drop. */
void ls_seen_observe(const uint8_t mac[6], uint8_t type, uint8_t source, uint8_t category,
                     int rssi, bool validated, bool flood, uint8_t section, int64_t now);
void ls_seen_drone(unsigned band, unsigned channel, unsigned rate_guess, int rssi,
                   unsigned hits_per_min, int64_t now);
/* Drone watch's alert: the app's while it watches LS_SEEN_DRONE, else the
   board's own banner and vibration. False when neither could take it. */
bool ls_seen_drone_watch(uint64_t id, int rssi, bool calibrated, unsigned band, bool approaching, int64_t now);
/* TUI task (ls_notify_poll): posts the board's own drone-watch banner. */
void ls_seen_banner_poll(void);
#ifdef __cplusplus
}
#endif
#endif
