

#include "nfc_a.h"

#include <string.h>

int ls_nfca_assemble_uid(const ls_nfca_sel_t *resps,
                         const uint8_t *saks,
                         size_t levels,
                         uint8_t out_uid[LS_NFC_UID_MAX],
                         uint8_t *out_uid_len,
                         uint8_t *out_final_sak)
{
    if (!resps || !saks || !out_uid || !out_uid_len || !out_final_sak)
        return LS_NFC_ERR_MALFORMED;
    if (levels < 1 || levels > 3) return LS_NFC_ERR_MALFORMED;

    memset(out_uid, 0, LS_NFC_UID_MAX);
    *out_uid_len = 0;
    *out_final_sak = 0;

    uint8_t uid[LS_NFC_UID_MAX];
    size_t  uid_pos = 0;

    for (size_t i = 0; i < levels; i++) {
        const uint8_t *b   = resps[i].bytes;
        uint8_t        sak = saks[i];

        if (ls_nfca_bcc(b) != b[4]) return LS_NFC_ERR_MALFORMED;

        bool is_last = (i + 1 == levels);
        bool cascade_present = (b[0] == LS_NFCA_CASCADE_TAG);
        bool cascade_bit_set = (sak & 0x04) != 0;

        /* SAK bit 2 (0x04) is the cascade bit: set when more levels
           follow, clear on the last.  A response that carries the
           cascade tag must have the cascade bit set and cannot be the
           last level; a response without the cascade tag must have the
           bit clear and must be the last. */
        if (cascade_present) {
            if (is_last) return LS_NFC_ERR_MALFORMED;
            if (!cascade_bit_set) return LS_NFC_ERR_MALFORMED;
            /* Contribute three UID bytes, dropping the cascade tag. */
            if (uid_pos + 3 > LS_NFC_UID_MAX) return LS_NFC_ERR_MALFORMED;
            uid[uid_pos++] = b[1];
            uid[uid_pos++] = b[2];
            uid[uid_pos++] = b[3];
        } else {
            if (!is_last) return LS_NFC_ERR_MALFORMED;
            if (cascade_bit_set) return LS_NFC_ERR_MALFORMED;
            if (uid_pos + 4 > LS_NFC_UID_MAX) return LS_NFC_ERR_MALFORMED;
            uid[uid_pos++] = b[0];
            uid[uid_pos++] = b[1];
            uid[uid_pos++] = b[2];
            uid[uid_pos++] = b[3];
        }
    }

    /* ISO 14443-3 allows exactly 4, 7 or 10 UID bytes.  Anything else
       here indicates a mismatch between levels[] and the cascade
       structure that slipped past the per-level checks. */
    if (uid_pos != 4 && uid_pos != 7 && uid_pos != 10)
        return LS_NFC_ERR_MALFORMED;

    memcpy(out_uid, uid, uid_pos);
    *out_uid_len   = (uint8_t)uid_pos;
    *out_final_sak = saks[levels - 1];
    return LS_NFC_ERR_NONE;
}
