/* Standalone LAP tests: receiver time follows the app's injectable host clock. */
#include <stdint.h>
extern uint32_t (*ls_lap_api_host_clock)(void);
int ls_shim_log_enabled;
int64_t esp_timer_get_time(void)
{
    return ls_lap_api_host_clock ? (int64_t)ls_lap_api_host_clock() * 1000 : 0;
}

/* Pure firmware decoders use the platform allocator. Standalone LAP tests
   do not link the harness clock/allocator object; keep their injected clock. */
#include <stdlib.h>
void *heap_caps_calloc(size_t n, size_t s, unsigned caps) { (void)caps; return calloc(n,s); }
void *heap_caps_malloc(size_t s, unsigned caps) { (void)caps; return malloc(s); }
void heap_caps_free(void *p) { free(p); }
void sys_log(unsigned char color, const char *fmt, ...) { (void)color; (void)fmt; }
void diag_line(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
