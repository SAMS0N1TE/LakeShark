/* p25_tui.c - P25-specific TUI rendering.
 *
 * Two surfaces:
 *   draw_main   - sync/NAC/TG/SRC/frame state + BCH stats + radio status
 *   draw_signal - IQ-level meter + ring-buffer fill + decode rate +
 *                 a 12-line scrolling P25-only event log carved out of
 *                 the shared TUI log buffer
 *
 * Style mirrors adsb_render.c: C_LABEL for headings, C_GREEN/C_AMBER/
 * C_RED for traffic-light status, EL on every line so stale glyphs
 * from the previous frame get wiped without a full CLS.
 */

#include "app_registry.h"
#include "p25_state.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

#ifdef CONFIG_ENABLE_TUI

#include "tui.h"
#include "settings.h"
#include "rtl-sdr.h"
#include "dsp_pipeline.h"

extern volatile int rtl_gain_request;
extern int           autoscan_bch_ok_flag;
extern int           dsd_bch_fail_counter;
extern dsp_state_t   s_dsp;

/* ---- helpers ------------------------------------------------------- */

static void fmt_mhz(char *buf, size_t sz, uint32_t hz)
{
    uint32_t mhz = hz / 1000000;
    uint32_t khz = (hz % 1000000) / 1000;
    snprintf(buf, sz, "%lu.%03lu MHz",
             (unsigned long)mhz, (unsigned long)khz);
}

/* Render a horizontal level bar 0..1.0, width chars wide. Greens up to
 * 0.6, ambers up to 0.85, reds above. */
static void draw_level_bar(float v, int width)
{
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    int filled = (int)(v * width + 0.5f);
    if (filled > width) filled = width;
    /* Pick colour by fill level. */
    const char *col = v < 0.20f ? C_DIM   :
                      v < 0.60f ? C_GREEN :
                      v < 0.85f ? C_AMBER : C_RED;
    fputs(col, stdout);
    for (int i = 0; i < filled; i++) fputs("\xe2\x96\x88", stdout); /* full block */
    fputs(C_DIM, stdout);
    for (int i = filled; i < width; i++) fputs("\xe2\x96\x91", stdout); /* light shade */
    fputs(RESET, stdout);
}

/* Truncate fsubtype/err_str to a fixed display width so layout doesn't
 * shift when state grows. Pads with spaces. */
static void print_padded(const char *s, int w, const char *col)
{
    int n = s ? (int)strlen(s) : 0;
    if (n > w) n = w;
    if (col) fputs(col, stdout);
    if (s && n > 0) fwrite(s, 1, n, stdout);
    fputs(RESET, stdout);
    for (int i = n; i < w; i++) fputc(' ', stdout);
}

/* ---- MAIN page ----------------------------------------------------- */

/* Draw a horizontal rule with a label inset on the left. The rule
 * extends to ~width chars wide. Used as a section divider. */
static void draw_rule(int row, int col, const char *label, int width)
{
    tui_goto(row, col);
    fputs(C_BORDER, stdout);
    /* Two-char lead-in. */
    fputs(HL HL " ", stdout);
    if (label) printf(C_LABEL "%s " RESET C_BORDER, label);
    int used = (label ? (int)strlen(label) + 4 : 3);
    for (int i = used; i < width; i++) fputs(HL, stdout);
    fputs(RESET, stdout);
    fputs(EL, stdout);
}

void p25_draw_main(int top, int rows, int cols)
{
    char freqbuf[24];
    fmt_mhz(freqbuf, sizeof(freqbuf), s_tune_freq_hz);
    int width = cols - 2;
    if (width < 60) width = 60;
    if (width > 124) width = 124;

    /* Pre-clear the entire body region in one sweep. The page renderer
     * does NOT do a CLS between page switches (deliberately, to avoid
     * flicker), so when we arrive on this page from LOG/SIGNAL/etc the
     * previous page's content still occupies whatever rows we don't
     * write to. The gap rows MUST be cleared explicitly or text from
     * the previous page leaks through them. */
    for (int r = top; r < top + rows; r++) {
        tui_goto(r, 3);
        fputs(EL, stdout);
    }

    /* ---- SYNC / FREQ banner (top row) ---------------------------- */
    /* One-line status banner. The previous version drew a small box
     * around the SYNC indicator, but the box had a fixed width and
     * the rest of the row spilled past its right edge - looked
     * unfinished. This version is a single coloured strap with the
     * sync indicator on the left, frequency in the middle, and VOX
     * status on the right. Bold colour does the visual-weight work
     * the box was trying to do. */
    bool synced = P25.dsd_has_sync;
    bool voice  = P25.voice_active_until_us > esp_timer_get_time();
    const char *sync_col   = synced ? C_GREEN : C_DIM;
    const char *vox_col    = voice  ? C_AMBER : C_DIM;
    const char *vox_text   = voice  ? "VOX ACTIVE" : "vox idle  ";

    tui_goto(top, 3);
    /* Sync indicator: solid bullet + label */
    fputs(sync_col, stdout);
    if (synced) {
        printf(BOLD "\xe2\x97\x8f SYNC" RESET C_GREEN " %-4s",
               P25.dsd_modulation[0] ? P25.dsd_modulation : "C4FM");
    } else {
        printf("\xe2\x97\x8b NO SYNC " RESET);
    }
    fputs(RESET, stdout);
    /* Spacer */
    fputs("    ", stdout);
    /* Frequency */
    printf(C_LABEL "FREQ " RESET C_CYAN BOLD "%s" RESET, freqbuf);
    /* Spacer */
    fputs("    ", stdout);
    /* VOX status */
    printf("%s%s" RESET, vox_col, vox_text);
    fputs(EL, stdout);

    /* Subtle horizontal rule under the banner. */
    draw_rule(top + 1, 3, NULL, width - 4);

    /* ---- DECODE STATE ------------------------------------------- */
    draw_rule(top + 3, 3, "DECODE", width - 4);

    /* NAC / TG / SRC row. */
    tui_goto(top + 4, 5);
    printf(C_LABEL "NAC "  RESET);
    if (P25.dsd_nac) printf(C_GOLD BOLD "0x%03X" RESET, P25.dsd_nac);
    else if (P25.dsd_last_ok_nac) printf(C_DIM "0x%03X" RESET, P25.dsd_last_ok_nac);
    else printf(C_DIM "----- " RESET);
    printf("      " C_LABEL "TG "   RESET);
    if (P25.dsd_tg)  printf(C_BRIGHT BOLD "%-7d" RESET,  P25.dsd_tg);
    else             printf(C_DIM   "------- " RESET);
    printf("   " C_LABEL "SRC "  RESET);
    if (P25.dsd_src) printf(C_MAGENTA BOLD "%-8d" RESET, P25.dsd_src);
    else             printf(C_DIM     "-------- " RESET);
    printf(EL);

    /* Frame type / DUID. */
    tui_goto(top + 5, 5);
    printf(C_LABEL "FRAME " RESET);
    print_padded(P25.dsd_ftype[0]    ? P25.dsd_ftype    : "----", 14, C_BRIGHT);
    printf("   " C_LABEL "DUID " RESET);
    print_padded(P25.dsd_fsubtype[0] ? P25.dsd_fsubtype : "----", 14, C_GOLD);
    printf(EL);

    /* BCH ok/fail counters. */
    int ok    = P25.dsd_bch_ok_count;
    int fail  = P25.dsd_bch_fail_count;
    int total = ok + fail;
    int pct_x10 = total > 0 ? (ok * 1000) / total : 0;
    const char *bch_col = total == 0          ? C_DIM   :
                          pct_x10 >= 950      ? C_GREEN :
                          pct_x10 >= 700      ? C_AMBER : C_RED;
    tui_goto(top + 6, 5);
    printf(C_LABEL "BCH " RESET);
    printf(C_GREEN "ok %-5d" RESET " " C_RED "fail %-5d" RESET "    ", ok, fail);
    printf(C_LABEL "RATIO " RESET "%s%3d.%d%%" RESET,
           bch_col, pct_x10 / 10, pct_x10 % 10);
    printf("    " C_LABEL "VOICE " RESET C_BRIGHT "%d" RESET " frames",
           P25.dsd_voice_count);
    printf(EL);

    /* ---- RADIO -------------------------------------------------- */
    draw_rule(top + 8, 3, "RADIO", width - 4);

    /* IQ level bar. */
    tui_goto(top + 9, 5);
    printf(C_LABEL "INPUT " RESET);
    draw_level_bar(P25.iq_level, 32);
    printf("  %s%4d%%" RESET,
           P25.iq_level < 0.05f ? C_RED :
           P25.iq_level < 0.20f ? C_AMBER :
                                  C_GREEN,
           (int)(P25.iq_level * 100.0f + 0.5f));
    printf(EL);

    /* Ring fill bar. */
    int ring_pct = P25.ring_size > 0
        ? (P25.ring_fill * 100) / P25.ring_size : 0;
    tui_goto(top + 10, 5);
    printf(C_LABEL "BUFR  " RESET);
    draw_level_bar(P25.ring_size > 0
                       ? (float)P25.ring_fill / (float)P25.ring_size
                       : 0.0f,
                   32);
    printf("  " C_DIM "%5d / %5d" RESET, P25.ring_fill, P25.ring_size);
    (void)ring_pct;
    printf(EL);

    /* Gain row */
    tui_goto(top + 12, 5);
    printf(C_LABEL "RTL GAIN  " RESET);
    if (P25.rtl_gain_tenths > 0)
        printf(C_GREEN "%2d.%d dB" RESET,
               P25.rtl_gain_tenths / 10, P25.rtl_gain_tenths % 10);
    else
        printf(C_AMBER "AGC    " RESET);
    printf("     " C_LABEL "DEMOD GAIN  " RESET C_BRIGHT "%5.0f" RESET,
           (double)P25.demod_gain);
    printf("     " C_LABEL "BEEP " RESET "%s",
           P25.sync_beep_enabled ? C_GREEN "on " RESET : C_DIM "off" RESET);
    printf(EL);

    /* Throughput strap. */
    tui_goto(top + 13, 5);
    uint32_t kbps = P25.iq_bytes_sec / 1024;
    const char *thr_col = kbps == 0     ? C_RED   :
                          kbps < 1000   ? C_AMBER : C_GREEN;
    printf(C_LABEL "IQ RATE " RESET "%s%4lu KB/s" RESET "  "
           C_LABEL "AUDIO " RESET C_BRIGHT "%5lu sps" RESET "  "
           C_LABEL "READ ERR " RESET "%s%d" RESET,
           thr_col, (unsigned long)kbps,
           (unsigned long)P25.audio_samples_sec,
           P25.read_errors == 0 ? C_DIM : C_RED, P25.read_errors);
    printf(EL);

    /* ---- STATUS LINE -------------------------------------------- */
    draw_rule(top + 15, 3, "STATUS", width - 4);
    tui_goto(top + 16, 5);
    if (!P25.dsd_buffers_ok) {
        printf(C_RED BOLD "*** DSD BUFFERS NOT ALLOCATED ***" RESET);
    } else if (P25.dsd_sync_count == 0 && P25.iq_level < 0.05f) {
        printf(C_AMBER "\xe2\x97\x8f " RESET C_AMBER
               "Waiting for signal" RESET C_DIM
               "  (try [g] for more gain or check antenna)" RESET);
    } else if (P25.dsd_sync_count == 0) {
        printf(C_AMBER "\xe2\x97\x8f " RESET C_AMBER
               "Hunting for sync" RESET C_DIM
               "  (signal present at %d%%, no frame lock yet)" RESET,
               (int)(P25.iq_level * 100.0f + 0.5f));
    } else if (!P25.dsd_has_sync) {
        printf(C_DIM "\xe2\x97\x8b " RESET C_DIM
               "Lost sync (last NAC was 0x%03X, %d good frames)" RESET,
               P25.dsd_last_ok_nac, P25.dsd_sync_count);
    } else {
        printf(C_GREEN BOLD "\xe2\x97\x8f LOCKED " RESET
               C_GREEN "%s NAC=0x%03X" RESET,
               P25.dsd_modulation, P25.dsd_nac);
    }
    printf(EL);

    /* Help strap at the bottom of the page body. */
    tui_goto(top + rows - 2, 5);
    printf(C_DIM
           "[q/w] freq \xc2\xb1" "25kHz  [e] mode  [r] reset  [s] gain  [d] AGC  [f] beep"
           RESET EL);
}

/* ---- SIGNAL page (P25 demod focus) -------------------------------- */
/*
 * The framework's default Mode-S analyzer is wrong for P25 - its
 * "BURSTS at SNR>10x" heuristic is meaningless on a continuous-
 * symbol C4FM control channel. Replace it with a P25-relevant view:
 * IQ envelope strip, decode-rate bar graph, recent-events log.
 */

#define P25_RATE_HISTORY 60     /* one-second-per-cell, 60 seconds wide */
static int   s_rate_hist[P25_RATE_HISTORY];
static int   s_rate_head = 0;
static int   s_last_sync_count = 0;
static int   s_last_voice_count = 0;
static int64_t s_last_sample_us = 0;

static void rate_update(void)
{
    int64_t now = esp_timer_get_time();
    if (s_last_sample_us == 0) { s_last_sample_us = now; return; }
    if (now - s_last_sample_us < 1000000LL) return;
    s_last_sample_us = now;

    int dsync  = P25.dsd_sync_count  - s_last_sync_count;
    int dvoice = P25.dsd_voice_count - s_last_voice_count;
    s_last_sync_count  = P25.dsd_sync_count;
    s_last_voice_count = P25.dsd_voice_count;
    /* Encode sync rate in low byte, voice rate in next byte. Capped 0..255 each. */
    if (dsync  > 255) dsync  = 255;
    if (dsync  < 0)   dsync  = 0;
    if (dvoice > 255) dvoice = 255;
    if (dvoice < 0)   dvoice = 0;
    s_rate_hist[s_rate_head] = (dvoice << 8) | dsync;
    s_rate_head = (s_rate_head + 1) % P25_RATE_HISTORY;
}

void p25_draw_signal(int top, int rows, int cols)
{
    rate_update();

    char freqbuf[24];
    fmt_mhz(freqbuf, sizeof(freqbuf), s_tune_freq_hz);
    int width = cols - 2;
    if (width < 60)  width = 60;
    if (width > 124) width = 124;

    /* Pre-clear the entire body region. See note in p25_draw_main. */
    for (int r = top; r < top + rows; r++) {
        tui_goto(r, 3);
        fputs(EL, stdout);
    }

    /* ---- Header line --------------------------------------------- */
    tui_goto(top, 3);
    printf(C_LABEL "DEMOD " RESET C_CYAN BOLD "%s" RESET
           "    " C_LABEL "MOD " RESET C_BRIGHT "%-4s" RESET
           "    " C_LABEL "NAC " RESET,
           freqbuf,
           P25.dsd_modulation[0] ? P25.dsd_modulation : "----");
    if (P25.dsd_nac) printf(C_GOLD "0x%03X" RESET, P25.dsd_nac);
    else if (P25.dsd_last_ok_nac) printf(C_DIM "0x%03X" RESET, P25.dsd_last_ok_nac);
    else printf(C_DIM "-----" RESET);
    printf(EL);

    /* ---- INPUT level meter --------------------------------------- */
    draw_rule(top + 2, 3, "INPUT", width - 4);
    tui_goto(top + 3, 5);
    int barw = cols - 18;
    if (barw < 32) barw = 32;
    if (barw > 80) barw = 80;
    draw_level_bar(P25.iq_level, barw);
    printf("  %s%4d%%" RESET,
           P25.iq_level < 0.05f ? C_RED :
           P25.iq_level < 0.20f ? C_AMBER :
                                  C_GREEN,
           (int)(P25.iq_level * 100.0f + 0.5f));
    printf(EL);

    /* ---- Decode rate sparkline ----------------------------------- */
    /* 5-row tall sparkline (bigger than the previous 1-row strip) so
     * the activity is actually visible. Uses 8-level Unicode block
     * glyphs for the rightmost (most recent) end and shorter ones
     * trailing left. */
    draw_rule(top + 5, 3, "DECODE RATE   60s", width - 4);

    static const char *BARS[9] = {
        " ",
        "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
    };

    int vmax = 1, smax = 1;
    for (int i = 0; i < P25_RATE_HISTORY; i++) {
        int e = s_rate_hist[i];
        int s = e & 0xFF;
        int v = (e >> 8) & 0xFF;
        if (v > vmax) vmax = v;
        if (s > smax) smax = s;
    }

    /* Voice events: amber, on top */
    tui_goto(top + 6, 5);
    printf(C_LABEL "voice " RESET C_AMBER);
    for (int i = 0; i < P25_RATE_HISTORY; i++) {
        int idx = (s_rate_head + i) % P25_RATE_HISTORY;
        int v = (s_rate_hist[idx] >> 8) & 0xFF;
        int g = (v * 8) / vmax;
        if (g < 0) g = 0;
        if (g > 8) g = 8;
        fputs(BARS[g], stdout);
    }
    printf(RESET "  " C_DIM "max %d/s" RESET EL, vmax);

    /* Sync events: green, below */
    tui_goto(top + 7, 5);
    printf(C_LABEL "sync  " RESET C_GREEN);
    for (int i = 0; i < P25_RATE_HISTORY; i++) {
        int idx = (s_rate_head + i) % P25_RATE_HISTORY;
        int s = s_rate_hist[idx] & 0xFF;
        int g = (s * 8) / smax;
        if (g < 0) g = 0;
        if (g > 8) g = 8;
        fputs(BARS[g], stdout);
    }
    printf(RESET "  " C_DIM "max %d/s" RESET EL, smax);

    /* Time-axis labels */
    tui_goto(top + 8, 5);
    fputs("      ", stdout);
    fputs(C_DIM "\xe2\x86\x90 60s ago" RESET, stdout);
    int gap = P25_RATE_HISTORY - 11 - 3;
    for (int i = 0; i < gap; i++) fputc(' ', stdout);
    printf("now \xe2\x86\x92" RESET EL);

    /* ---- TOTALS -------------------------------------------------- */
    draw_rule(top + 10, 3, "TOTALS", width - 4);

    int total = P25.dsd_bch_ok_count + P25.dsd_bch_fail_count;
    int pct_x10 = total > 0 ? (P25.dsd_bch_ok_count * 1000) / total : 0;
    const char *bch_col = total == 0       ? C_DIM   :
                          pct_x10 >= 950   ? C_GREEN :
                          pct_x10 >= 700   ? C_AMBER : C_RED;

    tui_goto(top + 11, 5);
    printf(C_LABEL "frames    " RESET);
    printf("sync "  C_GREEN  BOLD "%-6d" RESET, P25.dsd_sync_count);
    printf("  voice " C_AMBER BOLD "%-6d" RESET, P25.dsd_voice_count);
    printf(EL);

    tui_goto(top + 12, 5);
    printf(C_LABEL "BCH       " RESET);
    printf("ok "  C_GREEN BOLD "%-6d" RESET,         P25.dsd_bch_ok_count);
    printf("  fail " C_RED  BOLD "%-6d" RESET,       P25.dsd_bch_fail_count);
    printf("  ratio " "%s%3d.%d%%" RESET,            bch_col,
                                                     pct_x10 / 10, pct_x10 % 10);
    printf(EL);

    /* ---- LAST ERROR --------------------------------------------- */
    draw_rule(top + 14, 3, "LAST ERR", width - 4);
    tui_goto(top + 15, 5);
    if (P25.dsd_err_str[0]) {
        printf(C_AMBER "%.*s" RESET, (int)sizeof(P25.dsd_err_str) - 1,
               P25.dsd_err_str);
    } else {
        printf(C_DIM "(none)" RESET);
    }
    printf(EL);

    /* Help strap. */
    tui_goto(top + rows - 2, 5);
    printf(C_DIM
           "[q/w] freq \xc2\xb1" "25kHz  [e] mode  [r] reset  [s] gain  [d] AGC  [f] beep"
           RESET EL);
}

/* ---- key handling -------------------------------------------------- */

void p25_on_key(tui_key_t k)
{
    int kk = (int)k;
    switch (kk) {
    /* ---- left hand: tuning + mode + reset ----------------------- */

    case 'q': case 'Q':
    case '<': case ',': {
        /* Step down 25 kHz. */
        const app_t *a = app_current();
        uint32_t f = (a ? settings_get_freq(a) : s_tune_freq_hz);
        if (f > 25000) f -= 25000;
        s_tune_freq_hz = f;
        if (a) settings_set_freq(a, f);
        if (rtldev) rtlsdr_set_center_freq(rtldev, f);
        sys_log(1, "Tune %.4f MHz", f / 1e6);
        break;
    }

    case 'w': case 'W':
    case '>': case '.': {
        /* Step up 25 kHz. */
        const app_t *a = app_current();
        uint32_t f = (a ? settings_get_freq(a) : s_tune_freq_hz) + 25000;
        s_tune_freq_hz = f;
        if (a) settings_set_freq(a, f);
        if (rtldev) rtlsdr_set_center_freq(rtldev, f);
        sys_log(1, "Tune %.4f MHz", f / 1e6);
        break;
    }

    case 'e': case 'E': {
        /* Cycle demod mode: C4FM -> CQPSK -> DIFF_4FSK -> FSK4_TRACKING -> wrap.
         * The dsp_pipeline supports four demods. C4FM is the default for
         * P25 Phase 1 Voice. CQPSK / DIFF_4FSK / FSK4_TRACKING are useful
         * for systems that drift on C4FM or for harder-to-decode sites. */
        static const char *names[] = { "C4FM", "CQPSK", "DIFF_4FSK", "FSK4_TRACKING" };
        int next = (s_dsp.mode + 1) % 4;
        dsp_set_mode(&s_dsp, (demod_mode_t)next);
        sys_log(1, "DSP mode: %s", names[next]);
        break;
    }

    case 'r': case 'R':
        /* Reset visible counters - useful for a clean read after tuning. */
        P25.dsd_sync_count       = 0;
        P25.dsd_voice_count      = 0;
        P25.dsd_bch_ok_count     = 0;
        P25.dsd_bch_fail_count   = 0;
        autoscan_bch_ok_flag     = 0;
        dsd_bch_fail_counter     = 0;
        memset(s_rate_hist, 0, sizeof(s_rate_hist));
        sys_log(1, "Stats reset");
        break;

    /* ---- right hand: gain + audio --------------------------------- */

    case 's': case 'S': {
        /* Step gain through the R820T2 ladder. Persist via settings_set_gain
         * so the new value survives switches and reboots. The async
         * rtl_gain_request still drives the live retune. */
        static const int gains[] = { 0, 90, 200, 280, 340, 370, 400, 437, 463, 496 };
        const int n = sizeof(gains) / sizeof(gains[0]);
        int cur = P25.rtl_gain_tenths;
        int next_idx = 0;
        for (int i = 0; i < n; i++) if (gains[i] == cur) { next_idx = (i + 1) % n; break; }
        rtl_gain_request = gains[next_idx];
        const app_t *a = app_current();
        if (a) settings_set_gain(a, gains[next_idx]);
        break;
    }

    case 'd': case 'D': {
        /* AGC toggle: 0 dB tenths means "let the tuner manage gain". */
        rtl_gain_request = 0;
        const app_t *a = app_current();
        if (a) settings_set_gain(a, 0);
        break;
    }

    case 'f': case 'F':
        P25.sync_beep_enabled = !P25.sync_beep_enabled;
        sys_log(1, "Beep %s", P25.sync_beep_enabled ? "ON" : "OFF");
        break;

    default: break;
    }
    tui_mark_dirty();
}

#endif /* CONFIG_ENABLE_TUI */
