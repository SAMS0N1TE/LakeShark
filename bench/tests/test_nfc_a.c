/* LS_TEST_SOURCES: ${FW}/components/lakeshark/nfc/nfc_a.c */
/* LS_TEST_INCLUDE: ${FW}/components/lakeshark/nfc */
/* NFC-A anticollision cascade assembly.  ISO/IEC 14443-3 §6.5.4.

   The three UID lengths are exercised end-to-end, plus the failure
   shapes: bad BCC, wrong cascade tag placement, and a SAK whose cascade
   bit disagrees with the response's cascade tag. */

#include "ls_test.h"
#include "ls_nfc.h"
#include "nfc_a.h"

#include <string.h>

static void fill_sel(ls_nfca_sel_t *s, uint8_t b0, uint8_t b1,
                     uint8_t b2, uint8_t b3)
{
    s->bytes[0] = b0; s->bytes[1] = b1; s->bytes[2] = b2; s->bytes[3] = b3;
    s->bytes[4] = (uint8_t)(b0 ^ b1 ^ b2 ^ b3);
}

LS_CASE(nfca_4byte_uid_single_cascade)
{
    ls_nfca_sel_t r; fill_sel(&r, 0x04, 0xE1, 0x5C, 0x11);
    uint8_t sak = 0x08;   /* cascade bit clear, MIFARE UL SAK */

    uint8_t uid[LS_NFC_UID_MAX]; uint8_t len = 0, final_sak = 0;
    int rc = ls_nfca_assemble_uid(&r, &sak, 1, uid, &len, &final_sak);
    LS_EQ_INT(rc, LS_NFC_ERR_NONE);
    LS_EQ_UINT(len, 4);
    LS_EQ_UINT(uid[0], 0x04);
    LS_EQ_UINT(uid[3], 0x11);
    LS_EQ_UINT(final_sak, 0x08);
}

LS_CASE(nfca_7byte_uid_two_cascades_drops_cascade_tag)
{
    ls_nfca_sel_t r[2];
    fill_sel(&r[0], 0x88, 0x04, 0x11, 0x22);   /* CT + first 3 UID */
    fill_sel(&r[1], 0x33, 0x44, 0x55, 0x66);   /* last 4 UID */
    uint8_t saks[2] = { 0x04, 0x00 };          /* cascade set, then clear */

    uint8_t uid[LS_NFC_UID_MAX]; uint8_t len = 0, final_sak = 0;
    int rc = ls_nfca_assemble_uid(r, saks, 2, uid, &len, &final_sak);
    LS_EQ_INT(rc, LS_NFC_ERR_NONE);
    LS_EQ_UINT(len, 7);
    /* CT (0x88) must be dropped.  UID starts with the second byte of
       level-1's response and continues through level-2. */
    uint8_t expected[7] = { 0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    LS_CHECK(memcmp(uid, expected, 7) == 0);
    LS_EQ_UINT(final_sak, 0x00);
}

LS_CASE(nfca_10byte_uid_three_cascades)
{
    ls_nfca_sel_t r[3];
    fill_sel(&r[0], 0x88, 0x01, 0x02, 0x03);
    fill_sel(&r[1], 0x88, 0x04, 0x05, 0x06);
    fill_sel(&r[2], 0x07, 0x08, 0x09, 0x0A);
    uint8_t saks[3] = { 0x04, 0x04, 0x20 };

    uint8_t uid[LS_NFC_UID_MAX]; uint8_t len = 0, final_sak = 0;
    int rc = ls_nfca_assemble_uid(r, saks, 3, uid, &len, &final_sak);
    LS_EQ_INT(rc, LS_NFC_ERR_NONE);
    LS_EQ_UINT(len, 10);
    uint8_t expected[10] = { 1,2,3,4,5,6,7,8,9,10 };
    LS_CHECK(memcmp(uid, expected, 10) == 0);
    LS_EQ_UINT(final_sak, 0x20);
}

LS_CASE(nfca_bad_bcc_rejected)
{
    ls_nfca_sel_t r; fill_sel(&r, 0x04, 0xE1, 0x5C, 0x11);
    r.bytes[4] ^= 0xFF;   /* corrupt the BCC */
    uint8_t sak = 0x08;
    uint8_t uid[LS_NFC_UID_MAX]; uint8_t len = 0, final_sak = 0;
    int rc = ls_nfca_assemble_uid(&r, &sak, 1, uid, &len, &final_sak);
    LS_EQ_INT(rc, LS_NFC_ERR_MALFORMED);
}

LS_CASE(nfca_cascade_tag_on_last_level_rejected)
{
    /* Two levels, both carry the cascade tag - the last must not. */
    ls_nfca_sel_t r[2];
    fill_sel(&r[0], 0x88, 0x04, 0x11, 0x22);
    fill_sel(&r[1], 0x88, 0x33, 0x44, 0x55);
    uint8_t saks[2] = { 0x04, 0x04 };
    uint8_t uid[LS_NFC_UID_MAX]; uint8_t len = 0, final_sak = 0;
    int rc = ls_nfca_assemble_uid(r, saks, 2, uid, &len, &final_sak);
    LS_EQ_INT(rc, LS_NFC_ERR_MALFORMED);
}

LS_CASE(nfca_missing_cascade_tag_on_middle_level_rejected)
{
    /* Level 1 without CT but we say 2 levels - inconsistent. */
    ls_nfca_sel_t r[2];
    fill_sel(&r[0], 0x11, 0x22, 0x33, 0x44);
    fill_sel(&r[1], 0x55, 0x66, 0x77, 0x88);
    uint8_t saks[2] = { 0x04, 0x00 };
    uint8_t uid[LS_NFC_UID_MAX]; uint8_t len = 0, final_sak = 0;
    int rc = ls_nfca_assemble_uid(r, saks, 2, uid, &len, &final_sak);
    LS_EQ_INT(rc, LS_NFC_ERR_MALFORMED);
}

LS_CASE(nfca_sak_cascade_bit_disagrees_rejected)
{
    /* CT present but SAK cascade bit clear. */
    ls_nfca_sel_t r[2];
    fill_sel(&r[0], 0x88, 0x04, 0x11, 0x22);
    fill_sel(&r[1], 0x33, 0x44, 0x55, 0x66);
    uint8_t saks[2] = { 0x00, 0x00 };
    uint8_t uid[LS_NFC_UID_MAX]; uint8_t len = 0, final_sak = 0;
    int rc = ls_nfca_assemble_uid(r, saks, 2, uid, &len, &final_sak);
    LS_EQ_INT(rc, LS_NFC_ERR_MALFORMED);
}

LS_CASE(nfca_out_of_range_levels_rejected)
{
    ls_nfca_sel_t r; fill_sel(&r, 0x04, 0x11, 0x22, 0x33);
    uint8_t sak = 0x00;
    uint8_t uid[LS_NFC_UID_MAX]; uint8_t len = 0, final_sak = 0;
    LS_EQ_INT(ls_nfca_assemble_uid(&r, &sak, 0, uid, &len, &final_sak),
              LS_NFC_ERR_MALFORMED);
    LS_EQ_INT(ls_nfca_assemble_uid(&r, &sak, 4, uid, &len, &final_sak),
              LS_NFC_ERR_MALFORMED);
}
