/* NFC Forum Type 2 Tag - capability container and TLV walk.  Pure logic:
   given a flat page-oriented memory image, decode CC and return the NDEF
   message TLV value.

   Type 2 memory layout (Type 2 Tag Operation Specification 1.2, section
   6.1):

     page 0        UID/serial number (part 1) - not parsed here
     page 1        UID/serial (part 2), BCC1, internal
     page 2        lock bytes (2), internal
     page 3        Capability Container (CC), 4 bytes:
                     [0] magic:  0xE1 = NDEF-formatted
                     [1] version: high nibble major, low nibble minor
                     [2] memory size in units of 8 bytes
                     [3] read-access nibble | write-access nibble
                          write nibble 0x0 = read/write, 0xF = read-only
     page 4..N     data area, TLVs

   TLV encoding (T2T Op Spec 2.3.2):
     Tag  = 1 byte
     Len  = 1 byte, OR 0xFF + two-byte big-endian length (>=255)
     Val  = Len bytes
   Special one-byte TLVs (no length, no value):
     0x00  NULL           (skip)
     0xFE  TERMINATOR     (stop)
   Interesting multi-byte TLVs:
     0x01  Lock control
     0x02  Memory control
     0x03  NDEF message
     0xFD  Proprietary
*/

#include "ls_nfc.h"

#include <string.h>

#define T2T_CC_PAGE            3
#define T2T_CC_OFFSET          (T2T_CC_PAGE * 4)
#define T2T_DATA_OFFSET        16          /* page 4 */

#define T2T_MAGIC_NDEF         0xE1

#define T2T_TLV_NULL           0x00
#define T2T_TLV_LOCK_CONTROL   0x01
#define T2T_TLV_MEMORY_CONTROL 0x02
#define T2T_TLV_NDEF_MESSAGE   0x03
#define T2T_TLV_PROPRIETARY    0xFD
#define T2T_TLV_TERMINATOR     0xFE

int ls_t2t_parse_cc(const uint8_t *mem, size_t len, ls_t2t_cc_t *cc)
{
    if (!mem || !cc) return LS_NFC_ERR_MALFORMED;
    memset(cc, 0, sizeof(*cc));

    if (len < T2T_CC_OFFSET + 4) return LS_NFC_ERR_TRUNCATED;

    const uint8_t *c = &mem[T2T_CC_OFFSET];
    cc->magic         = c[0];
    cc->version_major = (uint8_t)((c[1] >> 4) & 0x0F);
    cc->version_minor = (uint8_t)(c[1] & 0x0F);
    cc->memory_size_bytes = (size_t)c[2] * 8u;
    /* Type 2 write-access nibble: 0x0 = full read/write, 0x0F = read-only,
       everything else is proprietary.  Treat any non-zero write nibble
       as read-only from the reader's point of view - we don't write
       anyway in this slice. */
    cc->read_only     = (c[3] & 0x0F) != 0x00;

    if (cc->magic != T2T_MAGIC_NDEF) {
        cc->valid = false;
        return LS_NFC_ERR_UNSUPPORTED_TAG;
    }
    /* NFC Forum Type 2 Tag Op Spec version was v1.x when this component
       was written; a major of 0 or above 1 is not a tag we know how to
       parse.  The read still succeeds (the CC is well-formed), but the
       caller must be told. */
    if (cc->version_major != 1) {
        cc->valid = false;
        return LS_NFC_ERR_UNSUPPORTED_TAG;
    }
    cc->valid = true;
    return LS_NFC_ERR_NONE;
}

int ls_t2t_find_ndef(const uint8_t *mem, size_t len,
                     size_t max_message_bytes,
                     const uint8_t **ndef_bytes, size_t *ndef_len)
{
    if (ndef_bytes) *ndef_bytes = NULL;
    if (ndef_len)   *ndef_len   = 0;
    if (!mem) return LS_NFC_ERR_MALFORMED;
    if (len < T2T_DATA_OFFSET) return LS_NFC_ERR_TRUNCATED;

    size_t i = T2T_DATA_OFFSET;
    while (i < len) {
        uint8_t t = mem[i++];

        if (t == T2T_TLV_NULL) continue;
        if (t == T2T_TLV_TERMINATOR) return LS_NFC_ERR_NONE;

        /* All other TLVs carry a length. */
        if (i >= len) return LS_NFC_ERR_TRUNCATED;
        size_t l;
        uint8_t l0 = mem[i++];
        if (l0 == 0xFF) {
            if (len - i < 2) return LS_NFC_ERR_TRUNCATED;
            l = ((size_t)mem[i] << 8) | mem[i + 1];
            i += 2;
            /* Three-byte length form is only defined for lengths >= 0xFF
               (T2T Op Spec 2.3.2).  A shorter length in the long form is
               malformed. */
            if (l < 0xFF) return LS_NFC_ERR_MALFORMED;
        } else {
            l = l0;
        }
        if (l > len - i) return LS_NFC_ERR_TRUNCATED;

        if (t == T2T_TLV_NDEF_MESSAGE) {
            if (max_message_bytes && l > max_message_bytes)
                return LS_NFC_ERR_TOO_LARGE;
            if (ndef_bytes) *ndef_bytes = l ? &mem[i] : NULL;
            if (ndef_len)   *ndef_len   = l;
            return LS_NFC_ERR_NONE;
        }

        /* Skip: LOCK_CONTROL, MEMORY_CONTROL, PROPRIETARY, unknown.  The
           TLV walk continues past them; anything unknown is not fatal by
           spec because it is TLV-encoded on purpose. */
        i += l;
    }

    /* Ran off the end without a terminator or an NDEF TLV.  Report no
       NDEF, not truncation - a legitimately blank tag with a nonzero
       memory size is a spec case. */
    return LS_NFC_ERR_NONE;
}
