/* CSBK and data header: the two BPTC blocks protected by CRC-CCITT rather
 * than the Reed-Solomon parity link control carries. */
#include "dmr.h"

#include <string.h>

static uint32_t be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/* x^16 + x^12 + x^5 + 1, MSB first, from zero, inverted on the way out
   (ETSI TS 102 361-1 B.3.7: the remainder is complemented before sending). */
uint16_t dmr_crc_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; data && i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return (uint16_t)~crc;
}

int dmr_block_crc_ok(const uint8_t bits96[12], uint16_t mask)
{
    if (!bits96) return 0;
    const uint16_t got = (uint16_t)(((uint16_t)bits96[10] << 8) | bits96[11]);
    return (uint16_t)(dmr_crc_ccitt(bits96, 10) ^ mask) == got;
}

/* ------------------------------------------------------------------ CSBK --
 * LB(1) PF(1) CSBKO(6) FID(8), 64 opcode bits, CRC(16). Layouts below are
 * ETSI TS 102 361-2 7.1.2 for the opcodes a Tier II receiver meets most. */

int dmr_csbk_decode(const uint8_t bits96[12], dmr_csbk_t *out)
{
    if (!bits96 || !out || !dmr_block_crc_ok(bits96, DMR_CRC_MASK_CSBK)) return 0;
    dmr_csbk_t c;
    memset(&c, 0, sizeof(c));
    c.last_block   = (bits96[0] & 0x80u) != 0;
    c.protect_flag = (bits96[0] & 0x40u) != 0;
    c.opcode       = bits96[0] & 0x3Fu;
    c.fid          = bits96[1];
    memcpy(c.data, bits96 + 2, sizeof(c.data));

    if (c.fid == 0) {
        switch (c.opcode) {
        case DMR_CSBKO_UU_V_REQ:
            /* service options, reserved, target, source */
            c.service_options = c.data[0];
            c.target = be24(c.data + 2);
            c.source = be24(c.data + 5);
            c.has_addresses = true;
            break;
        case DMR_CSBKO_UU_ANS_RSP:
            /* service options, answer response, target, source */
            c.service_options = c.data[0];
            c.answer = c.data[1];
            c.target = be24(c.data + 2);
            c.source = be24(c.data + 5);
            c.has_addresses = true;
            break;
        case DMR_CSBKO_BS_DWN_ACT:
            /* 16 reserved, the BS being woken, the MS waking it */
            c.target = be24(c.data + 2);
            c.source = be24(c.data + 5);
            c.has_addresses = true;
            break;
        case DMR_CSBKO_PREAMBLE:
            /* data/CSBK, group/individual, 6 reserved, blocks to follow,
               target, source */
            c.preamble_data    = (c.data[0] & 0x80u) != 0;
            c.preamble_group   = (c.data[0] & 0x40u) != 0;
            c.blocks_to_follow = c.data[1];
            c.target = be24(c.data + 2);
            c.source = be24(c.data + 5);
            c.has_addresses = true;
            break;
        default:
            break;
        }
    }
    *out = c;
    return 1;
}

const char *dmr_csbk_name(uint8_t opcode, uint8_t fid)
{
    if (fid != 0) return "manufacturer";
    switch (opcode & 0x3Fu) {
    case DMR_CSBKO_UU_V_REQ:   return "UU voice request";
    case DMR_CSBKO_UU_ANS_RSP: return "UU answer";
    case DMR_CSBKO_CT_CSBK:    return "channel timing";
    case DMR_CSBKO_NACK_RSP:   return "NACK";
    case DMR_CSBKO_BS_DWN_ACT: return "BS activation";
    case DMR_CSBKO_PREAMBLE:   return "preamble";
    default:                   return "other";
    }
}

/* ------------------------------------------------------------ data header --
 * Octet 0: G/I, A, then format-specific bits, DPF in the low nibble. Octet
 * 1: SAP in the high nibble. Octets 2-4 destination, 5-7 source. Octets 8-9
 * format-specific: BF and FSN for packet data, class/type/status for a
 * response, appended blocks spread over octets 0-1 for short data. */

int dmr_data_header_decode(const uint8_t bits96[12], dmr_data_header_t *out)
{
    if (!bits96 || !out || !dmr_block_crc_ok(bits96, DMR_CRC_MASK_DATA_HDR)) return 0;
    dmr_data_header_t h;
    memset(&h, 0, sizeof(h));
    h.group              = (bits96[0] & 0x80u) != 0;
    h.response_requested = (bits96[0] & 0x40u) != 0;
    h.dpf                = bits96[0] & 0x0Fu;
    h.sap                = bits96[1] >> 4;
    if (h.dpf != DMR_DPF_PROPRIETARY) {
        h.destination = be24(bits96 + 2);
        h.source      = be24(bits96 + 5);
    }
    switch (h.dpf) {
    case DMR_DPF_UNCONFIRMED:
    case DMR_DPF_CONFIRMED:
        h.pad_octets       = (uint8_t)(((bits96[0] & 0x10u) ? 0x10u : 0u) | (bits96[1] & 0x0Fu));
        h.full_message     = (bits96[8] & 0x80u) != 0;
        h.blocks_to_follow = bits96[8] & 0x7Fu;
        h.fsn              = bits96[9] & 0x0Fu;
        break;
    case DMR_DPF_RESPONSE:
        h.blocks_to_follow = bits96[8] & 0x7Fu;
        h.rsp_class  = bits96[9] >> 6;
        h.rsp_type   = (bits96[9] >> 3) & 0x07u;
        h.rsp_status = bits96[9] & 0x07u;
        break;
    case DMR_DPF_SHORT_DEFINED:
    case DMR_DPF_SHORT_RAW:
        h.blocks_to_follow = (uint8_t)(((bits96[0] & 0x30u) << 0) | (bits96[1] & 0x0Fu));
        break;
    default:
        break;
    }
    *out = h;
    return 1;
}

const char *dmr_dpf_name(uint8_t dpf)
{
    switch (dpf & 0x0Fu) {
    case DMR_DPF_UDT:           return "UDT";
    case DMR_DPF_RESPONSE:      return "response";
    case DMR_DPF_UNCONFIRMED:   return "unconfirmed";
    case DMR_DPF_CONFIRMED:     return "confirmed";
    case DMR_DPF_SHORT_DEFINED: return "short defined";
    case DMR_DPF_SHORT_RAW:     return "short raw";
    case DMR_DPF_PROPRIETARY:   return "proprietary";
    default:                    return "reserved";
    }
}
