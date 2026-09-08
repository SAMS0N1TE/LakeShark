#include "p25_acquisition.h"
#include "dsd.h"
#include "p25_receive.h"

bool p25_acquisition_prepare(p25_acquisition_gate_t *gate,
    dsd_opts *opts, dsd_state *state, uint32_t generation,
    bool pending, bool failed)
{
    if (gate->generation != generation) {
        noCarrier(opts, state);
        p25_receive_call_reset(state);
        gate->generation = generation;
        gate->resets++;
    }
    if (pending) gate->pending_waits++;
    if (failed) gate->failed_waits++;
    return !pending && !failed;
}

bool p25_acquisition_accept_sync(p25_acquisition_gate_t *gate,
    uint32_t generation, int sync)
{
    if (gate->generation != generation) {
        if (sync >= 0) gate->discarded_syncs++;
        return false;
    }
    if (sync >= 0) gate->accepted_syncs++;
    return true;
}

bool p25_acquisition_accept_frame(p25_acquisition_gate_t *gate,
    uint32_t generation)
{
    if (gate->generation == generation) return true;
    gate->discarded_frames++;
    return false;
}

void p25_acquisition_probe_iq(p25_iq_probe_t *out,
    const uint8_t *iq, size_t bytes)
{
    *out = (p25_iq_probe_t){0};
    if (!iq) return;
    /* One live 16 KiB block; bound work and arithmetic for other callers. */
    if (bytes > 16384) bytes = 16384;
    int32_t sum_i = 0, sum_q = 0;
    uint32_t sum_square = 0;
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        int di = (int)iq[i] - 128;
        int dq = (int)iq[i + 1] - 128;
        sum_i += di;
        sum_q += dq;
        sum_square += (uint32_t)(di * di + dq * dq);
        out->clipped_components += (iq[i] == 0 || iq[i] == 255);
        out->clipped_components += (iq[i + 1] == 0 || iq[i + 1] == 255);
        out->sampled_pairs++;
    }
    if (out->sampled_pairs) {
        out->mean_square = sum_square / (2 * out->sampled_pairs);
        out->mean_i_milli = sum_i * 1000 / (int32_t)out->sampled_pairs;
        out->mean_q_milli = sum_q * 1000 / (int32_t)out->sampled_pairs;
    }
}

size_t p25_acquisition_format(const p25_acquisition_status_t *s,
                              char *out, size_t capacity)
{
    if (!s || !out || !capacity) return 0;
    int n = snprintf(out, capacity,
        "P25 acquisition: decoder_ms=%lu iq_ms=%lu tune_state=%lu\n"
        "gate: generation=%lu resets=%lu pending_waits=%lu failed_waits=%lu "
        "discard_sync=%lu discard_frame=%lu accepted_sync=%lu\n"
        "hunt: attempts=%lu timeouts=%lu symbols=%lu raw_sync=%lu inverted=%lu "
        "best_hd=%lu/%lu range=%ld..%ld\n"
        "iq: pairs=%lu clipped=%lu mean_square=%lu dc_milli=%ld/%ld ring_drops=%lu\n",
        (unsigned long)s->decoder_updated_ms, (unsigned long)s->iq_updated_ms,
        (unsigned long)s->tune_state,
        (unsigned long)s->gate.generation, (unsigned long)s->gate.resets,
        (unsigned long)s->gate.pending_waits, (unsigned long)s->gate.failed_waits,
        (unsigned long)s->gate.discarded_syncs, (unsigned long)s->gate.discarded_frames,
        (unsigned long)s->gate.accepted_syncs,
        (unsigned long)s->hunt.attempts, (unsigned long)s->hunt.timeouts,
        (unsigned long)s->hunt.symbols, (unsigned long)s->hunt.raw_syncs,
        (unsigned long)s->hunt.inverted_matches,
        (unsigned long)s->hunt.best_normal_hd, (unsigned long)s->hunt.best_inverted_hd,
        (long)s->hunt.symbol_min, (long)s->hunt.symbol_max,
        (unsigned long)s->iq.sampled_pairs, (unsigned long)s->iq.clipped_components,
        (unsigned long)s->iq.mean_square, (long)s->iq.mean_i_milli,
        (long)s->iq.mean_q_milli, (unsigned long)s->ring_drops);
    return n < 0 ? 0 : (size_t)n;
}
