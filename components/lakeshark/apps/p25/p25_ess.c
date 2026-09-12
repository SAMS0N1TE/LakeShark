/* P25 Encryption Sync Stream helpers. */

#include <string.h>
#include "dsd.h"

void p25_ess_clear(dsd_state *state)
{
    if (!state) return;
    state->p25_algid = 0;
    state->p25_kid = 0;
    memset(state->p25_mi, 0, sizeof(state->p25_mi));
    state->p25_ess_valid = 0;
}

const char *p25_algid_name(uint8_t algid)
{
    switch (algid) {
        case 0x80: return "CLEAR";
        case 0x81: return "DES-OFB";
        case 0x83: return "3DES";
        case 0x84: return "ADP";
        case 0x89: return "AES-256";
        case 0x9F: return "AES-128";
        default:   return NULL;
    }
}

int p25_ldu_should_mute_encrypted(const dsd_state *state, const dsd_opts *opts)
{
    if (!state || !opts) return 0;
    if (opts->unmute_encrypted_p25 != 0) return 0;
    /* unknown ESS (no LDU2 seen yet on this call) mutes. */

    if (state->p25_ess_valid == 0) return 1;
    return state->p25_algid != 0x80;
}

int p25_algid_is_encrypted(uint8_t algid)
{
    /* CLEAR is 0x80. Anything else in a valid ESS is an encryption algorithm
     * we cannot decode. The grant follower consults this to decide whether
     * to hand control back after the first LDU2 lands. */
    return algid != 0x80;
}
