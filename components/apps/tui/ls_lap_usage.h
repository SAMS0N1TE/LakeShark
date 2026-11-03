#ifndef LS_LAP_USAGE_H
#define LS_LAP_USAGE_H
#include <stdint.h>
typedef struct { uint32_t bytes,handles; } ls_lap_usage_t;
ls_lap_usage_t ls_lap_net_usage(unsigned slot);
ls_lap_usage_t ls_lap_http_usage(unsigned slot);
ls_lap_usage_t ls_lap_udp_usage(unsigned slot);
ls_lap_usage_t ls_lap_mdns_usage(unsigned slot);
ls_lap_usage_t ls_lap_ble_usage(unsigned slot);
ls_lap_usage_t ls_lap_sound_usage(unsigned slot);
ls_lap_usage_t ls_lap_radio_usage(unsigned slot);
ls_lap_usage_t ls_lap_io_usage(unsigned slot);
ls_lap_usage_t ls_lap_audio_usage(unsigned slot);
ls_lap_usage_t ls_lap_system_usage(unsigned slot);
ls_lap_usage_t ls_lap_dsp_usage(unsigned slot);
ls_lap_usage_t ls_lap_sys_usage(unsigned slot);
#endif
