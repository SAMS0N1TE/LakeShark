#include "p25_demod_control.h"

#include "p25_qual.h"

static uint32_t counter_delta(uint32_t value, uint32_t baseline)
{
    return value >= baseline ? value - baseline : value;
}

float p25_demod_output_gain(demod_mode_t mode, bool inverted)
{

    float normal = mode == DEMOD_CQPSK ? 9000.0f : -9000.0f;
    return inverted ? -normal : normal;
}

int p25_demod_preference_normalize(int preference)
{
    return preference >= P25_DEMOD_AUTO &&
           preference <= (int)DEMOD_FSK4_TRACKING
               ? preference : P25_DEMOD_AUTO;
}

static void begin_hunt(p25_demod_control_t *c, uint32_t now_ms,
                       uint32_t valid_nids, uint32_t valid_tsbks)
{
    c->active = DEMOD_C4FM;
    c->phase = P25_DEMOD_HUNT_C4FM;
    c->window_start_ms = now_ms;
    c->last_protocol_ms = now_ms;
    c->base_nids = valid_nids;
    c->base_tsbks = valid_tsbks;
    c->observed_nids = valid_nids;
    c->observed_tsbks = valid_tsbks;
    c->c4fm_nids = 0;
    c->c4fm_tsbks = 0;
    c->cqpsk_nids = 0;
    c->cqpsk_tsbks = 0;
    c->c4fm_score = P25_QUAL_NO_SCORE;
    c->cqpsk_score = P25_QUAL_NO_SCORE;
    c->locked = false;
}

void p25_demod_control_init(p25_demod_control_t *c, int preference,
                            uint32_t now_ms, uint32_t valid_nids,
                            uint32_t valid_tsbks)
{
    if (!c) return;
    c->preference = p25_demod_preference_normalize(preference);
    c->reacquire_count = 0;
    c->stream_ms = now_ms;
    c->stream_fraction = 0;
    c->stream_rate_hz = 0;
    if (c->preference == P25_DEMOD_AUTO) {
        begin_hunt(c, now_ms, valid_nids, valid_tsbks);
        return;
    }

    c->active = (demod_mode_t)c->preference;
    c->phase = P25_DEMOD_LOCKED;
    c->window_start_ms = now_ms;
    c->last_protocol_ms = now_ms;
    c->base_nids = c->observed_nids = valid_nids;
    c->base_tsbks = c->observed_tsbks = valid_tsbks;
    c->c4fm_nids = c->c4fm_tsbks = 0;
    c->cqpsk_nids = c->cqpsk_tsbks = 0;
    c->c4fm_score = c->cqpsk_score = P25_QUAL_NO_SCORE;
    c->locked = true;
}

bool p25_demod_call_active(int last_p25_type, bool voice_active)
{
    /* DSD values 1/2 are LDU1/LDU2 voice. TSDU (3) is specifically the
     * control-channel protocol traffic acquisition must compare. */
    return voice_active || last_p25_type == 1 || last_p25_type == 2;
}

bool p25_demod_control_tick(p25_demod_control_t *c, uint32_t now_ms,
                            uint32_t valid_nids, uint32_t valid_tsbks,
                            bool call_active)
{
    if (!c || c->preference != P25_DEMOD_AUTO) return false;

    if (valid_nids != c->observed_nids || valid_tsbks != c->observed_tsbks) {
        c->observed_nids = valid_nids;
        c->observed_tsbks = valid_tsbks;
        c->last_protocol_ms = now_ms;
    }

    if (c->phase == P25_DEMOD_LOCKED) {
        if (call_active ||
            (uint32_t)(now_ms - c->last_protocol_ms) < P25_DEMOD_LOCK_LOSS_MS)
            return false;

        c->reacquire_count++;
        begin_hunt(c, now_ms, valid_nids, valid_tsbks);
        return true;
    }

    uint32_t nids = counter_delta(valid_nids, c->base_nids);
    uint32_t tsbks = counter_delta(valid_tsbks, c->base_tsbks);
    int score = p25_qual_protocol_score(nids, tsbks);

    /* eight valid NIDs and two TSBKs still caused a 1500 ms detour
     * through the other demodulator. Retain a protocol-confirmed receiver
     * immediately, including a call beginning during the no-lock retry.
     * This chooses a working mode, not the best score from two trials;
     * sparse evidence still uses comparison and silence still reacquires. */
    if (nids >= 2 || (nids >= 1 && tsbks >= 1)) {
        if (c->active == DEMOD_C4FM) {
            c->c4fm_nids = nids;
            c->c4fm_tsbks = tsbks;
            c->c4fm_score = score;
        } else {
            c->cqpsk_nids = nids;
            c->cqpsk_tsbks = tsbks;
            c->cqpsk_score = score;
        }
        c->phase = P25_DEMOD_LOCKED;
        c->locked = true;
        c->window_start_ms = now_ms;
        return true;
    }

    if (c->phase == P25_DEMOD_NO_LOCK) {
        if ((uint32_t)(now_ms - c->window_start_ms) < P25_DEMOD_RETRY_MS)
            return false;
        begin_hunt(c, now_ms, valid_nids, valid_tsbks);
        return true;
    }

    if ((uint32_t)(now_ms - c->window_start_ms) < P25_DEMOD_AUTO_WINDOW_MS ||
        call_active)
        return false;

    if (c->phase == P25_DEMOD_HUNT_C4FM) {
        c->c4fm_nids = nids;
        c->c4fm_tsbks = tsbks;
        c->c4fm_score = score;
        c->active = DEMOD_CQPSK;
        c->phase = P25_DEMOD_HUNT_CQPSK;
        c->window_start_ms = now_ms;
        c->base_nids = valid_nids;
        c->base_tsbks = valid_tsbks;
        return true;
    }

    c->cqpsk_nids = nids;
    c->cqpsk_tsbks = tsbks;
    c->cqpsk_score = score;
    c->window_start_ms = now_ms;
    if (c->c4fm_score == P25_QUAL_NO_SCORE &&
        c->cqpsk_score == P25_QUAL_NO_SCORE) {
        c->active = DEMOD_C4FM;
        c->phase = P25_DEMOD_NO_LOCK;
        c->locked = false;
        c->base_nids = valid_nids;
        c->base_tsbks = valid_tsbks;
    } else {
        /* Equal protocol results deliberately prefer C4FM: it is the proven
         * conventional path and avoids labelling a clean C4FM carrier LSM. */
        c->active = c->cqpsk_score > c->c4fm_score
                        ? DEMOD_CQPSK : DEMOD_C4FM;
        c->phase = P25_DEMOD_LOCKED;
        c->locked = true;
        c->last_protocol_ms = now_ms;
        c->observed_nids = valid_nids;
        c->observed_tsbks = valid_tsbks;
    }
    return true;
}

bool p25_demod_control_feed(p25_demod_control_t *c,
                            uint32_t iq_pairs, uint32_t sample_rate_hz,
                            uint32_t valid_nids, uint32_t valid_tsbks,
                            bool call_active)
{
    if (!c || c->preference != P25_DEMOD_AUTO || !iq_pairs || !sample_rate_hz)
        return false;
    if (sample_rate_hz != c->stream_rate_hz) {
        c->stream_fraction = 0;
        c->stream_rate_hz = sample_rate_hz;
    }
    uint64_t ticks = (uint64_t)iq_pairs * 1000U + c->stream_fraction;
    c->stream_ms += (uint32_t)(ticks / sample_rate_hz);
    c->stream_fraction = (uint32_t)(ticks % sample_rate_hz);
    return p25_demod_control_tick(c, c->stream_ms, valid_nids, valid_tsbks,
                                  call_active);
}

const char *p25_demod_mode_name(demod_mode_t mode)
{
    static const char *const names[] = {
        "C4FM", "CQPSK", "DIFF_4FSK", "FSK4_TRACKING"
    };
    return mode >= DEMOD_C4FM && mode <= DEMOD_FSK4_TRACKING
               ? names[(int)mode] : "?";
}

const char *p25_demod_control_name(const p25_demod_control_t *c)
{
    if (!c) return "?";
    if (c->preference != P25_DEMOD_AUTO)
        return p25_demod_mode_name(c->active);
    if (c->phase == P25_DEMOD_NO_LOCK) return "AUTO:NO LOCK";
    if (c->active == DEMOD_CQPSK)
        return c->locked ? "AUTO:CQPSK" : "AUTO:CQPSK?";
    return c->locked ? "AUTO:C4FM" : "AUTO:C4FM?";
}

void p25_demod_dsd_flags(demod_mode_t mode, int *c4fm, int *qpsk, int *gfsk)
{
    (void)mode;
    if (c4fm) *c4fm = 1;
    if (qpsk) *qpsk = 0;
    if (gfsk) *gfsk = 0;
}
