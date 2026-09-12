/* Host shim. Not compiled into firmware. */
#ifndef LS_SHIM_ESP_TIMER_H
#define LS_SHIM_ESP_TIMER_H
#include <stdint.h>
/* Monotonic microseconds. Deterministic in tests: the harness can freeze it so
   a decoder that timestamps its output produces byte-identical results run to
   run, which is what lets a fixture be compared exactly. */
int64_t esp_timer_get_time(void);
void    ls_shim_time_set(int64_t us);
void    ls_shim_time_advance(int64_t us);

void    ls_shim_time_live(int on);
#endif
