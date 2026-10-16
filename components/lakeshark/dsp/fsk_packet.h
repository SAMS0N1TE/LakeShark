/* 2-FSK packets of the kind a CC1101, SX1231/RFM69 or Si443x sends:
   a 1010 preamble, a sync word, a payload and usually a CRC-16.

   Two stages. fsk_pkt_iq16_edges() turns complex I/Q into signed run
   lengths in microseconds, positive for the upper tone and negative for
   the lower one (the edge format REC keeps), from the carrier gate and the
   instantaneous frequency. fsk_pkt_from_edges() takes edges from there or
   from REC's own FSK slicer, measures the symbol time from the preamble,
   slices the bits, finds the sync word and tries every payload length for a
   CRC that checks, with and without PN9 whitening, so nothing about the
   transmitter has to be known in advance.

   Pure: no radio, no allocation, no globals. */

#ifndef FSK_PACKET_H
#define FSK_PACKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FSK_PKT_MAX_BYTES 64

typedef enum {
    FSK_CRC_NONE,       /* no length had a CRC that checked              */
    FSK_CRC_CC1101,     /* CRC-16 poly 0x8005 init 0xFFFF (CC1101, Si443x) */
    FSK_CRC_CCITT,      /* CRC-16 poly 0x1021 init 0xFFFF                  */
    FSK_CRC_SX1231,     /* CRC-16 poly 0x1021 init 0x1D0F, output inverted */
} fsk_crc_t;

typedef struct {
    uint32_t baud;          /* measured from the preamble                   */
    uint32_t baud_std;      /* nearest common rate within 3 %, else 0       */
    int      preamble_bits;
    uint32_t sync;          /* as sent, first bit in the top of sync_bits   */
    uint8_t  sync_bits;
    uint8_t  sync_repeats;  /* 2 when the word was sent twice (D391 D391)   */
    bool     sync_known;    /* a default sync word of a common radio chip   */
    bool     inverted;      /* the lower tone is the 1                      */
    bool     whitened;      /* PN9 data whitening was removed               */
    bool     length_byte;   /* first byte counts the rest, as CC1101 does   */
    fsk_crc_t crc_kind;
    uint16_t crc;           /* as sent                                      */
    uint8_t  len;           /* payload bytes, length byte included          */
    uint8_t  data[FSK_PKT_MAX_BYTES];
} fsk_pkt_t;

/* What the I/Q stage measured about the carrier. */
typedef struct {
    float offset_hz;        /* midpoint between the tones, from the tune    */
    float deviation_hz;     /* half the tone spacing                        */
    float snr_db;           /* burst power over the floor                   */
    float burst_ms;         /* longest gated stretch                        */
} fsk_pkt_rf_t;

/* I/Q (interleaved little-endian int16 I,Q) to signed edges in us. The
   carrier dropping ends a run with a gap of its own length, sent as a low
   run, so a packet never spans two bursts. Returns the number of edges
   written, 0 when no burst stands 10 dB over the floor. `rf` may be NULL. */
size_t fsk_pkt_iq16_edges(const int16_t *iq, size_t pairs, uint32_t fs,
                          int32_t *edges, size_t cap, fsk_pkt_rf_t *rf);

/* Packets from signed edges in us (positive = upper tone). Repeats of the
   same packet in one capture are reported once. Returns how many. */
int fsk_pkt_from_edges(const int32_t *edges, size_t n, fsk_pkt_t *out, int max);

/* One line: "2-FSK 2400 bd sync D391x2 4 B 01 01 00 01 CRC 9435 ok". */
int fsk_pkt_describe(const fsk_pkt_t *p, char *out, size_t n);
/* Fits a narrow panel: "2400 bd D391x2 | 01 01 00 01 | CRC ok", the first
   six bytes then "..". */
int fsk_pkt_describe_short(const fsk_pkt_t *p, char *out, size_t n);

/* Exposed for the bench. */
uint16_t fsk_pkt_crc16(const uint8_t *m, size_t n, uint16_t poly, uint16_t init);
void fsk_pkt_pn9(uint8_t *buf, size_t n);   /* XOR in place, seed 0x1FF */

#ifdef __cplusplus
}
#endif

#endif /* FSK_PACKET_H */
