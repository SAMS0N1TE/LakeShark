/* NFC Forum NDEF message parser.  Pure logic - no radio, no SDK, no
   allocation.  Views into the input buffer, so the input must outlive
   the record array.

   The chunk-flag (CF) is not honoured.  A CF=1 record fails as
   LS_NFC_ERR_MALFORMED; the read-only slice in bench/NFC.md does not
   promise chunk reassembly.  Reserved TNF=0x07 is a spec-level malformed
   record and fails likewise. */

#include "ls_nfc.h"

#include <string.h>

/* NDEF record header, first byte:
     bit 7 MB   message-begin
     bit 6 ME   message-end
     bit 5 CF   chunk-flag
     bit 4 SR   short-record (1-byte payload length)
     bit 3 IL   id-length field present
     bits 2..0  TNF */
#define NDEF_MB 0x80
#define NDEF_ME 0x40
#define NDEF_CF 0x20
#define NDEF_SR 0x10
#define NDEF_IL 0x08
#define NDEF_TNF_MASK 0x07

int ls_ndef_parse(const uint8_t *buf, size_t len,
                  ls_nfc_ndef_record_t *out, size_t out_cap,
                  size_t *out_count)
{
    if (out_count) *out_count = 0;
    if (!buf || !out || out_cap == 0) return LS_NFC_ERR_MALFORMED;
    if (len == 0) return LS_NFC_ERR_TRUNCATED;

    size_t i = 0;
    size_t n = 0;
    bool saw_end = false;

    while (i < len) {
        if (n >= out_cap) return LS_NFC_ERR_TOO_LARGE;
        if (saw_end) {
            /* Bytes after ME=1 are trailing garbage - not part of the
               message.  Refuse rather than pretend they parse. */
            return LS_NFC_ERR_MALFORMED;
        }

        if (len - i < 1) return LS_NFC_ERR_TRUNCATED;
        uint8_t h = buf[i++];

        ls_nfc_ndef_record_t *r = &out[n];
        memset(r, 0, sizeof(*r));
        r->mb  = (h & NDEF_MB) != 0;
        r->me  = (h & NDEF_ME) != 0;
        r->cf  = (h & NDEF_CF) != 0;
        r->sr  = (h & NDEF_SR) != 0;
        r->il  = (h & NDEF_IL) != 0;
        r->tnf = (ls_nfc_tnf_t)(h & NDEF_TNF_MASK);

        /* First record must set MB; last must set ME.  Middle records
           must not set either. */
        if (n == 0 && !r->mb)          return LS_NFC_ERR_MALFORMED;
        if (n != 0 && r->mb)           return LS_NFC_ERR_MALFORMED;
        if (r->cf)                     return LS_NFC_ERR_MALFORMED;
        if (r->tnf == LS_NFC_TNF_RESERVED)
                                       return LS_NFC_ERR_MALFORMED;

        /* An EMPTY record must have type_len, id_len and payload_len all
           zero; UNKNOWN must have type_len zero; UNCHANGED must have
           type_len zero.  These are the NFC Forum record-well-formedness
           rules that the parser can check without knowing the payload. */

        if (len - i < 1) return LS_NFC_ERR_TRUNCATED;
        r->type_len = buf[i++];

        uint32_t payload_len;
        if (r->sr) {
            if (len - i < 1) return LS_NFC_ERR_TRUNCATED;
            payload_len = buf[i++];
        } else {
            if (len - i < 4) return LS_NFC_ERR_TRUNCATED;
            payload_len = ((uint32_t)buf[i]     << 24) |
                          ((uint32_t)buf[i + 1] << 16) |
                          ((uint32_t)buf[i + 2] <<  8) |
                          ((uint32_t)buf[i + 3]);
            i += 4;
        }
        r->payload_len = payload_len;

        if (r->il) {
            if (len - i < 1) return LS_NFC_ERR_TRUNCATED;
            r->id_len = buf[i++];
        } else {
            r->id_len = 0;
        }

        if (r->tnf == LS_NFC_TNF_EMPTY) {
            if (r->type_len != 0 || r->id_len != 0 || r->payload_len != 0)
                return LS_NFC_ERR_MALFORMED;
        } else if (r->tnf == LS_NFC_TNF_UNKNOWN ||
                   r->tnf == LS_NFC_TNF_UNCHANGED) {
            if (r->type_len != 0) return LS_NFC_ERR_MALFORMED;
        }

        /* Range-check every remaining field against len before capturing
           any pointer.  A truncated payload length that would wrap must
           be rejected. */
        if (r->type_len > len - i) return LS_NFC_ERR_TRUNCATED;
        r->type = r->type_len ? &buf[i] : NULL;
        i += r->type_len;

        if (r->id_len > len - i) return LS_NFC_ERR_TRUNCATED;
        r->id = r->id_len ? &buf[i] : NULL;
        i += r->id_len;

        if (r->payload_len > len - i) return LS_NFC_ERR_TRUNCATED;
        r->payload = r->payload_len ? &buf[i] : NULL;
        i += r->payload_len;

        n++;
        if (r->me) saw_end = true;
    }

    if (!saw_end) return LS_NFC_ERR_TRUNCATED;

    if (out_count) *out_count = n;
    return LS_NFC_ERR_NONE;
}

/* -------- convenience decoders -------- */

bool ls_ndef_decode_text(const ls_nfc_ndef_record_t *r,
                         const uint8_t **lang, uint8_t *lang_len,
                         const uint8_t **text, size_t *text_len,
                         bool *utf16)
{
    if (!r || r->tnf != LS_NFC_TNF_WELL_KNOWN) return false;
    if (r->type_len != 1 || !r->type || r->type[0] != 'T') return false;
    if (r->payload_len < 1 || !r->payload) return false;

    uint8_t status = r->payload[0];
    uint8_t lang_l = status & 0x3F;
    if (status & 0x40) return false;    /* RFU bit */
    if ((size_t)1 + lang_l > r->payload_len) return false;

    if (utf16)    *utf16    = (status & 0x80) != 0;
    if (lang_len) *lang_len = lang_l;
    if (lang)     *lang     = lang_l ? &r->payload[1] : NULL;
    if (text)     *text     = &r->payload[1 + lang_l];
    if (text_len) *text_len = r->payload_len - 1 - lang_l;
    return true;
}

bool ls_ndef_decode_uri(const ls_nfc_ndef_record_t *r,
                        uint8_t *prefix,
                        const uint8_t **rest, size_t *rest_len)
{
    if (!r || r->tnf != LS_NFC_TNF_WELL_KNOWN) return false;
    if (r->type_len != 1 || !r->type || r->type[0] != 'U') return false;
    if (r->payload_len < 1 || !r->payload) return false;

    /* NFC URI RTD abbreviation table has 36 entries; anything beyond that
       is reserved.  Callers can render 0 as "no prefix, take the rest
       verbatim". */
    if (r->payload[0] > 0x23) return false;

    if (prefix)   *prefix   = r->payload[0];
    if (rest)     *rest     = r->payload_len > 1 ? &r->payload[1] : NULL;
    if (rest_len) *rest_len = r->payload_len - 1;
    return true;
}
