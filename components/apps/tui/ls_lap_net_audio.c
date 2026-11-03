#include "ls_lap_net_audio.h"
#include "ls_lap_ble_peripheral.h"
#include "ls_lap_net.h"
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif
atomic_uint ls_lap_av_epochs[LS_LAP_CONTEXTS] = { [0 ... LS_LAP_CONTEXTS-1] = 1 };
atomic_bool ls_lap_av_actives[LS_LAP_CONTEXTS];
static void (*wake_io)(void);
void ls_lap_av_set_wake(void (*wake)(void)) { wake_io = wake; }
void ls_lap_av_wake(void) {
    if (wake_io)
        wake_io();
}
uint32_t ls_lap_av_now(void) {
#ifdef ESP_PLATFORM
    return esp_timer_get_time() / 1000;
#else
    extern uint32_t (*ls_lap_api_host_clock)(void);
    return ls_lap_api_host_clock ? ls_lap_api_host_clock() : 0;
#endif
}
bool ls_lap_av_string(char *out, size_t cap, const char *in) {
    if (!in)
        return false;
    for (size_t i = 0; i < cap; i++) {
        if (!ls_lap_api_readable(in + i, 1))
            return false;
        out[i] = in[i];
        if (!out[i])
            return true;
    }
    return false;
}
int ls_lap_av_access(uint32_t hw, bool network) {
    if (!ls_lap_api_inside() || !atomic_load(&ls_lap_av_active))
        return LAP_IO_EINVAL;
    if (network && !ls_lap_net_allowed())
        return LAP_IO_EACCES;
    if ((ls_lap_sys_caps() & hw) != hw)
        return LAP_IO_ENOTSUP;
    return 0;
}
void ls_lap_av_begin(void) {
    atomic_fetch_add(&ls_lap_av_epoch, 1);
    atomic_store(&ls_lap_av_active, true);
}
void ls_lap_av_end(void) {
    atomic_store(&ls_lap_av_active, false);
    atomic_fetch_add(&ls_lap_av_epoch, 1);
    ls_lap_av_wake();
}
bool ls_lap_av_service(void) {
    bool busy = ls_lap_sound_service();
    busy = ls_lap_ble_service() || busy;
    busy = ls_lap_peripheral_service() || busy;
    busy = ls_lap_udp_service() || busy;
    busy = ls_lap_mdns_service() || busy;
    return ls_lap_wifi_http_service() || busy;
}
