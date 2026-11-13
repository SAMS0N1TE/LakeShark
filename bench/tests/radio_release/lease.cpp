/* The production radio lease (components/meshcore/ls_mesh.cpp) for
   test_radio_release. Which task is asking is the test's choice, not the
   host thread's: every app in a case is a task id on one thread. */
#define xTaskGetCurrentTaskHandle rr_current_task
extern "C" {
#include "esp_timer.h"
#include "esp_random.h"
}
#include "freertos/task.h"
#include <sys/stat.h>
#include <mutex>
extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t);
static int rr_mkdir(const char *, int) { return 0; }
#define mkdir(path, mode) rr_mkdir(path, mode)
static std::recursive_mutex rr_mutex;
#undef portENTER_CRITICAL
#undef portEXIT_CRITICAL
#define portENTER_CRITICAL(mux) rr_mutex.lock()
#define portEXIT_CRITICAL(mux) rr_mutex.unlock()
#include "../../../components/meshcore/ls_mesh.cpp"
#undef mkdir

extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 8192; }

/* No MeshCore task: a paused mesh is one somebody holds the radio from. */
extern "C" void rr_lease_reset(void)
{
    s_task = nullptr;
    memset(&s_fg_holders, 0, sizeof(s_fg_holders));
    memset(&s_bg_holders, 0, sizeof(s_bg_holders));
    memset(&s_svc_holders, 0, sizeof(s_svc_holders));
    memset(&s_bg_claims, 0, sizeof(s_bg_claims));
    s_radio_blocked = false; s_front_want = false; s_bg_allowed = true; s_tx_want_until = 0;
    radio_hold_update();
}
extern "C" bool rr_mesh_paused(void) { return s_radio_hold; }
/* MeshCore has just sent (an advert, a DM and its retries): it wants the
   chip for MESH_TX_WANT_US. */
extern "C" void rr_mesh_sent(void) { s_tx_want_until = esp_timer_get_time() + MESH_TX_WANT_US; }
static bool rr_in(const ls_radio_holders_t *h, uintptr_t task)
{
    for (unsigned i = 0; i < h->count; i++) if (h->task[i] == task) return true;
    return false;
}
/* 1 foreground, 2 background, 0 neither. */
extern "C" int rr_lease_of(uintptr_t task)
{
    return rr_in(&s_fg_holders, task) ? 1 : rr_in(&s_bg_holders, task) ? 2 : 0;
}
extern "C" void rr_lease_describe(char *out, size_t len)
{
    snprintf(out, len, "fg=%u bg=%u paused=%d front=%d bg_allowed=%d", s_fg_holders.count,
             s_bg_holders.count, (int)s_radio_hold, (int)s_front_want, (int)s_bg_allowed);
}
