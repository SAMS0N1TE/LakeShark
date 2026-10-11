#ifndef LS_UAT_WEATHER_H
#define LS_UAT_WEATHER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UAT_WX_TEXT_MAX 560
#define UAT_WX_TEXT_SLOTS 16
#define UAT_WX_BLOCK_SLOTS 64
#define UAT_WX_STATION_SLOTS 8
#define UAT_WX_MAX_AGE_US (60 * 60 * 1000000LL)
#define UAT_RADAR_MAX_AGE_US (20 * 60 * 1000000LL)
typedef struct {
    char text[UAT_WX_TEXT_MAX];
    char type[12], location[12], issued[16];
    int64_t received_us;
    bool valid;
} uat_weather_text_t;
typedef struct {
    uint32_t number;
    uint16_t product;
    uint8_t scale, pixels[128]; /* north to south, west to east: 4 x 32 */
    bool south, valid;
    float north, west, height, width;
    uint8_t hour, minute;
    int64_t received_us;
} uat_weather_block_t;
typedef struct {
    float lat, lon;
    uint8_t site, slot;
    bool position_valid, valid;
    int64_t received_us;
} uat_ground_station_t;
typedef struct {
    uint32_t apdus, text_records, radar_blocks, malformed, unsupported, truncated;
} uat_weather_stats_t;
void uat_weather_reset(void);
void uat_weather_uplink(const uint8_t payload[432], int64_t now_us);
/* Standalone APDU entry also used by host fixtures. false means malformed
   or unsupported; a rejected APDU never changes a stored radar block. */
bool uat_weather_apdu(const uint8_t *, size_t bytes, int64_t now_us);
bool uat_weather_text_get(int slot, int64_t now_us, uat_weather_text_t *);
bool uat_weather_station_get(int slot, int64_t now_us, uat_ground_station_t *);
/* Only fresh blocks containing this coordinate are returned. Multiple
   resolutions can overlap; the caller may prefer regional product 63. */
int uat_weather_blocks_at(double lat, double lon, int64_t now_us,
                          uat_weather_block_t *out, int capacity);
void uat_weather_stats(uat_weather_stats_t *);
#ifdef __cplusplus
}
#endif

#endif
