/* NFC-A anticollision - private helpers.  Public API stays in ls_nfc.h;
   this header carries the pure-logic seams the bench drives directly. */

#ifndef LS_NFC_A_H
#define LS_NFC_A_H

#include "ls_nfc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One SEL response, five bytes: four UID (or cascade-tag + three UID) plus
   the BCC.  ISO/IEC 14443-3 §6.5.4. */
typedef struct {
    uint8_t bytes[5];
} ls_nfca_sel_t;

/* NFC-A cascade tag inserted into level-N responses when the UID has more
   bytes to come (ISO/IEC 14443-3 §6.5.4). */
#define LS_NFCA_CASCADE_TAG 0x88

int ls_nfca_assemble_uid(const ls_nfca_sel_t *resps,
                         const uint8_t *saks,
                         size_t levels,
                         uint8_t out_uid[LS_NFC_UID_MAX],
                         uint8_t *out_uid_len,
                         uint8_t *out_final_sak);

/* BCC (block check character): XOR of the first four bytes. */
static inline uint8_t ls_nfca_bcc(const uint8_t b[4])
{
    return (uint8_t)(b[0] ^ b[1] ^ b[2] ^ b[3]);
}

#ifdef __cplusplus
}
#endif
#endif
