#include "dmr_watch.h"

#include <math.h>
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

#include "esp_attr.h"
#include "esp_timer.h"
static int64_t now_us(void) { return esp_timer_get_time(); }

/* Data types (ETSI TS 102 361-1 Table 9.4) this reads past the Slot
   Type: 1 is the voice LC header, 2 the terminator with LC, 3 a CSBK and 6
   a data header. */
#define DMR_DT_VOICE_LC_HEADER 1u
#define DMR_DT_TERMINATOR_LC   2u
#define DMR_DT_CSBK            3u
#define DMR_DT_DATA_HEADER     6u

/* A TDMA frame is two 30 ms slots of 288 bits (CACH or guard + burst), so
   the next burst of the same slot completes 576 bits after this one and the
   other slot's 288 after. */
#define SLOT_BITS   288u
#define FRAME_BITS  576u

/* Symbols whose sample positions are kept for re-slicing: a 288-bit window
   is 144 of them. */
#define POS_RING    256u
#define WINDOW_SYMS 144u

typedef struct {
    bool          on;
    uint32_t      next_bit;                /* where the next burst completes */
    uint8_t       index;                   /* 1..5: voice burst B..F         */
    float         hi, lo, d;               /* what burst A's sync measured   */
    dmr_emb_asm_t emb;
} flywheel_t;

static dmr_watch_t   s_state;
static dmr_framer_t  s_framer[2];         /* [0] as received, [1] negated  */
static flywheel_t    s_fly[2][2];         /* [polarity][slot - 1]          */
static uint32_t      s_bits;              /* bits pushed into each framer  */
static uint32_t      s_counted_bit;       /* last sync event counted       */
static bool          s_counted_any;
static uint32_t      s_ms_ref_bit;        /* mobile slot numbering anchor  */
static uint8_t       s_ms_ref_slot;
static uint32_t      s_gen, s_pub_gen;
static bool          s_started;
static dmr_watch_t   s_published;
static atomic_uint   s_symbols;
static atomic_bool   s_samples_fed;

/* PSRAM: 11 KB that sat in internal RAM, the DMA pool, from boot whether
   DMR ever heard anything or not. Only the decoder task touches them. */
static EXT_RAM_BSS_ATTR dmr_symsync_t s_sync;
static EXT_RAM_BSS_ATTR struct { uint32_t at; float frac; } s_pos[POS_RING];
static EXT_RAM_BSS_ATTR float s_val[POS_RING];   /* strobe values, for correlation */
static float         s_rho[2][2];         /* [shape][last, the one before]  */
static float         s_last_d;            /* last sync's grid off the loop    */
static float         s_last_hi, s_last_lo; /* and its +3 and -3 levels      */
static uint32_t      s_sym_n;             /* symbols out of s_sync          */

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
    s_state.polarity = -1;
    dmr_framer_reset(&s_framer[0], 5);
    dmr_framer_reset(&s_framer[1], 5);
    memset(s_fly, 0, sizeof(s_fly));
    s_bits = 0;
    s_counted_any = false;
    s_ms_ref_slot = 0;
    s_gen = s_pub_gen = 0;
    s_started = false;
    dmr_symsync_reset(&s_sync, 10.0f);
    s_sym_n = 0;
    memset(s_rho, 0, sizeof(s_rho));
    s_last_d = s_last_hi = s_last_lo = 0.0f;
    atomic_store(&s_symbols, 0);
    atomic_store(&s_samples_fed, false);
    LOCK();
    memset(&s_published, 0, sizeof(s_published));
    s_published.polarity = -1;
    UNLOCK();
}

/* ETSI TS 102 361-1 §5.1.1: +3 is 01, +1 is 00, -1 is 10, -3 is 11. The two
   bits go out MSB first, which is what the framer wants. */
static void symbol_bits(float symbol, float center, float umid, float lmid,
                        int *hi, int *lo)
{
    if (symbol > umid)        { *hi = 0; *lo = 1; }   /* +3 */
    else if (symbol > center) { *hi = 0; *lo = 0; }   /* +1 */
    else if (symbol > lmid)   { *hi = 1; *lo = 0; }   /* -1 */
    else                      { *hi = 1; *lo = 1; }   /* -3 */
}

/* ------------------------------------------------------------- calls --- */

static void call_expire(int64_t now)
{
    for (unsigned i = 0; i < 2; i++) {
        dmr_call_t *c = &s_state.call[i];
        if (c->active && now - c->last_us > DMR_CALL_TIMEOUT_US) {
            c->active = false;
            c->end_us = c->last_us;
            c->end_reason = DMR_END_TIMEOUT;
            s_gen++;
        }
    }
}

/* Identity for a slot from a checked LC - a header, a terminator or an
   embedded LC picked up mid-call. A different source, destination or colour
   code is a different call. */
static void call_identity(unsigned slot, const dmr_lc_t *lc, uint8_t cc,
                          bool terminator, bool embedded, bool corrected,
                          int64_t now)
{
    dmr_call_t *call = &s_state.call[slot - 1];
    const bool same = call->have_lc && call->lc.source == lc->source &&
                      call->lc.destination == lc->destination &&
                      call->colour_code == cc;
    if (!same || !call->active) {
        /* A terminator for a call never seen opening starts and ends here.
           Alias and position belong to the talker: kept for the same one,
           and for an identity that arrives after them in a late entry. */
        const bool keep = same || !call->have_lc;
        const dmr_alias_t alias = call->alias;
        const bool have_gps = call->have_gps;
        const dmr_gps_t gps = call->gps;
        memset(call, 0, sizeof(*call));
        if (keep) { call->alias = alias; call->have_gps = have_gps; call->gps = gps; }
        call->start_us = now;
        call->late_entry = embedded;
        call->corrected = corrected;
    } else if (!corrected) {
        call->corrected = false;
    }
    if (call->late_entry && !embedded) call->late_entry = false;
    call->lc = *lc;
    call->have_lc = true;
    call->colour_code = cc;
    call->group = lc->flco == DMR_FLCO_GROUP_VOICE;
    /* Service options privacy bit means encrypted voice. PF protects LC. */
    call->encrypted = (lc->service_options & 0x40u) != 0 || lc->protect_flag != 0;
    call->last_us = now;
    call->active = !terminator;
    call->end_us = terminator ? now : 0;
    call->end_reason = terminator ? DMR_END_TERMINATOR : DMR_END_NONE;
    s_gen++;
}

/* Talker alias and GPS ride LCs of their own and belong to whoever holds
   the slot. */
static void call_extra(unsigned slot, const uint8_t lc9[DMR_LC_BYTES], int64_t now)
{
    dmr_call_t *call = &s_state.call[slot - 1];
    dmr_gps_t gps;
    if (dmr_alias_feed(&call->alias, lc9)) s_gen++;
    else if (dmr_gps_decode(lc9, &gps)) { call->gps = gps; call->have_gps = true; s_gen++; }
    if (call->active) call->last_us = now;
}

static void apply_lc(unsigned slot, const uint8_t lc9[DMR_LC_BYTES], uint8_t cc,
                     bool terminator, bool embedded, bool corrected, int64_t now)
{
    if (!slot) return;
    uint8_t b12[12];
    memset(b12, 0, sizeof(b12));
    memcpy(b12, lc9, DMR_LC_BYTES);
    dmr_lc_t lc;
    dmr_lc_parse(b12, &lc);
    if (lc.flco == DMR_FLCO_GROUP_VOICE || lc.flco == DMR_FLCO_UNIT_VOICE)
        call_identity(slot, &lc, cc, terminator, embedded, corrected, now);
    else
        call_extra(slot, lc9, now);
}

/* ------------------------------------------------------------ bursts --- */

static void set_polarity(bool inverted)
{
    const int8_t p = inverted ? 1 : 0;
    if (s_state.polarity != p) { s_state.polarity = p; s_gen++; }
    s_state.inverted = inverted;
}

/* A checked block settles which reading of this burst was the real one -
   until then the counters held whichever framer reported first. */
static void confirm(const dmr_burst_frame_t *f, bool inverted)
{
    set_polarity(inverted);
    s_state.last_class       = f->class_id;
    s_state.last_sync_errors = f->sync_errors;
}

static unsigned burst_slot(const dmr_burst_frame_t *f, uint32_t end_bit)
{
    if (f->class_id == DMR_SYNC_BS_DATA || f->class_id == DMR_SYNC_BS_VOICE)
        return f->have_cach ? dmr_cach_slot(f->cach) : 0;
    if (f->class_id != DMR_SYNC_MS_DATA && f->class_id != DMR_SYNC_MS_VOICE)
        return 0;
    /* A mobile sends no CACH. Number its slots by timing instead: the
       first mobile burst heard is slot 1, one a whole number of frames
       later is too, one half a frame off is slot 2. Within 32 bits, to
       allow a few symbols of clock slip. */
    const uint32_t d = (end_bit - s_ms_ref_bit) % FRAME_BITS;
    if (s_ms_ref_slot && (end_bit - s_ms_ref_bit) < 4800u * 2u * 10u) {
        if (d < 32u || d > FRAME_BITS - 32u) { s_ms_ref_bit = end_bit; return s_ms_ref_slot; }
        if (d > SLOT_BITS - 32u && d < SLOT_BITS + 32u) {
            s_ms_ref_bit = end_bit;
            s_ms_ref_slot = (uint8_t)(3u - s_ms_ref_slot);
            return s_ms_ref_slot;
        }
    }
    s_ms_ref_bit = end_bit;
    s_ms_ref_slot = 1;
    return 1;
}

/* One voice burst B-F, found by counting from burst A. */
static void on_voice_embedded(const dmr_burst_frame_t *f, bool inverted,
                              unsigned slot, flywheel_t *fly, int64_t now)
{
    const bool mine = s_state.polarity == (inverted ? 1 : 0);
    uint8_t cc = 0, lcss = 0;
    bool pi = false;
    /* QR(16,7,6) corrects two, but at two a random word passes one time in
       four. The LCSS sequence is only worth following on a near-clean EMB. */
    const int e = dmr_emb_decode(dmr_burst_emb(f->burst), &cc, &pi, &lcss);
    if (mine) {
        s_state.voice_bursts++;
        dmr_call_t *call = &s_state.call[slot - 1];
        if (call->active) { call->last_us = now; call->voice_bursts++; }
        s_gen++;
    }
    if (e < 0 || e > 1) { dmr_emb_asm_reset(&fly->emb); return; }

    uint8_t lc9[DMR_LC_BYTES];
    const int r = dmr_emb_asm_feed(&fly->emb, lcss, dmr_burst_embedded(f->burst), lc9);
    if (r < 0) { if (mine) { s_state.emb_lc_fail++; s_gen++; } return; }
    if (r == 0) return;
    s_state.emb_lc_ok++;
    set_polarity(inverted);
    apply_lc(slot, lc9, cc, false, true, false, now);
    s_gen++;
}

static void on_burst(const dmr_burst_frame_t *f, bool inverted, uint32_t end_bit)
{
    /* Before the polarity is known both readings go through the data path
       - only a CRC can say which was right - but the burst is counted once.
       After, the other reading only gets a quiet look for a checked block,
       in case the first verdict came from a lucky word. */
    const bool known = s_state.polarity >= 0;
    const bool mine = !known || s_state.polarity == (inverted ? 1 : 0);
    const bool count = mine && !(s_counted_any && s_counted_bit == end_bit);
    const bool voice = f->class_id == DMR_SYNC_BS_VOICE || f->class_id == DMR_SYNC_MS_VOICE;
    const int64_t now = now_us();

    unsigned slot = 0;
    if (mine) slot = burst_slot(f, end_bit);
    else if (f->class_id == DMR_SYNC_BS_DATA && f->have_cach) slot = dmr_cach_slot(f->cach);

    if (count) {
        s_counted_bit = end_bit;
        s_counted_any = true;
        s_state.bursts++;
        s_state.inverted         = inverted;
        s_state.last_class       = f->class_id;
        s_state.last_sync_errors = f->sync_errors;
        if (!slot) s_state.unknown_slot++;
        else dmr_tracker_burst(&s_state.tracker, slot, f->class_id);
        s_gen++;
    }
    call_expire(now);

    flywheel_t *fly = slot ? &s_fly[inverted ? 1 : 0][slot - 1] : NULL;
    /* Voice A contains AMBE payload around sync, not Slot Type or BPTC LC.
       It does start a superframe: B-F follow at one-frame steps. */
    if (voice) {
        if (!mine || !fly) return;
        fly->on = true;
        fly->next_bit = end_bit + FRAME_BITS;
        fly->index = 1;
        /* On the sample path this burst was just re-sliced at its sync. */
        fly->hi = s_last_hi;
        fly->lo = s_last_lo;
        fly->d  = s_last_d;
        dmr_emb_asm_reset(&fly->emb);
        dmr_call_t *call = &s_state.call[slot - 1];
        if (call->active) { call->last_us = now; call->voice_bursts++; }
        return;
    }
    /* Anything with a data sync in a slot ends that slot's superframe. */
    if (fly) fly->on = false;

    uint8_t cc = 0, dt = 0;
    if (dmr_burst_slot_type(f->burst, &cc, &dt) < 0) return;
    if (mine) { s_state.slot_type_ok++; s_state.colour_code = cc; }
    if (dt != DMR_DT_VOICE_LC_HEADER && dt != DMR_DT_TERMINATOR_LC &&
        dt != DMR_DT_CSBK && dt != DMR_DT_DATA_HEADER) return;

    uint8_t coded[DMR_BPTC_BITS / 8 + 1];
    uint8_t data[12];
    dmr_burst_extract_bptc(f->burst, coded);
    if (dmr_bptc_decode(coded, data) < 0) { if (mine) s_state.bptc_fail++; return; }
    if (mine) s_state.bptc_ok++;

    if (dt == DMR_DT_CSBK) {
        dmr_csbk_t c;
        if (!dmr_csbk_decode(data, &c)) return;
        confirm(f, inverted);
        s_state.csbk_ok++;
        s_state.csbk = c;
        s_state.have_csbk = true;
        s_state.csbk_slot = (uint8_t)slot;
        s_state.csbk_us = now;
        s_state.colour_code = cc;
        s_gen++;
        return;
    }
    if (dt == DMR_DT_DATA_HEADER) {
        dmr_data_header_t h;
        if (!dmr_data_header_decode(data, &h)) return;
        confirm(f, inverted);
        s_state.data_hdr_ok++;
        s_state.data_hdr = h;
        s_state.have_data_hdr = true;
        s_state.data_hdr_slot = (uint8_t)slot;
        s_state.data_hdr_us = now;
        s_state.colour_code = cc;
        s_gen++;
        return;
    }

    dmr_lc_t lc;
    const int r = dmr_lc_correct(data, dt, &lc);
    if (!r) return;
    confirm(f, inverted);
    if (r == 1) s_state.lc_ok++;
    else        s_state.lc_fixed++;
    s_state.lc = lc;
    s_state.have_lc = true;
    s_state.lc_us = now;
    s_state.colour_code = cc;
    s_gen++;
    uint8_t lc9[DMR_LC_BYTES];
    dmr_lc_pack(&lc, lc9);
    apply_lc(slot, lc9, cc, dt == DMR_DT_TERMINATOR_LC, false, r == 2, now);
}

/* --------------------------------------------------------- re-slicing ---
 * On the sample path a burst is not taken from the framer. The framer
 * sees bits sliced with whatever the timing loop and level tracker had at
 * the time - which, for a mobile that keyed up 30 ms ago, is not much -
 * and its 5-error threshold then decides whether the burst exists at all.
 *
 * Instead each symbol's strobe value is correlated against the two sync
 * shapes (base station, mobile; voice and data are the same shape negated),
 * normalised so neither the levels nor the DC matter: the 24 sync symbols
 * are balanced twelve and twelve. A local peak above SYNC_RHO is a
 * candidate. Its sync is 24 outer symbols in a known order, which is enough
 * to measure everything properly: try each half-sample phase around the
 * loop's, keep the one that opens the eye widest across the sync, take the
 * levels from it, and slice the whole 288-bit window again. The candidate
 * is a burst only if the re-sliced sync then matches within 5 of 48 bits,
 * which is the framer's own test - so this finds more, not looser. */

#define SYNC_RHO 0.6f                    /* random data: sd 0.2           */


/* The 288-bit window on a straight grid: window symbol s at `base` + s*T
   samples from `anchor`, cut at the levels given. */
static void slice_window(uint8_t w[DMR_BURST_BITS / 8 + 3], uint32_t anchor,
                         float base, float hi_level, float lo_level, bool inverted)
{
    const float T = s_sync.nominal;
    const float c = 0.5f * (hi_level + lo_level), a = 0.5f * (hi_level - lo_level);
    memset(w, 0, DMR_BURST_BITS / 8 + 3);
    for (unsigned s = 0; s < WINDOW_SYMS; s++) {
        const float v = dmr_symsync_sample(&s_sync, anchor, base + (float)s * T);
        int hi, lo;
        symbol_bits(v, c, c + a * (2.0f / 3.0f), c - a * (2.0f / 3.0f), &hi, &lo);
        if (inverted) hi = !hi;
        if (hi) w[(2 * s) >> 3]     |= (uint8_t)(0x80u >> ((2 * s) & 7u));
        if (lo) w[(2 * s + 1) >> 3] |= (uint8_t)(0x80u >> ((2 * s + 1) & 7u));
    }
}

/* Voice bursts B-F have no sync to measure against. They are cut on a
   straight line through the loop's strobes across their own window - the
   loop's average is steady even where a single strobe is not - moved by the
   phase correction burst A's sync found, at the levels it measured. Sliced
   on the raw strobes at the tracker's levels instead, a long run of one
   symbol in the other slot (an idle burst's payload is one) narrowed the
   tracker's eye, and the embedded fragments came out wrong on a noiseless
   signal. */
static void reslice_voice(dmr_burst_frame_t *f, bool inverted, uint32_t last,
                          const flywheel_t *fly)
{
    const float T = s_sync.nominal;
    const uint32_t first = last + 1u - WINDOW_SYMS;
    const uint32_t anchor = s_pos[first & (POS_RING - 1u)].at;
    float base = 0.0f;
    for (unsigned j = 0; j < WINDOW_SYMS; j++) {
        const unsigned k = (first + j) & (POS_RING - 1u);
        base += (float)(int32_t)(s_pos[k].at - anchor) + s_pos[k].frac - (float)j * T;
    }
    base *= 1.0f / (float)WINDOW_SYMS;
    uint8_t w[DMR_BURST_BITS / 8 + 3];
    /* The eye's height from A's sync, its centre from the tracker now: the
       pipeline's own DC removal is still moving for the first couple of
       hundred symbols of a carrier, which is two frames. */
    const float dc = 0.5f * (s_sync.hi + s_sync.lo), half = 0.5f * (fly->hi - fly->lo);
    slice_window(w, anchor, base + fly->d, dc + half, dc - half, inverted);
    memcpy(f->cach, w, 3);
    memcpy(f->burst, w + 3, DMR_BURST_BITS / 8);
    f->have_cach   = true;
    f->class_id    = DMR_SYNC_NONE;
    f->sync_errors = DMR_SYNC_BITS;
}

static bool reslice(dmr_burst_frame_t *f, dmr_sync_class_t cls, bool inverted,
                    uint32_t last, bool feedback)
{
    const uint8_t *pat = dmr_sync_pattern(cls);
    if (!pat || last + 1u < WINDOW_SYMS) return false;
    const uint32_t first = last + 1u - WINDOW_SYMS;
    const unsigned sync_sym = 12u + DMR_SYNC_OFFSET_BITS / 2u;

    /* A straight symbol grid through the sync, not the loop's strobes: a
       loop still pulling in from noise wanders by most of a symbol across
       the payload ahead of the sync, where 100 ppm of clock error moves a
       straight line by a seventieth of one over the whole window. The grid
       sits where the loop's sync strobes are on average. */
    const float T = s_sync.nominal;
    const uint32_t k0 = first + sync_sym;
    const uint32_t anchor = s_pos[k0 & (POS_RING - 1u)].at;
    float base = 0.0f;
    for (unsigned j = 0; j < 24; j++) {
        const unsigned k = (k0 + j) & (POS_RING - 1u);
        base += (float)(int32_t)(s_pos[k].at - anchor) + s_pos[k].frac - (float)j * T;
    }
    base *= 1.0f / 24.0f;                     /* sync symbol 0, from anchor */

    float best = 0.0f, best_d = 0.0f, best_hi = 0.0f, best_lo = 0.0f;
    bool found = false;
    for (int h = -10; h <= 10; h++) {
        const float d = 0.5f * (float)h;
        float p = 0.0f, m = 0.0f;
        unsigned np = 0, nm = 0;
        for (unsigned j = 0; j < 24; j++) {
            const unsigned b = j * 2u;
            /* Sync is outer symbols only, so the first bit is the sign. */
            const bool neg = ((pat[b >> 3] >> (7u - (b & 7u))) & 1u) != (inverted ? 1u : 0u);
            const float v = dmr_symsync_sample(&s_sync, anchor, base + (float)j * T + d);
            if (neg) { m += v; nm++; } else { p += v; np++; }
        }
        if (!np || !nm) return false;
        const float open = p / (float)np - m / (float)nm;
        if (!found || open > best) {
            best = open; best_d = d; best_hi = p / (float)np; best_lo = m / (float)nm;
            found = true;
        }
    }
    if (!found || best <= 0.0f) return false;

    uint8_t w[DMR_BURST_BITS / 8 + 3];
    slice_window(w, anchor, base - (float)sync_sym * T + best_d, best_hi, best_lo, inverted);
    uint8_t sync[DMR_SYNC_BITS / 8];
    memset(sync, 0, sizeof(sync));
    for (unsigned i = 0; i < DMR_SYNC_BITS; i++) {
        const unsigned b = 24u + DMR_SYNC_OFFSET_BITS + i;
        if ((w[b >> 3] >> (7u - (b & 7u))) & 1u) sync[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
    }
    const dmr_sync_match_t m = dmr_sync_detect(sync, 5);
    if (m.class_id != cls) return false;

    memcpy(f->cach, w, 3);
    memcpy(f->burst, w + 3, DMR_BURST_BITS / 8);
    f->have_cach   = true;
    f->class_id    = cls;
    f->sync_errors = m.errors;
    /* Hand the loop what the sync measured - all of the levels, half of the
       phase, so one burst's noise cannot throw the clock around. Both
       readings find the same burst; it is fed back once. */
    if (feedback) {
        /* For B-F: where this window's grid sits against the loop's average
           over the same window, which is what they will be measured from. */
        float avg = 0.0f;
        for (unsigned j = 0; j < WINDOW_SYMS; j++) {
            const unsigned k = (first + j) & (POS_RING - 1u);
            avg += (float)(int32_t)(s_pos[k].at - anchor) + s_pos[k].frac - (float)j * T;
        }
        avg *= 1.0f / (float)WINDOW_SYMS;
        s_last_d = (base - (float)sync_sym * T + best_d) - avg;
        s_last_hi = best_hi;
        s_last_lo = best_lo;
        const unsigned kl = last & (POS_RING - 1u);
        const float grid = base + ((float)(WINDOW_SYMS - 1u) - (float)sync_sym) * T + best_d;
        const float loop = (float)(int32_t)(s_pos[kl].at - anchor) + s_pos[kl].frac;
        dmr_symsync_set_levels(&s_sync, best_hi, best_lo);
        dmr_symsync_shift(&s_sync, 0.5f * (grid - loop));
    }
    return true;
}

/* Normalised correlation of the sync slot of the window ending at symbol
   `last` against a sync shape, +1 for +3. */
static float sync_rho(const uint8_t *pat, uint32_t last)
{
    const uint32_t from = last - 77u;            /* 54 symbols after the sync */
    float sum = 0.0f, mean = 0.0f, energy = 0.0f;
    for (unsigned j = 0; j < 24; j++) mean += s_val[(from + j) & (POS_RING - 1u)];
    mean *= 1.0f / 24.0f;
    for (unsigned j = 0; j < 24; j++) {
        const float v = s_val[(from + j) & (POS_RING - 1u)];
        const unsigned b = j * 2u;
        sum += ((pat[b >> 3] >> (7u - (b & 7u))) & 1u) ? -v : v;
        energy += (v - mean) * (v - mean);
    }
    return energy > 0.0f ? sum / sqrtf(24.0f * energy) : 0.0f;
}

static void soft_sync(void)
{
    if (s_sym_n < WINDOW_SYMS + 2u) return;
    static const dmr_sync_class_t shape_voice[2] = { DMR_SYNC_BS_VOICE, DMR_SYNC_MS_VOICE };
    static const dmr_sync_class_t shape_data[2]  = { DMR_SYNC_BS_DATA,  DMR_SYNC_MS_DATA  };
    const uint32_t newest = s_sym_n - 1u;
    for (unsigned sh = 0; sh < 2; sh++) {
        const float rho = sync_rho(dmr_sync_pattern(shape_voice[sh]), newest);
        const float prev = s_rho[sh][0], before = s_rho[sh][1];
        s_rho[sh][1] = prev;
        s_rho[sh][0] = rho;
        if (fabsf(prev) < SYNC_RHO || fabsf(prev) < fabsf(before) ||
            fabsf(prev) <= fabsf(rho)) continue;
        /* The window ending one symbol back. Positive is voice the right
           way up or data upside down; negative the other two. */
        const uint32_t last = newest - 1u;
        const uint32_t end_bit = 2u * (last + 1u);
        bool fed_back = false;
        for (unsigned inv = 0; inv < 2; inv++) {
            const dmr_sync_class_t cls = (prev > 0.0f) == (inv == 0)
                                       ? shape_voice[sh] : shape_data[sh];
            dmr_burst_frame_t f;
            if (!reslice(&f, cls, inv != 0, last, !fed_back)) continue;
            if (!fed_back) s_state.refined++;
            fed_back = true;
            on_burst(&f, inv != 0, end_bit);
        }
    }
}

/* ------------------------------------------------------------ feeding --- */

static void push_bit(int bit_normal, int bit_inverted, bool from_framer)
{
    dmr_burst_frame_t frame;
    s_bits++;
    for (unsigned p = 0; p < 2; p++) {
        const int bit = p ? bit_inverted : bit_normal;
        /* The framers run on both paths - the flywheel reads voice bursts
           B-F out of them - but only report bursts on the symbol path. */
        if (dmr_framer_bit(&s_framer[p], bit, &frame) && from_framer)
            on_burst(&frame, p != 0, s_bits);
        for (unsigned slot = 0; slot < 2; slot++) {
            flywheel_t *fly = &s_fly[p][slot];
            if (!fly->on || fly->next_bit != s_bits) continue;
            if (!from_framer) {
                reslice_voice(&frame, p != 0, s_sym_n - 1u, fly);
                on_voice_embedded(&frame, p != 0, slot + 1, fly, now_us());
            } else if (dmr_framer_peek(&s_framer[p], &frame)) {
                on_voice_embedded(&frame, p != 0, slot + 1, fly, now_us());
            }
            if (++fly->index > 5) fly->on = false;
            else fly->next_bit += FRAME_BITS;
        }
    }
}

static void publish(void)
{
    if (s_gen == s_pub_gen) return;
    s_pub_gen = s_gen;
    LOCK();
    s_published = s_state;
    UNLOCK();
}

static void feed(float symbol, float center, float umid, float lmid, bool from_framer)
{
    const unsigned n = atomic_fetch_add(&s_symbols, 1) + 1;

    int hi, lo;
    symbol_bits(symbol, center, umid, lmid, &hi, &lo);
    /* A negated symbol keeps its magnitude and flips its sign, which in the
       ETSI mapping is the first bit only. */
    push_bit(hi, !hi, from_framer);
    push_bit(lo, lo, from_framer);

    if ((n % 480u) == 0) call_expire(now_us());
}

void dmr_watch_symbol(int symbol, int center, int umid, int lmid)
{
    if (!s_started) { dmr_watch_reset(); s_started = true; }
    if (atomic_load(&s_samples_fed)) return;
    feed((float)symbol, (float)center, (float)umid, (float)lmid, true);
    publish();
}

void dmr_watch_samples(const int16_t *samples, int count)
{
    if (!samples || count <= 0) return;
    if (!s_started) { dmr_watch_reset(); s_started = true; }
    if (!atomic_load(&s_samples_fed)) {
        atomic_store(&s_samples_fed, true);
        s_state.from_samples = true;
        s_gen++;
    }
    dmr_symbol_t sym;
    for (int i = 0; i < count; i++) {
        if (!dmr_symsync_push(&s_sync, (float)samples[i], &sym)) continue;
        const unsigned k = s_sym_n++ & (POS_RING - 1u);
        s_pos[k].at = sym.at;
        s_pos[k].frac = sym.frac;
        s_val[k] = sym.value;
        feed(sym.value, sym.center, sym.umid, sym.lmid, false);
        soft_sync();
    }
    publish();
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
