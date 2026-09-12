#include "flipper_link_telemetry.h"

#include <math.h>   /* lroundf */

#include <stdio.h>
#include <string.h>

#include "ls_board.h"
/* identity helper lives in ble_link_core so the bench test can drive
   it without pulling in the IDF-facing telemetry frontend. */
#include "ble_link_core.h"

#define AC_PER_FRAME 4

/* A ceiling on the whole telemetry line, in bytes. */

#define TEL_WIRE_MAX 320

/* Room kept for append_sys - uptime, heap, stall, health, board - so the
   budget above is not spent on aircraft and then overrun by the suffix. */
#define TEL_SUFFIX_RESERVE 80

static void sanitize(char *s)
{
    for (; *s; s++) {
        if (*s == ' ' || *s == '\t' || *s == '=' || *s == '\r' || *s == '\n') *s = '_';
    }
}

static void copy_text(char *dst, size_t dst_len, const char *src)
{
    if (!src) src = "";
    size_t i = 0;
    for (; i + 1 < dst_len; i++) {
        char c = src[i];
        if (!c) break;
        dst[i] = c;
    }
    dst[i] = '\0';
}

static int append_sys(char *buf, size_t len, int n, const ls_telemetry_common_t *common)
{
    if (!common || n < 0 || (size_t)n >= len - 1) return n;

    int w = snprintf(buf + n, len - (size_t)n,
                     " up=%lu fi=%lu fd=%lu stl=%d rhs=%s",
                     (unsigned long)common->uptime_s,
                     (unsigned long)common->free_internal,
                     (unsigned long)common->free_dma,
                     common->sdr_stall_s,
                     common->rhs_state ? common->rhs_state : "-");
    if (w < 0 || (size_t)(n + w) >= len - 1) return n;
    n += w;

    /* Board identity as its own field so two boards on the same head
       are distinguishable in the app.  Central helper - the format cannot
       silently drift out from under the bench test that pins it. */
    int id = ble_link_format_identity(buf + n, len - (size_t)n, LS_BOARD_NAME);
    if (id <= 0 || (size_t)(n + id) >= len - 1) return n;
    return n + id;
}

int ls_telemetry_build_fm(char *buf, size_t len,
                          const lakeshark_fm_tel_t *t,
                          const ls_telemetry_common_t *common)
{
    /* FLEX joined the FM submodes while this was being
       extracted, and the extraction carried the older list. A short
       array indexed by mode silently reports the wrong name rather
       than failing, so it has to be kept in step with fm_state.h. */
    static const char *sub[] = { "listen", "scan", "pocsag", "wfm",
                                 "acars", "flex" };

    char text[80];
    copy_text(text, sizeof(text), t->pocsag_last_text[0] ? t->pocsag_last_text : "-");
    sanitize(text);

    char ptype[2] = { t->pocsag_last_type ? t->pocsag_last_type : '-', 0 };

    int n = snprintf(buf, len,
                     "$ f=%lu g=%d ef=%llu eg=%d efk=%d egk=%d "
                     "tst=%d ter=%d gst=%d ger=%d rxs=%d rer=%d "
                     "v=%d mu=%d iq=%d rtl=%d re=%d bps=%lu "
                     "md=FM "
                     "fm=%s sq=%d so=%d au=%d "
                     "ss=%lu se=%lu spk=%lu sdb=%d sw=%lu "
                     "pb=%d pau=%d psy=%d pp=%lu pf=%lu pa=%lu pbd=%d pty=%s ptx=%s",
                     (unsigned long)t->freq_hz, t->gain_tenths,
                     (unsigned long long)t->effective_freq_hz,
                     t->effective_gain_tenths,
                     t->effective_freq_known, t->effective_gain_known,
                     t->tune_state, t->tune_error,
                     t->gain_state, t->gain_error,
                     t->receiver_streaming, t->receiver_error,
                     common->volume, common->muted, t->iq_level,
                     common->rtl_ready, t->read_errors,
                     (unsigned long)t->iq_bytes_sec,
                     (unsigned)t->submode < sizeof(sub) / sizeof(sub[0])
                         ? sub[t->submode] : "unknown",
                     t->squelch_tenths, t->squelch_open,
                     t->audio_level, (unsigned long)t->scan_start_hz,
                     (unsigned long)t->scan_stop_hz,
                     (unsigned long)t->scan_peak_hz, t->scan_peak_db,
                     (unsigned long)t->scan_sweeps, t->pocsag_baud,
                     t->pocsag_auto, t->pocsag_sync,
                     (unsigned long)t->pocsag_pages, (unsigned long)t->pocsag_frames,
                     (unsigned long)t->pocsag_last_addr,
                     t->pocsag_last_baud, ptype, text);
    if (n < 0) return n;
    if ((size_t)n >= len - 1) n = (int)len - 1;

    n = append_sys(buf, len, n, common);
    return n;
}

int ls_telemetry_build_adsb(char *buf, size_t len,
                            const lakeshark_adsb_tel_t *t,
                            const ls_telemetry_common_t *common)
{
    static int s_ac_cursor = 0;
    int n = snprintf(buf, len,
                     "$ f=%lu g=%d v=%d mu=%d rtl=%d bps=%lu "
                     "md=ADSB "
                     "ac=%d mt=%d mps=%d cg=%d ce=%d bps1=%d mga=%d mgp=%d lms=%d "
                     "aci=%d acn=%d",
                     (unsigned long)t->freq_hz, t->gain_tenths,
                     common->volume, common->muted,
                     common->rtl_ready, (unsigned long)t->iq_bytes_sec,
                     t->tracked, t->msgs_total, t->msgs_sec, t->crc_good,
                     t->crc_err, t->bursts_sec, t->mag_avg, t->mag_peak,
                     t->last_msg_ms,
                     s_ac_cursor, t->n_aircraft);
    if (n < 0) return n;
    if ((size_t)n >= len - 1) return len - 1;

    if (t->n_aircraft <= 0) s_ac_cursor = 0;
    else if (s_ac_cursor >= t->n_aircraft) s_ac_cursor = 0;

    int sent = 0;
    for (int k = 0; k < AC_PER_FRAME && (size_t)n < len - 1; k++) {
        int idx = s_ac_cursor + k;
        if (idx >= t->n_aircraft) break;

        lakeshark_adsb_ac_t a;
        if (!lakeshark_adsb_aircraft_at(idx, &a)) break;

        char call[12];
        copy_text(call, sizeof(call), a.callsign[0] ? a.callsign : "-");
        sanitize(call);

        /* Position, only when the decoder actually has one. */

        char pos[26];
        pos[0] = '\0';
        if (a.pos_valid) {
            snprintf(pos, sizeof(pos), ",%ld,%ld",
                     (long)lroundf(a.lat * 10000.0f),
                     (long)lroundf(a.lon * 10000.0f));
        }

        int w = snprintf(buf + n, len - (size_t)n,
                         " a%d=%06lX,%s,%d,%d,%d,%d,%d,%d%s",
                         idx, (unsigned long)a.icao, call,
                         a.altitude, a.velocity, a.heading, a.vert_rate,
                         a.age_ms, a.msg_count, pos);
        if (w < 0) break;

        if (n + w + TEL_SUFFIX_RESERVE > TEL_WIRE_MAX) {
            buf[n] = '\0';
            break;
        }
        n += w;
        sent++;
    }
    s_ac_cursor += sent;
    if (sent == 0 || s_ac_cursor >= t->n_aircraft) s_ac_cursor = 0;

    n = append_sys(buf, len, n, common);
    return n;
}

int ls_telemetry_build_p25(char *buf, size_t len,
                            const lakeshark_p25_tel_t *t,
                            const char *mode_name,
                            const char *demod_name,
                            const ls_telemetry_common_t *common)
{
    char ftype[16];
    char err[64];
    copy_text(ftype, sizeof(ftype), t->ftype[0] ? t->ftype : "-");
    copy_text(err, sizeof(err), t->err[0] ? t->err : "-");
    sanitize(ftype);
    sanitize(err);

    int n = snprintf(buf, len,
                     "$ f=%lu dm=%d dmn=%s g=%d agc=%d v=%d mu=%d "
                     "nac=%d tg=%d src=%d na=%d ta=%d sa=%d sy=%d vo=%d sc=%d vc=%d bo=%d bf=%d "
                     "iq=%d pol=%d bp=%d vg=%d rtl=%d rf=%d rs=%d re=%d "
                     "bps=%lu ad=%lu dus=%d md=%s ft=%s e=%s",
                     (unsigned long)t->freq_hz, t->demod_mode,
                     demod_name ? demod_name : "-",
                     t->gain_tenths, t->agc_on, common->volume, common->muted,
                     t->nac, t->tg, t->src,
                     t->nac_age_ms, t->tg_age_ms, t->src_age_ms,
                     t->has_sync, t->voice_active,
                     t->sync_count, t->voice_count, t->bch_ok, t->bch_fail,
                     t->iq_level, t->polarity_inverted, t->beep, t->voice_gate,
                     common->rtl_ready, t->ring_fill, t->ring_size, t->read_errors,
                     (unsigned long)t->iq_bytes_sec, (unsigned long)t->audio_drops,
                     t->decode_us, mode_name ? mode_name : "P25",
                     ftype, err);
    if (n < 0) return n;
    if ((size_t)n >= len - 1) n = (int)len - 1;

    n = append_sys(buf, len, n, common);
    return n;
}

int ls_telemetry_build_rec(char *buf, size_t len,
                           const rec_status_t *s,
                           const ls_telemetry_common_t *common)
{
    char last[24];
    copy_text(last, sizeof(last), s->last_file[0] ? s->last_file : "-");
    sanitize(last);

    int n = snprintf(buf, len,
                     "$ f=%lu g=%d v=%d mu=%d rtl=%d bps=%lu md=REC "
                     "rph=%d red=%d rsp=%lu rmg=%d rfl=%d rth=%d rtf=%d rgp=%d rcp=%lu "
                     "rbw=%lu rmp=%lu rms=%lu rme=%d ren=%d rmn=%lu rmx=%lu rbd=%lu rlf=%s",
                     (unsigned long)s->freq_hz, s->gain_tenths,
                     common->volume, common->muted, common->rtl_ready,
                     (unsigned long)s->bytes_sec,
                     (int)s->phase, s->edges, (unsigned long)s->span_us,
                     s->mag_now, s->mag_floor, s->mag_thresh, s->thresh_fixed, s->gap_ms,
                     (unsigned long)s->captures,
                     (unsigned long)s->bw_hz, (unsigned long)s->min_pulse_us,
                     (unsigned long)(s->max_span_us / 1000u), s->min_edges, s->end_reason,
                     (unsigned long)s->min_mark_us, (unsigned long)s->max_mark_us,
                     (unsigned long)s->baud_est, last);
    if (n < 0) return n;
    if ((size_t)n >= len - 1) n = (int)len - 1;

    n = append_sys(buf, len, n, common);
    return n;
}
