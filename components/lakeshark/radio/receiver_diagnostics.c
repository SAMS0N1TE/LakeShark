/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */
/* LS-1000: receiver troubleshooting previously combined a live decoder byte
 * rate with an uninitialised LCD health service and printed rhs=absent.  This
 * formatter preserves each source's validity, so an unavailable health or
 * effective tune is printed as '?' and cannot impersonate a measured zero. */

#include "receiver_diagnostics.h"

typedef struct {
    char *out;
    size_t cap;
    size_t used;
} diag_writer_t;

static void put_char(diag_writer_t *w, char c)
{
    if (w->used + 1 >= w->cap) return;
    w->out[w->used++] = c;
    w->out[w->used] = '\0';
}

static void put_text(diag_writer_t *w, const char *text)
{
    if (!text) return;
    while (*text && w->used + 1 < w->cap)
        w->out[w->used++] = *text++;
    w->out[w->used] = '\0';
}

static void put_u64(diag_writer_t *w, uint64_t value)
{
    char digits[20];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && count < sizeof(digits));
    while (count) put_char(w, digits[--count]);
}

static void put_i32(diag_writer_t *w, int value)
{
    int64_t wide = value;
    if (wide < 0) {
        put_char(w, '-');
        wide = -wide;
    }
    put_u64(w, (uint64_t)wide);
}

static void put_key(diag_writer_t *w, const char *key)
{
    put_char(w, ' ');
    put_text(w, key);
    put_char(w, '=');
}

const char *ls_receiver_rx_state_name(ls_receiver_rx_state_t state)
{
    switch (state) {
    case LS_RECEIVER_RX_DISCONNECTED: return "disconnected";
    case LS_RECEIVER_RX_PARKED:       return "parked";
    case LS_RECEIVER_RX_IDLE:         return "idle";
    case LS_RECEIVER_RX_ACTIVE:       return "active";
    case LS_RECEIVER_RX_UNKNOWN:      return "?";
    }
    return "?";
}

static void put_key_u64(diag_writer_t *w, const char *key,
                        bool known, uint64_t value)
{
    put_key(w, key);
    if (known) put_u64(w, value);
    else put_char(w, '?');
}

static void put_key_i32(diag_writer_t *w, const char *key,
                        bool known, int value)
{
    put_key(w, key);
    if (known) put_i32(w, value);
    else put_char(w, '?');
}

int ls_receiver_diag_format(const ls_receiver_diag_t *s,
                            char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    diag_writer_t w = { .out = out, .cap = cap };
    if (!s) {
        put_text(&w, "app=? park=? rx=?");
        return (int)w.used;
    }

    /* LS-1002: newlib's snprintf path reserves 1328 bytes before formatting
     * a single field on ESP32-P4 (_svfprintf_r=1152, snprintf=176).  The LCD
     * console had only 832 bytes left at this point and faulted before the
     * first STAT reply.  This writer only copies bounded tokens and renders
     * integers into a 20-byte local, so receiver queries never enter libc's
     * variadic formatter. */
    put_text(&w, "app=");
    put_text(&w, s->app_known && s->app[0] ? s->app : "?");
    put_text(&w, " park=");
    if (s->parked_known) put_char(&w, s->parked ? '1' : '0');
    else put_char(&w, '?');
    put_text(&w, " rx=");
    put_text(&w, ls_receiver_rx_state_name(s->rx_state));
    put_text(&w, " dec=");
    put_text(&w, s->decoder[0] ? s->decoder : "?");
    put_text(&w, " demod=");
    put_text(&w, s->demod[0] ? s->demod : "?");
    put_text(&w, " ep=");
    put_text(&w, s->endpoint_known && s->endpoint_id[0]
                     ? s->endpoint_id : "?");
    put_text(&w, " own=");
    put_text(&w, s->endpoint_known && s->owner[0] ? s->owner : "?");
    put_text(&w, " present=");
    if (s->endpoint_known) put_char(&w, s->endpoint_present ? '1' : '0');
    else put_char(&w, '?');
    put_text(&w, " stream=");
    if (s->endpoint_known) put_char(&w, s->endpoint_streaming ? '1' : '0');
    else put_char(&w, '?');
    put_text(&w, " rhs=");
    put_text(&w, s->health_known && s->health[0] ? s->health : "?");
    put_key_u64(&w, "hbps", s->health_rate_known,
                s->health_bytes_per_second);

    put_key_u64(&w, "freq_req", s->requested_frequency_known,
                s->requested_frequency_hz);
    put_key_u64(&w, "freq_eff", s->effective_frequency_known,
                s->effective_frequency_hz);
    put_text(&w, s->effective_frequency_known
                     ? " freq_valid=1" : " freq_valid=?");
    put_key_i32(&w, "gain_req", s->requested_gain_known,
                s->requested_gain_tenths_db);
    put_key_i32(&w, "gain_eff", s->effective_gain_known,
                s->effective_gain_tenths_db);
    put_text(&w, s->effective_gain_known
                     ? " gain_valid=1" : " gain_valid=?");

    put_key_u64(&w, "iq", s->iq_total_known, s->iq_bytes_total);
    put_key_u64(&w, "iqps", s->iq_rate_known, s->iq_bytes_per_second);
    put_key_u64(&w, "frame", s->frame_count_known, s->frame_count);
    put_key_u64(&w, "valid", s->valid_count_known, s->valid_count);
    put_key_u64(&w, "fail", s->failed_count_known, s->failed_count);
    put_key_u64(&w, "ctrl", s->control_count_known, s->control_count);
    put_key_u64(&w, "voice", s->voice_count_known, s->voice_count);
    put_key_u64(&w, "adrop", s->audio_drop_count_known,
                s->audio_drop_count);
    put_text(&w, " err=");
    put_text(&w, s->receiver_error_known && s->receiver_error[0]
                     ? s->receiver_error : "?");

    put_text(&w, " mem=");
    if (s->memory_known) {
        put_u64(&w, s->internal_free);
        put_char(&w, '/');
        put_u64(&w, s->internal_largest);
        put_char(&w, ',');
        put_u64(&w, s->dma_free);
        put_char(&w, '/');
        put_u64(&w, s->dma_largest);
    } else {
        put_text(&w, "?/?,?/?");
    }
    return (int)w.used;
}
