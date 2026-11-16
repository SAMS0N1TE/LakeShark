#ifndef REC_SCOPE_H
#define REC_SCOPE_H
/* What SUB-GHZ's receiver hears right now, as a trace READ and RECORD RAW
   draw across the screen the way a Flipper's Read RAW does: one column per
   REC_SCOPE_BUCKET_US of signal, its loudest moment in dB over the noise
   floor, and whether that stood out as signal.

   Whichever source is listening feeds it - the LR2021's I/Q stream by
   sample magnitude, the SX1262 by its RSSI register, the RTL by REC's block
   peaks - and the scope keeps its own floor, so every source draws on the
   same scale. Writers run in stream callbacks and pollers: no locks, no
   allocation, one writer at a time (only one source listens at once). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REC_SCOPE_N          512      /* 2 s of history at 4 ms a column */
#define REC_SCOPE_BUCKET_US  4000u
#define REC_SCOPE_HOT        0x80u    /* the column stood out as signal  */
#define REC_SCOPE_DB         0x7Fu    /* dB over the floor, 0..127       */
/* How far over the floor counts as signal: 12 dB, the 4x magnitude the
   LR2021 and RTL slicers call a carrier. */
#define REC_SCOPE_HOT_DB     12

/* 16-bit little-endian I/Q pairs, `us_per_sample` apart. */
void rec_scope_iq16(const uint8_t *iq, size_t bytes, uint32_t us_per_sample);
/* One level reading in dB (dBm, dBFS, anything logarithmic) covering
   `span_us` of signal. */
void rec_scope_level(float db, uint32_t span_us);
/* A source stopped: the trace keeps what it has, the floor starts over. */
void rec_scope_reset(void);

/* The newest `n` columns, oldest first. Returns how many there were. */
int rec_scope_read(uint8_t *out, int n);
/* Milliseconds since the last column; large or negative when nothing feeds. */
int32_t rec_scope_age_ms(void);
/* The last column's level over the floor, and the floor itself (same unit
   as the source's: dBm for an RSSI source, dBFS-ish for I/Q). */
float rec_scope_now_db(void);
float rec_scope_floor_db(void);
/* The source's level is absolute dBm (the SX1262's RSSI), not relative. */
bool rec_scope_dbm(void);
void rec_scope_set_dbm(bool dbm);

#endif
