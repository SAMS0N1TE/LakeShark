#pragma once
#include "ls_lap_sys.h"
void ls_lap_sensors_begin(void);
void ls_lap_sensors_end(void);
bool ls_lap_sensors_service(void);
uint32_t ls_lap_input_hardware(void);
bool ls_lap_sensors_script(const char *command);
#if !defined(ESP_PLATFORM) || defined(LS_LAP_HOST_TEST)
typedef struct {
  lap_gps_status_t gps;
  lap_gps_satellite_t satellites[48];
  unsigned satellite_count;
  uint32_t sentence_at, fix_at, sky_at;
  bool sentence_seen, fix_seen, sky_seen, gps_running;
  uint32_t motion_quality;
  float sigma;
} ls_lap_sensors_host_t;
ls_lap_sensors_host_t *ls_lap_sensors_host(void);
#endif
