/* imbe_shim.h - plain-C entry to OP25's fixed-point IMBE decoder.
 *
 * Lets the C DSD decoder (dsd_main.c) call the C++ imbe_vocoder without pulling
 * C++ into that translation unit. decode_88 takes the 88-bit FEC-decoded IMBE
 * frame (11 bytes, u0..u7 = 12,12,12,12,11,11,11,7 bits, MSB-first) and writes
 * 160 int16 PCM samples. */
#ifndef IMBE_SHIM_H
#define IMBE_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void imbe_shim_init(void);
void imbe_shim_decode_88(const uint8_t *imbe88, int16_t *snd160);

#ifdef __cplusplus
}
#endif

#endif /* IMBE_SHIM_H */
