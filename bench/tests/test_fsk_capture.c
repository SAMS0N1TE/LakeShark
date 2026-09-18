/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/ls_fsk_capture.c ${FW}/components/lakeshark/radio/ls_sub_fsk.c */
/* Capture, fold, and render back out as a file a Flipper will replay. */

#include "ls_test.h"
#include "ls_fsk_capture.h"
#include "ls_sub_fsk.h"

#include <stdlib.h>
#include <string.h>

static ls_fsk_capture_t frame(uint32_t hz, const char *bytes, int len,
                              int64_t when)
{
    ls_fsk_capture_t c;
    memset(&c, 0, sizeof(c));
    c.freq_hz = hz;
    c.bitrate = 2400;
    c.deviation_hz = 18500;
    c.bandwidth_hz = 46900;
    c.sync_word = 0xD391D391u;
    c.preamble_bits = 64;
    c.len = (uint8_t)len;
    memcpy(c.data, bytes, (size_t)len);
    c.rssi_dbm = -70.0f;
    c.first_us = when;
    return c;
}

LS_CASE(a_transmitter_on_a_rhythm_is_one_entry_with_a_count)
{
    ls_fsk_capture_reset();

    /* The same frame every 3.0 s, which is what a beacon looks like. A ring
       that filed sixteen copies of it would be full of one transmitter and
       would have evicted everything else worth seeing. */
    for (int i = 0; i < 20; i++) {
        ls_fsk_capture_t c = frame(433394300u, "\x02\x01", 2,
                                   (int64_t)i * 3000000);
        LS_CHECK(ls_fsk_capture_add(&c) == 0);
    }
    LS_CHECK_MSG(ls_fsk_capture_count() == 1,
                 "a repeating frame took %d slots", ls_fsk_capture_count());

    ls_fsk_capture_t got;
    LS_CHECK(ls_fsk_capture_get(0, &got));
    LS_CHECK_MSG(got.heard == 20, "counted %lu", (unsigned long)got.heard);

    /* And it names its own period. */
    const uint32_t ms = ls_fsk_capture_period_ms(0);
    LS_CHECK_MSG(ms == 3000, "period read as %lu ms, not 3000",
                 (unsigned long)ms);
    LS_CHECK_MSG(ls_fsk_capture_evicted() == 0, "it evicted something");
}

LS_CASE(the_same_bytes_on_a_different_channel_are_a_different_capture)
{
    ls_fsk_capture_reset();
    ls_fsk_capture_t a = frame(433394300u, "\x02\x01", 2, 0);
    ls_fsk_capture_t b = frame(433594100u, "\x02\x01", 2, 1000);
    LS_CHECK(ls_fsk_capture_add(&a) == 0);
    LS_CHECK(ls_fsk_capture_add(&b) == 1);
    /* Folding these together would produce a capture that replays onto a
       channel it was never heard on. */
    LS_CHECK_MSG(ls_fsk_capture_count() == 2,
                 "two channels folded into %d entry", ls_fsk_capture_count());
}

LS_CASE(a_full_ring_drops_the_oldest_and_says_so)
{
    ls_fsk_capture_reset();
    for (int i = 0; i < LS_FSK_CAPTURE_MAX + 4; i++) {
        char bytes[2] = {(char)i, 0x01};
        ls_fsk_capture_t c = frame(433394300u, bytes, 2, (int64_t)i * 1000);
        LS_CHECK(ls_fsk_capture_add(&c) >= 0);
    }
    LS_CHECK(ls_fsk_capture_count() == LS_FSK_CAPTURE_MAX);
    LS_CHECK_MSG(ls_fsk_capture_evicted() == 4, "evicted %lu, expected 4",
                 (unsigned long)ls_fsk_capture_evicted());

    /* Index 0 is now the fifth frame, not the first. */
    ls_fsk_capture_t got;
    LS_CHECK(ls_fsk_capture_get(0, &got));
    LS_CHECK_MSG(got.data[0] == 4, "oldest kept is %02X", got.data[0]);
}

LS_CASE(a_short_capture_never_replays_its_neighbours_leftovers)
{
    ls_fsk_capture_reset();
    ls_fsk_capture_t c;
    memset(&c, 0xEE, sizeof(c));       /* a caller's dirty stack struct */
    c.freq_hz = 433394300u;
    c.bitrate = 2400;
    c.deviation_hz = 18500;
    c.bandwidth_hz = 46900;
    c.sync_word = 0xD391D391u;
    c.preamble_bits = 64;
    c.len = 2;
    c.data[0] = 0x02; c.data[1] = 0x01;
    c.first_us = 0;
    c.heard = 0;
    LS_CHECK(ls_fsk_capture_add(&c) == 0);

    ls_fsk_capture_t got;
    LS_CHECK(ls_fsk_capture_get(0, &got));
    for (int i = got.len; i < LS_FSK_CAPTURE_BYTES; i++)
        LS_CHECK_MSG(got.data[i] == 0, "byte %d past the frame is %02X",
                     i, got.data[i]);

    LS_CHECK_MSG(ls_fsk_capture_add(NULL) == -1, "a null capture was filed");
    c.len = 0;
    LS_CHECK_MSG(ls_fsk_capture_add(&c) == -1, "an empty capture was filed");
    c.len = LS_FSK_CAPTURE_BYTES + 1;
    LS_CHECK_MSG(ls_fsk_capture_add(&c) == -1, "an oversized capture was filed");
}

LS_CASE(cc1101_deviation_encoding_matches_the_datasheet)
{
    /* f_dev = f_xosc / 2^17 * (8 + M) * 2^E, with a 26 MHz crystal. Two
       values worth pinning because they are the ones anybody comparing
       against a working preset will have in front of them:
         0x24 -> E=2, M=4 -> 198.36 * 12 * 4  =  9521 Hz
         0x34 -> E=3, M=4 -> 198.36 * 12 * 8  = 19043 Hz */
    uint32_t hz = 0;
    LS_CHECK_MSG(ls_cc1101_deviatn(9521, &hz) == 0x24,
                 "9521 Hz encoded as %02X", ls_cc1101_deviatn(9521, NULL));
    LS_CHECK_MSG(hz == 9521, "0x24 reported as %lu Hz", (unsigned long)hz);

    LS_CHECK_MSG(ls_cc1101_deviatn(19043, &hz) == 0x34,
                 "19043 Hz encoded as %02X", ls_cc1101_deviatn(19043, NULL));
    LS_CHECK_MSG(hz == 19043, "0x34 reported as %lu Hz", (unsigned long)hz);

    /* And an arbitrary ask lands near enough to matter. A receiver's capture
       window is tens of kHz; being inside a few percent is the requirement. */
    for (uint32_t want = 2000; want <= 100000; want += 1500) {
        (void)ls_cc1101_deviatn(want, &hz);
        const uint32_t err = hz > want ? hz - want : want - hz;
        LS_CHECK_MSG(err * 100 <= want * 12,
                     "%lu Hz encoded to %lu Hz, %lu%% out",
                     (unsigned long)want, (unsigned long)hz,
                     (unsigned long)(err * 100 / want));
    }
}

LS_CASE(cc1101_data_rate_encoding_lands_on_the_common_rates)
{
    /* rate = ((256 + M) * 2^E) * f_xosc / 2^28 */
    static const uint32_t RATES[] = {600, 1200, 2400, 4800, 9600, 38400};
    for (size_t i = 0; i < sizeof(RATES) / sizeof(RATES[0]); i++) {
        uint8_t e, m;
        uint32_t got;
        ls_cc1101_drate(RATES[i], &e, &m, &got);
        const uint32_t err = got > RATES[i] ? got - RATES[i] : RATES[i] - got;
        LS_CHECK_MSG(err * 200 <= RATES[i],
                     "%lu baud encoded to %lu (E=%u M=%u)",
                     (unsigned long)RATES[i], (unsigned long)got, e, m);
        LS_CHECK_MSG(e < 16, "DRATE_E %u does not fit its nibble", e);
    }
}

LS_CASE(the_bit_stream_is_preamble_then_sync_then_payload)
{
    ls_fsk_capture_t c = frame(433394300u, "\x02\x01", 2, 0);
    uint8_t bits[512];
    const size_t n = ls_fsk_bitstream(&c, bits, sizeof(bits));
    LS_CHECK_MSG(n == 64 + 32 + 16, "stream is %u bits, expected 112",
                 (unsigned)n);

    /* Alternating, starting high - 0xAA on a byte boundary. */
    for (size_t i = 0; i < 64; i++)
        LS_CHECK_MSG(bits[i] == (i & 1 ? 0 : 1), "preamble bit %u", (unsigned)i);

    /* Then the sync word, MSB first. */
    for (int i = 0; i < 32; i++)
        LS_CHECK_MSG(bits[64 + i] == ((c.sync_word >> (31 - i)) & 1u),
                     "sync bit %d", i);

    /* Then the payload, MSB first, no encoding. */
    LS_CHECK(bits[96 + 6] == 1 && bits[96 + 7] == 0);   /* 0x02 */

    /* A buffer that cannot hold it gets nothing, not a partial stream. */
    LS_CHECK_MSG(ls_fsk_bitstream(&c, bits, 8) == 0,
                 "a short buffer produced a truncated stream");
}

LS_CASE(raw_data_run_lengths_do_not_drift_along_the_frame)
{
    /* Eight ones then eight zeros at 2400 baud: two runs, each 8 bit
       periods. Rounding per bit would give 8 * 417 = 3336 us; rounding per
       run gives 3333. The difference is one bit in 250, which over a long
       frame walks a receiver's clock recovery off the end. */
    uint8_t bits[16];
    for (int i = 0; i < 8; i++)  bits[i] = 1;
    for (int i = 8; i < 16; i++) bits[i] = 0;

    int32_t raw[8];
    const size_t n = ls_fsk_raw_data(bits, 16, 2400, raw, 8);
    LS_CHECK_MSG(n == 2, "%u runs, expected 2", (unsigned)n);
    LS_CHECK_MSG(raw[0] == 3333, "high run is %ld us", (long)raw[0]);
    LS_CHECK_MSG(raw[1] == -3333, "low run is %ld us", (long)raw[1]);

    /* Total duration is the frame's real length, which is the property that
       actually has to hold. */
    long total = 0;
    for (size_t i = 0; i < n; i++) total += raw[i] < 0 ? -raw[i] : raw[i];
    LS_CHECK_MSG(total == 6666, "16 bits at 2400 baud came to %ld us", total);

    LS_CHECK_MSG(ls_fsk_raw_data(bits, 16, 2400, raw, 1) == 0,
                 "a short buffer produced a truncated run list");
}

LS_CASE(a_rendered_file_is_one_a_flipper_will_load)
{
    ls_fsk_capture_reset();
    ls_fsk_capture_t c = frame(433394300u, "\x02\x01\x00\x01", 4, 0);

    char out[8192];
    const size_t n = ls_sub_fsk_render(&c, "bench", out, sizeof(out));
    LS_CHECK_MSG(n > 0 && n < sizeof(out), "render wanted %u bytes",
                 (unsigned)n);

    /* The four lines the SubGhz app parses before it will touch the data. */
    LS_CHECK_MSG(strstr(out, "Filetype: Flipper SubGhz RAW File\n") == out,
                 "the file does not start with the Flipper magic line");
    LS_CHECK(strstr(out, "\nVersion: 1\n"));
    LS_CHECK(strstr(out, "\nFrequency: 433394300\n"));
    LS_CHECK(strstr(out, "\nProtocol: RAW\n"));
    LS_CHECK(strstr(out, "\nRAW_Data:"));

    /* A frequency-keyed frame saved under the stock OOK preset replays as
       silence, so the preset has to be the custom one. */
    LS_CHECK_MSG(strstr(out, "Preset: FuriHalSubGhzPresetCustom\n"),
                 "not saved under a custom preset");
    LS_CHECK_MSG(!strstr(out, "Ook650"), "saved under an OOK preset");
    LS_CHECK(strstr(out, "Custom_preset_module: CC1101\n"));

    /* IOCFG0 = 0x0D puts serial data on GDO0 and PKTCTRL0 = 0x32 is
       asynchronous serial with no length limit. Without both of those the
       Flipper keys a pin the radio is not listening to. */
    LS_CHECK_MSG(strstr(out, "Custom_preset_data: 02 0D"),
                 "GDO0 is not configured as serial data out");
    LS_CHECK_MSG(strstr(out, " 08 32 "), "not in asynchronous serial mode");
    LS_CHECK_MSG(strstr(out, " 12 04 "), "not configured for 2-FSK");

    /* And the deviation it was heard at, encoded: 18500 Hz is 0x34. */
    LS_CHECK_MSG(strstr(out, " 15 34 ") || strstr(out, " 15 34\n"),
                 "the capture's deviation did not reach the preset");

    /* Truncation reports itself the way snprintf does rather than writing a
       short file that looks whole. */
    char small[64];
    const size_t wanted = ls_sub_fsk_render(&c, "bench", small, sizeof(small));
    LS_CHECK_MSG(wanted == n, "a small buffer reported %u, not %u",
                 (unsigned)wanted, (unsigned)n);
    LS_CHECK_MSG(small[sizeof(small) - 1] == '\0', "truncation left no NUL");
}

LS_CASE(a_frame_survives_the_trip_through_edge_timings_and_back)
{
    /* This is the property the archive rests on. A demodulated capture is
       stored as edge timings, because that is what the archive, its on-card
       format, the preview strip and the .sub export already speak - so the
       bytes have to come back out of those timings exactly. A round trip
       that loses a bit stores a capture that replays as noise, and the only
       place that would show up is on air.

       Every payload length, so an off-by-one in the preamble or sync skip
       cannot hide behind one convenient size. */
    for (int len = 1; len <= 16; len++) {
        ls_fsk_capture_t c;
        memset(&c, 0, sizeof(c));
        c.freq_hz = 433394300u;
        c.bitrate = 2400;
        c.deviation_hz = 18500;
        c.bandwidth_hz = 46900;
        c.sync_word = 0xD391D391u;
        c.preamble_bits = 64;
        c.len = (uint8_t)len;
        for (int i = 0; i < len; i++) c.data[i] = (uint8_t)(0xA7 * (i + 1));

        uint8_t bits[2048];
        const size_t n_bits = ls_fsk_bitstream(&c, bits, sizeof(bits));
        LS_CHECK_MSG(n_bits, "len %d produced no bit stream", len);

        int32_t raw[2048];
        const size_t n_raw = ls_fsk_raw_data(bits, n_bits, c.bitrate, raw,
                                             sizeof(raw) / sizeof(raw[0]));
        LS_CHECK_MSG(n_raw, "len %d produced no run list", len);

        uint8_t back[64];
        const size_t n_back = ls_fsk_payload_from_raw(raw, n_raw, c.bitrate,
                                                      c.preamble_bits, back,
                                                      sizeof(back));
        LS_CHECK_MSG(n_back == (size_t)len,
                     "len %d came back as %u bytes", len, (unsigned)n_back);
        for (int i = 0; i < len; i++)
            LS_CHECK_MSG(back[i] == c.data[i],
                         "len %d byte %d: %02X out, %02X back",
                         len, i, c.data[i], back[i]);
    }
}

LS_CASE(bits_come_back_out_of_the_timings_exactly)
{
    /* The bit level too, not just the packed bytes - a run miscounted by one
       shifts everything after it, and at the byte level that can still look
       plausible. */
    uint8_t bits[256];
    for (size_t i = 0; i < 100; i++) bits[i] = (uint8_t)((i * i / 7) & 1u);

    int32_t raw[256];
    const size_t n_raw = ls_fsk_raw_data(bits, 100, 2400, raw, 256);
    LS_CHECK(n_raw);

    uint8_t back[256];
    const size_t n_back = ls_fsk_bits_from_raw(raw, n_raw, 2400, back, 256);
    LS_CHECK_MSG(n_back == 100, "100 bits came back as %u", (unsigned)n_back);
    for (size_t i = 0; i < 100; i++)
        LS_CHECK_MSG(back[i] == bits[i], "bit %u flipped", (unsigned)i);

    /* A run list that cannot be a frame is refused rather than packed into
       bytes that look like one. */
    LS_CHECK_MSG(ls_fsk_payload_from_raw(raw, n_raw, 2400, 64, back,
                                         sizeof(back)) == 0,
                 "a stream shorter than its own preamble reported a payload");

    int32_t zero = 0;
    LS_CHECK_MSG(ls_fsk_bits_from_raw(&zero, 1, 2400, back, 256) == 0,
                 "a zero-length run was accepted");
    LS_CHECK_MSG(ls_fsk_bits_from_raw(raw, n_raw, 2400, back, 4) == 0,
                 "a short buffer produced a truncated stream");
}

/* The Flipper reads a fixed eight PA-table bytes from past the 00 00 that
   ends the register pairs, and refuses the whole file - "Custom_preset_data
   size error" - if it is handed fewer. A capture saved with a short table is
   one that will not replay, which is the only thing the file is for. */
LS_CASE(sub_preset_carries_a_whole_pa_table)
{
    ls_fsk_capture_t cap;
    memset(&cap, 0, sizeof(cap));
    cap.freq_hz = 433920000u;
    cap.bitrate = 2400u;
    cap.deviation_hz = 18500u;
    cap.bandwidth_hz = 46900u;
    cap.sync_word = 0xD391D391u;
    cap.preamble_bits = 64;
    cap.data[0] = 0xA5; cap.data[1] = 0x5A;
    cap.len = 2;

    static char text[65536];
    const size_t want = ls_sub_fsk_render(&cap, "pa table", text, sizeof(text));
    LS_CHECK(want > 0 && want < sizeof(text));

    const char *line = strstr(text, "Custom_preset_data: ");
    LS_CHECK(line != NULL);
    line += strlen("Custom_preset_data: ");
    const char *eol = strchr(line, '\n');
    LS_CHECK(eol != NULL);

    /* Count the bytes, and where the 00 00 terminator falls. */
    int total = 0, terminator = -1, prev_zero = 0;
    for (const char *p = line; p < eol; ) {
        while (p < eol && *p == ' ') p++;
        if (p >= eol) break;
        const int v = (int)strtol(p, NULL, 16);
        if (v == 0 && prev_zero && terminator < 0) terminator = total - 1;
        prev_zero = (v == 0);
        total++;
        while (p < eol && *p != ' ') p++;
    }

    LS_CHECK_MSG(terminator >= 0, "no 00 00 terminator in the preset");
    /* Everything after the terminator's two bytes is the PA table. */
    LS_EQ_INT(total - (terminator + 2), 8);
}

/* One renderer, and it has to refuse rather than fall back.

   There were three of these in three files and they drifted: two carried a
   one-byte PA table the Flipper rejects outright, and the sampling-clock fix
   reached only the third. What matters now is that the single one says the
   right thing for each kind of capture and says nothing at all for a capture
   it cannot express. */
LS_CASE(preset_text_matches_the_capture_or_refuses)
{
    char out[320];

    /* Nothing recorded: an edge-timed capture, which is the stock preset every
       Flipper already has. */
    size_t n = ls_sub_preset_text(NULL, out, sizeof(out));
    LS_CHECK(n > 0 && n < sizeof(out));
    LS_CHECK(strstr(out, "FuriHalSubGhzPresetOok650Async") != NULL);
    LS_CHECK(strstr(out, "Custom_preset") == NULL);

    /* A rate but no deviation is still amplitude keying. */
    ls_sub_mod_t ook = { 2400, 0 };
    n = ls_sub_preset_text(&ook, out, sizeof(out));
    LS_CHECK(n > 0 && strstr(out, "Ook650Async") != NULL);

    /* A deviation makes it frequency keyed and needs a built preset. */
    ls_sub_mod_t fsk = { 2400, 18500 };
    n = ls_sub_preset_text(&fsk, out, sizeof(out));
    LS_CHECK(n > 0 && n < sizeof(out));
    LS_CHECK(strstr(out, "FuriHalSubGhzPresetCustom") != NULL);
    LS_CHECK(strstr(out, "Custom_preset_module: CC1101") != NULL);
    LS_CHECK(strstr(out, "Custom_preset_data:") != NULL);

    /* The sampling clock is one DRATE_E step above the capture's own rate.
       2400 baud encodes as MDMCFG4 0x66; twice that is 0x67, and a frame
       written with 0x66 is heard loudly and decodes as nothing. */
    LS_CHECK_MSG(strstr(out, " 10 67 ") != NULL,
                 "MDMCFG4 should carry twice the capture's rate");

    /* Outside what the radio can be given, it refuses - it does not quietly
       write the amplitude preset over a frequency-keyed capture. */
    ls_sub_mod_t too_slow = { 100, 18500 };
    LS_EQ_UINT(ls_sub_preset_text(&too_slow, out, sizeof(out)), 0u);
    ls_sub_mod_t too_wide = { 2400, 400000 };
    LS_EQ_UINT(ls_sub_preset_text(&too_wide, out, sizeof(out)), 0u);

    /* snprintf semantics, so a caller sizing a buffer gets a straight answer. */
    char small[8];
    LS_CHECK(ls_sub_preset_text(&fsk, small, sizeof(small)) > sizeof(small));
}
