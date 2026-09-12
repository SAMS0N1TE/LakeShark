#include "ls_ctl.h"
#include "settings.h"
#include "location_pref.h"
#include "scan_channels.h"
#include "scan_engine.h"
#include "lakeshark_backend.h"
/**/
#include "fm_state.h"
/**/
#include "p25_state.h"
/**/
#include "audio_out.h"
/**/
#include "ls_time.h"
#include "ls_board.h"
/**/
#include "ls_crash.h"

#ifdef ESP_PLATFORM
#include "esp_app_desc.h"
#endif
/**/
#include "ls_version.h"
/**/
#include "ls_safe_mode.h"
/**/
#include "p25_state.h"
#include "dsd.h"
#include "p25_iq_capture.h"
#include "esp_timer.h"
#include "esp_console.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ls_ctl";

static int cmd_home(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "unset")) {
        printf("%s\n", settings_clear_home() ? "home unset; save queued" : "home not changed: settings unavailable or queue full");
        return 0;
    }
    if (argc < 3) {
        float lat = 0, lon = 0;
        if (settings_get_home(&lat, &lon)) printf("home=%.5f,%.5f\n", lat, lon);
        else printf("home=unset (usage: home <lat> <lon>)\n");
        return 0;
    }
    double lat, lon;
    if (argc != 3 || !location_parse(argv[1], false, &lat) || !location_parse(argv[2], true, &lon)) {
        printf("bad coords (lat -90..90, lon -180..180)\n");
        return 0;
    }
    if (settings_set_home((float)lat, (float)lon)) printf("home=%.5f,%.5f; save queued\n", lat, lon);
    else printf("home not changed: settings unavailable or queue full\n");
    return 0;
}

static void ch_list(void)
{
    int n = scan_channels_count();
    printf("channels: %d\n", n);
    for (int i = 0; i < n; i++) {
        const scan_channel_t *c = scan_channel_get(i);
        if (!c) continue;
        printf("  %2d  %-12s %9.4f MHz  %-3s z%d%s%s%s\n",
               i, c->name, c->freq_hz / 1e6, scan_mode_name(c->mode), c->zone,
               (c->flags & SCAN_FLAG_ENABLED)  ? "" : " off",
               (c->flags & SCAN_FLAG_LOCKOUT)  ? " lock" : "",
               (c->flags & SCAN_FLAG_PRIORITY) ? " pri" : "");
    }
}

static int cmd_ch(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "list")) { ch_list(); return 0; }

    if (!strcmp(argv[1], "add")) {
        if (argc < 4) { printf("usage: ch add <MHz> <p25|nfm|wfm> [name] [zone]\n"); return 0; }
        uint32_t hz = (uint32_t)(atof(argv[2]) * 1e6 + 0.5);
        int mode = scan_mode_parse(argv[3]);
        if (mode < 0) { printf("bad mode (p25|nfm|wfm)\n"); return 0; }
        const char *name = (argc > 4) ? argv[4] : NULL;
        int zone = (argc > 5) ? atoi(argv[5]) : 0;
        if (zone < 0 || zone >= SCAN_MAX_ZONES) { printf("bad zone (0-%d)\n", SCAN_MAX_ZONES - 1); return 0; }
        /**/
        int dup = scan_channel_find_freq_zone(hz, (uint8_t)zone);
        if (dup >= 0) { printf("already ch %d in zone %d - not added\n", dup, zone); return 0; }
        int idx = scan_channel_add(name, hz, (scan_mode_t)mode, (uint8_t)zone);
        if (idx < 0) printf("add failed (list full or invalid freq)\n");
        else printf("added ch %d\n", idx);
        return 0;
    }

    /**/
    /* Bulk import: one commit for the whole list instead of one per channel.
       Frequencies are MHz, comma separated, and every channel in the push
       shares the mode and zone - which is what a band plan for one trip
       actually looks like. Existing channels in that zone are kept and
       re-adding one is a no-op, so re-running an import is safe. */
    if (!strcmp(argv[1], "import")) {
        if (argc < 4) {
            printf("usage: ch import <p25|nfm|wfm> <zone> <MHz>[,<MHz>...] [name-prefix]\n"
                   "       ch import nfm 1 154.400,154.010,162.475 SED\n");
            return 0;
        }
        int mode = scan_mode_parse(argv[2]);
        if (mode < 0) { printf("bad mode (p25|nfm|wfm)\n"); return 0; }
        int zone = atoi(argv[3]);
        if (zone < 0 || zone >= SCAN_MAX_ZONES) { printf("bad zone (0-%d)\n", SCAN_MAX_ZONES - 1); return 0; }
        if (argc < 5) { printf("nothing to import\n"); return 0; }
        const char *prefix = (argc > 5) ? argv[5] : NULL;

        int added = 0, dupes = 0, bad = 0;
        scan_channels_batch_begin();
        const char *p = argv[4];
        int seq = 1;
        while (*p) {
            while (*p == ',' || *p == ' ') p++;
            if (!*p) break;
            char *end = NULL;
            double mhz = strtod(p, &end);
            if (end == p) { bad++; break; }
            p = end;
            uint32_t hz = (uint32_t)(mhz * 1e6 + 0.5);
            if (scan_channel_find_freq_zone(hz, (uint8_t)zone) >= 0) { dupes++; seq++; continue; }
            char nm[SCAN_NAME_LEN];
            if (prefix) snprintf(nm, sizeof(nm), "%.*s%d", SCAN_NAME_LEN - 4, prefix, seq);
            else        snprintf(nm, sizeof(nm), "%.4f", mhz);
            if (scan_channel_add(nm, hz, (scan_mode_t)mode, (uint8_t)zone) >= 0) added++;
            else { bad++; break; }   /* list full - stop, do not spin */
            seq++;
        }
        bool ok = scan_channels_batch_end();
        printf("import: %d added, %d already present, %d rejected, zone %d %s - %s\n",
               added, dupes, bad, zone, scan_mode_name((uint8_t)mode),
               ok ? "committed once" : "COMMIT FAILED");
        return 0;
    }
    if (!strcmp(argv[1], "del")) {
        if (argc < 3) { printf("usage: ch del <idx>\n"); return 0; }
        printf("%s\n", scan_channel_remove(atoi(argv[2])) ? "removed" : "bad idx");
        return 0;
    }
    if (!strcmp(argv[1], "lock") || !strcmp(argv[1], "pri") || !strcmp(argv[1], "en")) {
        if (argc < 4) { printf("usage: ch %s <idx> on|off\n", argv[1]); return 0; }
        int idx = atoi(argv[2]);
        bool on = !strcmp(argv[3], "on") || !strcmp(argv[3], "1");
        bool ok;
        if      (!strcmp(argv[1], "lock")) ok = scan_channel_set_lockout(idx, on);
        else if (!strcmp(argv[1], "pri"))  ok = scan_channel_set_priority(idx, on);
        else                               ok = scan_channel_set_enabled(idx, on);
        printf("%s\n", ok ? "ok" : "bad idx");
        return 0;
    }
    if (!strcmp(argv[1], "clear")) { scan_channels_clear(); printf("cleared\n"); return 0; }

    /**/
    if (!strcmp(argv[1], "zone")) {
        if (argc < 3) {
            int z = scan_engine_get_zone();
            if (z < 0) printf("zone=all\n"); else printf("zone=%d\n", z);
            return 0;
        }
        int z = !strcmp(argv[2], "all") ? -1 : atoi(argv[2]);
        scan_engine_set_zone(z);
        z = scan_engine_get_zone();
        if (z < 0) printf("zone=all - every channel is in the sweep\n");
        else       printf("zone=%d - only channels in zone %d are scanned\n", z, z);
        return 0;
    }

    printf("usage: ch [list|add|import|del|lock|pri|en|clear|zone <0-7|all>]\n");
    return 0;
}

/**/
/* reasoned the scanner's NVS write rate from SETTLE_MS+MEASURE_MS and
   said so; settings_write_stats() has always had the real counters and
   nothing surfaced them. This is that surface. dropped>0 means the settings
   queue filled and a write was thrown away - which is how a volume or gain
   change made while scanning silently fails to stick. */
static int cmd_stats(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint32_t done = 0, dropped = 0, commits = 0;
    settings_write_stats(&done, &dropped, &commits);
    printf("settings nvs: done=%lu dropped=%lu commits=%lu\n",
           (unsigned long)done, (unsigned long)dropped, (unsigned long)commits);
    if (dropped) printf("  *** %lu write(s) DROPPED - a setting changed then did not stick\n",
                        (unsigned long)dropped);
    printf("channels: %d stored, batch %s\n",
           scan_channels_count(), scan_channels_batching() ? "OPEN (unsaved)" : "closed");
    return 0;
}

static int cmd_scan(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "status")) {
        char st[96]; scan_engine_status(st, sizeof(st));
        printf("scan: %s\n", st);
        return 0;
    }
    if (!strcmp(argv[1], "on")  || !strcmp(argv[1], "start")) { scan_engine_start(); printf("scan on\n");  return 0; }
    if (!strcmp(argv[1], "off") || !strcmp(argv[1], "stop"))  { scan_engine_stop();  printf("scan off\n"); return 0; }
    if (!strcmp(argv[1], "skip")) { scan_engine_skip(); printf("skip\n"); return 0; }
    if (!strcmp(argv[1], "hang")) {
        if (argc < 3) { printf("usage: scan hang <ms>\n"); return 0; }
        scan_engine_set_hang_ms(atoi(argv[2])); printf("hang=%d ms\n", atoi(argv[2])); return 0;
    }
    /**/
    if (!strcmp(argv[1], "thresh")) {
        if (argc < 3) { printf("usage: scan thresh <1-100 %% of full scale>\n"); return 0; }
        scan_engine_set_threshold_pct(atoi(argv[2]));
        printf("thresh=%d %% of full scale\n", scan_engine_get_threshold_pct());
        return 0;
    }
    /**/
    if (!strcmp(argv[1], "pri")) {
        if (argc < 3) {
            printf("pri=%d ms\n", scan_engine_get_priority_ms());
            return 0;
        }
        int ms = !strcmp(argv[2], "off") ? 0 : atoi(argv[2]);
        scan_engine_set_priority_ms(ms);
        int now = scan_engine_get_priority_ms();
        if (now) printf("pri=%d ms - holds are interrupted to sample priority channels\n", now);
        else     printf("pri=off\n");
        return 0;
    }
    /**/
    if (!strcmp(argv[1], "asql")) {
        int margin = (argc > 2) ? atoi(argv[2]) : -1;
        scan_engine_autosquelch(margin);
        printf("autosql requested%s - measuring the floor, watch `scan status`\n",
               (margin >= 0) ? " (new margin)" : "");
        return 0;
    }
    /**/
    if (!strcmp(argv[1], "src")) {
        if (argc < 3) {
            printf("src=%s\n", scan_engine_get_source() == SCAN_SRC_BAND ? "band" : "preset");
            return 0;
        }
        if (!strcmp(argv[2], "band"))        scan_engine_set_source(SCAN_SRC_BAND);
        else if (!strcmp(argv[2], "preset") ||
                 !strcmp(argv[2], "ch"))     scan_engine_set_source(SCAN_SRC_CHANNELS);
        else { printf("usage: scan src <preset|band>\n"); return 0; }
        printf("src=%s\n", scan_engine_get_source() == SCAN_SRC_BAND ? "band" : "preset");
        return 0;
    }
    /**/
    if (!strcmp(argv[1], "band")) {
        uint32_t a = 0, b = 0, st = 0;
        if (argc < 4) {
            scan_engine_get_band(&a, &b, &st);
            printf("band %.4f-%.4f MHz step %.1f kHz - %d steps\n",
                   a / 1e6, b / 1e6, st / 1000.0,
                   scan_engine_band_steps());
            printf("usage: scan band <startMHz> <stopMHz> [stepkHz]\n");
            return 0;
        }
        a  = (uint32_t)(atof(argv[2]) * 1e6 + 0.5);
        b  = (uint32_t)(atof(argv[3]) * 1e6 + 0.5);
        st = (argc > 4) ? (uint32_t)(atof(argv[4]) * 1000 + 0.5) : 0;
        if (!scan_engine_set_band(a, b, st)) {
            printf("bad band (need start<stop, >=1 MHz, step >=0.1 kHz)\n");
            return 0;
        }
        scan_engine_get_band(&a, &b, &st);
        /* Selecting a band is the intent to scan it - switch the source too,
           so this is one command and not a two-step people forget. */
        scan_engine_set_source(SCAN_SRC_BAND);
        printf("band %.4f-%.4f MHz step %.1f kHz - %d steps, src=band\n",
               a / 1e6, b / 1e6, st / 1000.0,
               scan_engine_band_steps());
        return 0;
    }
    printf("usage: scan [on|off|status|skip|hang <ms>|thresh <1-100>|pri <ms|off>\n"
           "            |src <preset|band>|band <startMHz> <stopMHz> [stepkHz]\n"
           "            |asql [margin]]\n");
    return 0;
}

/**/
/* `p25tsbk` - print the P25 control-channel state the TSBK dispatch has
 * accumulated: IDEN table, neighbour list from ADJ_STS_BCST, RFSS/site
 * from RFSS_STS_BCST, TSBK counts, and per-opcode counters for the
 * opcodes the parser recognises but does nothing else with. On an
 * unfamiliar system this last line is the fastest way to see what the
 * site actually emits. */
static int cmd_p25tsbk(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* 2 KiB is enough for a full 16-slot IDEN table + 16 neighbours +
     * every unhandled opcode counter, and fits on the REPL stack. */
    static char buf[2048];
    size_t n = p25_tsbk_status_format(&s_dsd_state, buf, sizeof(buf));
    if (n > 0) fputs(buf, stdout);
    if (n >= sizeof(buf)) printf("[truncated - %zu bytes]\n", n);
    return 0;
}

static int cmd_p25gate(int argc, char **argv)
{
    if (argc < 2) {
        printf("p25 voice gate = %d (lower mutes more error frames; 99 = play all)\n",
               lakeshark_p25_voice_gate());
        return 0;
    }
    lakeshark_p25_set_voice_gate(atoi(argv[1]));
    printf("p25 voice gate = %d\n", lakeshark_p25_voice_gate());
    return 0;
}

/* Keep the 768-byte acquisition formatter off the capture export frame. */
static __attribute__((noinline)) int print_p25_acquisition(void)
{
    p25_acquisition_status_t status;
    char output[768];
    p25_get_acquisition_status(&status);
    size_t n = p25_acquisition_format(&status, output, sizeof(output));
    fputs(output, stdout);
    if (n >= sizeof(output)) puts("[acquisition snapshot truncated]");
    return 0;
}

static int cmd_p25(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "capture"))
        return p25_iq_capture_command(argc - 2, argv + 2,
            (uint32_t)(esp_timer_get_time() / 1000LL));
    if (argc != 2 || strcmp(argv[1], "acquisition")) {
        puts("usage: p25 acquisition | capture start [blocks] | status | cancel | read <offset> [bytes] | free");
        return 0;
    }
    return print_p25_acquisition();
}

/* `p25enc` - status and settings for the P25 encryption gate. */

static int cmd_p25enc(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "leave")) {
        int on = !strcmp(argv[2], "on") || !strcmp(argv[2], "1");
        p25_set_leave_on_encrypted(on ? true : false);
        printf("p25enc leave=%s\n", on ? "on" : "off");
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "skip")) {
        int s = atoi(argv[2]);
        if (s < 0) s = 0;
        if (s > 3600) s = 3600;
        p25_set_encrypted_skip_ms((unsigned int)s * 1000u);
        printf("p25enc skip=%d s\n", s);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "reset")) {
        P25.p25_enc_muted_frames_total = 0;
        P25.p25_enc_returns            = 0;
        P25.p25_enc_skips              = 0;
        printf("p25enc counters cleared\n");
        return 0;
    }

    const char *algid_now = p25_algid_name(P25.p25_algid);
    printf("p25enc leave=%s skip=%us muted-frames=%u returns=%u skips=%u evictions=%u\n",
           p25_get_leave_on_encrypted() ? "on" : "off",
           p25_get_encrypted_skip_ms() / 1000u,
           (unsigned)P25.p25_enc_muted_frames_total,
           (unsigned)P25.p25_enc_returns,
           (unsigned)P25.p25_enc_skips,
           (unsigned)P25.p25_enc_tg_evictions);
    printf("  current: tg=%u algid=0x%02X %s kid=0x%04X ess=%s muted=%s\n",
           (unsigned)P25.grant_talkgroup,
           (unsigned)P25.p25_algid,
           algid_now ? algid_now : "?",
           (unsigned)P25.p25_kid,
           P25.p25_ess_valid ? "valid" : "unknown",
           P25.p25_enc_muted ? "yes" : "no");
    if (P25.p25_enc_tg_count == 0) {
        printf("  tg-history: (none)\n");
    } else {
        printf("  tg-history:\n");
        for (unsigned i = 0; i < P25.p25_enc_tg_count; i++) {
            const char *nm = p25_algid_name(P25.p25_enc_tg[i].algid);
            printf("    tg=%-5u algid=0x%02X %-8s kid=0x%04X",
                   (unsigned)P25.p25_enc_tg[i].talkgroup,
                   (unsigned)P25.p25_enc_tg[i].algid,
                   nm ? nm : "?",
                   (unsigned)P25.p25_enc_tg[i].kid);
            if (P25.p25_enc_tg[i].skip_remaining_ms > 0)
                printf("  skip=%ds", P25.p25_enc_tg[i].skip_remaining_ms / 1000);
            printf("\n");
        }
    }
    return 0;
}

/**/
static int cmd_vol(int argc, char **argv)
{
    if (argc < 2) { printf("vol=%d mute=%d\n", audio_volume_get(), audio_is_muted()); return 0; }
    if (argv[1][0] == '+' || argv[1][0] == '-') audio_volume_delta(atoi(argv[1]));
    else                                         audio_volume_set(atoi(argv[1]));
    printf("vol=%d\n", audio_volume_get());
    return 0;
}

/**/
static int cmd_mute(int argc, char **argv)
{
    (void)argc; (void)argv;
    audio_toggle_mute();
    printf("mute=%d\n", audio_is_muted());
    return 0;
}

/**/
/* `spec` - the spectrum as text. This exists because every debugging session
   on this project happens down a serial cable through cmd.py, and a signal
   finder you can only read by looking at the panel is unusable from the bench.
   It prints the same FM.scan_db the SWEEP tab plots, so the screen and the
   console cannot disagree - the same rule the scanner status line follows. */
static int cmd_spec(int argc, char **argv)
{
    /**/
    /* `spec band <a> <b>` - the SWEEP range was settable ONLY from the FM
       app's BAND button, cycling a fixed preset table, so it could not be
       aimed from the console at all. Same split fixed for the channel
       scanner; the sweep had it too. */
    if (argc >= 4 && strcmp(argv[1], "band") == 0) {
        double a = atof(argv[2]), b = atof(argv[3]);
        uint32_t ah = (uint32_t)(a * 1e6), bh = (uint32_t)(b * 1e6);
        if (ah < 1000000UL || bh <= ah) {
            printf("spec: bad range (want MHz, stop > start)\n");
            return 0;
        }
        FM.scan_start_hz = ah;
        FM.scan_stop_hz  = bh;
        lakeshark_fm_set_mode(FM_MODE_SCAN);
        lakeshark_fm_scan_restart();
        printf("spec: sweep %.4f-%.4f MHz\n", ah / 1e6, bh / 1e6);
        return 0;
    }

    int top = 12;
    if (argc >= 2) {
        int v = atoi(argv[1]);
        if (v > 0 && v <= 64) top = v;
    }

    int bins = FM.scan_bins;
    if (bins < 1 || bins > FM_SCAN_BINS_MAX) {
        printf("spec: no sweep data - open FM and select SWEEP\n");
        return 0;
    }
    uint32_t span = (FM.scan_stop_hz > FM.scan_start_hz)
                  ? (FM.scan_stop_hz - FM.scan_start_hz) : 0;
    if (!span) { printf("spec: empty range\n"); return 0; }

    printf("spec: %lu.%04lu-%lu.%04lu MHz  %d bins  %lu Hz/bin  sweeps=%lu tune %d/%d\n",
           (unsigned long)(FM.scan_start_hz / 1000000UL),
           (unsigned long)((FM.scan_start_hz % 1000000UL) / 100UL),
           (unsigned long)(FM.scan_stop_hz / 1000000UL),
           (unsigned long)((FM.scan_stop_hz % 1000000UL) / 100UL),
           bins, (unsigned long)(span / (uint32_t)bins),
           (unsigned long)FM.scan_sweeps, FM.scan_idx, FM.scan_tunes);

    /* Rank a COPY. FM.scan_db is live sweep data owned by the rx task; a
       listing must never consume it to mark what it has already printed. */
    static float snap[FM_SCAN_BINS_MAX];
    for (int i = 0; i < bins; i++) snap[i] = FM.scan_db[i];

    /* Strongest bins first - on a finder that is the entire question. */
    for (int r = 0; r < top; r++) {
        int   best = -1;
        float bv   = 0.0f;
        for (int i = 0; i < bins; i++)
            if (snap[i] > bv) { bv = snap[i]; best = i; }
        if (best < 0) break;

        uint32_t f = FM.scan_start_hz + (uint32_t)(((uint64_t)best * span) / bins);
        int pct = (int)(bv * 100.0f);
        int fill = pct * 40 / 100;
        if (fill < 0)  fill = 0;
        if (fill > 40) fill = 40;
        char bar[41];
        for (int i = 0; i < 40; i++) bar[i] = (i < fill) ? '#' : '.';
        bar[40] = 0;
        printf("  %2d  %lu.%04lu MHz  %3d%%  %s\n", r + 1,
               (unsigned long)(f / 1000000UL),
               (unsigned long)((f % 1000000UL) / 100UL), pct, bar);

        /* Suppress this peak AND its immediate neighbours, so a single strong
           carrier spread over three bins does not fill the whole listing. */
        for (int i = best - 2; i <= best + 2; i++)
            if (i >= 0 && i < bins) snap[i] = 0.0f;
    }
    return 0;
}

/**/
/* `date` - the honest report of what this device thinks the time is. Prints
   the ISO-8601 UTC stamp once SNTP has landed, and an "up <secs>s" marker
   until then, which is the same string ls_time_render_stamp writes into any
   file this session produces. A confidently wrong wall-clock date is worse
   than no date - see ls_time.c. */
static int cmd_date(int argc, char **argv)
{
    (void)argc; (void)argv;
    char stamp[LS_TIME_STAMP_MAX];
    ls_time_render_stamp(stamp, sizeof(stamp));
    printf("%s  (%s)\n", stamp,
           ls_time_is_synced() ? "wall clock set"

           : (LS_HAS_RTC ? "no wall clock yet - 'rtc set <unix>' or join a WiFi network"
                         : "no wall clock yet - join a WiFi network"));
    return 0;
}

/**/
/* `version` says which build is running - PROJECT_VER from
   esp_app_get_description() (which IDF derives from `git describe --dirty`),
   the board variant it was compiled for (from LS_BOARD_NAME, not a
   CONFIG_LS_BOARD_* test), the build date/time and the IDF revision.

   Before this the firmware could not identify itself: a screenshot, a bug
   report or a .sub file all landed without any way to tie them back to a
   particular commit, and with two agents committing through
   bench/agent-loop.ps1 "which build is on this device" had stopped being
   answerable from memory. */
static int cmd_version(int argc, char **argv)
{
    (void)argc; (void)argv;
    char buf[LS_VERSION_LINE_MAX];
    ls_version_line(buf, sizeof(buf));
    printf("%s\n", buf);
    return 0;
}

/**/
/* `crash` reports bounded metadata, never parses ELF on-device.
   Export and preserve the full partition before explicit crash clear. */
static int cmd_crash(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "clear")) {
        if (ls_crash_erase()) printf("crash: erased\n");
        else                  printf("crash: erase failed - see log\n");
        return 0;
    }
    /* One buffer big enough for a full RISC-V summary with a reason string
       and a 16-frame Xtensa backtrace. On the REPL task stack (4KB). */
    char buf[768];
    ls_crash_format(buf, sizeof(buf));
    fputs(buf, stdout);

    /* Say which build is running, because that is what decides whether the stored dump can be read at all. */

#ifdef ESP_PLATFORM
    {
        const esp_app_desc_t *d = esp_app_get_description();
        if (d) {
            printf("crash: this firmware's ELF sha256 starts %02x%02x%02x%02x\n",
                   d->app_elf_sha256[0], d->app_elf_sha256[1],
                   d->app_elf_sha256[2], d->app_elf_sha256[3]);
            printf("crash: esp-coredump refuses a dump whose sha differs. If "
                   "it reports another one, the dump is from an older build "
                   "and needs THAT build's ELF, which this repo does not "
                   "keep.\n");
        }
    }
#endif
    return 0;
}

/**/
/* Manual entry to and exit from early safe mode, through the console that
   already exists. There is deliberately no button for it: the BOOT button is
   wired to the boost module's K pin (), and inventing a GPIO would be a
   guess about hardware nobody has measured. */
static int cmd_safemode(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "on")) {
        ls_safe_force(true);
        printf("safemode: armed - the next boot will be safe mode\n"
               "          ('safemode reboot' restarts now, 'safemode off' cancels)\n");
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "off")) {
        ls_safe_force(false);
        printf("safemode: disarmed, startup fault counter cleared\n");
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "reboot")) {
        printf("safemode: restarting\n");
        fflush(stdout);
        esp_restart();
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "normal")) {
        printf("safemode: restarting into a normal boot with the guard re-armed\n");
        fflush(stdout);
        ls_safe_retry_normal_boot();
        return 0;
    }

    char buf[LS_SAFE_REPORT_MAX];
    ls_safe_report(buf, sizeof(buf));
    fputs(buf, stdout);
    printf("safemode: currently %s, forced=%d\n"
           "usage: safemode [on|off|normal|reboot]\n",
           ls_safe_active() ? "ACTIVE" : "inactive", ls_safe_forced() ? 1 : 0);
    return 0;
}

/**/
/* The recovery console. Deliberately not the full set: `scan`, `ch`, `vol`,
   `p25*` and `spec` all reach into a backend that safe mode never started, so
   offering them would turn a diagnostic session into the next panic. */
void ls_ctl_register_recovery_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "safemode",
          .help = "Safe-mode state, and manual entry/exit",
          .hint = "[on|off|normal|reboot]", .func = &cmd_safemode },
        { .command = "crash",
          .help = "Show safe dump metadata; back up before 'crash clear' erases it",
          .hint = "[clear]", .func = &cmd_crash },
        { .command = "version",
          .help = "Firmware version (git describe), board variant, build date, IDF",
          .func = &cmd_version },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        esp_console_cmd_register(&cmds[i]);
}

void ls_ctl_register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        /**/
        { .command = "vol",     .help = "Volume 0-100 (or +n / -n)",
          .hint = "<n|+n|-n>", .func = &cmd_vol },
        { .command = "mute",    .help = "Toggle audio mute",
          .func = &cmd_mute },
        { .command = "p25gate", .help = "P25 voice error gate (lower=mute weak frames)",
          .hint = "<0-99>", .func = &cmd_p25gate },
        /**/
        { .command = "p25tsbk",
          .help = "P25 control-channel state: IDEN, neighbours, unhandled opcodes",
          .func = &cmd_p25tsbk },
        { .command = "p25",
          .help = "Read-only P25 acquisition, tuning fence and IQ diagnostics",
          .hint = "acquisition", .func = &cmd_p25 },
        /**/
        { .command = "p25enc",
          .help = "P25 encryption gate: status, counters, and leave-on-encrypted",
          .hint = "[leave on|off | skip <sec> | reset]", .func = &cmd_p25enc },
        { .command = "home", .help = "Get/set home QTH for the radar",
          .hint = "<lat> <lon>", .func = &cmd_home },
        /**/
        { .command = "ch", .help = "Scanner channel list",
          .hint = "list|add|import|del|lock|pri|en|clear|zone <0-7|all>", .func = &cmd_ch },
        /**/
        { .command = "stats", .help = "NVS write counters (done/dropped/commits)",
          .func = &cmd_stats },
        /**/
        { .command = "scan", .help = "Channel scanner control",
          .hint = "on|off|status|skip|hang|thresh|pri|src <preset|band>|band <a> <b> [step]", .func = &cmd_scan },
        /**/
        { .command = "spec", .help = "Strongest bins of the FM SWEEP spectrum",
          .hint = "[count]", .func = &cmd_spec },
        /**/
        { .command = "date", .help = "Print the wall-clock time (or uptime marker if unsynced)",
          .func = &cmd_date },
        /**/
        { .command = "crash",
          .help = "Show safe dump metadata; back up before 'crash clear' erases it",
          .hint = "[clear]", .func = &cmd_crash },
        /**/
        { .command = "version",
          .help = "Firmware version (git describe), board variant, build date, IDF",
          .func = &cmd_version },
        /**/
        { .command = "safemode",
          .help = "Safe-mode state, and manual entry into early recovery",
          .hint = "[on|off|normal|reboot]", .func = &cmd_safemode },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        esp_console_cmd_register(&cmds[i]);
}

static void cli_task(void *arg)
{
    (void)arg;
    char line[160];
    int  len = 0;
    uint8_t ch;
    for (;;) {
        int n = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &ch, 1, portMAX_DELAY);
        if (n != 1) continue;
        if (ch == '\r' || ch == '\n') {
            printf("\n");
            if (len == 0) continue;
            line[len] = 0;
            len = 0;
            int ret = 0;
            esp_err_t e = esp_console_run(line, &ret);
            if (e == ESP_ERR_NOT_FOUND) printf("unknown command (try 'help')\n");
            else if (e == ESP_ERR_INVALID_ARG) { }
            else if (e != ESP_OK) printf("error: %s\n", esp_err_to_name(e));
        } else if ((ch == 0x7f || ch == 0x08) && len > 0) {
            len--;
            printf("\b \b"); fflush(stdout);
        } else if (ch >= 0x20 && len < (int)sizeof(line) - 1) {
            line[len++] = (char)ch;
            putchar((char)ch); fflush(stdout);
        }
    }
}

/**/
static void start_repl(bool full)
{
    esp_console_config_t ccfg = ESP_CONSOLE_CONFIG_DEFAULT();
    if (esp_console_init(&ccfg) != ESP_OK) {
        ESP_LOGW(TAG, "console init failed");
        return;
    }
    esp_console_register_help_command();
    if (full) ls_ctl_register_commands();
    else      ls_ctl_register_recovery_commands();

    if (!uart_is_driver_installed(CONFIG_ESP_CONSOLE_UART_NUM)) {
        uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 512, 0, 0, NULL, 0);
    }
    /**/
    static StackType_t cli_stack[4096 / sizeof(StackType_t)];
    static StaticTask_t cli_tcb;
    xTaskCreateStaticPinnedToCore(cli_task, "ls_cli", 4096 / sizeof(StackType_t),
                                  NULL, 3, cli_stack, &cli_tcb, 0);
    printf(full ? "\nlakeshark CLI ready (type 'help')\n"
                : "\nlakeshark RECOVERY CLI ready (type 'help') - radio, audio "
                  "and scanner commands are not registered in safe mode\n");
}

void ls_ctl_start_repl(void)          { start_repl(true); }
/**/
void ls_ctl_start_recovery_repl(void) { start_repl(false); }
