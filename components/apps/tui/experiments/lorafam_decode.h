/* Header-only recon: no payload decryption or personal content display. */
#ifndef LORAFAM_DECODE_H
#define LORAFAM_DECODE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint32_t dest, source, id;
    uint8_t flags, channel, route, type;
    bool source_known, source_hash;
} lorafam_mesh_t;
bool lorafam_meshtastic(const uint8_t *data, size_t n, lorafam_mesh_t *out);
bool lorafam_meshcore(const uint8_t *data, size_t n, lorafam_mesh_t *out);
typedef struct {
    uint32_t homeid;
    uint16_t control;
    uint8_t source, dest, type, length;
    bool dest_known, checksum_ok;
} lorafam_zwave_t;
bool lorafam_zwave(const uint8_t *data, size_t n, uint8_t rate, lorafam_zwave_t *out);
typedef struct { uint16_t length; bool crc16, whitening, mode_switch; } lorafam_phr_t;
bool lorafam_wisun_phr(uint16_t raw, lorafam_phr_t *out);
bool lorafam_wisun_mac(const uint8_t *data, size_t n, uint8_t *type);
#endif
