/* LS-610: P25 Encryption Sync Stream helpers.
 *
 * Kept in a small file of their own so the bench can link them against
 * fake dsd_state / dsd_opts without pulling in the whole decoder. The
 * three routines here are:
 *   p25_ess_clear                - reset ESS on end-of-call / TG change
 *   p25_algid_name               - short display name for six common ALGIDs
 *   p25_ldu_should_mute_encrypted - the single predicate the vocoder gate
 *                                   consults; also mirrored by the P25 UI.
 */

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
    /* LS-611: unknown ESS (no LDU2 seen yet on this call) mutes. LDU1/LDU2
     * alternate at 180 ms and ALGID is at the end of LDU2. Worst-case wait
     * for a first ALGID after joining a granted channel mid-call:
     *   - land at start of LDU1: ~360 ms to end of following LDU2
     *   - one LDU2 FEC failure:  ~720 ms (skip a whole 360 ms LDU1+LDU2 pair)
     *   - two consecutive fails: ~1080 ms
     * The old policy passed on unknown to avoid clipping the head of a clear
     * call. That left a burst of vocoder noise on the head of every encrypted
     * call - exactly the symptom this whole gate is meant to remove. Muting
     * on unknown costs at most ~360 ms of a clear call (inaudible as a clip
     * for typical PTT ramp-ups); an FEC-failure tail of ~720 ms is a noticeable
     * clip but still preferable to encrypted-audio leakage. */
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
