#include "flipper_link.h"

#include <ctype.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#include "ls_mesh.h"
#include "ls_gps.h"
#include "ls_track_log.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "soc/soc_caps.h"

#include "lakeshark_backend.h"
#include "fm_state.h"
#include "fm_mode_label.h"
#include "audio_out.h"
#include "audio_eq.h"
#include "tone.h"
#include "ls_board.h"
#include "flipper_link_telemetry.h"
#include "ble_link.h"
#include "rec_state.h"
/**/
#include "radio_health.h"
/**/
#include "ls_version.h"

static const char *TAG = "fl_link";

#define FL_LINE_MAX    192
/**/
#define REPLY_MAX   384
#define TEL_MAX     576
#define RX_BUF_SZ   1024
#define TX_BUF_SZ   2048

static flipper_link_cfg_t s_cfg      = FLIPPER_LINK_CFG_DEFAULT();
static flipper_link_host_t s_host    = { 0 };
static TaskHandle_t       s_task     = NULL;
static volatile bool      s_run      = false;
static volatile bool      s_verbose  = false;
static volatile bool      s_installed = false;

static uint32_t s_rx_lines  = 0;
static uint32_t s_tx_lines  = 0;
static uint32_t s_bad_lines = 0;

static volatile bool s_stat_now = false;

static esp_err_t link_install(void);
static void      link_uninstall(void);

typedef enum { HOST_MODE_P25, HOST_MODE_ADSB, HOST_MODE_FM, HOST_MODE_REC } host_mode_t;

static host_mode_t host_mode(void)
{
    const char *n = s_host.current_mode_name ? s_host.current_mode_name() : "P25";
    if (!n) return HOST_MODE_P25;
    if (!strcasecmp(n, "FM"))    return HOST_MODE_FM;
    if (!strcasecmp(n, "ADS-B")) return HOST_MODE_ADSB;
    if (!strcasecmp(n, "ADSB"))  return HOST_MODE_ADSB;
    /**/
    if (!strcasecmp(n, "REC"))   return HOST_MODE_REC;
    return HOST_MODE_P25;
}

static void str_upper(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

static int tokenize(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

static void sanitize(char *s)
{
    for (; *s; s++) {
        if (*s == ' ' || *s == '\t' || *s == '=' || *s == '\r' || *s == '\n') *s = '_';
    }
}

static bool parse_i32(const char *s, int32_t *out)
{
    if (!s || !*s) return false;
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (end == s) return false;
    while (*end == ' ') end++;
    if (*end) return false;
    *out = (int32_t)v;
    return true;
}

static bool parse_freq_hz(const char *s, uint32_t *out)
{
    if (!s || !*s) return false;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || v <= 0) return false;
    if (v < 10000.0) v *= 1e6;
    if (v < 1e6 || v > 2.0e9) return false;
    *out = (uint32_t)(v + 0.5);
    return true;
}

/**/
static int sdr_stall_s(void)
{
    radio_health_snapshot_t health;
    return radio_health_get(LS_RADIO_ENDPOINT_RTL_USB, &health)
               ? health.stall_s : 0;
}

static void sdr_stall_reset(void)
{
    radio_health_note_progress_reset(LS_RADIO_ENDPOINT_RTL_USB);
}

static rh_state_t sdr_health_state(void)
{
    radio_health_snapshot_t health;
    return radio_health_get(LS_RADIO_ENDPOINT_RTL_USB, &health)
               ? health.state : RH_ABSENT;
}

static ls_telemetry_common_t telemetry_common(void)
{
    uint32_t fi = 0, fd = 0, fp = 0;
    if (s_host.heap_stats) s_host.heap_stats(&fi, &fd, &fp);

    return (ls_telemetry_common_t){
        .volume = audio_volume_get(),
        .muted = audio_is_muted() ? 1 : 0,
        .rtl_ready = lakeshark_iq_receiver_ready() ? 1 : 0,
        .uptime_s = s_host.uptime_s ? s_host.uptime_s() : 0,
        .free_internal = fi,
        .free_dma = fd,
        .sdr_stall_s = sdr_stall_s(),
        .rhs_state = radio_health_state_name(sdr_health_state()),
    };
}

int flipper_link_eq_snapshot(char *buf, size_t len)
{
    if (!buf || len < 8) return 0;

    audio_eq_cfg_t eq;
    audio_eq_get(&eq);

    int n = snprintf(buf, len, "& eq=%d eh=%d eb=%d et=%d ep=%d el=%d egr=%d\n",
                     eq.preset, eq.hp10 * 10, eq.bass_db, eq.treb_db,
                     eq.punch, eq.loud, audio_eq_gr_db10());
    if (n < 0) return 0;
    if ((size_t)n >= len) n = (int)len - 1;
    return n;
}

static int build_telemetry_fm(char *buf, size_t len)
{
    lakeshark_fm_tel_t t;
    lakeshark_fm_telemetry(&t);
    ls_telemetry_common_t common = telemetry_common();
    return ls_telemetry_build_fm(buf, len, &t, &common);
}

#define AC_PER_FRAME 4

static int build_telemetry_adsb(char *buf, size_t len)
{
    lakeshark_adsb_tel_t t;
    lakeshark_adsb_telemetry(&t);
    ls_telemetry_common_t common = telemetry_common();
    return ls_telemetry_build_adsb(buf, len, &t, &common);
}

static int build_telemetry_p25(char *buf, size_t len)
{
    lakeshark_p25_tel_t t;
    lakeshark_p25_telemetry(&t);
    const char *mode = (s_host.current_mode_name ? s_host.current_mode_name() : "P25");
    ls_telemetry_common_t common = telemetry_common();
    return ls_telemetry_build_p25(buf, len, &t, mode, lakeshark_p25_mode_name(),
                                  &common);
}

/**/
static int build_telemetry_rec(char *buf, size_t len)
{
    rec_status_t s;
    rec_get_status(&s);
    ls_telemetry_common_t common = telemetry_common();
    return ls_telemetry_build_rec(buf, len, &s, &common);
}

static int build_telemetry(char *buf, size_t len)
{
    int n;
    switch (host_mode()) {
    case HOST_MODE_FM:   n = build_telemetry_fm(buf, len);   break;
    case HOST_MODE_ADSB: n = build_telemetry_adsb(buf, len); break;
    case HOST_MODE_REC:  n = build_telemetry_rec(buf, len);  break;
    default:             n = build_telemetry_p25(buf, len);  break;
    }
    if (n < 0) return n;

    if ((size_t)n >= len - 2) n = (int)len - 2;

    buf[n++] = '\n';
    buf[n]   = '\0';
    return n;
}

static void eq_reply(char *reply, size_t reply_len)
{
    audio_eq_cfg_t eq;
    audio_eq_get(&eq);
    snprintf(reply, reply_len,
             "+OK eq=%s hp=%d bass=%+d treb=%+d punch=%d loud=%d\n",
             audio_eq_preset_name(eq.preset), eq.hp10 * 10,
             eq.bass_db, eq.treb_db, eq.punch, eq.loud);
}

static void receiver_reply(char *reply, size_t reply_len)
{
    if (!reply || reply_len == 0) return;
    static const char prefix[] = "+OK ";
    if (reply_len <= sizeof(prefix) - 1) {
        reply[0] = '\0';
        return;
    }
    memcpy(reply, prefix, sizeof(prefix));
    size_t used = sizeof(prefix) - 1;
    int n = lakeshark_receiver_status(reply + used, reply_len - used);
    if (n < 0) n = 0;
    used += strnlen(reply + used, reply_len - used);
    if (used + 1 < reply_len) {
        reply[used++] = '\n';
        reply[used] = '\0';
    } else if (reply_len >= 2) {
        reply[reply_len - 2] = '\n';
        reply[reply_len - 1] = '\0';
    }
}

/**/
#define REC_CHUNK_EDGES 32

static void rec_reply_load_status(char *reply, size_t reply_len, int load_index)
{
    rec_status_t s;
    rec_get_status(&s);
    int prefix = load_index >= 0
        ? snprintf(reply, reply_len, "+OK load=%d ", load_index)
        : snprintf(reply, reply_len, "+OK ");
    if (prefix < 0 || (size_t)prefix >= reply_len) return;
    snprintf(reply + prefix, reply_len - (size_t)prefix,
             "ph=%d e=%d sp=%lu f=%lu th=%d gp=%d"
             /**/
             " bw=%lu mp=%lu ms=%lu me=%d\n",
             (int)s.phase, s.edges, (unsigned long)s.span_us,
             (unsigned long)s.freq_hz, s.thresh_fixed, s.gap_ms,
             (unsigned long)s.bw_hz, (unsigned long)s.min_pulse_us,
             (unsigned long)(s.max_span_us / 1000u), s.min_edges);
}

static void rec_reply_status(char *reply, size_t reply_len)
{
    rec_reply_load_status(reply, reply_len, -1);
}

static void handle_rec(int argc, char **argv, char *reply, size_t reply_len)
{
    const char *sub = (argc > 1) ? argv[1] : NULL;
    const char *arg = (argc > 2) ? argv[2] : NULL;
    int32_t n = 0;

    if (!sub) {
        rec_reply_status(reply, reply_len);
        return;
    }

    char up[16];
    strlcpy(up, sub, sizeof(up));
    str_upper(up);

    if (!strcmp(up, "ARM")) {
        /**/
        if (host_mode() != HOST_MODE_REC && s_host.select_mode_by_name) {
            sdr_stall_reset();
            s_host.select_mode_by_name("rec");
        }
        rec_arm_request();
        s_stat_now = true;
        rec_reply_status(reply, reply_len);

    } else if (!strcmp(up, "STOP")) {
        rec_disarm();
        s_stat_now = true;
        rec_reply_status(reply, reply_len);

    } else if (!strcmp(up, "FREQ")) {
        uint32_t hz;
        if (!arg || !parse_freq_hz(arg, &hz)) {
            snprintf(reply, reply_len, "-ERR rec freq\n");
            return;
        }
        rec_set_freq(hz);
        s_stat_now = true;
        rec_reply_status(reply, reply_len);

    } else if (!strcmp(up, "GAIN")) {
        if (!arg || !parse_i32(arg, &n)) {
            snprintf(reply, reply_len, "-ERR rec gain\n");
            return;
        }
        rec_set_gain((int)n);
        s_stat_now = true;
        rec_reply_status(reply, reply_len);

    } else if (!strcmp(up, "THRESH")) {
        /**/
        if (!arg || !parse_i32(arg, &n)) {
            snprintf(reply, reply_len, "-ERR rec thresh\n");
            return;
        }
        rec_set_thresh((int)n);
        s_stat_now = true;
        rec_reply_status(reply, reply_len);

    } else if (!strcmp(up, "GAP")) {
        /**/
        if (!arg || !parse_i32(arg, &n)) {
            snprintf(reply, reply_len, "-ERR rec gap\n");
            return;
        }
        rec_set_gap_ms((int)n);
        s_stat_now = true;
        rec_reply_status(reply, reply_len);

    } else if (!strcmp(up, "BW")) {
        /**/
        if (!arg || !parse_i32(arg, &n)) {
            snprintf(reply, reply_len, "-ERR rec bw\n");
            return;
        }
        rec_set_bw(n > 0 ? (uint32_t)n : 0);
        s_stat_now = true;
        rec_reply_status(reply, reply_len);

    } else if (!strcmp(up, "MINP")) {
        /**/
        if (!arg || !parse_i32(arg, &n) || n <= 0) {
            snprintf(reply, reply_len, "-ERR rec minp\n");
            return;
        }
        rec_set_min_pulse((uint32_t)n);
        s_stat_now = true;
        rec_reply_status(reply, reply_len);

    } else if (!strcmp(up, "MAXSPAN")) {
        /**/
        if (!arg || !parse_i32(arg, &n) || n <= 0) {
            snprintf(reply, reply_len, "-ERR rec maxspan\n");
            return;
        }
        rec_set_max_span((uint32_t)n * 1000u);
        s_stat_now = true;
        rec_reply_status(reply, reply_len);

    } else if (!strcmp(up, "MINEDGES")) {
        /**/
        if (!arg || !parse_i32(arg, &n) || n <= 0) {
            snprintf(reply, reply_len, "-ERR rec minedges\n");
            return;
        }
        rec_set_min_edges((int)n);
        s_stat_now = true;
        rec_reply_status(reply, reply_len);

    } else if (!strcmp(up, "SAVE")) {
        char path[64];
        int w = rec_save(arg && *arg ? arg : "capture", path, sizeof(path));
        if (w > 0)       snprintf(reply, reply_len, "+OK saved %s %d\n", path, w);
        else if (w == -1) snprintf(reply, reply_len, "-ERR nothing captured\n");
        /**/
        else if (w == -3) snprintf(reply, reply_len, "-ERR rec busy\n");
        else              snprintf(reply, reply_len, "-ERR write %d\n", w);

    } else if (!strcmp(up, "LS")) {
        /**/
        /* One saved capture per round trip:
               %S <index> <total> <freq_hz> <bytes> <name>
           A zero total means nothing is saved. The head walks index 0..total-1
           to build its list. Deliberately NOT one reply carrying every name -
           REPLY_MAX is 384 and a directory has no bound, so that reply would
           truncate silently, which is the failure was written about. */
        int32_t idx = 0;
        if (arg && !parse_i32(arg, &idx)) {
            snprintf(reply, reply_len, "-ERR rec ls\n");
            return;
        }
        char name[40];
        uint32_t freq = 0;
        long size = -1;
        int total = rec_file_info((int)idx, name, sizeof(name), &freq, &size);
        if (total <= 0 || idx < 0 || idx >= total) {
            snprintf(reply, reply_len, "%%S %ld %d 0 0 -\n", (long)idx, total);
            return;
        }
        snprintf(reply, reply_len, "%%S %ld %d %lu %ld %s\n",
                 (long)idx, total, (unsigned long)freq, size, name);

    } else if (!strcmp(up, "LOAD")) {
        /**/
        int32_t idx = 0;
        if (!arg || !parse_i32(arg, &idx)) {
            snprintf(reply, reply_len, "-ERR rec load\n");
            return;
        }
        int n = rec_load((int)idx);
        if (n == -3)      snprintf(reply, reply_len, "-ERR rec busy\n");
        else if (n == -1) snprintf(reply, reply_len, "-ERR no such capture\n");
        else if (n < 0)   snprintf(reply, reply_len, "-ERR load %d\n", n);
        else {
            s_stat_now = true;
            /* Correlate the completed load with the head's selected file.
               Cached DONE telemetry may still describe a previous capture. */
            rec_reply_load_status(reply, reply_len, (int)idx);
        }

    } else if (!strcmp(up, "DEL")) {
        /**/
        int32_t idx = 0;
        if (!arg || !parse_i32(arg, &idx)) {
            snprintf(reply, reply_len, "-ERR rec del\n");
            return;
        }
        char name[40];
        int total = rec_file_info((int)idx, name, sizeof(name), NULL, NULL);
        if (total <= 0 || idx < 0 || idx >= total) {
            snprintf(reply, reply_len, "-ERR no such capture\n");
            return;
        }
        snprintf(reply, reply_len, rec_remove(name) == 0
                 ? "+OK deleted %s\n" : "-ERR delete %s\n", name);

    } else if (!strcmp(up, "GET")) {
        /**/
        int32_t off = 0;
        if (arg && !parse_i32(arg, &off)) {
            snprintf(reply, reply_len, "-ERR rec get\n");
            return;
        }
        int32_t edge[REC_CHUNK_EDGES];
        int got = rec_edges_copy((int)off, edge, REC_CHUNK_EDGES);
        if (got < 0) {
            snprintf(reply, reply_len, "-ERR rec busy\n");
            return;
        }
        int w = snprintf(reply, reply_len, "%%D %ld %d", (long)off, got);
        for (int i = 0; i < got && w > 0 && (size_t)w < reply_len - 2; i++) {
            int k = snprintf(reply + w, reply_len - (size_t)w, " %ld", (long)edge[i]);
            if (k < 0) break;
            w += k;
        }
        if (w < 0) w = 0;
        if ((size_t)w > reply_len - 2) w = (int)reply_len - 2;
        reply[w++] = '\n';
        reply[w]   = '\0';

    } else {
        snprintf(reply, reply_len,
                 "-ERR rec <arm|stop|freq|gain|thresh|gap|bw|minp|maxspan|"
                 "minedges|save|get|ls|load|del>\n");
    }
}

static void handle_line(char *line, char *reply, size_t reply_len)
{
    reply[0] = '\0';

    char *argv[8];
    int argc = tokenize(line, argv, 8);
    if (argc == 0) return;

    str_upper(argv[0]);
    const char *cmd = argv[0];
    const char *a1  = (argc > 1) ? argv[1] : NULL;
    int32_t     n   = 0;

    if (!strcmp(cmd, "PING")) {
        snprintf(reply, reply_len, "+PONG %d LakeShark\n", FLIPPER_LINK_PROTO_VERSION);

    /**/
    /* `VER` - one line describing the firmware, so the head can put "which
       LakeShark am I talking to" next to its own version.  The line is
       assembled by ls_version_format() and covered by test_ls_version, so
       the string here cannot silently drift out of shape.  sanitize()
       swaps whitespace and '=' for '_' so a single reply line survives
       the protocol without needing a special-case parser. */
    } else if (!strcmp(cmd, "VER")) {
        char v[LS_VERSION_LINE_MAX];
        ls_version_line(v, sizeof(v));
        sanitize(v);
        snprintf(reply, reply_len, "+OK %s\n", v);

    } else if (!strcmp(cmd, "STAT")) {
        s_stat_now = true;
        receiver_reply(reply, reply_len);

    } else if (!strcmp(cmd, "FREQ")) {
        uint32_t hz;
        if (!a1) {
            receiver_reply(reply, reply_len);
        } else if (!parse_freq_hz(a1, &hz)) {
            snprintf(reply, reply_len, "-ERR freq\n");
        } else if (host_mode() == HOST_MODE_ADSB) {
            snprintf(reply, reply_len, "-ERR adsb is fixed at 1090 MHz\n");
        } else {

            /**/ /**/
            sdr_stall_reset();
            if      (host_mode() == HOST_MODE_FM)  lakeshark_fm_set_freq(hz);
            else if (host_mode() == HOST_MODE_REC) rec_set_freq(hz);
            else                                   lakeshark_p25_set_freq(hz);
            s_stat_now = true;
            snprintf(reply, reply_len, "+OK f=%lu\n", (unsigned long)hz);
        }

    } else if (!strcmp(cmd, "TUNE")) {
        if (!a1 || !parse_i32(a1, &n)) {
            snprintf(reply, reply_len, "-ERR tune\n");
        } else if (host_mode() == HOST_MODE_ADSB) {
            snprintf(reply, reply_len, "-ERR adsb is fixed at 1090 MHz\n");
        } else {
            uint32_t now;
            /**/
            sdr_stall_reset();
            if (host_mode() == HOST_MODE_FM) {
                lakeshark_fm_tune((int)n);
                now = lakeshark_fm_get_freq();
            } else if (host_mode() == HOST_MODE_REC) {
                /**/
                int64_t want = (int64_t)rec_get_freq() + n;
                if (want < 1000000LL)    want = 1000000LL;
                if (want > 2000000000LL) want = 2000000000LL;
                rec_set_freq((uint32_t)want);
                now = rec_get_freq();
            } else {
                lakeshark_p25_tune((int)n);
                now = lakeshark_p25_get_freq();
            }
            s_stat_now = true;
            snprintf(reply, reply_len, "+OK f=%lu\n", (unsigned long)now);
        }

    } else if (!strcmp(cmd, "FM")) {
        if (!a1) {
            snprintf(reply, reply_len, "+OK fm=%s\n", fm_mode_command_name(
                     (fm_mode_t)lakeshark_fm_get_mode()));
            return;
        }

        fm_mode_t mode;
        if (!fm_mode_parse(a1, &mode)) {
            snprintf(reply, reply_len, "-ERR fm\n");
            return;
        }

        if (host_mode() != HOST_MODE_FM && s_host.select_mode_by_name) {
            sdr_stall_reset();
            s_host.select_mode_by_name("fm");
        }

        lakeshark_fm_set_mode((int)mode);
        if (s_host.show_fm_mode) s_host.show_fm_mode((int)mode);
        s_stat_now = true;
        snprintf(reply, reply_len, "+OK fm=%s\n", fm_mode_command_name(mode));

    } else if (!strcmp(cmd, "SQL")) {
        if (!a1) {
            snprintf(reply, reply_len, "+OK sq=%d\n", lakeshark_fm_squelch_get());
        } else if (a1[0] == '+' || a1[0] == '-') {
            if (!parse_i32(a1, &n)) { snprintf(reply, reply_len, "-ERR sql\n"); return; }
            lakeshark_fm_squelch_delta((int)n);
            s_stat_now = true;
            snprintf(reply, reply_len, "+OK sq=%d\n", lakeshark_fm_squelch_get());
        } else if (parse_i32(a1, &n)) {
            lakeshark_fm_set_squelch((int)n);
            s_stat_now = true;
            snprintf(reply, reply_len, "+OK sq=%d\n", lakeshark_fm_squelch_get());
        } else {
            snprintf(reply, reply_len, "-ERR sql\n");
        }

    } else if (!strcmp(cmd, "BAUD")) {
        if (!a1) {
            snprintf(reply, reply_len, "+OK pb=%d\n", lakeshark_fm_get_baud());
        } else if (parse_i32(a1, &n)) {
            lakeshark_fm_set_baud((int)n);
            s_stat_now = true;
            snprintf(reply, reply_len, "+OK pb=%d\n", lakeshark_fm_get_baud());
        } else {
            snprintf(reply, reply_len, "-ERR baud\n");
        }

    } else if (!strcmp(cmd, "SCAN")) {
        lakeshark_fm_scan_restart();
        s_stat_now = true;
        snprintf(reply, reply_len, "+OK\n");

    } else if (!strcmp(cmd, "PEAK")) {
        lakeshark_fm_tune_to_peak();
        s_stat_now = true;
        snprintf(reply, reply_len, "+OK f=%lu\n",
                 (unsigned long)lakeshark_fm_get_freq());

    } else if (!strcmp(cmd, "VOL")) {
        if (!a1) {
            snprintf(reply, reply_len, "+OK v=%d\n", audio_volume_get());
        } else if (a1[0] == '+' || a1[0] == '-') {
            if (!parse_i32(a1, &n)) { snprintf(reply, reply_len, "-ERR vol\n"); return; }
            audio_volume_delta((int)n);
            s_stat_now = true;
            snprintf(reply, reply_len, "+OK v=%d\n", audio_volume_get());
        } else if (parse_i32(a1, &n)) {
            if (n < 0) n = 0;
            if (n > 100) n = 100;
            audio_volume_set((int)n);
            s_stat_now = true;
            snprintf(reply, reply_len, "+OK v=%d\n", audio_volume_get());
        } else {
            snprintf(reply, reply_len, "-ERR vol\n");
        }

    } else if (!strcmp(cmd, "MUTE")) {
        if (!a1) {
            audio_toggle_mute();
        } else if (parse_i32(a1, &n)) {
            bool want = (n != 0);
            if (audio_is_muted() != want) audio_toggle_mute();
        } else {
            snprintf(reply, reply_len, "-ERR mute\n");
            return;
        }
        if (!audio_is_muted()) audio_out_ensure_unmuted();
        s_stat_now = true;
        snprintf(reply, reply_len, "+OK mu=%d\n", audio_is_muted() ? 1 : 0);

    } else if (!strcmp(cmd, "GAIN")) {
        if (!a1) {
            snprintf(reply, reply_len, "+OK g=%d\n", lakeshark_p25_gain_tenths());
            return;
        }
        char up[16];
        strlcpy(up, a1, sizeof(up));
        str_upper(up);
        if (!strcmp(up, "AUTO")) {
            /**/
            if (host_mode() == HOST_MODE_REC) rec_set_gain(0);
            else                              lakeshark_p25_agc();
        } else if (!strcmp(up, "STEP")) {
            lakeshark_p25_gain_step();
        } else if (parse_i32(a1, &n)) {
            if (n < 0) n = 0;
            if (n > 496) n = 496;
            /**/
            if (host_mode() == HOST_MODE_REC) rec_set_gain((int)n);
            else                              lakeshark_radio_set_gain((int)n);
        } else {
            snprintf(reply, reply_len, "-ERR gain\n");
            return;
        }
        s_stat_now = true;
        snprintf(reply, reply_len, "+OK g=%d\n", lakeshark_p25_gain_tenths());

    } else if (!strcmp(cmd, "DEMOD")) {
        if (!a1) {
            snprintf(reply, reply_len, "+OK dm=%d %s\n",
                     lakeshark_p25_mode_index(), lakeshark_p25_mode_name());
            return;
        }
        char up[16];
        strlcpy(up, a1, sizeof(up));
        str_upper(up);
        if (!strcmp(up, "CYCLE")) {
            lakeshark_p25_cycle_mode();
        } else if (!strcmp(up, "AUTO")) {
            lakeshark_p25_set_mode(-1);
        } else if (parse_i32(a1, &n) && n >= -1 && n <= 3) {
            lakeshark_p25_set_mode((int)n);
        } else {
            snprintf(reply, reply_len, "-ERR demod\n");
            return;
        }
        s_stat_now = true;
        snprintf(reply, reply_len, "+OK dm=%d %s\n",
                 lakeshark_p25_mode_index(), lakeshark_p25_mode_name());

    } else if (!strcmp(cmd, "POL")) {
        lakeshark_p25_toggle_polarity();
        s_stat_now = true;
        snprintf(reply, reply_len, "+OK pol=%d\n",
                 lakeshark_p25_polarity_inverted() ? 1 : 0);

    } else if (!strcmp(cmd, "BEEP")) {

        if (a1 && !strcasecmp(a1, "NOW")) {
            if (!s_host.play_test_sound) {
                snprintf(reply, reply_len, "-ERR no test sound\n");
                return;
            }
            s_host.play_test_sound();
            snprintf(reply, reply_len, "+OK beep now\n");
            return;
        }
        if (!a1) {
            lakeshark_p25_beep_toggle();
        } else if (parse_i32(a1, &n)) {
            if (lakeshark_p25_beep_enabled() != (n != 0)) lakeshark_p25_beep_toggle();
        } else {
            snprintf(reply, reply_len, "-ERR beep\n");
            return;
        }
        s_stat_now = true;
        snprintf(reply, reply_len, "+OK bp=%d\n",
                 lakeshark_p25_beep_enabled() ? 1 : 0);

    } else if (!strcmp(cmd, "VGATE")) {
        if (!a1 || !parse_i32(a1, &n)) {
            snprintf(reply, reply_len, "-ERR vgate\n");
        } else {
            lakeshark_p25_set_voice_gate((int)n);
            s_stat_now = true;
            snprintf(reply, reply_len, "+OK vg=%d\n", lakeshark_p25_voice_gate());
        }

    } else if (!strcmp(cmd, "RESET")) {
        lakeshark_p25_reset_stats();
        s_stat_now = true;
        snprintf(reply, reply_len, "+OK\n");

    } else if (!strcmp(cmd, "REC")) {
        /**/
        handle_rec(argc, argv, reply, reply_len);

    } else if (!strcmp(cmd, "MODE")) {
        if (!a1) {
            snprintf(reply, reply_len, "+OK md=%s\n",
                     s_host.current_mode_name ? s_host.current_mode_name() : "?");
            return;
        }
        if (!s_host.select_mode_by_name) {
            snprintf(reply, reply_len, "-ERR nomode\n");
            return;
        }
        sdr_stall_reset();
        s_host.select_mode_by_name(a1);
        s_stat_now = true;
        snprintf(reply, reply_len, "+OK md=%s\n",
                 s_host.current_mode_name ? s_host.current_mode_name() : "?");

    } else if (!strcmp(cmd, "REBOOT")) {
        if (!s_host.reboot) {
            snprintf(reply, reply_len, "-ERR no reboot hook\n");
            return;
        }

        snprintf(reply, reply_len, "+OK rebooting\n");
        s_host.reboot();

    } else if (!strcmp(cmd, "LOG")) {

        if (!s_host.set_log_level) {
            snprintf(reply, reply_len, "-ERR no log hook\n");
            return;
        }
        const char *a2 = (argc > 2) ? argv[2] : NULL;
        const char *tag = a1 ? a1 : "*";
        const char *lvl = a2 ? a2 : a1;
        if (!lvl) { snprintf(reply, reply_len, "-ERR log <tag|*> <level>\n"); return; }
        if (!a2) tag = "*";
        if (s_host.set_log_level(tag, lvl)) {
            snprintf(reply, reply_len, "+OK log %s=%s\n", tag, lvl);
        } else {
            snprintf(reply, reply_len, "-ERR level (none|error|warn|info|debug|verbose)\n");
        }

    } else if (!strcmp(cmd, "SYS")) {
        if (!s_host.sys_info) {
            snprintf(reply, reply_len, "-ERR no sys hook\n");
            return;
        }
        char info[192];
        info[0] = '\0';
        s_host.sys_info(info, sizeof(info));
        sanitize(info);
        snprintf(reply, reply_len, "+OK %s\n", info);

    } else if (!strcmp(cmd, "C6")) {

        if (a1 && !strcasecmp(a1, "reset")) {
            if (!s_host.c6_reset) { snprintf(reply, reply_len, "-ERR no c6 hook\n"); return; }
            snprintf(reply, reply_len, "+OK c6 reset\n");
            s_host.c6_reset();
        } else if (a1 && !strcasecmp(a1, "up")) {
            if (!s_host.c6_up) { snprintf(reply, reply_len, "-ERR no c6 hook\n"); return; }
            int e = s_host.c6_up();
            snprintf(reply, reply_len, "+OK c6 up=%d\n", e);
        } else {
            snprintf(reply, reply_len, "-ERR c6 <reset|up>\n");
        }

    } else if (!strcmp(cmd, "SDR")) {
        if (a1 && !strcasecmp(a1, "reset")) {

            if (!s_host.sdr_reset) { snprintf(reply, reply_len, "-ERR no sdr hook\n"); return; }
            s_host.sdr_reset();
            s_stat_now = true;
            snprintf(reply, reply_len, "+OK sdr reset rtl=%d\n",
                     lakeshark_radio_endpoint_ready(LS_RADIO_ENDPOINT_RTL_USB) ? 1 : 0);
        } else if (a1 && !strcasecmp(a1, "recover")) {

            if (!s_host.sdr_recover) {
                snprintf(reply, reply_len, "-ERR no sdr recover hook\n");
                return;
            }
            s_host.sdr_recover();
            snprintf(reply, reply_len, "+OK sdr recovering\n");

        } else if (a1 && !strcasecmp(a1, "power")) {

            if (!s_host.sdr_power_cycle) {
                snprintf(reply, reply_len, "-ERR no sdr power hook\n");
                return;
            }
            /* Ask first, then answer. This replied +OK before
               calling, so the head was told the dongle had been power
               cycled while the board logged that it cannot do that at all -
               and a head that believes it will stop trying anything else. */
            if (s_host.sdr_power_cycle()) {
                snprintf(reply, reply_len, "+OK sdr power cycling\n");
            } else {
                snprintf(reply, reply_len,
                         "-ERR no VBUS switch on this board - replug the "
                         "dongle, or SDR recover\n");
            }
        } else {

            receiver_reply(reply, reply_len);
        }

    } else if (!strcmp(cmd, "BLE")) {

        if (!s_host.ble_enable) {
            snprintf(reply, reply_len, "-ERR no ble hook\n");
            return;
        }
        if (a1 && !strcasecmp(a1, "off")) {
            s_host.ble_enable(false);
            snprintf(reply, reply_len, "+OK ble off\n");
        } else if (a1 && !strcasecmp(a1, "on")) {
            s_host.ble_enable(true);
            snprintf(reply, reply_len, "+OK ble on\n");
        } else {
            snprintf(reply, reply_len, "-ERR ble <on|off>\n");
        }

    } else if (!strcmp(cmd, "TEL")) {
        if (!a1 || !parse_i32(a1, &n)) {
            snprintf(reply, reply_len, "+OK tel=%d\n", s_cfg.telemetry_hz);
        } else {
            if (n < 0)  n = 0;
            if (n > 20) n = 20;
            s_cfg.telemetry_hz = (int)n;
            snprintf(reply, reply_len, "+OK tel=%d\n", s_cfg.telemetry_hz);
        }

    } else if (!strcmp(cmd, "EQ")) {
        audio_eq_cfg_t eq;
        audio_eq_get(&eq);

        if (!a1) {
            eq_reply(reply, reply_len);
            return;
        }

        char up[16];
        strlcpy(up, a1, sizeof(up));
        str_upper(up);

        const char *a2 = (argc > 2) ? argv[2] : NULL;
        bool is_field = strcmp(up, "BASS") == 0 || strcmp(up, "TREB") == 0 ||
                        strcmp(up, "HP")   == 0 || strcmp(up, "PUNCH") == 0 ||
                        strcmp(up, "LOUD") == 0;
        bool need_val = is_field && a2 != NULL;

        if (!need_val) {
            int p = audio_eq_preset_from_name(a1);
            if (p < 0 && parse_i32(a1, &n) &&
                n >= 0 && n < AUDIO_EQ_PRESET_COUNT) p = (int)n;
            if (p < 0) { snprintf(reply, reply_len, "-ERR eq\n"); return; }
            audio_eq_apply_preset(p);
            s_stat_now = true;
            eq_reply(reply, reply_len);
            return;
        }

        if (!a2 || !parse_i32(a2, &n)) { snprintf(reply, reply_len, "-ERR eq\n"); return; }

        if (!strcmp(up, "HP")) {
            int hz = (n > 0 && n < AUDIO_EQ_HP_MIN) ? (int)n * 10 : (int)n;
            eq.hp10 = (uint8_t)(hz <= 0 ? 0 : (hz + 5) / 10);
        } else if (!strcmp(up, "BASS")) {
            eq.bass_db = (int8_t)n;
        } else if (!strcmp(up, "TREB")) {
            eq.treb_db = (int8_t)n;
        } else if (!strcmp(up, "PUNCH")) {
            eq.punch = (uint8_t)(n < 0 ? 0 : (n > 100 ? 100 : n));
        } else {
            eq.loud = (uint8_t)(n < 0 ? 0 : (n > AUDIO_EQ_LOUD_MAX ? AUDIO_EQ_LOUD_MAX : n));
        }
        eq.preset = AUDIO_EQ_CUSTOM;
        audio_eq_set(&eq);
        s_stat_now = true;
        eq_reply(reply, reply_len);

    } else if (!strcmp(cmd, "TEST")) {
        if (!a1) {
            snprintf(reply, reply_len, "+OK test=%s busy=%d\n",
                     "sweep|bass|noise|tone|chirp|moto", snd_test_busy() ? 1 : 0);
            return;
        }
        int w = snd_test_from_name(a1);
        if (w < 0 && parse_i32(a1, &n) && n >= 0 && n < SND_TEST_COUNT) w = (int)n;
        if (w < 0) { snprintf(reply, reply_len, "-ERR test\n"); return; }

        audio_out_ensure_unmuted();
        if (!snd_test_start(w)) {
            snprintf(reply, reply_len, "-ERR test busy\n");
            return;
        }
        snprintf(reply, reply_len, "+OK test=%s\n", snd_test_name(w));

    } else if (!strcmp(cmd, "MESH")) {
        ls_mesh_stats_t ms;
        memset(&ms, 0, sizeof(ms));
        ls_mesh_get_stats(&ms);

        snprintf(reply, reply_len,
                 "+OK run=%d tx=%d rx=%lu bad=%lu sent=%lu rssi=%.0f snr=%.1f "
                 "air=%lu budget=%lu peers=%d id=%s\n",
                 ms.running ? 1 : 0, ms.tx_enabled ? 1 : 0,
                 (unsigned long)ms.rx_packets, (unsigned long)ms.rx_bad,
                 (unsigned long)ms.tx_packets,
                 (double)ms.last_rssi, (double)ms.last_snr,
                 (unsigned long)ms.airtime_ms, (unsigned long)ms.tx_budget_ms,
                 ls_mesh_peers(NULL, 0),
                 ms.self_id[0] ? ms.self_id : "-");

    } else if (!strcmp(cmd, "GPS")) {
        ls_gps_state_t g;
        ls_gps_get(&g);
        /*'s own distinction, carried to the head: bytes climbing with
           sentences flat is a baud rate, both climbing with no fix is the
           antenna, and neither is "the GPS is broken". */
        snprintf(reply, reply_len,
                 "+OK on=%d fix=%d alive=%d used=%u seen=%u "
                 "lat=%.5f lon=%.5f bytes=%lu ok=%lu bad=%lu\n",
                 ls_gps_running() ? 1 : 0, g.fix ? 1 : 0, g.alive ? 1 : 0,
                 (unsigned)g.sats_used, (unsigned)g.sats_visible,
                 g.fix ? g.lat_deg : 0.0, g.fix ? g.lon_deg : 0.0,
                 (unsigned long)g.bytes, (unsigned long)g.sentences,
                 (unsigned long)g.checksum_errors);

    } else if (!strcmp(cmd, "TRACK")) {
        if (a1 && !strcasecmp(a1, "on")) {
            /* Brings the receiver up with it, the same way the screen's
               toggle does - a recorder running against a receiver nobody
               started records a thousand seconds of nothing and reports
               itself as working. */
            const bool ok = ls_track_rec_start() == ESP_OK;
            snprintf(reply, reply_len, ok ? "+OK track=on\n"
                                          : "-ERR track would not start\n");
        } else if (a1 && !strcasecmp(a1, "off")) {
            ls_track_rec_stop();
            snprintf(reply, reply_len, "+OK track=off\n");
        } else if (a1) {
            snprintf(reply, reply_len, "-ERR track [on|off]\n");
        } else {
            /* The count is what turns "recording" into evidence: a recorder
               that is on and has kept nothing looks identical to one that is
               working until the number is on the screen. */
            snprintf(reply, reply_len, "+OK on=%d points=%d nodes=%d\n",
                     ls_track_rec_running() ? 1 : 0,
                     ls_track_points(), ls_mesh_sightings());
        }

    } else {
        s_bad_lines++;
        snprintf(reply, reply_len, "-ERR unknown %s\n", cmd);
    }
}

int flipper_link_snapshot(char *buf, size_t len)
{
    return build_telemetry(buf, len);
}

/* The wired head link needs pins this board may not have declared. */

#if LS_HAS_LINK_UART

/**/
static const int SCAN_PINS[] = LS_BOARD_LINK_SCAN_PINS;
#define N_SCAN_PINS ((int)(sizeof(SCAN_PINS) / sizeof(SCAN_PINS[0])))

/**/
#define VBUS_EN_GPIO LS_BOARD_VBUS_EN_GPIO
#define C6_EN_GPIO   LS_BOARD_C6_EN_GPIO

static const int IDLE_HIGH_PINS[] = { LS_BOARD_LINK_TX_GPIO, 36 };

static bool pin_idles_high(int pin)
{
    for (size_t i = 0; i < sizeof(IDLE_HIGH_PINS) / sizeof(IDLE_HIGH_PINS[0]); i++) {
        if (IDLE_HIGH_PINS[i] == pin) return true;
    }
    return false;
}

int flipper_link_scan_rx(int dwell_ms)
{
    if (dwell_ms < 200)  dwell_ms = 200;
    if (dwell_ms > 5000) dwell_ms = 5000;

    flipper_link_cfg_t saved = s_cfg;
    bool was_running = s_run;
    if (was_running) flipper_link_stop();

    s_cfg.tx_gpio = -1;
    esp_err_t err = link_install();
    if (err != ESP_OK) {
        printf("scan: uart install failed: %s\n", esp_err_to_name(err));
        s_cfg = saved;
        if (was_running) flipper_link_start(&s_cfg, NULL);
        return -1;
    }

    printf("scanning %d pins, %d ms each (drive the head so it keeps talking)\n",
           N_SCAN_PINS, dwell_ms);

    int best = -1;
    size_t best_n = 0;
    for (int i = 0; i < N_SCAN_PINS; i++) {
        int pin = SCAN_PINS[i];

        if (uart_set_pin(s_cfg.uart_num, UART_PIN_NO_CHANGE, pin,
                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
            printf("  GPIO%-2d : (cannot route)\n", pin);
            continue;
        }
        uart_flush_input(s_cfg.uart_num);
        vTaskDelay(pdMS_TO_TICKS(dwell_ms));

        size_t avail = 0;
        uart_get_buffered_data_len(s_cfg.uart_num, &avail);

        char peek[33] = { 0 };
        if (avail) {
            int n = uart_read_bytes(s_cfg.uart_num, (uint8_t *)peek,
                                    avail > 32 ? 32 : avail, 0);
            for (int k = 0; k < n; k++) {
                if (peek[k] < 32 || peek[k] > 126) peek[k] = '.';
            }
            peek[n > 0 ? n : 0] = '\0';
        }
        printf("  GPIO%-2d : %4u bytes  %s\n", pin, (unsigned)avail, peek);

        if (avail > best_n) { best_n = avail; best = pin; }
        uart_flush_input(s_cfg.uart_num);
    }

    link_uninstall();
    s_cfg = saved;
    if (was_running) flipper_link_start(&s_cfg, NULL);

    if (best >= 0) printf("scan: busiest pin GPIO%d (%u bytes)\n", best, (unsigned)best_n);
    else           printf("scan: nothing heard on any pin\n");
    return best;
}

static int probe_pin_pct(int pin)
{
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&in) != ESP_OK) return -1;

    vTaskDelay(pdMS_TO_TICKS(5));

    int high = 0;
    const int samples = 200;
    for (int i = 0; i < samples; i++) {
        if (gpio_get_level(pin)) high++;
        esp_rom_delay_us(100);
    }

    in.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&in);

    return (high * 100) / samples;
}

int flipper_link_probe_rx(void)
{
    flipper_link_cfg_t saved = s_cfg;
    bool was_running = s_run;
    if (was_running) flipper_link_stop();

    esp_log_level_t gpio_lvl = esp_log_level_get("gpio");
    esp_log_level_set("gpio", ESP_LOG_WARN);

    printf("probing %d header pins with a pull-down.\n"
           "  ~100%% = something external is driving it high (an idle UART TX)\n"
           "  <100%% = that line is also carrying traffic\n"
           "     0%% = nothing connected, or the far end is unpowered\n",
           N_SCAN_PINS);

    int best = -1, best_pct = 0, found = 0;
    for (int i = 0; i < N_SCAN_PINS; i++) {
        int pin = SCAN_PINS[i];
        /**/
        if (LS_HAS_VBUS_CTRL && pin == VBUS_EN_GPIO) continue;
        if (pin == C6_EN_GPIO) continue;

        int pct = probe_pin_pct(pin);
        if (pct < 0) {
            printf("  GPIO%-2d : (cannot configure)\n", pin);
            continue;
        }

        bool baseline = pin_idles_high(pin);
        printf("  GPIO%-2d : %3d%% high%s\n", pin, pct,
               baseline ? "   (board pull-up, ignored)"
                        : (pct > 0 ? "   <-- driven" : ""));
        if (baseline) continue;

        if (pct > 0) found++;
        if (pct > best_pct) { best_pct = pct; best = pin; }
    }

    if (best >= 0) {
        printf("probe: GPIO%d is being driven (%d%%). %d pin(s) driven in total.\n",
               best, best_pct, found);
        if (best != saved.rx_gpio) {
            printf("probe: the link expects RX on GPIO%d - the wire is on the "
                   "wrong pin. `link pins %d %d` to adopt it.\n",
                   saved.rx_gpio, best, saved.tx_gpio);
        } else {
            printf("probe: that is the configured RX pin - wiring is good.\n");
        }
    } else {
        printf("probe: every pin followed the pull-down - the head's TX is not "
               "reaching any P4 header pin.\n"
               "       The P4 cannot see C6_IO12/C6_IO13/C6_U0RXD/C6_U0TXD at "
               "all (those belong to the on-board ESP32-C6), so a jumper one or\n"
               "       two positions off the mark looks exactly like this. "
               "Count header positions against the Waveshare pinout, and check "
               "the head is powered with its app open.\n");
    }

    esp_log_level_set("gpio", gpio_lvl);
    s_cfg = saved;
    if (was_running) flipper_link_start(&s_cfg, NULL);
    return best;
}

#else  /* !LS_HAS_LINK_UART */

int flipper_link_scan_rx(int dwell_ms)
{
    (void)dwell_ms;
    printf("this board declares no wired head-link pins - nothing to scan.\n"
           "  see LS_BOARD_LINK_SCAN_PINS in components/lakeshark/board/variants/\n");
    return -1;
}

int flipper_link_probe_rx(void)
{
    printf("this board declares no wired head-link pins - nothing to probe.\n");
    return -1;
}

#endif /* LS_HAS_LINK_UART */

static bool link_has_scan_pins(void)
{
#if LS_HAS_LINK_UART
    return N_SCAN_PINS > 0;
#else
    return false;
#endif
}

void flipper_link_inject(const char *line, char *reply, size_t reply_len)
{
    char tmp[FL_LINE_MAX];
    strlcpy(tmp, line, sizeof(tmp));
    handle_line(tmp, reply, reply_len);
}

static void link_write(const char *s, int len)
{
    if (!s_installed) return;
    uart_write_bytes(s_cfg.uart_num, s, len);
    s_tx_lines++;
}

static void link_task(void *arg)
{
    (void)arg;
    char    line[FL_LINE_MAX];
    int     pos = 0;
    char    reply[REPLY_MAX];
    char    tel[TEL_MAX];
    uint8_t rx[128];
    int64_t next_tel_us = 0;

    ESP_LOGI(TAG, "link task up: uart%d rx=GPIO%d tx=GPIO%d %lu baud tel=%dHz",
             s_cfg.uart_num, s_cfg.rx_gpio, s_cfg.tx_gpio,
             (unsigned long)s_cfg.baud, s_cfg.telemetry_hz);

    while (s_run) {
        int n = uart_read_bytes(s_cfg.uart_num, rx, sizeof(rx), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) {
            char c = (char)rx[i];
            if (c == '\n' || c == '\r') {
                if (pos > 0) {
                    line[pos] = '\0';
                    s_rx_lines++;
                    if (s_verbose) ESP_LOGI(TAG, "RX <%s>", line);
                    handle_line(line, reply, sizeof(reply));
                    if (reply[0]) link_write(reply, (int)strlen(reply));
                    pos = 0;
                }
            } else if (pos < FL_LINE_MAX - 1) {
                line[pos++] = c;
            } else {

                pos = 0;
                s_bad_lines++;
            }
        }

        int64_t now = esp_timer_get_time();
        int hz = s_cfg.telemetry_hz;
        bool due = s_stat_now || (hz > 0 && now >= next_tel_us);
        if (due) {
            s_stat_now = false;
            if (hz > 0) next_tel_us = now + (1000000 / hz);
            int len = build_telemetry(tel, sizeof(tel));
            if (len > 0) link_write(tel, len > (int)sizeof(tel) - 1
                                             ? (int)sizeof(tel) - 1 : len);
            char eqline[96];
            int eqn = flipper_link_eq_snapshot(eqline, sizeof(eqline));
            if (eqn > 0) link_write(eqline, eqn);
        }
    }

    ESP_LOGI(TAG, "link task exiting");
    s_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t link_install(void)
{
    uart_config_t uc = {
        .baud_rate  = (int)s_cfg.baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (uart_is_driver_installed(s_cfg.uart_num)) {
        int found = -1;
        for (int u = SOC_UART_HP_NUM - 1; u >= 1; u--) {
            if (u == CONFIG_ESP_CONSOLE_UART_NUM) continue;
            if (!uart_is_driver_installed(u)) { found = u; break; }
        }
        if (found < 0) return ESP_ERR_NOT_FOUND;
        ESP_LOGW(TAG, "uart%d busy, using uart%d instead", s_cfg.uart_num, found);
        s_cfg.uart_num = found;
    }

    esp_err_t err = uart_driver_install(s_cfg.uart_num, RX_BUF_SZ, TX_BUF_SZ, 0, NULL, 0);
    if (err != ESP_OK) return err;
    err = uart_param_config(s_cfg.uart_num, &uc);
    if (err != ESP_OK) goto fail;
    err = uart_set_pin(s_cfg.uart_num, s_cfg.tx_gpio, s_cfg.rx_gpio,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) goto fail;

    /* HOLD THE RECEIVE PIN AT IDLE WHEN NOTHING IS DRIVING IT. */

    gpio_set_pull_mode(s_cfg.rx_gpio, GPIO_PULLUP_ONLY);

    s_installed = true;
    return ESP_OK;

fail:
    uart_driver_delete(s_cfg.uart_num);
    return err;
}

static void link_uninstall(void)
{
    if (!s_installed) return;
    s_installed = false;
    uart_driver_delete(s_cfg.uart_num);

    if (s_cfg.tx_gpio < 0) return;

    gpio_config_t out = {
        .pin_bit_mask = 1ULL << s_cfg.tx_gpio,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&out);
    gpio_set_level(s_cfg.tx_gpio, 1);
}

/**/
#define LS_HEAL_MAX_SWEEPS 3

static void heal_task(void *arg)
{
    (void)arg;
    int  sweeps   = 0;
    bool told_ble = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        if (!s_run || s_rx_lines > 0) continue;
        if (s_tx_lines < 50) continue;

        if (ble_link_state() == BLE_LINK_READY) {
            if (!told_ble) {
                ESP_LOGI(TAG, "head is on BLE - leaving the UART pins alone");
                told_ble = true;
            }
            continue;
        }

        /* A board with no pins to sweep has no search to announce, and announcing it three times a minute apart is worse than saying it once. */

        if (!link_has_scan_pins()) {
            if (!sweeps) {
                ESP_LOGI(TAG, "no RX after %lu TX frames, and this board has "
                              "no pins to sweep - a head is on GPIO%d or it "
                              "is not attached",
                         (unsigned long)s_tx_lines, s_cfg.rx_gpio);
            }
            sweeps = LS_HEAL_MAX_SWEEPS;
            continue;
        }

        if (sweeps >= LS_HEAL_MAX_SWEEPS) continue;
        sweeps++;

        ESP_LOGW(TAG, "no RX after %lu TX frames - sweeping header pins for the head "
                      "(attempt %d of %d)",
                 (unsigned long)s_tx_lines, sweeps, LS_HEAL_MAX_SWEEPS);
        int pin = flipper_link_scan_rx(700);
        if (pin >= 0 && pin != s_cfg.rx_gpio) {
            flipper_link_cfg_t c = s_cfg;
            c.rx_gpio = pin;
            ESP_LOGW(TAG, "adopting GPIO%d as link RX", pin);
            flipper_link_reconfigure(&c);
        } else if (sweeps >= LS_HEAL_MAX_SWEEPS) {
            ESP_LOGW(TAG, "giving up on the UART sweep - use 'link pins <rx> <tx>' "
                          "or run the head over BLE");
        }
    }
}

void flipper_link_set_host(const flipper_link_host_t *host)
{
    if (host) s_host = *host;
}

esp_err_t flipper_link_start(const flipper_link_cfg_t *cfg,
                             const flipper_link_host_t *host)
{
    if (!LS_HAS_LINK_UART) return ESP_ERR_NOT_SUPPORTED;
    if (s_run) return ESP_ERR_INVALID_STATE;
    if (cfg)  s_cfg  = *cfg;
    flipper_link_set_host(host);

    esp_err_t err = link_install();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart install failed: %s", esp_err_to_name(err));
        return err;
    }

    s_run = true;

    if (xTaskCreate(link_task, "fliplink", 6144, NULL, 5, &s_task) != pdPASS) {
        s_run = false;
        link_uninstall();
        return ESP_ERR_NO_MEM;
    }

    static bool healer_started = false;
    if (!healer_started) {
        healer_started = (xTaskCreate(heal_task, "fl_heal", 4096, NULL, 3, NULL) == pdPASS);
    }

    /**/
    /* Include the version in the HELLO so the head sees which firmware it
       just connected to without having to ask.  A stale head that only
       parses `+HELLO %d LakeShark` still matches (the version is appended
       after "LakeShark ", not before). */
    char v[LS_VERSION_LINE_MAX];
    ls_version_line(v, sizeof(v));
    sanitize(v);
    char hello[LS_VERSION_LINE_MAX + 32];
    int  len = snprintf(hello, sizeof(hello), "+HELLO %d LakeShark %s\n",
                        FLIPPER_LINK_PROTO_VERSION, v);
    link_write(hello, len);
    return ESP_OK;
}

void flipper_link_stop(void)
{
    if (!s_run) return;
    s_run = false;
    for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
    link_uninstall();
}

bool flipper_link_running(void) { return s_run; }

esp_err_t flipper_link_reconfigure(const flipper_link_cfg_t *cfg)
{
    bool was = s_run;
    if (was) flipper_link_stop();
    if (cfg) s_cfg = *cfg;
    if (!was) return ESP_OK;
    return flipper_link_start(&s_cfg, NULL);
}

void flipper_link_get_cfg(flipper_link_cfg_t *out) { if (out) *out = s_cfg; }

int flipper_link_sdr_stall_s(void) { return sdr_stall_s(); }

void flipper_link_set_verbose(bool en) { s_verbose = en; }
bool flipper_link_verbose(void)        { return s_verbose; }

void flipper_link_stats(uint32_t *rx_lines, uint32_t *tx_lines, uint32_t *bad_lines)
{
    if (rx_lines)  *rx_lines  = s_rx_lines;
    if (tx_lines)  *tx_lines  = s_tx_lines;
    if (bad_lines) *bad_lines = s_bad_lines;
}
