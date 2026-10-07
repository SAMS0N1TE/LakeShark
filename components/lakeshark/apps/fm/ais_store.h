#ifndef LS_AIS_STORE_H
#define LS_AIS_STORE_H
#include "ais.h"
typedef struct { uint16_t keep_minutes, max_km; bool names, map; } ais_options_t;
void ais_options_load(void);
void ais_options_get(ais_options_t *out);
void ais_options_set(const ais_options_t *options);
void ais_store_receive(const ais_vessel_t *vessel, void *user);
int ais_store_snapshot(ais_vessel_t *out, int capacity, int64_t now);
void ais_store_clear(void);
int64_t ais_store_last_heard(void);
bool ais_visible(const ais_vessel_t *p, const ais_options_t *o, int64_t now,
                 bool fix, double lat, double lon, int64_t fix_us, double *km, double *bearing);
#endif
