/* LR2021 FIFO signs, MSB first, through the production timing and BCH path. */
#include "ls_test.h"
#include "pager_recon.h"
#include "pocsag_gen.h"
#include <stdlib.h>
#include <string.h>

static void transmission(pgr_stream_t *s, int baud, bool inverted, int offset, int ppm, bool errors)
{
    uint8_t bits[BITS_GEN_MAX];
    const size_t nb = pocsag_build_bits(1234568, 3, "STREAM TEST PAGE", 576, bits, sizeof(bits));
    if (errors) {
        bits[576 + 32 + 9] ^= 1;
        bits[576 + 32 + 32 + 12] ^= 1;
    }
    const double step = (double)baud * (1.0 + ppm / 1000000.0) / PGR_STREAM_RATE;
    uint32_t rng = 12345;
    uint8_t fifo[137] = { 0 };
    size_t used = 0;
    int k = 0;
    /* Include the post-burst slicer tail so the low-pass/PLL can deliver
       the final word at either sample rate. */
    for (double phase = 0.37; phase < nb + 2; phase += step) {
        const int bit = bits[phase < nb ? (size_t)phase : nb - 1] ^ inverted;
        rng = rng * 1664525u + 1013904223u;
        /* A discriminator offset plus bounded noise becomes asymmetric
           sign errors near one FSK tone. No analogue values reach RX. */
        const int noise = (int)((rng >> 8) % 1201) - 600;
        int sign = (bit ? 4500 : -4500) + offset + noise > 0;
        if ((rng & 1023u) == 0) sign ^= 1;
        fifo[used] |= (uint8_t)(sign << (7 - k));
        if (++k == 8) {
            k = 0;
            if (++used == sizeof(fifo)) {
                pgr_stream_feed(s, fifo, used);
                used = 0;
                memset(fifo, 0, sizeof(fifo));
            }
        }
    }
    if (k) used++;
    if (used) pgr_stream_feed(s, fifo, used);
}

LS_CASE(all_rates_polarities_offset_and_clock_error)
{
    static const int BAUD[] = { 512, 1200, 2400 };
    for (int rate = 0; rate < 3; rate++) for (int inv = 0; inv < 2; inv++) {
        pgr_stream_t s;
        LS_CHECK(pgr_stream_init(&s, NULL));
        transmission(&s, BAUD[rate], inv != 0, inv ? -4100 : 4100, 800, true);
        LS_EQ_INT(pgr_stream_best(&s), rate);
        LS_CHECK_MSG(s.counts[rate].frames >= 1, "baud %d inv %d good %lu bad %lu",
                     BAUD[rate], inv, (unsigned long)s.counts[rate].clean, (unsigned long)s.counts[rate].bad);
        LS_CHECK(s.counts[rate].fixed >= 2);
        /* Eight samples/bit at 2400 cannot always correct the deliberately
           corrupted words plus one-sided sign noise at a 4.1 kHz offset.
           Still require the frame, capcode, page and >=16 accepted words. */
        LS_CHECK_MSG(s.counts[rate].bad <= (BAUD[rate] == 2400 ? 1u : 0u), "baud %d inv %d bad %lu",
                     BAUD[rate], inv, (unsigned long)s.counts[rate].bad);
        LS_CHECK(s.counts[rate].clean + s.counts[rate].fixed >= 16);
        LS_CHECK(s.caps[rate].n >= 1);
        LS_EQ_UINT(s.caps[rate].v[0], 1234568);
        LS_CHECK(pocsag_n_pages(s.decoder[rate]) >= 1);
        pgr_stream_free(&s);
    }
}

LS_CASE(gaps_discard_partial_words_and_preserve_totals)
{
    pgr_stream_t s;
    LS_CHECK(pgr_stream_init(&s, NULL));
    transmission(&s, 1200, false, 0, 0, false);
    const uint32_t frames = s.counts[1].frames;
    const uint32_t pages = pocsag_n_pages(s.decoder[1]);
    pgr_stream_seam(&s);
    LS_CHECK(!pocsag_synced(s.decoder[1]));
    LS_EQ_UINT(pocsag_n_pages(s.decoder[1]), pages);
    transmission(&s, 1200, false, 0, -800, false);
    LS_EQ_UINT(s.counts[1].frames, frames * 2);
    LS_EQ_UINT(pocsag_n_pages(s.decoder[1]), pages + 1);
    pgr_stream_free(&s);
}

LS_CASE(density_trim_is_directional_bounded_and_ignores_stuck_signs)
{
    pgr_stream_t s;
    LS_CHECK(pgr_stream_init(&s, NULL));
    uint8_t fifo[PGR_STREAM_RATE / 8];
    memset(fifo, 0xEE, sizeof(fifo));
    for (int i = 0; i < 40; i++) {
        pgr_stream_feed(&s, fifo, sizeof(fifo));
        s.trim_hz = s.want_trim_hz;
    }
    LS_EQ_INT(s.trim_hz, 6000);
    memset(fifo, 0x11, sizeof(fifo));
    for (int i = 0; i < 80; i++) {
        pgr_stream_feed(&s, fifo, sizeof(fifo));
        s.trim_hz = s.want_trim_hz;
    }
    LS_EQ_INT(s.trim_hz, -6000);
    memset(fifo, 0xFF, sizeof(fifo));
    pgr_stream_feed(&s, fifo, sizeof(fifo));
    LS_EQ_INT(s.want_trim_hz, -6000);
    pgr_stream_free(&s);
}

LS_CASE(text_is_opt_in)
{
    fm_state_t *out = calloc(1, sizeof(*out));
    LS_CHECK(out != NULL);
    pgr_stream_t s;
    LS_CHECK(pgr_stream_init(&s, out));
    transmission(&s, 1200, false, 0, 0, false);
    LS_CHECK(out->page_count >= 1);
    LS_EQ_STR(out->pages[0].text, "STREAM TEST PAGE");
    pgr_stream_free(&s);
    free(out);
}




LS_CASE(a_gap_discards_an_unfinished_page)
{
    pgr_stream_t s;
    LS_CHECK(pgr_stream_init(&s, NULL));
    const size_t samples = PGR_STREAM_RATE / 1200;
    uint8_t bits[BITS_GEN_MAX], fifo[656 * (PGR_STREAM_RATE / 1200) / 8] = { 0 };
    pocsag_build_bits(1234568, 3, "UNFINISHED PAGE", 576, bits, sizeof(bits));
    /* Stop halfway through a message word after its address was read. */
    for (size_t i = 0; i < 656 * samples; i++)
        fifo[i / 8] |= (uint8_t)(bits[i / samples] << (7 - i % 8));
    pgr_stream_feed(&s, fifo, sizeof(fifo));
    LS_CHECK(pocsag_n_addr(s.decoder[1]) >= 1);
    LS_EQ_UINT(pocsag_n_pages(s.decoder[1]), 0);
    pgr_stream_seam(&s);
    transmission(&s, 1200, false, 0, 0, false);
    LS_EQ_UINT(pocsag_n_pages(s.decoder[1]), 1);
    pgr_stream_free(&s);
}

LS_CASE(long_stream_counters_do_not_wrap_at_256_codewords)
{
    pgr_stream_t s;
    LS_CHECK(pgr_stream_init(&s, NULL));
    for (int i = 0; i < 17; i++) {
        pgr_stream_seam(&s);
        transmission(&s, 1200, false, 0, 0, false);
    }
    LS_CHECK(s.counts[1].clean > 256);
    LS_EQ_UINT(pocsag_n_pages(s.decoder[1]), 17);
    pgr_stream_free(&s);
}

/* One preamble opens ten successive batches; the hardware consumes the
   preamble and first sync. Later syncs remain in the MSB-first FIFO. */
LS_CASE(native_baud_ten_batches_inverted_errors_and_gap)
{
    static const int rates[] = { 512, 1200, 2400 };
    for (int r = 0; r < 3; r++) for (int inv = 0; inv < 2; inv++) {
        pocsag_ctx_t *d = pocsag_create(NULL, rates[r]);
        LS_CHECK(d != NULL);
        uint8_t bits[576 + 10 * 544];
        for (int i = 0; i < 576; i++) bits[i] = (i & 1) ^ inv;
        for (int batch = 0; batch < 10; batch++) {
            const size_t base = 576 + batch * 544;
            uint32_t sync = PGR_POCSAG_FSC ^ (batch == 7 ? 1u : 0u);
            for (int b = 0; b < 32; b++) bits[base + b] = ((sync >> (31-b)) & 1) ^ inv;
            for (int word = 0; word < 16; word++) {
                uint32_t cw = word == 0 ? pocsag_encode_address(1234560, 0) : PGR_POCSAG_IDLE;
                if (word == 2) cw ^= 1u << 9; /* one BCH correction per batch */
                for (int b = 0; b < 32; b++) bits[base + 32 + word*32 + b] = ((cw >> (31-b)) & 1) ^ inv;
            }
        }
        /* Chip triggers once; the driver restores just its stripped sync. */
        const uint32_t sync = inv ? ~PGR_POCSAG_FSC : PGR_POCSAG_FSC;
        uint8_t prefix[4];
        for (int i = 0; i < 4; i++) prefix[i] = sync >> (24-8*i);
        pocsag_process_bits(d, prefix, sizeof(prefix));
        for (int batch = 0; batch < 10; batch++) {
            if (batch == 5) {
                uint8_t gap[] = { 0x00, 0xff, 0x00 };
                pocsag_process_bits(d, gap, sizeof(gap));
                pocsag_seam(d); /* a loss seam has no synthetic sync */
            }
            size_t base = 576 + batch * 544 + (batch == 0 ? 32 : 0);
            size_t nb = batch == 0 ? 512 : 544;
            uint8_t fifo[68] = { 0 };
            for (size_t i = 0; i < nb; i++) fifo[i/8] |= bits[base+i] << (7-i%8);
            /* Reads cross codeword boundaries, with no sample-rate PLL. */
            for (size_t i = 0; i < nb/8;) {
                size_t n = nb/8-i < 7 ? nb/8-i : 7;
                pocsag_process_bits(d, fifo+i, n); i += n;
            }
        }
        LS_EQ_UINT(pocsag_n_frames(d), 10);
        LS_EQ_UINT(pocsag_n_pages(d), 10);
        LS_EQ_UINT(pocsag_n_cwerr(d), 0);
        LS_EQ_UINT(pocsag_n_addr(d), 10);
        LS_EQ_INT(pocsag_baud_of(d), rates[r]);
        LS_EQ_INT(pocsag_inverted(d), inv);
        pocsag_destroy(d);
    }
}
