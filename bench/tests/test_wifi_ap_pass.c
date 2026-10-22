/* The file hotspot's password (main/ls_wifi_ap_pass.h): made per board,
   never the old fixed word, only from the readable alphabet, and checked
   before a stored one is used. */
#include "ls_test.h"
#include "ls_wifi_ap_pass.h"

#include <stdint.h>
#include <string.h>

static uint32_t s_state = 0x12345678u;
static uint32_t xorshift(void)
{
    s_state ^= s_state << 13;
    s_state ^= s_state >> 17;
    s_state ^= s_state << 5;
    return s_state;
}

static uint32_t s_seq[4];
static int s_seq_i;
static uint32_t scripted(void) { return s_seq[s_seq_i++ % 4]; }

LS_CASE(made_password_is_valid_and_shaped)
{
    char p[LS_AP_PASS_LEN + 1];
    for (int i = 0; i < 1000; i++) {
        LS_CHECK(ls_ap_pass_make(p, sizeof(p), xorshift));
        LS_CHECK(ls_ap_pass_valid(p));
        LS_EQ_INT((int)strlen(p), 11);
        LS_EQ_INT(p[5], '-');
        LS_CHECK(strcmp(p, "sharkbait") != 0);
        /* WPA2 wants 8..63 characters. */
        LS_CHECK(strlen(p) >= 8 && strlen(p) <= 63);
    }
}

LS_CASE(no_lookalike_letters)
{
    char p[LS_AP_PASS_LEN + 1];
    for (int i = 0; i < 2000; i++) {
        ls_ap_pass_make(p, sizeof(p), xorshift);
        LS_CHECK(!strpbrk(p, "01ilo"));
    }
}

LS_CASE(two_boards_differ)
{
    char a[LS_AP_PASS_LEN + 1], b[LS_AP_PASS_LEN + 1];
    ls_ap_pass_make(a, sizeof(a), xorshift);
    ls_ap_pass_make(b, sizeof(b), xorshift);
    LS_CHECK(strcmp(a, b) != 0);
}

LS_CASE(rejection_skips_the_biased_tail)
{
    /* Words at or above the largest multiple of 31 are thrown away, so the
       first letter comes from the next word. */
    s_seq[0] = UINT32_MAX;          /* rejected                 */
    s_seq[1] = 0;                   /* 'a'                      */
    s_seq[2] = 1;                   /* 'b'                      */
    s_seq[3] = 30;                  /* '9' (last in alphabet)   */
    s_seq_i = 0;
    char p[LS_AP_PASS_LEN + 1];
    LS_CHECK(ls_ap_pass_make(p, sizeof(p), scripted));
    LS_EQ_INT(p[0], 'a');
    LS_EQ_INT(p[1], 'b');
    LS_EQ_INT(p[2], '9');
}

LS_CASE(stored_values_are_checked)
{
    LS_CHECK(!ls_ap_pass_valid(NULL));
    LS_CHECK(!ls_ap_pass_valid(""));
    LS_CHECK(!ls_ap_pass_valid("sharkbait"));
    LS_CHECK(!ls_ap_pass_valid("abcde_fghjk"));
    LS_CHECK(!ls_ap_pass_valid("abcde-fghj1"));   /* 1 is not in the alphabet */
    LS_CHECK(!ls_ap_pass_valid("abcde-fghjkm"));  /* too long */
    LS_CHECK(ls_ap_pass_valid("abcde-fghjk"));
}

LS_CASE(small_buffer_is_refused)
{
    char p[8] = "x";
    LS_CHECK(!ls_ap_pass_make(p, sizeof(p), xorshift));
    LS_CHECK(!ls_ap_pass_make(NULL, 32, xorshift));
}
