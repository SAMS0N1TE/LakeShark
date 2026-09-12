#ifndef LS_DMR_H
#define LS_DMR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DMR_BURST_BITS       264u
#define DMR_SYNC_BITS         48u
#define DMR_BPTC_BITS        196u
#define DMR_LC_BITS           96u        /* BPTC output: 72 LC + 24 RS parity */
#define DMR_LC_CONTENT_BITS   72u
#define DMR_LC_BYTES           9u

typedef enum {
    DMR_SYNC_NONE = 0,
    DMR_SYNC_BS_VOICE,
    DMR_SYNC_BS_DATA,
    DMR_SYNC_MS_VOICE,
    DMR_SYNC_MS_DATA,
} dmr_sync_class_t;

typedef struct {
    dmr_sync_class_t class_id;
    uint8_t          errors;              /* Hamming distance to best match. */
} dmr_sync_match_t;

/* dmr_sync_detect() looks at 48 bits (MSB first in bits[0]) and returns the
 * closest ETSI sync class if the Hamming distance is at most `max_errors`.
 * Threshold of 5 matches OP25/DSD practice for 48-bit patterns. */
dmr_sync_match_t dmr_sync_detect(const uint8_t bits[DMR_SYNC_BITS / 8],
                                 uint8_t max_errors);

/* Two-slot TDMA burst tracker. A DMR frame is 60 ms carrying one burst per
 * slot (30 ms each). The caller pushes decoded bursts as they arrive; the
 * tracker keeps a per-slot state so a burst lost in one slot does not affect
 * the other. Slots are 1-based to match the ETSI convention. */
typedef enum {
    DMR_SLOT_IDLE = 0,
    DMR_SLOT_VOICE,
    DMR_SLOT_DATA,
} dmr_slot_state_t;

typedef struct {
    dmr_slot_state_t state;
    uint32_t         bursts;              /* how many bursts landed in slot */
    uint32_t         losses;              /* consecutive losses since last hit */
    uint32_t         total_losses;        /* running counter across the call */
} dmr_slot_t;

typedef struct {
    dmr_slot_t slot[2];                   /* index 0 = slot 1, index 1 = slot 2 */
} dmr_tracker_t;

void dmr_tracker_reset(dmr_tracker_t *t);

/* Feed a burst outcome into the tracker. `slot` is 1 or 2; `class_id` is the
 * detected sync class (DMR_SYNC_NONE = burst lost / no sync). Returns 1 if the
 * update was accepted, 0 for an invalid slot number. */
int dmr_tracker_burst(dmr_tracker_t *t, unsigned int slot, dmr_sync_class_t class_id);

/* BPTC(196,96) forward and reverse (ETSI TS 102 361-1 Annex B.1).
 * bits_in / bits_out are packed MSB-first (bit i lives in bits[i/8] at position
 * (7 - i%8)). Errors are corrected using Hamming(15,11,3) row-wise then
 * Hamming(13,9,3) column-wise, then rows again for residual single-bit hits
 * (matching the pattern used by MMDVMHost). Correction cap is exercised in
 * bench/tests/test_dmr.c. */
void dmr_bptc_encode(const uint8_t data[12], uint8_t coded[DMR_BPTC_BITS / 8 + 1]);

/* dmr_bptc_decode() returns the number of bit errors that were corrected, or
 * -1 if the code word was not recoverable (an uncorrectable double error in a
 * row or column). Corrected data bits are packed into data_out[0..11] MSB-first
 * with only the first 96 bits used. */
int  dmr_bptc_decode(const uint8_t coded[DMR_BPTC_BITS / 8 + 1],
                     uint8_t data_out[12]);

/* Voice LC Header parse (ETSI TS 102 361-2 §7.1.1) from the 72-bit LC content.
 * The 72 bits sit in the top of a BPTC 96-bit block; RS(12,9) parity in the
 * low 24 bits is not verified by the raw parse call. Receive paths must use
 * dmr_lc_decode() to check the masked parity before publishing identity. */
typedef struct {
    uint8_t  protect_flag;                /* PF: 1 bit */
    uint8_t  flco;                        /* Full Link Control Opcode: 6 bits */
    uint8_t  fid;                         /* Feature set ID: 8 bits */
    uint8_t  service_options;             /* 8 bits */
    uint32_t destination;                 /* 24 bits (TG or radio ID) */
    uint32_t source;                      /* 24 bits (radio ID) */
} dmr_lc_t;

/* Parse the first 72 bits of the 96-bit LC block into a dmr_lc_t. */
void dmr_lc_parse(const uint8_t bits96[12], dmr_lc_t *out);

/* Validate full LC RS(12,9) parity, including the wire mask for data type
 * 1 (voice LC header) or 2 (terminator with LC), then parse. Returns 1 on
 * success; unsupported type, invalid parity or NULL returns 0 without
 * changing *out. Detects errors only; no RS correction or voice decoding. */
int dmr_lc_decode(const uint8_t bits96[12], uint8_t data_type, dmr_lc_t *out);

/* Extract the DMR colour code from the burst's Slot Type field. Every burst
 * carries 20 bits of Slot Type - 10 before sync and 10 after (see ETSI
 * §9.1.3). The high nibble of the 8-bit data field is the colour code. This
 * routes through dmr_slot_type_decode() so bit errors that landed in the
 * Slot Type region are corrected before the nibble is read. */
uint8_t dmr_burst_colour_code(const uint8_t burst_bits[DMR_BURST_BITS / 8]);

/* Extract the 196-bit BPTC block that straddles the sync in a burst. */
void dmr_burst_extract_bptc(const uint8_t burst_bits[DMR_BURST_BITS / 8],
                            uint8_t out_bits[DMR_BPTC_BITS / 8 + 1]);

/* Slot Type FEC per ETSI TS 102 361-1 §B.3.4 - Golay(20,8,7).
 *
 * The 20-bit codeword carries 8 data bits (colour code in the high nibble,
 * data type in the low nibble) with 12 parity bits and minimum distance 7,
 * so up to (7-1)/2 = 3 bit errors are correctable.
 *
 * `in` is 3 bytes packed MSB-first: byte 0 holds codeword bits 19..12,
 * byte 1 holds bits 11..4, byte 2's top nibble holds bits 3..0 (the low
 * nibble of in[2] is padding and ignored). Callers assemble this from the
 * two 10-bit Slot Type halves that straddle the burst sync.
 *
 * On success returns the number of bit errors corrected (0..3); on failure
 * (distance to nearest legal codeword > 3) returns -1 and leaves *cc_out /
 * *dt_out untouched. */
int dmr_slot_type_decode(const uint8_t in[3],
                         uint8_t *cc_out,
                         uint8_t *dt_out);

/* Encode an 8-bit Slot Type data field (colour code | data type) into a
 * 20-bit Golay(20,8,7) codeword.  Bit 19 (MSB) carries the top bit of the
 * colour code; bit 0 (LSB) carries the low parity bit.  Fixtures and any
 * on-device Slot Type transmitter use this. */
uint32_t dmr_slot_type_encode(uint8_t data8);

#ifdef __cplusplus
}
#endif

#endif /* LS_DMR_H */
