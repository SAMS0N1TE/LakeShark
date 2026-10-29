#ifndef LS_DMR_H
#define LS_DMR_H

#include <stddef.h>
#include <stdbool.h>
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

/* The 48-bit pattern for a class, or NULL for DMR_SYNC_NONE.
 *
 * Every sync is outer symbols only, twelve +3 and twelve -3, and voice and
 * data are each other's negation: BS voice received through an inverted
 * discriminator IS BS data, symbol for symbol. A receiver that does not
 * know its polarity cannot tell the two apart from the sync, only from what
 * follows - a data burst's CRC, or a voice superframe's embedded LC. */
const uint8_t *dmr_sync_pattern(dmr_sync_class_t c);

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

/* RS(12,9) over GF(256) (ETSI TS 102 361-1 B.3.6): three parity octets, so
 * one octet in error can be corrected and two detected. `cw` is the
 * unmasked codeword, octet 0 first. Returns 0 if clean, 1 if one octet was
 * corrected in place, -1 if not correctable (cw untouched).
 *
 * A correcting decoder accepts about one random word in 5500 where the
 * checking one accepts one in 16.7 million, so receive paths decode with
 * dmr_lc_decode() first and only fall back to this, and count the two apart:
 * a corrected LC is not, on its own, evidence that a channel is DMR. */
int dmr_rs129_correct(uint8_t cw[12]);

/* dmr_lc_decode(), falling back to one-octet RS correction. Returns 0 on
 * failure, 1 if the parity was exact, 2 if an octet was corrected. */
int dmr_lc_correct(const uint8_t bits96[12], uint8_t data_type, dmr_lc_t *out);

/* Full Link Control opcodes with FID 0 (ETSI TS 102 361-2 7.1.1). */
#define DMR_FLCO_GROUP_VOICE   0x00u     /* Grp_V_Ch_Usr                   */
#define DMR_FLCO_UNIT_VOICE    0x03u     /* UU_V_Ch_Usr                    */
#define DMR_FLCO_TA_HEADER     0x04u     /* Talker Alias header            */
#define DMR_FLCO_TA_BLOCK1     0x05u     /* ... blocks 1-3 are 0x05..0x07  */
#define DMR_FLCO_TA_BLOCK3     0x07u
#define DMR_FLCO_GPS           0x08u     /* GPS Info                       */

/* The 72 LC bits as nine octets, for the opcodes whose payload is not the
 * service options / destination / source layout dmr_lc_t carries. */
void dmr_lc_pack(const dmr_lc_t *lc, uint8_t out[DMR_LC_BYTES]);

/* GPS Info LC (FLCO 8): position error class and a WGS84 fix in two's
 * complement - longitude 25 bits in units of 360/2^25 degrees, latitude 24
 * bits in units of 180/2^24. Returns 1 if `lc9` is a GPS Info LC. */
typedef struct {
    uint8_t position_error;              /* 0 <2 m ... 6 >200 km, 7 unknown */
    int32_t longitude_raw, latitude_raw;
    float   longitude, latitude;         /* degrees                         */
} dmr_gps_t;
int dmr_gps_decode(const uint8_t lc9[DMR_LC_BYTES], dmr_gps_t *out);

/* Talker Alias (FLCO 4 header, 5..7 blocks). The header carries format (2
 * bits: 7-bit, ISO 8-bit, UTF-8, UTF-16BE), length (5 bits) and 49 bits of
 * text; each block 56 more. The 7-bit format packs from the first of the
 * header's 49 bits; the octet formats skip that bit and start on an octet
 * boundary. Up to 31 seven-bit, 27 octet or 13 UTF-16 characters. */
#define DMR_ALIAS_TEXT 48

typedef struct {
    uint8_t format;                      /* 0 7-bit, 1 ISO-8859-1, 2 UTF-8,
                                            3 UTF-16BE                      */
    uint8_t length;                      /* characters announced           */
    uint8_t have;                        /* bit 0 header, bits 1-3 blocks  */
    uint8_t raw[28];                     /* 49 + 3x56 = 217 bits           */
    char    text[DMR_ALIAS_TEXT];        /* UTF-8, what has arrived so far */
    bool    complete;                    /* every character announced is in */
} dmr_alias_t;

void dmr_alias_reset(dmr_alias_t *a);

/* Feed one LC. Returns true if it was a talker alias header or block and
 * the text was rebuilt. A header whose format or length differs from the
 * one held starts a new alias. */
bool dmr_alias_feed(dmr_alias_t *a, const uint8_t lc9[DMR_LC_BYTES]);

/* --------------------------------------------- CRC-protected blocks ---
 * CSBK and data header both ride BPTC(196,96) with a CRC-CCITT in place of
 * the RS parity: x^16 + x^12 + x^5 + 1 from zero, inverted, then masked by
 * data type (ETSI TS 102 361-1 B.3.7 and Table B.21). */
#define DMR_CRC_MASK_CSBK       0xA5A5u
#define DMR_CRC_MASK_DATA_HDR   0xCCCCu

uint16_t dmr_crc_ccitt(const uint8_t *data, size_t len);

/* Returns 1 if octets 10-11 hold the masked CRC of octets 0-9. */
int dmr_block_crc_ok(const uint8_t bits96[12], uint16_t mask);

/* CSBK opcodes with FID 0 (ETSI TS 102 361-2 7.1.2, 361-4 for Tier III). */
#define DMR_CSBKO_UU_V_REQ      0x04u    /* Unit to Unit Voice Service Request */
#define DMR_CSBKO_UU_ANS_RSP    0x05u    /* Unit to Unit Voice Service Answer  */
#define DMR_CSBKO_CT_CSBK       0x07u    /* Channel Timing                     */
#define DMR_CSBKO_NACK_RSP      0x26u    /* Negative Acknowledge Response      */
#define DMR_CSBKO_BS_DWN_ACT    0x38u    /* BS Outbound Activation             */
#define DMR_CSBKO_PREAMBLE      0x3Du    /* Preamble CSBK                      */

typedef struct {
    bool     last_block, protect_flag;
    uint8_t  opcode;                     /* 6 bits                         */
    uint8_t  fid;
    uint8_t  data[8];                    /* the opcode's 64 bits           */
    bool     has_addresses;              /* target/source below are valid  */
    uint32_t target, source;
    uint8_t  service_options;            /* UU_V_Req, UU_Ans_Rsp           */
    uint8_t  answer;                     /* UU_Ans_Rsp: 0x20 proceed,
                                            0x21 deny                      */
    bool     preamble_data;              /* Pre_CSBK: data (vs CSBK) follows */
    bool     preamble_group;             /* Pre_CSBK: target is a group    */
    uint8_t  blocks_to_follow;           /* Pre_CSBK                       */
} dmr_csbk_t;

/* Check the CSBK CRC and parse. Returns 1 on success; *out is untouched on
 * failure. Opcodes this does not know still parse to opcode/fid/data. */
int dmr_csbk_decode(const uint8_t bits96[12], dmr_csbk_t *out);
const char *dmr_csbk_name(uint8_t opcode, uint8_t fid);

/* Data header (ETSI TS 102 361-1 9.2.6). The common fields are where every
 * format puts them; blocks_to_follow folds the confirmed/unconfirmed BF and
 * the short-data appended-block count together. */
#define DMR_DPF_UDT             0x0u
#define DMR_DPF_RESPONSE        0x1u
#define DMR_DPF_UNCONFIRMED     0x2u
#define DMR_DPF_CONFIRMED       0x3u
#define DMR_DPF_SHORT_DEFINED   0xDu
#define DMR_DPF_SHORT_RAW       0xEu
#define DMR_DPF_PROPRIETARY     0xFu

typedef struct {
    uint8_t  dpf;
    bool     group, response_requested;
    uint8_t  sap;                        /* service access point           */
    uint32_t destination, source;        /* not set for proprietary        */
    uint8_t  blocks_to_follow;
    bool     full_message;               /* F: (un)confirmed only          */
    uint8_t  pad_octets;                 /* POC: (un)confirmed only        */
    uint8_t  fsn;                        /* fragment sequence number       */
    uint8_t  rsp_class, rsp_type, rsp_status;   /* response only           */
} dmr_data_header_t;

int dmr_data_header_decode(const uint8_t bits96[12], dmr_data_header_t *out);
const char *dmr_dpf_name(uint8_t dpf);

/* ------------------------------------------------- embedded signalling ---
 * Voice bursts B-F replace the sync with EMB (16 bits, split 8 + 8 around
 * the middle) and 32 bits of embedded signalling. EMB is colour code, PI and
 * LCSS protected by QR(16,7,6): the (17,9) quadratic residue code's
 * generator x^8+x^5+x^4+x^3+1, shortened to 15 bits, plus even parity.
 *
 * Four fragments - LCSS first, continuation, continuation, last - make the
 * 128-bit embedded LC: 72 LC bits and a 5-bit checksum (the nine LC octets
 * summed, mod 31) in a matrix of seven Hamming(16,11,4) rows and a row of
 * column parity, sent down the columns (ETSI TS 102 361-1 B.2.1, B.3.11). */

#define DMR_LCSS_SINGLE   0u
#define DMR_LCSS_FIRST    1u
#define DMR_LCSS_LAST     2u
#define DMR_LCSS_CONT     3u

uint16_t dmr_emb_encode(uint8_t colour_code, bool pi, uint8_t lcss);

/* Returns the number of bits corrected (at most 2), or -1. */
int dmr_emb_decode(uint16_t emb, uint8_t *cc, bool *pi, uint8_t *lcss);

/* EMB and the embedded fragment out of a voice burst's middle 48 bits. */
uint16_t dmr_burst_emb(const uint8_t burst[DMR_BURST_BITS / 8]);
uint32_t dmr_burst_embedded(const uint8_t burst[DMR_BURST_BITS / 8]);

/* 72 LC bits (nine octets) to the four 32-bit fragments, in send order. */
void dmr_emb_lc_encode(const uint8_t lc9[DMR_LC_BYTES], uint32_t frag[4]);

/* The reverse, with Hamming correction on the rows, then column parity and
 * checksum. Returns the bits corrected, or -1. */
int dmr_emb_lc_decode(const uint32_t frag[4], uint8_t lc9[DMR_LC_BYTES]);

/* Reassembly across bursts, driven by LCSS. */
typedef struct {
    uint32_t frag[4];
    uint8_t  have;                       /* fragments in order so far */
} dmr_emb_asm_t;

void dmr_emb_asm_reset(dmr_emb_asm_t *a);

/* Feed one burst's fragment with its decoded LCSS. Returns 1 and fills lc9
 * when a last fragment completes a checked LC, -1 if it completed one that
 * failed, 0 otherwise. Anything out of order restarts the assembly. */
int dmr_emb_asm_feed(dmr_emb_asm_t *a, uint8_t lcss, uint32_t frag,
                     uint8_t lc9[DMR_LC_BYTES]);

/* Extract the DMR colour code from the burst's Slot Type field. Data-sync bursts
 * carry 20 bits of Slot Type - 10 before sync and 10 after (see ETSI
 * §9.1.3). The high nibble of the 8-bit data field is the colour code. This
 * routes through dmr_slot_type_decode() so bit errors that landed in the
 * Slot Type region are corrected before the nibble is read. */
uint8_t dmr_burst_colour_code(const uint8_t burst_bits[DMR_BURST_BITS / 8]);

/* Colour code AND data type from the burst's Slot Type, through the same
 * Golay(20,8,7) correction. Returns the number of bit errors corrected, or
 * -1 when the codeword was outside the correction radius - in which case
 * neither output is written, because a data type guessed from a broken
 * codeword sends the LC parse down the wrong branch. */
int dmr_burst_slot_type(const uint8_t burst_bits[DMR_BURST_BITS / 8],
                        uint8_t *cc_out, uint8_t *dt_out);

/* Extract the 196-bit BPTC block that straddles the sync in a burst. */
void dmr_burst_extract_bptc(const uint8_t burst_bits[DMR_BURST_BITS / 8],
                            uint8_t out_bits[DMR_BPTC_BITS / 8 + 1]);

int dmr_slot_type_decode(const uint8_t in[3],
                         uint8_t *cc_out,
                         uint8_t *dt_out);

/* Encode an 8-bit Slot Type data field (colour code | data type) into a
 * 20-bit Golay(20,8,7) codeword.  Bit 19 (MSB) carries the top bit of the
 * colour code; bit 0 (LSB) carries the low parity bit.  Fixtures and any
 * on-device Slot Type transmitter use this. */
uint32_t dmr_slot_type_encode(uint8_t data8);

/* ------------------------------------------------------------ framer ---
 * The step between a symbol stream and everything above: find where a burst
 * starts.  Sync sits at a fixed place INSIDE a burst - bits 108..155 of 264 -
 * so a burst is only complete 108 bits after its sync has gone by, and the
 * framer reports it then.
 *
 * Bit-fed rather than dibit-fed on purpose.  A dibit carries two bits and a
 * burst can complete on either of them, so a dibit-shaped call would have to
 * return two results or silently drop one.  A 4FSK caller pushes the two bits
 * of each symbol MSB first.
 *
 * The preceding 24 bits are retained as CACH for repeater slot identity.
 * Mobile sync has no CACH; the observer leaves its slot unknown. */

#define DMR_SYNC_OFFSET_BITS 108u    /* where sync sits inside a burst */

typedef struct {
    uint8_t  window[DMR_BURST_BITS / 8 + 3]; /* CACH followed by burst */
    unsigned filled;                      /* bits seen, saturating at 288 */
    unsigned since_hit;                   /* bits since the last report   */
    uint8_t  max_sync_errors;
} dmr_framer_t;

typedef struct {
    uint8_t          burst[DMR_BURST_BITS / 8];
    dmr_sync_class_t class_id;
    uint8_t          sync_errors;
    uint8_t          cach[3];
    bool             have_cach;
} dmr_burst_frame_t;

/* max_sync_errors of 5 is the OP25/DSD threshold for a 48-bit pattern, and is
 * what dmr_sync_detect() is documented against. */
void dmr_framer_reset(dmr_framer_t *f, uint8_t max_sync_errors);

/* Push one bit.  Returns true and fills `out` on the bit that completes a
 * burst whose sync matched.  Two reports can never overlap: a burst is only
 * offered once a full burst of bits has arrived since the previous one, which
 * is exactly the spacing of contiguous bursts and less than the 288-bit
 * spacing the outbound CACH produces. */
bool dmr_framer_bit(dmr_framer_t *f, int bit, dmr_burst_frame_t *out);

/* The last 288 bits as a burst, whether or not a sync sits in them: voice
 * bursts B-F carry EMB where the sync would be, so they are only found by
 * counting 576 bits on from the voice sync of burst A. class_id is NONE and
 * sync_errors 48. Returns false until a whole burst has been seen. */
bool dmr_framer_peek(const dmr_framer_t *f, dmr_burst_frame_t *out);

/* ----------------------------------------------------------- symsync ---
 * Symbol timing and level recovery on the discriminator output, so DMR does
 * not depend on the P25 frame-sync loop's clock. Input is the C4FM
 * pipeline's 48 kHz output: FM-discriminated and already through the RRC
 * matched filter, 10 samples a symbol.
 *
 * Timing is a Gardner detector on linearly interpolated strobes, which needs
 * no decisions and so works before the levels are known. Levels are the
 * outer (+3/-3) symbol means, tracked from decisions; the centre between
 * them is the DC, so a carrier offset moves both together and is followed.
 * If almost nothing has been sliced as +3 (or -3) for a while that side of
 * the eye is wider than the signal - what a stretch of noise before a mobile
 * keys up leaves behind - and it snaps to the extreme of the last 16 symbols.
 * Each side on its own: a carrier offset arriving with the signal can leave
 * one side right and the other far out.
 *
 * The last DMR_SS_RING samples are kept so a caller that finds a sync can
 * go back and re-slice the burst it sits in at a better phase. */

#define DMR_SS_RING 2048u                /* power of two; > one 288-bit slot */

typedef struct {
    float    ring[DMR_SS_RING];
    uint32_t n;                          /* samples pushed                 */
    float    t;                          /* samples to the next strobe     */
    float    period;                     /* samples per symbol             */
    float    nominal;
    float    prev;                       /* last strobe                    */
    float    hi, lo;                     /* +3 and -3 levels               */
    float    outer_hi, outer_lo;         /* share sliced +3 / -3, smoothed */
    float    recent[16];
    unsigned recent_at;
    uint32_t symbols;
} dmr_symsync_t;

typedef struct {
    float    value;
    float    center, umid, lmid;         /* slicer thresholds it was cut at */
    uint32_t at;                         /* index of the newest sample     */
    float    frac;                       /* strobe offset from it, (-1, 0] */
} dmr_symbol_t;

void dmr_symsync_reset(dmr_symsync_t *s, float samples_per_symbol);

/* One sample in. Returns true and fills `out` when a symbol strobe fell in
 * this sample. */
bool dmr_symsync_push(dmr_symsync_t *s, float x, dmr_symbol_t *out);

/* The input interpolated at `at + frac`; samples older than the ring read
 * as 0. */
float dmr_symsync_sample(const dmr_symsync_t *s, uint32_t at, float frac);

/* Overwrite the levels, e.g. with ones measured on a sync. */
void dmr_symsync_set_levels(dmr_symsync_t *s, float hi, float lo);

/* Nudge the clock: positive moves the next strobes later. */
void dmr_symsync_shift(dmr_symsync_t *s, float samples);

#ifdef __cplusplus
}
#endif

#endif /* LS_DMR_H */
