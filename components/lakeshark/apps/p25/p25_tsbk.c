#include "p25_tsbk.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const uint8_t trellis_words[4][4] = {
    { 0x2, 0xc, 0x1, 0xf },
    { 0xe, 0x0, 0xd, 0x3 },
    { 0x9, 0x7, 0xa, 0x4 },
    { 0x5, 0xb, 0x6, 0x8 },
};

static unsigned int bit_count4(uint8_t value)
{
    value = (uint8_t)(value - ((value >> 1) & 0x55u));
    value = (uint8_t)((value & 0x33u) + ((value >> 2) & 0x33u));
    return value & 0x0fu;
}

void p25_tsbk_deinterleave(const uint8_t input[P25_TSBK_ENCODED_DIBITS],
                           uint8_t output[P25_TSBK_ENCODED_DIBITS])
{
    size_t wire = 0;

    for (size_t phase = 0; phase < 4; phase++) {
        size_t pairs = phase == 0 ? 13 : 12;
        for (size_t pair = 0; pair < pairs; pair++) {
            size_t decoded = phase * 2 + pair * 8;
            output[decoded] = input[wire++];
            output[decoded + 1] = input[wire++];
        }
    }
}

int p25_tsbk_trellis_decode(
    const uint8_t encoded[P25_TSBK_ENCODED_DIBITS],
    uint8_t decoded[P25_TSBK_DECODED_DIBITS])
{
    enum { STEPS = P25_TSBK_ENCODED_DIBITS / 2, INF = INT_MAX / 4 };
    int metric[4] = { 0, INF, INF, INF };
    uint8_t predecessor[STEPS][4];

    for (size_t step = 0; step < STEPS; step++) {
        int next_metric[4] = { INF, INF, INF, INF };
        uint8_t received;

        if (encoded[step * 2] > 3 || encoded[step * 2 + 1] > 3)
            return -1;
        received = (uint8_t)((encoded[step * 2] << 2)
                             | encoded[step * 2 + 1]);

        for (uint8_t next = 0; next < 4; next++) {
            for (uint8_t previous = 0; previous < 4; previous++) {
                int candidate;
                if (metric[previous] == INF)
                    continue;
                candidate = metric[previous]
                    + (int)bit_count4((uint8_t)(received
                                                ^ trellis_words[previous][next]));
                if (candidate < next_metric[next]) {
                    next_metric[next] = candidate;
                    predecessor[step][next] = previous;
                }
            }
        }
        memcpy(metric, next_metric, sizeof(metric));
    }

    /* The 49th dibit is the zero tail symbol, so a valid encoder terminates
     * in state zero. CRC provides the final rejection for an ambiguous path. */
    uint8_t state = 0;
    for (size_t step = STEPS; step-- > 0;) {
        if (step < P25_TSBK_DECODED_DIBITS)
            decoded[step] = state;
        state = predecessor[step][state];
    }
    return metric[0];
}

uint16_t p25_tsbk_crc16(const uint8_t *data, size_t length)
{
    uint32_t crc = 0;

    /* OP25's field decoder uses this augmented CRC-16/CCITT form for
     * TSBKs: poly 0x1021, init 0, MSB-first, no reflection, xorout 0xffff.
     * It checks all 12 decoded bytes for a zero result; this intentionally is
     * not the byte-wise XMODEM or CCITT-FALSE recurrence. See boatbod/op25,
     * op25/gr-op25_repeater/lib/p25p1_fdma.cc, crc16(). */
    if (data == NULL)
        return UINT16_MAX;
    for (size_t i = 0; i < length; i++) {
        for (unsigned int bit_index = 0; bit_index < 8; bit_index++) {
            uint32_t bit = (data[i] >> (7 - bit_index)) & 1u;
            crc = ((crc << 1) | bit) & 0x1ffffu;
            if (crc & 0x10000u)
                crc = (crc & 0xffffu) ^ 0x1021u;
        }
    }
    return (uint16_t)(crc ^ 0xffffu);
}

static uint32_t get_bits(const uint8_t block[P25_TSBK_BYTES],
                         unsigned int first, unsigned int count)
{
    uint32_t value = 0;

    for (unsigned int i = 0; i < count; i++) {
        unsigned int bit = first + i;
        value = (value << 1) | ((block[bit / 8] >> (7 - bit % 8)) & 1u);
    }
    return value;
}

typedef struct {
    uint64_t frequency_hz;
    uint8_t slot;
    uint8_t slots_per_carrier;
} p25_channel_resolution_t;

static p25_channel_resolution_t channel_resolve(const dsd_state *state,
                                                uint16_t channel)
{

    p25_channel_resolution_t result = { 0 };
    const p25_iden_entry_t *entry = &state->p25_iden_table[channel >> 12];
    if (!entry->valid || !entry->base_hz || !entry->spacing_hz ||
        entry->base_hz > UINT32_MAX ||
        (entry->slots_per_carrier != 0 && entry->slots_per_carrier != 1 &&
         entry->slots_per_carrier != 2 && entry->slots_per_carrier != 4))
        return result;

    uint16_t position = channel & 0x0fffu;
    result.slots_per_carrier = entry->slots_per_carrier;
    if (entry->slots_per_carrier != 0) {
        result.slot = (uint8_t)(position % entry->slots_per_carrier);
        position = (uint16_t)(position / entry->slots_per_carrier);
    }
    result.frequency_hz = entry->base_hz + (uint64_t)position * entry->spacing_hz;
    if (result.frequency_hz > UINT32_MAX)
        result.frequency_hz = 0;
    return result;
}

static uint64_t channel_frequency(const dsd_state *state, uint16_t channel)
{
    return channel_resolve(state, channel).frequency_hz;
}

static uint32_t p25_iden_vu_bandwidth_hz(uint8_t bwvu_code)
{
    switch (bwvu_code) {
    case 4: return 6250u;
    case 5: return 12500u;
    default: return 0u;
    }
}

typedef struct {
    uint8_t valid;
    uint8_t is_tdma;
    uint8_t slots_per_carrier;
    uint32_t channel_bw_hz;
} p25_tdma_channel_type_t;

/* TIA-102.AABC-D table 2.3.40-1 defines access, bandwidth, slot count and
 * vocoder for channel types 0-5; 6-15 are reserved. Type 4's four-slot form
 * is retained so its channel arithmetic and unsupported-voice reporting are
 * still truthful. FDMA entries normalise slots_per_carrier to zero in the
 * IDEN table, preserving the existing channel-number arithmetic. */
static const p25_tdma_channel_type_t p25_tdma_channel_types[16] = {
    [0] = { 1, 0, 1, 12500u },
    [1] = { 1, 0, 1, 12500u },
    [2] = { 1, 0, 1,  6250u },
    [3] = { 1, 1, 2, 12500u },
    [4] = { 1, 1, 4, 25000u },
    [5] = { 1, 1, 2, 12500u },
};

/* single write path for voice grant fields. Every voice-grant
 * opcode (0x00 GRP_V_CH_GRANT, 0x02 GRP_V_CH_GRANT_UPDATE, 0x03
 * GRP_V_CH_GRANT_UPDT_EXP) funnels through here; data grants (0x10, 0x14),
 * telephone interconnect (0x08, 0x09), unit-to-unit (0x04, 0x06), and
 * every status broadcast never touch these fields, so a grant follower
 * that keys on p25_tsbk_last_opcode + p25_tsbk_frequency_hz cannot chase
 * a data or reg broadcast even if the whitelist grows a bug later. */
static void set_voice_grant(dsd_state *state, uint16_t channel,
                            uint16_t talkgroup, uint32_t source)
{
    p25_channel_resolution_t resolved = channel_resolve(state, channel);
    state->p25_tsbk_channel      = channel;
    state->p25_tsbk_talkgroup    = talkgroup;
    state->p25_tsbk_source       = source;

    if (resolved.slots_per_carrier > 1 && resolved.frequency_hz && talkgroup) {
        state->p25_phase2_grant_count++;
        state->p25_phase2_last_talkgroup = talkgroup;
        state->p25_phase2_last_frequency_hz = resolved.frequency_hz;
        state->p25_phase2_last_slot = resolved.slot;
        state->p25_phase2_last_slots_per_carrier =
            resolved.slots_per_carrier;
        state->p25_tsbk_frequency_hz = 0;
    } else {
        state->p25_tsbk_frequency_hz =
            state->p25_iden_table[channel >> 12].voice_unsupported || !talkgroup
                ? 0 : resolved.frequency_hz;
    }
    state->lasttg  = talkgroup;
    state->lastsrc = (int)source;
}

static void queue_voice_grant(dsd_state *state, uint16_t channel,
                              uint16_t talkgroup, uint32_t source,
                              uint8_t service_options)
{
    if (state->p25_grant_count >= P25_GRANTS_PER_TSDU) return;
    p25_call_info_t *g = &state->p25_grants[state->p25_grant_count++];
    memset(g, 0, sizeof(*g));
    const p25_iden_entry_t *iden = &state->p25_iden_table[channel >> 12];
    p25_channel_resolution_t r = channel_resolve(state, channel);
    g->carrier_hz = r.frequency_hz;
    g->channel = channel;
    g->talkgroup = talkgroup;
    g->source = source;
    g->slot = r.slot;
    g->slots_per_carrier = r.slots_per_carrier ? r.slots_per_carrier : 1;
    g->channel_type = iden->channel_type;
    g->service_options = service_options;
    g->wacn = state->p25_tsbk_wacn;
    g->sysid = state->p25_tsbk_sysid;
    g->system_valid = state->p25_net_valid;
    g->nac = (uint16_t)state->nac;
    g->support = !talkgroup || channel == 0xffff ? P25_CALL_INVALID :
        !iden->valid ? P25_CALL_MISSING_IDEN :
        !r.frequency_hz ? P25_CALL_INVALID :
        iden->voice_unsupported || r.slots_per_carrier > 1
            ? P25_CALL_UNSUPPORTED : P25_CALL_PHASE1;
}

/* overwrite an ADJ_STS_BCST slot keyed by (rfss_id, site_id) so
 * the neighbour list does not grow every time a site re-announces itself.
 * A brand new site takes the first empty slot; the table is small (16)
 * because a scanner rarely sees more distinct sites than that. */
static void neighbor_upsert(dsd_state *state, uint8_t rfss, uint8_t site,
                            uint8_t lra, uint16_t sysid, uint16_t channel,
                            uint8_t svc_class)
{
    int slot = -1;
    int first_free = -1;
    for (int i = 0; i < P25_NEIGHBOR_TABLE_SIZE; i++) {
        if (state->p25_neighbors[i].valid) {
            if (state->p25_neighbors[i].rfss_id == rfss &&
                state->p25_neighbors[i].site_id == site) {
                slot = i;
                break;
            }
        } else if (first_free < 0) {
            first_free = i;
        }
    }
    if (slot < 0) slot = first_free;
    if (slot < 0) return;
    p25_neighbor_entry_t *n = &state->p25_neighbors[slot];
    int was_valid = n->valid;
    n->valid         = 1;
    n->rfss_id       = rfss;
    n->site_id       = site;
    n->lra           = lra;
    n->sysid         = sysid;
    n->channel       = channel;
    n->service_class = svc_class;
    n->freq_hz       = channel_frequency(state, channel);
    if (!was_valid && state->p25_neighbor_count < P25_NEIGHBOR_TABLE_SIZE)
        state->p25_neighbor_count++;
    state->p25_neighbor_generation++;
}

static int parse_block(dsd_state *state,
                   const uint8_t block[P25_TSBK_BYTES])
{
    uint8_t opcode;
    uint8_t mfid;

    if (state == NULL || block == NULL || p25_tsbk_crc16(block, P25_TSBK_BYTES) != 0)
        return 0;

    opcode = block[0] & 0x3fu;
    mfid = block[1];

    /* MFID gates the standard TSBK decode. */

    if (mfid != 0x00 && mfid != 0x01) {
        state->p25_tsbk_vendor_count++;
        state->p25_tsbk_last_vendor_mfid = mfid;
        return 0;
    }
    /* protected payload cannot be interpreted as plaintext grants. */
    if (block[0] & 0x40u) return 0;

    switch (opcode) {
    case 0x33: {

        uint8_t identifier = (uint8_t)get_bits(block, 16, 4);
        uint8_t channel_type = (uint8_t)get_bits(block, 20, 4);
        uint32_t sign = get_bits(block, 24, 1);
        uint32_t mag = get_bits(block, 25, 13);
        uint32_t spacing_hz = get_bits(block, 38, 10) * 125u;
        const p25_tdma_channel_type_t *type =
            &p25_tdma_channel_types[channel_type];
        p25_iden_entry_t *entry = &state->p25_iden_table[identifier];
        int64_t offset_hz = (int64_t)mag * spacing_hz;
        if (sign == 0)
            offset_hz = -offset_hz;

        memset(entry, 0, sizeof(*entry));
        entry->channel_type = channel_type;
        entry->slots_per_carrier = type->is_tdma
                                       ? type->slots_per_carrier : 0;
        entry->channel_bw_hz = type->channel_bw_hz;
        entry->spacing_hz = spacing_hz;
        entry->base_hz = (uint64_t)get_bits(block, 48, 32) * 5u;
        entry->tx_offset_hz = (int32_t)offset_hz;
        entry->voice_unsupported = channel_type != 1;
        entry->valid = type->valid && spacing_hz && entry->base_hz &&
                       entry->base_hz <= UINT32_MAX;
        break;
    }
    case 0x34: {
        /* IDEN_UP_VU, the VHF/UHF form. */

        uint8_t identifier = (uint8_t)get_bits(block, 16, 4);
        p25_iden_entry_t *entry = &state->p25_iden_table[identifier];
        uint8_t bwvu = (uint8_t)get_bits(block, 20, 4);
        uint32_t sign = get_bits(block, 24, 1);
        uint32_t mag = get_bits(block, 25, 13);
        int32_t offset_hz = (int32_t)(mag * 250000u);
        if (sign == 0)
            offset_hz = -offset_hz;
        entry->tx_offset_hz = offset_hz;
        entry->channel_bw_hz = p25_iden_vu_bandwidth_hz(bwvu);
        entry->spacing_hz = get_bits(block, 38, 10) * 125u;
        entry->base_hz = (uint64_t)get_bits(block, 48, 32) * 5u;
        entry->slots_per_carrier = 0;
        entry->channel_type = 0;
        entry->voice_unsupported = 0;
        entry->valid = 1;
        break;
    }
    case 0x3d: {
        /* IDEN_UP, the 700/800 MHz form. Field layout:
         *   bits 16-19  Identifier      (4 bits)
         *   bits 20-28  BW              (9 bits, units of 125 Hz)
         *   bit  29     TxOffset sign   (0 = negative, 1 = positive)
         *   bits 30-37  TxOffset value  (8 bits, units of 250 kHz)
         *   bits 38-47  Channel spacing (10 bits, units of 125 Hz)
         *   bits 48-79  Base frequency  (32 bits, units of 5 Hz) */
        uint8_t identifier = (uint8_t)get_bits(block, 16, 4);
        p25_iden_entry_t *entry = &state->p25_iden_table[identifier];
        uint32_t sign = get_bits(block, 29, 1);
        uint32_t mag = get_bits(block, 30, 8);
        int32_t offset_hz = (int32_t)(mag * 250000u);
        if (sign == 0)
            offset_hz = -offset_hz;
        entry->tx_offset_hz = offset_hz;
        entry->channel_bw_hz = get_bits(block, 20, 9) * 125u;
        entry->spacing_hz = get_bits(block, 38, 10) * 125u;
        entry->base_hz = (uint64_t)get_bits(block, 48, 32) * 5u;
        entry->slots_per_carrier = 0;
        entry->channel_type = 0;
        entry->voice_unsupported = 0;
        entry->valid = 1;
        break;
    }
    case 0x00: {
        /* GRP_V_CH_GRANT (standard voice grant). Field layout:
         *   bits 16-23  Service options (ignored here; emergency etc.)
         *   bits 24-39  Channel number
         *   bits 40-55  Talkgroup
         *   bits 56-79  Source address */
        uint16_t channel = (uint16_t)get_bits(block, 24, 16);
        uint16_t tg      = (uint16_t)get_bits(block, 40, 16);
        uint32_t src     = get_bits(block, 56, 24);
        set_voice_grant(state, channel, tg, src);
        queue_voice_grant(state, channel, tg, src, block[2]);
        break;
    }
    case 0x02: {
        /* Standard GRP_V_CH_GRANT_UPDATE carries two channel/group pairs and
         * no source address. Retain the first pair as the latest grant.
         *   bits 16-31  Channel A
         *   bits 32-47  Talkgroup A
         *   bits 48-63  Channel B (ignored)
         *   bits 64-79  Talkgroup B (ignored) */
        uint16_t channel = (uint16_t)get_bits(block, 16, 16);
        uint16_t tg      = (uint16_t)get_bits(block, 32, 16);
        set_voice_grant(state, channel, tg, 0);
        queue_voice_grant(state, channel, tg, 0, 0);
        uint16_t channel_b = (uint16_t)get_bits(block, 48, 16);
        uint16_t tg_b = (uint16_t)get_bits(block, 64, 16);
        if (tg_b && (channel_b != channel || tg_b != tg))
            queue_voice_grant(state, channel_b, tg_b, 0, 0);
        break;
    }
    case 0x03: {
        /* GRP_V_CH_GRANT_UPDT_EXP - explicit form carrying separate transmit and receive channels. */

        uint16_t channel_t = (uint16_t)get_bits(block, 24, 16);
        uint16_t tg        = (uint16_t)get_bits(block, 56, 16);
        set_voice_grant(state, channel_t, tg, 0);
        queue_voice_grant(state, channel_t, tg, 0, block[2]);
        break;
    }
    case 0x04: {
        /* UU_V_CH_GRANT - unit-to-unit voice grant. Private call;
         * no talkgroup, so this cannot feed the group-oriented follower.
         *   bits 16-31  Channel
         *   bits 32-55  Target address (24 bits)
         *   bits 56-79  Source address (24 bits)
         * Store the identifying fields so a UI can label a private call
         * happening on the site; the follower whitelist stays voice-group
         * only, so this cannot retune. */
        uint16_t channel = (uint16_t)get_bits(block, 16, 16);
        uint32_t target  = get_bits(block, 32, 24);
        uint32_t source  = get_bits(block, 56, 24);
        state->p25_last_uu_channel = channel;
        state->p25_last_uu_target  = target;
        state->p25_last_uu_source  = source;
        state->p25_uu_grant_count++;
        break;
    }
    case 0x05: {
        /* UU_ANS_REQ - unit-to-unit answer request. Records the
         * two parties involved.
         *   bit  16     AIV / additional-info flag
         *   bits 17-23  Reserved
         *   bits 24-31  Service type / reserved
         *   bits 32-55  Target address (24 bits)
         *   bits 56-79  Source address (24 bits) */
        uint32_t target = get_bits(block, 32, 24);
        uint32_t source = get_bits(block, 56, 24);
        state->p25_last_uu_target = target;
        state->p25_last_uu_source = source;
        state->p25_uu_grant_count++;
        break;
    }
    case 0x06: {
        /* UU_V_CH_GRANT_UPDT - update for an in-progress unit-to-unit
         * call. Same layout as 0x04. */
        uint16_t channel = (uint16_t)get_bits(block, 16, 16);
        uint32_t target  = get_bits(block, 32, 24);
        uint32_t source  = get_bits(block, 56, 24);
        state->p25_last_uu_channel = channel;
        state->p25_last_uu_target  = target;
        state->p25_last_uu_source  = source;
        state->p25_uu_grant_count++;
        break;
    }
    case 0x08: {

        uint16_t channel = (uint16_t)get_bits(block, 24, 16);
        uint32_t addr    = get_bits(block, 56, 24);
        state->p25_last_interconnect_channel = channel;
        state->p25_last_interconnect_address = addr;
        state->p25_interconnect_grant_count++;
        break;
    }
    case 0x09: {
        /* TELE_INT_CH_GRANT_UPDT - update carrying the current channel and call timer. */

        uint16_t channel = (uint16_t)get_bits(block, 16, 16);
        uint32_t addr    = get_bits(block, 32, 24);
        state->p25_last_interconnect_channel = channel;
        state->p25_last_interconnect_address = addr;
        state->p25_interconnect_grant_count++;
        break;
    }
    case 0x10: {
        /* GRP_D_CH_GRANT - group data channel grant. Recognise so
         * the counter records that data is happening on this site, do not
         * touch voice-grant fields so the follower cannot chase it.
         *   bits 16-23  Data service options
         *   bits 24-39  Channel T (downlink)
         *   bits 40-55  Channel R (uplink)
         *   bits 56-71  Group */
        state->p25_last_data_channel = (uint16_t)get_bits(block, 24, 16);
        state->p25_last_data_group   = (uint16_t)get_bits(block, 56, 16);
        state->p25_data_grant_count++;
        break;
    }
    case 0x14: {
        /* SNDCP_CH_GRANT - SNDCP (packet data) channel grant.
         *   bits 16-23  DSCC / service options
         *   bits 24-39  Channel T
         *   bits 40-55  Channel R
         *   bits 56-79  Target address (24 bits) */
        state->p25_last_data_channel = (uint16_t)get_bits(block, 24, 16);
        state->p25_data_grant_count++;
        break;
    }
    case 0x20: {
        /* ACK_RSP_FNE - FNE acknowledgement to a subscriber request.
         *   bit  16     AIV additional-info flag
         *   bit  17     Extended addr flag
         *   bits 18-23  Reserved
         *   bits 24-31  Service type being acked
         *   bits 32-55  Source (subscriber) address (24 bits)
         *   bits 56-79  Target (group / unit) address (24 bits) */
        state->p25_last_reg_opcode = 0x20;
        state->p25_last_reg_reason = (uint8_t)get_bits(block, 24, 8);
        state->p25_last_reg_source = get_bits(block, 32, 24);
        state->p25_last_reg_target = get_bits(block, 56, 24);
        state->p25_reg_event_count++;
        break;
    }
    case 0x27: {
        /* DENY_RSP - the FNE denied a request. */

        state->p25_last_reg_opcode = 0x27;
        state->p25_last_reg_reason = (uint8_t)get_bits(block, 24, 8);
        state->p25_last_reg_target = get_bits(block, 40, 16);
        state->p25_last_reg_source = get_bits(block, 56, 24);
        state->p25_reg_event_count++;
        break;
    }
    case 0x28: {
        /* GRP_AFF_RSP - group affiliation response. Which unit just
         * affiliated to which group, and the FNE's verdict.
         *   bit  16     Local/global affiliation
         *   bits 17-23  Response value (accept / deny / etc.)
         *   bits 24-39  Announcement group
         *   bits 40-55  Group
         *   bits 56-79  Target (subscriber) address (24 bits) */
        state->p25_last_reg_opcode = 0x28;
        state->p25_last_reg_reason = (uint8_t)get_bits(block, 17, 7);
        state->p25_last_reg_target = get_bits(block, 40, 16);
        state->p25_last_reg_source = get_bits(block, 56, 24);
        state->p25_reg_event_count++;
        break;
    }
    case 0x2c: {
        /* UNIT_REG_RSP - unit registration response.
         *   bits 16-21  Reserved
         *   bits 22-23  Response value (accept / fail / deny / refused)
         *   bits 24-35  System ID (partial)
         *   bits 36-55  Source ID (20 bits, cross-system form)
         *   bits 56-79  Target unit address (24 bits) */
        state->p25_last_reg_opcode = 0x2c;
        state->p25_last_reg_reason = (uint8_t)get_bits(block, 22, 2);
        state->p25_last_reg_source = get_bits(block, 36, 20);
        state->p25_last_reg_target = get_bits(block, 56, 24);
        state->p25_reg_event_count++;
        break;
    }
    case 0x2f: {
        /* U_DE_REG_ACK - unit de-registration acknowledge. Symmetric
         * to unit registration.
         *   bits 16-19  Reserved
         *   bits 20-23  Response value
         *   bits 24-35  WACN (partial)
         *   bits 36-47  System ID
         *   bits 48-71  Source unit address (24 bits) */
        state->p25_last_reg_opcode = 0x2f;
        state->p25_last_reg_reason = (uint8_t)get_bits(block, 20, 4);
        state->p25_last_reg_source = get_bits(block, 48, 24);
        state->p25_last_reg_target = 0;
        state->p25_reg_event_count++;
        break;
    }
    case 0x30: {
        /* SYNC_BCST - system clock and microslot count. */

        state->p25_sync_valid = 1;
        state->p25_sync_year       = get_bits(block, 24, 4) + 2000u;
        state->p25_sync_month_day  = (get_bits(block, 28, 4) << 8)
                                   | get_bits(block, 32, 5);
        state->p25_sync_us         = get_bits(block, 47, 17);
        state->p25_sync_microslot  = (uint16_t)get_bits(block, 64, 16);
        break;
    }
    case 0x38: {
        /* SYS_SRV_BCST - the services this system offers, expressed
         * as bit fields the standard defines. Kept as raw uint32s so a UI can
         * decode the bits it cares about without this parser making guesses.
         *   bits 16-23  Reserved
         *   bits 24-47  Services available (24 bits)
         *   bits 48-71  Services supported (24 bits)
         *   bits 72-79  Reserved */
        state->p25_sys_srv_valid = 1;
        state->p25_sys_srv_twv       = (uint8_t)get_bits(block, 16, 8);
        state->p25_sys_srv_available = get_bits(block, 24, 24);
        state->p25_sys_srv_supported = get_bits(block, 48, 24);
        break;
    }
    case 0x39:
    case 0x3e: {
        /* SCCB / SCCB_EXP - secondary control channel broadcast for a site that runs more than one control channel. */

        state->p25_sccb_valid       = 1;
        state->p25_sccb_rfss_id     = (uint8_t)get_bits(block, 24, 8);
        state->p25_sccb_site_id     = (uint8_t)get_bits(block, 32, 8);
        state->p25_sccb_svc_class1  = (uint8_t)get_bits(block, 40, 8);
        state->p25_sccb_ch1         = (uint16_t)get_bits(block, 48, 16);
        state->p25_sccb_svc_class2  = (uint8_t)get_bits(block, 64, 8);
        state->p25_sccb_ch2         = (uint16_t)get_bits(block, 72, 8);
        state->p25_sccb_generation++;
        break;
    }
    case 0x3a: {
        /* RFSS_STS_BCST - which RFSS / site this control channel belongs to. */

        state->p25_rfss_valid   = 1;
        state->p25_rfss_lra     = (uint8_t)get_bits(block, 16, 8);
        state->p25_rfss_sysid   = (uint16_t)get_bits(block, 24, 12);
        state->p25_rfss_id      = (uint8_t)get_bits(block, 36, 8);
        state->p25_rfss_site_id = (uint8_t)get_bits(block, 44, 8);
        state->p25_rfss_ch_t    = (uint16_t)get_bits(block, 52, 12);
        state->p25_rfss_ch_r    = (uint16_t)get_bits(block, 64, 12);
        state->p25_rfss_svc_class = (uint8_t)get_bits(block, 76, 4);
        state->p25_rfss_generation++;
        break;
    }
    case 0x3b: {
        uint32_t new_wacn = get_bits(block, 24, 20);
        uint16_t new_sysid = (uint16_t)get_bits(block, 44, 12);
        /* iden-table staleness. WACN/SYSID identifies the trunked
         * system. Moving between two systems on the same band would leave
         * the old band plan in place and produce confidently wrong
         * frequencies for the new one, so cross a WACN or SYSID boundary
         * by clearing the whole table - individual entries cannot be
         * invalidated selectively until the new system's IDEN_UP arrives.
         * First-time population (both fields still zero) is not a boundary. */
        if (state->p25_net_valid &&
            (state->p25_tsbk_wacn != new_wacn ||
             state->p25_tsbk_sysid != new_sysid)) {
            memset(state->p25_iden_table, 0, sizeof(state->p25_iden_table));
            /* neighbour list belongs to the old system too. */
            memset(state->p25_neighbors, 0, sizeof(state->p25_neighbors));
            state->p25_neighbor_count = 0;
            /* earlier grants in this TSDU belong to the old system. */
            state->p25_grant_count = 0;
            state->p25_tsbk_frequency_hz = 0;
            state->p25_phase2_last_talkgroup = 0;
            state->p25_phase2_last_frequency_hz = 0;
            state->p25_phase2_last_slot = 0;
            state->p25_phase2_last_slots_per_carrier = 0;
            state->p25_rfss_valid = 0;
            state->p25_sccb_valid = 0;
            state->p25_sys_srv_valid = 0;
            state->p25_sync_valid = 0;
        }
        state->p25_tsbk_wacn = new_wacn;
        state->p25_tsbk_sysid = new_sysid;
        state->p25_net_valid = 1;
        state->p25_net_generation++;
        break;
    }
    case 0x3c: {
        /* ADJ_STS_BCST - adjacent site status. */

        uint8_t  lra     = (uint8_t)get_bits(block, 16, 8);
        uint16_t sysid   = (uint16_t)get_bits(block, 28, 12);
        uint8_t  rfss_id = (uint8_t)get_bits(block, 40, 8);
        uint8_t  site_id = (uint8_t)get_bits(block, 48, 8);
        uint16_t channel = (uint16_t)get_bits(block, 56, 16);
        uint8_t  svc     = (uint8_t)get_bits(block, 72, 8);
        neighbor_upsert(state, rfss_id, site_id, lra, sysid, channel, svc);
        break;
    }
    default:

        state->p25_tsbk_unhandled[opcode]++;
        break;
    }

    state->p25_tsbk_last_opcode = opcode;
    return 1;
}

void p25_tsbk_reset_system(dsd_state *s)
{
    if (!s) return;
    memset(s->p25_iden_table, 0, sizeof(s->p25_iden_table));
    memset(s->p25_neighbors, 0, sizeof(s->p25_neighbors));
    s->p25_neighbor_count = 0;
    s->p25_net_valid = s->p25_rfss_valid = s->p25_sccb_valid = 0;
    s->p25_control_nac_valid = 0;
    s->p25_control_nac = 0;
    s->p25_sys_srv_valid = s->p25_sync_valid = 0;
    s->p25_tsbk_wacn = s->p25_tsbk_sysid = 0;
    s->p25_tsbk_channel = s->p25_tsbk_talkgroup = 0;
    s->p25_tsbk_source = 0;
    s->p25_tsbk_frequency_hz = 0;
    s->p25_phase2_last_talkgroup = 0;
    s->p25_phase2_last_frequency_hz = 0;
    s->p25_phase2_last_slot = s->p25_phase2_last_slots_per_carrier = 0;
    s->p25_grant_count = 0;
    s->p25_grant_batch_valid = 1;
    s->p25_grant_generation++;
}

static void begin_grant_batch(dsd_state *state)
{
    state->p25_grant_batch_valid = 1;
    state->p25_grant_count = 0;
    state->p25_grant_generation++;
}

int p25_tsbk_parse(dsd_state *state, const uint8_t block[P25_TSBK_BYTES])
{
    if (!state) return 0;
    /* even a rejected block retires the preceding delivery. Legacy
     * last-seen fields remain telemetry, never a fresh-grant notification. */
    begin_grant_batch(state);
    return parse_block(state, block);
}

static void pack_dibits(const uint8_t dibits[P25_TSBK_DECODED_DIBITS],
                        uint8_t block[P25_TSBK_BYTES])
{
    memset(block, 0, P25_TSBK_BYTES);
    for (size_t i = 0; i < P25_TSBK_DECODED_DIBITS; i++)
        block[i / 4] |= (uint8_t)(dibits[i] << (6 - (i % 4) * 2));
}

static size_t sfappend(char *buf, size_t buf_sz, size_t pos, const char *fmt,
                       ...) __attribute__((format(printf, 4, 5)));
static size_t sfappend(char *buf, size_t buf_sz, size_t pos, const char *fmt,
                       ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = 0;
    if (buf == NULL || buf_sz == 0) {
        n = vsnprintf(NULL, 0, fmt, ap);
    } else if (pos < buf_sz) {
        n = vsnprintf(buf + pos, buf_sz - pos, fmt, ap);
    } else {
        n = vsnprintf(NULL, 0, fmt, ap);
    }
    va_end(ap);
    if (n < 0) return pos;
    return pos + (size_t)n;
}

size_t p25_tsbk_status_format(const dsd_state *state, char *buf, size_t buf_sz)
{
    size_t pos = 0;
    if (state == NULL) {
        if (buf && buf_sz) buf[0] = 0;
        return 0;
    }
    pos = sfappend(buf, buf_sz, pos,
                   "tsbk ok=%u crc_err=%u trellis_err=%u vendor=%u (last mfid $%02x)\n",
                   state->p25_tsbk_valid_count,
                   state->p25_tsbk_crc_errors,
                   state->p25_tsbk_trellis_errors,
                   state->p25_tsbk_vendor_count,
                   (unsigned)state->p25_tsbk_last_vendor_mfid);
    pos = sfappend(buf, buf_sz, pos, "wacn=$%05lx sysid=$%03x\n",
                   (unsigned long)state->p25_tsbk_wacn,
                   (unsigned)state->p25_tsbk_sysid);
    if (state->p25_rfss_valid) {
        pos = sfappend(buf, buf_sz, pos,
                       "rfss=%u site=%u lra=$%02x ch_t=$%04x ch_r=$%04x svc=$%x\n",
                       (unsigned)state->p25_rfss_id,
                       (unsigned)state->p25_rfss_site_id,
                       (unsigned)state->p25_rfss_lra,
                       (unsigned)state->p25_rfss_ch_t,
                       (unsigned)state->p25_rfss_ch_r,
                       (unsigned)state->p25_rfss_svc_class);
    }
    if (state->p25_sccb_valid) {
        pos = sfappend(buf, buf_sz, pos,
                       "sccb rfss=%u site=%u ch1=$%04x ch2=$%04x\n",
                       (unsigned)state->p25_sccb_rfss_id,
                       (unsigned)state->p25_sccb_site_id,
                       (unsigned)state->p25_sccb_ch1,
                       (unsigned)state->p25_sccb_ch2);
    }
    if (state->p25_sys_srv_valid) {
        pos = sfappend(buf, buf_sz, pos,
                       "sys_srv avail=$%06lx sup=$%06lx twv=$%02x\n",
                       (unsigned long)state->p25_sys_srv_available,
                       (unsigned long)state->p25_sys_srv_supported,
                       (unsigned)state->p25_sys_srv_twv);
    }
    if (state->p25_sync_valid) {
        pos = sfappend(buf, buf_sz, pos,
                       "sync %04u-%02u-%02u us=%lu microslot=%u\n",
                       (unsigned)state->p25_sync_year,
                       (unsigned)((state->p25_sync_month_day >> 8) & 0x0f),
                       (unsigned)(state->p25_sync_month_day & 0x1f),
                       (unsigned long)state->p25_sync_us,
                       (unsigned)state->p25_sync_microslot);
    }
    int iden_valid = 0;
    for (int i = 0; i < P25_IDEN_TABLE_SIZE; i++)
        if (state->p25_iden_table[i].valid) iden_valid++;
    pos = sfappend(buf, buf_sz, pos, "iden %d/%d valid\n",
                   iden_valid, P25_IDEN_TABLE_SIZE);
    for (int i = 0; i < P25_IDEN_TABLE_SIZE; i++) {
        const p25_iden_entry_t *e = &state->p25_iden_table[i];
        if (!e->valid) continue;
        pos = sfappend(buf, buf_sz, pos,
                       "  iden %2d base=%.4f MHz sp=%u bw=%u off=%+d slots=%u\n",
                       i, (double)e->base_hz / 1e6,
                       (unsigned)e->spacing_hz,
                       (unsigned)e->channel_bw_hz,
                       (int)e->tx_offset_hz,
                       (unsigned)e->slots_per_carrier);
    }
    if (state->p25_phase2_grant_count != 0) {
        pos = sfappend(buf, buf_sz, pos,
                       "Phase 2 unsupported: grants=%u last TG=%u %.4f MHz "
                       "slot=%u/%u (staying on control)\n",
                       state->p25_phase2_grant_count,
                       (unsigned)state->p25_phase2_last_talkgroup,
                       (double)state->p25_phase2_last_frequency_hz / 1e6,
                       (unsigned)state->p25_phase2_last_slot,
                       (unsigned)state->p25_phase2_last_slots_per_carrier);
    }
    pos = sfappend(buf, buf_sz, pos, "neighbours %u\n",
                   (unsigned)state->p25_neighbor_count);
    for (int i = 0; i < P25_NEIGHBOR_TABLE_SIZE; i++) {
        const p25_neighbor_entry_t *n = &state->p25_neighbors[i];
        if (!n->valid) continue;
        pos = sfappend(buf, buf_sz, pos,
                       "  rfss=%u site=%u lra=$%02x sysid=$%03x ch=$%04x "
                       "svc=$%02x %.4f MHz\n",
                       (unsigned)n->rfss_id, (unsigned)n->site_id,
                       (unsigned)n->lra, (unsigned)n->sysid,
                       (unsigned)n->channel, (unsigned)n->service_class,
                       (double)n->freq_hz / 1e6);
    }
    pos = sfappend(buf, buf_sz, pos,
                   "data grants=%u uu=%u interconnect=%u reg_events=%u\n",
                   state->p25_data_grant_count,
                   state->p25_uu_grant_count,
                   state->p25_interconnect_grant_count,
                   state->p25_reg_event_count);
    int printed = 0;
    for (int op = 0; op < P25_TSBK_OPCODE_COUNT; op++) {
        if (state->p25_tsbk_unhandled[op] == 0) continue;
        if (!printed) {
            pos = sfappend(buf, buf_sz, pos, "unhandled:");
            printed = 1;
        }
        pos = sfappend(buf, buf_sz, pos, " $%02x=%u",
                       op, (unsigned)state->p25_tsbk_unhandled[op]);
    }
    if (printed) pos = sfappend(buf, buf_sz, pos, "\n");
    return pos;
}

unsigned int p25_tsbk_process_tsdu(dsd_state *state,
                                   const uint8_t *wire_dibits,
                                   size_t wire_count)
{
    uint8_t interleaved[P25_TSBK_ENCODED_DIBITS];
    uint8_t encoded[P25_TSBK_ENCODED_DIBITS];
    uint8_t decoded[P25_TSBK_DECODED_DIBITS];
    uint8_t block[P25_TSBK_BYTES];
    size_t in_block = 0;
    unsigned int accepted = 0;

    if (state == NULL)
        return 0;
    begin_grant_batch(state);
    if (!wire_dibits) return 0;
    if (wire_count > P25_TSDU_DIBIT_COUNT) wire_count = P25_TSDU_DIBIT_COUNT;

    for (size_t wire = 0; wire < wire_count; wire++) {
        /* The capture begins at packet dibit 57. Status symbols therefore
         * occur at offset 14 and every 36 dibits after it. */
        if (wire >= 14 && (wire - 14) % 36 == 0)
            continue;
        interleaved[in_block++] = wire_dibits[wire];
        if (in_block != P25_TSBK_ENCODED_DIBITS)
            continue;
        in_block = 0;

        p25_tsbk_deinterleave(interleaved, encoded);
        if (p25_tsbk_trellis_decode(encoded, decoded) < 0) {

            state->p25_tsbk_trellis_errors++;
            continue;
        }
        pack_dibits(decoded, block);
        if (p25_tsbk_crc16(block, P25_TSBK_BYTES) != 0) {
            state->p25_tsbk_crc_errors++;
            continue;
        }
        state->p25_tsbk_valid_count++;
        accepted++;
        (void)parse_block(state, block);
        if (block[0] & 0x80u)
            break;
    }
    return accepted;
}
