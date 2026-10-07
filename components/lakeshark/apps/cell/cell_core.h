#ifndef LS_CELL_CORE_H
#define LS_CELL_CORE_H
#include <stdbool.h>
#include <stdint.h>
#define CELL_MAX_BINS 1400
#define CELL_RATE 256000u
#define CELL_BIN 25000u
#define CELL_GAIN 297
#define CELL_MISSING (-128)
typedef struct { const char *name; uint32_t low, high; } cell_band_t;
extern const cell_band_t cell_bands[];
extern const unsigned cell_band_count;
typedef struct {
    uint32_t magic, version, band, count, rate, gain, passes;
    int32_t lat_tile, lon_tile;
    uint32_t located, antenna, created;
    int8_t power[CELL_MAX_BINS];
    uint32_t checksum;
} cell_baseline_t;
uint32_t cell_checksum(const cell_baseline_t *b);
bool cell_baseline_valid(const cell_baseline_t *b, unsigned band, int lat, int lon, bool located, bool antenna);
/* Returns persistent changed-bin count. Missing data and motion reset streaks. */
unsigned cell_compare(const int8_t *base, const int8_t *now, uint8_t *streak,
                      unsigned n, bool comparable);
bool cell_motion_quiet(float ax, float ay, float az, float gx, float gy, float gz);

/* Stillness judged against the gyro's own resting reading, which on these
   boards is up to ~12 deg/s on one axis: a fixed limit on the raw rate reads
   every board as moving. The resting reading is followed slowly, so a turn
   reads as movement and a still board settles within a few seconds. */
typedef struct { float rest[3]; bool seeded; } cell_motion_t;
void cell_motion_reset(cell_motion_t *m);
bool cell_motion_step(cell_motion_t *m, float ax, float ay, float az,
                      float gx, float gy, float gz);
#endif
