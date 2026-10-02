
#ifndef ADSB_DECODE_H
#define ADSB_DECODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void adsb_decode_init(void);

void adsb_on_sample(uint8_t *iq, int len);

void adsb_periodic_age(int64_t now_us);

void adsb_inject_fake_aircraft(void);

/* One received message, already demodulated and CRC-checked. The firmware
   reaches this through adsb_on_sample; the bench calls it directly. */
struct mode_s_msg;
void adsb_decode_on_message(struct mode_s_msg *mm);

/* Frame sources that find the Mode S frame themselves (the LR2021 OOK
   detector), so there is no magnitude vector to scan. They enter the decoder
   where mode_s_detect hands over: mode_s_decode, CRC check and repair, the
   known-address gate, then adsb_decode_on_message. Call from the one thread
   that calls adsb_on_sample; they share its decoder state. */
typedef enum {
    ADSB_FRAME_OK = 0,       /* decoded, CRC good, aircraft table updated */
    ADSB_FRAME_BAD_CHIPS,    /* malformed: bad length, or invalid chip pairs */
    ADSB_FRAME_BAD_CRC,      /* well formed, but the CRC (after repair) is bad */
} adsb_frame_result_t;

#define ADSB_RSSI_NONE (-32768)   /* signal level not available */

/* Chip layouts for adsb_on_chips. */
#define ADSB_CHIPS_FULL      0    /* chips start at the first bit after the preamble */
#define ADSB_CHIPS_DF17_HEAD 1    /* detector consumed the first four DF17 bits, "1000" */

/* An assembled message: 7 bytes (56 bits) or 14 bytes (112 bits). */
adsb_frame_result_t adsb_on_frame(const uint8_t *msg, int nbytes,
                                  int rssi_tenths_dbm);

/* The chips the LR2021 delivers after the preamble, 2 per bit, MSB first. */
adsb_frame_result_t adsb_on_chips(const uint8_t *chips, int nbytes,
                                  int variant, int rssi_tenths_dbm);

/* The same, and `first_bad_bit` (may be NULL) gets the message bit index of the
   first chip pair that was neither 10 nor 01, or -1 when every pair was valid
   or the frame was refused before its pairs were read. It is reported for any
   verdict: a frame can pass the pair rule and still carry bad pairs beyond
   bit 56 for the CRC to judge. */
adsb_frame_result_t adsb_on_chips_ex(const uint8_t *chips, int nbytes,
                                     int variant, int rssi_tenths_dbm,
                                     int *first_bad_bit);

/* Level of the last frame handed in with one, tenths of dBm, or ADSB_RSSI_NONE. */
int adsb_last_rssi_tenths_dbm(void);

#ifdef __cplusplus
}
#endif

#endif
