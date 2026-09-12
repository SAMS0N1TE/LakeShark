#include "ls_wireless.h"
#include <string.h>

static uint16_t rate(uint32_t delta, uint32_t ms)
{
    uint64_t value = (uint64_t)delta * 1000 / ms;
    return value > UINT16_MAX ? UINT16_MAX : (uint16_t)value;
}

void ls_wireless_history_push(ls_wireless_history_t *h, uint32_t now_ms,
    int wifi, bool wifi_valid, int bt, bool bt_valid, bool ready,
    uint32_t rx, uint32_t tx)
{
    uint32_t dt = now_ms - h->last_ms;
    if (h->count && dt < 1000) return;
    /* Long scans and time away break the trace instead of stretching time. */
    if (h->count && dt > 2500) {
        unsigned gaps = dt / 1000 - 1;
        if (gaps > LS_WIRELESS_SAMPLES) gaps = LS_WIRELESS_SAMPLES;
        while (gaps--) {
            memset(&h->samples[h->next], 0, sizeof(h->samples[0]));
            h->next = (h->next + 1) % LS_WIRELESS_SAMPLES;
            if (h->count < LS_WIRELESS_SAMPLES) h->count++;
        }
    }
    ls_wireless_sample_t p = {0};
    if (wifi_valid && wifi >= -127 && wifi <= 0) {
        p.wifi = (int8_t)wifi;
        p.valid |= LS_WIRELESS_WIFI_VALID;
    }
    if (bt_valid && bt >= -127 && bt <= 0) {
        p.bt = (int8_t)bt;
        p.valid |= LS_WIRELESS_BT_VALID;
    }
    if (h->count && ready && h->last_ready && dt <= 2500) {
        /* A counter reset is a new session; ordinary unsigned wrap is valid. */
        uint32_t dr = rx - h->last_rx, dtxt = tx - h->last_tx;
        if (dr < INT32_MAX && dtxt < INT32_MAX) {
            p.rx = rate(dr, dt);
            p.tx = rate(dtxt, dt);
            p.valid |= LS_WIRELESS_RATE_VALID;
        }
    }
    h->samples[h->next] = p;
    h->next = (h->next + 1) % LS_WIRELESS_SAMPLES;
    if (h->count < LS_WIRELESS_SAMPLES) h->count++;
    h->last_ms = now_ms;
    h->last_rx = rx;
    h->last_tx = tx;
    h->last_ready = ready;
}
