#ifndef LS_RID_H
#define LS_RID_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define LS_RID_MAX 32
typedef struct {
    uint32_t serial;
    uint8_t mac[6], address_type, uas_raw[20], id_type, ua_type;
    uint8_t counter, version, status, height_agl, auth_type, auth_page;
    uint8_t description_type, operator_id_type, operator_location_type;
    uint8_t classification_type, category, class_id;
    uint16_t messages, area_count, area_radius_m;
    int8_t rssi;
    int64_t last_us, location_us, basic_us;
    char uas_id[41], self_id[24], operator_id[21];
    double lat, lon, operator_lat, operator_lon;
    float geo_m, baro_m, height_m, speed_mps, vertical_mps, track_deg;
    float area_ceiling_m, area_floor_m, operator_geo_m;
    bool position_valid, operator_valid;
} ls_rid_drone_t;
/* Complete AD payload, including length/type. No connections or TX. */
bool ls_rid_advert(const uint8_t mac[6], uint8_t address_type, int rssi,
                   const uint8_t *data, size_t len, int64_t now_us);
/* Recognizes the service envelope, even when the RID message is malformed. */
bool ls_rid_is_advert(const uint8_t *data, size_t len);
size_t ls_rid_snapshot(ls_rid_drone_t *out, size_t cap, int64_t now_us);
typedef struct { unsigned count; int nearest_m; char id[21]; } ls_rid_summary_t;
void ls_rid_summary(ls_rid_summary_t *out, bool gps, double lat, double lon, int64_t now);
void ls_rid_clear(void);
uint32_t ls_rid_generation(void);
int ls_rid_command(int argc, char **argv);
#ifdef __cplusplus
}
#endif
#endif
