#ifndef LS_AIS_H
#define LS_AIS_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define AIS_CENTER_HZ 162000000u
#define AIS_SAMPLE_RATE 256000u
#define AIS_VESSELS 128
enum { AIS_POSITION = 1, AIS_MOTION = 2, AIS_NAME = 4, AIS_STATIC = 8, AIS_VOYAGE = 16 };
typedef struct {
    uint32_t mmsi;
    uint8_t message, fields, ship_type, nav_status, channel;
    bool position;
    double lat, lon;
    /* Raw AIS units preserve unavailable values: 1023 / 3600 / 511. */
    uint16_t sog, cog, heading;
    char name[21], callsign[8], destination[21];
    int64_t heard_us, position_us;
} ais_vessel_t;
typedef void (*ais_receive_fn)(const ais_vessel_t *, void *);
typedef struct ais_ctx ais_ctx_t;
/* Payload bytes are in message order, with MSB-first fields. */
bool ais_parse(const uint8_t *payload, size_t bits, ais_vessel_t *out);
uint16_t ais_fcs(const uint8_t *data, size_t bytes);
ais_ctx_t *ais_create(ais_receive_fn receive, void *user);
void ais_destroy(ais_ctx_t *ctx);
void ais_reset(ais_ctx_t *ctx);
/* Unsigned interleaved IQ at 256 kHz, centred between 87B and 88B. */
void ais_process(ais_ctx_t *ctx, const uint8_t *iq, size_t bytes);
/* NRZI discriminator levels, one per recovered symbol; also useful for replay. */
void ais_symbol(ais_ctx_t *ctx, unsigned channel, bool level);
#endif
