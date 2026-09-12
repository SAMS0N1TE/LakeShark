/* LS_TEST_SOURCES: ${APP}/adsb/mode-s.c ${APP}/adsb/adsb_decode.c ${APP}/adsb/adsb_state.c */
/* A frame that needed a bit repaired does not introduce an aircraft. */

#include "ls_test.h"

#include "adsb_decode.h"
#include "adsb_state.h"
#include "mode-s.h"

#include "audio_events.h"
#include "event_bus.h"
#include "perf.h"
#include "plane_audio.h"

#include <string.h>

/* mode-s.c defines it; mode-s.h does not declare it. */
uint32_t mode_s_checksum(unsigned char *msg, int bits);

/* ---------------------------------------------- what adsb_decode.c reaches -- */

/* Contact events, spoken alerts and throughput counters are other modules'
   business. The decoder and the aircraft table are this test's, and both are
   linked for real. */
void event_bus_publish_contact(evt_kind_t kind, const char *app,
                               const evt_contact_t *c)
{
    (void)kind; (void)app; (void)c;
}

void audio_events_publish(audio_evt_kind_t kind, uint32_t icao,
                          const char *callsign, bool crc_shaky)
{
    (void)kind; (void)icao; (void)callsign; (void)crc_shaky;
}

plane_category_t plane_classify(uint32_t icao, const char *callsign)
{
    (void)icao; (void)callsign;
    return PLANE_UNKNOWN;
}

void perf_count_msg_good(void) { }
void perf_count_msg_bad(void) { }
void perf_count_burst(void) { }
void perf_mark_good_msg(int64_t now_us) { (void)now_us; }
void perf_mark_position(int64_t now_us) { (void)now_us; }
void perf_set_mag(int avg, int peak) { (void)avg; (void)peak; }
void perf_set_active_count(int n) { (void)n; }
int  perf_get_crc_good(void) { return 0; }
int  perf_get_crc_err(void) { return 0; }

/* ---------------------------------------------------------------- frames -- */

#define SENDER 0xAE1234u   /* a US military block address, as a Black Hawk's is */

static const char AIS[] =
    "?ABCDEFGHIJKLMNOPQRSTUVWXYZ????? ???????????????0123456789??????";

/* The parity field: the CRC of everything before it, with `overlay` XOR'd
   in - the address, for a frame whose parity is an AP field; nothing, for an
   extended squitter or an all-call reply to interrogator 0. */
static void seal(uint8_t *m, int bits, uint32_t overlay)
{
    const int n = bits / 8;
    m[n - 3] = m[n - 2] = m[n - 1] = 0;
    const uint32_t crc = mode_s_checksum(m, bits) ^ overlay;
    m[n - 3] = (uint8_t)(crc >> 16);
    m[n - 2] = (uint8_t)(crc >> 8);
    m[n - 1] = (uint8_t)crc;
}

/* DF17 identification, type code 4 and category 7: a rotorcraft, by name. */
static void df17_ident(uint8_t m[14], uint32_t aa, const char cs[8])
{
    memset(m, 0, 14);
    m[0] = (17 << 3) | 5;
    m[1] = (uint8_t)(aa >> 16);
    m[2] = (uint8_t)(aa >> 8);
    m[3] = (uint8_t)aa;
    m[4] = (4 << 3) | 7;
    uint64_t chars = 0;
    for (int i = 0; i < 8; i++) {
        /* From index 1: '?' marks the codes nobody sends. */
        const char *p = strchr(AIS + 1, cs[i]);
        chars = (chars << 6) | (uint64_t)(p ? p - AIS : 32);
    }
    for (int i = 0; i < 6; i++) m[5 + i] = (uint8_t)(chars >> (40 - 8 * i));
    seal(m, 112, 0);
}

/* DF11 all-call reply to interrogator 0: the parity field is the bare CRC. */
static void df11(uint8_t m[14], uint32_t aa)
{
    memset(m, 0, 14);
    m[0] = (11 << 3) | 5;
    m[1] = (uint8_t)(aa >> 16);
    m[2] = (uint8_t)(aa >> 8);
    m[3] = (uint8_t)aa;
    seal(m, 56, 0);
}

static void flip(uint8_t *m, int bit) { m[bit / 8] ^= (uint8_t)(0x80 >> (bit % 8)); }

/* The first five bits are the downlink format. Damage there changes what the
   frame IS - a DF17 with bit 2 flipped is a DF21 - and those frames were
   refused before and are refused now, by the AP path, so every case starts
   after them. */
#define FIRST_BIT_AFTER_DF 5

/* ------------------------------------------------------------ at decoder -- */

static mode_s_t s_ms;   /* 32 KB of address cache - not on the stack */

static bool decode(const uint8_t *frame, uint32_t *aa, int *errorbit)
{
    struct mode_s_msg mm;
    uint8_t buf[MODE_S_LONG_MSG_BYTES];
    memcpy(buf, frame, sizeof(buf));
    mode_s_decode(&s_ms, &mm, buf);
    if (aa)
        *aa = ((uint32_t)mm.aa1 << 16) | ((uint32_t)mm.aa2 << 8) |
              (uint32_t)mm.aa3;
    if (errorbit) *errorbit = mm.errorbit;
    return mm.crcok != 0;
}

LS_CASE(a_repaired_frame_from_an_address_never_heard_clean_is_refused)
{
    /* At the decoder, a frame one bit away from a codeword is exactly what a
       noise phantom looks like, and nothing downstream can tell it from a
       real aircraft's damaged frame - which is why the decision is made here,
       on whether the address has ever been heard without help. */
    uint8_t f17[14], f11[14];
    df17_ident(f17, SENDER, "BLKHK21 ");
    df11(f11, SENDER);

    int tried = 0, accepted = 0, first = -1;
    for (int bit = FIRST_BIT_AFTER_DF; bit < 112; bit++, tried++) {
        uint8_t m[14];
        memcpy(m, f17, sizeof(m));
        flip(m, bit);
        mode_s_init(&s_ms);                 /* a cold cache: never heard */
        if (decode(m, NULL, NULL)) {
            accepted++;
            if (first < 0) first = bit;
        }
    }
    for (int bit = FIRST_BIT_AFTER_DF; bit < 56; bit++, tried++) {
        uint8_t m[14];
        memcpy(m, f11, sizeof(m));
        flip(m, bit);
        mode_s_init(&s_ms);
        if (decode(m, NULL, NULL)) {
            accepted++;
            if (first < 0) first = 112 + bit;
        }
    }
    LS_CHECK_MSG(accepted == 0,
                 "%d of %d one-bit-damaged DF17/DF11 frames from an address "
                 "never heard clean were accepted (first: %s bit %d) - every "
                 "one of them an aircraft in the table",
                 accepted, tried, first >= 112 ? "DF11" : "DF17",
                 first >= 112 ? first - 112 : first);
}

LS_CASE(once_heard_clean_the_same_damage_is_repaired_to_the_sender)
{
    /* What the rule must not cost: a real aircraft's damaged frames. After
       one clean frame from the sender, every one of the same errors is
       repaired - to the sender's address, at the bit that was flipped. */
    uint8_t f17[14], f11[14];
    df17_ident(f17, SENDER, "BLKHK21 ");
    df11(f11, SENDER);

    mode_s_init(&s_ms);
    LS_CHECK(decode(f17, NULL, NULL));      /* heard clean, once */

    int wrong = 0, first = -1;
    for (int bit = FIRST_BIT_AFTER_DF; bit < 112; bit++) {
        uint8_t m[14];
        memcpy(m, f17, sizeof(m));
        flip(m, bit);
        uint32_t aa = 0;
        int eb = -2;
        if (!decode(m, &aa, &eb) || aa != SENDER || eb != bit) {
            wrong++;
            if (first < 0) first = bit;
        }
    }
    for (int bit = FIRST_BIT_AFTER_DF; bit < 56; bit++) {
        uint8_t m[14];
        memcpy(m, f11, sizeof(m));
        flip(m, bit);
        uint32_t aa = 0;
        int eb = -2;
        if (!decode(m, &aa, &eb) || aa != SENDER || eb != bit) {
            wrong++;
            if (first < 0) first = 112 + bit;
        }
    }
    LS_CHECK_MSG(wrong == 0,
                 "%d damaged frames from a sender already heard clean were "
                 "not repaired to it (first: %s bit %d)",
                 wrong, first >= 112 ? "DF11" : "DF17",
                 first >= 112 ? first - 112 : first);
}

/* ------------------------------------------------------------ end to end -- */

/* PPM at 2 MSPS: a pulse is one half-microsecond sample at full scale and a
   bit is two samples, the pulse first for a one and second for a zero. The
   preamble is pulses at 0, 1.0, 3.5 and 4.5 us. 127 is the ADC's zero, so
   the silence around the frame has no magnitude at all and nothing but the
   frame can trigger the detector. */
#define IQ_SAMPLES 1024
#define LEAD       100
#define PULSE      227

static uint8_t s_iq[IQ_SAMPLES * 2];

static void on_air(const uint8_t *frame, int bits)
{
    static const int PREAMBLE[] = { 0, 2, 7, 9 };
    memset(s_iq, 127, sizeof(s_iq));
    for (int i = 0; i < 4; i++) s_iq[2 * (LEAD + PREAMBLE[i])] = PULSE;
    for (int b = 0; b < bits; b++) {
        const bool one = (frame[b / 8] & (0x80 >> (b % 8))) != 0;
        s_iq[2 * (LEAD + 16 + 2 * b + (one ? 0 : 1))] = PULSE;
    }
    adsb_on_sample(s_iq, (int)sizeof(s_iq));
}

static const adsb_aircraft_t *tracked(uint32_t icao)
{
    for (int i = 0; i < ADSB_MAX_TRACKED; i++) {
        const adsb_aircraft_t *a = adsb_state_get(i);
        if (a && a->active && a->icao == icao) return a;
    }
    return NULL;
}

LS_CASE(one_flipped_bit_puts_no_phantom_in_the_aircraft_table)
{
    uint8_t f17[14];
    df17_ident(f17, SENDER, "BLKHK21 ");

    int created = 0, first = -1;
    for (int bit = FIRST_BIT_AFTER_DF; bit < 112; bit++) {
        uint8_t m[14];
        memcpy(m, f17, sizeof(m));
        flip(m, bit);
        adsb_decode_init();                  /* an empty table, a cold cache */
        on_air(m, 112);
        if (adsb_state_active_count() != 0) {
            created++;
            if (first < 0) first = bit;
        }
    }
    LS_CHECK_MSG(created == 0,
                 "%d of %d one-bit-damaged frames from an aircraft never "
                 "heard clean put an entry in the table (first: bit %d)",
                 created, 112 - FIRST_BIT_AFTER_DF, first);

    /* The same aircraft heard properly: one entry, named, and it keeps its
       repaired frames - the second message here is damaged and still counts.
       This half is also what proves the samples above were decodable at
       all, so the empty table is a refusal and not a detector that heard
       nothing. */
    adsb_decode_init();
    on_air(f17, 112);
    LS_EQ_INT(1, adsb_state_active_count());

    uint8_t m[14];
    memcpy(m, f17, sizeof(m));
    flip(m, 60);                             /* inside the callsign */
    on_air(m, 112);
    LS_EQ_INT(1, adsb_state_active_count());

    const adsb_aircraft_t *a = tracked(SENDER);
    LS_CHECK(a != NULL);
    if (a) {
        LS_EQ_INT(2, a->msg_count);
        LS_EQ_STR("BLKHK21", a->callsign);
        /* And it says what it is: type code 4, category 7 - A7, a
           rotorcraft, which is the word the detail page shows. */
        LS_EQ_INT(4, a->emitter_tc);
        LS_EQ_INT(7, a->emitter_ca);
    }
}
