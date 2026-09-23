/* P25 screen. */

#include "../../ls_tui_screen.h"
#include "../../ls_radio_panel.h"
#include "audio/audio_out.h"
#include "scan_engine.h"

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
#include "../../ls_field.h"
#include "../../ls_notify.h"
#include "p25_program.h"

#include <dirent.h>
#include <strings.h>

static char s_hint[80] = "LEFT/RIGHT tune  1/2/3 views  +/- volume";
static ls_radio_panel_t s_radio = { .focus = -1 };
static ls_radio_view_t s_view;
#include "../../ls_p25_settings.h"

static int s_page;   /* 0 decode, 1 signal, 2 scanner */

/* The same free-running blink REC and FM now use (scr_rec.c,
   scr_fm.c) and MESH originated (scr_mesh.c s_blink): incremented once a
   draw, read back as `(s_blink / 12) & 1`. TG read a number the instant a
   grant landed and held that number, unchanged in appearance, for the rest
   of the call - a live conversation and its own five-second-old tail end
   looked identical. */
static uint32_t s_blink;

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

static void draw_activity(tui_surface *sf, tui_rect r, bool sync, int64_t now_us)
{
    const uint8_t frame = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t head  = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t live  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t old   = TUI_ATTR(TUI_WHITE, TUI_BLACK);

    tui_box(sf, r, "ACTIVITY", frame);
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
                    sync ? "synced, nothing heard yet"
                         : "no sync - nothing to hear",
                    LS_ATTR_DIM);
        return;
    }

    if (r.h < 4) return;
    tui_put_str(sf, r, r.x + 2, r.y + 1,
                "TALKGROUP    HEARD   AGE  SEEN IN", head);

    const uint32_t now_ms = (uint32_t)(now_us / 1000);
    const int rows = r.h - 3;

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

        char age[8];
        ls_age_str(age, sizeof(age), row->last_seen_ms, now_ms);

        /* Where it was seen answers a different question from how often: a
           link control word is the talkgroup of a call in progress, a header
           is one starting, and a grant is one being set up on another
           channel - so a row seen only in grants is traffic this radio is
           not listening to. */
        char how[4];
        int hi = 0;
        if (row->sources & P25_TG_SEEN_LCW)   how[hi++] = 'L';
        if (row->sources & P25_TG_SEEN_HDU)   how[hi++] = 'H';
        if (row->sources & P25_TG_SEEN_GRANT) how[hi++] = 'G';
        how[hi] = 0;

        char line[64];
        snprintf(line, sizeof(line), "%-9u  %7lu  %4s  %-3s",
                 (unsigned)row->talkgroup, (unsigned long)row->hits,
                 age, how);

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
        if (chan && delta > 6250ULL && r.w >= 46) {
            char freq[16];
            snprintf(freq, sizeof(freq), "  %u.%04u",
                     (unsigned)(chan / 1000000ULL),
                     (unsigned)((chan % 1000000ULL) / 100ULL));
            strncat(line, freq, sizeof(line) - strlen(line) - 1);
        }

        /* Thirty seconds is the span a P25 call and its tail occupy, so a
           row brighter than the rest is one that is or has just been live. */
        const bool recent = now_ms >= row->last_seen_ms &&
                            (now_ms - row->last_seen_ms) < 30000;
        tui_put_str(sf, r, r.x + 2, r.y + 2 + slot, line, recent ? live : old);
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
    tui_box(sf, r, "RECEIVER HEALTH / TOTALS", TUI_ATTR(TUI_CYAN, TUI_BLACK));
    const uint8_t ink = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    const int step = r.h >= 11 ? 2 : 1;
    char line[80];
    snprintf(line, sizeof(line), "IQ %lu B/s  AUDIO %lu samples/s",
             (unsigned long)P25.iq_bytes_sec, (unsigned long)P25.audio_samples_sec);
    tui_put_str(sf, r, r.x + 2, r.y + 1, line, ink);
    snprintf(line, sizeof(line), "SYNC %d   VOICE %d", P25.dsd_sync_count, P25.dsd_voice_count);
    tui_put_str(sf, r, r.x + 2, r.y + 1 + step, line, ink);
    snprintf(line, sizeof(line), "BCH OK %d   FAIL %d", P25.dsd_bch_ok_count, P25.dsd_bch_fail_count);
    tui_put_str(sf, r, r.x + 2, r.y + 1 + step * 2, line, ink);
    snprintf(line, sizeof(line), "USB errors %lu  AUDIO drops %lu",
             (unsigned long)P25.read_errors_total, (unsigned long)P25.audio_drops);
    tui_put_str(sf, r, r.x + 2, r.y + 1 + step * 3, line, ink);
    if (r.h >= 7) {
        snprintf(line, sizeof(line), "DECODE %.1f ms / block", (double)P25.dsd_decode_ms);
        tui_put_str(sf, r, r.x + 2, r.y + 1 + step * 4, line, LS_ATTR_DIM);
    }
}

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
    const uint8_t good  = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t idle  = TUI_ATTR(TUI_WHITE, TUI_BLACK);

    if (area.w < 20 || area.h < 3) return;

    tg_refresh(esp_timer_get_time());
    const int held = (s_tg_have && s_tg.count) ? (int)s_tg.count : 0;
    const int fits = held ? 3 + held : 3;
    const bool wide = area.w >= 60;
    const int dial_want = wide && area.h >= 18 ? 10 : 8;
    const int top_h = area.h < dial_want ? area.h : dial_want;
    tui_rect dial = tui_rect_make(area.x, area.y,
                                  wide ? area.w * 9 / 20 : area.w, top_h);
    tui_rect detail = wide
        ? tui_rect_make(dial.x + dial.w + 1, area.y,
                        area.w - dial.w - 1, top_h)
        : tui_rect_make(area.x, area.y + top_h, area.w,
                        area.h > top_h ? area.h - top_h : 0);
    tui_rect activity = tui_rect_make(0, 0, 0, 0);

    ls_iq_control_status_t st;
    p25_get_receiver_status(&st);
    const uint32_t shown = st.effective_center_known
                         ? (uint32_t)st.effective_center_hz : freq;
    tui_box(sf, dial, p25_p2_enabled() ? "P25 II EXP / RECEIVER"
                                       : "P25 / RECEIVER", frame);
    char buf[64];
    if (dial.h >= 8) {
        ls_radio_frequency_draw(sf, dial, 2, shown, value);
        if (dial.h >= 9)
            tui_put_str(sf, dial, dial.x + 2, dial.y + 7, "MHz", label);
    } else {
        snprintf(buf, sizeof(buf), "%u.%04u MHz", (unsigned)(shown / 1000000u),
                 (unsigned)((shown % 1000000u) / 100u));
        field(sf, dial, 1, "FREQ", buf, label, value);
    }
    if (dial.h >= 10) {
        if (scan_engine_active()) scan_engine_status(buf, sizeof(buf));
        else snprintf(buf, sizeof(buf), "%s", st.receiver_streaming
                      ? "MANUAL / RECEIVER ONLINE" : "NO USB RECEIVER");
        tui_put_str(sf, dial, dial.x + 2, dial.y + 8, buf,
                    scan_engine_active() ? value : idle);
    }

    if (wide) {
        tui_box(sf, detail, "DECODE / SIGNAL", frame);
        char line[80];
        if (nac > 0 && tg > 0) snprintf(line, sizeof(line), "NAC %03X   TG %d", nac, tg);
        else if (nac > 0) snprintf(line, sizeof(line), "NAC %03X   TG ---", nac);
        else if (tg > 0) snprintf(line, sizeof(line), "NAC ---   TG %d", tg);
        else snprintf(line, sizeof(line), "NAC ---   TG ---");
        tui_put_str(sf, detail, detail.x + 2, detail.y + 1, line, value);
        if (src > 0) snprintf(line, sizeof(line), "UNIT %d   MOD %s", src, mod[0] ? mod : "----");
        else snprintf(line, sizeof(line), "UNIT ---   MOD %s", mod[0] ? mod : "----");
        tui_put_str(sf, detail, detail.x + 2, detail.y + 2, line, value);
        snprintf(line, sizeof(line), "SYNC %s", sync ? "LOCKED" : "SEARCHING");
        tui_put_str(sf, detail, detail.x + 2, detail.y + 3, line, sync ? good : idle);
        draw_iq_gauge(sf, tui_rect_make(detail.x + 2, detail.y + 5,
                                        detail.w - 4, 1), level);
        snprintf(line, sizeof(line), "RX %s   AUDIO %s",
                 st.receiver_streaming ? "STREAMING" : "STOPPED", voice_status());
        tui_put_str(sf, detail, detail.x + 2, detail.y + 7, line,
                    st.receiver_streaming ? good : idle);
        int ah = area.h - top_h;
        if (ah >= 3) activity = tui_rect_make(area.x, area.y + top_h, area.w, ah);
        if (ah >= 7 && area.w >= 92) {
            activity.w = area.w / 2;
            draw_receiver_health(sf, tui_rect_make(activity.x + activity.w + 1,
                activity.y, area.w - activity.w - 1, ah));
        }
    } else if (detail.h > 0) {
        tui_rect decode = detail;
        decode.h = detail.h < 8 ? detail.h : 8;
        tui_box(sf, decode, "DECODE", frame);
        if (nac > 0) snprintf(buf, sizeof(buf), "%03X", nac); else snprintf(buf, sizeof(buf), "---");
        field(sf, decode, 1, "NAC", buf, label, nac > 0 ? value : idle);
        if (tg > 0) snprintf(buf, sizeof(buf), "%d", tg); else snprintf(buf, sizeof(buf), "--");
        field(sf, decode, 2, "TG", buf, label, tg > 0 ? value : idle);
        if (voice && ((s_blink / 12) & 1))
            tui_put_char(sf, decode, decode.x + 10 + (int)strlen(buf) + 1,
                         decode.y + 2, LS_TUI_SHADE_FULL, good);
        if (src > 0) snprintf(buf, sizeof(buf), "%d", src); else snprintf(buf, sizeof(buf), "--");
        field(sf, decode, 3, "SRC", buf, label, src > 0 ? value : idle);
        field(sf, decode, 4, "MOD", mod[0] ? mod : "----", label, value);
        field(sf, decode, 5, "SYNC", sync ? "LOCKED" : "searching", label,
              sync ? good : idle);

        int remain = detail.h - decode.h;
        tui_rect signal = tui_rect_make(area.x, decode.y + decode.h, area.w,
                                        remain < 9 ? remain : 9);
        if (signal.h > 0) {
            tui_box(sf, signal, "SIGNAL", frame);
            int bw = signal.w - 4;
            int lit = (int)(level * (float)bw);
            if (lit < 0) lit = 0;
            if (lit > bw) lit = bw;
            for (int i = 0; i < bw; i++)
                tui_put_char(sf, signal, signal.x + 2 + i, signal.y + 2,
                             i < lit ? LS_TUI_SHADE_FULL : LS_TUI_SHADE_25,
                             TUI_ATTR(i < lit ? TUI_GREEN | TUI_BRIGHT
                                              : TUI_BLACK | TUI_BRIGHT, TUI_BLACK));
            /* Level alone does not separate a strong signal from a front end
               being driven into its rails, and both read near 1.0. The share
               of samples pinned at 0 or 255 does, so it sits on the same
               line once there is enough of it to matter. */
            p25_acquisition_status_t acq;
            p25_get_acquisition_status(&acq);
            const uint32_t comps = acq.iq.sampled_pairs * 2u;
            const int clip_pct = comps
                ? (int)((acq.iq.clipped_components * 100u) / comps) : 0;
            if (clip_pct >= P25_CLIP_SHOW_PCT)
                snprintf(buf, sizeof(buf), "%.3f  CLIP %d%%",
                         (double)level, clip_pct);
            else
                snprintf(buf, sizeof(buf), "%.3f", (double)level);
            field(sf, signal, 4, "IQ", buf, label,
                  clip_pct >= P25_CLIP_SHOW_PCT
                      ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : value);
            field(sf, signal, 6, "RX", st.receiver_streaming ? "STREAMING" : "stopped",
                  label, st.receiver_streaming ? good : idle);
            field(sf, signal, 7, "AUDIO", voice_status(), label,
                  P25.p25_enc_muted ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : idle);
        }
        remain -= signal.h;
        int ah = remain;
        if (ah >= 3) {
            activity = tui_rect_make(area.x, signal.y + signal.h, area.w, ah);
            if (ah >= fits + 7) {
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

    tui_put_str(sf, r, r.x, r.y, "SIG",
                TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));

    char pct[6];
    snprintf(pct, sizeof(pct), "%3d%%", (int)(level * 100.0f + 0.5f));
    tui_put_str(sf, r, r.x + r.w - 4, r.y, pct,
                TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));

    /* Rising eighth-height traces, the same reading MESH's signal meter uses
       (scr_mesh.c, sigmeter) - each cell's own threshold so the bar fills
       left to right and grows taller as it goes, just driven by a fraction
       here instead of a dBm span. */
    const int cells = r.w - 9;
    for (int i = 0; i < cells; i++) {
        const float t = (float)(i + 1) / (float)cells;
        const int   h = 2 + (i * 6) / (cells > 1 ? cells - 1 : 1);
        const bool  lit = level >= t - (0.5f / (float)cells);
        const uint8_t hue = i > cells * 3 / 4 ? TUI_RED
                           : i > cells / 2     ? TUI_YELLOW
                                                : TUI_GREEN;
        tui_put_char(sf, r, r.x + 4 + i, r.y, LS_TUI_TRACE(h),
                     TUI_ATTR(lit ? (hue | TUI_BRIGHT) : (TUI_BLACK | TUI_BRIGHT),
                              TUI_BLACK));
    }
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

    const char *why = ls_wf_idle_reason();
    /* Keep HOLD reachable while the shared display is paused. */
    if (why && !ls_wf_cfg()->paused) {
        ls_panel_notice(sf, area, "SPECTRUM", why,
                        "the receiver feeds this once it is streaming");
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
        n += (size_t)snprintf(text + n, sizeof(text) - n,
            "NAC %03X, talkgroup %d, unit %d, %s. ",
            P25.dsd_nac, P25.dsd_tg, P25.dsd_src,
            P25.dsd_modulation[0] ? P25.dsd_modulation : "modulation unknown");

    if (n < sizeof(text))
        n += (size_t)snprintf(text + n, sizeof(text) - n,
            "Signal %.2f of full scale, gain %.1f dB. "
            "%d sync events and %d voice frames this session. Audio: %s. ",
            (double)P25.iq_level, P25.rtl_gain_tenths / 10.0,
            P25.dsd_sync_count, P25.dsd_voice_count, voice_status());

    /* Said plainly, the way the sub-GHz bookmark says it: the attachment is
       where the RECEIVER was when the note was kept. It is not where the
       transmitter is, and a trunked site can be tens of kilometres away. */
    if (n < sizeof(text))
        snprintf(text + n, sizeof(text) - n,
            "The position and motion attached describe this receiver when "
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
static char s_profiles[LS_PICKER_MAX][P25_PROGRAM_PATH_MAX];
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
    if (skipped) snprintf(title, sizeof(title), "P25 PROFILES  %d SKIPPED", skipped);
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
                              "choose a P25 profile from the card");
}

static void enter(void)
{
    register_p25_actions();
}

static const ls_btn_t PAGES[] = {
    { "DECODE", NULL, '1', false, false },
    { "SIGNAL", NULL, '2', false, false },
    { "SCAN", NULL, '3', false, false },
    { "SETTINGS", NULL, '4', false, false },
};
#define N_PAGES ((int)(sizeof(PAGES) / sizeof(PAGES[0])))

static tui_rect s_bar;
static tui_rect s_quick_rect;
static bool s_quick_compact;

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

/* The large face has only 46 rows.  The full pictorial sliders use 21 of
   them, hiding ACTIVITY on the very screen where the history matters.  Keep
   every action as a compact decode-style button when rows are scarce. */
static const ls_btn_t COMPACT_QUICK[] = {
    { "TUNE",  NULL, 't', false, false },
    { "VOL-",  NULL, '-', false, false },
    { "VOL+",  NULL, '+', false, false },
    { "GAIN-", NULL, 'j', false, false },
    { "GAIN+", NULL, 'u', false, false },
};
#define N_COMPACT_QUICK ((int)(sizeof(COMPACT_QUICK) / sizeof(COMPACT_QUICK[0])))

static void radio_view(void)
{
    memset(&s_view,0,sizeof(s_view));
    s_view.frequency=s_tune_freq_hz;
    s_view.mode="P25";
    s_view.power=P25.iq_level;
    s_view.volume=audio_volume_get();
    p25_get_receiver_status(&s_view.receiver);
    bool sync=s_view.receiver.receiver_streaming && P25.dsd_has_sync;
    if(sync) snprintf(s_view.detail[0],64,"NAC %03X  TG %d  UNIT %d",P25.dsd_nac,P25.dsd_tg,P25.dsd_src);
    else snprintf(s_view.detail[0],64,"NAC ---   TG ---   UNIT ---");
    snprintf(s_view.detail[1],64,"SYNC %s",sync?"LOCKED":"SEARCHING");
    snprintf(s_view.detail[2],64,"GAIN %.1f dB",P25.rtl_gain_tenths/10.0);
    snprintf(s_view.detail[3],64,"AUDIO %s",s_view.receiver.receiver_streaming?voice_status():"OFFLINE");
    if(p25_p2_enabled()) {
        s_view.mode="P25 II EXP";
        p25_p2_describe(s_view.detail[0],64);
        snprintf(s_view.detail[1],64,"MANUAL VOICE / 6000 baud");
        snprintf(s_view.detail[3],64,"Experimental; RF path unverified");
    }
}
static void radio_action(char c)
{
    if(c=='P') {ps_open=true;ls_wf_source_release();return;}
    if(c=='W') {s_page=1;return;}
    if(c=='M'||c=='D') {s_page=0;return;}
    if(c=='[' || c==']') {
        int64_t hz = (int64_t)s_tune_freq_hz + (c=='[' ? -12500 : 12500);
        if(hz<24000000) hz=24000000;
        if(hz>1766000000) hz=1766000000;
        ls_args_t a={.n=1}; ls_val_t out;
        a.v[0].kind=LS_VAL_FLOAT; a.v[0].f=(float)(hz/1e6);
        ls_action_call("p25.freq",&a,&out,ls_quick_grant_builtin());
        return;
    }
    if(c=='T') scan_engine_stop();
    if(c) ls_quick_key(c,QUICK,N_QUICK,ls_quick_grant_builtin(),NULL);
}

static void draw(tui_surface *sf, tui_rect area)
{
    if(ps_open) {snprintf(s_hint,sizeof(s_hint),"P25 SETTINGS  arrows select/change  ENTER edit");ps_draw(sf,area);return;}
    snprintf(s_hint,sizeof(s_hint),"%s",s_page==1 ? "LEFT/RIGHT select  SPACE tune  1/2/3 views" : "LEFT/RIGHT tune  1/2/3 views  +/- volume");
    s_blink++;
    const bool wide = ls_tui_is_wide();
    /* Three rows in portrait, not two. */

    const int bar_h = wide ? 3 : 5;

    tui_rect body;
    s_quick_compact = !wide && area.h < 55 && s_page == 0;
    if (wide) {
        /* LANDSCAPE HAS THE CONTROLS TOO. */

        const int want = ls_quick_rows(QUICK, N_QUICK, area.w, true);
        const int ctl_h = (area.h > want + 8) ? want : 0;
        s_bar = tui_rect_make(area.x, area.y, area.w, bar_h);
        s_quick_rect = ctl_h
            ? tui_rect_make(area.x, area.y + area.h - ctl_h, area.w, ctl_h)
            : tui_rect_make(0, -1, 0, 0);
        body = tui_rect_make(area.x, area.y + bar_h, area.w,
                             area.h - bar_h - ctl_h);
    } else {
        /* Page bar above the settings. Choosing a page and changing a
           setting are different kinds of thing, and the page bar was under
           the settings where it read as one more of them. */
        const int want = s_quick_compact ? 5
            : (s_page == 0 ? ls_quick_rows(QUICK, N_QUICK, area.w, false) : 0);
        const int ctl_h = (area.h > want + 12) ? want : 0;
        s_quick_rect = ctl_h
            ? tui_rect_make(area.x, area.y + area.h - ctl_h, area.w, ctl_h)
            : tui_rect_make(0, -1, 0, 0);
        /* The blank row goes ABOVE the page bar.

           The arithmetic reserved one and then put the bar directly under
           the page content, so the row of air ended up between the bar and
           the quick controls - where nothing needed separating - and the
           page's bottom border sat flush on the buttons. */
        const int used = bar_h + 1 + ctl_h;
        body = tui_rect_make(area.x, area.y, area.w, area.h - used);
        s_bar = tui_rect_make(area.x, body.y + body.h + 1, area.w, bar_h);
    }

    ls_btn_t b[N_PAGES];
    for (int i = 0; i < N_PAGES; i++) {
        b[i] = PAGES[i];
        b[i].on = (i == s_page);
    }
    if (area.w < 40) b[3].label = "SET";
    ls_btn_bar_slot(sf, s_bar, b, N_PAGES, -1, LS_BTN_SLOT_QUICK);

    if (body.h > 0) {
        if (s_page == 1) draw_signal(sf, body);
        else if (s_page == 2) {
            radio_view();
            ls_radio_panel_draw(&s_radio, &s_view, sf, body);
        } else draw_decode(sf, body);
    }

    if (s_quick_rect.h > 0) {
        if (s_quick_compact)
            ls_btn_bar(sf, s_quick_rect, COMPACT_QUICK, N_COMPACT_QUICK, -1);
        else
            ls_quick_draw_posture(sf, s_quick_rect, wide, QUICK, N_QUICK);
    }
}

/* The feed follows the page, and leaving the app releases it whichever page
   was up - the spectrum must not keep costing FFTs behind another screen. */
static void leave(void) { ls_wf_source_release(); }

static bool key(ls_tk_t k, char ch)
{
    if(k>=LS_TK_F1) return false;
    if(ps_open)return ps_key(k,ch);
    if(k==LS_TK_CHAR && (ch=='4'||((ch=='p'||ch=='P')&&s_page!=1))) {ps_open=true;ls_wf_source_release();return true;}
    if(k==LS_TK_CHAR && ch>='1'&&ch<='3') {s_page=ch-'1';return true;}
    /* 'l' for load. The profile chooser gets no quick-bar slot on purpose:
       every control there costs rows the waterfall and the activity table
       need, and choosing a profile is not what a thumb does while watching a
       decode. It lives on SETTINGS; this is the keyboard way in. p and P
       already open SETTINGS, and the waterfall owns a c d f g h k p r s y on
       SIGNAL, which is searched after this. */
    /* 'n' for note. LABS and SUB-GHZ both keep an observation on 'j', but 'j'
       is gain-down on this screen and moving it would break a binding that is
       in the hint line and in muscle memory. */
    if(k==LS_TK_CHAR && ch=='n') { mark_observation(); return true; }
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
        const int i = ls_btn_key(ch, PAGES, N_PAGES);
        if (i >= 0) { if(i==3) {ps_open=true;ls_wf_source_release();} else s_page=i; return true; }
        /* The same controls the panel draws, so a keyboard and a thumb reach
           them by one path. After the page keys, so a digit still pages. */
        if (ls_quick_key(ch, QUICK, N_QUICK,
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
    if(ps_open)return ps_touch(col,row);
    if (row >= s_bar.y && row < s_bar.y + s_bar.h) {
        const int i = ls_btn_hit_slot(col, row, LS_BTN_SLOT_QUICK);
        if (i >= 0) { if(i==3) {ps_open=true;ls_wf_source_release();} else s_page=i; return true; }
        return true;
    }
    if (s_quick_rect.h > 0 && row >= s_quick_rect.y &&
        row < s_quick_rect.y + s_quick_rect.h) {
        if (s_quick_compact) {
            const int i = ls_btn_hit(col, row);
            if (i >= 0 && i < N_COMPACT_QUICK)
                ls_quick_key(COMPACT_QUICK[i].key, QUICK, N_QUICK,
                             ls_quick_grant_builtin(), NULL);
            return true;
        }
        if (ls_quick_touch(col, row, QUICK, N_QUICK,
                           ls_quick_grant_builtin(), NULL)) return true;
    }
    if (s_page == 1) return ls_wf_touch(col, row);
    if (s_page == 2) { radio_view(); radio_action(ls_radio_panel_touch(&s_radio,&s_view,col,row)); return true; }
    return false;
}

const ls_tui_screen_t ls_scr_p25 = {
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
