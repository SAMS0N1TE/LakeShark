#ifndef UAT_DECODE_H
#define UAT_DECODE_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UAT GF polynomial 0x187, first root 120, primitive step 1.
   Systematic shortened RS(30,18) and RS(48,34), and uplink RS(92,72). */
bool uat_rs_encode(const uint8_t *data, int k, uint8_t *frame);
/* Returns corrected symbols, -1 if uncorrectable; leaves failures intact. */
int uat_rs_decode(uint8_t *frame, int n);
typedef struct {
    uint32_t address;
    uint8_t qualifier, type;
    bool position;
    double lat, lon;
    bool altitude_valid, velocity_valid, vert_rate_valid;
    int altitude, ns_velocity, ew_velocity, vert_rate;
    uint8_t airground, emitter_category;
    char callsign[9];
} uat_fix_t;
bool uat_frame_decode(const uint8_t *frame, int n, uat_fix_t *fix, int *corrected);
int uat_uplink_decode(const uint8_t *wire552, uint8_t *payload432);
#ifdef __cplusplus
}
#endif

#endif
