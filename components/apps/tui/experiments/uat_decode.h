#ifndef UAT_DECODE_H
#define UAT_DECODE_H
#include <stdbool.h>
#include <stdint.h>
/* UAT GF polynomial 0x187, first root 120, primitive step 1.
   Systematic shortened RS(30,18) and RS(48,34). */
bool uat_rs_encode(const uint8_t *data, int k, uint8_t *frame);
/* Returns corrected symbols, -1 if uncorrectable; leaves failures intact. */
int uat_rs_decode(uint8_t *frame, int n);
typedef struct {
    uint32_t address;
    uint8_t qualifier, type;
    bool position;
    double lat, lon;
} uat_fix_t;
bool uat_frame_decode(const uint8_t *frame, int n, uat_fix_t *fix, int *corrected);
#endif
