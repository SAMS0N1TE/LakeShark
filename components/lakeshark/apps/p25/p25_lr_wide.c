#include "p25_lr_wide.h"
#include "esp_attr.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define WIDE_YIELD() vTaskDelay(1)
#else
#define WIDE_YIELD() do { } while (0)
#endif

/* Narrow at boot: P25's own capture is the proven path. Wide only when
   pinned (`p25 lrwide on`) or told to follow the shared setting. */
static atomic_int s_pref = P25_LR_WIDE_OFF;
void p25_lr_wide_set_pref(p25_lr_wide_pref_t pref)
{
    atomic_store(&s_pref, pref <= P25_LR_WIDE_OFF ? (int)pref : (int)P25_LR_WIDE_OFF);
}
p25_lr_wide_pref_t p25_lr_wide_pref(void) { return (p25_lr_wide_pref_t)atomic_load(&s_pref); }
bool p25_lr_wide_pick(p25_lr_wide_pref_t pref, bool shared_wide)
{
    return pref == P25_LR_WIDE_ON || (pref == P25_LR_WIDE_FOLLOW && shared_wide);
}
const char *p25_lr_wide_pref_name(p25_lr_wide_pref_t pref)
{
    return pref == P25_LR_WIDE_ON ? "on" : pref == P25_LR_WIDE_OFF ? "off" : "follow";
}

void p25_lr_wide_sink_consume(void *ctx, const uint8_t *data, size_t bytes, bool spare)
{
    (void)spare;   /* P25 never drops samples for time; a full queue breaks the run */
    p25_lr_wide_sink_t *k = ctx;
    /* seq_cst pair with close: either close sees busy, or this sees closed. */
    atomic_store(&k->busy, true);
    if (!data) {
        /* Closed, the staging belongs to the P25 task's next open. */
        if (atomic_load(&k->open)) { atomic_store(&k->broken, true); k->staged = 0; }
        atomic_fetch_add(&k->resets, 1);
    } else if (atomic_load(&k->open) && !atomic_load(&k->broken) && bytes % 4 == 0) {
        while (bytes) {
            size_t n = sizeof(k->stage) - k->staged;
            if (n > bytes) n = bytes;
            memcpy(k->stage + k->staged, data, n);
            k->staged += n; data += n; bytes -= n;
            if (k->staged < sizeof(k->stage)) break;
            if (!lr2021_iq_queue_push(&k->queue, k->stage, k->staged, k->pair, k->generation)) {
                atomic_store(&k->broken, true); k->staged = 0; break;
            }
            k->pair += k->staged / 4; k->staged = 0;
        }
    }
    atomic_store(&k->busy, false);
}

void p25_lr_wide_sink_close(p25_lr_wide_sink_t *k)
{
    atomic_store(&k->open, false);
    while (atomic_load(&k->busy)) WIDE_YIELD();
}

bool p25_lr_wide_sink_open(p25_lr_wide_sink_t *k, uint32_t generation)
{
    p25_lr_wide_sink_close(k);
    if (!lr2021_iq_queue_init(&k->queue)) return false;
    k->generation = generation; k->pair = 0; k->staged = 0;
    atomic_store(&k->broken, false);
    atomic_store(&k->open, true);
    return true;
}
