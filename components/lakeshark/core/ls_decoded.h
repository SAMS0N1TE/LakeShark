/* Decoded data as the board's decoders produce it: pager pages, APRS
   packets, AIS reports, ADS-B contacts, mesh messages. One timeline that any
   number of readers follow, each with its own cursor (native apps'
   lap_event_subscribe is one; the radio head's decoded list will be
   another).

   Decoders call ls_decoded_publish from their own task. It copies one entry
   into a 256-entry PSRAM ring under a short critical section and returns; it
   never waits on a reader. A reader that falls more than the ring behind
   loses the oldest entries and is told how many. With no reader registered
   publishing is one test and nothing is allocated. */
#ifndef LS_DECODED_H
#define LS_DECODED_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Append-only, and equal to the SDK's LAP_EV_* bits (ls_lap_events.c checks). */
typedef enum {
    LS_DECODED_PAGE = 1,       /* POCSAG or FLEX; from = protocol name, address = RIC */
    LS_DECODED_APRS = 2,       /* from = callsign (or object name), text = comment or message */
    LS_DECODED_AIS  = 4,       /* address = MMSI, from = vessel name when known */
    LS_DECODED_ADSB = 8,       /* address = ICAO, from = callsign */
    LS_DECODED_MESH = 16,      /* from = sender, text = message */
    /* The radio head's decoders (rx/ls_rx_plugin.h). */
    LS_DECODED_SAME   = 32,
    LS_DECODED_ACARS  = 64,
    LS_DECODED_SUBGHZ = 128,
    LS_DECODED_LORA   = 256,
    LS_DECODED_TEXT   = 512,   /* head notices: SHED, attach refused */
    /* TIME MACHINE's timeline (feature/ovh-time-machine-20261028): */
    LS_DECODED_P25    = 1024,  /* a call: address = talkgroup, from = "TG n", text = source unit */
    LS_DECODED_REC    = 2048,  /* SUB-GHZ watch / REC detection: from = radio, text = what it was */
    LS_DECODED_DETECT = 4096,  /* DETECTOR alert: from = source, text = reason */
} ls_decoded_kind_t;
#define LS_DECODED_KINDS 0x3FFu

typedef struct {
    uint16_t kind;             /* ls_decoded_kind_t */
    bool has_pos;
    uint32_t address;
    double lat, lon;
    int32_t alt_ft;
    int64_t at_us;             /* esp_timer time it was decoded */
    char from[24];
    char text[160];
    /* Appended for TIME MACHINE; 0 = not said, and the timeline finds them
       from the receiver that was recording at at_us. */
    uint32_t freq_hz;
    uint8_t radio;             /* LS_TM_RADIO_* (ls_tmach_ring.h) */
    char source[32];           /* who decoded it: "" the board, else a native app's id */
} ls_decoded_t;

#define LS_DECODED_RING 256u
#define LS_DECODED_READERS 4

void ls_decoded_publish(const ls_decoded_t *d);

/* A reader: a cursor into the ring and the kinds it wants. open returns a
   reader id >= 0 (the ring is allocated with the first), -1 when all are
   taken or PSRAM is short. A new reader starts at the newest entry: nothing
   from before it opened is replayed. */
int  ls_decoded_open(uint32_t kinds);
void ls_decoded_kinds(int reader, uint32_t kinds);
void ls_decoded_close(int reader);
/* Copies the next entry this reader wants into out: true when there was one.
   *lost (may be NULL) adds the entries it missed by falling behind. */
bool ls_decoded_next(int reader, ls_decoded_t *out, uint32_t *lost);
/* Skip to the newest entry. */
void ls_decoded_flush(int reader);

#ifdef __cplusplus
}
#endif
#endif
