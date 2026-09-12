/* LS_TEST_SOURCES: ${FW}/components/lakeshark/nfc/ndef.c */
/* LS_TEST_INCLUDE: ${FW}/components/lakeshark/nfc */
/* NFC NDEF parser tests.  Covers:

     - single-record short record with MB=ME=1
     - multi-record chain (Text + URI) with the message-begin/end flags
       placed on the right ends
     - long-form (32-bit) payload length
     - id-length field
     - malformed: bad flag layout, chunk flag set, trailing bytes
     - truncation: header, payload-length, payload
     - too-many-records via out_cap

   Each malformed/truncated case is written so the parser must reject
   it - a permissive parser that accepted the case would pass, and this
   suite would go red.

   NFC Forum NDEF spec 1.0, section 3.2. */

#include "ls_test.h"
#include "ls_nfc.h"

#include <string.h>

static void put_sr(uint8_t *buf, size_t *off, uint8_t flags,
                   const char *type, const void *payload, uint8_t plen)
{
    /* Short record: header | type_len | payload_len | type | payload */
    uint8_t tlen = (uint8_t)strlen(type);
    buf[(*off)++] = flags | 0x10 /* SR*/ | 0x01 /* WELL_KNOWN*/;
    buf[(*off)++] = tlen;
    buf[(*off)++] = plen;
    memcpy(&buf[*off], type, tlen); *off += tlen;
    memcpy(&buf[*off], payload, plen); *off += plen;
}

LS_CASE(ndef_single_short_uri_record_parses)
{
    uint8_t buf[64]; size_t off = 0;
    /* URI record, "\x02example.com" = "https://www." + rest */
    const char *p = "\x02" "example.com";
    put_sr(buf, &off, 0x80 | 0x40 /* MB|ME*/, "U", p, (uint8_t)strlen(p));

    ls_nfc_ndef_record_t recs[4];
    size_t n = 0;
    int r = ls_ndef_parse(buf, off, recs, 4, &n);
    LS_EQ_INT(r, LS_NFC_ERR_NONE);
    LS_EQ_UINT(n, 1);
    LS_CHECK(recs[0].mb && recs[0].me && recs[0].sr);
    LS_EQ_UINT(recs[0].tnf, LS_NFC_TNF_WELL_KNOWN);
    LS_EQ_UINT(recs[0].type_len, 1);
    LS_EQ_UINT(recs[0].type[0], 'U');
    LS_EQ_UINT(recs[0].payload_len, strlen(p));
    LS_EQ_UINT(recs[0].payload[0], 0x02);

    uint8_t prefix = 0;
    const uint8_t *rest = NULL; size_t rest_len = 0;
    LS_CHECK(ls_ndef_decode_uri(&recs[0], &prefix, &rest, &rest_len));
    LS_EQ_UINT(prefix, 0x02);
    LS_EQ_UINT(rest_len, 11);
}

LS_CASE(ndef_multi_record_text_then_uri)
{
    uint8_t buf[128]; size_t off = 0;
    /* Text: "en" + "hi" */
    uint8_t text[] = { 0x02 /* status: UTF-8, lang_len 2*/, 'e','n', 'h','i' };
    /* URI: "\x00example.com" (no prefix) */
    const char *uri = "\x00" "example.com";

    /* first record: MB=1, ME=0 */
    put_sr(buf, &off, 0x80, "T", text, sizeof(text));
    /* second record: MB=0, ME=1 */
    put_sr(buf, &off, 0x40, "U", uri, (uint8_t)strlen(uri));

    ls_nfc_ndef_record_t recs[4];
    size_t n = 0;
    int r = ls_ndef_parse(buf, off, recs, 4, &n);
    LS_EQ_INT(r, LS_NFC_ERR_NONE);
    LS_EQ_UINT(n, 2);
    LS_CHECK(recs[0].mb && !recs[0].me);
    LS_CHECK(!recs[1].mb && recs[1].me);

    const uint8_t *lang = NULL; uint8_t lang_len = 0;
    const uint8_t *txt  = NULL; size_t   txt_len  = 0;
    bool utf16 = true;
    LS_CHECK(ls_ndef_decode_text(&recs[0], &lang, &lang_len, &txt,
                                 &txt_len, &utf16));
    LS_EQ_UINT(lang_len, 2);
    LS_EQ_UINT(txt_len, 2);
    LS_CHECK(lang[0] == 'e' && lang[1] == 'n');
    LS_CHECK(txt[0] == 'h' && txt[1] == 'i');
    LS_CHECK(!utf16);
}

LS_CASE(ndef_long_form_payload_length_parses)
{
    /* MB|ME, not SR, not IL, TNF=UNKNOWN(0x05), type_len=0, payload_len=300 */
    uint8_t buf[400]; size_t off = 0;
    buf[off++] = 0x80 | 0x40 | 0x05;
    buf[off++] = 0;
    buf[off++] = 0; buf[off++] = 0; buf[off++] = 0x01; buf[off++] = 0x2C; /* 300*/
    for (int i = 0; i < 300; i++) buf[off++] = (uint8_t)i;

    ls_nfc_ndef_record_t recs[4]; size_t n = 0;
    int r = ls_ndef_parse(buf, off, recs, 4, &n);
    LS_EQ_INT(r, LS_NFC_ERR_NONE);
    LS_EQ_UINT(n, 1);
    LS_EQ_UINT(recs[0].payload_len, 300u);
    LS_EQ_UINT(recs[0].payload[299], 299 & 0xFF);
}

LS_CASE(ndef_id_length_field_parses)
{
    /* MB|ME|SR|IL, TNF=EXTERNAL(0x04), type_len=1, id_len=2, plen=0 */
    uint8_t buf[16]; size_t off = 0;
    buf[off++] = 0x80 | 0x40 | 0x10 | 0x08 | 0x04;
    buf[off++] = 1;   /* type_len */
    buf[off++] = 0;   /* payload_len */
    buf[off++] = 2;   /* id_len */
    buf[off++] = 'X'; /* type */
    buf[off++] = 'a'; buf[off++] = 'b';   /* id */

    ls_nfc_ndef_record_t recs[4]; size_t n = 0;
    int r = ls_ndef_parse(buf, off, recs, 4, &n);
    LS_EQ_INT(r, LS_NFC_ERR_NONE);
    LS_EQ_UINT(n, 1);
    LS_EQ_UINT(recs[0].id_len, 2);
    LS_CHECK(recs[0].id[0] == 'a' && recs[0].id[1] == 'b');
}

LS_CASE(ndef_missing_message_begin_rejected)
{
    uint8_t buf[8]; size_t off = 0;
    put_sr(buf, &off, 0x40 /* ME only, no MB*/, "T", "", 0);
    ls_nfc_ndef_record_t recs[4]; size_t n = 0;
    int r = ls_ndef_parse(buf, off, recs, 4, &n);
    LS_EQ_INT(r, LS_NFC_ERR_MALFORMED);
}

LS_CASE(ndef_missing_message_end_is_truncation)
{
    /* MB=1 record but no ME anywhere - message is not complete. */
    uint8_t buf[8]; size_t off = 0;
    put_sr(buf, &off, 0x80 /* MB only*/, "T", "", 0);
    ls_nfc_ndef_record_t recs[4]; size_t n = 0;
    int r = ls_ndef_parse(buf, off, recs, 4, &n);
    LS_EQ_INT(r, LS_NFC_ERR_TRUNCATED);
}

LS_CASE(ndef_chunk_flag_rejected)
{
    uint8_t buf[8]; size_t off = 0;
    /* MB|CF|SR - chunked first record.  We do not support reassembly. */
    put_sr(buf, &off, 0x80 | 0x20, "T", "", 0);
    ls_nfc_ndef_record_t recs[4]; size_t n = 0;
    int r = ls_ndef_parse(buf, off, recs, 4, &n);
    LS_EQ_INT(r, LS_NFC_ERR_MALFORMED);
}

LS_CASE(ndef_bytes_after_message_end_rejected)
{
    uint8_t buf[16]; size_t off = 0;
    put_sr(buf, &off, 0x80 | 0x40, "T", "", 0);
    /* trailing 0x00 - not a NULL TLV (that lives at the T2T layer), just
       a stray byte at the NDEF layer. */
    buf[off++] = 0x00;
    ls_nfc_ndef_record_t recs[4]; size_t n = 0;
    int r = ls_ndef_parse(buf, off, recs, 4, &n);
    LS_EQ_INT(r, LS_NFC_ERR_MALFORMED);
}

LS_CASE(ndef_payload_longer_than_buffer_is_truncation)
{
    uint8_t buf[8]; size_t off = 0;
    /* MB|ME|SR|TNF=1, type=1, payload_len=42, but only 3 payload bytes */
    buf[off++] = 0x80 | 0x40 | 0x10 | 0x01;
    buf[off++] = 1;
    buf[off++] = 42;
    buf[off++] = 'T';
    buf[off++] = 'a'; buf[off++] = 'b'; buf[off++] = 'c';
    ls_nfc_ndef_record_t recs[4]; size_t n = 0;
    int r = ls_ndef_parse(buf, off, recs, 4, &n);
    LS_EQ_INT(r, LS_NFC_ERR_TRUNCATED);
}

LS_CASE(ndef_truncated_long_length_field_rejected)
{
    /* Long form: header, type_len, then only 3 of the 4 length bytes. */
    uint8_t buf[8] = { 0x80 | 0x40 | 0x05, 0, 0, 0, 1 };
    ls_nfc_ndef_record_t recs[4]; size_t n = 0;
    int r = ls_ndef_parse(buf, 5, recs, 4, &n);
    LS_EQ_INT(r, LS_NFC_ERR_TRUNCATED);
}

LS_CASE(ndef_too_many_records_reports_too_large)
{

    uint8_t buf[64]; size_t off = 0;
    put_sr(buf, &off, 0x80,        "T", "", 0);   /* MB      */
    put_sr(buf, &off, 0x00,        "T", "", 0);   /* neither */
    put_sr(buf, &off, 0x40,        "T", "", 0);   /*      ME */
    ls_nfc_ndef_record_t recs[2]; size_t n = 0;
    int r = ls_ndef_parse(buf, off, recs, 2, &n);
    LS_EQ_INT(r, LS_NFC_ERR_TOO_LARGE);
}

LS_CASE(ndef_empty_record_with_nonzero_type_rejected)
{
    /* TNF=EMPTY must have all lengths zero. */
    uint8_t buf[8] = { 0x80 | 0x40 | 0x10 | 0x00, /* type_len*/1, /* plen*/0, 'X' };
    ls_nfc_ndef_record_t recs[4]; size_t n = 0;
    int r = ls_ndef_parse(buf, 4, recs, 4, &n);
    LS_EQ_INT(r, LS_NFC_ERR_MALFORMED);
}
