#ifndef LS_APRS_H
#define LS_APRS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define APRS_SAMPLE_RATE 16000
#define APRS_FRAME_MAX 512
#define APRS_STATIONS 64
typedef enum { APRS_POSITION, APRS_OBJECT, APRS_ITEM, APRS_WEATHER,
               APRS_MESSAGE, APRS_STATUS } aprs_kind_t;
typedef struct {
    char call[12], name[10], recipient[10], text[160];
    double lat, lon;
    char table, symbol;
    aprs_kind_t kind;
    bool position, killed, weather, ambiguous;
    /* Missing weather quantities stay absent, rather than becoming zero. */
    bool temperature_valid, humidity_valid, wind_valid;
    int temperature_f, humidity, wind_deg, wind_mph;
    bool gust_valid, rain_valid, pressure_valid;
    int gust_mph, rain_hour_hundredths, pressure_tenths_hpa;
    int64_t heard_us, position_us;
} aprs_packet_t;
typedef struct aprs_ctx aprs_ctx_t;
typedef void (*aprs_callback_t)(const aprs_packet_t *, void *);
bool aprs_parse(const char *source, const char *destination,
                const uint8_t *info, size_t n, aprs_packet_t *out);
uint16_t aprs_crc(const uint8_t *bytes, size_t n);
aprs_ctx_t *aprs_create(aprs_callback_t callback, void *user);
void aprs_destroy(aprs_ctx_t *s);
void aprs_reset(aprs_ctx_t *s);
/* Mono audio before volume and squelch, as in SAME. */
void aprs_process(aprs_ctx_t *s, const int16_t *pcm, int n);
/* An AX.25 frame includes its little-endian FCS, but no flags. */
bool aprs_frame(aprs_ctx_t *s, const uint8_t *frame, size_t n);
/* NRZI-decoded HDLC bits, LSB first; useful for transport tests. */
void aprs_bit(aprs_ctx_t *s, unsigned bit);
#endif
