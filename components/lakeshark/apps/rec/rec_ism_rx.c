/* rec_ism_listen: the ISM front end on REC's SDR stream. REC owns the
   dongle, the session and the tuning; this only reads the blocks it hands
   its tap and asks it to retune between channels. */

#include "rec_ism.h"
#include "rec_state.h"
#include "rec_watch.h"

#include <math.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"

#define EDGES 4096

static EXT_RAM_BSS_ATTR int32_t s_ook[EDGES], s_fsk[EDGES];
static EXT_RAM_BSS_ATTR rec_ism_front_t s_front;
static EXT_RAM_BSS_ATTR rec_ism_work_t s_work;
static EXT_RAM_BSS_ATTR lr433_msg_t s_msgs[6];
static uint32_t s_plan[REC_ISM_PLAN_MAX];
static int s_plan_n, s_plan_at;
static uint32_t s_dwell_us;
static int64_t s_hop_at;
static volatile uint32_t s_hz, s_bursts, s_decoded;
static volatile bool s_on;
static uint32_t s_restore_hz;

/* rec_rx: one burst's two edge lists through both device families. */
static void burst(void *ctx, const int32_t *ook, int n_ook, const int32_t *fsk, int n_fsk, int peak)
{
    (void)ctx; (void)peak;
    int k = rec_ism_decode_edges(ook, n_ook, false, &s_work, s_msgs, 6);
    k += rec_ism_decode_edges(fsk, n_fsk, true, &s_work, s_msgs + k, 6 - k);
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < k; i++) {
        const rec_ism_msg_t m = { .m = s_msgs[i], .hz = s_hz, .rssi = NAN, .source = REC_SOURCE_RTL };
        rec_ism_publish(&m, now);
    }
    s_bursts++;
    s_decoded += (uint32_t)k;
}

/* rec_rx: each block, or a hop when the dwell is up. */
static void tap(const uint8_t *iq, size_t bytes, void *ctx)
{
    (void)ctx;
    if (!s_on) return;
    if (s_plan_n > 1) {
        const int64_t now = esp_timer_get_time();
        if (now >= s_hop_at) {
            s_hop_at = now + s_dwell_us;
            /* A burst in progress belongs to the old channel. */
            rec_ism_front_reset(&s_front);
            const int next = (s_plan_at + 1) % s_plan_n;
            if (rec_tune_transient(s_plan[next])) { s_plan_at = next; s_hz = s_plan[next]; }
            return;
        }
    }
    rec_ism_front_u8(&s_front, iq, bytes);
}

bool rec_ism_listen(const uint32_t *hz, int n, uint32_t dwell_ms)
{
    rec_set_iq_tap(NULL, NULL);
    s_on = false;
    if (!hz || n <= 0) {
        if (s_restore_hz) rec_tune_transient(s_restore_hz);
        s_restore_hz = 0;
        return true;
    }
    if (n > REC_ISM_PLAN_MAX) n = REC_ISM_PLAN_MAX;
    if (!s_restore_hz) s_restore_hz = rec_get_freq();
    memcpy(s_plan, hz, (size_t)n * sizeof(hz[0]));
    s_plan_n = n;
    s_plan_at = 0;
    s_dwell_us = (dwell_ms < 200 ? 200 : dwell_ms) * 1000u;
    s_hop_at = esp_timer_get_time() + s_dwell_us;
    s_hz = hz[0];
    rec_ism_front_init(&s_front, REC_RTL_RATE, s_ook, s_fsk, EDGES, burst, NULL);
    if (!rec_tune_transient(hz[0])) {
        s_restore_hz = 0;
        return false;
    }
    s_on = true;
    rec_set_iq_tap(tap, NULL);
    return true;
}

void rec_ism_listen_status(rec_ism_listen_t *out)
{
    if (!out) return;
    out->on = s_on;
    out->hz = s_hz;
    out->bursts = s_bursts;
    out->decoded = s_decoded;
    out->plan_n = s_plan_n;
    out->plan_at = s_plan_at;
}
