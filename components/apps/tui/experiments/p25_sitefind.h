/* P25 SITE FINDER's pure half: a P25 Phase 1 NID (NAC and DUID) recovered
   from the sign of each C4FM symbol, which is all a two-level FSK receiver
   gives.

   C4FM sends a dibit as one of four frequencies, +1800, +600, -600 and
   -1800 Hz for 01, 00, 10 and 11. A two-level demodulator at 4800 bps slices
   each symbol by the sign of its frequency, so it delivers the dibit's first
   bit (or its complement, depending on which way the receiver counts) and
   loses the second.

   The frame sync 0x5575F5FF77FF is 24 dibits all at +-1800 Hz, so it comes
   through whole: its 24 sign bits are 0x04CF5F, or 0xFB30A0 the other way
   round. The radio matches that as its sync word.

   After the sync come the NID's 32 dibits with one status symbol after the
   eleventh (frame symbol 35, as OP25's p25_framer.cc drops it), so 33 sign
   bits, one of them ignored. The NID is a BCH(63,16) codeword plus a parity
   bit; in OP25's numbering (bch.cc) codeword bit i is NID bit i+1 counted
   from the last one sent, and the 16 information bits, NAC then DUID, are
   codeword bits 62..47. The sign bits are the NID bits sent first in each
   dibit, which are codeword bits 62, 60, ..., 0: every even one.

   Those 32 bits alone still name the codeword. Over all 65536 NIDs the
   projection onto them is one to one with a minimum distance of 6, so the
   nearest projected codeword is the NID whenever no more than two sign bits
   are wrong, and three wrong are always seen as wrong. */
#ifndef P25_SITEFIND_H
#define P25_SITEFIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define P25SF_FS             0x5575F5FF77FFull
#define P25SF_SYNC_BITS      24
#define P25SF_SYNC_NORMAL    0x04CF5Fu
#define P25SF_SYNC_INVERTED  0xFB30A0u
/* 33 sign bits in whole bytes. */
#define P25SF_PAYLOAD_BYTES  5
/* Where the status symbol falls among the 33. */
#define P25SF_STATUS_INDEX   11
/* Accepted when the nearest NID is this close. */
#define P25SF_MAX_DIST       2

/* The 24 sign bits of a 48-bit frame sync: the first bit of every dibit. */
uint32_t p25sf_fs_signs(uint64_t fs);

/* The 64-bit NID for a NAC and DUID, first bit sent in bit 63: BCH(63,16)
   then the parity bit, which is 1 for LDU1 and LDU2 and 0 otherwise. */
uint64_t p25sf_nid_encode(uint16_t nac, uint8_t duid);

/* The 32 sign bits of a NID, first one sent in bit 31. */
uint32_t p25sf_nid_signs(uint64_t nid);

/* The 32 NID sign bits out of the radio's payload: 33 bits MSB first after
   the sync word, the status symbol's dropped, all flipped for a session that
   matched the inverted sync. */
uint32_t p25sf_payload_signs(const uint8_t *payload, bool inverted);

typedef struct {
    uint16_t nac;
    uint8_t  duid;
    uint8_t  dist;      /* sign bits that disagree with that NID */
} p25sf_nid_t;

/* The NID nearest `signs`, in `out`. True when it is within P25SF_MAX_DIST
   and its DUID is one P25 uses. */
bool p25sf_nid_decode(uint32_t signs, p25sf_nid_t *out);

bool p25sf_duid_valid(uint8_t duid);
/* "HDU", "TDU", "LDU1", "TSBK", "LDU2", "PDU", "TDULC", or "?". */
const char *p25sf_duid_name(uint8_t duid);
/* 0..6 for the seven valid DUIDs in that order, -1 otherwise. */
int p25sf_duid_slot(uint8_t duid);

/* CONTROL CHANNELS FROM A P25 PROFILE (P25_PROFILE_FORMAT.md). Each
   control= line gives one, its Hz before any '|'. A comment that opens a
   section, "# --- Manchester NH -- ... NAC 8A1 ---", names the site and the
   NAC every control after it should carry; a comment "# 8A1 Manchester"
   does the same. A section header without a NAC clears it. */
#define P25SF_LABEL 18
typedef struct {
    uint32_t hz;
    int16_t  nac;                   /* -1 when the profile does not say */
    char     label[P25SF_LABEL];
} p25sf_chan_t;

/* Returns the number of channels written to `out` (at most `max`), from
   `len` bytes of `text`, which need not be terminated. Duplicates and
   frequencies outside lo_hz..hi_hz are skipped. */
int p25sf_profile_parse(const char *text, size_t len, uint32_t lo_hz, uint32_t hi_hz,
                        p25sf_chan_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* P25_SITEFIND_H */
