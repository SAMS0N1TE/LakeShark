#ifndef LS_LAP_NET_AUDIO_H
#define LS_LAP_NET_AUDIO_H
#include "ls_lap_api.h"
#include "ls_lap_sys.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "esp_attr.h"
#define LAP_DEVICE 1
#else
#define LAP_DEVICE 0
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif
#endif
/* Each subsystem lock is try-only on the screen and producer tasks. Worker
   state never retains app pointers. Epoch invalidates resources at unload. */
extern atomic_uint ls_lap_av_epochs[LS_LAP_CONTEXTS];
extern atomic_bool ls_lap_av_actives[LS_LAP_CONTEXTS];
#define ls_lap_av_epoch ls_lap_av_epochs[ls_lap_context_index()]
#define ls_lap_av_active ls_lap_av_actives[ls_lap_context_index()]
uint32_t ls_lap_av_now(void);
void ls_lap_av_wake(void);
bool ls_lap_av_string(char *out, size_t cap, const char *in);
int ls_lap_av_access(uint32_t hardware, bool network);
void ls_lap_av_begin(void);
void ls_lap_av_end(void);
bool ls_lap_av_service(void);
void ls_lap_av_set_wake(void (*wake)(void));
bool ls_lap_wifi_http_service(void);
bool ls_lap_udp_service(void);
bool ls_lap_mdns_service(void);
bool ls_lap_ble_service(void);
bool ls_lap_sound_service(void);
bool ls_lap_mic_active(void);
void ls_lap_av_script(const char *command);
void ls_lap_ble_advert_push(const uint8_t *addr, uint8_t type, int rssi, bool response, const void *data,
                            size_t n, uint32_t at_ms);
/* Only main's existing NimBLE host consumes commands and produces events. */
typedef struct {
    int kind, handle, operation;
    uint16_t attribute, end;
    uint8_t addr[6], addr_type, len, data[20];
} ls_lap_ble_command_t;
bool ls_lap_ble_command(ls_lap_ble_command_t *out);
void ls_lap_ble_event(const lap_ble_event_t *event, const void *data, size_t n);
void ls_lap_ble_host_pump(void);
int ls_lap_ble_lease(bool on);
bool ls_lap_api_data_path(char *out, size_t cap, const char *name);
#if !LAP_DEVICE
typedef struct {
    int16_t pcm[512];
    uint32_t frames, count;
} ls_lap_sound_host_t;
ls_lap_sound_host_t *ls_lap_sound_host(void);
void ls_lap_sound_script(const char *command);
void ls_lap_ble_script(const char *command);
void ls_lap_mdns_script(const char *command);
void ls_lap_udp_script(const char *command);
#endif
/* Old handles cannot overwrite a newer close request for a reused slot. */
static inline void av_cancel(atomic_int *slot, int ticket) {
    int old = atomic_load(slot);
    while (ticket > old && !atomic_compare_exchange_weak(slot, &old, ticket)) {
    }
}
/* Validate close without taking a worker lock; arbitrary/stale handles cannot
   poison the cancellation watermark of a current resource. */
static inline void av_close(atomic_int *request, atomic_int *published, int h) {
    if (h > 0 && atomic_load_explicit(published, memory_order_acquire) == h)
        av_cancel(request, h);
}
void ls_lap_ble_closed(int handle);
static inline bool av_take(atomic_flag *lock) {
    return !atomic_flag_test_and_set_explicit(lock, memory_order_acquire);
}
static inline void av_give(atomic_flag *lock) { atomic_flag_clear_explicit(lock, memory_order_release); }
static inline lap_operation_t av_status(int state, int error) {
    return (lap_operation_t){.size = sizeof(lap_operation_t), .version = 1, .state = state, .error = error};
}
#endif
