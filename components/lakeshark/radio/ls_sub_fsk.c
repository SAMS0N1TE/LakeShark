/* See ls_sub_fsk.h. */

#include "ls_sub_fsk.h"
#include "esp_attr.h"

/* These scratch buffers are ~7.8 KB and were plain statics, which puts them
   in internal .bss. This board has about 8 KB of internal heap left by the
   time the display comes up, and taking that much of it dropped the largest
   DMA-capable block to 176 bytes - too small for the panel path to
   allocate, so nothing painted and the screen sat on its test pattern.
   PSRAM is where a kilobyte-scale scratch buffer belongs. */

#include <stdio.h>
#include <string.h>

uint8_t ls_cc1101_deviatn(uint32_t deviation_hz, uint32_t *actual_hz)
{
    /* Search rather than solve: there are 24 encodable values and picking
       the nearest by inspection is both exact and obviously correct, which
       is worth more here than closed form. */
    uint8_t best = 0;
    uint32_t best_hz = 0;
    uint64_t best_err = (uint64_t)-1;

    for (uint8_t e = 0; e < 8; e++) {
        for (uint8_t m = 0; m < 8; m++) {
            /* Rounded, not truncated. 0x34 is 19042.97 Hz and every
               datasheet and working preset in circulation calls it
               19043; a driver that reports 19042 disagrees with every
               reference the reader has to hand, over half a hertz. */
            const uint64_t exact = (uint64_t)LS_CC1101_XOSC_HZ * (8u + m)
                                   * (1u << e);
            const uint64_t hz = (exact + (1u << 16)) >> 17;
            const uint64_t err = hz > deviation_hz ? hz - deviation_hz
                                                   : deviation_hz - hz;
            if (err >= best_err) continue;
            best_err = err;
            best = (uint8_t)((e << 4) | m);
            best_hz = (uint32_t)hz;
        }
    }
    if (actual_hz) *actual_hz = best_hz;
    return best;
}

void ls_cc1101_drate(uint32_t bitrate, uint8_t *drate_e, uint8_t *drate_m,
                     uint32_t *actual)
{
    uint8_t best_e = 0, best_m = 0;
    uint32_t best_rate = 0;
    uint64_t best_err = (uint64_t)-1;

    for (uint8_t e = 0; e < 16; e++) {
        for (uint32_t m = 0; m < 256; m++) {
            const uint64_t rate = ((uint64_t)(256u + m) * (1u << e)
                                   * LS_CC1101_XOSC_HZ) >> 28;
            const uint64_t err = rate > bitrate ? rate - bitrate
                                                : bitrate - rate;
            if (err >= best_err) continue;
            best_err = err;
            best_e = e;
            best_m = (uint8_t)m;
            best_rate = (uint32_t)rate;
        }
    }
    if (drate_e) *drate_e = best_e;
    if (drate_m) *drate_m = best_m;
    if (actual)  *actual = best_rate;
}

size_t ls_fsk_bitstream(const ls_fsk_capture_t *capture, uint8_t *bits,
                        size_t max_bits)
{
    if (!capture || !bits || !capture->len) return 0;

    const size_t want = (size_t)capture->preamble_bits + 32 +
                        (size_t)capture->len * 8;
    if (want > max_bits) return 0;

    size_t n = 0;
    for (uint16_t i = 0; i < capture->preamble_bits; i++)
        bits[n++] = (uint8_t)((i & 1) ? 0 : 1);
    for (int i = 31; i >= 0; i--)
        bits[n++] = (uint8_t)((capture->sync_word >> i) & 1u);
    for (uint8_t b = 0; b < capture->len; b++)
        for (int i = 7; i >= 0; i--)
            bits[n++] = (uint8_t)((capture->data[b] >> i) & 1u);
    return n;
}

size_t ls_fsk_raw_data(const uint8_t *bits, size_t n_bits, uint32_t bitrate,
                       int32_t *out, size_t max_out)
{
    if (!bits || !n_bits || !out || !max_out || !bitrate) return 0;

    /* One bit's worth of microseconds. Rounded, and the rounding is spent
       per RUN rather than per bit - a run of eight bits is eight times the
       exact period, not eight times a rounded one, so the error cannot
       accumulate along the frame. */
    size_t n = 0, i = 0;
    while (i < n_bits) {
        const uint8_t level = bits[i];
        size_t run = 0;
        while (i + run < n_bits && bits[i + run] == level) run++;
        const int32_t us = (int32_t)(((uint64_t)run * 1000000u + bitrate / 2)
                                     / bitrate);
        if (n == max_out) return 0;
        out[n++] = level ? us : -us;
        i += run;
    }
    return n;
}

size_t ls_fsk_bits_from_raw(const int32_t *raw, size_t n_raw, uint32_t bitrate,
                            uint8_t *bits, size_t max_bits)
{
    if (!raw || !n_raw || !bits || !bitrate) return 0;

    size_t n = 0;
    for (size_t i = 0; i < n_raw; i++) {
        const int32_t d = raw[i];
        if (d == 0) return 0;
        const uint32_t us = (uint32_t)(d < 0 ? -d : d);
        /* How many bit periods that run was, rounded. ls_fsk_raw_data spends
           its rounding per run, so this recovers the count exactly for
           anything it produced and to the nearest bit for anything else. */
        const uint32_t count = (uint32_t)(((uint64_t)us * bitrate + 500000u)
                                          / 1000000u);
        if (!count) return 0;
        if (n + count > max_bits) return 0;
        for (uint32_t k = 0; k < count; k++) bits[n++] = d > 0 ? 1u : 0u;
    }
    return n;
}

size_t ls_fsk_payload_from_raw(const int32_t *raw, size_t n_raw,
                               uint32_t bitrate, uint16_t preamble_bits,
                               uint8_t *out, size_t max_out)
{
    if (!out || !max_out) return 0;

    static EXT_RAM_BSS_ATTR uint8_t bits[LS_FSK_CAPTURE_BYTES * 8 + 1024 + 32];
    const size_t n_bits = ls_fsk_bits_from_raw(raw, n_raw, bitrate, bits,
                                               sizeof(bits));
    const size_t skip = (size_t)preamble_bits + 32;
    /* A stream that does not even carry its own preamble and sync is not a
       frame this can report bytes for. */
    if (n_bits <= skip) return 0;

    const size_t payload_bits = n_bits - skip;
    size_t bytes = payload_bits / 8;
    if (bytes > max_out) bytes = max_out;

    for (size_t b = 0; b < bytes; b++) {
        uint8_t v = 0;
        for (int k = 0; k < 8; k++) v = (uint8_t)((v << 1) | bits[skip + b * 8 + k]);
        out[b] = v;
    }
    return bytes;
}

/* The registers a 2-FSK asynchronous-serial transmit needs, in the order the
   Flipper writes them. Everything not derived from the capture is the
   asynchronous-serial configuration itself and is fixed:

     IOCFG0   0x0D  GDO0 is serial data, which is what makes RAW replay work
     PKTCTRL0 0x32  asynchronous serial, infinite packet length
     MDMCFG2  0x04  2-FSK, no Manchester, no sync-word detection in the part

   The rest are the front-end and AGC defaults the SubGhz app uses for a
   custom preset; they matter for receive and are inert for a keyed transmit,
   but a preset that omits them is not one the Flipper will load. */
static size_t preset_bytes(const ls_fsk_capture_t *capture, uint8_t *out,
                           size_t max)
{
    uint32_t dev_actual = 0, rate_actual = 0;
    uint8_t drate_e = 0, drate_m = 0;
    const uint8_t deviatn = ls_cc1101_deviatn(capture->deviation_hz,
                                              &dev_actual);
    /* Twice the capture's rate, not the rate itself.

       In asynchronous serial mode the CC1101 does not clock the data - it
       SAMPLES the level on GDO0 at eight times the programmed rate, and the
       datasheet requires the bit period error to stay under an eighth of it.
       Programming the real rate leaves a 417 us bit spanning 8.02 samples of
       52 us: no margin at all, and the edges of a replayed frame land wherever
       the sampler happens to be. Doubling the programmed rate halves the
       sampling period and gives every bit sixteen samples instead of eight.
       The RAW timings are untouched; this changes only how finely the part
       looks at them. A known-good capture of this protocol carries exactly
       this - one DRATE_E step above its own bit rate. */
    ls_cc1101_drate(capture->bitrate * 2u, &drate_e, &drate_m, &rate_actual);

    /* CHANBW_E=1, CHANBW_M=2 with DRATE_E in the low nibble: a 325 kHz
       receive filter, wide enough for anything this path can be tuned to. */
    const uint8_t mdmcfg4 = (uint8_t)(0x60 | (drate_e & 0x0F));

    static const uint8_t TAIL[] = {
        0x0B, 0x06,  0x08, 0x32,  0x07, 0x04,  0x14, 0x00,
        0x13, 0x02,  0x12, 0x04,
    };
    const uint8_t HEAD[] = { 0x02, 0x0D };
    const uint8_t RATE[] = { 0x11, drate_m, 0x10, mdmcfg4, 0x15, deviatn };
    /* The 0x00 0x00 ends the register pairs, and what follows is the PA
       table, which is EIGHT bytes and not one. The Flipper reads a fixed
       eight from past the terminator; given one it refuses the whole file
       with "Custom_preset_data size error" and will not transmit it. Only
       the first entry is ever keyed here - the rest are zero - but they have
       to be present for the file to load at all. */
    static const uint8_t REST[] = {
        0x18, 0x18,  0x19, 0x16,  0x1D, 0x91,  0x1C, 0x00,
        0x1B, 0x07,  0x20, 0xFB,  0x22, 0x10,  0x21, 0x56,
        0x00, 0x00,
        0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

    const size_t need = sizeof(HEAD) + sizeof(TAIL) + sizeof(RATE) +
                        sizeof(REST);
    if (need > max) return 0;

    size_t n = 0;
    memcpy(out + n, HEAD, sizeof(HEAD)); n += sizeof(HEAD);
    memcpy(out + n, TAIL, sizeof(TAIL)); n += sizeof(TAIL);
    memcpy(out + n, RATE, sizeof(RATE)); n += sizeof(RATE);
    memcpy(out + n, REST, sizeof(REST)); n += sizeof(REST);
    return n;
}

size_t ls_sub_preset_text(const ls_sub_mod_t *mod, char *out, size_t len)
{
    if (!out || !len) return 0;

    /* Nothing recorded, or amplitude keying: the stock preset. Every
       Flipper already has it, so the file stays readable on one that has
       never seen this board. A rate without a deviation is still amplitude
       keying - knowing how fast it was keyed does not change what the
       modulator has to do. */
    if (!mod || !mod->deviation_hz)
        return (size_t)snprintf(out, len,
                                "Preset: FuriHalSubGhzPresetOok650Async\n");

    /* Frequency keyed, so it needs a preset built for it. The bounds are
       the radio's own, the same ones ls_lora_fsk_begin enforces: a capture
       outside them could not have been heard as recorded, and a file
       written from it would replay as something else. */
    if (mod->bitrate < 600u || mod->bitrate > 300000u ||
        mod->deviation_hz < 600u || mod->deviation_hz > 200000u)
        return 0;

    ls_fsk_capture_t shim;
    memset(&shim, 0, sizeof(shim));
    shim.bitrate = mod->bitrate;
    shim.deviation_hz = mod->deviation_hz;

    uint8_t preset[64];
    const size_t n = preset_bytes(&shim, preset, sizeof(preset));
    if (!n) return 0;

    /* snprintf semantics all the way through, so a caller sizing a buffer
       gets a straight answer instead of a truncated preset that looks
       complete. */
    size_t want = 0;
#define PRESET_EMIT(...) do { \
        const int w = snprintf(want < len ? out + want : NULL, \
                               want < len ? len - want : 0, __VA_ARGS__); \
        if (w > 0) want += (size_t)w; \
    } while (0)

    PRESET_EMIT("Preset: FuriHalSubGhzPresetCustom\n");
    PRESET_EMIT("Custom_preset_module: CC1101\n");
    PRESET_EMIT("Custom_preset_data:");
    for (size_t i = 0; i < n; i++) PRESET_EMIT(" %02X", preset[i]);
    PRESET_EMIT("\n");
#undef PRESET_EMIT
    return want;
}

size_t ls_sub_fsk_render(const ls_fsk_capture_t *capture, const char *name,
                         char *out, size_t len)
{
    if (!capture || !capture->len) return 0;

    const ls_sub_mod_t mod = { capture->bitrate, capture->deviation_hz };
    char preset[320];
    const size_t preset_len = ls_sub_preset_text(&mod, preset, sizeof(preset));
    if (!preset_len || preset_len >= sizeof(preset)) return 0;

    static EXT_RAM_BSS_ATTR uint8_t bits[LS_FSK_CAPTURE_BYTES * 8 + 1024 + 32];
    const size_t n_bits = ls_fsk_bitstream(capture, bits, sizeof(bits));
    if (!n_bits) return 0;

    static EXT_RAM_BSS_ATTR int32_t raw[sizeof(bits)];
    const size_t n_raw = ls_fsk_raw_data(bits, n_bits, capture->bitrate, raw,
                                         sizeof(raw) / sizeof(raw[0]));
    if (!n_raw) return 0;

    uint32_t dev_actual = 0;
    (void)ls_cc1101_deviatn(capture->deviation_hz, &dev_actual);

    size_t n = 0;
    /* snprintf semantics throughout: `n` is what the file WANTS to be, so a
       caller sizing a buffer gets a straight answer instead of a truncated
       file that looks complete. */
#define EMIT(...) do { \
        const int w = snprintf(n < len ? out + n : NULL, \
                               n < len ? len - n : 0, __VA_ARGS__); \
        if (w > 0) n += (size_t)w; \
    } while (0)

    EMIT("Filetype: Flipper SubGhz RAW File\n");
    EMIT("Version: 1\n");
    EMIT("Frequency: %lu\n", (unsigned long)capture->freq_hz);
    EMIT("%s", preset);
    EMIT("Protocol: RAW\n");
    if (name && *name) EMIT("# Name: %s\n", name);
    EMIT("# %u baud, %lu Hz deviation requested, %lu Hz encodable\n",
         (unsigned)capture->bitrate, (unsigned long)capture->deviation_hz,
         (unsigned long)dev_actual);
    EMIT("# %u-bit preamble, sync %08lX, %u byte payload\n",
         (unsigned)capture->preamble_bits,
         (unsigned long)capture->sync_word, (unsigned)capture->len);

    size_t per_line = 0;
    for (size_t i = 0; i < n_raw; i++) {
        if (per_line == 0) EMIT("RAW_Data:");
        EMIT(" %ld", (long)raw[i]);
        if (++per_line >= 512) { EMIT("\n"); per_line = 0; }
    }
    if (per_line) EMIT("\n");
#undef EMIT

    if (len) out[n < len ? n : len - 1] = '\0';
    return n;
}
