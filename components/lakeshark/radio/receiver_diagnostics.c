/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */

#include "receiver_diagnostics.h"
#include <string.h>
#include "ls_textpack.h"

void ls_receiver_diag_ui_counters(ls_receiver_diag_t *s)
{
    if (!s) return;
    if (!s->app_known || !strcmp(s->app, "REC") ||
        (!strcmp(s->app, "FM") &&
         (!strcmp(s->demod, "pocsag") || !strcmp(s->demod, "flex"))))
        s->valid_count_known = false;
}

ls_receiver_check_t ls_receiver_diag_assess(const ls_receiver_diag_t *s)
{
    const ls_receiver_check_t unknown = {
        LS_RECEIVER_CHECK_UNKNOWN, "STATE UNAVAILABLE",
        "Current receiver evidence is incomplete.",
        "Open a receiver, then check again."
    };
    if (!s) return unknown;
    if (s->parked_known && s->parked)
        return (ls_receiver_check_t){ LS_RECEIVER_CHECK_INFO, "RECEIVER PARKED",
            "No receiver is requested right now.",
            "Open FM, P25 or another receiver app." };

    const char *error = s->receiver_error_known ? s->receiver_error : "";
    if (!strcmp(error, "unsupported"))
        return (ls_receiver_check_t){ LS_RECEIVER_CHECK_ATTENTION, "MODE NOT READY",
            "The receiver refused this operation.",
            "Use RADIO to choose a supported receiver." };
    if (!strcmp(error, "busy"))
        return (ls_receiver_check_t){ LS_RECEIVER_CHECK_ATTENTION, "RECEIVER BUSY",
            "Another receiver operation holds it.",
            "Check the owner below or choose another radio." };
    if (!strcmp(error, "no-memory"))
        return (ls_receiver_check_t){ LS_RECEIVER_CHECK_ATTENTION, "MEMORY SHORTAGE",
            "The receiver could not allocate memory.",
            "Close the receiver and check DIAG MEMORY." };
    if (s->rx_state == LS_RECEIVER_RX_DISCONNECTED ||
        (s->rx_state != LS_RECEIVER_RX_ACTIVE &&
         s->endpoint_known && !s->endpoint_present))
        return (ls_receiver_check_t){ LS_RECEIVER_CHECK_ATTENTION, "RECEIVER MISSING",
            "The selected receiver is not available.",
            "Check its connection and the RADIO choice." };
    if (!strcmp(error, "io") || !strcmp(error, "timeout"))
        return (ls_receiver_check_t){ LS_RECEIVER_CHECK_ATTENTION, "RECEIVER ERROR",
            "A receiver control or read failed.",
            "Check the error and endpoint health below." };

    /* A lifetime count cannot establish that frames are arriving now. A
     * positive current sample rate only proves the transport reaches the app. */
    if (s->rx_state == LS_RECEIVER_RX_ACTIVE) {
        if (s->iq_rate_known && s->iq_bytes_per_second == 0)
            return (ls_receiver_check_t){ LS_RECEIVER_CHECK_ATTENTION, "NO CURRENT IQ",
                "Receiver active; sampled IQ rate is zero.",
                "Wait for a sample; check endpoint health." };
        if (s->valid_count_known && s->valid_count > 0)
            return (ls_receiver_check_t){ LS_RECEIVER_CHECK_INFO, "VALID FRAMES SEEN",
                "The valid-frame count is cumulative.",
                "Compare it over time; history is not live proof." };
        if (s->iq_rate_known && s->iq_bytes_per_second > 0)
            return (ls_receiver_check_t){ LS_RECEIVER_CHECK_INFO, "SAMPLES FLOWING",
                s->valid_count_known ? "Samples arrive; no valid frames counted."
                                     : "Samples arrive; valid count unavailable.",
                "Check frequency, mode and antenna." };
        return (ls_receiver_check_t){ LS_RECEIVER_CHECK_UNKNOWN, "RECEIVER ACTIVE",
            "Current sample or decoder evidence is unavailable.",
            "Compare the counters; do not infer reception." };
    }
    if (s->rx_state == LS_RECEIVER_RX_IDLE)
        return (ls_receiver_check_t){ LS_RECEIVER_CHECK_INFO, "RECEIVER IDLE",
            "The receiver is not streaming right now.",
            "Check the app mode and RADIO selection." };
    return unknown;
}

typedef struct {
    char *out;
    size_t cap;
    size_t used;
    bool truncated;
} diag_writer_t;

static void put_char(diag_writer_t *w, char c)
{
    if (w->used + 1 >= w->cap) { w->truncated = true; return; }
    w->out[w->used++] = c;
    w->out[w->used] = '\0';
}

static void put_text(diag_writer_t *w, const char *text)
{
    if (!text) return;
    while (*text && w->used + 1 < w->cap)
        w->out[w->used++] = *text++;
    if (*text) w->truncated = true;
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

void ls_receiver_trial_baseline(ls_receiver_trial_t *t,
                                const ls_receiver_diag_t *s)
{
    if (!t) return;
    memset(t, 0, sizeof(*t));
    if (s) { t->before = *s; t->baseline_set = true; }
}

void ls_receiver_trial_compare(ls_receiver_trial_t *t,
                               const ls_receiver_diag_t *s, uint64_t now)
{
    if (!t || !t->baseline_set) return;
    /* Leave the operator's baseline in place even after rejected compares. */
    memset(&t->after, 0, sizeof(t->after));
    t->compared = true;
    t->changed = t->unknown = t->delta_known = t->counter_reset = 0;
    t->elapsed_us = t->iq_delta = 0;
    t->frame_delta = t->valid_delta = t->failed_delta = 0;
    t->control_delta = t->voice_delta = t->audio_drop_delta = 0;
    t->time_invalid = t->stale = t->inactive = false;
    if (s) t->after = *s;
    const ls_receiver_diag_t *a = &t->before, *b = &t->after;
#define KEY(bit, known, equal) do { if (!(known)) t->unknown |= (bit); \
    else if (!(equal)) t->changed |= (bit); } while (0)
    KEY(LS_TRIAL_APP, a->app_known && b->app_known && a->app[0] && b->app[0],
        !strcmp(a->app, b->app));
    KEY(LS_TRIAL_SOURCE, a->source_known && b->source_known &&
        a->endpoint_known && b->endpoint_known,
        !strcmp(a->source, b->source) && !strcmp(a->endpoint_id,b->endpoint_id) &&
        !strcmp(a->owner,b->owner));
    KEY(LS_TRIAL_MODE, a->demod[0] && b->demod[0], !strcmp(a->demod,b->demod));
    KEY(LS_TRIAL_TUNE, a->requested_frequency_known && b->requested_frequency_known &&
        a->effective_frequency_known && b->effective_frequency_known,
        a->requested_frequency_hz == b->requested_frequency_hz &&
        a->effective_frequency_hz == b->effective_frequency_hz);
    KEY(LS_TRIAL_RATE, a->sample_rate_known && b->sample_rate_known &&
        a->sample_rate_hz && b->sample_rate_hz, a->sample_rate_hz == b->sample_rate_hz);
    KEY(LS_TRIAL_CONFIG, a->config_known && b->config_known &&
        a->requested_gain_known && b->requested_gain_known &&
        a->effective_gain_known && b->effective_gain_known,
        a->bandwidth_hz == b->bandwidth_hz && a->gain_mode == b->gain_mode &&
        a->requested_gain_tenths_db == b->requested_gain_tenths_db &&
        a->effective_gain_tenths_db == b->effective_gain_tenths_db);
    KEY(LS_TRIAL_CONTEXT, a->context_epoch_known && b->context_epoch_known &&
        a->consumer_epoch_known && b->consumer_epoch_known &&
        a->context_epoch != UINT64_MAX && b->context_epoch != UINT64_MAX &&
        a->consumer_epoch != UINT64_MAX && b->consumer_epoch != UINT64_MAX,
        a->context_epoch == b->context_epoch && a->consumer_epoch == b->consumer_epoch);
    KEY(LS_TRIAL_IQ_EPOCH, a->iq_epoch_known && b->iq_epoch_known &&
        a->iq_epoch != UINT64_MAX && b->iq_epoch != UINT64_MAX,
        a->iq_epoch == b->iq_epoch);
    KEY(LS_TRIAL_DECODER_EPOCH, a->decoder_epoch_known && b->decoder_epoch_known &&
        a->decoder_epoch != UINT64_MAX && b->decoder_epoch != UINT64_MAX,
        a->decoder_epoch == b->decoder_epoch);
#undef KEY
    t->time_invalid = !a->captured_known || !b->captured_known ||
        b->captured_us <= a->captured_us || now < b->captured_us;
    if (!t->time_invalid) {
        t->elapsed_us = b->captured_us - a->captured_us;
        t->stale = t->elapsed_us > LS_RECEIVER_TRIAL_MAX_US ||
            now - b->captured_us > LS_RECEIVER_TRIAL_FRESH_US;
    }
    t->inactive = a->rx_state != LS_RECEIVER_RX_ACTIVE ||
        b->rx_state != LS_RECEIVER_RX_ACTIVE || !a->parked_known ||
        !b->parked_known || a->parked || b->parked;
    /* Detect decreases even for rejected contexts, but never publish deltas. */
#define RESET(bit, field, known) do { if (a->known && b->known && b->field < a->field) \
    t->counter_reset |= bit; } while (0)
    RESET(LS_TRIAL_IQ, iq_bytes_total, iq_total_known);
    RESET(LS_TRIAL_FRAME, frame_count, frame_count_known);
    RESET(LS_TRIAL_VALID, valid_count, valid_count_known);
    RESET(LS_TRIAL_FAILED, failed_count, failed_count_known);
    RESET(LS_TRIAL_CONTROL, control_count, control_count_known);
    RESET(LS_TRIAL_VOICE, voice_count, voice_count_known);
    RESET(LS_TRIAL_AUDIO_DROP, audio_drop_count, audio_drop_count_known);
#undef RESET
    if (t->time_invalid || t->stale || t->inactive ||
        ((t->unknown | t->changed) & LS_TRIAL_CONTEXT_KEYS)) return;
#define DELTA(bit, field, known, result, epoch) do { \
    if (a->known && b->known && !(t->counter_reset & (bit)) && \
        !((t->unknown | t->changed) & (epoch))) { \
        t->result = b->field - a->field; t->delta_known |= (bit); } } while (0)
    DELTA(LS_TRIAL_IQ, iq_bytes_total, iq_total_known, iq_delta, LS_TRIAL_IQ_EPOCH);
    DELTA(LS_TRIAL_FRAME, frame_count, frame_count_known, frame_delta, LS_TRIAL_DECODER_EPOCH);
    DELTA(LS_TRIAL_VALID, valid_count, valid_count_known, valid_delta, LS_TRIAL_DECODER_EPOCH);
    DELTA(LS_TRIAL_FAILED, failed_count, failed_count_known, failed_delta, LS_TRIAL_DECODER_EPOCH);
    DELTA(LS_TRIAL_CONTROL, control_count, control_count_known, control_delta, LS_TRIAL_DECODER_EPOCH);
    DELTA(LS_TRIAL_VOICE, voice_count, voice_count_known, voice_delta, LS_TRIAL_DECODER_EPOCH);
    DELTA(LS_TRIAL_AUDIO_DROP, audio_drop_count, audio_drop_count_known, audio_drop_delta, LS_TRIAL_DECODER_EPOCH);
#undef DELTA
}

const char *ls_receiver_trial_state(const ls_receiver_trial_t *t)
{
    if (!t || !t->baseline_set) return "NO BASELINE";
    if (!t->compared) return "BASELINE SAVED";
    if (t->time_invalid) return "TIME UNKNOWN/REVERSED";
    if (t->stale) return "STALE - REBASELINE";
    if (t->inactive) return "RECEIVER INACTIVE";
    if (t->changed & LS_TRIAL_CONTEXT_KEYS) return "SETTINGS CHANGED";
    if (t->unknown & LS_TRIAL_CONTEXT_KEYS) return "SETTINGS UNKNOWN";
    if (t->counter_reset) return "COUNTER RESET/WRAP";
    if (t->changed & (LS_TRIAL_IQ_EPOCH | LS_TRIAL_DECODER_EPOCH)) return "COUNTER EPOCH CHANGED";
    return t->delta_known ? "COUNTER DELTAS ONLY" : "COUNTERS UNKNOWN";
}

int ls_receiver_trial_format(const ls_receiver_trial_t *t, char *out, size_t cap)
{
    if (!out || !cap) return 0;
    out[0] = 0;
    diag_writer_t w = {.out=out,.cap=cap};
    put_text(&w,"trial="); put_text(&w,ls_receiver_trial_state(t));
    if (!t) return (int)w.used;
    put_key_u64(&w,"before_us",t->baseline_set && t->before.captured_known,t->before.captured_us);
    put_key_u64(&w,"after_us",t->compared && t->after.captured_known,t->after.captured_us);
    put_key_u64(&w,"changed_mask",t->compared,t->changed);
    put_key_u64(&w,"unknown_mask",t->compared,t->unknown);
    put_key_u64(&w,"reset_mask",t->compared,t->counter_reset);
    put_key_u64(&w,"delta_known_mask",t->compared,t->delta_known);
    put_key_u64(&w,"iq_delta",(t->delta_known & LS_TRIAL_IQ)!=0,t->iq_delta);
    put_key_u64(&w,"frame_delta",(t->delta_known & LS_TRIAL_FRAME)!=0,t->frame_delta);
    put_key_u64(&w,"valid_delta",(t->delta_known & LS_TRIAL_VALID)!=0,t->valid_delta);
    put_key_u64(&w,"failed_delta",(t->delta_known & LS_TRIAL_FAILED)!=0,t->failed_delta);
    put_text(&w,LS_TEXT(t_receiver_diagnostics_rf_coverage_calibration_direction, " rf_coverage=? calibration=? direction=?"));
    return (int)w.used;
}

static void json_string(diag_writer_t *w, const char *s)
{
    put_char(w,'"');
    for (;s && *s;s++) {
        const unsigned char c=(unsigned char)*s;
        if (c=='"' || c=='\\') { put_char(w,'\\'); put_char(w,(char)c); }
        else if (c<32) {
            static const char hex[]="0123456789abcdef";
            put_text(w,"\\u00"); put_char(w,hex[c>>4]); put_char(w,hex[c&15]);
        } else put_char(w,(char)c);
    }
    put_char(w,'"');
}

static void json_u64(diag_writer_t *w,const char *key,bool known,uint64_t v)
{
    put_char(w,','); json_string(w,key); put_char(w,':');
    if (known) put_u64(w,v); else put_text(w,"null");
}

static void json_i32(diag_writer_t *w,const char *key,bool known,int v)
{
    put_char(w,','); json_string(w,key); put_char(w,':');
    if (known) put_i32(w,v); else put_text(w,"null");
}

static void json_text(diag_writer_t *w,const char *key,bool known,const char *v)
{
    put_char(w,','); json_string(w,key); put_char(w,':');
    if (known) json_string(w,v); else put_text(w,"null");
}

static void json_snapshot(diag_writer_t *w,const ls_receiver_diag_t *s)
{
    put_text(w,"{\"rx_state\":"); json_string(w,ls_receiver_rx_state_name(s->rx_state));
    json_u64(w,"captured_us",s->captured_known,s->captured_us);
    json_text(w,"app",s->app_known,s->app);
    json_text(w,"source",s->source_known,s->source);
    json_text(w,"endpoint",s->endpoint_known,s->endpoint_id);
    json_text(w,"owner",s->endpoint_known,s->owner);
    json_text(w,"mode",s->demod[0]!=0,s->demod);
    json_u64(w,"requested_hz",s->requested_frequency_known,s->requested_frequency_hz);
    json_u64(w,"effective_hz",s->effective_frequency_known,s->effective_frequency_hz);
    json_u64(w,"sample_rate_hz",s->sample_rate_known,s->sample_rate_hz);
    json_u64(w,"bandwidth_hz",s->config_known,s->bandwidth_hz);
    json_i32(w,"gain_mode",s->config_known,s->gain_mode);
    json_i32(w,LS_TEXT(t_receiver_diagnostics_requested_gain_tenths_db, "requested_gain_tenths_db"),s->requested_gain_known,s->requested_gain_tenths_db);
    json_i32(w,LS_TEXT(t_receiver_diagnostics_effective_gain_tenths_db, "effective_gain_tenths_db"),s->effective_gain_known,s->effective_gain_tenths_db);
    json_u64(w,"context_epoch",s->context_epoch_known,s->context_epoch);
    json_u64(w,"consumer_epoch",s->consumer_epoch_known,s->consumer_epoch);
    json_u64(w,"iq_epoch",s->iq_epoch_known,s->iq_epoch);
    json_u64(w,"decoder_epoch",s->decoder_epoch_known,s->decoder_epoch);
    json_u64(w,"iq_bytes_total",s->iq_total_known,s->iq_bytes_total);
    json_u64(w,"frame_count",s->frame_count_known,s->frame_count);
    json_u64(w,"valid_count",s->valid_count_known,s->valid_count);
    json_u64(w,"failed_count",s->failed_count_known,s->failed_count);
    json_u64(w,"control_count",s->control_count_known,s->control_count);
    json_u64(w,"voice_count",s->voice_count_known,s->voice_count);
    json_u64(w,"audio_drop_count",s->audio_drop_count_known,s->audio_drop_count);
    put_char(w,'}');
}

int ls_receiver_trial_json(const ls_receiver_trial_t *t,char *out,size_t cap)
{
    if (!out || !cap || !t) return -1;
    out[0]=0;
    diag_writer_t w={.out=out,.cap=cap};
    put_text(&w,LS_TEXT(t_receiver_diagnostics_schema_version_1_type, "{\"schema_version\":1,\"type\":\"receiver_trial\",\"state\":"));
    json_string(&w,ls_receiver_trial_state(t));
    put_text(&w,",\"baseline_set\":"); put_text(&w,t->baseline_set?"true":"false");
    put_text(&w,",\"compared\":"); put_text(&w,t->compared?"true":"false");
    put_text(&w,",\"time_invalid\":"); put_text(&w,t->time_invalid?"true":"false");
    put_text(&w,",\"stale\":"); put_text(&w,t->stale?"true":"false");
    put_text(&w,",\"inactive\":"); put_text(&w,t->inactive?"true":"false");
    json_u64(&w,"elapsed_us",t->compared&&!t->time_invalid,t->elapsed_us);
    json_u64(&w,"changed_mask",t->compared,t->changed);
    json_u64(&w,"unknown_mask",t->compared,t->unknown);
    json_u64(&w,"reset_mask",t->compared,t->counter_reset);
    json_u64(&w,"delta_known_mask",t->compared,t->delta_known);
    json_u64(&w,"iq_delta",(t->delta_known&LS_TRIAL_IQ)!=0,t->iq_delta);
    json_u64(&w,"frame_delta",(t->delta_known&LS_TRIAL_FRAME)!=0,t->frame_delta);
    json_u64(&w,"valid_delta",(t->delta_known&LS_TRIAL_VALID)!=0,t->valid_delta);
    json_u64(&w,"failed_delta",(t->delta_known&LS_TRIAL_FAILED)!=0,t->failed_delta);
    json_u64(&w,"control_delta",(t->delta_known&LS_TRIAL_CONTROL)!=0,t->control_delta);
    json_u64(&w,"voice_delta",(t->delta_known&LS_TRIAL_VOICE)!=0,t->voice_delta);
    json_u64(&w,"audio_drop_delta",(t->delta_known&LS_TRIAL_AUDIO_DROP)!=0,t->audio_drop_delta);
    put_text(&w,",\"before\":");
    if (t->baseline_set) json_snapshot(&w,&t->before); else put_text(&w,"null");
    put_text(&w,",\"after\":");
    if (t->compared) json_snapshot(&w,&t->after); else put_text(&w,"null");
    put_text(&w,LS_TEXT(t_receiver_diagnostics_rf_coverage_null_calibration, ",\"rf_coverage\":null,\"calibration\":null,\"direction\":null}"));
    if (w.truncated) { out[0]=0; return -1; }
    return (int)w.used;
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

    /* newlib's snprintf path reserves 1328 bytes before formatting
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
    put_key_u64(&w,"captured_us",s->captured_known,s->captured_us);
    put_key(&w,"source"); put_text(&w,s->source_known ? s->source : "?");
    put_key_u64(&w,"sample_rate",s->sample_rate_known,s->sample_rate_hz);
    put_key_u64(&w,"bandwidth",s->config_known,s->bandwidth_hz);
    put_key_i32(&w,"gain_mode",s->config_known,s->gain_mode);
    put_key_u64(&w,"context_epoch",s->context_epoch_known,s->context_epoch);
    put_key_u64(&w,"consumer_epoch",s->consumer_epoch_known,s->consumer_epoch);
    put_key_u64(&w,"iq_epoch",s->iq_epoch_known,s->iq_epoch);
    put_key_u64(&w,"decoder_epoch",s->decoder_epoch_known,s->decoder_epoch);
    return (int)w.used;
}
