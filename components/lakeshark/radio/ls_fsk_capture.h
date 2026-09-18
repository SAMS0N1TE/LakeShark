/* Frames heard on the FSK receiver, kept so they can be sent again.
 *
 * Read, save, replay - the loop a sub-GHz tool is for. What makes a capture
 * replayable is not the bytes on their own but the bytes TOGETHER WITH the
 * settings that heard them: a payload without its carrier, rate and deviation
 * is a frame nobody can put back on air. So a capture carries both, and
 * replay never has to ask the operator to remember how they tuned.
 *
 * Deliberately not a .sub reader. A Flipper's raw capture is a list of edge
 * timings for an OOK transmitter to key a carrier against, and this board's
 * radio has no OOK modulator - SX126x does LoRa, (G)FSK, (G)MSK and BPSK, and
 * that is the whole list. What this stores is a demodulated FSK frame, which
 * the same part can transmit exactly.
 *
 * No hardware in here, so the bench can fill the ring and read it back.
 */

#ifndef LS_FSK_CAPTURE_H
#define LS_FSK_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sixteen is what a bench session actually uses, and the ring costs its
   size in RAM whether or not anything is in it. */
#define LS_FSK_CAPTURE_MAX    16
#define LS_FSK_CAPTURE_BYTES  64

typedef struct {
    /* How it was heard, and therefore how it goes back out. */
    uint32_t freq_hz;
    uint32_t bitrate;
    uint32_t deviation_hz;
    uint32_t bandwidth_hz;
    uint32_t sync_word;
    uint16_t preamble_bits;

    uint8_t  data[LS_FSK_CAPTURE_BYTES];
    uint8_t  len;

    float    rssi_dbm;
    int64_t  first_us, last_us;
    /* How many identical frames folded into this one. A transmitter that
       repeats on a rhythm is one entry with a count, not a ring full of the
       same six bytes - which is the difference between a list you can read
       and a list you scroll past. */
    uint32_t heard;
} ls_fsk_capture_t;

void ls_fsk_capture_reset(void);

/* File a frame. An identical payload on identical settings bumps that
   entry's count and its last-heard time instead of taking a new slot;
   anything else takes the next one, evicting the oldest when full.

   Returns the slot it landed in, or -1 for a capture with no bytes, too many
   bytes, or a null pointer. */
int ls_fsk_capture_add(const ls_fsk_capture_t *capture);

int  ls_fsk_capture_count(void);

/* Oldest first, so an index stays put while later frames arrive - an
   operator who reads a list and then replays index 3 must get the frame they
   read, not whatever has since arrived at the head of a newest-first list. */
bool ls_fsk_capture_get(int index, ls_fsk_capture_t *out);

/* How many frames were dropped because the ring was full. Reported rather
   than hidden: a session that quietly lost the frame it was waiting for is
   worse than one that says so. */
uint32_t ls_fsk_capture_evicted(void);

/* The gap between the two most recent sightings of `index`, in
   milliseconds, or 0 when it has only been heard once. A transmitter on a
   rhythm names its own period this way. */
uint32_t ls_fsk_capture_period_ms(int index);

#ifdef __cplusplus
}
#endif

#endif /* LS_FSK_CAPTURE_H */
