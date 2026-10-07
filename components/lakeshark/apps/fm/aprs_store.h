#ifndef LS_APRS_STORE_H
#define LS_APRS_STORE_H
#include "aprs.h"
typedef struct {
    uint32_t custom_hz;
    uint16_t keep_minutes, max_km;
    uint8_t preset; /* 0 US, 1 EU, 2 custom. */
    bool messages, map;
} aprs_options_t;
void aprs_options_load(void);
void aprs_options_get(aprs_options_t *out);
void aprs_options_set(const aprs_options_t *options);
uint32_t aprs_frequency(void);
void aprs_store_receive(const aprs_packet_t *packet, void *user);
/* The table remains available to MAP after leaving the FM receiver. */
int aprs_store_count(int64_t now);
bool aprs_store_get(int index, int64_t now, aprs_packet_t *out);
/* Copy one coherent frame's table into the caller's PSRAM buffer. */
int aprs_store_snapshot(aprs_packet_t *out, int capacity, int64_t now);
void aprs_store_clear(void);
int64_t aprs_store_last_heard(void);
/* Distance limits require a fresh receiver fix; absent fixes do not hide stations. */
bool aprs_visible(const aprs_packet_t *p, const aprs_options_t *o, int64_t now,
                  bool fix, double lat, double lon, int64_t fix_us,
                  double *km, double *bearing);
#endif
