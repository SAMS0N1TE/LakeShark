/* LS_TEST_SOURCES: ${FW}/components/lakeshark/nfc/t2t.c */
/* LS_TEST_INCLUDE: ${FW}/components/lakeshark/nfc */
/* NFC Forum Type 2 Tag CC and TLV walk. */

#include "ls_test.h"
#include "ls_nfc.h"

#include <string.h>

static void set_cc(uint8_t *mem, uint8_t magic, uint8_t ver, uint8_t sz_x8,
                   uint8_t access)
{
    mem[12] = magic;
    mem[13] = ver;
    mem[14] = sz_x8;
    mem[15] = access;
}

LS_CASE(t2t_cc_parses_ndef_formatted_ntag213)
{
    /* NTAG213 CC: E1 10 12 00  => version 1.0, memory 0x12*8 = 144 bytes,
       full read/write. */
    uint8_t mem[64] = {0};
    set_cc(mem, 0xE1, 0x10, 0x12, 0x00);

    ls_t2t_cc_t cc;
    int r = ls_t2t_parse_cc(mem, sizeof(mem), &cc);
    LS_EQ_INT(r, LS_NFC_ERR_NONE);
    LS_CHECK(cc.valid);
    LS_EQ_UINT(cc.version_major, 1);
    LS_EQ_UINT(cc.version_minor, 0);
    LS_EQ_UINT(cc.memory_size_bytes, 144u);
    LS_CHECK(!cc.read_only);
}

LS_CASE(t2t_cc_write_locked_flags_read_only)
{
    uint8_t mem[64] = {0};
    set_cc(mem, 0xE1, 0x10, 0x06, 0x0F);   /* write nibble non-zero */

    ls_t2t_cc_t cc;
    LS_EQ_INT(ls_t2t_parse_cc(mem, sizeof(mem), &cc), LS_NFC_ERR_NONE);
    LS_CHECK(cc.read_only);
}

LS_CASE(t2t_cc_missing_magic_reports_unsupported)
{
    uint8_t mem[64] = {0};
    set_cc(mem, 0x00, 0x10, 0x12, 0x00);   /* not NDEF-formatted */

    ls_t2t_cc_t cc;
    int r = ls_t2t_parse_cc(mem, sizeof(mem), &cc);
    LS_EQ_INT(r, LS_NFC_ERR_UNSUPPORTED_TAG);
    LS_CHECK(!cc.valid);
}

LS_CASE(t2t_cc_short_memory_truncates)
{
    uint8_t mem[8] = {0};
    ls_t2t_cc_t cc;
    LS_EQ_INT(ls_t2t_parse_cc(mem, sizeof(mem), &cc), LS_NFC_ERR_TRUNCATED);
}

LS_CASE(t2t_find_ndef_skips_null_and_lock_control)
{
    /* Data area: NULL, NULL, LOCK_CONTROL(len=3, three bytes), NDEF(len=4)
       ["\xd1\x01\x00T"], TERMINATOR. */
    uint8_t mem[64] = {0};
    set_cc(mem, 0xE1, 0x10, 0x06, 0x00);
    size_t o = 16;
    mem[o++] = 0x00;                     /* NULL */
    mem[o++] = 0x00;                     /* NULL */
    mem[o++] = 0x01; mem[o++] = 0x03;
    mem[o++] = 0x11; mem[o++] = 0x22; mem[o++] = 0x33;
    mem[o++] = 0x03; mem[o++] = 0x04;
    mem[o++] = 0xd1; mem[o++] = 0x01; mem[o++] = 0x00; mem[o++] = 'T';
    mem[o++] = 0xFE;                     /* terminator */

    const uint8_t *nd = NULL; size_t nlen = 0;
    int r = ls_t2t_find_ndef(mem, sizeof(mem), 512, &nd, &nlen);
    LS_EQ_INT(r, LS_NFC_ERR_NONE);
    LS_EQ_UINT(nlen, 4);
    LS_CHECK(nd && nd[0] == 0xd1 && nd[3] == 'T');
}

LS_CASE(t2t_find_ndef_three_byte_length)
{
    /* NDEF TLV with length 0xFF using the 3-byte form: 03 FF 01 00 ... */
    uint8_t mem[1024] = {0};
    set_cc(mem, 0xE1, 0x10, 0x40, 0x00);
    size_t o = 16;
    mem[o++] = 0x03; mem[o++] = 0xFF; mem[o++] = 0x01; mem[o++] = 0x00;
    size_t payload_at = o;
    for (int i = 0; i < 0x0100; i++) mem[o++] = (uint8_t)i;
    mem[o++] = 0xFE;

    const uint8_t *nd = NULL; size_t nlen = 0;
    int r = ls_t2t_find_ndef(mem, sizeof(mem), 512, &nd, &nlen);
    LS_EQ_INT(r, LS_NFC_ERR_NONE);
    LS_EQ_UINT(nlen, 0x0100u);
    LS_CHECK(nd == &mem[payload_at]);
}

LS_CASE(t2t_find_ndef_over_max_message_bytes_rejected)
{
    /* 200-byte NDEF message, limit 128. */
    uint8_t mem[400] = {0};
    set_cc(mem, 0xE1, 0x10, 0x20, 0x00);
    size_t o = 16;
    mem[o++] = 0x03; mem[o++] = 200;
    for (int i = 0; i < 200; i++) mem[o++] = 0xAA;
    mem[o++] = 0xFE;

    const uint8_t *nd = NULL; size_t nlen = 0;
    int r = ls_t2t_find_ndef(mem, sizeof(mem), 128, &nd, &nlen);
    LS_EQ_INT(r, LS_NFC_ERR_TOO_LARGE);
}

LS_CASE(t2t_find_ndef_truncated_tlv_reports_truncated)
{
    /* NDEF TLV says length 100 but only 5 bytes follow before end. */
    uint8_t mem[24] = {0};
    set_cc(mem, 0xE1, 0x10, 0x01, 0x00);
    size_t o = 16;
    mem[o++] = 0x03; mem[o++] = 100;
    mem[o++] = 0; mem[o++] = 0; mem[o++] = 0;

    const uint8_t *nd = NULL; size_t nlen = 0;
    int r = ls_t2t_find_ndef(mem, o, 512, &nd, &nlen);
    LS_EQ_INT(r, LS_NFC_ERR_TRUNCATED);
}

LS_CASE(t2t_find_ndef_bad_long_length_rejected)
{
    /* Long form used for a length < 0xFF - malformed per T2T Op Spec. */
    uint8_t mem[32] = {0};
    set_cc(mem, 0xE1, 0x10, 0x02, 0x00);
    size_t o = 16;
    mem[o++] = 0x03; mem[o++] = 0xFF; mem[o++] = 0x00; mem[o++] = 0x0A;
    for (int i = 0; i < 10; i++) mem[o++] = 0;

    const uint8_t *nd = NULL; size_t nlen = 0;
    int r = ls_t2t_find_ndef(mem, o, 512, &nd, &nlen);
    LS_EQ_INT(r, LS_NFC_ERR_MALFORMED);
}

LS_CASE(t2t_find_ndef_missing_ndef_tlv_returns_none)
{
    uint8_t mem[32] = {0};
    set_cc(mem, 0xE1, 0x10, 0x02, 0x00);
    mem[16] = 0xFE;   /* terminator with no NDEF TLV */

    const uint8_t *nd = (const uint8_t *)0x1;
    size_t nlen = 42;
    int r = ls_t2t_find_ndef(mem, sizeof(mem), 512, &nd, &nlen);
    LS_EQ_INT(r, LS_NFC_ERR_NONE);
    LS_CHECK(nd == NULL);
    LS_EQ_UINT(nlen, 0);
}
