#include "ls_ctl.h"
#include "settings.h"
#include "location_pref.h"
#include "scan_channels.h"
#include "scan_engine.h"
#include "scan_journal.h"
#include "scan_import.h"
#include "scan_sd_store.h"
#include "lakeshark_backend.h"
/**/
#include "fm_state.h"
/**/
#include "p25_state.h"
/**/
#include "audio_out.h"
#include "audio_events.h"
#include "speech.h"
#include "esp_heap_caps.h"
/**/
#include "ls_time.h"
#include "ls_board.h"
/**/
#include "ls_crash.h"
/**/
#include "ls_search.h"
#include "rec_catch.h"

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
#include "p25_program.h"
#include "../../../main/ls_p25_packs.h"
#include "rec_state.h"
#include "cell_iq.h"
#include "cell_report.h"
#include "esp_timer.h"
#include "lr_iq_spectrum.h"
#include "lr_wf_session.h"
#include "p25_lr_wide.h"
#include "p25_lr_frontend.h"
#include "dsp_pipeline.h"
#include "esp_cpu.h"
#include "esp_private/esp_clk.h"
#include <math.h>
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
#include "../apps/dmr/dmr_watch.h"
#include "ls_textpack.h"

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
        LS_FPUTS(msg_ls_ctl_bad_coords_lat_90, "bad coords (lat -90..90, lon -180..180)\n", stdout);
        return 0;
    }
    if (settings_set_home((float)lat, (float)lon)) LS_PRINTF(t_ls_ctl_home_5f_5f_save, "home=%.5f,%.5f; save queued\n", lat, lon);
    else LS_FPUTS(msg_ls_ctl_home_not_changed_settings, "home not changed: settings unavailable or queue full\n", stdout);
    return 0;
}

static void ch_list(void)
{
    int n = scan_channels_count();
    printf("channels: %d\n", n);
    for (int i = 0; i < n; i++) {
        const scan_channel_t *c = scan_channel_get(i);
        if (!c) continue;
        LS_PRINTF(t_ls_ctl_2d_12s_9_4f, "  %2d  %-12s %9.4f MHz  %-3s z%d%s%s%s\n",
               i, c->name, c->freq_hz / 1e6, scan_mode_name(c->mode), c->zone,
               (c->flags & SCAN_FLAG_ENABLED)  ? "" : " off",
               (c->flags & SCAN_FLAG_LOCKOUT)  ? " lock" : "",
               (c->flags & SCAN_FLAG_PRIORITY) ? " pri" : "");
    }
}

static int cmd_ch(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "caps")) {
        LS_PRINTF(t_ls_ctl_chcap_max_d_sd, "CHCAP max=%d sd=%d flash=%d\n", SCAN_MAX_CHANNELS, scan_sd_available(), SCAN_FLASH_CHANNELS); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "profiles")) { scan_sd_list(); return 0; }
    if (argc == 3 && !strcmp(argv[1], "save")) {
        bool ok = scan_channels_save_profile(argv[2]);
        printf("CHSAVE %s %s\n", ok ? "OK" : "FAILED", argv[2]); return ok ? 0 : 1;
    }
    if (argc == 3 && !strcmp(argv[1], "profile")) {
        scan_engine_stop();
        bool ok = scan_channels_load_profile(argv[2]);
        LS_PRINTF(t_ls_ctl_chload_s_s_scan, "CHLOAD %s %s; scan stopped\n", ok ? "OK" : "FAILED", argv[2]); return ok ? 0 : 1;
    }

    if (argc == 2 && !strcmp(argv[1], "stage")) { scan_import_begin(); puts("CHSTAGE READY"); return 0; }
    if (argc == 2 && !strcmp(argv[1], "abort")) { scan_import_abort(); puts("CHSTAGE ABORTED"); return 0; }
    if (argc == 3 && !strcmp(argv[1], "row")) {
        bool ok = scan_import_add(argv[2]); puts(ok ? "CHROW OK" : "CHROW REJECTED"); return ok ? 0 : 1;
    }
    if (argc == 2 && !strcmp(argv[1], "commit")) {
        char message[96]; bool ok = scan_import_commit(message, sizeof(message));
        printf("CHCOMMIT %s %s\n", ok ? "OK" : "FAILED", message); return ok ? 0 : 1;
    }
    if (argc == 2 && !strcmp(argv[1], "dump")) {
        puts("LSCAN1");
        for (int i = 0; i < scan_channels_count(); ++i) {
            const scan_channel_t *c = scan_channel_get(i);
            if (c) LS_PRINTF(t_ls_ctl_s_lu_s_u, "%s|%lu|%s|%u|%.7f|%.7f|%.3f|%d\n", c->name,
                (unsigned long)c->freq_hz, scan_mode_name(c->mode), c->zone,
                c->lat_e7 * 1e-7, c->lon_e7 * 1e-7, c->radius_m * .001, !!(c->flags & SCAN_FLAG_PRIORITY));
        }
        puts("CHDUMP END"); return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "load")) {
        char message[96] = "Import worker unavailable";
        bool ok = scan_import_file(argv[2], message, sizeof(message));
        printf("%s: %s\n", ok ? "OK" : "ERROR", message);
        return ok ? 0 : 1;
    }
    if (argc < 2 || !strcmp(argv[1], "list")) { ch_list(); return 0; }

    if (!strcmp(argv[1], "add")) {
        if (argc < 4) { LS_FPUTS(msg_ls_ctl_usage_ch_add_mhz, "usage: ch add <MHz> <p25|nfm|wfm> [name] [zone]\n", stdout); return 0; }
        uint32_t hz = (uint32_t)(atof(argv[2]) * 1e6 + 0.5);
        int mode = scan_mode_parse(argv[3]);
        if (mode < 0) { printf("bad mode (p25|nfm|wfm)\n"); return 0; }
        const char *name = (argc > 4) ? argv[4] : NULL;
        int zone = (argc > 5) ? atoi(argv[5]) : 0;
        if (zone < 0 || zone >= SCAN_MAX_ZONES) { printf("bad zone (0-%d)\n", SCAN_MAX_ZONES - 1); return 0; }
        /**/
        int dup = scan_channel_find_freq_zone(hz, (uint8_t)zone);
        if (dup >= 0) { LS_PRINTF(t_ls_ctl_already_ch_d_in, "already ch %d in zone %d - not added\n", dup, zone); return 0; }
        int idx = scan_channel_add(name, hz, (scan_mode_t)mode, (uint8_t)zone);
        if (idx < 0) LS_FPUTS(msg_ls_ctl_add_failed_list_full, "add failed (list full or invalid freq)\n", stdout);
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
            LS_FPUTS(msg_ls_ctl_usage_ch_import_p25, "usage: ch import <p25|nfm|wfm> <zone> <MHz>[,<MHz>...] [name-prefix]\n"
                   "       ch import nfm 1 154.400,154.010,162.475 SED\n", stdout);
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
        LS_PRINTF(fmt_ls_ctl_import_added_already_present, "import: %d added, %d already present, %d rejected, zone %d %s - %s\n",
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
        if (argc < 4) { LS_PRINTF(t_ls_ctl_usage_ch_s_idx, "usage: ch %s <idx> on|off\n", argv[1]); return 0; }
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
        if (z < 0) LS_FPUTS(msg_ls_ctl_zone_all_every_channel, "zone=all - every channel is in the sweep\n", stdout);
        else       LS_PRINTF(fmt_ls_ctl_zone_only_channels_in, "zone=%d - only channels in zone %d are scanned\n", z, z);
        return 0;
    }

    LS_FPUTS(msg_ls_ctl_usage_ch_list_add, "usage: ch [list|add|import|del|lock|pri|en|clear|zone <0-7|all>]\n", stdout);
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
    LS_PRINTF(fmt_ls_ctl_settings_nvs_done_dropped, "settings nvs: done=%lu dropped=%lu commits=%lu\n",
           (unsigned long)done, (unsigned long)dropped, (unsigned long)commits);
    if (dropped) LS_PRINTF(fmt_ls_ctl_write_s_dropped_a, "  *** %lu write(s) DROPPED - a setting changed then did not stick\n",
                        (unsigned long)dropped);
    LS_PRINTF(t_ls_ctl_channels_d_stored_batch, "channels: %d stored, batch %s\n",
           scan_channels_count(), scan_channels_batching() ? "OPEN (unsaved)" : "closed");
    return 0;
}

static int cmd_scan(int argc, char **argv)
{
    if (argc == 3 && (!strcmp(argv[1], "mixed") || !strcmp(argv[1], "gps"))) {
        if (strcmp(argv[2], "on") && strcmp(argv[2], "off")) return 1;
        bool on = !strcmp(argv[2], "on");
        if (!strcmp(argv[1], "mixed")) scan_engine_set_mixed(on);
        else scan_engine_set_location(on);
        LS_PRINTF(t_ls_ctl_s_s_scan_stopped, "%s %s; scan stopped\n", argv[1], argv[2]); return 0;
    }
    if (argc < 2 || !strcmp(argv[1], "status")) {
        char st[96]; scan_engine_status(st, sizeof(st));
        LS_PRINTF(t_ls_ctl_scan_s_mixed_s, "scan: %s / mixed=%s GPS=%s\n", st, scan_engine_mixed() ? "on" : "off",
               !scan_engine_location() ? "off" : scan_engine_location_ready() ? "ready" : "waiting");
        scan_journal_status_t log;
        scan_journal_get_status(&log);
        LS_PRINTF(fmt_ls_ctl_scan_log_records_dropped, "scan log: %s records=%lu dropped=%lu errors=%lu bytes=%llu path=%s\n",
               log.detail, (unsigned long)log.records_written,
               (unsigned long)log.queue_dropped, (unsigned long)log.write_errors,
               (unsigned long long)log.bytes_written, log.path[0] ? log.path : "none");
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
        LS_PRINTF(t_ls_ctl_thresh_d_of_full, "thresh=%d %% of full scale\n", scan_engine_get_threshold_pct());
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
        if (now) LS_PRINTF(fmt_ls_ctl_pri_ms_holds_are, "pri=%d ms - holds are interrupted to sample priority channels\n", now);
        else     printf("pri=off\n");
        return 0;
    }
    /**/
    if (!strcmp(argv[1], "asql")) {
        int margin = (argc > 2) ? atoi(argv[2]) : -1;
        scan_engine_autosquelch(margin);
        LS_PRINTF(fmt_ls_ctl_autosql_requested_measuring_the, "autosql requested%s - measuring the floor, watch `scan status`\n",
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
            LS_PRINTF(t_ls_ctl_band_4f_4f_mhz, "band %.4f-%.4f MHz step %.1f kHz - %d steps\n",
                   a / 1e6, b / 1e6, st / 1000.0,
                   scan_engine_band_steps());
            LS_FPUTS(msg_ls_ctl_usage_scan_band_startmhz, "usage: scan band <startMHz> <stopMHz> [stepkHz]\n", stdout);
            return 0;
        }
        a  = (uint32_t)(atof(argv[2]) * 1e6 + 0.5);
        b  = (uint32_t)(atof(argv[3]) * 1e6 + 0.5);
        st = (argc > 4) ? (uint32_t)(atof(argv[4]) * 1000 + 0.5) : 0;
        if (!scan_engine_set_band(a, b, st)) {
            LS_FPUTS(msg_ls_ctl_bad_band_need_start, "bad band (need start<stop, >=1 MHz, step >=0.1 kHz)\n", stdout);
            return 0;
        }
        scan_engine_get_band(&a, &b, &st);
        /* Selecting a band is the intent to scan it - switch the source too,
           so this is one command and not a two-step people forget. */
        scan_engine_set_source(SCAN_SRC_BAND);
        LS_PRINTF(fmt_ls_ctl_band_mhz_step_khz, "band %.4f-%.4f MHz step %.1f kHz - %d steps, src=band\n",
               a / 1e6, b / 1e6, st / 1000.0,
               scan_engine_band_steps());
        return 0;
    }
    LS_FPUTS(msg_ls_ctl_usage_scan_on_off, "usage: scan [on|off|status|skip|hang <ms>|thresh <1-100>|pri <ms|off>\n"
           "            |src <preset|band>|band <startMHz> <stopMHz> [stepkHz]\n"
           "            |asql [margin]]\n", stdout);
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
    static EXT_RAM_BSS_ATTR char buf[2048];
    size_t n = p25_tsbk_status_format(&s_dsd_state, buf, sizeof(buf));
    if (n > 0) fputs(buf, stdout);
    if (n >= sizeof(buf)) LS_PRINTF(t_ls_ctl_truncated_zu_bytes, "[truncated - %zu bytes]\n", n);
    return 0;
}

/* apps/fm/fm_bcast_cmd.c */
int fm_bcast_command(int argc, char **argv);

static int cmd_fmlr(int argc, char **argv)
{
    (void)argc; (void)argv;
    LS_PRINTF(fmt_ls_ctl_fmlr_active_hz_ch, "FMLR active=%d hz=%lu ch=%lu sql=%d lost=%lu gaps=%lu drops=%lu\n", FM.lr_iq_active,
           (unsigned long)FM.freq_hz, (unsigned long)FM.lr_channel_hz, FM.squelch_open,
           (unsigned long)FM.lr_lost_pairs, (unsigned long)FM.lr_gaps, (unsigned long)FM.lr_audio_drops);
    return 0;
}

static int cmd_wf(int argc, char **argv)
{
    (void)argc; (void)argv;
    bool on; uint32_t hz, lines, gaps, skipped;
    lr_spec_stats(&on, &hz, &lines, &gaps, &skipped);
    LS_PRINTF(fmt_ls_ctl_lrwf_on_center_lines, "LRWF on=%d center=%lu lines=%lu gaps=%lu skipped=%lu\n", on, (unsigned long)hz,
           (unsigned long)lines, (unsigned long)gaps, (unsigned long)skipped);
    return 0;
}

/* `dmr` - what the DMR watcher has seen on the C4FM discriminator output
 * the P25 receiver is already producing. Read-only; it steers nothing. */
static int cmd_dmr(int argc, char **argv)
{
    (void)argc; (void)argv;
    static EXT_RAM_BSS_ATTR dmr_watch_t w;
    if (!dmr_watch_get(&w)) {
        LS_FPUTS(msg_ls_ctl_dmr_nothing_fed_yet, "dmr: nothing fed yet - the watcher runs while the P25 receiver is in C4FM\n", stdout);
        return 0;
    }
    /* The report and its verdict live with the decoder (dmr_report.c), so
       the bench can hold them to what they say. */
    static EXT_RAM_BSS_ATTR char buf[1536];
    size_t n = dmr_watch_format(&w, esp_timer_get_time(), buf, sizeof(buf));
    fputs(buf, stdout);
    if (n >= sizeof(buf)) LS_PRINTF(t_ls_ctl_truncated_zu_bytes, "[truncated - %zu bytes]\n", n);
    return 0;
}

/* `radios` - which radios come up at boot. BLE scans continuously once
 * started, so a board with no control head attached pays for it all day.
 * The preference is applied at boot rather than by stopping the radio
 * afterwards, which would still have powered it up. */
static int cmd_radios(int argc, char **argv)
{
    if (argc >= 3) {
        const bool on = !strcmp(argv[2], "on") || !strcmp(argv[2], "1");
        bool now;
        if (!strcmp(argv[1], "ble")) {
            settings_set_ble_at_boot(on);
            now = settings_get_ble_at_boot();
        } else if (!strcmp(argv[1], "wifi")) {
            settings_set_wifi_at_boot(on);
            now = settings_get_wifi_at_boot();
        } else { printf("usage: radios [ble|wifi] [on|off]\n"); return 0; }
        /* The setters only move the value once the write is queued, so the
           read-back is what was saved. This used to echo the request, and
           a write refused before settings were up read as a success. */
        if (now != on)
            LS_PRINTF(fmt_ls_ctl_at_boot_not_saved, "%s at boot: NOT saved (settings not ready?) - still %s\n",
                   argv[1], now ? "on" : "off");
        else
            LS_PRINTF(fmt_ls_ctl_at_boot_takes_effect, "%s at boot: %s - takes effect on the next reboot\n",
                   argv[1], on ? "on" : "off");
        return 0;
    }
    LS_PRINTF(t_ls_ctl_at_boot_ble_s, "at boot:  ble=%s  wifi=%s\n",
           settings_get_ble_at_boot() ? "on" : "off",
           settings_get_wifi_at_boot() ? "on" : "off");
    printf("usage: radios [ble|wifi] [on|off]\n");
    return 0;
}

static int cmd_p25gate(int argc, char **argv)
{
    if (argc < 2) {
        LS_PRINTF(fmt_ls_ctl_p25_voice_gate_lower, "p25 voice gate = %d (lower mutes more error frames; 99 = play all)\n",
               lakeshark_p25_voice_gate());
        return 0;
    }
    lakeshark_p25_set_voice_gate(atoi(argv[1]));
    LS_PRINTF(t_ls_ctl_p25_voice_gate_d, "p25 voice gate = %d\n", lakeshark_p25_voice_gate());
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
    if (n >= sizeof(output)) LS_PUTS(t_ls_ctl_acquisition_snapshot_truncated, "[acquisition snapshot truncated]");
    LS_PRINTF(fmt_ls_ctl_signal_syncs_uncorroborated_last, "signal: syncs=%d uncorroborated=%u last_nac=%03X "
           "(NID passed BCH, not counted)\n",
           P25.dsd_sync_count, (unsigned)P25.sync_unconfirmed_total,
           (unsigned)P25.sync_unconfirmed_nac);
    return 0;
}

/* `p25 capture save [name]` puts a finished capture on the card beside REC's
   captures, to be taken off the card (or from the Wi-Fi file page, on the
   headless boards that run it - boards with a screen do not). 'p25 capture
   read' moves 256 bytes a command, and a full 4 MiB capture took 35 minutes
   over the console on 2026-09-23. Writing 4 MiB to the card takes 30-90 s.
   The .txt beside it carries what bench/tools/p25_voice_replay needs to
   decode it: rate and demod gain. */
static EXT_RAM_BSS_ATTR uint8_t s_capture_stage[16 * P25_IQ_CAPTURE_READ_MAX];

static int p25_capture_save(const char *name)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000LL);
    p25_iq_capture_status_t st;
    if (!p25_iq_capture_status(&st, now) || !st.captured_bytes ||
        (st.phase != P25_IQ_CAPTURE_DONE && st.phase != P25_IQ_CAPTURE_ABORTED)) {
        LS_PUTS(msg_ls_ctl_p25iq_save_no_finished, "P25IQ save: no finished capture - 'p25 capture start' first");
        return 1;
    }
    char base[48];
    if (name) {
        size_t n = strlen(name);
        if (!n || n >= sizeof(base)) { LS_PUTS(t_ls_ctl_p25iq_save_name_too, "P25IQ save: name too long"); return 1; }
        for (size_t i = 0; i < n; i++) {
            char c = name[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_')) {
                LS_PUTS(msg_ls_ctl_p25iq_save_name_is, "P25IQ save: name is letters, digits, - and _ only");
                return 1;
            }
        }
        snprintf(base, sizeof(base), "%s", name);
    } else {
        snprintf(base, sizeof(base), "p25iq_%lu", (unsigned long)st.session);
    }
    const char *dir = rec_dir();
    if (!dir) { puts("P25IQ save: no card"); return 1; }
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.iq", dir, base);
    FILE *f = fopen(path, "wb");
    if (!f) { LS_PRINTF(t_ls_ctl_p25iq_save_cannot_create, "P25IQ save: cannot create %s\n", path); return 1; }
    uint32_t off = 0;
    int rc = 0;
    while (off < st.captured_bytes && !rc) {
        size_t fill = 0;
        while (fill < sizeof(s_capture_stage) && off < st.captured_bytes) {
            size_t got = 0;
            p25_iq_capture_result_t r = p25_iq_capture_read(off, s_capture_stage + fill,
                P25_IQ_CAPTURE_READ_MAX, &got, now);
            if (r != P25_IQ_CAPTURE_OK || !got) {
                LS_PRINTF(t_ls_ctl_p25iq_save_read_at, "P25IQ save: read at %lu: %s\n", (unsigned long)off,
                       p25_iq_capture_result_name(r));
                rc = 1;
                break;
            }
            fill += got;
            off += (uint32_t)got;
        }
        if (fill && fwrite(s_capture_stage, 1, fill, f) != fill) {
            LS_PRINTF(t_ls_ctl_p25iq_save_card_write, "P25IQ save: card write failed at %lu\n", (unsigned long)off);
            rc = 1;
        }
    }
    if (fclose(f) != 0) rc = 1;
    if (rc) { remove(path); return 1; }

    snprintf(path, sizeof(path), "%s/%s.txt", dir, base);
    FILE *m = fopen(path, "w");
    if (m) {
        uint32_t gain_bits;
        memcpy(&gain_bits, &st.receiver.demod_gain, sizeof(gain_bits));
        LS_FPRINTF(fmt_ls_ctl_format_u8_iq_bytes, m, "format=u8-iq\nbytes=%lu\nfrequency=%lu\nrate=%lu\nbandwidth=%lu\n"
                   "gain_tenths=%ld\ndemod_mode=%ld\ndemod_gain=%g\ndemod_gain_bits=%08lx\n"
                   "phase=%s\nreason=%s\nfirst_ms=%lu\nlast_ms=%lu\nfirmware=%s\n",
                (unsigned long)st.captured_bytes, (unsigned long)st.receiver.frequency_hz,
                (unsigned long)st.receiver.sample_rate_hz, (unsigned long)st.receiver.bandwidth_hz,
                (long)st.receiver.gain_tenths, (long)st.receiver.demod_mode,
                (double)st.receiver.demod_gain, (unsigned long)gain_bits,
                p25_iq_capture_phase_name(st.phase), p25_iq_capture_result_name(st.reason),
                (unsigned long)st.first_ms, (unsigned long)st.last_ms,
                esp_app_get_description()->version);
        fclose(m);
    }
    LS_PRINTF(t_ls_ctl_p25iq_saved_s_iq, "P25IQ saved=%s.iq bytes=%lu in %s\n",
           base, (unsigned long)st.captured_bytes, dir);
    return 0;
}

/* `p25 profile` lists the card's p25_profile*.txt and the last load;
   `p25 profile <name>` loads one. The same reload the P25 screen's picker
   asks for, so a board with no screen can change system too. */
static int p25_profile_command(int argc, char **argv)
{
    char text[160];
    const p25_program_t *ps = p25_program_session();
    const char *active = p25_program_active_name();

    if (argc == 0) {
        char name[P25_PROGRAM_PATH_MAX], system[40];
        int total = p25_program_card_profile(-1, NULL, 0, NULL, 0);
        for (int i = 0; i < total; i++) {
            p25_program_card_profile(i, name, sizeof(name), system, sizeof(system));
            printf("  %-32s %s%s\n", name, system,
                   active && !strcmp(active, name) ? "  (loaded)" : "");
        }
        if (!total) LS_PUTS(t_ls_ctl_no_p25_profile_txt, "  no p25_profile*.txt in the SD root");
        p25_program_format_status(ps, text, sizeof(text));
        printf("P25PROFILE %s\n", text);
        p25_program_format_geo_now(text, sizeof(text));
        printf("P25PROFILE %s\n", text);
        return 0;
    }

    if (!p25_program_is_profile_name(argv[0])) {
        LS_PUTS(msg_ls_ctl_p25profile_refused_a_p25, "P25PROFILE refused: a p25_profile*.txt name in the SD root");
        return 1;
    }
    if (!p25_program_request_card_profile(argv[0])) {
        LS_PUTS(msg_ls_ctl_p25profile_refused_a_load, "P25PROFILE refused: a load is already running");
        return 1;
    }
    LS_PRINTF(fmt_ls_ctl_p25profile_loading_p25_profile, "P25PROFILE loading %s - 'p25 profile' shows the result\n", argv[0]);
    return 0;
}

/* `p25 lr bench`: what P25's LR chain costs per second of air, with its
   state and buffers in internal RAM or in PSRAM: the LR front end
   (125k IQ16 -> 240k float) and the C4FM demodulator behind it, on a
   synthetic 0.5 s of signal. Run with P25 stopped: it shares the core. */
static void p25_lr_bench_one(const char *where, uint32_t caps)
{
    enum { BLOCKS = 200, PAIRS = 320 };
    p25_lr_frontend_t *fe = heap_caps_malloc(sizeof(*fe), caps);
    dsp_state_t *dsp = heap_caps_malloc(sizeof(*dsp), caps);
    int16_t *in = heap_caps_malloc(PAIRS * 4, caps);
    float *iq = heap_caps_malloc(640 * 2 * sizeof(float), caps);
    int16_t *audio = heap_caps_malloc(1024 * sizeof(int16_t), caps);
    if (!fe || !dsp || !in || !iq || !audio) {
        printf("P25LRBENCH %s: no memory\n", where);
    } else {
        p25_lr_frontend_init(fe, 0, false);
        dsp_init(dsp); dsp_set_mode(dsp, DEMOD_C4FM);
        uint32_t fe_cycles = 0, dsp_cycles = 0, ph = 0;
        for (int b = 0; b < BLOCKS; ++b) {
            /* A deviating carrier: the phase steps by a varying amount. */
            for (int k = 0; k < PAIRS; ++k) {
                ph += 0x01000000u + (uint32_t)((b * PAIRS + k) % 26) * 0x00100000u;
                const float a = (float)ph * (6.2831853f / 4294967296.0f);
                in[2 * k] = (int16_t)(12000.0f * cosf(a));
                in[2 * k + 1] = (int16_t)(12000.0f * sinf(a));
            }
            const uint32_t c0 = esp_cpu_get_cycle_count();
            const size_t pairs = p25_lr_frontend_process(fe, (const uint8_t *)in, PAIRS * 4, iq, 640);
            const uint32_t c1 = esp_cpu_get_cycle_count();
            if (pairs != SIZE_MAX) dsp_process_float_iq(dsp, iq, (int)pairs, audio, 1024);
            const uint32_t c2 = esp_cpu_get_cycle_count();
            fe_cycles += c1 - c0; dsp_cycles += c2 - c1;
        }
        /* BLOCKS * PAIRS pairs at 125k is the air time measured. */
        const double air_cycles = (double)BLOCKS * PAIRS / 125000.0 * esp_clk_cpu_freq();
        printf("P25LRBENCH %-8s front_end=%.1f%% demod=%.1f%% of one core (%lu + %lu cycles a pair)\n",
               where, 100.0 * fe_cycles / air_cycles, 100.0 * dsp_cycles / air_cycles,
               (unsigned long)(fe_cycles / (BLOCKS * PAIRS)), (unsigned long)(dsp_cycles / (BLOCKS * PAIRS)));
    }
    heap_caps_free(fe); heap_caps_free(dsp); heap_caps_free(in);
    heap_caps_free(iq); heap_caps_free(audio);
}

/* The optional trim after the gain, in Hz and signed; false when it is out of range. */
/* IQ status and bounded IQ recordings replace the retired P25 FSK tools. */
static int p25_lr_command(int argc, char **argv)
{
    if (argc >= 1 && !strcmp(argv[0], "record"))
        return p25_lr_iq_record_command(argc - 1, argv + 1);
    if (argc >= 1 && !strcmp(argv[0], "bench")) {
        if (P25.lr_iq_active) { puts("P25LRBENCH: stop P25 first; it shares the core"); return 1; }
        p25_lr_bench_one("internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        p25_lr_bench_one("psram", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return 0;
    }
    if (argc && (strcmp(argv[0], "mode") || argc > 2 ||
                 (argc == 2 && strcmp(argv[1], "iq")))) {
        LS_PUTS(msg_ls_ctl_p25_on_lr2021_uses, "P25 on LR2021 uses IQ only; usage: p25 lr [mode [iq] | record ...]");
        return 1;
    }
    if (argc == 2 && !p25_lr_iq_select(true)) {
        LS_PUTS(t_ls_ctl_p25_iq_requires_lr2021, "P25 IQ requires LR2021"); return 1;
    }
    LS_PRINTF(fmt_ls_ctl_p25lriq_selected_iq_active, "P25LRIQ selected=iq active=%u blocks=%lu rejected=%lu gaps=%lu fifo_loss=%lu rate=%lu; select LR2021 in RADIO\n",
           P25.lr_iq_active, (unsigned long)P25.lr_iq_good,
           (unsigned long)P25.lr_iq_bad, (unsigned long)P25.lr_iq_gaps,
           (unsigned long)P25.lr_iq_fifo_overruns, (unsigned long)P25.lr_iq_sample_rate);
    /* The voice handed to the speaker since the last look, and two
       entries of mbelib's Golay table, built at start-up (72 and 80). */
    extern int golayMatrix[2048];
    printf("P25VOICE peak_max=%u golay[15]=%d golay[2047]=%d\n",
           (unsigned)P25.voice_peak_max, golayMatrix[15], golayMatrix[2047]);
    P25.voice_peak_max = 0;
    printf("P25LRCPU front_end=%lu.%lu%% demod=%lu.%lu%% of one core\n",
           (unsigned long)(P25.lr_fe_permille / 10), (unsigned long)(P25.lr_fe_permille % 10),
           (unsigned long)(P25.lr_dsp_permille / 10), (unsigned long)(P25.lr_dsp_permille % 10));
    return 0;
}

/* `p25 lrwide [on|off|follow]`: P25 on the LR2021's wide route. off (the
   boot state) pins P25's narrow capture; on pins wide for P25's own
   session; follow takes the shared WIDE I/Q setting. Not saved. Prints each path's
   hops (start or grant to the first block of the new channel). */
static int p25_lrwide_command(int argc, char **argv)
{
    if (argc >= 1) {
        if (!strcmp(argv[0], "on")) p25_lr_wide_set_pref(P25_LR_WIDE_ON);
        else if (!strcmp(argv[0], "off")) p25_lr_wide_set_pref(P25_LR_WIDE_OFF);
        else if (!strcmp(argv[0], "follow")) p25_lr_wide_set_pref(P25_LR_WIDE_FOLLOW);
        else if (strcmp(argv[0], "status")) {
            puts("usage: p25 lrwide [on|off|follow|status]"); return 1;
        }
    }
    const p25_lr_wide_pref_t pref = p25_lr_wide_pref();
    const bool shared = lr_wf_wide_on();
    printf("P25LRWIDE pref=%s shared=%s -> %s | running=%s\n", p25_lr_wide_pref_name(pref),
           shared ? "on" : "off", p25_lr_wide_pick(pref, shared) ? "wide" : "narrow",
           !P25.lr_iq_active ? "none" : P25.lr_iq_wide ? "wide" : "narrow");
    static const char *const path[2] = {"narrow", "wide"};
    for (int i = 0; i < 2; ++i)
        printf("P25LRWIDE %-6s hops=%lu last=%lu.%lu ms worst=%lu.%lu ms\n", path[i],
               (unsigned long)P25.lr_iq_hops[i],
               (unsigned long)(P25.lr_iq_hop_us[i] / 1000), (unsigned long)(P25.lr_iq_hop_us[i] / 100 % 10),
               (unsigned long)(P25.lr_iq_hop_max_us[i] / 1000), (unsigned long)(P25.lr_iq_hop_max_us[i] / 100 % 10));
    return 0;
}

/* `p25 sym ...`: the decoder's symbols, whichever radio feeds it. A dump
   line holds 256 symbols, four to a byte, with a Fletcher-16 as "c". */
static int p25_sym_command(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[0], "cap")) {
        if (!strcmp(argv[1], "start")) p25_sym_capture_start();
        else if (!strcmp(argv[1], "stop")) p25_sym_capture_stop();
    } else if (argc >= 1 && !strcmp(argv[0], "dump")) {
        p25_sym_capture_info_t ci;
        p25_sym_capture_info(&ci);
        uint32_t from = argc >= 2 ? (uint32_t)strtoul(argv[1], NULL, 10) & ~3u : ci.oldest;
        if (from < ci.oldest) from = ci.oldest;
        if (from > ci.symbols) from = ci.symbols & ~3u;
        uint32_t count = argc >= 3 ? (uint32_t)strtoul(argv[2], NULL, 10) : ci.symbols - from;
        if (count > ci.symbols - from) count = ci.symbols - from;
        LS_PRINTF(t_ls_ctl_symcap_from_lu_count, "# symcap from=%lu count=%lu\n", (unsigned long)from, (unsigned long)count);
        static const char hex[] = "0123456789ABCDEF";
        uint8_t d[64];
        char line[48 + 2 * sizeof d + 16];
        for (uint32_t at = from; at < from + count; at += 256) {
            const uint32_t n = from + count - at < 256 ? from + count - at : 256;
            if (!p25_sym_capture_read(at, d, n)) { puts("SYMLOST"); break; }
            int k = LS_SNPRINTF(t_ls_ctl_symdata_off_lu_n, line, sizeof line, "SYMDATA off %lu n %lu data ", (unsigned long)at, (unsigned long)n);
            unsigned s1 = 0, s2 = 0;
            for (uint32_t j = 0; j < (n + 3u) / 4u; j++) {
                line[k++] = hex[d[j] >> 4];
                line[k++] = hex[d[j] & 15];
                s1 = (s1 + d[j]) % 255u;
                s2 = (s2 + s1) % 255u;
            }
            snprintf(line + k, sizeof line - (size_t)k, " c %04X\n", (s2 << 8) | s1);
            fputs(line, stdout);
            if (((at - from) & 0xFFFF) == 0) vTaskDelay(1);
        }
        puts("SYMEND");
        return 0;
    }
    p25_sym_capture_info_t ci;
    p25_sym_capture_info(&ci);
    LS_PRINTF(t_ls_ctl_p25sym_capture_s_symbols, "P25SYM capture %s symbols=%lu oldest=%lu\n", ci.on ? "on" : "off",
           (unsigned long)ci.symbols, (unsigned long)ci.oldest);
    if (argc == 0) LS_PUTS(msg_ls_ctl_usage_p25_sym_cap, "usage: p25 sym [cap start|stop | dump [from] [count]]");
    return 0;
}

/* `p25 imbe ...`: the IMBE frames the decoder settled. A dump line holds 4
   records (p25_state.h), with a Fletcher-16 as "c". */
static int p25_imbe_command(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[0], "cap")) {
        if (!strcmp(argv[1], "start")) p25_imbe_capture_start();
        else if (!strcmp(argv[1], "stop")) p25_imbe_capture_stop();
    } else if (argc >= 1 && !strcmp(argv[0], "dump")) {
        p25_imbe_capture_info_t ci;
        p25_imbe_capture_info(&ci);
        uint32_t from = argc >= 2 ? (uint32_t)strtoul(argv[1], NULL, 10) : ci.oldest;
        if (from < ci.oldest) from = ci.oldest;
        if (from > ci.frames) from = ci.frames;
        uint32_t count = argc >= 3 ? (uint32_t)strtoul(argv[2], NULL, 10) : ci.frames - from;
        if (count > ci.frames - from) count = ci.frames - from;
        LS_PRINTF(t_ls_ctl_imbecap_from_lu_count, "# imbecap from=%lu count=%lu\n", (unsigned long)from, (unsigned long)count);
        static const char hex[] = "0123456789ABCDEF";
        uint8_t d[4 * P25_IMBE_REC];
        char line[48 + 2 * sizeof d + 16];
        for (uint32_t at = from; at < from + count; at += 4) {
            const uint32_t n = from + count - at < 4 ? from + count - at : 4;
            if (!p25_imbe_capture_read(at, d, n)) { puts("IMBELOST"); break; }
            int k = LS_SNPRINTF(t_ls_ctl_imbedata_off_lu_n, line, sizeof line, "IMBEDATA off %lu n %lu data ", (unsigned long)at, (unsigned long)n);
            unsigned s1 = 0, s2 = 0;
            for (uint32_t j = 0; j < n * P25_IMBE_REC; j++) {
                line[k++] = hex[d[j] >> 4];
                line[k++] = hex[d[j] & 15];
                s1 = (s1 + d[j]) % 255u;
                s2 = (s2 + s1) % 255u;
            }
            snprintf(line + k, sizeof line - (size_t)k, " c %04X\n", (s2 << 8) | s1);
            fputs(line, stdout);
            if (((at - from) & 0x3FF) == 0) vTaskDelay(1);
        }
        puts("IMBEEND");
        return 0;
    }
    p25_imbe_capture_info_t ci;
    p25_imbe_capture_info(&ci);
    LS_PRINTF(t_ls_ctl_p25imbe_capture_s_frames, "P25IMBE capture %s frames=%lu oldest=%lu\n", ci.on ? "on" : "off",
           (unsigned long)ci.frames, (unsigned long)ci.oldest);
    if (argc == 0) LS_PUTS(msg_ls_ctl_usage_p25_imbe_cap, "usage: p25 imbe [cap start|stop | dump [from] [count]]");
    return 0;
}

static int cmd_p25(int argc, char **argv)
{
#if CONFIG_LAKESHARK_HEADLESS
    if (argc >= 2 && (!strcmp(argv[1], "pack") || !strcmp(argv[1], "geo")))
        return ls_p25_packs_command(argc - 1, argv + 1);
#endif
    if (argc >= 2 && !strcmp(argv[1], "split")) {
        if (argc != 3 || (strcmp(argv[2], "on") && strcmp(argv[2], "off") &&
                          strcmp(argv[2], "stats"))) {
            LS_PUTS(t_ls_ctl_usage_p25_split_on, "usage: p25 split on|off|stats");
            return 1;
        }
        if (strcmp(argv[2], "stats")) {
            p25_set_split(!strcmp(argv[2], "on"));
            LS_PRINTF(fmt_ls_ctl_p25_split_requested_p25, "P25 split requested %s; p25 split stats shows owner state\n", argv[2]);
            return 0;
        }
        LS_PRINTF(fmt_ls_ctl_p25_split_cc_site, "P25 split=%s CC=%s site=%u/%u CRC=%u%% fallback=%u recovery=%u skips=%u drops=%u VOICE=RTL tg=%u hz=%llu reason=%s\n",
            P25.split_enabled ? "on" : "off", P25.split_active ? "LR2021" : "RTL fallback",
            P25.split_rfss, P25.split_site, P25.split_crc_percent,
            P25.split_fallbacks, P25.split_recoveries, P25.split_skipped, P25.split_drops,
            P25.grant_on_traffic ? P25.grant_talkgroup : 0,
            (unsigned long long)(P25.grant_on_traffic ? P25.grant_freq_hz : 0),
            P25.split_reason);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "lr"))
        return p25_lr_command(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "lrwide"))
        return p25_lrwide_command(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "sym"))
        return p25_sym_command(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "imbe"))
        return p25_imbe_command(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "profile"))
        return p25_profile_command(argc - 2, argv + 2);
    if (argc >= 3 && !strcmp(argv[1], "capture") && !strcmp(argv[2], "save"))
        return p25_capture_save(argc >= 4 ? argv[3] : NULL);
    if (argc >= 3 && !strcmp(argv[1], "capture"))
        return p25_iq_capture_command(argc - 2, argv + 2,
            (uint32_t)(esp_timer_get_time() / 1000LL));
    if (argc != 2 || strcmp(argv[1], "acquisition")) {
        LS_PUTS(msg_ls_ctl_usage_p25_acquisition_profile, "usage: p25 acquisition | profile [name] | capture start [blocks] | status | cancel | read <offset> [bytes] | save [name] | free");
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
    LS_PRINTF(fmt_ls_ctl_p25enc_leave_skip_muted, "p25enc leave=%s skip=%us muted-frames=%u returns=%u skips=%u evictions=%u\n",
           p25_get_leave_on_encrypted() ? "on" : "off",
           p25_get_encrypted_skip_ms() / 1000u,
           (unsigned)P25.p25_enc_muted_frames_total,
           (unsigned)P25.p25_enc_returns,
           (unsigned)P25.p25_enc_skips,
           (unsigned)P25.p25_enc_tg_evictions);
    LS_PRINTF(fmt_ls_ctl_of_the_muted_with, "  of the muted: %u with ESS still unknown; LDU2 ESS failed RS %u times, "
           "%u of them kept (call already clear), the rest dropped 180 ms each\n",
           (unsigned)P25.p25_enc_muted_unknown_total,
           (unsigned)P25.p25_ess_rs_failed_total,
           (unsigned)P25.p25_ess_rs_kept_total);
    LS_PRINTF(fmt_ls_ctl_unproven_voice_frames_held, "  unproven voice: %u frames held until the call proved itself, "
           "%u released clear, %u discarded (encrypted, ended or overflow)\n",
           (unsigned)P25.p25_voice_held_total,
           (unsigned)P25.p25_voice_released_total,
           (unsigned)P25.p25_voice_discarded_total);
    LS_PRINTF(fmt_ls_ctl_current_tg_algid_0x, "  current: tg=%u algid=0x%02X %s kid=0x%04X ess=%s muted=%s\n",
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
            LS_PRINTF(t_ls_ctl_tg_5u_algid_0x, "    tg=%-5u algid=0x%02X %-8s kid=0x%04X",
                   (unsigned)P25.p25_enc_tg[i].talkgroup,
                   (unsigned)P25.p25_enc_tg[i].algid,
                   nm ? nm : "?",
                   (unsigned)P25.p25_enc_tg[i].kid);
            if (P25.p25_enc_tg[i].skip_remaining_ms > 0)
                printf("  skip=%ds",
                       (int)(P25.p25_enc_tg[i].skip_remaining_ms / 1000));
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

/* `say` - speech status, or say something. Anything that is not a
   subcommand is spoken as written. */
static int cmd_say(int argc, char **argv)
{
    if (argc < 2) {
        speech_stats_t st;
        speech_stats_get(&st);
        LS_PRINTF(fmt_ls_ctl_speech_voice_level_b, "speech %s voice=%s level=%d%% arena=%u/%u B stack free=%u/%u B\n",
               st.available ? "ready" : "unavailable",
               speech_voice_name(speech_voice_get()), speech_volume_get(),
               (unsigned)st.arena_peak, (unsigned)st.arena_bytes,
               st.stack_unused, st.stack_bytes);
        LS_PRINTF(fmt_ls_ctl_spoken_stopped_failed_busy, "  spoken=%u stopped=%u failed=%u busy=%u last_error=%d radio_yields=%u\n",
               (unsigned)st.spoken, (unsigned)st.stopped, (unsigned)st.failed,
               (unsigned)st.busy, st.last_error, (unsigned)audio_out_tts_yielded());
        LS_PRINTF(fmt_ls_ctl_last_first_sample_ms, "  last: first sample %u ms, synth %u ms, audio %u ms, %u unit(s); max first %u ms, max synth %u ms\n",
               (unsigned)st.last_first_ms, (unsigned)st.last_synth_ms,
               (unsigned)st.last_audio_ms, (unsigned)st.last_units,
               (unsigned)st.max_first_ms, (unsigned)st.max_synth_ms);
        LS_PRINTF(t_ls_ctl_audio_drops_u_underruns, "  audio drops=%u underruns=%u\n",
               (unsigned)audio_drops_get(), (unsigned)audio_underruns_get());
        LS_PRINTF(t_ls_ctl_heap_internal_u_largest, "  heap internal=%u largest=%u psram=%u\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return 0;
    }
    if (!strcmp(argv[1], "voice")) {
        if (argc > 2) {
            for (int v = 0; v < SPEECH_VOICE_COUNT; v++) {
                if (!strcmp(argv[2], speech_voice_name((speech_voice_t)v))) {
                    speech_voice_set((speech_voice_t)v);
                    settings_speech_voice_set(v);
                }
            }
        }
        printf("voice=%s\n", speech_voice_name(speech_voice_get()));
        return 0;
    }
    if (!strcmp(argv[1], "vol")) {
        if (argc > 2) {
            speech_volume_set(atoi(argv[2]));
            settings_speech_volume_set(speech_volume_get());
        }
        LS_PRINTF(t_ls_ctl_speech_volume_d_of, "speech volume=%d%% (of vol)\n", speech_volume_get());
        return 0;
    }
    if (!strcmp(argv[1], "mesh") && argc > 2) {
        /* As if MeshCore had just delivered it: 'say mesh [dm] name: text'. */
        const bool dm = !strcmp(argv[2], "dm");
        char text[SPEECH_TEXT_MAX];
        size_t len = 0;
        text[0] = 0;
        for (int i = dm ? 3 : 2; i < argc && len + 1 < sizeof(text); i++)
            len += (size_t)snprintf(text + len, sizeof(text) - len, "%s%s", len ? " " : "", argv[i]);
        audio_events_mesh_message(text, dm);
        printf("mesh voice: %s\n", audio_mesh_say_label(audio_events_mesh_say_get()));
        return 0;
    }
    if (!strcmp(argv[1], "stop")) { speech_cancel(); return 0; }
    if (!strcmp(argv[1], "test")) { audio_events_play_test(); return 0; }
    if (!strcmp(argv[1], "burst")) {
        /* New contacts in a burst, as ADS-B reports them: one is spoken in
           full and the rest as "AND n MORE". */
        static const char *const cs[] = { "N123AB", "UAL1234", "RCH871", "DAL88", "", "N9KX" };
        const int n = argc > 2 ? atoi(argv[2]) : 4;
        for (int i = 0; i < n && i < 8; i++)
            audio_events_publish(AUDIO_EVT_NEW_CONTACT, 0xA10000u + (uint32_t)i,
                                 cs[i % 6], false);
        return 0;
    }

    char text[SPEECH_TEXT_MAX];
    size_t len = 0;
    text[0] = 0;
    for (int i = 1; i < argc && len + 1 < sizeof(text); i++)
        len += (size_t)snprintf(text + len, sizeof(text) - len, "%s%s", i > 1 ? " " : "", argv[i]);
    audio_out_ensure_unmuted();
    const speech_result_t r = speech_say_async(text);
    printf("%s\n", r == SPEECH_SPOKEN ? "queued" :
                   r == SPEECH_BUSY ? "busy" :
                   r == SPEECH_UNAVAILABLE ? "unavailable" : "refused");
    return 0;
}

/**/
static int cmd_mute(int argc, char **argv)
{
    if(argc==1) audio_toggle_mute();
    else if(argc==2 && !strcmp(argv[1],"on")) audio_mute_set(true);
    else if(argc==2 && !strcmp(argv[1],"off")) audio_mute_set(false);
    else {printf("usage: mute [on|off]\n");return 1;}
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
            LS_FPUTS(msg_ls_ctl_spec_bad_range_want, "spec: bad range (want MHz, stop > start)\n", stdout);
            return 0;
        }
        FM.scan_start_hz = ah;
        FM.scan_stop_hz  = bh;
        lakeshark_fm_set_mode(FM_MODE_SCAN);
        lakeshark_fm_scan_restart();
        LS_PRINTF(t_ls_ctl_spec_sweep_4f_4f, "spec: sweep %.4f-%.4f MHz\n", ah / 1e6, bh / 1e6);
        return 0;
    }

    int top = 12;
    if (argc >= 2) {
        int v = atoi(argv[1]);
        if (v > 0 && v <= 64) top = v;
    }

    int bins = FM.scan_bins;
    if (bins < 1 || bins > FM_SCAN_BINS_MAX) {
        LS_FPUTS(msg_ls_ctl_spec_no_sweep_data, "spec: no sweep data - open FM and select SWEEP\n", stdout);
        return 0;
    }
    uint32_t span = (FM.scan_stop_hz > FM.scan_start_hz)
                  ? (FM.scan_stop_hz - FM.scan_start_hz) : 0;
    if (!span) { printf("spec: empty range\n"); return 0; }

    LS_PRINTF(fmt_ls_ctl_spec_mhz_bins_hz, "spec: %lu.%04lu-%lu.%04lu MHz  %d bins  %lu Hz/bin  sweeps=%lu tune %d/%d\n",
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
        LS_PRINTF(t_ls_ctl_2d_lu_04lu_mhz, "  %2d  %lu.%04lu MHz  %3d%%  %s\n", r + 1,
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
            LS_PRINTF(fmt_ls_ctl_crash_this_firmware_s, "crash: this firmware's ELF sha256 starts %02x%02x%02x%02x\n",
                   d->app_elf_sha256[0], d->app_elf_sha256[1],
                   d->app_elf_sha256[2], d->app_elf_sha256[3]);
            LS_FPUTS(msg_ls_ctl_crash_esp_coredump_refuses, "crash: esp-coredump refuses a dump whose sha differs. If "
                   "it reports another one, the dump is from an older build "
                   "and needs THAT build's ELF, which this repo does not "
                   "keep.\n", stdout);
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
        LS_FPUTS(msg_ls_ctl_safemode_armed_the_next, "safemode: armed - the next boot will be safe mode\n"
               "          ('safemode reboot' restarts now, 'safemode off' cancels)\n", stdout);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "off")) {
        ls_safe_force(false);
        LS_FPUTS(msg_ls_ctl_safemode_disarmed_startup_fault, "safemode: disarmed, startup fault counter cleared\n", stdout);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "reboot")) {
        printf("safemode: restarting\n");
        fflush(stdout);
        esp_restart();
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "normal")) {
        LS_FPUTS(msg_ls_ctl_safemode_restarting_into_a, "safemode: restarting into a normal boot with the guard re-armed\n", stdout);
        fflush(stdout);
        ls_safe_retry_normal_boot();
        return 0;
    }

    char buf[LS_SAFE_REPORT_MAX];
    ls_safe_report(buf, sizeof(buf));
    fputs(buf, stdout);
    LS_PRINTF(fmt_ls_ctl_safemode_currently_forced_usage, "safemode: currently %s, forced=%d\n"
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
          .help = LS_TXT(con_ls_ctl_safemode_help, "Safe-mode state, and manual entry/exit"),
          .hint = LS_TXT(con_ls_ctl_safemode_hint, "[on|off|normal|reboot]"), .func = &cmd_safemode },
        { .command = "crash",
          .help = LS_TXT(con_ls_ctl_crash_help, "Show safe dump metadata; back up before 'crash clear' erases it"),
          .hint = LS_TXT(con_ls_ctl_crash_hint, "[clear]"), .func = &cmd_crash },
        { .command = "version",
          .help = LS_TXT(con_ls_ctl_version_help, "Firmware version (git describe), board variant, build date, IDF"),
          .func = &cmd_version },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        esp_console_cmd_register(&cmds[i]);
}

void ls_ctl_register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "celliq", .help = LS_TXT(con_ls_ctl_celliq_help, "Bounded passive cellular IQ capture and transport diagnostics"),
          .func = &cell_iq_command },
        { .command = "cellreport", .help = LS_TXT(con_ls_ctl_cellreport_help, "Addressed cellular observations over MeshCore"),
          .func = &cell_report_command },
        /**/
        { .command = "vol",     .help = LS_TXT(con_ls_ctl_vol_help, "Volume 0-100 (or +n / -n)"),
          .hint = LS_TXT(con_ls_ctl_vol_hint, "<n|+n|-n>"), .func = &cmd_vol },
        { .command = "mute",    .help = LS_TXT(con_ls_ctl_mute_help, "System mute: toggle or on/off"),
          .func = &cmd_mute },
        { .command = "say",     .help = LS_TXT(con_ls_ctl_say_help, "Speech status, or speak text"),
          .hint = LS_TXT(con_ls_ctl_say_hint, "[voice glitch|dark|female | vol 0-100 | stop | test | burst n | mesh [dm] name: text | <text>]"), .func = &cmd_say },
        { .command = "p25gate", .help = LS_TXT(con_ls_ctl_p25gate_help, "P25 voice error gate (lower=mute weak frames)"),
          .hint = LS_TXT(con_ls_ctl_p25gate_hint, "<0-99>"), .func = &cmd_p25gate },
        /**/
        { .command = "fmlr", .help = LS_TXT(con_ls_ctl_fmlr_help, "FM on the LR2021: active, frequency, squelch, losses"),
          .func = &cmd_fmlr },
        { .command = "bcast", .help = LS_TXT(con_ls_ctl_bcast_help, "FM broadcast extras: stereo/RDS, air/CB scan, weather, SSTV"),
          .hint = LS_TXT(con_ls_ctl_bcast_hint, "[air off|25|833|cb|cbusb|cblsb | skip|lock|hold|free|clear | wx | sstv on|off | rds on|off | mono on|off]"),
          .func = &fm_bcast_command },
        { .command = "wf", .help = LS_TXT(con_ls_ctl_wf_help, "LR2021 IQ waterfall: on, centre, lines, gaps, skipped"),
          .func = &cmd_wf },
        { .command = "p25tsbk",
          .help = LS_TXT(con_ls_ctl_p25tsbk_help, "P25 control-channel state: IDEN, neighbours, unhandled opcodes"),
          .func = &cmd_p25tsbk },
        { .command = "p25",
          .help = LS_TXT(con_ls_ctl_p25_help, "P25 acquisition and IQ diagnostics; 'p25 profile [name]' lists or loads a profile"),
          .hint = LS_TXT(con_ls_ctl_p25_hint, "acquisition | profile [name]"), .func = &cmd_p25 },
        { .command = "radios",
          .help = LS_TXT(con_ls_ctl_radios_help, "Which radios start at boot: 'radios', 'radios ble off'"),
          .hint = LS_TXT(con_ls_ctl_radios_hint, "[ble|wifi] [on|off]"), .func = &cmd_radios },
        { .command = "dmr",
          .help = LS_TXT(con_ls_ctl_dmr_help, "DMR seen on the P25 symbol stream: bursts, colour code, last LC"),
          .func = &cmd_dmr },
        /**/
        { .command = "p25enc",
          .help = LS_TXT(con_ls_ctl_p25enc_help, "P25 encryption gate: status, counters, and leave-on-encrypted"),
          .hint = LS_TXT(con_ls_ctl_p25enc_hint, "[leave on|off | skip <sec> | reset]"), .func = &cmd_p25enc },
        { .command = "home", .help = LS_TXT(con_ls_ctl_home_help, "Get/set home QTH for the radar"),
          .hint = LS_TXT(con_ls_ctl_home_hint, "<lat> <lon>"), .func = &cmd_home },
        /**/
        { .command = "ch", .help = LS_TXT(con_ls_ctl_ch_help, "Scanner channel list"),
          .hint = LS_TXT(con_ls_ctl_ch_hint, "list|add|import|del|lock|pri|en|clear|zone <0-7|all>"), .func = &cmd_ch },
        /**/
        { .command = "stats", .help = LS_TXT(con_ls_ctl_stats_help, "NVS write counters (done/dropped/commits)"),
          .func = &cmd_stats },
        /**/
        { .command = "scan", .help = LS_TXT(con_ls_ctl_scan_help, "Channel scanner control"),
          .hint = LS_TXT(con_ls_ctl_scan_hint, "on|off|status|skip|hang|thresh|pri|src <preset|band>|band <a> <b> [step]"), .func = &cmd_scan },
        /**/
        { .command = "spec", .help = LS_TXT(con_ls_ctl_spec_help, "Strongest bins of the FM SWEEP spectrum"),
          .hint = LS_TXT(con_ls_ctl_spec_hint, "[count]"), .func = &cmd_spec },
        /**/
        { .command = "search", .help = LS_TXT(con_ls_ctl_search_help, "Repeating wideband search on the RTL: what keeps showing up"),
          .hint = LS_TXT(con_ls_ctl_search_hint, "[start [lo_MHz hi_MHz] | stop | clear | dump]"), .func = &ls_search_command },
        { .command = "catch", .help = LS_TXT(con_ls_ctl_catch_help, "RTL burst spotting with LR2021 repeat decode"),
          .hint = LS_TXT(con_ls_ctl_catch_hint, "start <MHz>|stop|list|stats|peaks|watch"), .func = &rec_catch_command },
        /**/
        { .command = "date", .help = LS_TXT(con_ls_ctl_date_help, "Print the wall-clock time (or uptime marker if unsynced)"),
          .func = &cmd_date },
        /**/
        { .command = "crash",
          .help = LS_TXT(con_ls_ctl_crash_help, "Show safe dump metadata; back up before 'crash clear' erases it"),
          .hint = LS_TXT(con_ls_ctl_crash_hint, "[clear]"), .func = &cmd_crash },
        /**/
        { .command = "version",
          .help = LS_TXT(con_ls_ctl_version_help, "Firmware version (git describe), board variant, build date, IDF"),
          .func = &cmd_version },
        /**/
        { .command = "safemode",
          .help = LS_TXT(con_ls_ctl_safemode2_help, "Safe-mode state, and manual entry into early recovery"),
          .hint = LS_TXT(con_ls_ctl_safemode2_hint, "[on|off|normal|reboot]"), .func = &cmd_safemode },
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
