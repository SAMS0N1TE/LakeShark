/* GRAW DFM-06/09/17 radiosonde frames, from the chips the LoRa chip's FSK
   engine hands over to the telemetry they carry. Pure: no radio, no clock,
   no allocation, so the bench drives it with frames it built itself.

   On air: 2500 chips/s GFSK, Manchester, so 1250 data bits/s, sent without
   a break. A frame is 280 bits (224 ms): the 16-bit header 0x45CF, then
   three Hamming(8,4) blocks, each interleaved - a configuration block of 7
   codewords and two data blocks of 13. The header as chips is the 32-bit
   sync word the receiver matches; what follows it is 528 chips, 66 bytes,
   MSB first. A receiver whose FSK polarity is the other way round sees
   every chip inverted, the header included. */

#ifndef DFM_DECODE_H
#define DFM_DECODE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFM_CHIP_RATE   2500u          /* chips per second, two per bit   */
#define DFM_SYNC        0x9A995A55u    /* the header 0x45CF as 32 chips   */
#define DFM_SYNC_INV    0x6566A5AAu    /* the same, polarity reversed     */
#define DFM_HEADER      0x45CFu
#define DFM_FRAME_BITS  280
#define DFM_HEAD_BITS   16
#define DFM_CHIP_BYTES  66             /* (280 - 16) bits * 2 chips / 8   */

/* What one frame did. */
typedef struct {
    int blocks_ok;    /* of the three, how many passed Hamming              */
    int blocks_bad;   /* had a codeword that could not be corrected         */
    int corrected;    /* bits Hamming put right                             */
    int violations;   /* chip pairs that were 00 or 11, not Manchester      */
    bool position;    /* this frame completed a position                    */
} dfm_frame_info_t;

/* The telemetry, as of the last complete position. */
typedef struct {
    uint32_t serial;     /* 0 until both halves have arrived                 */
    bool     serial_hex; /* a DFM-06's, printed in hex; the others decimal    */
    uint8_t  subtype;    /* the serial channel, 0x6..0xD; 0 not yet known    */
    const char *type;    /* "DFM17", "DFM09", "DFM06", ... or "DFM"          */
    bool     have_fix;
    uint32_t fixes;      /* positions completed since dfm_init               */
    double   lat, lon;   /* degrees, north and east positive                 */
    float    alt_m;      /* above mean sea level where the sonde says how    */
    float    vel_h;      /* horizontal speed, m/s                            */
    float    heading;    /* degrees true, the way it is moving               */
    float    vel_v;      /* vertical speed, m/s, up positive                 */
    uint16_t year;
    uint8_t  month, day, hour, minute;
    float    sec;
    uint8_t  sats;       /* in the GPS solution, 0 when it does not say      */
    int      frnr;       /* the sonde's own frame number, -1 if it sends none */
    bool     have_batt;
    float    batt_v;
    bool     have_temp;
    float    temp_c;
} dfm_report_t;

/* Everything the decoder keeps between frames. */
typedef struct {
    dfm_report_t out;

    /* The serial number, assembled from two halves of one channel. */
    uint8_t  max_ch, nul_ch, sn_ch, chx_bits;
    uint32_t sn_x, chx[2];
    uint32_t sn6;
    uint16_t sonde_typ;  /* SN_BIT | channel once a serial is known */
    uint32_t sn;
    uint8_t  ptu_out;

    /* Measurement channels and whether each has arrived clean. */
    float    meas24[9];
    uint8_t  cfgchk24[9];
    bool     cfgchk;
    float    status[3];
    char     sensortyp;
    float    rf;

    /* The data packets of the cycle being gathered, and the frame each
       last arrived in. */
    uint32_t frame;
    uint32_t pck_at[9];
    int8_t   posmode;
    int      frnr;
    double   lat, lon, alt;
    float    dir, vel_h, vel_v, dmsl, sec;
    uint32_t prn;
    uint16_t year;
    uint8_t  month, day, hour, minute, nsv;
} dfm_t;

void dfm_init(dfm_t *d);

/* One frame: the DFM_CHIP_BYTES bytes that followed the header, MSB first.
   `inverted` says the receiver matched DFM_SYNC_INV, so every chip is turned
   over first. True when at least one block passed Hamming. `info` may be
   NULL. */
bool dfm_frame(dfm_t *d, const uint8_t *chips, bool inverted, dfm_frame_info_t *info);

/* Hamming(8,4) as the sonde sends it: four data bits first, MSB first, then
   four parity bits. The codeword is bit 7 first. For the bench encoder. */
uint8_t dfm_hamming_encode(uint8_t nibble);

#ifdef __cplusplus
}
#endif

#endif /* DFM_DECODE_H */
