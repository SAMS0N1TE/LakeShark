#include "dmr.h"

#include <string.h>

static uint32_t get_bits_msb(const uint8_t *buf, unsigned first, unsigned count)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned bit = first + i;
        v = (v << 1) | ((buf[bit >> 3] >> (7u - (bit & 7u))) & 1u);
    }
    return v;
}

void dmr_lc_parse(const uint8_t bits96[12], dmr_lc_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!bits96) return;
    out->protect_flag    = (uint8_t)get_bits_msb(bits96, 0, 1);
    /* bit 1 reserved, skipped */
    out->flco            = (uint8_t)get_bits_msb(bits96, 2, 6);
    out->fid             = (uint8_t)get_bits_msb(bits96, 8, 8);
    out->service_options = (uint8_t)get_bits_msb(bits96, 16, 8);
    out->destination     = get_bits_msb(bits96, 24, 24);
    out->source          = get_bits_msb(bits96, 48, 24);
}

/* Parity ordering, generator coefficients and burst masks independently
 * checked against MMDVM-Host RS129.cpp/DMRFullLC.cpp/DMRDefines.h, commit
 * 590c531391dfd3146073afbc3956f70d42c62a46 (Jonathan Naylor G4KLX and
 * upstream contributors, GPL-2.0-or-later). This implementation uses direct
 * GF(256) multiplication instead of upstream's logarithm lookup tables. */
static uint8_t gf_multiply(uint8_t a, uint8_t b)
{
    uint8_t result=0;
    while(b) {
        if(b&1u) result^=a;
        a=(uint8_t)((a<<1)^((a&0x80u)?0x1Du:0u));
        b>>=1;
    }
    return result;
}

int dmr_lc_decode(const uint8_t bits96[12], uint8_t data_type, dmr_lc_t *out)
{
    if(!bits96 || !out || (data_type!=1 && data_type!=2)) return 0;
    uint8_t parity[3]={0};
    for(unsigned i=0;i<9;i++) {
        uint8_t feedback=bits96[i]^parity[2];
        parity[2]=parity[1]^gf_multiply(14,feedback);
        parity[1]=parity[0]^gf_multiply(56,feedback);
        parity[0]=gf_multiply(64,feedback);
    }
    uint8_t mask=data_type==1?0x96u:0x99u;
    for(unsigned i=0;i<3;i++)
        if(bits96[9+i]!=(uint8_t)(parity[2-i]^mask)) return 0;
    dmr_lc_parse(bits96,out);
    return 1;
}
