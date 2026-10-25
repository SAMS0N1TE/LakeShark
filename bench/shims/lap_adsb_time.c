/* Standalone LAP tests: receiver time follows the app's injectable host clock. */
#include <stdint.h>
extern uint32_t (*ls_lap_api_host_clock)(void);
int ls_shim_log_enabled;
int64_t esp_timer_get_time(void)
{
    return ls_lap_api_host_clock ? (int64_t)ls_lap_api_host_clock() * 1000 : 0;
}
