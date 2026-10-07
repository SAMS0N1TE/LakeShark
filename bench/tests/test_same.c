#include "ls_test.h"
#include "same.h"
#include "same_store.h"
#include "fm_state.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
static const char header[] = "ZCZC-WXR-TOR-025017-025021+0030-2801430-KBOX/NWS-";
static same_alert_t alert;
static int received, ended;
static void receive(const same_alert_t *a, void *user)
{ (void)user; alert = *a; if (a->ended) ++ended; else ++received; }
static void init_counts(void) { received = ended = 0; }
static unsigned noise_state;
static float noise(void)
{
    noise_state = noise_state * 1664525u + 1013904223u;
    return ((noise_state >> 8) / 8388608.0f - 1);
}
static void silence(same_ctx_t *s)
{ static const int16_t zero[257]; for (int i = 0; i < 63; ++i) same_process(s, zero, 257); }
static void transmit(same_ctx_t *s, const char *text, float hiss, int offset)
{
    int16_t pcm[137]; int used = 0, sample = 0;
    double boundary = offset, phase = 0.73;
    for (int i = 0; i < offset; ++i) { int16_t z = 0; same_process(s, &z, 1); }
    sample = offset;
    for (int i = -16; i < (int)strlen(text); ++i) {
        unsigned byte = i < 0 ? 0xab : (unsigned char)text[i];
        for (int b = 0; b < 8; ++b) {
            double freq = (byte >> b) & 1 ? 2083.333333333 : 1562.5;
            boundary += FM_AUDIO_RATE / (3125.0 / 6.0);
            while (sample < boundary) {
                pcm[used++] = (int16_t)(11000 * sin(phase) + hiss * 11000 * noise());
                phase += 6.283185307179586 * freq / FM_AUDIO_RATE; ++sample;
                if (used == 137) { same_process(s, pcm, used); used = 0; }
            }
        }
    }
    if (used) same_process(s, pcm, used);
    silence(s);
}
LS_CASE(same_parse_and_filter)
{
    LS_CHECK(same_parse(header, &alert));
    LS_EQ_INT(2, alert.location_count); LS_EQ_INT(25017, alert.locations[0]);
    LS_EQ_INT(30, alert.valid_minutes); LS_EQ_INT(280, alert.day);
    LS_EQ_INT(14, alert.hour); LS_EQ_INT(30, alert.minute);
    LS_CHECK(!strcmp("WXR", alert.originator)); LS_CHECK(!strcmp("KBOX/NWS", alert.sender));
    same_options_t o = {.only_mine = true, .fips = {25017}};
    LS_CHECK(same_matches(&alert, &o)); o.fips[0] = 33001; LS_CHECK(!same_matches(&alert, &o));
    alert.locations[0] = 125017; o.fips[0] = 25017; LS_CHECK(same_matches(&alert, &o));
    alert.locations[0] = 25000; LS_CHECK(same_matches(&alert, &o));
    alert.locations[0] = 0; o.fips[0] = 0; LS_CHECK(same_matches(&alert, &o));
    alert.test = true; LS_CHECK(!same_matches(&alert, &o)); o.include_tests = true;
    LS_CHECK(same_matches(&alert, &o));
}
LS_CASE(same_malformed)
{
    const char *bad[] = { "ZCZC-", "NNNN", "ZCZC-XXX-TOR-025017+0030-2801430-KBOX/NWS-",
        "ZCZC-WXR-TOR-02501X+0030-2801430-KBOX/NWS-",
        "ZCZC-WXR-TOR-025017+0060-2801430-KBOX/NWS-",
        "ZCZC-WXR-TOR-025017+0030-0001430-KBOX/NWS-",
        "ZCZC-WXR-TOR-025017+0030-2802430-KBOX/NWS-",
        "ZCZC-WXR-TOR-025017+0030-2801460-KBOX/NWS-",
        "ZCZC-WXR-TOR-025017+0030-2801430-KBOX/NWS-X" };
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) LS_CHECK(!same_parse(bad[i], &alert));
}
LS_CASE(same_afsk_clean_noise_and_end)
{
    for (int noisy = 0; noisy < 2; ++noisy) {
        init_counts(); noise_state = 7;
        same_ctx_t *s = same_create(receive, NULL); LS_CHECK(s != NULL);
        transmit(s, header, noisy ? 0.5f : 0, 11);
        LS_EQ_INT(0, received);
        transmit(s, "ZCZC-WXR-SVR-025017-025021+0030-2801430-KBOX/NWS-", noisy ? 0.5f : 0, 3);
        transmit(s, header, noisy ? 0.5f : 0, 23);
        LS_EQ_INT(1, received); LS_CHECK(!strcmp(header, alert.header));
        transmit(s, "NNNN", noisy ? 0.5f : 0, 7);
        LS_EQ_INT(1, ended); LS_CHECK(alert.ended);
        transmit(s, "NNNN", 0, 0); LS_EQ_INT(1, ended);
        same_destroy(s);
    }
}
LS_CASE(same_vote_and_store)
{
    init_counts(); same_ctx_t *s = same_create(receive, NULL);
    char a[SAME_HEADER_MAX], b[SAME_HEADER_MAX], c[SAME_HEADER_MAX];
    strcpy(a, header); strcpy(b, header); strcpy(c, header);
    a[9] ^= 1; b[15] ^= 2; c[18] ^= 4;
    same_burst(s, a); same_burst(s, b); same_burst(s, c);
    LS_EQ_INT(1, received); LS_CHECK(!strcmp(header, alert.header));
    same_options_t o = {.only_mine = true, .fips = {33001}};
    same_options_set(&o); same_store_receive(&alert, NULL);
    LS_EQ_INT(1, same_store_count()); LS_CHECK(!same_store_notice(&alert));
    LS_CHECK(same_parse(header, &alert)); o.fips[0] = 25017; same_options_set(&o);
    same_store_receive(&alert, NULL); LS_CHECK(same_store_notice(&alert));
    LS_CHECK(!same_store_notice(&alert)); alert.ended = true; same_store_receive(&alert, NULL);
    LS_CHECK(same_store_get(0, &alert)); LS_CHECK(alert.ended);
    same_destroy(s);
}
LS_CASE(same_long_header_and_sample_phase)
{
    char h[SAME_HEADER_MAX];
    strcpy(h, "ZCZC-WXR-FFW-");
    for (int i = 0; i < SAME_LOCATIONS; ++i) {
        char f[8]; snprintf(f, sizeof(f), "%s%06d", i ? "-" : "", 25001 + i);
        strcat(h, f);
    }
    strcat(h, "+0130-3662359-KBOX/NWS-");
    LS_CHECK(same_parse(h, &alert)); LS_EQ_INT(31, alert.location_count);
    for (int offset = 0; offset < 31; ++offset) {
        init_counts(); same_ctx_t *s = same_create(receive, NULL);
        transmit(s, h, 0.3f, offset); transmit(s, h, 0.3f, (offset + 15) % 31);
        LS_EQ_INT(1, received); LS_CHECK(!strcmp(h, alert.header));
        same_destroy(s);
    }
}
