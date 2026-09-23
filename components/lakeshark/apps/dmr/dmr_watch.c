#include "dmr_watch.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
static int64_t now_us(void) { return esp_timer_get_time(); }
#else
static int64_t s_fake_us;
static int64_t now_us(void) { return s_fake_us += 1000; }
#endif

/* Data types that carry a Link Control block worth parsing (ETSI TS 102
   361-1 Table 9.4): 1 is the voice LC header, 2 the terminator with LC. */
#define DMR_DT_VOICE_LC_HEADER 1u
#define DMR_DT_TERMINATOR_LC   2u

static dmr_watch_t s_state;
static dmr_framer_t s_framer[2];      /* [0] as received, [1] complemented */
static bool s_started;

void dmr_watch_reset(void)
{
    memset(&s_state, 0, sizeof(s_state));
    dmr_tracker_reset(&s_state.tracker);
    dmr_framer_reset(&s_framer[0], 5);
    dmr_framer_reset(&s_framer[1], 5);
    s_started = false;
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

    uint8_t cc = 0, dt = 0;
    if (dmr_burst_slot_type(f->burst, &cc, &dt) < 0) return;
    s_state.slot_type_ok++;
    s_state.colour_code = cc;

    /* A voice or data burst is tracked whichever slot it came from. The slot
       number needs the CACH ahead of the burst, which a sync search does not
       see, so both slots are not distinguished here - slot 1 stands for "a
       burst landed" until the CACH path exists. */
    dmr_tracker_burst(&s_state.tracker, 1, f->class_id);

    uint8_t coded[DMR_BPTC_BITS / 8 + 1];
    uint8_t data[12];
    dmr_burst_extract_bptc(f->burst, coded);
    if (dmr_bptc_decode(coded, data) < 0) { s_state.bptc_fail++; return; }
    s_state.bptc_ok++;

    if (dt != DMR_DT_VOICE_LC_HEADER && dt != DMR_DT_TERMINATOR_LC) return;

    dmr_lc_t lc;
    if (!dmr_lc_decode(data, dt, &lc)) return;
    s_state.lc_ok++;
    s_state.lc       = lc;
    s_state.have_lc  = true;
    s_state.lc_us    = now_us();
}

void dmr_watch_symbol(int symbol, int center, int umid, int lmid)
{
    if (!s_started) { dmr_watch_reset(); s_started = true; }
    s_state.symbols++;

    int hi, lo;
    symbol_bits(symbol, center, umid, lmid, &hi, &lo);

    dmr_burst_frame_t frame;
    if (dmr_framer_bit(&s_framer[0], hi, &frame)) on_burst(&frame, false);
    if (dmr_framer_bit(&s_framer[0], lo, &frame)) on_burst(&frame, false);
    if (dmr_framer_bit(&s_framer[1], !hi, &frame)) on_burst(&frame, true);
    if (dmr_framer_bit(&s_framer[1], !lo, &frame)) on_burst(&frame, true);
}

bool dmr_watch_get(dmr_watch_t *out)
{
    if (!out || !s_started) return false;
    *out = s_state;
    return true;
}
