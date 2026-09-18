/* A captured FSK frame as a Flipper `.sub` the SubGhz app can replay.
 *
 * The Flipper transmits a saved file by keying GDO0 against a list of edge
 * durations while the CC1101 sits in asynchronous serial mode, so a `.sub`
 * is two things: a register preset that says how to modulate, and RAW_Data
 * that says when to flip.  For 2-FSK the preset picks the two tones and the
 * timings carry the bits - high is one tone, low is the other.
 *
 * That is why this exists rather than reusing the OOK writer in app_rec.c:
 * the stock `FuriHalSubGhzPresetOok650Async` is amplitude keying, and a
 * frequency-keyed frame saved under it replays as silence.  The preset here
 * is built from the capture's own bitrate and deviation, so a frame is
 * replayable by whatever heard it without the operator hand-editing
 * registers.
 *
 * All of this is CC1101 datasheet arithmetic and Flipper file format.  No
 * hardware, so the bench checks the bytes.
 */

#ifndef LS_SUB_FSK_H
#define LS_SUB_FSK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ls_fsk_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The CC1101 runs from a 26 MHz crystal on every module this would meet. */
#define LS_CC1101_XOSC_HZ  26000000u

/* DEVIATN: f_dev = f_xosc / 2^17 * (8 + DEVIATION_M) * 2^DEVIATION_E.
   Returns the nearest encodable register value, and reports what that value
   actually produces so a caller can say how far it had to move. */
uint8_t ls_cc1101_deviatn(uint32_t deviation_hz, uint32_t *actual_hz);

/* MDMCFG4's low nibble and MDMCFG3:
   rate = ((256 + DRATE_M) * 2^DRATE_E) * f_xosc / 2^28. */
void ls_cc1101_drate(uint32_t bitrate, uint8_t *drate_e, uint8_t *drate_m,
                     uint32_t *actual);

/* The bit stream a capture puts on air: preamble, then the sync word, then
   the payload, MSB first with no encoding.  Writes into `bits` one byte per
   bit, and returns how many - or 0 if they would not fit.

   The preamble is emitted as alternating ones and zeros starting with a one,
   which is 0xAA read from a byte boundary and 0x55 read one place over. The
   emitted stream is the same either way; only the name differs. */
size_t ls_fsk_bitstream(const ls_fsk_capture_t *capture, uint8_t *bits,
                        size_t max_bits);

/* Run-length encode a bit stream into Flipper RAW_Data durations: positive
   microseconds for a run of ones, negative for a run of zeros.  Returns how
   many durations were written, or 0 if they would not fit. */
size_t ls_fsk_raw_data(const uint8_t *bits, size_t n_bits, uint32_t bitrate,
                       int32_t *out, size_t max_out);

/* Read a run-length list back as bits: the exact inverse of
   ls_fsk_raw_data, so a frame that went into the archive as edge timings
   comes back out as the bits it was made of.

   This is what lets a demodulated capture be STORED as edges. The archive,
   its on-card format, the preview strip and the .sub export all already
   speak edges; a capture that invented a second representation would have
   to be threaded through every one of them. Returns the bit count, or 0 if
   they would not fit. */
size_t ls_fsk_bits_from_raw(const int32_t *raw, size_t n_raw, uint32_t bitrate,
                            uint8_t *bits, size_t max_bits);

/* And the payload those bits carry: skip the preamble and the sync word,
   then pack what is left MSB first. Returns the byte count.

   Refuses rather than guesses when the stream is shorter than its own
   preamble and sync - a partial frame packed into bytes looks like a real
   capture and would replay as noise. */
size_t ls_fsk_payload_from_raw(const int32_t *raw, size_t n_raw,
                               uint32_t bitrate, uint16_t preamble_bits,
                               uint8_t *out, size_t max_out);

/* How a capture was modulated, as far as anything knows.

   A zeroed struct is a capture whose source timed edges rather than
   demodulating - an RTL or a CC1101 - and that is the stock amplitude-keyed
   preset every Flipper already has. A deviation makes it frequency keyed and
   needs a preset built for it. */
typedef struct {
    uint32_t bitrate;        /* 0 when the source only timed edges */
    uint32_t deviation_hz;   /* 0 for amplitude keying */
} ls_sub_mod_t;

/* The Preset lines for a capture, terminated and ready to write into a .sub.
   snprintf semantics: returns the bytes it wanted, so more than `len` means
   truncated.

   Returns 0 when the modulation cannot be put on a CC1101 at all. That is a
   refusal, not a fallback: writing the stock amplitude preset over a
   frequency-keyed capture produces a file that loads, transmits, and is not
   the signal that was recorded - which is the failure that looks exactly like
   a replay that did nothing.

   One function because there were three, in three files, drifting: two of
   them carried a one-byte PA table the Flipper rejects outright, and the
   sampling-clock fix reached only the third. */
size_t ls_sub_preset_text(const ls_sub_mod_t *mod, char *out, size_t len);

/* The whole file, as text, into `out`.  Returns the number of bytes it
   wanted to write - so a return of more than `len` means truncated, the way
   snprintf reports it.  `name` is only a comment in the file. */
size_t ls_sub_fsk_render(const ls_fsk_capture_t *capture, const char *name,
                         char *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* LS_SUB_FSK_H */
