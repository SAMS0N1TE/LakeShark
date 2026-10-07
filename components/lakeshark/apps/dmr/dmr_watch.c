#include "dmr_watch.h"

#include <string.h>
#include <stdatomic.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&s_lock)
#define UNLOCK() portEXIT_CRITICAL(&s_lock)
#else
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif

#include "esp_timer.h"
static int64_t now_us(void) { return esp_timer_get_time(); }

/* Data types that carry a Link Control block worth parsing (ETSI TS 102
   361-1 Table 9.4): 1 is the voice LC header, 2 the terminator with LC. */
#define DMR_DT_VOICE_LC_HEADER 1u
#define DMR_DT_TERMINATOR_LC   2u

static dmr_watch_t s_state;
static dmr_framer_t s_framer[2];      /* [0] as received, [1] complemented */
static bool s_started;
static dmr_watch_t s_published;
static atomic_uint s_symbols;

unsigned dmr_cach_slot(const uint8_t cach[3])
{
    if (!cach) return 0;
    /* ETSI CACH TACT positions and systematic Hamming(7,4), also used by
       DSDcc dmr.cpp/fec.cpp. The TC bit is the second information bit. */
    static const unsigned pos[7] = {0, 4, 8, 12, 14, 18, 22};
    static const unsigned syndrome[7] = {5, 7, 3, 6, 1, 2, 4};
    unsigned bits[7], syn = 0;
    for (unsigned i = 0; i < 7; ++i) {
        bits[i] = (cach[pos[i] / 8] >> (7 - pos[i] % 8)) & 1;
        if (bits[i]) syn ^= syndrome[i];
    }
    for (unsigned i = 0; syn && i < 7; ++i)
        if (syndrome[i] == syn) { bits[i] ^= 1; break; }
    return bits[1] + 1;
}

void dmr_watch_reset(void)
{
    memset(&s_state, 0, sizeof(s_state));
    dmr_tracker_reset(&s_state.tracker);
    dmr_framer_reset(&s_framer[0], 5);
    dmr_framer_reset(&s_framer[1], 5);
    s_started = false;
    atomic_store(&s_symbols, 0);
    LOCK();
    memset(&s_published, 0, sizeof(s_published));
    UNLOCK();
}

/* ETSI TS 102 361-1 §5.1.1: +3 is 01, +1 is 00, -1 is 10, -3 is 11. The two
   bits go out MSB first, which is what the framer wants. */
static void symbol_bits(int symbol, int center, int umid, int lmid,
                        int *hi, int *lo)
{
    if (symbol > umid)        { *hi = 0; *lo = 1; }   /* +3 */
    else if (symbol > center) { *hi = 0; *lo = 0; }   /* +1 */
    else if (symbol > lmid)   { *hi = 1; *lo = 0; }   /* -1 */
    else                      { *hi = 1; *lo = 1; }   /* -3 */
}

static void on_burst(const dmr_burst_frame_t *f, bool inverted)
{
    s_state.bursts++;
    s_state.inverted         = inverted;
    s_state.last_class       = f->class_id;
    s_state.last_sync_errors = f->sync_errors;

    unsigned slot = 0;
    if ((f->class_id == DMR_SYNC_BS_DATA || f->class_id == DMR_SYNC_BS_VOICE) &&
        f->have_cach) slot = dmr_cach_slot(f->cach);
    if (!slot) s_state.unknown_slot++;
    else dmr_tracker_burst(&s_state.tracker, slot, f->class_id);
    int64_t now = now_us();
    dmr_call_t *call = slot ? &s_state.call[slot - 1] : NULL;
    if (call && call->active && now - call->last_us > 1500000) {
        call->active = false;
        call->end_us = call->last_us;
    }
    /* Voice A contains AMBE payload around sync, not Slot Type or BPTC LC. */
    if (f->class_id == DMR_SYNC_BS_VOICE || f->class_id == DMR_SYNC_MS_VOICE) {
        if (call && call->active) call->last_us = now;
        return;
    }
    uint8_t cc = 0, dt = 0;
    if (dmr_burst_slot_type(f->burst, &cc, &dt) < 0) return;
    s_state.slot_type_ok++;
    s_state.colour_code = cc;
    if (dt != DMR_DT_VOICE_LC_HEADER && dt != DMR_DT_TERMINATOR_LC) return;

    uint8_t coded[DMR_BPTC_BITS / 8 + 1];
    uint8_t data[12];
    dmr_burst_extract_bptc(f->burst, coded);
    if (dmr_bptc_decode(coded, data) < 0) { s_state.bptc_fail++; return; }
    s_state.bptc_ok++;
    dmr_lc_t lc;
    if (!dmr_lc_decode(data, dt, &lc)) return;
    s_state.lc_ok++;
    s_state.lc = lc;
    s_state.have_lc = true;
    s_state.lc_us = now;
    if (!call) return;
    bool same = call->have_lc && call->lc.source == lc.source &&
                call->lc.destination == lc.destination && call->colour_code == cc;
    if (!same || !call->active) call->start_us = now;
    call->lc = lc;
    call->have_lc = true;
    call->colour_code = cc;
    /* Service options privacy bit means encrypted voice. PF protects LC. */
    call->encrypted = (lc.service_options & 0x40u) != 0 || lc.protect_flag != 0;
    call->last_us = now;
    call->active = dt == DMR_DT_VOICE_LC_HEADER;
    call->end_us = call->active ? 0 : now;
}

void dmr_watch_symbol(int symbol, int center, int umid, int lmid)
{
    if (!s_started) { dmr_watch_reset(); s_started = true; }
    atomic_fetch_add(&s_symbols, 1);

    int hi, lo;
    symbol_bits(symbol, center, umid, lmid, &hi, &lo);

    dmr_burst_frame_t frame;
    if (dmr_framer_bit(&s_framer[0], hi, &frame)) on_burst(&frame, false);
    if (dmr_framer_bit(&s_framer[0], lo, &frame)) on_burst(&frame, false);
    if (dmr_framer_bit(&s_framer[1], !hi, &frame)) on_burst(&frame, true);
    if (dmr_framer_bit(&s_framer[1], !lo, &frame)) on_burst(&frame, true);
    if (s_state.bursts != s_published.bursts) {
        LOCK();
        s_published = s_state;
        UNLOCK();
    }
}

bool dmr_watch_get(dmr_watch_t *out)
{
    if (!out || !atomic_load(&s_symbols)) return false;
    LOCK();
    *out = s_published;
    UNLOCK();
    out->symbols = atomic_load(&s_symbols);
    return true;
}
