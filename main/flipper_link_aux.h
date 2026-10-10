#ifndef FLIPPER_LINK_AUX_H
#define FLIPPER_LINK_AUX_H
#include <stddef.h>
#include <stdint.h>
#define LS_AUX_WIRE_MAX 320
/* AUX v1 is opt-in: only an AUX request or SWEEP MUTE returns this frame.
 * No extension is added to the legacy periodic telemetry stream. */
typedef struct {
    int connected, saved, running, muted;
    char ssid[33], ip[16], drone_id[21];
    unsigned counts[5], drones;
    uint32_t serial[2];
    int rssi[2], nearest_m;
    uint8_t category[2];
} ls_link_aux_t;
int ls_link_aux_encode(char *buf, size_t len, const ls_link_aux_t *s);
#endif
