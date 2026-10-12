/* Host shim. Not compiled into firmware. */
#ifndef LS_SHIM_ESP_TIMER_H
#define LS_SHIM_ESP_TIMER_H
#include <stdint.h>
#include <stdbool.h>
/* Monotonic microseconds. Deterministic in tests: the harness can freeze it so
   a decoder that timestamps its output produces byte-identical results run to
   run, which is what lets a fixture be compared exactly. */
int64_t esp_timer_get_time(void);
void    ls_shim_time_set(int64_t us);
void    ls_shim_time_advance(int64_t us);

void    ls_shim_time_live(int on);
typedef struct ls_shim_timer *esp_timer_handle_t;
typedef struct {
    void (*callback)(void *);
    void *arg;
    int dispatch_method;
    const char *name;
} esp_timer_create_args_t;
#define ESP_TIMER_ISR 1
int esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *);
int esp_timer_start_once(esp_timer_handle_t, uint64_t);
int esp_timer_stop(esp_timer_handle_t);
int esp_timer_delete(esp_timer_handle_t);
void esp_timer_isr_dispatch_need_yield(void);
/* Deterministic one-shot ISR delivery when a task blocks on its notification.
   Hook runs before sleeping; lateness is additional scheduling delay. */
void ls_shim_timer_wait_hook(void (*hook)(void), int64_t late_us);
bool ls_shim_timer_notify_wait(void);
unsigned ls_shim_timer_count(void);
#endif
