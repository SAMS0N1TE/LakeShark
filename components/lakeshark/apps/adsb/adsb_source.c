#include "adsb_source.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_timer.h"

#include "adsb_decode.h"
#include "ls_lora.h"

/* ------------------------------------------------------- chip-frame log -- */

/* One writer (the pump's thread) and one reader at a time (the console), but
   the reader is a different task on a different core, so both sides go through
   a spinlock critical section. What is held under it is a handful of word
   writes and one copy of about 350 bytes: well under a microsecond of the
   pump's time, with no allocation and no wait on anything. */
static portMUX_TYPE s_diag_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint32_t frames, decoded, bad_chips, bad_crc, errors, clean;
    uint32_t first_bad[ADSB_FIRST_BAD_BUCKETS];
    uint32_t next_seq;                        /* seq of the next frame; 0 = none yet */
    adsb_chip_rec_t ring[ADSB_CHIP_LOG_N];
    /* One bin per wall-clock second, for the rates: `stamp` is the second plus
       one (0 = never used), so a bin from a second long gone is not read as
       this one. */
    struct { int64_t stamp; uint32_t frames, decoded; } bins[16];
    bool    started;
    int64_t start_us;
} s_diag;

void adsb_chip_diag_reset(void)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_diag_mux);
    memset(&s_diag, 0, sizeof(s_diag));
    s_diag.started = true;
    s_diag.start_us = now;
    portEXIT_CRITICAL(&s_diag_mux);
}

void adsb_chip_diag_mark_start(void)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_diag_mux);
    s_diag.started = true;
    s_diag.start_us = now;
    portEXIT_CRITICAL(&s_diag_mux);
}

void adsb_chip_diag_note_error(void)
{
    portENTER_CRITICAL(&s_diag_mux);
    s_diag.errors++;
    portEXIT_CRITICAL(&s_diag_mux);
}

void adsb_chip_diag_record(const uint8_t *chips, int nbytes, int rssi_tenths,
                           int verdict, int first_bad_bit)
{
    adsb_chip_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    if (chips && nbytes > 0)
        memcpy(rec.chips, chips, nbytes > ADSB_MODES_FRAME_BYTES ? ADSB_MODES_FRAME_BYTES
                                                                  : (size_t)nbytes);
    rec.len = (uint8_t)(nbytes < 0 ? 0 : nbytes > ADSB_MODES_FRAME_BYTES ? ADSB_MODES_FRAME_BYTES : nbytes);
    rec.verdict = (uint8_t)verdict;
    rec.first_bad_bit = (int16_t)first_bad_bit;
    rec.rssi_tenths = (int16_t)rssi_tenths;

    const int64_t now = esp_timer_get_time();
    const int64_t sec = now / 1000000;
    portENTER_CRITICAL(&s_diag_mux);
    if (!s_diag.started) { s_diag.started = true; s_diag.start_us = now; }
    rec.seq = ++s_diag.next_seq;
    s_diag.ring[(rec.seq - 1) % ADSB_CHIP_LOG_N] = rec;
    s_diag.frames++;
    {
        __typeof__(s_diag.bins[0]) *bin = &s_diag.bins[sec & 15];
        if (bin->stamp != sec + 1) { bin->stamp = sec + 1; bin->frames = 0; bin->decoded = 0; }
        bin->frames++;
        if (verdict == ADSB_FRAME_OK) bin->decoded++;
    }
    switch (verdict) {
    case ADSB_FRAME_OK:        s_diag.decoded++;   break;
    case ADSB_FRAME_BAD_CHIPS: s_diag.bad_chips++; break;
    case ADSB_FRAME_BAD_CRC:   s_diag.bad_crc++;   break;
    }
    if (first_bad_bit < 0) {
        s_diag.clean++;
    } else {
        int bucket = first_bad_bit / 8;
        if (bucket >= ADSB_FIRST_BAD_BUCKETS) bucket = ADSB_FIRST_BAD_BUCKETS - 1;
        s_diag.first_bad[bucket]++;
    }
    portEXIT_CRITICAL(&s_diag_mux);
}

void adsb_chip_diag_snapshot(adsb_chip_diag_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    const int64_t now = esp_timer_get_time();
    const int64_t now_sec = now / 1000000;
    portENTER_CRITICAL(&s_diag_mux);
    out->frames = s_diag.frames;
    out->decoded = s_diag.decoded;
    out->bad_chips = s_diag.bad_chips;
    out->bad_crc = s_diag.bad_crc;
    out->errors = s_diag.errors;
    out->clean = s_diag.clean;
    memcpy(out->first_bad, s_diag.first_bad, sizeof(out->first_bad));
    const uint32_t last = s_diag.next_seq;          /* seq of the newest, 0 if none */
    const uint32_t n = last < ADSB_CHIP_LOG_N ? last : ADSB_CHIP_LOG_N;
    out->n_recs = n;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t seq = last - n + 1 + i;      /* oldest first */
        out->recs[i] = s_diag.ring[(seq - 1) % ADSB_CHIP_LOG_N];
    }
    if (s_diag.started && now > s_diag.start_us) {
        const int64_t start_sec = s_diag.start_us / 1000000;
        /* The completed seconds: not the one the session began in, nor the one
           now running. At most ADSB_RATE_WINDOW_S of them. */
        int64_t lo = now_sec - ADSB_RATE_WINDOW_S;
        if (lo < start_sec + 1) lo = start_sec + 1;
        const int64_t hi = now_sec - 1;
        if (hi >= lo) {
            uint32_t frames = 0, decoded = 0;
            for (int64_t s = lo; s <= hi; s++) {
                const __typeof__(s_diag.bins[0]) *bin = &s_diag.bins[s & 15];
                if (bin->stamp == s + 1) { frames += bin->frames; decoded += bin->decoded; }
            }
            out->rate_window_s = (int)(hi - lo + 1);
            out->trigger_per_s = (float)frames / (float)out->rate_window_s;
            out->decoded_per_s = (float)decoded / (float)out->rate_window_s;
        }
        const int64_t elapsed_us = now - s_diag.start_us;
        out->elapsed_s = (uint32_t)(elapsed_us / 1000000);
        if (elapsed_us >= 1000000) {
            out->avg_trigger_per_s = (float)((double)s_diag.frames * 1e6 / (double)elapsed_us);
            out->avg_decoded_per_s = (float)((double)s_diag.decoded * 1e6 / (double)elapsed_us);
        }
    }
    portEXIT_CRITICAL(&s_diag_mux);
}

int adsb_chip_diag_format_rec(const adsb_chip_rec_t *rec, char *out, size_t size)
{
    if (!rec || !out || !size) return 0;
    char rssi[16];
    if (rec->rssi_tenths == ADSB_RSSI_NONE) snprintf(rssi, sizeof(rssi), "   n/a");
    else snprintf(rssi, sizeof(rssi), "%6.1f", rec->rssi_tenths / 10.0);

    char verdict[32];
    switch (rec->verdict) {
    case ADSB_FRAME_OK:        snprintf(verdict, sizeof(verdict), "ok"); break;
    case ADSB_FRAME_BAD_CHIPS:
        if (rec->first_bad_bit >= 0) snprintf(verdict, sizeof(verdict), "bad_chips@%d", rec->first_bad_bit);
        else                         snprintf(verdict, sizeof(verdict), "bad_chips");
        break;
    case ADSB_FRAME_BAD_CRC:   snprintf(verdict, sizeof(verdict), "bad_crc"); break;
    default:                   snprintf(verdict, sizeof(verdict), "?%u", (unsigned)rec->verdict); break;
    }
    /* A frame that passed the pair rule can still carry bad pairs in its tail. */
    char tail[16] = "";
    if (rec->verdict != ADSB_FRAME_BAD_CHIPS && rec->first_bad_bit >= 0)
        snprintf(tail, sizeof(tail), " fb@%d", rec->first_bad_bit);

    int n = snprintf(out, size, "#%lu %s dBm %s%s ", (unsigned long)rec->seq, rssi, verdict, tail);
    for (int i = 0; i < rec->len && n >= 0 && (size_t)n + 3 < size; i++)
        n += snprintf(out + n, size - (size_t)n, "%02X", rec->chips[i]);
    return n;
}

int adsb_chip_diag_format_counts(const adsb_chip_diag_t *d, char *out, size_t size)
{
    if (!d || !out || !size) return 0;
    return snprintf(out, size,
                    "frames=%lu decoded=%lu bad_chips=%lu bad_crc=%lu errors=%lu clean_pairs=%lu",
                    (unsigned long)d->frames, (unsigned long)d->decoded,
                    (unsigned long)d->bad_chips, (unsigned long)d->bad_crc,
                    (unsigned long)d->errors, (unsigned long)d->clean);
}

int adsb_chip_diag_format_hist(const adsb_chip_diag_t *d, char *out, size_t size)
{
    if (!d || !out || !size) return 0;
    int n = 0;
    for (int b = 0; b < ADSB_FIRST_BAD_BUCKETS && n >= 0 && (size_t)n < size; b++)
        n += snprintf(out + n, size - (size_t)n, "%s%d-%d:%lu", b ? " " : "", b * 8, b * 8 + 7,
                      (unsigned long)d->first_bad[b]);
    return n;
}

int adsb_chip_diag_format_rates(const adsb_chip_diag_t *d, char *out, size_t size)
{
    if (!d || !out || !size) return 0;
    if (d->rate_window_s <= 0)
        return snprintf(out, size, "rates: n/a, no whole second counted yet");
    return snprintf(out, size,
                    "triggers %.1f/s, decoded %.1f/s (last %d s); since start %.1f/s and %.1f/s over %lu s",
                    d->trigger_per_s, d->decoded_per_s, d->rate_window_s,
                    d->avg_trigger_per_s, d->avg_decoded_per_s, (unsigned long)d->elapsed_s);
}

void adsb_chip_diag_print(void)
{
    adsb_chip_diag_t d;
    adsb_chip_diag_snapshot(&d);
    char line[160];
    adsb_chip_diag_format_counts(&d, line, sizeof(line));
    printf("adsb chips: %s (since boot or `adsb chips reset`)\n", line);
    adsb_chip_diag_format_rates(&d, line, sizeof(line));
    printf("adsb chips: %s\n", line);
    adsb_chip_diag_format_hist(&d, line, sizeof(line));
    printf("adsb chips: first invalid pair at message bit, counts per 8 bits: %s\n", line);
    printf("adsb chips: last %lu frame%s, oldest first (28 chip bytes after the preamble)\n",
           (unsigned long)d.n_recs, d.n_recs == 1 ? "" : "s");
    for (uint32_t i = 0; i < d.n_recs; i++) {
        adsb_chip_diag_format_rec(&d.recs[i], line, sizeof(line));
        printf("  %s\n", line);
    }
}

adsb_source_t adsb_source_choose(bool want_lora, bool iq_present, uint32_t lora_caps)
{
    const bool lora = (lora_caps & LS_LORA_CAP_MODES_RX) != 0;
    if (want_lora && lora) return ADSB_SRC_LORA;
    if (iq_present) return ADSB_SRC_IQ;
    if (lora) return ADSB_SRC_LORA;
    return ADSB_SRC_NONE;
}

const char *adsb_source_name(adsb_source_t source)
{
    switch (source) {
    case ADSB_SRC_IQ:   return "iq";
    case ADSB_SRC_LORA: return "lr-chip";
    case ADSB_SRC_NONE: break;
    }
    return "none";
}

int adsb_lr_gain_step(int gain_tenths_db)
{
    /* The slider runs 0..496 tenths of a dB and 0 is automatic, as it is for
       the IQ receivers. The chip's own AGC cannot settle inside the 8 us
       preamble, and on air it triggered on twice the noise of step 13 and
       decoded a sixth as much, so automatic is step 13. Anything above 0 is
       a fixed gain: the 13 steps are spread evenly across the slider, rounded
       up so the smallest nonzero setting is still step 1 and the top of it is
       step 13, the most. */
    if (gain_tenths_db <= 0) return LS_LORA_MODES_GAIN_MAX;
    if (gain_tenths_db >= 496) return LS_LORA_MODES_GAIN_MAX;
    int step = (gain_tenths_db * LS_LORA_MODES_GAIN_MAX + 495) / 496;
    if (step < 1) step = 1;
    if (step > LS_LORA_MODES_GAIN_MAX) step = LS_LORA_MODES_GAIN_MAX;
    return step;
}

int adsb_modes_pump(const adsb_modes_ops_t *ops, adsb_modes_stats_t *stats,
                    int max_frames)
{
    uint8_t buf[ADSB_MODES_FRAME_BYTES];
    adsb_modes_stats_t scratch = { 0 };
    if (!ops || !ops->poll) return -1;
    if (!stats) stats = &scratch;

    int handled = 0;
    while (handled < max_frames) {
        float rssi = NAN;
        const int n = ops->poll(buf, sizeof(buf), &rssi);
        if (n == 0) {
            stats->empty_polls++;
            break;
        }
        if (n < 0) {
            stats->errors++;
            adsb_chip_diag_note_error();
            return n;
        }
        /* The level of a frame the source did not measure is "not known", not
           zero: adsb_on_chips keeps the last real one. */
        const int tenths = isnan(rssi) ? ADSB_RSSI_NONE : (int)lroundf(rssi * 10.0f);
        stats->frames++;
        handled++;
        int first_bad = -1;
        const adsb_frame_result_t verdict =
            adsb_on_chips_ex(buf, n, ADSB_CHIPS_FULL, tenths, &first_bad);
        adsb_chip_diag_record(buf, n, tenths, (int)verdict, first_bad);
        switch (verdict) {
        case ADSB_FRAME_OK:        stats->decoded++;   break;
        case ADSB_FRAME_BAD_CHIPS: stats->bad_chips++; break;
        case ADSB_FRAME_BAD_CRC:   stats->bad_crc++;   break;
        }
    }
    return handled;
}
