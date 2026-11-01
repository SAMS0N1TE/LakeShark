#include "../../ls_calls.h"
#include "p25_calls_live.h"
#include "../../ls_braille.h"
/* P25 screen. */

#include "../../ls_tui_screen.h"
#include "../../ls_i18n.h"
#include "../../ls_radio_panel.h"
#include "audio/audio_out.h"
#include "scan_engine.h"
#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p25_state.h"
#include "p25_acquisition.h"
#include "p25_tg_observed.h"
#include "esp_attr.h"
#include "iq_app_control.h"
#include "esp_timer.h"

#include "../../ls_quick.h"
#include "../../ls_tui_ui.h"
#include "../../ls_waterfall.h"
#include "../../ls_wf_source.h"
#include "../../ls_text.h"
#include "../../ls_picker.h"
#include "../../ls_radio_select.h"
#include "../../ls_options.h"
#include "../../ls_rffe_ui.h"
#include "../../ls_numpad.h"
#include "../../ls_field.h"
#include "../../ls_notify.h"
#include "p25_program.h"

#include <dirent.h>
#include <strings.h>
#include "ls_textpack.h"

static char s_hint[80] = "LEFT/RIGHT tune  1/2/3 views  +/- volume";
static ls_radio_panel_t s_radio = { .focus = -1 };
static ls_radio_view_t s_view;
#include "../../ls_p25_settings.h"
#include "../../ls_dmr_view.h"

static int s_page;   /* 0 decode, 1 signal, 2 scanner */

/* The same free-running blink REC and FM now use (scr_rec.c,
   scr_fm.c) and MESH originated (scr_mesh.c s_blink): incremented once a
   draw, read back as `(s_blink / 12) & 1`. TG read a number the instant a
   grant landed and held that number, unchanged in appearance, for the rest
   of the call - a live conversation and its own five-second-old tail end
   looked identical. */
static uint32_t s_blink;
EXT_RAM_BSS_ATTR static ls_radio_glass_t s_glass;
static bool s_keypad;
static bool s_entry_bad;
static char s_entry[16];

static void field(tui_surface *sf, tui_rect a, int row, const char *label,
                  const char *value, uint8_t lattr, uint8_t vattr)
{
    tui_put_str(sf, a, a.x + 2, a.y + row, label, lattr);
    tui_put_str(sf, a, a.x + 10, a.y + row, value, vattr);
}

/* What else has been talking, which the firmware has always known and this screen has never said. */

EXT_RAM_BSS_ATTR static p25_tg_observed_snapshot_t s_tg;
static int64_t s_tg_read_us;
static bool    s_tg_have;

static void tg_refresh(int64_t now_us)
{

    if (s_tg_read_us && now_us >= s_tg_read_us &&
        now_us - s_tg_read_us < 1000000) return;
    s_tg_read_us = now_us;
    if (p25_tg_observed_read(&s_tg)) s_tg_have = true;
}

/* Clipping share worth showing. Peaks pin a few samples on any strong
   signal; a fifth of them means the gain is past what the ADC can hold.
   Measured on 154.7850 at 49.6 dB: 5792 of 16384 components, which is what
   too much gain looks like on this receiver. */
#define P25_CLIP_SHOW_PCT     20

/* THE ACTIVITY TABLE, in CALLS' colours: each talkgroup in its own hue,
   its name when the card has one, the last minutes of hits as a braille
   sparkline, then hits, age and where it was seen. Newest first, because a
   scanner's list is read from the top. Brightness is freshness. */
static void draw_activity(tui_surface *sf, tui_rect r, bool sync, int64_t now_us)
{
    const uint8_t frame = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t head  = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);

    tui_box(sf, r, ls_tr("ACTIVITY"), frame);
    if (r.h < 3 || r.w < 24) return;

    /* Already read, by the layout above - it sizes this frame from the count,
       so the count has to be known before the rect exists. */
    const int n = s_tg_have ? (int)s_tg.count : 0;
    if (n <= 0) {
        /* Two different silences, and only one of them is worth waiting
           through. No sync means nothing has been heard BECAUSE nothing is
           being decoded; sync with an empty table is a genuinely quiet
           system. Saying "no traffic" for both would report a broken
           receiver as a quiet channel, which is the failure this panel
           exists to make visible. */
        tui_put_str(sf, r, r.x + 2, r.y + 1,
                    ls_tr(sync ? "synced, nothing heard yet"
                         : "no sync - nothing to hear"),
                    LS_ATTR_DIM);
        /* What to do about it, while there is room: a quiet panel that only
           says "nothing" leaves a newcomer with nowhere to go. */
        static const struct { const char *key, *what; } HOW[] = {
            { "< >", "step the channel, << >> seek" },
            { "T", "type a frequency" },
            { "SCANNER", "search a list of channels" },
            { "R", "pick the radio it listens with" },
            { "GAIN", "raise it until SIG moves, not past CLIP" },
        };
        const int how_n = (int)(sizeof(HOW) / sizeof(HOW[0]));
        for (int i = 0; i < how_n && r.y + 3 + i < r.y + r.h - 1; i++) {
            const int y = r.y + 3 + i;
            tui_put_str(sf, r, r.x + 2, y, HOW[i].key, TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
            tui_put_str(sf, r, r.x + 12, y, ls_tr(HOW[i].what), TUI_ATTR(TUI_WHITE, TUI_BLACK));
        }
        return;
    }

    if (r.h < 4) return;
    const int inner = r.w - 4;
    /* Columns from the right: SEEN 3, AGE 4, HITS 5; the name (or the
       channel, when it is not this one) takes nine; the sparkline gets the
       rest, sixteen minutes at most. */
    const int tail = 3 + 1 + 4 + 1 + 5 + 1;
    int minutes = inner - 7 - 10 - tail;
    if (minutes > P25_TG_OBSERVED_MINUTES) minutes = P25_TG_OBSERVED_MINUTES;
    if (minutes < 4) minutes = 0;
    const int x_name = r.x + 2 + 7;
    const int x_spark = x_name + 10;
    const int x_tail = r.x + r.w - 2 - tail;
    tui_put_str(sf, r, r.x + 2, r.y + 1, ls_tr("TALKGROUP"), head);
    if (minutes) {
        char span[16];
        snprintf(span, sizeof(span), minutes >= 12 ? "LAST %d MIN" : "%dM", minutes);
        tui_put_str(sf, r, x_spark, r.y + 1, span, frame);
    }
    tui_put_str(sf, r, x_tail + 1, r.y + 1, "HITS  AGE SEEN", head);

    const uint32_t now_ms = (uint32_t)(now_us / 1000);
    const int rows = r.h - 3;
    uint16_t on_tg = 0;
    bool on_air = false;
    if (!p25_calls_live_now(&on_tg, NULL, &on_air, NULL) || !on_air) on_tg = 0;

    /* One scale for every row, so a taller sparkline is a busier talkgroup. */
    unsigned top = 1;
    for (int i = 0; i < n; i++)
        for (int m = 0; m < minutes; m++) {
            const unsigned v = p25_tg_observed_minute_hits(&s_tg.rows[i], now_ms, (unsigned)m);
            if (v > top) top = v;
        }

    uint32_t used = 0;
    for (int slot = 0; slot < rows && slot < n; slot++) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (used & (1u << i)) continue;
            if (best < 0 ||
                s_tg.rows[i].last_seen_ms > s_tg.rows[best].last_seen_ms)
                best = i;
        }
        if (best < 0) break;
        used |= 1u << best;

        const p25_tg_observed_row_t *row = &s_tg.rows[best];
        const int y = r.y + 2 + slot;
        const uint32_t age_ms = now_ms - row->last_seen_ms;
        /* Thirty seconds is the span a P25 call and its tail occupy, so a
           row brighter than the rest is one that is or has just been live;
           five minutes is still warm. */
        const bool hot = now_ms >= row->last_seen_ms && age_ms < 30000;
        const bool warm = now_ms >= row->last_seen_ms && age_ms < 300000;
        const uint8_t hue = ls_calls_tg_hue(row->talkgroup);
        const uint8_t ink = TUI_ATTR(hot ? hue | TUI_BRIGHT : warm ? hue : LS_FAINT_FG,
                                     TUI_BLACK);
        char text[24];
        if (on_tg && row->talkgroup == on_tg)
            tui_put_char(sf, r, r.x + 1, y, '>', TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
        snprintf(text, sizeof(text), "%-5u", (unsigned)row->talkgroup);
        tui_put_str(sf, r, r.x + 2, y, text, ink);

        /* The channel only when it is not the one being listened to. The
           store keys on frequency as well as talkgroup, deliberately, so the
           table can hold rows from a channel this radio has since left - and
           those must not read as traffic on the current one. The tolerance is
           half a 12.5 kHz channel because `channel_hz` is the tuner's
           EFFECTIVE centre, which lands a few kHz off what was asked for;
           comparing for equality would mark every row off-channel. */
        const uint64_t tuned = (uint64_t)s_tune_freq_hz;
        const uint64_t chan  = row->channel_hz;
        const uint64_t delta = chan > tuned ? chan - tuned : tuned - chan;
        const tui_rect name_box = tui_rect_make(x_name, y, (minutes ? x_spark : x_tail) - x_name - 1, 1);
        if (chan && delta > 6250ULL) {
            snprintf(text, sizeof(text), "%u.%04u",
                     (unsigned)(chan / 1000000ULL),
                     (unsigned)((chan % 1000000ULL) / 100ULL));
            tui_put_str(sf, name_box, x_name, y, text, TUI_ATTR(TUI_CYAN, TUI_BLACK));
        } else {
            const char *alias = p25_calls_alias(row->talkgroup);
            if (alias)
                tui_put_str(sf, name_box, x_name, y, alias,
                            TUI_ATTR(hot ? (TUI_WHITE | TUI_BRIGHT) : LS_DIM_FG, TUI_BLACK));
        }

        /* Oldest minute at the left; a floor of dots where nothing was
           heard, so the row reads as a timeline, not as missing data. */
        for (int cell = 0; cell < minutes; cell++) {
            const unsigned v = p25_tg_observed_minute_hits(row, now_ms,
                                                           (unsigned)(minutes - 1 - cell));
            unsigned bits;
            if (v) {
                unsigned h = 1;
                while (h < 4 && v * 16u > top * h * h) h++;   /* about a square root */
                bits = ls_braille_column(h, 0) | ls_braille_column(h, 1);
            } else {
                bits = ls_braille_dot(0, 0) | ls_braille_dot(0, 1);
            }
            ls_braille_put(sf, r, x_spark + cell, y, (int16_t)(0x2800 | bits),
                           v ? ink : LS_ATTR_FAINT);
        }

        if (row->hits >= 100000)
            snprintf(text, sizeof(text), "%luk", (unsigned long)(row->hits / 1000));
        else
            snprintf(text, sizeof(text), "%lu", (unsigned long)row->hits);
        tui_put_str(sf, r, x_tail + 5 - (int)strlen(text), y, text,
                    TUI_ATTR(TUI_YELLOW | (hot ? TUI_BRIGHT : 0), TUI_BLACK));
        char age[8];
        /* Seconds: ls_age_str counts them. Handed milliseconds, as this
           table did, a talkgroup heard 22 s ago read as seven hours. */
        ls_age_str(age, sizeof(age), row->last_seen_ms / 1000u, now_ms / 1000u);
        tui_put_str(sf, r, x_tail + 10 - (int)strlen(age), y, age,
                    hot ? TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);

        /* Where it was seen answers a different question from how often: a
           link control word is the talkgroup of a call in progress, a header
           is one starting, and a grant is one being set up on another
           channel - so a row seen only in grants is traffic this radio is
           not listening to. Each letter in its own colour. */
        int sx = x_tail + 11;
        if (row->sources & P25_TG_SEEN_LCW)
            tui_put_char(sf, r, sx++, y, 'L', TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
        if (row->sources & P25_TG_SEEN_HDU)
            tui_put_char(sf, r, sx++, y, 'H', TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        if (row->sources & P25_TG_SEEN_GRANT)
            tui_put_char(sf, r, sx++, y, 'G', TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    }
}

static const char *voice_status(void)
{
    if (P25.p25_enc_muted)
        return P25.p25_ess_valid ? "encrypted" : "waiting for voice header";
    return P25.voice_active_until_us > esp_timer_get_time()
           ? "voice decoded" : "waiting for voice";
}

static void draw_iq_gauge(tui_surface *sf, tui_rect r, float level);

static void draw_receiver_health(tui_surface *sf, tui_rect r)
{
    tui_box(sf, r, ls_tr("RECEIVER HEALTH / TOTALS"), TUI_ATTR(TUI_CYAN, TUI_BLACK));
    const uint8_t ink = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    /* Pair counters whenever the pane is wide enough: spaced single
       columns left most of a desk window's pane black. */
    const bool compact = r.h >= 5 && r.w >= 50;
    const int step = !compact && r.h >= 11 ? 2 : 1;
    int sync_end, bch_end;
    char line[80];
    uint8_t first = ink;
    if (P25.lr_iq_active)
        LS_SNPRINTF(t_scr_p25_iq_blocks_lu_bad, line, sizeof(line), "IQ blocks %lu  bad %lu  gaps %lu",
                 (unsigned long)P25.lr_iq_good, (unsigned long)P25.lr_iq_bad,
                 (unsigned long)P25.lr_iq_gaps);
    else if (P25.lora_rx)
        LS_SNPRINTF(t_scr_p25_s_lu_b_s, line, sizeof(line), "%s %lu B/s  RSSI %.0f dBm", ls_rsel_name(LS_RSEL_LORA),
                 (unsigned long)P25.iq_bytes_sec, (double)P25.lora_rssi_dbm);
    else if (P25.lora_wait) {
        snprintf(line, sizeof(line), ls_tr("%s IN USE ELSEWHERE"), ls_rsel_name(LS_RSEL_LORA));
        first = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    } else
        snprintf(line, sizeof(line), ls_tr("IQ %lu B/s  AUDIO %lu samples/s"),
                 (unsigned long)P25.iq_bytes_sec, (unsigned long)P25.audio_samples_sec);
    tui_put_str(sf, r, r.x + 2, r.y + 1, line, first);
    snprintf(line, sizeof(line), ls_tr("SYNC %d   VOICE %d"), P25.dsd_sync_count, P25.dsd_voice_count);
    tui_put_str(sf, r, r.x + 2, r.y + 1 + step, line, ink);
    sync_end = 4 + (int)strlen(line);
    snprintf(line, sizeof(line), ls_tr("BCH OK %d   FAIL %d"), P25.dsd_bch_ok_count, P25.dsd_bch_fail_count);
    tui_put_str(sf, r, r.x + 2, r.y + (compact ? 3 : 1 + step * 2), line, ink);
    bch_end = 4 + (int)strlen(line);
    snprintf(line, sizeof(line), ls_tr("%s errors %lu  AUDIO drops %lu"),
             P25.lora_rx || P25.lora_wait ? ls_tr("Read") : "USB",
             (unsigned long)P25.read_errors_total, (unsigned long)P25.audio_drops);
    tui_put_str(sf, r, r.x + (compact ? bch_end : 2),
                r.y + (compact ? 3 : 1 + step * 3), line, ink);
    if (r.h >= 7 || compact) {
        snprintf(line, sizeof(line), ls_tr("DECODE %.1f ms / block"), (double)P25.dsd_decode_ms);
        tui_put_str(sf, r, r.x + (compact ? sync_end : 2),
                    r.y + (compact ? 2 : 1 + step * 4), line, LS_ATTR_DIM);
    }
    /* How many frames pass their check, as a bar: the one figure here that
       says at a glance whether the decode is healthy. */
    const int total = P25.dsd_bch_ok_count + P25.dsd_bch_fail_count;
    if (compact && r.h >= 6) {
        const float ok = total ? (float)P25.dsd_bch_ok_count / (float)total : 0.0f;
        tui_put_str(sf, r, r.x + 2, r.y + 4, ls_tr("FRAMES OK"), TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        snprintf(line, sizeof(line), total ? "%3d%%" : " --", (int)(ok * 100.0f + 0.5f));
        tui_put_str(sf, r, r.x + r.w - 6, r.y + 4, line,
                    TUI_ATTR(!total ? LS_DIM_FG : ok >= 0.8f ? TUI_GREEN | TUI_BRIGHT
                             : ok >= 0.4f ? TUI_YELLOW | TUI_BRIGHT : TUI_RED | TUI_BRIGHT, TUI_BLACK));
        ls_radio_bargraph(sf, r, r.x + 12, r.y + 4, r.w - 20, 1, ok, ok);
    }
}

/* The receiver's level over the last few seconds, under the health counters
   in a wide pane: the same trace the glass keeps, given the room. */
static void draw_signal_history(tui_surface *sf, tui_rect r)
{
    tui_box(sf, r, ls_tr("SIGNAL / LAST SECONDS"), TUI_ATTR(TUI_CYAN, TUI_BLACK));
    if (r.w < 16 || r.h < 3) return;
    /* In decibels, -60 to 0: on a straight scale the few percent a channel
       sits at between calls is no height at all, and the box read as dead. */
    const int h = r.h - 2, x0 = r.x + 6, w = r.w - 8;
    static const char *const AXIS[3] = { "  0", "-30", "-60" };
    for (int k = 0; k < 3 && h >= 3; k++)
        tui_put_str(sf, r, r.x + 2, r.y + 1 + k * (h - 1) / 2, AXIS[k], LS_ATTR_DIM);
    const int n = s_glass.trace_n < w ? s_glass.trace_n : w;
    for (int i = 0; i < w; i++) {
        const int idx = s_glass.trace_n - w + i;
        int v = 0;
        if (idx >= 0 && idx < s_glass.trace_n)
            v = s_glass.trace[(s_glass.trace_head - s_glass.trace_n + idx + LS_RADIO_TRACE_N) % LS_RADIO_TRACE_N];
        float db = v > 0 ? 20.0f * log10f((float)v / 255.0f) : -99.0f;
        int q = db <= -60.0f ? 0 : (int)((db + 60.0f) / 60.0f * (float)(h * 8));
        if (q > h * 8) q = h * 8;
        if (i < w - n || !q) {
            tui_put_char(sf, r, x0 + i, r.y + h, LS_TUI_TRACE(1), TUI_ATTR(LS_FAINT_FG, TUI_BLACK));
            continue;
        }
        for (int row = 0; row < h; row++) {
            const int base = (h - 1 - row) * 8, fill = q - base;
            if (fill < 1) continue;
            const uint8_t hue = base >= 6 * h ? (TUI_RED | TUI_BRIGHT) : base >= 3 * h ? (TUI_YELLOW | TUI_BRIGHT)
                              : (TUI_GREEN | TUI_BRIGHT);
            tui_put_char(sf, r, x0 + i, r.y + 1 + row, LS_TUI_TRACE(fill > 8 ? 8 : fill), TUI_ATTR(hue, TUI_BLACK));
        }
    }
}

/* Rows the merged DECODE / SIGNAL window asks for in portrait: six lines
   and a frame. */
#define MERGED_ROWS 8

static int kv(tui_surface *sf, tui_rect box, int x, int y, const char *k,
              const char *v, uint8_t vattr)
{
    k = ls_tr(k);
    v = ls_tr(v);
    tui_put_str(sf, box, x, y, k, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    x += (int)strlen(k) + 1;
    tui_put_str(sf, box, x, y, v, vattr);
    return x + (int)strlen(v) + 3;
}

static const char *receive_label(void)
{
    static const char *const names[] = {
        "CONTROL SEARCH", "CONTROL LOCKED", "GRANT / TRAFFIC WAIT",
        "TRAFFIC SYNC", "VOICE", "ENCRYPTED", "TUNE FAILED"
    };
    if (P25.receive_state == P25_RX_TUNE_FAILED) return "TUNE FAILED";
    if (!P25.grant_on_traffic && P25.grant_observed.support != P25_CALL_NONE) {
        if (P25.grant_observed.support == P25_CALL_MISSING_IDEN || P25.grant_observed.support == P25_CALL_INVALID) return "UNRESOLVED GRANT";
        if (P25.grant_observed.support == P25_CALL_UNSUPPORTED || (P25.grant_observed.support == P25_CALL_PHASE2 && !P25.phase2_follow)) return "UNSUPPORTED GRANT";
    }
    unsigned state = (unsigned)P25.receive_state;
    return state < sizeof(names) / sizeof(names[0]) ? names[state] : "CONTROL SEARCH";
}

static void draw_merged(tui_surface *sf, tui_rect r, int nac, int tg, int src,
                        const char *mod, bool sync, float level, bool voice,
                        const ls_iq_control_status_t *st)
{
    const uint8_t value = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t good  = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t idle  = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    const uint8_t warn  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    tui_box(sf, r, ls_tr("DECODE / SIGNAL"), TUI_ATTR(TUI_CYAN, TUI_BLACK));
    const int inner = r.h - 2;
    const int iw = r.w - 4;
    if (inner < 1 || iw < 16) return;
    const int x0 = r.x + 2;

    char n[12], t[12], u[12], iq[32], rx[16];
    if (p25_nac_valid(&P25)) snprintf(n, sizeof(n), "%03X", nac); else snprintf(n, sizeof(n), "---");
    if (tg > 0) snprintf(t, sizeof(t), "%d", tg); else snprintf(t, sizeof(t), "---");
    if (src > 0) snprintf(u, sizeof(u), "%d", src); else snprintf(u, sizeof(u), "---");

    p25_acquisition_status_t acq;
    p25_get_acquisition_status(&acq);
    const uint32_t comps = acq.iq.sampled_pairs * 2u;
    const int clip_pct = comps ? (int)((acq.iq.clipped_components * 100u) / comps) : 0;
    const bool fsk_rx = P25.lora_rx && !P25.lr_iq_active;
    const char *iqk = fsk_rx ? "RSSI" : "IQ";
    if (fsk_rx) snprintf(iq, sizeof(iq), "%.0f dBm", (double)P25.lora_rssi_dbm);
    else if (clip_pct >= P25_CLIP_SHOW_PCT)
        snprintf(iq, sizeof(iq), "%.3f CLIP %d%%", (double)level, clip_pct);
    else snprintf(iq, sizeof(iq), "%.3f", (double)level);
    snprintf(rx, sizeof(rx), "%s", P25.lr_iq_active
             ? (st->receiver_streaming ? "STREAM I/Q" : "I/Q WAIT")
             : st->receiver_streaming ? "STREAMING" : "STOPPED");
    const char *vs = voice_status();
    const uint8_t iqattr = clip_pct >= P25_CLIP_SHOW_PCT ? warn : value;

    /* Lines in reading order; the gauge is the first to go when rows run out,
       because the IQ number says the same thing. */
    int y = r.y + 1;
    const int last = r.y + r.h - 2;
    const bool gauge = inner >= 6;
    /* Air between lines only when there is plenty. */
    const int step = inner >= 11 ? 2 : 1;
    int x;
    #define ROW_OK() (y <= last)
    if (ROW_OK()) {
        x = kv(sf, r, x0, y, "NAC", n, p25_nac_valid(&P25) ? value : idle);
        x = kv(sf, r, x, y, "TG", t, tg > 0 ? value : idle);
        if (voice && ((s_blink / 12) & 1) && x - 2 < r.x + r.w - 2)
            tui_put_char(sf, r, x - 2, y, LS_TUI_SHADE_FULL, good);
        y += step;
    }
    if (ROW_OK()) {
        x = kv(sf, r, x0, y, "UNIT", u, src > 0 ? value : idle);
        kv(sf, r, x, y, "MOD", mod[0] ? mod : "----", value);
        y += step;
    }
    if (ROW_OK()) {
        kv(sf, r, x0, y, "SYNC", receive_label(), sync ? good : idle);
        y += step;
    }
    if (gauge && ROW_OK()) {
        draw_iq_gauge(sf, tui_rect_make(x0, y, iw, 1), level);
        y += step;
    }
    if (ROW_OK()) {
        x = kv(sf, r, x0, y, iqk, iq, iqattr);
        if (x - 3 + 3 + (int)strlen(rx) + 3 <= r.x + r.w - 2) {
            kv(sf, r, x, y, "RX", rx, st->receiver_streaming ? good : idle);
            rx[0] = 0;
        }
        y += step;
    }
    if (rx[0] && ROW_OK()) {
        kv(sf, r, x0, y, "RX", rx, st->receiver_streaming ? good : idle);
        y += step;
    }
    if (ROW_OK())
        kv(sf, r, x0, y, "AUDIO", vs, P25.p25_enc_muted ? warn : idle);
    #undef ROW_OK
}

static void tune_draw(tui_surface *sf, tui_rect area);

static void draw_decode(tui_surface *sf, tui_rect area)
{
    /* One read, at the top. See the header note. */
    const int      nac   = P25.dsd_nac;
    const int      tg    = P25.dsd_tg;
    const int      src   = P25.dsd_src;
    const bool     sync  = P25.dsd_has_sync;
    const float    level = P25.iq_level;
    const uint32_t freq  = s_tune_freq_hz;
    const bool     voice = P25.voice_active_until_us > esp_timer_get_time();
    char mod[8];
    snprintf(mod, sizeof(mod), "%.7s", P25.dsd_modulation);

    const uint8_t frame = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t label = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t value = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t idle  = TUI_ATTR(TUI_WHITE, TUI_BLACK);

    if (area.w < 20 || area.h < 3) return;

    if (P25.split_enabled && area.h >= 9) {
        tui_rect roles = tui_rect_make(area.x, area.y, area.w, 5);
        tui_box(sf, roles, ls_tr("P25 SPLIT"), frame);
        char roles_line[80];
        LS_SNPRINTF(t_scr_p25_cc_s_site_u, roles_line, sizeof(roles_line), "CC: %s site %u/%u CRC %u%%",
            P25.split_active ? "LR2021" : "RTL fallback",
            P25.split_rfss, P25.split_site, P25.split_crc_percent);
        tui_put_str(sf, roles, roles.x + 1, roles.y + 1, roles_line, label);
        if (P25.grant_on_traffic)
            LS_SNPRINTF(t_scr_p25_voice_rtl_tg_u, roles_line, sizeof(roles_line), "VOICE: RTL TG %u %.4f MHz",
                P25.grant_talkgroup, (double)P25.grant_freq_hz / 1e6);
        else snprintf(roles_line, sizeof(roles_line), "VOICE: RTL idle");
        tui_put_str(sf, roles, roles.x + 1, roles.y + 2, roles_line, value);
        tui_put_str(sf, roles, roles.x + 1, roles.y + 3, P25.split_reason, idle);
        area.y += 5;
        area.h -= 5;
    }

    tg_refresh(esp_timer_get_time());
    const int held = (s_tg_have && s_tg.count) ? (int)s_tg.count : 0;
    const int fits = held ? 3 + held : 3;
    const bool wide = area.w >= 60;
    /* The same stacked glass as FM, with a side column for digital decode.
       Leave the existing history/health windows the rest of the pane. */
    const int join = wide && !s_keypad && area.h <= 18 ? 1 : 0;
    const int tune_rows = !wide && !s_keypad && area.w >= 47 && area.h >= 35 ? 8 : 0;
    int dial_want = wide ? (area.h >= 15 || s_keypad ? 11 : 10)
                         : s_keypad && area.w >= 51 ? 11 : area.h >= 25 ? 14 : 8;
    /* Pay for TUNE with empty glass/trace rows; retain decode, activity and
       every health counter. The short landscape glass uses compact figures. */
    if (!s_keypad) {
        if (tune_rows && dial_want > 12) dial_want = 12;
        int spare = wide ? area.h - (area.w >= 104 ? 5 : 7) + join : area.h - tune_rows - MERGED_ROWS - 7 - fits;
        /* A full activity history may shorten its visible tail, never
           evict health or the receiver readouts to keep empty glass rows. */
        if (spare < 7) spare = 7;
        if (dial_want > spare) dial_want = spare;
    }
    const int top_h = area.h < dial_want ? area.h : dial_want;
    tui_rect dial = tui_rect_make(area.x, area.y,
                                  wide ? area.w * 11 / 20 : area.w, top_h);
    tui_rect detail = wide
        ? tui_rect_make(dial.x + dial.w + 1, area.y,
                        area.w - dial.w - 1, top_h)
        : tui_rect_make(area.x, area.y + top_h, area.w,
                        area.h > top_h ? area.h - top_h : 0);
    if (tune_rows) {
        tune_draw(sf, tui_rect_make(detail.x, detail.y, detail.w, 7));
        detail.y += tune_rows; detail.h -= tune_rows;
    }
    tui_rect activity = tui_rect_make(0, 0, 0, 0);

    ls_iq_control_status_t st;
    p25_get_receiver_status(&st);
    /* THE GLASS. The scanner's bezel and its rail of lit annunciators, so the
       two pages of one radio read as one instrument. The title keeps its
       words - they are what a test and a thumb both look for. */
    ls_radio_glass_bezel(sf, dial, s_keypad ? "ENTER FREQUENCY"
                               : p25_p2_enabled() ? "P25 II EXP / RECEIVER"
                                                  : "P25 / RECEIVER");
    char buf[64];
    if (dial.h >= 7) {
        ls_radio_chip_t chips[5];
        int n = 0;
        chips[n++] = (ls_radio_chip_t){ p25_p2_enabled() ? "P2" : "P1", true,
                                      p25_p2_enabled() ? TUI_MAGENTA : TUI_BLUE };
        chips[n++] = (ls_radio_chip_t){ sync ? "SYNC" : "NO SYNC", sync, TUI_GREEN };
        if (P25.p25_enc_muted)
            chips[n++] = (ls_radio_chip_t){ "ENC", true, TUI_RED };
        if (scan_engine_active())
            chips[n++] = (ls_radio_chip_t){ scan_engine_manual_hold() ? "HOLD" : "SCAN",
                                          true, TUI_YELLOW };
        if (P25.split_enabled)
            chips[n++] = (ls_radio_chip_t){ "TRUNK", P25.split_active, TUI_CYAN };
        ls_radio_glass_rail(sf, dial, dial.x + 1, dial.y + 1, dial.w - 2,
                            chips, n, NULL, 0);
    }
    /* Animate requested retunes, never receiver centre-estimate jitter. */
    tui_rect figures = dial;
    figures.h -= 2; /* reserve receiver line and meter below the figures */
    int below = ls_radio_glass_frequency(&s_glass, sf, figures, dial.y + 2,
                                         freq, s_keypad ? s_entry : NULL,
                                         s_blink, false);
    ls_radio_glass_wipe(&s_glass, sf, dial, dial.y + 2, below, p25_p2_enabled() ? 2 : 0);
    if (below <= dial.y + dial.h - 2) {
        if (scan_engine_active()) scan_engine_status(buf, sizeof(buf));
        else if (P25.lr_iq_active)
            snprintf(buf, sizeof(buf), "%s", ls_tr("EXPERIMENTAL I/Q / CAPTURE GAPS"));
        else if (P25.lora_rx && st.receiver_streaming)
            snprintf(buf, sizeof(buf), ls_tr("MANUAL / %s ONLINE"), ls_rsel_name(LS_RSEL_LORA));
        else if (P25.lora_wait && !st.receiver_streaming)
            snprintf(buf, sizeof(buf), ls_tr("%s IN USE ELSEWHERE"), ls_rsel_name(LS_RSEL_LORA));
        else snprintf(buf, sizeof(buf), "%s", ls_tr(st.receiver_streaming
                      ? "MANUAL / RECEIVER ONLINE" : "NO USB RECEIVER"));
        if (s_keypad) {
            const uint32_t rx = st.effective_center_known ? (uint32_t)st.effective_center_hz : freq;
            snprintf(buf, sizeof(buf), "RX %u.%04u MHz", (unsigned)(rx / 1000000u),
                     (unsigned)((rx % 1000000u) / 100u));
        }
        tui_put_str(sf, dial, dial.x + 2, below++, buf,
                    scan_engine_active() ? value : idle);
    }

    const float signal = level < 0.0f ? 0.0f : level > 1.0f ? 1.0f : level;
    ls_radio_glass_sample(&s_glass, st.receiver_streaming ? signal : 0.0f);
    const int meter_y = dial.y + dial.h - 2;
    const int trace_h = meter_y - below - 1;
    if (trace_h >= 2) {
        tui_put_str(sf, dial, dial.x + 2, below, ls_tr("ACT"), label);
        ls_radio_glass_trace(&s_glass, sf, dial, dial.x + 6, below,
                             dial.w - 10, trace_h, 0.0f);
    }
    if (below <= meter_y)
        ls_radio_glass_meter(&s_glass, sf, dial, dial.x + 2, meter_y,
                             dial.w - 4, 1, signal, st.receiver_streaming,
                             sync, false, 0.0f, NULL);

    /* One window for what is decoded and how well it is received. It was
       two boxes in portrait (DECODE, SIGNAL) and a spectrum page behind a
       tab; the numbers a thumb watches sit together now. */
    tui_rect mbox = wide ? detail
        : tui_rect_make(detail.x, detail.y, detail.w,
                        detail.h < MERGED_ROWS ? detail.h : !s_keypad && area.h < 25 ? MERGED_ROWS - 1 : MERGED_ROWS);
    if (mbox.h >= 3)
        draw_merged(sf, mbox, nac, tg, src, mod, sync, level, voice, &st);
    if (wide) {
        /* Share a frame rule; health pairs counters before activity or
           receiver rows are squeezed further. All six decode lines stay. */
        int ah = area.h - top_h + join;
        if (ah >= 3) activity = tui_rect_make(area.x, area.y + top_h - join, area.w, ah);
        if ((ah >= 7 && area.w >= 92) || (ah >= 5 && area.w >= 104)) {
            activity.w = area.w / 2;
            const int hx = activity.x + activity.w + 1, hw = area.w - activity.w - 1;
            /* The counters take the rows they fill; the trace gets the rest. */
            const int hh = hw >= 50 && ah >= 11 ? 7 : ah;
            draw_receiver_health(sf, tui_rect_make(hx, activity.y, hw, hh));
            if (ah - hh >= 4) draw_signal_history(sf, tui_rect_make(hx, activity.y + hh, hw, ah - hh));
        }
    } else {
        const int ah = detail.h - mbox.h;
        if (ah >= 3) {
            activity = tui_rect_make(area.x, mbox.y + mbox.h, area.w, ah);
            if (ah >= fits + 7 || (tune_rows && ah >= 10)) {
                activity.h = ah - 7;
                draw_receiver_health(sf, tui_rect_make(area.x,
                    activity.y + activity.h, area.w, 7));
            }
        }
    }

    if (activity.h > 0) draw_activity(sf, activity, sync, esp_timer_get_time());

}

/* A level, since landscape has nowhere else to put one. */

static void draw_iq_gauge(tui_surface *sf, tui_rect r, float level)
{
    if (r.h < 1 || r.w < 16) return;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    tui_put_str(sf, r, r.x, r.y, ls_tr("SIG"),
                TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));

    char pct[6];
    snprintf(pct, sizeof(pct), "%3d%%", (int)(level * 100.0f + 0.5f));
    tui_put_str(sf, r, r.x + r.w - 4, r.y, pct,
                TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));

    /* The scanner's VFD bargraph, with the peak it just held decaying behind
       the reading - the only animation on this row, and it costs one float.
       Filaments, not a filled block: a hundred readings must not run together
       into one coloured shape. */
    static float peak;
    if (level >= peak) peak = level;
    else               peak -= (peak - level) * 0.06f;
    ls_radio_bargraph(sf, r, r.x + 4, r.y, r.w - 9, 1, level, peak);
}

static void draw_signal(tui_surface *sf, tui_rect area)
{
    ls_wf_note_full_view();
    ls_wf_source_select(LS_WF_SRC_P25);
    ls_wf_source_pump();

    if (area.h > 10) {
        char status[48];
        snprintf(status, sizeof(status), "Audio: %s", voice_status());
        tui_rect status_row = tui_rect_make(area.x, area.y + area.h - 1, area.w, 1);
        tui_put_str(sf, status_row, status_row.x + 1, status_row.y, status,
                    P25.p25_enc_muted ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
        if (area.w >= 90)
            draw_iq_gauge(sf, tui_rect_make(area.x + 48, status_row.y, area.w - 48, 1), P25.iq_level);
        area.h--;
    }

    if (P25.lora_rx && !P25.lr_iq_active) {
        ls_panel_notice(sf, area, "SPECTRUM", "LR IQ acquiring", LS_TEXT(t_scr_p25_waiting_for_the_iq, "waiting for the IQ stream"));
        return;
    }
    const char *why = ls_wf_idle_reason();
    /* Keep HOLD reachable while the shared display is paused. */
    if (why && !ls_wf_cfg()->paused) {
        ls_panel_notice(sf, area, "SPECTRUM", why,
                        LS_TEXT(t_scr_p25_the_receiver_feeds_this, "the receiver feeds this once it is streaming"));
        return;
    }

    ls_wf_draw(sf, area);
}

/* Pages are buttons, not a key you have to know about.

   LEFT/RIGHT still turns the page and always will - it is the fastest thing
   with a keyboard attached. But with no keyboard the only clue that a second
   page existed was the hint bar, and the hint bar is a legend, not a control.
   Two fat buttons cost two rows in portrait and one in landscape, and they
   are the same action from either input. */
/* ----------------------------------------- keeping an observation --- */

/* P25 kept nothing. A site hit - control channel, NAC, talkgroup, the signal
   it was heard at - is the most obviously recordable thing this board sees,
   and until now the only way to keep one was to write the numbers down off
   the glass. ls_field_mark_radio does exactly this shape of write already;
   four other screens use it.

   Manual, not automatic, on purpose. The journal is a notebook: MIX_RF.md
   states the rule as "no automatic SD files; only explicit marks enqueue
   notes through Journal", and a trunked system would fill the 48-entry ring
   in under a minute if every grant kept itself. */

static void mark_observation(void)
{
    char title[48];
    char text[512];

    const bool sync = P25.dsd_has_sync;
    snprintf(title, sizeof(title), sync ? "P25 site NAC %03X" : "P25 search",
             P25.dsd_nac);

    size_t n = (size_t)snprintf(text, sizeof(text),
        "%.4f MHz, %s. ", s_tune_freq_hz / 1e6,
        sync ? "synchronised" : "no sync at the time this was kept");

    if (sync && n < sizeof(text))
        n += (size_t)LS_SNPRINTF(t_scr_p25_nac_03x_talkgroup_d, text + n, sizeof(text) - n,
            "NAC %03X, talkgroup %d, unit %d, %s. ",
            P25.dsd_nac, P25.dsd_tg, P25.dsd_src,
            P25.dsd_modulation[0] ? P25.dsd_modulation : "modulation unknown");

    if (n < sizeof(text))
        n += (size_t)LS_SNPRINTF(t_scr_p25_signal_2f_of_full, text + n, sizeof(text) - n,
            "Signal %.2f of full scale, gain %.1f dB. "
            "%d sync events and %d voice frames this session. Audio: %s. ",
            (double)P25.iq_level, P25.rtl_gain_tenths / 10.0,
            P25.dsd_sync_count, P25.dsd_voice_count, voice_status());

    /* Said plainly, the way the sub-GHz bookmark says it: the attachment is
       where the RECEIVER was when the note was kept. It is not where the
       transmitter is, and a trunked site can be tens of kilometres away. */
    if (n < sizeof(text))
        LS_SNPUTS(t_scr_p25_the_position_and_motion, text + n, sizeof(text) - n, "The position and motion attached describe this receiver when "
            "the note was kept, not the location of the transmitter.");

    const bool ok = ls_field_mark_radio(title, text, LS_FIELD_RTL,
                                        (uint32_t)s_tune_freq_hz,
                                        (uint32_t)P25.dsd_sync_count);

    /* Through the banner, not a status field this screen would have to find
       room for. It also gives the operator somewhere to go: tapping it opens
       JOURNAL, which is where the answer about whether it saved actually is. */
    ls_notice_t note;
    memset(&note, 0, sizeof(note));
    snprintf(note.title, sizeof(note.title), "P25");
    snprintf(note.body, sizeof(note.body), "%s",
             ok ? "observation kept - check JOURNAL for the save"
                : "journal busy - try again shortly");
    note.hue = TUI_GREEN;
    /* By name over the registry rather than an extern to another screen's
       descriptor: the notice carries a registry index, and P25 has no
       business holding a pointer to JOURNAL to get one. -1 if it is not
       registered, which is what a build without it should do. */
    note.screen = -1;
    for (int i = 0; i < ls_tui_screen_count(); i++) {
        const char *nm = ls_tui_screen_name(i);
        if (nm && !strcmp(nm, "JOURNAL")) { note.screen = i; break; }
    }
    ls_notify_post(&note);
}

/* ------------------------------------------ profiles off the card --- */

/* The TUI build has always been able to SHOW a loaded profile - ls_wf_source
   lists its control channels and steps between them - but nothing in this
   build ever loaded one. p25_program_request_reload is only called from the
   LVGL AppP25, which is not compiled here, so on this board a profile on the
   card was unreachable. This is that missing half. */

#define P25_PROFILE_DIR "/sdcard"

/* Sized to what the session will actually hold. A name too long for
   P25_PROGRAM_PATH_MAX is dropped while listing rather than offered and then
   refused by the claim, because a row that cannot be chosen is worse than an
   absent one. */
static EXT_RAM_BSS_ATTR char s_profiles[LS_PICKER_MAX][P25_PROGRAM_PATH_MAX];
static char s_profile_detail[LS_PICKER_MAX][LS_PICKER_DETAIL];
static int  s_profile_n;

static int profile_order(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

/* The system= line is the one thing worth knowing about a profile before
   loading it. Reading the head of the file is far cheaper than a full parse,
   which would also want a p25_profile_parse_scratch_t - far too large to put
   on the UI task's stack for a list of up to forty-eight files. */
static void profile_describe(const char *path, char *out, size_t cap)
{
    /* UI task only, so a static head buffer is safe and keeps 2 KiB off the
       stack. The example profiles carry long comment preambles, so 512 bytes
       was not enough to reach system= on a real file. */
    static char head[2048];

    snprintf(out, cap, "profile");
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(out, cap, "unreadable"); return; }
    size_t n = fread(head, 1, sizeof(head) - 1, f);
    fclose(f);
    head[n] = '\0';

    for (char *line = head; line && *line; ) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        while (*line == ' ' || *line == '\t') line++;
        if (!strncmp(line, "system=", 7)) {
            const char *v = line + 7;
            while (*v == ' ') v++;
            size_t len = strlen(v);
            while (len && (v[len - 1] == '\r' || v[len - 1] == ' ')) len--;
            if (len) snprintf(out, cap, "%.*s", (int)len, v);
            return;
        }
        line = end ? end + 1 : NULL;
    }
    /* Reaching here means no system= in the first 2 KiB. Say that rather than
       "profile", because it is also what an unrelated .txt looks like. */
    snprintf(out, cap, "no system= line");
}

static void pick_profile(int i)
{
    if (i < 0 || i >= s_profile_n) return;
    if (!p25_program_request_reload_path(s_profiles[i])) {
        snprintf(s_hint, sizeof(s_hint), "profile refused: %s",
                 s_profiles[i] + sizeof(P25_PROFILE_DIR));
        return;
    }
    /* The worker reports through the session, not through here: the load is
       asynchronous and this call only says the request was taken. */
    snprintf(s_hint, sizeof(s_hint), "loading %s",
             s_profiles[i] + sizeof(P25_PROFILE_DIR));
}

static ls_act_status_t a_p25_profile(const ls_args_t *in, ls_val_t *out)
{
    ls_wf_source_release();
    (void)in;
    s_profile_n = 0;

    /* A file that matched the name but could not be offered is counted, not
       dropped in silence. A card holding one profile with a long name would
       otherwise produce an empty list and the advice to go put a profile on
       the card, which is the one thing the operator has already done. */
    int skipped = 0;

    DIR *d = opendir(P25_PROFILE_DIR);
    const struct dirent *e;
    if (d) {
        while ((e = readdir(d)) != NULL) {
            const size_t n = strlen(e->d_name);
            if (strncasecmp(e->d_name, "p25_profile", 11)) continue;
            if (n < 5 || strcasecmp(e->d_name + n - 4, ".txt")) continue;
            if (s_profile_n >= LS_PICKER_MAX ||
                sizeof(P25_PROFILE_DIR) + n > sizeof(s_profiles[0])) {
                skipped++;
                continue;
            }
            snprintf(s_profiles[s_profile_n++], sizeof(s_profiles[0]),
                     "%s/%s", P25_PROFILE_DIR, e->d_name);
        }
        closedir(d);
    }
    qsort(s_profiles, (size_t)s_profile_n, sizeof(s_profiles[0]), profile_order);

    static char title[LS_PICKER_TEXT];
    if (skipped) LS_SNPRINTF(t_scr_p25_p25_profiles_d_skipped, title, sizeof(title), "P25 PROFILES  %d SKIPPED", skipped);
    else         snprintf(title, sizeof(title), "P25 PROFILES");

    const p25_program_t *ps = p25_program_session();
    ls_picker_open(title, pick_profile);
    for (int i = 0; i < s_profile_n; i++) {
        const bool loaded = ps && ps->active_valid &&
                            !strcmp(ps->active_path, s_profiles[i]);
        if (loaded)
            snprintf(s_profile_detail[i], sizeof(s_profile_detail[i]), "loaded");
        else
            profile_describe(s_profiles[i], s_profile_detail[i],
                             sizeof(s_profile_detail[i]));
        ls_picker_add(s_profiles[i] + sizeof(P25_PROFILE_DIR),
                      s_profile_detail[i]);
    }
    if (!s_profile_n)
        ls_picker_empty_reason(skipped
            ? "names too long for the card path - rename them shorter"
            : "put p25_profile*.txt in the SD root");

    out->kind = LS_VAL_TEXT;
    out->s = "choose a P25 profile";
    return LS_ACT_OK;
}

/* Registered on entry rather than with the builtins: the action belongs to
   this screen, which owns the list it picks from. */
static void register_p25_actions(void)
{
    static bool done;
    if (done) return;
    /* STORE as well as TUNE: applying a profile rewrites the persisted scan
       roster, so this is not a read-only browse. */
    done = ls_action_register("p25.profile", "", LS_CAP_TUNE | LS_CAP_STORE,
                              a_p25_profile,
                              LS_TEXT(t_scr_p25_choose_a_p25_profile, "choose a P25 profile from the card"));
}

/* The radio the receiver really has; before it has one, the choice. */
static ls_rsel_radio_t p25_in_use(void)
{
    if (P25.lora_rx) return LS_RSEL_LORA;
    return ls_rsel_sdr_held_by("p25");
}

/* A different dongle than the one held: the receiver starts again on it. A
   move to or from the LoRa chip the receiver makes by itself. */
static void radio_chosen(ls_rsel_radio_t radio)
{
    const ls_rsel_radio_t held = ls_rsel_sdr_held_by("p25");
    if ((radio == LS_RSEL_SDR_RTL || radio == LS_RSEL_SDR_HACKRF ||
         radio == LS_RSEL_P25_SPLIT) &&
        held != LS_RSEL_NONE && held != radio) ls_rsel_restart_sdr();
}

static void open_radio(void) { ls_wf_source_release(); ls_rsel_open(LS_RSEL_P25, radio_chosen); }

/* SETTINGS and OPTIONS share one tree and the same backend callbacks. */
static void o_p25_act(const ls_opt_t *o)
{
    ps_selected = o->arg;
    ps_change(1, true);
}
static void o_p25_show(const ls_opt_t *o, char *out, size_t n) { ps_value(o->arg, out, n); }
static double o_p25_num(const ls_opt_t *o)
{
    char value[48]; ps_value(o->arg, value, sizeof(value)); return atof(value);
}
static void o_p25_set_num(const ls_opt_t *o, double v)
{
    ps_selected = o->arg; ps_number(v);
}
static const char *o_p25_iq_only(const ls_opt_t *o)
{
    (void)o; return ps_lr_receiver() ? "IQ receiver required" : NULL;
}
static const ls_opt_t OPT_P25_POLICY[] = {
    { .label = "Hold current talkgroup", .kind = LS_OPT_ACTION, .arg = 18, .act = o_p25_act, .show = o_p25_show },
    { .label = "Lockout current talkgroup", .kind = LS_OPT_ACTION, .arg = 19, .act = o_p25_act, .show = o_p25_show },
    { .label = "Lockout list", .kind = LS_OPT_ACTION, .arg = 25, .act = o_p25_act, .show = o_p25_show, .leaves = true },
    { .label = "Remove lockout TG", .kind = LS_OPT_NUMBER, .arg = 20, .num = o_p25_num, .set_num = o_p25_set_num, .lo = 1, .hi = 65535 },
    { .label = "Allow list mode", .kind = LS_OPT_ACTION, .arg = 21, .act = o_p25_act, .show = o_p25_show },
    { .label = "Allowed list", .kind = LS_OPT_ACTION, .arg = 26, .act = o_p25_act, .show = o_p25_show, .leaves = true },
    { .label = "Add allowed TG", .kind = LS_OPT_NUMBER, .arg = 22, .num = o_p25_num, .set_num = o_p25_set_num, .lo = 1, .hi = 65535 },
    { .label = "Remove allowed TG", .kind = LS_OPT_NUMBER, .arg = 23, .num = o_p25_num, .set_num = o_p25_set_num, .lo = 1, .hi = 65535 },
};
static const ls_opt_ctx_t CTX_P25_POLICY = { .name = "Talkgroups", .job = LS_RSEL_P25, LS_OPT_ROWS(OPT_P25_POLICY) };

/* ---- GPS sites and state packs ------------------------------------------
   The backend (main/ls_p25_packs.c, p25_sites.c) follows the nearest site in
   the packs on the card and fetches signed packs per state. These rows are
   its face: the switch, where it is following now, and getting or removing
   a state's pack. A state list is longer than a picker holds, so getting
   one goes region first. Every value comes through the value table; the
   download runs on the packs worker, never here. */
static const char *const PACK_REGIONS[] = { "Northeast", "Southeast", "Midwest", "South Central",
                                            "Mountain", "Pacific" };
#define N_PACK_REGIONS ((int)(sizeof(PACK_REGIONS) / sizeof(PACK_REGIONS[0])))
/* USPS code -> region, two letters then the region's digit. */
static const char PACK_REGION_OF[] =
    "CT0DE0MA0MD0ME0NH0NJ0NY0PA0RI0VT0DC0"
    "AL1FL1GA1KY1MS1NC1SC1TN1VA1WV1"
    "IA2IL2IN2KS2MI2MN2MO2ND2NE2OH2SD2WI2"
    "AR3LA3OK3TX3"
    "AZ4CO4ID4MT4NM4NV4UT4WY4"
    "AK5CA5HI5OR5WA5";
static int pack_region(const char *st)
{
    for (const char *p = PACK_REGION_OF; p[0] && p[1] && p[2]; p += 3)
        if (p[0] == st[0] && p[1] == st[1]) return p[2] - '0';
    return -1;
}

/* One catalog line: "ST|Name|free|installed-or-absent|version". */
typedef struct { char st[3], name[28], tier[6], have[16], ver[16]; } pack_line_t;
static int pack_lines(pack_line_t *out, int cap)
{
    ls_val_t v;
    if (!ls_value_read("p25.pack.entries", &v, NULL) || v.kind != LS_VAL_TEXT || !v.s) return 0;
    int n = 0;
    for (const char *p = v.s; *p && n < cap;) {
        const char *end = strchr(p, '\n');
        if (!end) end = p + strlen(p);
        char row[96];
        int len = (int)(end - p) < (int)sizeof(row) - 1 ? (int)(end - p) : (int)sizeof(row) - 1;
        memcpy(row, p, (size_t)len); row[len] = 0;
        char *f[5] = {0}; int k = 0;
        for (char *t = row; k < 5; k++) { f[k] = t; t = strchr(t, '|'); if (!t) { k++; break; } *t++ = 0; }
        if (k == 5 && strlen(f[0]) == 2) {
            pack_line_t *l = &out[n++];
            snprintf(l->st, sizeof(l->st), "%s", f[0]); snprintf(l->name, sizeof(l->name), "%s", f[1]);
            snprintf(l->tier, sizeof(l->tier), "%s", f[2]); snprintf(l->have, sizeof(l->have), "%s", f[3]);
            snprintf(l->ver, sizeof(l->ver), "%s", f[4]);
        }
        p = *end ? end + 1 : end;
    }
    return n;
}

static EXT_RAM_BSS_ATTR pack_line_t s_pack[64];
static EXT_RAM_BSS_ATTR int s_pick_map[LS_PICKER_MAX];
static int s_pack_n, s_pick_n;
static bool s_pick_remove;

static void pack_call(const char *action, const char *st)
{
    ls_args_t a = { .n = st ? 1 : 0 };
    if (st) { a.v[0].kind = LS_VAL_TEXT; a.v[0].s = st; }
    ls_val_t out;
    (void)ls_action_call(action, &a, &out, ls_quick_grant_builtin());
}

static void pack_state_done(int i)
{
    if (i < 0 || i >= s_pick_n) return;
    const pack_line_t *l = &s_pack[s_pick_map[i]];
    pack_call(s_pick_remove ? "p25.pack.remove" : "p25.pack.get", l->st);
}

static void pack_detail(const pack_line_t *l, char *d, size_t n)
{
    if (strcmp(l->have, "absent")) snprintf(d, n, "%s / on card %s", l->tier, l->have);
    else snprintf(d, n, "%s / %s", l->tier, l->ver);
}

static void pack_region_done(int r)
{
    if (r < 0 || r >= N_PACK_REGIONS) return;
    ls_picker_open(PACK_REGIONS[r], pack_state_done);
    s_pick_n = 0;
    for (int i = 0; i < s_pack_n && s_pick_n < LS_PICKER_MAX; i++) {
        if (pack_region(s_pack[i].st) != r) continue;
        char label[40], d[LS_PICKER_DETAIL];
        snprintf(label, sizeof(label), "%s  %s", s_pack[i].st, s_pack[i].name);
        pack_detail(&s_pack[i], d, sizeof(d));
        s_pick_map[s_pick_n++] = i;
        ls_picker_add(label, d);
    }
    ls_picker_empty_reason(LS_TEXT(t_scr_p25_no_packs_for_this, "No packs for this region yet"));
}

static void o_pack_get(const ls_opt_t *o)
{
    (void)o;
    s_pack_n = pack_lines(s_pack, 64);
    s_pick_remove = false;
    if (!s_pack_n) { pack_call("p25.pack.refresh", NULL); return; }
    ls_picker_open("State pack", pack_region_done);
    for (int r = 0; r < N_PACK_REGIONS; r++) {
        int have = 0, total = 0;
        for (int i = 0; i < s_pack_n; i++)
            if (pack_region(s_pack[i].st) == r) { total++; have += strcmp(s_pack[i].have, "absent") != 0; }
        char d[LS_PICKER_DETAIL];
        LS_SNPRINTF(t_scr_p25_d_states_d_on, d, sizeof(d), "%d states, %d on card", total, have);
        ls_picker_add(PACK_REGIONS[r], d);
    }
}

static void o_pack_remove(const ls_opt_t *o)
{
    (void)o;
    s_pack_n = pack_lines(s_pack, 64);
    s_pick_remove = true;
    ls_picker_open("Remove pack", pack_state_done);
    s_pick_n = 0;
    for (int i = 0; i < s_pack_n && s_pick_n < LS_PICKER_MAX; i++) {
        if (!strcmp(s_pack[i].have, "absent")) continue;
        char label[40], d[LS_PICKER_DETAIL];
        snprintf(label, sizeof(label), "%s  %s", s_pack[i].st, s_pack[i].name);
        pack_detail(&s_pack[i], d, sizeof(d));
        s_pick_map[s_pick_n++] = i;
        ls_picker_add(label, d);
    }
    ls_picker_empty_reason(LS_TEXT(t_scr_p25_no_state_packs_on, "No state packs on the card"));
}

static void o_pack_refresh(const ls_opt_t *o) { (void)o; pack_call("p25.pack.refresh", NULL); }
static void o_pack_state_show(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    ls_val_t st, pr;
    const bool have = ls_value_read("p25.pack.state", &st, NULL) && st.kind == LS_VAL_TEXT && st.s;
    if (ls_value_read("p25.pack.progress", &pr, NULL) && pr.f > 0.0f && pr.f < 1.0f)
        snprintf(out, n, "%s %d%%", have ? st.s : "", (int)(pr.f * 100.0f + 0.5f));
    else snprintf(out, n, "%s", have ? st.s : "--");
}
static void o_pack_count_show(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    ls_val_t c;
    const int on = pack_lines(s_pack, 64);
    int have = 0;
    for (int i = 0; i < on; i++) have += strcmp(s_pack[i].have, "absent") != 0;
    if (ls_value_read("p25.pack.count", &c, NULL) && c.i > 0) snprintf(out, n, "%d of %ld on card", have, c.i);
    else snprintf(out, n, "%s", "refresh the list first");
}

/* Where GPS has it now: the site, its system and how far, or why not. */
static void o_geo_show(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    ls_val_t en, st, site, sys, km;
    if (!ls_value_read("p25.geo.enabled", &en, NULL) || !en.i) { snprintf(out, n, "%s", "off"); return; }
    const bool s_ok = ls_value_read("p25.geo.site", &site, NULL) && site.kind == LS_VAL_TEXT && site.s && site.s[0];
    if (s_ok && ls_value_read("p25.geo.system", &sys, NULL) && sys.kind == LS_VAL_TEXT && sys.s) {
        if (ls_value_read("p25.geo.distance", &km, NULL)) snprintf(out, n, "%s / %s %.0f km", sys.s, site.s, (double)km.f);
        else snprintf(out, n, "%s / %s", sys.s, site.s);
        return;
    }
    snprintf(out, n, "%s", ls_value_read("p25.geo.state", &st, NULL) && st.kind == LS_VAL_TEXT && st.s ? st.s : "--");
}
static void o_geo_resume(const ls_opt_t *o) { (void)o; pack_call("p25.geo.resume", NULL); }

static const ls_opt_t OPT_P25_SITES[] = {
    { .label = "GPS sites",          .kind = LS_OPT_ACTION, .arg = LS_P25_SETTING_GPS_SITES, .act = o_p25_act, .show = o_p25_show },
    { .label = "Following",          .kind = LS_OPT_ACTION, .act = o_geo_resume, .show = o_geo_show },
    { .label = "Get a state pack",   .kind = LS_OPT_ACTION, .act = o_pack_get, .show = o_pack_count_show, .leaves = true },
    { .label = "Remove a state pack", .kind = LS_OPT_ACTION, .act = o_pack_remove, .leaves = true },
    { .label = "Refresh list",       .kind = LS_OPT_ACTION, .act = o_pack_refresh, .show = o_pack_state_show },
};
static const ls_opt_ctx_t CTX_P25_SITES = { .name = "GPS sites", .job = LS_RSEL_P25, .refresh_ms = 250,
                                            LS_OPT_ROWS(OPT_P25_SITES) };
static const ls_opt_t OPT_P25[] = {
    LS_CALLS_MENU,
    { .label = "Talkgroup policy", .kind = LS_OPT_MENU, .sub = &CTX_P25_POLICY },
    { .label = "GPS sites", .kind = LS_OPT_MENU, .sub = &CTX_P25_SITES },
    { .label = "Control survey", .kind = LS_OPT_ACTION, .arg = 24, .act = o_p25_act, .show = o_p25_show },
    { .label = "Demodulation",          .kind = LS_OPT_ACTION, .arg = 0,  .act = o_p25_act, .show = o_p25_show, .radios = LS_OPT_SDR },
    { .label = "Polarity",              .kind = LS_OPT_ACTION, .arg = 1,  .act = o_p25_act, .show = o_p25_show },
    { .label = "Automatic gain",        .kind = LS_OPT_ACTION, .arg = 2,  .act = o_p25_act, .show = o_p25_show, .radios = LS_OPT_SDR },
    { .label = "Receiver gain (dB)",    .kind = LS_OPT_LEVEL, .arg = 3,  .num = o_p25_num, .set_num = o_p25_set_num, .lo = 0, .hi = 50, .step = 1, .radios = LS_OPT_SDR, .show = o_p25_show },
    LS_RFFE_MENUS,
    { .label = "Volume",                .kind = LS_OPT_LEVEL, .arg = 4,  .num = o_p25_num, .set_num = o_p25_set_num, .lo = 0, .hi = 100, .step = 5, .show = o_p25_show },
    { .label = "Voice gate",            .kind = LS_OPT_LEVEL, .arg = 5,  .num = o_p25_num, .set_num = o_p25_set_num, .lo = 0, .hi = 99, .step = 1, .show = o_p25_show },
    { .label = "Sync beep",             .kind = LS_OPT_ACTION, .arg = 6,  .act = o_p25_act, .show = o_p25_show },
    { .label = "Trunking auto-follow",  .kind = LS_OPT_ACTION, .arg = 7,  .act = o_p25_act, .show = o_p25_show },
    { .label = "Leave encrypted calls", .kind = LS_OPT_ACTION, .arg = 8,  .act = o_p25_act, .show = o_p25_show },
    { .label = "Encrypted skip (s)",    .kind = LS_OPT_LEVEL, .arg = 9,  .num = o_p25_num, .set_num = o_p25_set_num, .lo = 1, .hi = 3600, .step = 5, .show = o_p25_show },
    { .label = "Scan threshold (%)",    .kind = LS_OPT_LEVEL, .arg = 13, .num = o_p25_num, .set_num = o_p25_set_num, .lo = 1, .hi = 100, .step = 1, .show = o_p25_show },
    { .label = "Scan hang (ms)",        .kind = LS_OPT_LEVEL, .arg = 14, .num = o_p25_num, .set_num = o_p25_set_num, .lo = 0, .hi = 30000, .step = 500, .show = o_p25_show },
    { .label = "Phase II (exp)",        .kind = LS_OPT_ACTION, .arg = 15, .act = o_p25_act, .show = o_p25_show, .why_not = o_p25_iq_only },
    { .label = "Phase II follow (exp)", .kind = LS_OPT_ACTION, .arg = 17, .act = o_p25_act, .show = o_p25_show, .why_not = o_p25_iq_only },
    { .label = "Profile from card",     .kind = LS_OPT_ACTION, .arg = 16, .act = o_p25_act, .show = o_p25_show },
    { .label = "CQPSK timing gain", .kind = LS_OPT_ACTION, .arg = 10, .act = o_p25_act, .show = o_p25_show, .radios = LS_OPT_SDR },
    { .label = "CQPSK carrier gain", .kind = LS_OPT_ACTION, .arg = 11, .act = o_p25_act, .show = o_p25_show, .radios = LS_OPT_SDR },
    { .label = "Reset CQPSK tuning", .kind = LS_OPT_ACTION, .arg = 12, .act = o_p25_act, .show = o_p25_show, .radios = LS_OPT_SDR },
};
static const ls_opt_ctx_t CTX_P25 = { .name = "P25", .job = LS_RSEL_P25, .radio = LS_RSEL_NONE,
    .refresh_ms = 250, .covers_screen = true, LS_OPT_ROWS(OPT_P25) };
static void open_options(void) { ls_wf_source_release(); ls_opt_open(&CTX_P25); }

static void enter(void)
{
    register_p25_actions();
    p25_calls_live_register();
    ls_rsel_track(LS_RSEL_P25, p25_in_use);
}

/* VFO, WATERFALL and SCANNER are views of one receiver, carried as tabs the
   way FM carries its own: at the bottom of portrait, under the thumb, and on
   every view, so the way back from the waterfall is the tab that says VFO
   rather than a toggle that has to be pressed again. */
enum { PV_VFO, PV_WATERFALL, PV_SCANNER, N_VIEWS };
static const ls_btn_t VIEWS[N_VIEWS] = {
    { "VFO", NULL, '1', false, false },
    { "WATERFALL", NULL, '2', false, false },
    { "SCANNER", NULL, '3', false, false },
};
/* Everything else P25 is told, on FM's control row. */
enum { PC_CALLS, PC_DMR, PC_RADIO, PC_OPTIONS, N_CTLS };

static void set_page(int page)
{
    if (page != PV_WATERFALL) ls_wf_source_release();
    s_page = page;
}

static tui_rect s_bar;          /* the view tabs                         */
static tui_rect s_ctl_rect;     /* CALLS DMR RADIO OPTIONS, portrait     */
static tui_rect s_quick_rect;   /* the deck, or the keypad in its place  */

/* The controls a thumb reaches for while watching a decode.

   Portrait has sixty rows and the decode page fills eighteen. Volume and
   gain were console-only here, exactly as they were on FM before the panel
   went in; a scanner you cannot turn down without a cable is not a handheld
   scanner. */
static const ls_quick_t QUICK[] = {

    { .label = "TUNE", .kind = LS_QUICK_ACTION, .action = "p25.tune",
      .key = 't' },
    { .label = "VOLUME", .kind = LS_QUICK_STEP, .action = "audio.volume",
      .value = "sys.volume", .delta = 5, .lo = 0, .hi = 100,
      .key = '+', .key_down = '-' },
    { .label = "GAIN", .kind = LS_QUICK_STEP, .action = "p25.gain",
      .value = "p25.gain", .delta = 2.0f, .lo = 0, .hi = 50,
      .key = 'u', .key_down = 'j' },
};
#define N_QUICK ((int)(sizeof(QUICK) / sizeof(QUICK[0])))

/* LR IQ does not use the FSK gain setting. Offer only controls that apply. */
static const ls_quick_t QUICK_LR[] = {
    { .label = "TUNE", .kind = LS_QUICK_ACTION, .action = "p25.tune", .key = 't' },
    { .label = "VOLUME", .kind = LS_QUICK_STEP, .action = "audio.volume",
      .value = "sys.volume", .delta = 5, .lo = 0, .hi = 100,
      .key = '+', .key_down = '-' },
};

/* The receiver P25 is set to use, not whether it is streaming this moment:
   the LR session drops lora_rx while it starts or restarts, and keying the
   deck on that swapped VOLUME for VOLUME + GAIN and back. */
static bool lr_receiver(void) { return ps_lr_receiver(); }
static const ls_quick_t *quick(void) { return lr_receiver() ? QUICK_LR : QUICK; }
static int quick_count(void) { return lr_receiver() ? 2 : N_QUICK; }

/* ------------------------------------------------------------- the deck -- */

/* The same controls the scanner grew, because it is the same radio: a card
   with a travel a thumb can press and a bracketed number that is the only way
   to the keypad. Volume is available on both receivers; only the SDR has
   an editable gain dial. */
enum { P25_CARD_VOL, P25_CARD_GAIN, P25_N_CARD };
static ls_radio_card_t s_deck[P25_N_CARD];
static int  s_deck_n;
static int  s_deck_flash = -1;
static bool s_deck_flash_up;
static int  s_deck_flash_ttl;
#define P25_FLASH_FRAMES 6

/* RTL gain is in tenths of a dB. LR IQ has no editable gain control. */
static int gain_now(void)
{
    const char *s = ls_quick_state(&QUICK[2]);
    return !s || !strcmp(s, "AUTO") ? 0 : (int)(atof(s) * 10.0 + 0.5);
}

static void gain_apply(double value)
{
    if (lr_receiver()) return;
    ls_args_t a = {.n = 1};
    ls_val_t out;
    if (value < 0) value = 0;
    if (value > 50) value = 50;
    a.v[0].kind = LS_VAL_FLOAT;
    a.v[0].f = (float)value;
    ls_action_call("p25.gain", &a, &out, ls_quick_grant_builtin());
}

static void volume_apply(double value)
{
    if (!(value >= 0 && value <= 100)) return;
    ls_args_t a = {.n = 1};
    ls_val_t out;
    a.v[0].kind = LS_VAL_INT;
    a.v[0].i = (int)(value + 0.5);
    ls_action_call("audio.volume", &a, &out, ls_quick_grant_builtin());
}

static void deck_fill(void)
{
    static char vol_text[12], gain_text[12];
    const int vol = audio_volume_get() < 0 ? 0
                  : audio_volume_get() > 100 ? 100 : audio_volume_get();
    snprintf(vol_text, sizeof(vol_text), "%d%%", vol);
    s_deck[P25_CARD_VOL] = (ls_radio_card_t){
        .label = "VOLUME", .value = vol_text, .frac = vol / 100.0f,
        .hue = TUI_GREEN, .live = true, .settable = true };

    const char *state = ls_quick_state(&QUICK[2]);
    const int g = gain_now();
    snprintf(gain_text, sizeof(gain_text), "%s", g <= 0 ? "AUTO" : state ? state : "-");
    s_deck[P25_CARD_GAIN] = (ls_radio_card_t){
        .label = "GAIN", .value = gain_text, .frac = (float)g / 500.0f,
        .hue = TUI_MAGENTA, .live = true, .settable = true };

    const bool lit = s_deck_flash_ttl > 0;
    for (int i = 0; i < P25_N_CARD; i++) {
        s_deck[i].flash_down = lit && s_deck_flash == i && !s_deck_flash_up;
        s_deck[i].flash_up   = lit && s_deck_flash == i && s_deck_flash_up;
    }
}

static uint32_t tune_step_hz(void)
{
    uint32_t hz = settings_get_p25_step();
    return hz ? hz : 12500;
}
static uint32_t tune_frequency(void) { return s_tune_freq_hz; }
static void cycle_tune_step(void)
{
    settings_set_p25_step(ls_radio_tune_step_next(tune_step_hz()));
}
static uint32_t tune_low(void)
{
    return lr_receiver() && ls_lora_chip() == LS_LORA_CHIP_LR20XX
           ? 104000000u : 24000000u;
}
static bool tune_step(int direction)
{
    scan_engine_stop();
    int64_t hz = (int64_t)s_tune_freq_hz + direction * (int64_t)tune_step_hz();
    if (hz < tune_low()) hz = tune_low();
    if (hz > 1766000000) hz = 1766000000;
    ls_args_t a = {.n = 1}; ls_val_t out;
    a.v[0].kind = LS_VAL_FLOAT; a.v[0].f = (float)(hz / 1e6);
    return ls_action_call("p25.freq", &a, &out, ls_quick_grant_builtin()) == LS_ACT_OK;
}
static void seek_tune(int direction)
{
    if (scan_engine_seek_direction()) { scan_engine_stop(); return; }
    (void)scan_engine_seek(s_tune_freq_hz, tune_step_hz(), direction,
                           tune_low(), 1766000000u);
}
/* Not EXT_RAM_BSS_ATTR: that section is zeroed at boot and drops the callbacks. */
static ls_radio_tune_t s_tune = {
    .step = tune_step_hz, .frequency = tune_frequency, .cycle = cycle_tune_step,
    .tune = tune_step, .seek = seek_tune,
};

static void tune_draw(tui_surface *sf, tui_rect area)
{
    ls_radio_tune_draw(&s_tune, sf, area);
}

static void deck_draw(tui_surface *sf, tui_rect area)
{
    s_deck_n = 0;
    if (ls_tui_compact() && area.h >= 2 && area.w >= 28) {
        /* Desk: the tune row, then the cards as one-row sliders side by side. */
        tune_draw(sf, tui_rect_make(area.x, area.y, area.w, 1));
        if (s_deck_flash_ttl > 0 && --s_deck_flash_ttl == 0) s_deck_flash = -1;
        deck_fill();
        const int n_cards = lr_receiver() ? 1 : P25_N_CARD;
        const int cw = area.w / (n_cards > 1 ? n_cards : 2);
        for (int i = 0; i < n_cards; i++) {
            ls_radio_vfo_card_draw(sf, tui_rect_make(area.x + i * cw, area.y + 1, cw - 2, 1), &s_deck[i]);
            s_deck_n = i + 1;
        }
        return;
    }
    if (ls_tui_is_wide() && area.h >= 10) {
        tune_draw(sf, tui_rect_make(area.x, area.y, area.w, 4));
        area.y += 4; area.h -= 4;
    }
    if (area.h < 6 || area.w < 28) return;
    if (s_deck_flash_ttl > 0 && --s_deck_flash_ttl == 0) s_deck_flash = -1;
    deck_fill();
    /* FM's step cards: stacked and as tall as the pane allows, up to the nine
       rows that give the keys a thumb's height, immediately below the
       receiver content. Landscape puts the two side by side. */
    const int n_cards = lr_receiver() ? 1 : P25_N_CARD;
    const int cols = area.w >= 56 && n_cards > 1 ? 2 : 1;
    const int rows = (n_cards + cols - 1) / cols;
    const int gap = area.h >= rows * 8 + rows - 1 ? 1 : 0;
    int card_h = (area.h - gap * (rows - 1)) / rows;
    if (!ls_tui_is_wide() && card_h > 8) card_h = 8;
    if (card_h > 9) card_h = 9;
    if (card_h < 6) return;
    /* Anchor the deck to the health box, including a single LR card. */
    const int y0 = area.y;
    const int x0 = cols > 1 ? area.x : area.x + 1;
    const int w0 = cols > 1 ? area.w : area.w - 2;
    const int cw = w0 / cols;
    for (int i = 0; i < n_cards; i++) {
        const tui_rect box = tui_rect_make(x0 + (i % cols) * cw,
                                           y0 + (i / cols) * (card_h + gap),
                                           cols > 1 ? cw - 1 : w0, card_h);
        ls_radio_vfo_card_draw(sf, box, &s_deck[i]);
        s_deck_n = i + 1;
    }
}

/* ------------------------------------------------ the frequency keyboard -- */

/* TYPING A FREQUENCY HAPPENS WHERE THE CONTROLS ARE, NOT OVER THEM.

   The keypad is FM's own (ls_freq_pad_draw), taking the deck's place the way
   the VFO's does, with the KEYBOARD / CONTROLS toggle in the bottom right
   corner. Fixed storage: drawing or editing it allocates nothing. */
static tui_rect s_key[12], s_key_act[3];
static tui_rect s_kbd_btn;

static void entry_clear(void) { s_entry[0] = 0; s_entry_bad = false; }

static void keypad_set(bool on)
{
    s_keypad = on;
    entry_clear();
}

/* Typing a frequency is an operator retune: it ends a stored-channel scan,
   the same as every other way of moving the carrier on this screen. */
static void entry_tune(void)
{
    uint32_t hz;
    if (!ls_freq_entry_hz(s_entry, &hz)) { s_entry_bad = true; return; }
    scan_engine_stop();
    ls_args_t a = {.n = 1};
    ls_val_t out;
    a.v[0].kind = LS_VAL_FLOAT;
    a.v[0].f = (float)(hz / 1e6);
    if (ls_action_call("p25.freq", &a, &out, ls_quick_grant_builtin()) != LS_ACT_OK) {
        s_entry_bad = true;
        return;
    }
    keypad_set(false);
}

/* One character from the pad or the keyboard, in ls_freq_pad_hit's terms. */
static bool entry_char(char c)
{
    if ((c >= '0' && c <= '9') || c == '.') {
        ls_freq_entry_add(s_entry, sizeof(s_entry), c);
        s_entry_bad = false;
        return true;
    }
    if (c == 'd') { ls_freq_entry_delete(s_entry); s_entry_bad = false; return true; }
    if (c == 'c') { entry_clear(); return true; }
    if (c == 'x') { keypad_set(false); return true; }
    if (c == 't') { entry_tune(); return true; }
    return false;
}

static void kbd_toggle_draw(tui_surface *sf, tui_rect r)
{
    if (r.w < 4 || r.h < 1) { s_kbd_btn = tui_rect_make(0, -1, 0, 0); return; }
    s_kbd_btn = r;
    const char *label = s_keypad ? "CONTROLS" : r.w >= 10 ? "KEYBOARD" : "KEYPAD";
    ls_freq_key_draw(sf, r, label, 0, s_keypad ? TUI_GREEN : TUI_MAGENTA, s_keypad);
}

/* Where the deck's controls landed, so a test presses the real thing instead
   of guessing at a screenshot - the service ls_radio_panel_control_rect gives
   for the scanner's deck, with the same part numbers. */
bool ls_scr_p25_control_rect(int card, int part, int *x, int *y, int *w, int *h)
{
    if (card < 0 || card >= s_deck_n) return false;
    const ls_radio_card_t *c = &s_deck[card];
    const tui_rect r = part == LS_RADIO_PART_UP    ? c->up
                     : part == LS_RADIO_PART_VALUE ? c->value_rect
                     : part == LS_RADIO_PART_BAR   ? c->bar
                                                   : c->down;
    if (r.h <= 0 || r.w <= 0) return false;
    if (x) *x = r.x;
    if (y) *y = r.y;
    if (w) *w = r.w;
    if (h) *h = r.h;
    return true;
}

/* Where the keypad's key for `c` landed ('d' DEL, 'c' CLEAR, 'x' CANCEL,
   't' TUNE), for a test to press it as a thumb would. */
bool ls_scr_p25_pad_rect(char c, int *x, int *y)
{
    static const char KEY[15] = "123456789.0dcxt";
    for (int i = 0; i < 15; i++) {
        if (KEY[i] != c) continue;
        const tui_rect r = i < 12 ? s_key[i] : s_key_act[i - 12];
        if (r.h <= 0 || r.w <= 0) return false;
        if (x) *x = r.x + r.w / 2;
        if (y) *y = r.y + r.h / 2;
        return true;
    }
    return false;
}

/* Step keys retain priority; bar and title value share exact entry. */
static bool deck_touch(int col, int row)
{
    for (int i = 0; i < s_deck_n; i++) {
        const ls_radio_card_t *c = &s_deck[i];
        const int part = ls_radio_vfo_card_hit(c->down, c->up, c->value_rect,
                                               c->bar, col, row);
        if (part < 0) continue;
        const bool down = part == LS_RADIO_PART_DOWN;
        const bool up = part == LS_RADIO_PART_UP;
        if (down || up) {
            /* The keys are the quick controls' own, so a thumb and the
               keyboard move one value by one step. */
            const ls_quick_t *q = &quick()[i == P25_CARD_VOL ? 1 : 2];
            ls_quick_key(up ? q->key : q->key_down, quick(), quick_count(),
                         ls_quick_grant_builtin(), NULL);
            s_deck_flash = i;
            s_deck_flash_up = up;
            s_deck_flash_ttl = P25_FLASH_FRAMES;
            return true;
        }
        if (part == LS_RADIO_PART_BAR) {
            const float f = ls_radio_vfo_card_bar_frac(c->bar, col);
            if (i == P25_CARD_VOL) volume_apply(f * 100.0);
            else gain_apply(f * 50.0);
            return true;
        }
        if (i == P25_CARD_VOL)
            ls_numpad_open("VOLUME", "0 to 100", audio_volume_get(), volume_apply);
        else
            ls_numpad_open("GAIN", "dB, 0 is automatic", gain_now() / 10.0, gain_apply);
        return true;
    }
    return false;
}

static void panel_set_volume_pct(int pct) { volume_apply((double)pct); }

static void radio_view(void)
{
    memset(&s_view,0,sizeof(s_view));
    /* The scanner deck's VOLUME travel sets through this; without it the
       bar drew as a slider and a tap did nothing. */
    s_view.set_volume_pct=panel_set_volume_pct;
    s_view.frequency=s_tune_freq_hz;
    s_view.mode="P25";
    s_view.power=P25.iq_level;
    s_view.volume=audio_volume_get();
    p25_get_receiver_status(&s_view.receiver);
    bool sync=s_view.receiver.receiver_streaming && P25.dsd_has_sync;
    if(sync && p25_nac_valid(&P25)) LS_SNPRINTF(t_scr_p25_nac_03x_tg_d, s_view.detail[0],64,"NAC %03X  TG %d  UNIT %d",P25.dsd_nac,P25.dsd_tg,P25.dsd_src);
    else LS_SNPUTS(t_scr_p25_nac_tg_unit, s_view.detail[0], 64, "NAC ---   TG ---   UNIT ---");
    snprintf(s_view.detail[1],64,"%s",receive_label());
    if(lr_receiver()) {
        snprintf(s_view.detail[2],64,"LR IQ receive");
    } else snprintf(s_view.detail[2],64,"GAIN %.1f dB",P25.rtl_gain_tenths/10.0);
    snprintf(s_view.detail[3],64,"AUDIO %s",s_view.receiver.receiver_streaming?voice_status():"OFFLINE");
    if(p25_p2_enabled()) {
        s_view.mode="P25 II EXP";
        p25_p2_describe(s_view.detail[0],64);
        LS_SNPUTS(t_scr_p25_manual_voice_6000_baud, s_view.detail[1], 64, "MANUAL VOICE / 6000 baud");
        LS_SNPUTS(t_scr_p25_experimental_rf_path_unverified, s_view.detail[3], 64, "Experimental; RF path unverified");
    }
}
static void radio_action(char c)
{
    if(c=='P') {open_options();return;}
    if(c=='W') {set_page(1);return;}
    if(c=='M'||c=='D') {set_page(0);return;}
    if(c=='[' || c==']') {
        (void)tune_step(c == '[' ? -1 : 1);
        return;
    }
    if(c=='T') scan_engine_stop();
    if(c) ls_quick_key(c,quick(),quick_count(),ls_quick_grant_builtin(),NULL);
}

static void draw(tui_surface *sf, tui_rect area)
{
    /* Every page follows the call, so CALLS opens on the truth. */
    p25_calls_live_tick();
    if (ls_calls_active()) {
        LS_SNPUTS(t_scr_p25_up_down_calls_p, s_hint, sizeof(s_hint), "UP/DOWN calls  P play  H hold  L lockout  D delete  ESC back");
        ls_wf_source_release(); ls_calls_draw(sf, area); return;
    }
    if (s_page == 5) {
        LS_SNPUTS(t_scr_p25_s_slot_h_hold, s_hint, sizeof(s_hint), "S slot  H hold  O options  1 P25  R radio");
        draw_dmr(sf, area);
        return;
    }
    snprintf(s_hint, sizeof(s_hint), "%s",
             s_keypad && s_page == PV_VFO ? "0-9 . type  ENTER tune  BACKSPACE delete  ESC controls"
             : s_page == PV_WATERFALL ? "LEFT/RIGHT select  SPACE tune  1 VFO  3 scanner  R radio"
             : scan_engine_active() ? "N next K skip H hold/resume X stop / STEP T keyboard O options"
             : "LEFT/RIGHT step ,/. seek / STEP T keyboard 1/2/3 views R radio O options");
    s_blink++;
    /* The desk lays out as landscape does, at one row a bar. */
    const bool compact = ls_tui_compact();
    const bool wide = ls_tui_is_wide() || compact;

    ls_btn_t ctl[N_CTLS] = {
        { "CALLS", NULL, '5', false, false },
        { "DMR",   NULL, '6', false, false },
        ls_rsel_button(LS_RSEL_P25),
        ls_opt_button(&CTX_P25),
    };
    ls_btn_t tabs[N_VIEWS + N_CTLS];
    for (int i = 0; i < N_VIEWS; i++) {
        tabs[i] = VIEWS[i];
        tabs[i].on = s_page == i;
    }
    if (!(s_page == PV_VFO)) s_keypad = false;

    s_ctl_rect = s_quick_rect = s_kbd_btn = tui_rect_make(0, -1, 0, 0);
    memset(s_tune.keys, 0, sizeof(s_tune.keys));
    tui_rect body;
    int ntabs = N_VIEWS;
    if (wide) {
        /* LANDSCAPE: one bar along the top, views first, as FM's is. The
           deck is the bottom of the screen, and the keyboard toggle comes
           out of its right end. */
        for (int i = 0; i < N_CTLS; i++) tabs[N_VIEWS + i] = ctl[i];
        ntabs = N_VIEWS + N_CTLS;
        const tui_rect probe = tui_rect_make(area.x, area.y, area.w, 3);
        const bool fits = ls_btn_compact_fits(probe, tabs, ntabs);
        const int bar_h = compact ? 1
                        : (ls_tui_keyboard_mode() && fits) || area.h < 24 || s_page == PV_VFO ? 3 : 4;
        s_bar = tui_rect_make(area.x, area.y, area.w, bar_h);
        body = tui_rect_make(area.x, area.y + bar_h, area.w, area.h - bar_h);
        if (compact && s_page == PV_VFO && area.w >= 56 && body.h >= 12) {
            /* Desk deck: a tune row and a slider row, KEYBOARD at its end. */
            s_quick_rect = tui_rect_make(body.x, body.y + body.h - 2, body.w, 2);
            body.h -= 2;
            s_kbd_btn = tui_rect_make(s_quick_rect.x + s_quick_rect.w - 14, s_quick_rect.y, 14, 1);
        } else if (s_page == PV_VFO && area.w >= 56 && !compact) {
            int deck_h = s_keypad ? body.h - 11 : body.h >= 25 ? 13 : body.h >= 24 ? 12 : 10;
            if (body.h - deck_h < 10) deck_h = 0;
            if (deck_h >= 5) {
                s_quick_rect = tui_rect_make(body.x, body.y + body.h - deck_h,
                                             body.w, deck_h);
                body.h -= deck_h;
                int kw = s_quick_rect.w / 8;
                if (kw < 10) kw = 10;
                if (kw > 14) kw = 14;
                s_kbd_btn = tui_rect_make(s_quick_rect.x + s_quick_rect.w - kw,
                                          s_quick_rect.y, kw, s_quick_rect.h);
                s_quick_rect.w -= kw + 1;
            }
        }
    } else {
        /* PORTRAIT, laid out as FM's VFO: the control row on top, the
           receiver's glass, the deck of tall cards under it, and the view
           tabs at the very bottom with KEYBOARD in the corner. The scanner
           gets the same row: its own bar is down to LISTS / SETUP / BACK,
           so CALLS, DMR, the radio and P25 OPTIONS are reached from here. */
        const int tabs_h = 5;
        const int ctl_h = area.h >= 55 ? 5 : 4;
        if (ctl_h) s_ctl_rect = tui_rect_make(area.x, area.y, area.w, ctl_h);
        s_bar = tui_rect_make(area.x, area.y + area.h - tabs_h, area.w, tabs_h);
        body = tui_rect_make(area.x, area.y + ctl_h, area.w,
                             area.h - ctl_h - tabs_h);
        if (s_page == PV_VFO) {
            /* Match FM's eight-row portrait cards. Spend the activity
               slack first, then the separating blank row on shorter panes. */
            int deck_h = s_keypad ? body.h - (body.h >= 40 ? 19 : 12)
                                 : lr_receiver() ? 8 : body.h >= 54 ? 17 : 16;
            if (!s_keypad && body.h - deck_h < 21) deck_h = body.h - 19;
            if (deck_h >= 8) {
                s_quick_rect = tui_rect_make(body.x, body.y + body.h - deck_h,
                                             body.w, deck_h);
                body.h -= deck_h;
            }
            int kw = s_bar.w / 5;
            if (kw < 10) kw = 10;
            if (kw > 14) kw = 14;
            if (s_quick_rect.h > 0 && s_bar.w - kw >= 24) {
                s_bar.w -= kw;
                s_kbd_btn = tui_rect_make(s_bar.x + s_bar.w, s_bar.y, kw, tabs_h);
            }
        }
    }
    if (s_quick_rect.h <= 0) s_keypad = false;

    if (body.h > 0) {
        if (s_page == PV_WATERFALL) draw_signal(sf, body);
        else if (s_page == PV_SCANNER) {
            radio_view();
            ls_radio_panel_draw(&s_radio, &s_view, sf, body);
        } else draw_decode(sf, body);
    }

    /* The deck and the keyboard belong to the VFO view. The waterfall needs
       its rows for history and the scanner has a deck of its own. */
    s_deck_n = 0;
    memset(s_key, 0, sizeof(s_key));
    memset(s_key_act, 0, sizeof(s_key_act));
    if (s_quick_rect.h > 0) {
        if (s_keypad)
            ls_freq_pad_draw(sf, s_quick_rect, s_entry, s_entry_bad, s_blink,
                             s_key, s_key_act);
        else {
            deck_draw(sf, s_quick_rect);
            if (!s_deck_n) ls_quick_draw_posture(sf, s_quick_rect, wide, quick(), quick_count());
        }
    }

    /* Bars after the panes, so each keeps its own hit slot: the scanner
       panel clears every slot when it is too small to draw. */
    if (s_ctl_rect.h > 0)
        ls_btn_bar_raised_slot(sf, s_ctl_rect, ctl, N_CTLS, -1, LS_BTN_SLOT_SCREEN);
    if (wide) ls_btn_bar_raised_slot(sf, s_bar, tabs, ntabs, -1, LS_BTN_SLOT_QUICK);
    else ls_btn_bar_slot(sf, s_bar, tabs, ntabs, -1, LS_BTN_SLOT_QUICK);
    if (s_kbd_btn.h > 0) kbd_toggle_draw(sf, s_kbd_btn);
}

/* The feed follows the page, and leaving the app releases it whichever page
   was up - the spectrum must not keep costing FFTs behind another screen. */
static void leave(void) { ls_calls_leave(); ls_wf_source_release(); }

static bool key(ls_tk_t k, char ch)
{
    if (ls_calls_active()) return ls_calls_key(k, ch);
    /* THE FREQUENCY KEYBOARD OWNS THE KEYS WHILE IT IS UP.

       A digit is a digit when a frequency is half typed - the pages, CALLS
       and DMR all answer to digits and would otherwise eat them - with ENTER
       to tune and ESC to put the controls back. This is the rule the VFO's
       inline keypad follows, and it has to be checked before anything else
       claims a number. */
    if (s_keypad && s_page == PV_VFO) {
        if (k == LS_TK_ESC) { keypad_set(false); return true; }
        if (k == LS_TK_ENTER) { entry_tune(); return true; }
        if (k == LS_TK_BACKSPACE) return entry_char('d');
        if (k == LS_TK_CHAR && ((ch >= '0' && ch <= '9') || ch == '.'))
            return entry_char(ch);
    }
    if (k == LS_TK_CHAR && ch == '5') { ls_calls_open("P25"); ls_wf_source_release(); return true; }
    if(k>=LS_TK_F1) return false;
    if (k == LS_TK_CHAR && ch == '6') { set_page(5); return true; }
    if (s_page == 5) {
        if (k == LS_TK_ESC || (k == LS_TK_CHAR && ch == '1')) {
            s_page = 0; ls_wf_source_release(); return true;
        }
        if (k == LS_TK_CHAR && ls_opt_key(&CTX_DMR, ch)) return true;
        if (k == LS_TK_CHAR && (ch == 's' || ch == 'S')) {
            s_dmr_filter.slot = (s_dmr_filter.slot + 1) % 3; return true;
        }
        if (k == LS_TK_CHAR && (ch == 'h' || ch == 'H')) { dmr_hold(); return true; }
        if (k == LS_TK_CHAR && (ch == 'r' || ch == 'R')) { open_radio(); return true; }
        if (k == LS_TK_CHAR && (ch == '+' || ch == '-'))
            return ls_quick_key(ch, quick(), quick_count(), ls_quick_grant_builtin(), NULL);
        return false;
    }
    /* R is RADIO on every page, except inside the scanner's lists, where
       R is RANGE. */
    if(k==LS_TK_CHAR && (ch=='r'||ch=='R') && !(s_page==2 && (s_radio.lists || s_radio.scan_choice))) {open_radio();return true;}
    /* O is OPTIONS on the same terms: inside the lists it is SCAN TYPE. */
    if(k==LS_TK_CHAR && !(s_page==2 && (s_radio.lists || s_radio.scan_choice)) && (ch=='o'||ch=='O')) {open_options();return true;}
    if(k==LS_TK_CHAR && (ch=='4'||((ch=='p'||ch=='P')&&s_page!=1))) {open_options();return true;}
    if(k==LS_TK_CHAR && ch>='1'&&ch<='3') { set_page(ch - '1'); return true; }
    /* 'l' for load. The profile chooser gets no quick-bar slot on purpose:
       every control there costs rows the waterfall and the activity table
       need, and choosing a profile is not what a thumb does while watching a
       decode. It lives on SETTINGS; this is the keyboard way in. p and P
       already open SETTINGS, and the waterfall owns a c d f g h i k p s y on
       SIGNAL, which is searched after this. */
    /* 'n' for note. LABS and SUB-GHZ both keep an observation on 'j', but 'j'
       is gain-down on this screen and moving it would break a binding that is
       in the hint line and in muscle memory. */
    if (s_page == PV_VFO && ls_radio_tune_key(&s_tune, k, ch)) return true;
    if(k==LS_TK_CHAR && ch=='n') { mark_observation(); return true; }
    /* T is still TUNE, and on a page that has room for it that now means the
       keyboard in place of the controls rather than a window over the decode.
       The scanner page keeps the house keypad, which is what it always had. */
    if (k == LS_TK_CHAR && (ch == 't' || ch == 'T') && s_page == PV_VFO &&
        s_quick_rect.h > 0) {
        keypad_set(!s_keypad);
        return true;
    }
    if(k==LS_TK_CHAR && ch=='l') {
        ls_args_t a={.n=0}; ls_val_t out;
        ls_action_call("p25.profile",&a,&out,ls_quick_grant_builtin());
        return true;
    }
    if (s_page==1 && (k==LS_TK_LEFT || k==LS_TK_RIGHT || (k==LS_TK_CHAR && ch==' ')))
        return ls_wf_key(k,ch);
    if (s_page!=2 && (k==LS_TK_LEFT || k==LS_TK_RIGHT)) {
        radio_action(k==LS_TK_LEFT ? '[' : ']'); return true;
    }
    if (s_page==2) { radio_view(); radio_action(ls_radio_panel_key(&s_radio,&s_view,k,ch)); return true; }
    if (k==LS_TK_ESC || (k==LS_TK_CHAR && ch=='0')) { s_page=0; ls_wf_source_release(); return true; }
    if (k == LS_TK_CHAR) {
        /* The same controls the panel draws, so a keyboard and a thumb reach
           them by one path. After the page keys, so a digit still pages. */
        if (ls_quick_key(ch, quick(), quick_count(),
                         ls_quick_grant_builtin(), NULL)) return true;
    }
    /* SIGNAL hands its keys to the waterfall: it is the waterfall, and a
       second set of bindings for the same instrument is how two screens end
       up disagreeing about what R means. */
    if (s_page == 1 && ls_wf_key(k, ch)) return true;

    if (k == LS_TK_LEFT)  { if (s_page > 0) s_page--; return true; }
    if (k == LS_TK_RIGHT) { if (s_page < 2) s_page++; return true; }
    return false;
}

static bool touch(int col, int row)
{
    if (ls_calls_active()) return ls_calls_touch(col, row);
    if (s_page == 5) {
        int i = ls_btn_hit_slot(col, row, LS_BTN_SLOT_QUICK);
        static const char keys[] = {'1', 's', 'h', 'r', 'o'};
        if (i >= 0 && i < 5) return key(LS_TK_CHAR, keys[i]);
        return false;
    }
    /* First: it shares a row band with the tabs in portrait and with the
       deck in landscape, and both of those claim their whole band. */
    if (s_kbd_btn.h > 0 && tui_rect_contains(s_kbd_btn, col, row)) {
        keypad_set(!s_keypad);
        return true;
    }
    if (s_ctl_rect.h > 0 && tui_rect_contains(s_ctl_rect, col, row)) {
        const int i = ls_btn_hit_slot(col, row, LS_BTN_SLOT_SCREEN);
        if (i == PC_CALLS) { ls_calls_open("P25"); ls_wf_source_release(); }
        else if (i == PC_DMR) set_page(5);
        else if (i == PC_RADIO) open_radio();
        else if (i == PC_OPTIONS) open_options();
        return true;
    }
    if (row >= s_bar.y && row < s_bar.y + s_bar.h) {
        const int i = ls_btn_hit_slot(col, row, LS_BTN_SLOT_QUICK);
        if (i >= 0 && i < N_VIEWS) { set_page(i); return true; }
        /* Landscape carries the controls on the same bar, after the views. */
        if (i == N_VIEWS + PC_CALLS) { ls_calls_open("P25"); ls_wf_source_release(); }
        else if (i == N_VIEWS + PC_DMR) set_page(5);
        else if (i == N_VIEWS + PC_RADIO) open_radio();
        else if (i == N_VIEWS + PC_OPTIONS) open_options();
        return true;
    }
    if (s_page == PV_VFO && !s_keypad && ls_radio_tune_touch(&s_tune, col, row)) return true;
    if (s_quick_rect.h > 0 && tui_rect_contains(s_quick_rect, col, row)) {
        if (s_keypad) {
            entry_char(ls_freq_pad_hit(s_key, s_key_act, col, row));
            return true;
        }
        if (s_deck_n) { deck_touch(col, row); return true; }
        if (ls_quick_touch(col, row, quick(), quick_count(),
                           ls_quick_grant_builtin(), NULL)) return true;
        return true;
    }
    if (s_page == PV_WATERFALL) return ls_wf_touch(col, row);
    if (s_page == PV_SCANNER) { radio_view(); radio_action(ls_radio_panel_touch(&s_radio,&s_view,col,row)); return true; }
    return false;
}

const ls_tui_screen_t ls_scr_p25 = {
    .diff_switch = true,
    /* P25 is this screen's whole subject. */
    .radio = "P25",
    .name = "P25",
    /* H belongs to the waterfall, which is only on SIGNAL.

       The key reaches ls_wf_key through this screen's handler and that call
       is gated on s_page == 1, while the hint row is drawn on both pages. So
       DECODE advertised a key that does nothing there. Said precisely rather
       than dropped: H is real and worth knowing about, it just lives on the
       other page. */
    .hint = s_hint,
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .key = key,
    .touch = touch,
};
