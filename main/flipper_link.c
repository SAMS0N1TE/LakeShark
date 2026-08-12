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
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "soc/soc_caps.h"

#include "lakeshark_backend.h"
#include "audio_out.h"
#include "audio_eq.h"
#include "tone.h"
#include "ls_board.h"
#include "ble_link.h"
#include "rec_state.h"

static const char *TAG = "fl_link";

#define FL_LINE_MAX    192
/*LS-511*/
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
    /*LS-510*/
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

static uint32_t s_last_bps = 0;
static int64_t  s_bps_ok_us = 0;

static bool s_bps_seen = false;

static int sdr_stall_s(void)
{
    int64_t now = esp_timer_get_time();

    if (!lakeshark_radio_device_ready()) {

        s_bps_ok_us = now;
        return 0;
    }
    if (s_last_bps > 0) {
        s_bps_ok_us = now;
        s_bps_seen  = true;
        return 0;
    }
    if (!s_bps_seen || s_bps_ok_us == 0) {
        s_bps_ok_us = now;
        return 0;
    }
    return (int)((now - s_bps_ok_us) / 1000000LL);
}

static void sdr_stall_reset(void)
{
    s_bps_seen  = false;
    s_last_bps  = 0;
    s_bps_ok_us = esp_timer_get_time();
}

static int append_sys(char *buf, size_t len, int n)
{
    static int64_t last_us = 0;

    if (n < 0 || (size_t)n >= len - 1) return n;

    int stall = sdr_stall_s();

    int64_t now = esp_timer_get_time();

    bool due = !last_us || (now - last_us >= 1000000LL);
    if (!due && stall == 0) return n;
    if (due) last_us = now;

    uint32_t up = s_host.uptime_s ? s_host.uptime_s() : 0;
    uint32_t fi = 0, fd = 0, fp = 0;
    if (s_host.heap_stats) s_host.heap_stats(&fi, &fd, &fp);

    int w = snprintf(buf + n, len - (size_t)n, " up=%lu fi=%lu fd=%lu stl=%d",
                     (unsigned long)up, (unsigned long)fi, (unsigned long)fd, stall);
    if (w < 0 || (size_t)(n + w) >= len - 1) return n;
    return n + w;
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
    static const char *sub[] = { "listen", "scan", "pocsag", "wfm" };

    lakeshark_fm_tel_t t;
    lakeshark_fm_telemetry(&t);
    s_last_bps = t.iq_bytes_sec;

    char text[80];
    strlcpy(text, t.pocsag_last_text[0] ? t.pocsag_last_text : "-", sizeof(text));
    sanitize(text);

    char ptype[2] = { t.pocsag_last_type ? t.pocsag_last_type : '-', 0 };

    return snprintf(buf, len,
        "$ f=%lu g=%d v=%d mu=%d iq=%d rtl=%d re=%d bps=%lu md=FM "
        "fm=%s sq=%d so=%d au=%d "
        "ss=%lu se=%lu spk=%lu sdb=%d sw=%lu "
        "pb=%d pau=%d psy=%d pp=%lu pf=%lu pa=%lu pbd=%d pty=%s ptx=%s",
        (unsigned long)t.freq_hz, t.gain_tenths,
        audio_volume_get(), audio_is_muted() ? 1 : 0,
        t.iq_level, lakeshark_radio_device_ready() ? 1 : 0, t.read_errors,
        (unsigned long)t.iq_bytes_sec,
        sub[t.submode & 3], t.squelch_tenths, t.squelch_open, t.audio_level,
        (unsigned long)t.scan_start_hz, (unsigned long)t.scan_stop_hz,
        (unsigned long)t.scan_peak_hz, t.scan_peak_db,
        (unsigned long)t.scan_sweeps,
        t.pocsag_baud, t.pocsag_auto, t.pocsag_sync,
        (unsigned long)t.pocsag_pages, (unsigned long)t.pocsag_frames,
        (unsigned long)t.pocsag_last_addr, t.pocsag_last_baud, ptype, text);
}

#define AC_PER_FRAME 4

static int build_telemetry_adsb(char *buf, size_t len)
{
    static int s_ac_cursor = 0;

    lakeshark_adsb_tel_t t;
    lakeshark_adsb_telemetry(&t);
    s_last_bps = t.iq_bytes_sec;

    if (t.n_aircraft <= 0) s_ac_cursor = 0;
    else if (s_ac_cursor >= t.n_aircraft) s_ac_cursor = 0;

    int n = snprintf(buf, len,
        "$ f=%lu g=%d v=%d mu=%d rtl=%d bps=%lu md=ADSB "
        "ac=%d mt=%d mps=%d cg=%d ce=%d bps1=%d mga=%d mgp=%d lms=%d "
        "aci=%d acn=%d",
        (unsigned long)t.freq_hz, t.gain_tenths,
        audio_volume_get(), audio_is_muted() ? 1 : 0,
        t.rtl_ready, (unsigned long)t.iq_bytes_sec,
        t.tracked, t.msgs_total, t.msgs_sec, t.crc_good, t.crc_err,
        t.bursts_sec, t.mag_avg, t.mag_peak, t.last_msg_ms,
        s_ac_cursor, t.n_aircraft);
    if (n < 0) return n;

    for (int k = 0; k < AC_PER_FRAME && (size_t)n < len - 1; k++) {
        int idx = s_ac_cursor + k;
        if (idx >= t.n_aircraft) break;

        lakeshark_adsb_ac_t a;
        if (!lakeshark_adsb_aircraft_at(idx, &a)) break;

        char call[12];
        strlcpy(call, a.callsign[0] ? a.callsign : "-", sizeof(call));
        sanitize(call);

        int w = snprintf(buf + n, len - (size_t)n,
                         " a%d=%06lX,%s,%d,%d,%d,%d,%d,%d",
                         idx, (unsigned long)a.icao, call,
                         a.altitude, a.velocity, a.heading, a.vert_rate,
                         a.age_ms, a.msg_count);
        if (w < 0 || (size_t)(n + w) >= len - 1) break;
        n += w;
    }

    s_ac_cursor += AC_PER_FRAME;
    if (s_ac_cursor >= t.n_aircraft) s_ac_cursor = 0;

    return n;
}

static int build_telemetry_p25(char *buf, size_t len)
{

    lakeshark_p25_tel_t t;
    lakeshark_p25_telemetry(&t);
    s_last_bps = t.iq_bytes_sec;

    char ftype[16];
    char err[64];
    strlcpy(ftype, t.ftype[0] ? t.ftype : "-", sizeof(ftype));
    strlcpy(err,   t.err[0]   ? t.err   : "-", sizeof(err));
    sanitize(ftype);
    sanitize(err);

    const char *mode = (s_host.current_mode_name ? s_host.current_mode_name() : "P25");

    return snprintf(buf, len,
        "$ f=%lu dm=%d dmn=%s g=%d agc=%d v=%d mu=%d "
        "nac=%d tg=%d src=%d na=%d ta=%d sa=%d sy=%d vo=%d sc=%d vc=%d bo=%d bf=%d "
        "iq=%d pol=%d bp=%d vg=%d rtl=%d rf=%d rs=%d re=%d "
        "bps=%lu ad=%lu dus=%d md=%s ft=%s e=%s",
        (unsigned long)t.freq_hz, t.demod_mode, lakeshark_p25_mode_name(),
        t.gain_tenths, t.agc_on, audio_volume_get(), audio_is_muted() ? 1 : 0,
        t.nac, t.tg, t.src,
        t.nac_age_ms, t.tg_age_ms, t.src_age_ms,
        t.has_sync, t.voice_active,
        t.sync_count, t.voice_count, t.bch_ok, t.bch_fail,
        t.iq_level, t.polarity_inverted, t.beep, t.voice_gate, t.rtl_ready,
        t.ring_fill, t.ring_size, t.read_errors,
        (unsigned long)t.iq_bytes_sec, (unsigned long)t.audio_drops, t.decode_us,
        mode, ftype, err);
}

/*LS-510*/
static int build_telemetry_rec(char *buf, size_t len)
{
    rec_status_t s;
    rec_get_status(&s);
    s_last_bps = s.bytes_sec;

    char last[24];
    strlcpy(last, s.last_file[0] ? s.last_file : "-", sizeof(last));
    sanitize(last);

    return snprintf(buf, len,
        "$ f=%lu g=%d v=%d mu=%d rtl=%d bps=%lu md=REC "
        "rph=%d red=%d rsp=%lu rmg=%d rfl=%d rth=%d rtf=%d rgp=%d rcp=%lu "
        /*LS-516*/ /*LS-517*/
        "rbw=%lu rmp=%lu rms=%lu rme=%d ren=%d rmn=%lu rmx=%lu rbd=%lu rlf=%s",
        (unsigned long)s.freq_hz, s.gain_tenths,
        audio_volume_get(), audio_is_muted() ? 1 : 0,
        lakeshark_radio_device_ready() ? 1 : 0, (unsigned long)s.bytes_sec,
        (int)s.phase, s.edges, (unsigned long)s.span_us,
        s.mag_now, s.mag_floor, s.mag_thresh, s.thresh_fixed, s.gap_ms,
        (unsigned long)s.captures,
        (unsigned long)s.bw_hz, (unsigned long)s.min_pulse_us,
        (unsigned long)(s.max_span_us / 1000u), s.min_edges, s.end_reason,
        (unsigned long)s.min_mark_us, (unsigned long)s.max_mark_us,
        (unsigned long)s.baud_est, last);
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

    n = append_sys(buf, len, n);

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

/*LS-511*/
#define REC_CHUNK_EDGES 32

static void rec_reply_status(char *reply, size_t reply_len)
{
    rec_status_t s;
    rec_get_status(&s);
    snprintf(reply, reply_len,
             "+OK ph=%d e=%d sp=%lu f=%lu th=%d gp=%d"
             /*LS-516*/
             " bw=%lu mp=%lu ms=%lu me=%d\n",
             (int)s.phase, s.edges, (unsigned long)s.span_us,
             (unsigned long)s.freq_hz, s.thresh_fixed, s.gap_ms,
             (unsigned long)s.bw_hz, (unsigned long)s.min_pulse_us,
             (unsigned long)(s.max_span_us / 1000u), s.min_edges);
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
        /*LS-506*/
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
        /*LS-503*/
        if (!arg || !parse_i32(arg, &n)) {
            snprintf(reply, reply_len, "-ERR rec thresh\n");
            return;
        }
        rec_set_thresh((int)n);
        s_stat_now = true;
        rec_reply_status(reply, reply_len);

    } else if (!strcmp(up, "GAP")) {
        /*LS-504*/
        if (!arg || !parse_i32(arg, &n)) {
            snprintf(reply, reply_len, "-ERR rec gap\n");
            return;
        }
        rec_set_gap_ms((int)n);
        s_stat_now = true;
        rec_reply_status(reply, reply_len);

    } else if (!strcmp(up, "BW")) {
        /*LS-516*/
        if (!arg || !parse_i32(arg, &n)) {
            snprintf(reply, reply_len, "-ERR rec bw\n");
            return;
        }
        rec_set_bw(n > 0 ? (uint32_t)n : 0);
        s_stat_now = true;
        rec_reply_status(reply, reply_len);

    } else if (!strcmp(up, "MINP")) {
        /*LS-516*/
        if (!arg || !parse_i32(arg, &n) || n <= 0) {
            snprintf(reply, reply_len, "-ERR rec minp\n");
            return;
        }
        rec_set_min_pulse((uint32_t)n);
        s_stat_now = true;
        rec_reply_status(reply, reply_len);

    } else if (!strcmp(up, "MAXSPAN")) {
        /*LS-516*/
        if (!arg || !parse_i32(arg, &n) || n <= 0) {
            snprintf(reply, reply_len, "-ERR rec maxspan\n");
            return;
        }
        rec_set_max_span((uint32_t)n * 1000u);
        s_stat_now = true;
        rec_reply_status(reply, reply_len);

    } else if (!strcmp(up, "MINEDGES")) {
        /*LS-516*/
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
        /*LS-513*/
        else if (w == -3) snprintf(reply, reply_len, "-ERR rec busy\n");
        else              snprintf(reply, reply_len, "-ERR write %d\n", w);

    } else if (!strcmp(up, "GET")) {
        /*LS-511*/
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
                 "minedges|save|get>\n");
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

    } else if (!strcmp(cmd, "STAT")) {
        s_stat_now = true;
        snprintf(reply, reply_len, "+OK\n");

    } else if (!strcmp(cmd, "FREQ")) {
        uint32_t hz;
        if (!a1 || !parse_freq_hz(a1, &hz)) {
            snprintf(reply, reply_len, "-ERR freq\n");
        } else if (host_mode() == HOST_MODE_ADSB) {
            snprintf(reply, reply_len, "-ERR adsb is fixed at 1090 MHz\n");
        } else {

            /*LS-510*/
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
            if (host_mode() == HOST_MODE_FM) {
                lakeshark_fm_tune((int)n);
                now = lakeshark_fm_get_freq();
            } else if (host_mode() == HOST_MODE_REC) {
                /*LS-510*/
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

        static const char *names[] = { "listen", "scan", "pocsag", "wfm" };
        if (!a1) {
            snprintf(reply, reply_len, "+OK fm=%s\n", names[lakeshark_fm_get_mode() & 3]);
            return;
        }
        char up[16];
        strlcpy(up, a1, sizeof(up));
        for (char *p = up; *p; p++) *p = (char)tolower((unsigned char)*p);

        int m = -1;
        for (int i = 0; i < 4; i++) if (!strcmp(up, names[i])) { m = i; break; }
        if (m < 0 && !strcmp(up, "nbfm")) m = 0;
        if (m < 0 && parse_i32(a1, &n) && n >= 0 && n <= 3) m = (int)n;
        if (m < 0) { snprintf(reply, reply_len, "-ERR fm\n"); return; }

        if (host_mode() != HOST_MODE_FM && s_host.select_mode_by_name) {
            sdr_stall_reset();
            s_host.select_mode_by_name("fm");
        }
        lakeshark_fm_set_mode(m);
        s_stat_now = true;
        snprintf(reply, reply_len, "+OK fm=%s\n", names[m]);

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
            /*LS-510*/
            if (host_mode() == HOST_MODE_REC) rec_set_gain(0);
            else                              lakeshark_p25_agc();
        } else if (!strcmp(up, "STEP")) {
            lakeshark_p25_gain_step();
        } else if (parse_i32(a1, &n)) {
            if (n < 0) n = 0;
            if (n > 496) n = 496;
            /*LS-510*/
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
        } else if (parse_i32(a1, &n) && n >= 0 && n <= 3) {
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
        /*LS-511*/
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
                     lakeshark_radio_device_ready() ? 1 : 0);
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
            snprintf(reply, reply_len, "+OK sdr power cycling\n");
            s_host.sdr_power_cycle();
        } else {
            snprintf(reply, reply_len, "+OK rtl=%d stl=%d\n",
                     lakeshark_radio_device_ready() ? 1 : 0, sdr_stall_s());
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

    } else {
        s_bad_lines++;
        snprintf(reply, reply_len, "-ERR unknown %s\n", cmd);
    }
}

int flipper_link_snapshot(char *buf, size_t len)
{
    return build_telemetry(buf, len);
}

/*LS-202*/
static const int SCAN_PINS[] = LS_BOARD_LINK_SCAN_PINS;
#define N_SCAN_PINS ((int)(sizeof(SCAN_PINS) / sizeof(SCAN_PINS[0])))

/*LS-203*/
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
        /*LS-203*/
        if (LS_BOARD_HAS_VBUS_CTRL && pin == VBUS_EN_GPIO) continue;
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

/*LS-201*/
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

esp_err_t flipper_link_start(const flipper_link_cfg_t *cfg,
                             const flipper_link_host_t *host)
{
    if (s_run) return ESP_ERR_INVALID_STATE;
    if (cfg)  s_cfg  = *cfg;
    if (host) s_host = *host;

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

    char hello[64];
    int  len = snprintf(hello, sizeof(hello), "+HELLO %d LakeShark\n",
                        FLIPPER_LINK_PROTO_VERSION);
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
