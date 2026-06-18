
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

#ifdef __cplusplus
}
#endif

#endif
