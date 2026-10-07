/* RS41 wire bytes are LSB first; this API accepts whitened logical bytes. */
#ifndef RS41_DECODE_H
#define RS41_DECODE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define RS41_STANDARD 320
#define RS41_EXTENDED 518
extern const uint8_t rs41_header[8];
extern const uint8_t rs41_mask[64];
typedef struct {
    bool status, position, measurement, gps_info, encrypted;
    char serial[9];
    uint16_t frame, week;
    uint32_t tow_ms, ptu[12];
    uint8_t sats;
    float battery, climb;
    double lat, lon, alt;
    unsigned corrected, bad_blocks, good_blocks;
} rs41_report_t;
/* Caller-owned scratch can live in PSRAM; decoding is reentrant. */
typedef struct {
    uint8_t frame[518], cw[255], exp[510], log[256];
    uint8_t syndrome[24], locator[25], previous[25], saved[25];
    uint8_t matrix[12][13], positions[12];
} rs41_decoder_t;
void rs41_init(rs41_decoder_t *d);
uint16_t rs41_crc(const uint8_t *p, size_t n);
void rs41_whiten(uint8_t *p, size_t n);
bool rs41_ecef(double x, double y, double z, double *lat, double *lon, double *alt);
bool rs41_decode(rs41_decoder_t *d, const uint8_t *wire, size_t n, rs41_report_t *out);
#endif
