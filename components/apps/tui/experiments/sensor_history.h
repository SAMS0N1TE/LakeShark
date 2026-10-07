/* Bounded sensor histories. Caller owns storage and serializes access. */
#ifndef SENSOR_HISTORY_H
#define SENSOR_HISTORY_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "lr433_dec.h"

#define SH_SENSORS 32
#define SH_READINGS 256
#define SH_NAME 32
#define SH_DAY 86400LL
typedef struct {
    int64_t time;
    float temp_c, humidity, rain_mm, wind_ms, kpa;
    int8_t battery_ok;
} sh_reading_t;
typedef struct {
    bool used, hidden, own, alert_on, alarm, pending;
    uint8_t proto;
    int8_t channel;
    char id[16], name[SH_NAME];
    float threshold_c;
    int64_t last_seen, last_append;
    uint16_t head, count;
    sh_reading_t last, raw;
    sh_reading_t ring[SH_READINGS];
} sh_sensor_t;
typedef struct {
    bool show_strangers, fahrenheit;
    uint8_t days;
    sh_sensor_t sensor[SH_SENSORS];
} sh_store_t;
void sh_init(sh_store_t *s);
/* Returns a slot; appended distinguishes repeated bursts from new samples. */
int sh_receive(sh_store_t *s, const lr433_msg_t *m, int64_t now, bool *appended);
void sh_expire(sh_store_t *s, int64_t now);
int sh_order(const sh_store_t *s, uint8_t out[SH_SENSORS]);
const sh_reading_t *sh_at(const sh_sensor_t *s, unsigned oldest_index);
void sh_own(sh_sensor_t *s, bool own);
bool sh_threshold(sh_sensor_t *s, float c);
/* CSV uses canonical units; every textual field is quoted. */
bool sh_csv(FILE *f, const sh_sensor_t *s, const sh_reading_t *r);
#endif
