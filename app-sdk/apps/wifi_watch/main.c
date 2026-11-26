/* WIFI WATCH: watches 802.11 management traffic for deauthentication and
   disassociation bursts, beacon floods, evil twins, hidden networks being
   named, probing clients and handshakes going past. Receive only on Wi-Fi:
   it never transmits, joins or deauthenticates there, and handshakes are a
   seen/not-seen mark, never key material.

   Over lap_wifi_capture_* (API 2.7). Where the board cannot capture (the
   T-Display's C6 without the forwarder firmware) it says why and how to fix
   it. Alerts go to /sdcard/wifi_watch as CSV, and, as the owner chooses in
   OPTIONS, to the speaker, the vibration motor, the voice, the board's
   banner and the LoRa mesh (off until turned on; rate limited per kind and
   per hour, repeats folded into one "x12" notice). */
#include "watch.h"
#include "lakeshark_ui.h"
#include "lap_scroll.h"
#include "lap_art.h"

LAP_ICON_PX("....................", "....................", ".......aaaaaa.......", ".....aa......aa.....",
            "....a..bbbbbb..a....", "......b......b......", ".....b..cccc..b.....", ".......c....c.......",
            ".........ee.........", "........eeee........", "........eeee........", ".........ee.........",
            "...........f........", "............f.......", ".............f......", "..............f.....",
            "....................", "....................");

static watch_t *w;
static int W, H, tab, scroll, smax, clip0, clip1, kr, tr, ox, ty;   /* ox: the panel's left edge, added by put() */
static bool desk, paused, logging, dragging;
static lap_scroll_t drag;
static uint32_t lost, seen_frames, status_ms;
static char status[96];
static bool status_bad;

/* ---- capture ---------------------------------------------------------------- */
enum { PLAN_STAY, PLAN_HOP3, PLAN_HOP13 };
static int h, reason, plan, chan;
static bool drop_link, fell_back;
static char why[72];
static int choose_h, confirm_h;
/* activity: frames a second per channel (for the VFD bars) and overall (the history) */
#define HIST 48
static uint32_t ch_hits[15], ch_rate[15], ch_peak[15], sec_frames, sec_ms;
static float fps_hist[HIST];
static int fps_n;

static void say(bool bad, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof(status), fmt, ap);
    va_end(ap);
    status_bad = bad;
    status_ms = lap_millis();
}
static void start(void)
{
    if (h > 0) lap_capture_stop(LAP_CAPTURE_WIFI);
    h = 0; why[0] = 0; reason = 0; fell_back = false;
    for (int attempt = 0; attempt < 2; attempt++) {
        lap_wifi_capture_config_t c = {.size = sizeof(c), .version = 1, .flags = drop_link ? LAP_CAPTURE_DROP_LINK : 0,
                                       .types = LAP_WIFI_MGMT | LAP_WIFI_DATA, .dwell_ms = 300};
        const int p = attempt ? PLAN_STAY : plan;
        if (p == PLAN_HOP3) { c.channel_count = 3; c.channels[0] = 1; c.channels[1] = 6; c.channels[2] = 11; }
        if (p == PLAN_HOP13) { c.dwell_ms = 250; for (; c.channel_count < 13; c.channel_count++) c.channels[c.channel_count] = (uint8_t)(c.channel_count + 1); }
        const int r = lap_wifi_capture_start(&c);
        lap_capture_status_t st;
        if (!lap_capture_status(LAP_CAPTURE_WIFI, &st)) { reason = st.reason; snprintf(why, sizeof(why), "%s", st.why); }
        if (r > 0) { h = r; fell_back = attempt == 1; return; }
        if (!(r == LAP_IO_EBUSY && reason == LAP_CAPTURE_LINK_IN_USE)) return;   /* only the link guard has a fallback */
    }
}
static void poll(void)
{
    static uint8_t buf[LAP_CAPTURE_SNAP_MAX];
    if (h <= 0) return;
    lap_capture_meta_t m;
    const uint32_t began = lap_millis();
    for (int i = 0, n; i < 256 && lap_millis() - began < 12u && (n = lap_capture_read(h, buf, sizeof(buf), &m)) >= 0; i++) {
        lost = m.dropped;
        if (m.channel) chan = m.channel;
        seen_frames++;
        sec_frames++;
        const int fc = m.channel ? m.channel : chan;      /* the radio may not say; then the channel it is on */
        if (fc > 0 && fc < 15) ch_hits[fc]++;
        if (!paused) watch_frame(w, buf, (size_t)n, m.rssi, m.channel, lap_millis());
    }
    if (!(lap_frame() % 25)) {
        lap_capture_status_t st;
        if (!lap_capture_status(LAP_CAPTURE_WIFI, &st)) {
            lost = st.dropped; reason = st.reason;
            if (st.channel) chan = st.channel;
            snprintf(why, sizeof(why), "%s", st.why);
        }
    }
}

/* ---- the log on the card -------------------------------------------------------- */
static char logpath[64], logq[4096];
static size_t logn;
static int logjob = -1, logstage;       /* 0 nothing yet, 1 folder asked, 2 writing */
static uint32_t logged, logdrop, logsent;
static lap_time_t t_open;

/* Local wall time of an alert, from the clock when the app opened. */
static void clock_of(const watch_alert_t *a, char *out, size_t cap, bool date)
{
    const int64_t d = (int64_t)(a->ms / 1000u);        /* lap_millis counts from the open, as t_open does */
    const long s = (long)(((int64_t)t_open.hour * 3600 + t_open.minute * 60 + t_open.second + d) % 86400);
    if (date) snprintf(out, cap, "%04d-%02d-%02d %02ld:%02ld:%02ld", t_open.year, t_open.month, t_open.day, s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, cap, "%02ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
}
static void log_alerts(void)
{
    static const char *const sev[3] = {"info", "warn", "alert"};
    while (logging && logged < w->nalerts) {
        if (w->nalerts - logged > W_ALERTS) { logdrop += w->nalerts - logged - W_ALERTS; logged = w->nalerts - W_ALERTS; }
        const watch_alert_t *a = &w->alert[logged % W_ALERTS];
        char when[24], line[256], text[110];
        size_t k = 0;
        for (const char *c = a->text; *c && k + 2 < sizeof(text); c++) { if (*c == '"') text[k++] = '"'; text[k++] = *c; }
        text[k] = 0;
        clock_of(a, when, sizeof(when), true);
        const int n = snprintf(line, sizeof(line), "%s,%s,%s,%u,%s,%s,\"%s\"\r\n", when, sev[a->sev], watch_kind_name[a->kind], a->channel, a->who, a->whom, text);
        if (n <= 0 || logn + (size_t)n >= sizeof(logq)) break;
        memcpy(logq + logn, line, (size_t)n);
        logn += (size_t)n;
        logged++;
    }
    if (logjob >= 0) {
        const int r = lap_sd_poll(logjob);
        if (r == LAP_SD_PENDING) return;
        logjob = -1;
        if (r < 0 && logstage == 2) { say(true, "log write failed (%d): is there a card?", r); logging = false; return; }
    }
    if (!logn) return;
    if (!logstage) {
        snprintf(logpath, sizeof(logpath), "/sdcard/wifi_watch/alerts-%04d%02d%02d-%02d%02d%02d.csv", t_open.year, t_open.month, t_open.day,
                 t_open.hour, t_open.minute, t_open.second);
        logjob = lap_sd_mkdir("/sdcard/wifi_watch");
        logstage = 1;
        return;
    }
    if (logstage == 1) {
        static const char head[] = "time,severity,kind,channel,source,target,detail\r\n";
        if (logn + sizeof(head) - 1 < sizeof(logq)) { memmove(logq + sizeof(head) - 1, logq, logn); memcpy(logq, head, sizeof(head) - 1); logn += sizeof(head) - 1; }
        logjob = lap_sd_write(logpath, 0, logq, (uint32_t)logn);
        logstage = 2;
    } else logjob = lap_sd_write(logpath, LAP_SD_APPEND, logq, (uint32_t)logn);
    if (logjob < 0) { say(true, "log write refused (%d)", logjob); logging = false; return; }
    logsent += (uint32_t)logn;
    logn = 0;
}

/* ---- alert delivery: sound, vibration, voice, banner, LoRa mesh ---------------- */
static watch_prefs_t prefs;
static watch_relay_t relay;
static uint32_t delivered;              /* alerts deliver() has seen */
static uint32_t local_ms, say_ms;       /* the last sound or buzz, the last spoken alert */
enum { MOTOR_UNKNOWN, MOTOR_YES, MOTOR_NO };
static int motor;                       /* learnt from lap_haptic / lap_vibrate */
static bool muted, quiet, tx_granted, me_ok;
static lap_mesh_self_t me;
static uint32_t board_ms;               /* when muted, the mesh and the grant were last read */
/* the last notice handed to the mesh, and what became of it */
static char mesh_last[160], mesh_dest[32];
static uint32_t mesh_last_ms, mesh_sent;
static int mesh_result;                 /* lap_mesh_send_state: 1 waiting, 0 handed over, <0 refused */
static bool mesh_waiting, mesh_any;
static char test_line[160];
static uint32_t test_ms, test_mesh_ms;
static bool pick_wanted, picking, reopen;  /* the node list: asked for, up, OPTIONS to come back */
enum { D_BANNER = 1, D_BOARD = 2, D_SOUND = 4, D_VIBRATE = 8, D_NOMOTOR = 16, D_SPEECH = 32 };

static void prefs_save(void)
{
    const int fd = lap_file_open("alerts.cfg", "w");
    if (fd < 0) { say(true, "alert options not saved (%d)", fd); return; }
    lap_file_write(fd, &prefs, sizeof(prefs));
    lap_file_close(fd);
}
static void prefs_read(void)
{
    watch_prefs_t buf;
    int n = -1;
    const int fd = lap_file_open("alerts.cfg", "r");
    if (fd >= 0) { n = lap_file_read(fd, &buf, sizeof(buf)); lap_file_close(fd); }
    watch_prefs_load(&prefs, &buf, n == (int)sizeof(buf) ? sizeof(buf) : 0);
}
static void relay_reset(void) { watch_relay_init(&relay, prefs.mesh_min_sev, prefs.mesh_gap_min, prefs.mesh_per_hour); }

/* Where notices go, in words: "CH1 ops", "CH0 public", "DM KD9XYZ". */
static void dest_name(char *o, size_t cap, int to)
{
    char name[16] = "";
    if (to == MESH_OFF) snprintf(o, cap, "off");
    else if (to == MESH_CH0) snprintf(o, cap, "CH0 public");
    else if (to == MESH_DM) snprintf(o, cap, "DM %s", prefs.dm_name[0] ? prefs.dm_name : prefs.dm_id[0] ? prefs.dm_id : "(no node)");
    else {
        const bool keyed = lap_mesh_channel_private(to - MESH_CH0, name, sizeof(name));
        snprintf(o, cap, "CH%d%s%s%s", to - MESH_CH0, name[0] ? " " : "", name, keyed ? "" : " (open)");
    }
}
static const char *refusal(int r)
{
    return r == -1 ? "TRANSMIT is off in MESH" : r == -2 ? "the mesh was busy" : r == -3 ? "too long" : r == -4 ? "no mesh" : "refused";
}
/* Why no notice can go now, or NULL when one can. */
static const char *mesh_blocked(void)
{
    if (prefs.mesh_to == MESH_OFF) return "LoRa notices are off";
    if (!tx_granted) return "WIFI WATCH may not transmit: reinstall it and allow RADIO TX";
    if (!me_ok || !me.running) return "no LoRa mesh: the board has no LoRa radio, or MESH is not running";
    if (!me.tx_armed) return "TRANSMIT is off in MESH: arm it there to send";
    if (prefs.mesh_to == MESH_DM && !prefs.dm_id[0]) return "no node chosen: OPTIONS > LORA MESH > PICK NODE";
    return NULL;
}
/* The board's state, once a second: mute, the mesh, the grant. */
static void board_poll(void)
{
    const uint32_t now = lap_millis();
    if (board_ms && now - board_ms < 1000u) return;
    board_ms = now ? now : 1;
    lap_audio_status_t a;
    muted = !lap_audio_status(&a) && a.muted;
    /* asking keeps the board refreshing its copy of the mesh: only while it is wanted */
    if (prefs.mesh_to != MESH_OFF || lap_options_showing() || picking) me_ok = lap_mesh_self(&me);
    lap_caps_t c;
    memset(&c, 0, sizeof(c));
    c.size = sizeof(c); c.version = 1;
    tx_granted = !lap_caps(&c, sizeof(c)) && (c.granted & LAP_CAP_RADIO_TX);
}
static void hhmm_of(const watch_alert_t *a, char *o)
{
    char t[12];
    clock_of(a, t, sizeof(t), false);
    snprintf(o, 6, "%.5s", t);
}
/* Hand one notice to the mesh; true when it took it. */
static bool mesh_out(const char *text)
{
    const bool ok = prefs.mesh_to == MESH_DM ? lap_mesh_dm(prefs.dm_id, text) : lap_mesh_send(prefs.mesh_to - MESH_CH0, text);
    if (!ok) return false;
    dest_name(mesh_dest, sizeof(mesh_dest), prefs.mesh_to);
    snprintf(mesh_last, sizeof(mesh_last), "%s", text);
    mesh_last_ms = lap_millis();
    mesh_waiting = mesh_any = true;
    mesh_result = 1;
    mesh_sent++;
    lap_log("mesh %s: %s", mesh_dest, text);
    return true;
}
static void mesh_pump(void)
{
    const uint32_t now = lap_millis();
    if (mesh_waiting) {
        const int r = lap_mesh_send_state();
        if (r != 1) {
            mesh_waiting = false;
            mesh_result = r;
            if (r < 0) { say(true, "mesh refused the notice: %s", refusal(r)); lap_log("mesh refused: %s", refusal(r)); }
        }
    }
    if (prefs.mesh_to == MESH_OFF || mesh_waiting || (quiet && !prefs.quiet_mesh) || mesh_blocked()) return;
    const int k = watch_relay_next(&relay, now);
    if (k < 0) return;
    char text[160], hm[8];
    hhmm_of(&relay.k[k].latest, hm);
    watch_mesh_text(text, sizeof(text), &relay.k[k].latest, NULL, relay.k[k].pending, hm);
    if (mesh_out(text)) watch_relay_sent(&relay, k, now);
}
static void speech(const watch_alert_t *a, char *o, size_t cap)
{
    static const char *const words[K_COUNT] = {"deauthentication burst", "disassociation burst", "beacon flood", "evil twin",
                                               "hidden network named", "handshake seen", "PMKID seen"};
    if (a->kind >= K_COUNT) snprintf(o, cap, "Wi-Fi watch test alert");
    else if (a->channel) snprintf(o, cap, "Wi-Fi watch: %s, channel %u", words[a->kind], a->channel);
    else snprintf(o, cap, "Wi-Fi watch: %s", words[a->kind]);
}
/* The board-side outputs for one alert. A test ignores quiet hours and the
   pacing (one sound or buzz every 3 s, one spoken alert every 5 s). When the
   board's own alert banner went up, it rang and buzzed with the board's ring
   and vibrate settings, so WIFI WATCH does not buzz a second time. */
static int local(const watch_alert_t *a, bool test)
{
    const uint32_t now = lap_millis();
    if (!test && (quiet || (local_ms && now - local_ms < 3000u))) return 0;
    const int sev = a->sev <= SEV_ALERT ? a->sev : SEV_ALERT, choice = prefs.out[sev];
    int did = 0;
    if (prefs.banner != BANNER_OFF && (choice != OUT_NONE || test)) {
        char title[24], body[110];
        snprintf(title, sizeof(title), "%s", a->kind < K_COUNT ? watch_kind_name[a->kind] : "TEST");
        snprintf(body, sizeof(body), "%s", a->text);
        if (prefs.banner == BANNER_BOARD && lap_alert(title, body, sev == SEV_ALERT ? LAP_ALERT_ALARM : sev == SEV_WARN ? LAP_ALERT_WARN : LAP_ALERT_INFO))
            did |= D_BANNER | D_BOARD;
        else if (lap_notify(title, body)) did |= D_BANNER;     /* QUIET, or BOARD inside its 5 s limit */
    }
    if (!(did & D_BOARD)) {
        if (choice == OUT_SOUND || choice == OUT_BOTH) {
            lap_sound(sev == SEV_ALERT ? LAP_SOUND_ERROR : sev == SEV_WARN ? LAP_SOUND_CHIRP : LAP_SOUND_ROGER);
            did |= D_SOUND;
        }
        if (choice == OUT_VIBRATE || choice == OUT_BOTH) {
            const bool ok = lap_vibrate(sev == SEV_ALERT ? 600 : sev == SEV_WARN ? 300 : 120, sev == SEV_ALERT ? 90 : 70);
            motor = ok ? MOTOR_YES : MOTOR_NO;
            did |= ok ? D_VIBRATE : D_NOMOTOR;
        }
    }
    if (prefs.speak && !muted && (choice != OUT_NONE || test) && (test || !say_ms || now - say_ms >= 5000u)) {   /* muted: no voice either */
        char words[80];
        speech(a, words, sizeof(words));
        if (lap_say(words)) { did |= D_SPEECH; say_ms = now ? now : 1; }
    }
    if (did & (D_SOUND | D_VIBRATE | D_BOARD)) local_ms = now ? now : 1;
    return did;
}
/* New alerts: each to the mesh relay; the most severe of them to the board's outputs. */
static void deliver(void)
{
    if (delivered > w->nalerts) delivered = 0;                 /* cleared */
    if (w->nalerts - delivered > W_ALERTS) delivered = w->nalerts - W_ALERTS;
    const watch_alert_t *top = NULL;
    for (; delivered < w->nalerts; delivered++) {
        const watch_alert_t *a = &w->alert[delivered % W_ALERTS];
        if (prefs.mesh_to != MESH_OFF) watch_relay_note(&relay, a);
        if (!top || a->sev > top->sev) top = a;
    }
    if (top) local(top, false);
}
/* TEST ALERT: the chosen ALERT outputs, the voice, the banner and the mesh, once. */
static void test_alert(void)
{
    watch_alert_t a;
    memset(&a, 0, sizeof(a));
    a.ms = lap_millis(); a.unix = w ? w->unix_now : 0;
    a.sev = SEV_ALERT; a.kind = K_COUNT; a.channel = (uint8_t)(chan > 0 && chan < 15 ? chan : 0);
    snprintf(a.text, sizeof(a.text), "Test alert: this is what an alert does");
    board_poll();
    const int did = local(&a, true);
    const int choice = prefs.out[SEV_ALERT];
    char snd[40], vib[24], msh[64];
    if (did & D_BOARD) snprintf(snd, sizeof(snd), "board alert rang");
    else if (choice == OUT_SOUND || choice == OUT_BOTH) snprintf(snd, sizeof(snd), "%s", muted ? "sound (board muted)" : "sound");
    else snprintf(snd, sizeof(snd), "no sound");
    if (did & D_BOARD) vib[0] = 0;
    else snprintf(vib, sizeof(vib), ", %s", did & D_VIBRATE ? "vibrate" : did & D_NOMOTOR ? "no motor" : "no vibrate");
    if (prefs.mesh_to == MESH_OFF) snprintf(msh, sizeof(msh), "LoRa off");
    else if (mesh_blocked()) snprintf(msh, sizeof(msh), "LoRa: %.50s", mesh_blocked());
    else if (mesh_waiting || (test_mesh_ms && lap_millis() - test_mesh_ms < 10000u)) snprintf(msh, sizeof(msh), "LoRa: busy, try again in 10 s");
    else {
        char text[160], hm[8];
        hhmm_of(&a, hm);
        watch_mesh_text(text, sizeof(text), &a, "TEST", 1, hm);
        if (mesh_out(text)) { snprintf(msh, sizeof(msh), "LoRa: sent to %s", mesh_dest); test_mesh_ms = lap_millis() | 1u; }
        else snprintf(msh, sizeof(msh), "LoRa: the mesh did not take it");
    }
    snprintf(test_line, sizeof(test_line), "%s%s%s%s, %s", snd, vib, did & D_SPEECH ? ", spoken" : "", did & D_BANNER && !(did & D_BOARD) ? ", banner" : "", msh);
    test_ms = lap_millis();
    say(false, "TEST: %s", test_line);
    lap_log("test: %s", test_line);
}

/* ---- drawing helpers ------------------------------------------------------------ */
static void put(int x, int y, int wd, uint8_t attr, const char *fmt, ...)
{
    char t[200];
    va_list ap;
    if (wd <= 0 || y < 0 || y >= H || x >= W) return;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    if (x + wd > W) wd = W - x;
    if ((size_t)wd < sizeof(t)) t[wd] = 0;
    lap_put_str(x + ox, y, t, attr);
}
/* Words across wd columns from x at *y, drawn between clip0 and clip1. */
static void wrap(int x, int *y, int wd, uint8_t attr, const char *t)
{
    if (wd < 8) wd = 8;
    while (*t) {
        while (*t == ' ') t++;
        if (!*t) break;
        int n = (int)strlen(t);
        if (n > wd) { n = wd; while (n > 0 && t[n] != ' ') n--; if (!n) n = wd; }
        if (*y >= clip0 && *y < clip1) put(x, *y, wd, attr, "%.*s", n, t);
        (*y)++;
        t += n;
    }
}
static void row(int *y, int x, uint8_t attr, const char *fmt, ...)
{
    char t[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    if (*y >= clip0 && *y < clip1) put(x, *y, W - x, attr, "%s", t);
    (*y)++;
}
static void mac(char *o, const uint8_t *a) { lap_dot11_mac(o, 18, a); }
static const char *ago(char *b, size_t n, uint32_t ms)
{
    const uint32_t s = (lap_millis() - ms) / 1000u;
    if (s < 60) snprintf(b, n, "%us", (unsigned)s);
    else if (s < 3600) snprintf(b, n, "%um", (unsigned)(s / 60));
    else snprintf(b, n, "%uh", (unsigned)(s / 3600));
    return b;
}

/* ---- screens ---------------------------------------------------------------------- */
enum { T_ALERTS, T_COUNT_, T_KICK, T_NETS, T_PROBES, T_SHAKES, TABS };
static const char *const tabs[TABS] = {"ALERTS", "COUNTERS", "DEAUTH", "NETWORKS", "PROBES", "HANDSHAKES"};
static const uint8_t sev_attr[3] = {LAP_ATTR(LAP_CYAN, LAP_BLACK), LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, LAP_BLACK), LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK)};
/* Each kind of alert has its own small mark: a kick, a cut, a flood of
   beacons, a pair of twins, a mask coming off, a handshake, a key. */
static const char *const kind_glyph[K_COUNT] = {"-x>", "-/>", ")))", "<=>", "(?)", "o~o", "[k]"};
/* A new alert flashes in the alarm colour for its first six seconds. */
static uint8_t alert_attr(const watch_alert_t *a)
{
    const uint32_t age = lap_millis() - a->ms;
    if (a->sev && age < 6000u && (lap_millis() / 250u) % 2u) return LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_RED);
    return sev_attr[a->sev];
}
/* The quiet screen: a shield with a scan line passing down it. */
static void shield(int *y, const char *say1)
{
    static const char *const art[5] = {" .-------. ", "|  .---.  |", "|  | v |  |", " \\ '---' / ", "  '-._.-'  "};
    const int x = (W - 11) / 2, scan = (int)(lap_millis() / 300u % 4u);
    for (int i = 0; i < 5; i++, (*y)++)
        if (*y >= clip0 && *y < clip1) {
            put(x, *y, 12, LAP_ATTR(LAP_CYAN, LAP_BLACK), "%s", art[i]);
            if (i == 2) put(x + 5, *y, 1, LAP_ATTR(LAP_GREEN | LAP_BRIGHT, LAP_BLACK), "v");
            if (i == scan + 1 && i < 4 && h > 0 && !paused) put(x + 2, *y, 7, LAP_ATTR(LAP_GREEN, LAP_BLACK), i == 2 ? "-  v  -" : "-------");
        }
    (*y)++;
    if (*y >= clip0 && *y < clip1) put((W - (int)strlen(say1)) / 2, *y, W, LAP_UI_TEXT, "%s", say1);
    (*y) += 2;
}

/* Not listening: the reason, and what to do about it. */
static void not_listening(int *y)
{
    const uint8_t warn = LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, LAP_BLACK), tip = LAP_ATTR(LAP_CYAN, LAP_BLACK);
    if (h > 0 && !fell_back) return;
    if (h > 0) {
        wrap(1, y, W - 2, warn, "The board is on a Wi-Fi network: watching its channel only. MORE > ALLOW LINK DROP to hop.");
        (*y)++;
        return;
    }
    row(y, 1, warn, "NOT LISTENING");
    wrap(1, y, W - 2, LAP_ATTR(LAP_WHITE, LAP_BLACK), why[0] ? why : "Wi-Fi capture did not start");
    if (reason == LAP_CAPTURE_NO_TRANSPORT) {
        wrap(1, y, W - 2, tip, "Wi-Fi capture needs the C6 monitor firmware. The board installs it by itself a minute after it boots on Wi-Fi, or now from:");
        row(y, 3, LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLACK), "SYSTEM > UPDATE > C6 RADIO FW > UPDATE");
    } else if (reason == LAP_CAPTURE_LINK_IN_USE) {
        wrap(1, y, W - 2, tip, "The board is on a Wi-Fi network and hopping would drop it. MORE > ALLOW LINK DROP, or stay on its channel.");
    } else if (reason == LAP_CAPTURE_BUSY) {
        wrap(1, y, W - 2, tip, "Another app holds the Wi-Fi capture. Close it and choose MORE > RESTART.");
    } else if (reason == LAP_CAPTURE_NO_RADIO) {
        wrap(1, y, W - 2, tip, "This board has no Wi-Fi radio, or it is switched off.");
    }
    (*y)++;
}
static void alerts_tab(int y)
{
    not_listening(&y);
    const uint32_t kept = watch_alerts_kept(w);
    if (!kept) {
        y++;
        shield(&y, h > 0 ? (paused ? "paused" : "all quiet") : "no alerts");
        wrap(1, &y, W - 2, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), h > 0 ? "No alerts. Watching for deauthentication and disassociation bursts, beacon floods, evil twins, hidden networks being named and handshakes." : "No alerts.");
        y++;
        for (int k = 0; k < K_COUNT; k++) row(&y, 3, LAP_ATTR(LAP_CYAN, LAP_BLACK), "%s  %s", kind_glyph[k], watch_kind_name[k]);
        smax = y + scroll - (clip1);
        if (smax < 0) smax = 0;
        return;
    }
    const int first = y;
    y -= scroll;
    for (uint32_t i = 0; i < kept; i++) {
        const watch_alert_t *a = watch_alert(w, i);
        char when[12];
        clock_of(a, when, sizeof(when), false);
        static const char *const sev[3] = {"info ", "WARN ", "ALERT"};
        const char *g = a->kind < K_COUNT ? kind_glyph[a->kind] : "   ";
        if (desk) {
            row(&y, 0, alert_attr(a), "%s %s %s %-9s ch%-3u %-17s %s", g, when, sev[a->sev], watch_kind_name[a->kind], a->channel, a->who, a->text);
        } else {
            row(&y, 0, alert_attr(a), "%s %s %s %-9s ch%-3u", g, when, sev[a->sev], watch_kind_name[a->kind], a->channel);
            if (a->who[0]) row(&y, 4, LAP_ATTR(LAP_WHITE, LAP_BLACK), "%s%s%s", a->who, a->whom[0] ? " > " : "", a->whom);
            wrap(4, &y, W - 4, LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLACK), a->text);
        }
    }
    if (w->nalerts > kept) row(&y, 0, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "%u older alerts are in the log only", (unsigned)(w->nalerts - kept));
    smax = y + scroll - first - (clip1 - first);
}
/* Where alerts go: the LoRa mesh and what became of the last notice, the
   speaker, the motor, the voice, the banner, quiet hours, the last test. */
static void delivery(int *y)
{
    static const char *const out_word[OUT_COUNT] = {"none", "sound", "vibrate", "both"};
    static const char *const sev_word[3] = {"info and up", "warn and up", "alerts only"};
    const uint8_t hd = LAP_ATTR(LAP_YELLOW, LAP_BLACK), k = LAP_UI_DIM, v = LAP_UI_TEXT, bad = LAP_ATTR(LAP_UI_ALARM, LAP_BLACK),
                  warn = LAP_ATTR(LAP_UI_WARN, LAP_BLACK);
    const int vx = 8, vw = W - vx - 1;             /* the value column */
    char t[200], d[32], a[8];
    row(y, 0, hd, "DELIVERY  MORE > ALERT OPTIONS");
    if (prefs.mesh_to == MESH_OFF) { row(y, 1, k, "LoRa"); (*y)--; wrap(vx, y, vw, k, "off: nothing is sent"); }
    else {
        dest_name(d, sizeof(d), prefs.mesh_to);
        row(y, 1, k, "LoRa"); (*y)--;
        row(y, vx, v, "%s", d);
        snprintf(t, sizeof(t), "%s, 1 per kind / %u min, max %u/h", sev_word[prefs.mesh_min_sev], prefs.mesh_gap_min, prefs.mesh_per_hour);
        wrap(vx, y, vw, k, t);
        const char *why = mesh_blocked();
        if (why) wrap(vx, y, vw, bad, why);
        else if (quiet && !prefs.quiet_mesh) wrap(vx, y, vw, warn, "quiet hours: held until they end");
    }
    if (mesh_any) {
        const char *st = mesh_result == 1 ? "waiting for the mesh" : mesh_result == 0 ? "handed to the mesh" : refusal(mesh_result);
        row(y, 1, k, "last"); (*y)--;
        snprintf(t, sizeof(t), "%s ago to %s: %s", ago(a, sizeof(a), mesh_last_ms), mesh_dest, st);
        wrap(vx, y, vw, mesh_result < 0 ? bad : v, t);
        wrap(vx, y, vw, k, mesh_last);
    }
    if (prefs.mesh_to != MESH_OFF || mesh_sent) {
        const uint32_t held = watch_relay_waiting(&relay), wait = watch_relay_wait_ms(&relay, lap_millis());
        if (held) {
            row(y, 1, k, "held"); (*y)--;
            snprintf(t, sizeof(t), "%u alert%s, next notice in %u min", (unsigned)held, held == 1 ? "" : "s", (unsigned)((wait + 59999u) / 60000u));
            wrap(vx, y, vw, warn, t);
        }
        row(y, 1, k, "sent"); (*y)--;
        snprintf(t, sizeof(t), "%u notice%s, %u repeats folded in", (unsigned)mesh_sent, mesh_sent == 1 ? "" : "s", (unsigned)relay.coalesced);
        wrap(vx, y, vw, k, t);
    }
    row(y, 1, k, "Sound"); (*y)--;
    snprintf(t, sizeof(t), "alert %s, warn %s, info %s", out_word[prefs.out[SEV_ALERT]], out_word[prefs.out[SEV_WARN]], out_word[prefs.out[SEV_INFO]]);
    wrap(vx, y, vw, v, t);
    if (muted) wrap(vx, y, vw, warn, "the board is muted: no sound or voice");
    row(y, 1, k, "Motor"); (*y)--;
    wrap(vx, y, vw, motor == MOTOR_NO ? warn : k, motor == MOTOR_NO ? "no motor: nothing vibrates" : motor == MOTOR_YES ? "yes" : "not tried yet");
    row(y, 1, k, "Voice"); (*y)--;
    snprintf(t, sizeof(t), "%s; banner %s", prefs.speak ? "on" : "off",
             prefs.banner == BANNER_BOARD ? "board alert (its ring and vibrate)" : prefs.banner == BANNER_QUIET ? "quiet" : "off");
    wrap(vx, y, vw, k, t);
    if (prefs.quiet_on) {
        row(y, 1, k, "Quiet"); (*y)--;
        snprintf(t, sizeof(t), "%02u:00-%02u:00%s", prefs.quiet_from, prefs.quiet_until, quiet ? ", quiet now" : "");
        wrap(vx, y, vw, quiet ? LAP_ATTR(LAP_BLUE | LAP_BRIGHT, LAP_BLACK) : k, t);
    }
    if (test_line[0]) {
        row(y, 1, k, "Test"); (*y)--;
        snprintf(t, sizeof(t), "%s ago: %s", ago(a, sizeof(a), test_ms), test_line);
        wrap(vx, y, vw, k, t);
    }
}
static void counters_tab(int y)
{
    not_listening(&y);
    const int first = y;
    y -= scroll;
    const uint8_t k = LAP_ATTR(LAP_WHITE, LAP_BLACK), v = LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLACK), hd = LAP_ATTR(LAP_YELLOW, LAP_BLACK);
    const struct { const char *name; uint32_t n; } c[] = {
        {"Frames", w->frames}, {"Management", w->mgmt}, {"Data", w->data}, {"Control", w->ctrl},
        {"Beacons", w->beacons}, {"Probe requests", w->probes}, {"Probe responses", w->proberesp},
        {"Deauthentication", w->deauth}, {"Disassociation", w->disassoc}, {"Authentication", w->auth},
        {"Association", w->assoc}, {"EAPOL", w->eapol}, {"Protected mgmt (11w)", w->protected_mgmt},
        {"Dropped by the radio", lost}, {"Networks (BSSIDs)", (uint32_t)w->naps + w->aps_over}, {"Probing clients", (uint32_t)w->ncl + w->clients_over}};
    const int cols = desk ? 2 : 1, colw = W / cols, n = (int)(sizeof(c) / sizeof(c[0]));
    row(&y, 0, hd, "TRAFFIC");
    for (int i = 0; i < n; i += cols, y++)
        for (int j = 0; j < cols && i + j < n; j++)
            if (y >= clip0 && y < clip1) {
                put(j * colw + 1, y, colw - 12, c[i + j].name[0] == 'D' && i + j == 13 && lost ? LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK) : k, "%s", c[i + j].name);
                put(j * colw + colw - 11, y, 10, v, "%10u", (unsigned)c[i + j].n);
            }
    y++;
    row(&y, 0, hd, "ALERTS");
    for (int i = 0; i < K_COUNT; i += cols, y++)
        for (int j = 0; j < cols && i + j < K_COUNT; j++)
            if (y >= clip0 && y < clip1) {
                put(j * colw + 1, y, colw - 12, k, "%s", watch_kind_name[i + j]);
                put(j * colw + colw - 11, y, 10, w->by_kind[i + j] ? LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK) : v, "%10u", (unsigned)w->by_kind[i + j]);
            }
    y++;
    /* each channel's frames a second as a VFD bar, its peak held */
    row(&y, 0, hd, "CHANNELS  frames/s");
    uint32_t top = 10;
    for (int c = 1; c < 15; c++) if (ch_peak[c] > top) top = ch_peak[c];
    for (int c = 1; c < 14; c++, y++)
        if (y >= clip0 && y < clip1) {
            put(1, y, 5, c == chan ? LAP_ATTR(LAP_GREEN | LAP_BRIGHT, LAP_BLACK) : k, "%s%2d", c == chan ? ">" : " ", c);
            const int bw = W - 14;
            if (bw > 4) lap_ui_bargraph(ox + 6, y, bw, 1, (float)ch_rate[c] / (float)top, (float)ch_peak[c] / (float)top);
            put(W - 7, y, 6, v, "%6u", (unsigned)ch_rate[c]);
        }
    y++;
    if (logging) row(&y, 1, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "log: %s", logpath[0] ? logpath + 8 : "on the first alert, in /sdcard/wifi_watch");
    else row(&y, 1, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "log to card is off");
    if (logdrop) row(&y, 1, LAP_ATTR(LAP_RED, LAP_BLACK), "%u alerts came too fast to log", (unsigned)logdrop);
    y++;
    delivery(&y);
    smax = y + scroll - first - (clip1 - first);
}
static void kick_tab(int y)
{
    const int first = y;
    y -= scroll;
    if (!w->npairs) wrap(1, &y, W - 2, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "No deauthentication or disassociation frames seen.");
    else if (desk) row(&y, 0, LAP_ATTR(LAP_YELLOW, LAP_BLACK), "%-8s %-17s    %-17s %7s %6s %4s %-24s %5s", "Kind", "Source", "Target", "Frames", "Rate/s", "Ch", "Reason", "Last");
    for (int i = 0; i < w->npairs; i++) {
        const watch_pair_t *p = &w->pair[i];
        char s[18], d[18], l[8];
        mac(s, p->src);
        if (lap_dot11_is_broadcast(p->dst)) snprintf(d, sizeof(d), "broadcast"); else mac(d, p->dst);
        const bool hot = p->said_ms && lap_millis() - p->said_ms < 30000u;
        const uint8_t a = hot ? LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK) : LAP_ATTR(LAP_WHITE, LAP_BLACK);
        if (desk) row(&y, 0, a, "%-8s %-17s -> %-17s %7u %6u %4u %3u %-20.20s %5s", p->disassoc ? "DISASSOC" : "DEAUTH", s, d, (unsigned)p->total,
                      (unsigned)p->rate, p->channel, p->reason, lap_dot11_reason_name(p->reason), ago(l, sizeof(l), p->last_ms));
        else {
            row(&y, 0, a, "%-8s %6u frames %4u/s ch%-3u %4s", p->disassoc ? "DISASSOC" : "DEAUTH", (unsigned)p->total, (unsigned)p->rate, p->channel, ago(l, sizeof(l), p->last_ms));
            row(&y, 2, LAP_ATTR(LAP_WHITE, LAP_BLACK), "%s > %s", s, d);
            row(&y, 2, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "reason %u: %s", p->reason, lap_dot11_reason_name(p->reason));
        }
    }
    smax = y + scroll - first - (clip1 - first);
}
static void nets_tab(int y)
{
    const int first = y;
    y -= scroll;
    if (!w->naps) wrap(1, &y, W - 2, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "No beacons heard yet.");
    else if (desk) row(&y, 0, LAP_ATTR(LAP_YELLOW, LAP_BLACK), "%-32s %-17s %4s %-8s %4s %7s %-12s", "SSID", "BSSID", "Ch", "Security", "dBm", "Beacons", "Notes");
    for (int i = 0; i < w->naps; i++) {
        const watch_ap_t *a = &w->ap[i];
        char b[18], name[40], notes[24];
        mac(b, a->bssid);
        snprintf(name, sizeof(name), "%s", a->ssid[0] ? a->ssid : "<hidden>");
        snprintf(notes, sizeof(notes), "%s%s%s", a->hidden ? (a->revealed ? "named " : "hidden ") : "", a->twin_said ? "twin " : "", a->changed_said ? "changed" : "");
        const uint8_t at = a->twin_said || a->changed_said ? LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK) : a->hidden ? LAP_ATTR(LAP_MAGENTA | LAP_BRIGHT, LAP_BLACK) :
                           a->sec == LAP_DOT11_SEC_OPEN ? LAP_ATTR(LAP_YELLOW, LAP_BLACK) : LAP_ATTR(LAP_WHITE, LAP_BLACK);
        if (desk) row(&y, 0, at, "%-32.32s %-17s %4u %-8s %4d %7u %s", name, b, a->channel, lap_dot11_sec_name(a->sec), a->rssi, (unsigned)a->beacons, notes);
        else {
            row(&y, 0, at, "%-24.24s %-8s ch%-3u %4d", name, lap_dot11_sec_name(a->sec), a->channel, a->rssi);
            row(&y, 2, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "%s %s", b, notes);
        }
    }
    if (w->aps_over) row(&y, 0, LAP_ATTR(LAP_YELLOW, LAP_BLACK), "%u more were heard; the oldest made room", (unsigned)w->aps_over);
    smax = y + scroll - first - (clip1 - first);
}
static void probes_tab(int y)
{
    const int first = y;
    y -= scroll;
    if (!w->ncl) wrap(1, &y, W - 2, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "No probe requests heard yet.");
    else wrap(0, &y, W, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "R: a randomised (locally administered) address, as phones use while probing.");
    for (int i = 0; i < w->ncl; i++) {
        const watch_client_t *c = &w->cl[i];
        char m[18], names[150], l[8];
        mac(m, c->mac);
        size_t k = 0;
        names[0] = 0;
        for (int j = 0; j < c->nssid && k + 36 < sizeof(names); j++) k += (size_t)snprintf(names + k, sizeof(names) - k, "%s\"%s\"", j ? " " : "", c->ssid[j]);
        if (c->wildcard && k + 12 < sizeof(names)) snprintf(names + k, sizeof(names) - k, "%s(any)", k ? " " : "");
        row(&y, 0, c->random ? LAP_ATTR(LAP_CYAN, LAP_BLACK) : LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_BLACK), "%-17s %s %5u probes %4d %4s", m, c->random ? "R" : " ",
            (unsigned)c->probes, c->rssi, ago(l, sizeof(l), c->last_ms));
        wrap(2, &y, W - 2, LAP_ATTR(LAP_WHITE, LAP_BLACK), names);
    }
    if (w->clients_over) row(&y, 0, LAP_ATTR(LAP_YELLOW, LAP_BLACK), "%u more clients; the oldest made room", (unsigned)w->clients_over);
    smax = y + scroll - first - (clip1 - first);
}
static void shakes_tab(int y)
{
    const int first = y;
    y -= scroll;
    wrap(0, &y, W, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "Seen, not kept: WIFI WATCH marks which handshake messages and PMKIDs went past. No key material is stored or shown.");
    y++;
    if (!w->nshakes) wrap(1, &y, W - 2, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "No EAPOL frames heard yet.");
    for (int i = 0; i < w->nshakes; i++) {
        const watch_shake_t *s = &w->shake[i];
        char a[18], b[18], l[8];
        mac(a, s->ap); mac(b, s->sta);
        const bool full = (s->msgs & 0x0F) == 0x0F;
        row(&y, 0, full ? LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, LAP_BLACK) : LAP_ATTR(LAP_WHITE, LAP_BLACK), "AP %s  M%c%c%c%c %s %s", a,
            s->msgs & 1 ? '1' : '.', s->msgs & 2 ? '2' : '.', s->msgs & 4 ? '3' : '.', s->msgs & 8 ? '4' : '.', s->pmkid ? "PMKID" : "     ", ago(l, sizeof(l), s->last_ms));
        row(&y, 2, LAP_ATTR(LAP_BLACK | LAP_BRIGHT, LAP_BLACK), "station %s", b);
    }
    smax = y + scroll - first - (clip1 - first);
}

/* What an alert will do, as marks at the right of a header row: the
   speaker <)) (<x muted), the motor {v} ({x} no motor), the voice "A" and the
   LoRa mesh (Y), lit when on and faint when off. The mesh mark flashes )Y(
   for three seconds as a notice goes, turns yellow while alerts are held
   back and red when nothing can go (no mesh, TRANSMIT off, refused); zz
   is quiet hours. Returns the column the marks start at. */
static int marks_x;
static int marks(int y)
{
    const uint32_t now = lap_millis();
    bool snd = false, vib = false;
    for (int i = 0; i < 3; i++) {
        snd |= prefs.out[i] == OUT_SOUND || prefs.out[i] == OUT_BOTH;
        vib |= prefs.out[i] == OUT_VIBRATE || prefs.out[i] == OUT_BOTH;
    }
    const uint8_t lit = LAP_ATTR(LAP_UI_LIVE, LAP_BLACK), warn = LAP_ATTR(LAP_UI_WARN, LAP_BLACK), off = LAP_UI_FAINT;
    const char *mesh = "(Y)";
    uint8_t ma = off;
    if (prefs.mesh_to != MESH_OFF) {
        const bool flash = mesh_last_ms && now - mesh_last_ms < 3000u;
        if (flash) { mesh = (now / 250u) % 2u ? ")Y(" : "(Y)"; ma = LAP_ATTR(LAP_MAGENTA | LAP_BRIGHT, LAP_BLACK); }
        else if (mesh_blocked() || (mesh_any && mesh_result < 0)) ma = LAP_ATTR(LAP_UI_ALARM, LAP_BLACK);
        else if (watch_relay_waiting(&relay)) ma = warn;
        else ma = LAP_ATTR(LAP_CYAN | LAP_BRIGHT, LAP_BLACK);
    }
    const int len = 15 + (quiet ? 3 : 0);
    int x = W - 1 - len;
    marks_x = x;
    put(x, y, 3, !snd ? off : muted ? warn : lit, "%s", snd && muted ? "<x " : "<))"); x += 4;
    put(x, y, 3, !vib ? off : motor == MOTOR_NO ? warn : lit, "%s", vib && motor == MOTOR_NO ? "{x}" : "{v}"); x += 4;
    put(x, y, 3, prefs.speak ? lit : off, "\"A\""); x += 4;
    put(x, y, 3, ma, "%s", mesh); x += 3;
    if (quiet) put(x + 1, y, 2, LAP_ATTR(LAP_BLUE | LAP_BRIGHT, LAP_BLACK), "zz");
    return marks_x;
}

/* The header: a radar of the networks heard (nearer the middle the louder,
   a hot blip for a twin or a changed network) with its beam turning while
   listening, and beside it what matters: alerts, frames, the channel's
   activity as a VFD bar, a braille history of frames a second, the last alert. */
static void header(int hd)
{
    const uint32_t now = lap_millis();
    art_blip_t b[32];
    int n = 0;
    for (int i = 0; i < w->naps && n < 32; i++) {
        const watch_ap_t *a = &w->ap[i];
        b[n++] = (art_blip_t){art_bearing(a->bssid), art_range(a->rssi), a->hidden ? LAP_MAGENTA : a->sec == LAP_DOT11_SEC_OPEN ? LAP_YELLOW : LAP_GREEN,
                              a->twin_said || a->changed_said};
    }
    const int aw = 13;
    art_radar(1, 1, aw, hd, now, h > 0 && !paused ? 2400u : 0u, b, n);
    if (h <= 0) put(1 + aw / 2 - 1, 1 + hd / 2, 3, LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK), " x ");
    const int fx = aw + 3, fw = W - fx - 1;
    if (fw < 12) return;
    const watch_alert_t *last = watch_alerts_kept(w) ? watch_alert(w, 0) : NULL;     /* the newest */
    const bool hot = last && last->sev && now - last->ms < 6000u;
    put(fx, 1, fw, LAP_UI_TEXT, "%s", h <= 0 ? "radio idle" : paused ? "paused" : "802.11 watch");
    if (w->nalerts) {
        char t[24];
        snprintf(t, sizeof(t), "%u ALERT%s", (unsigned)w->nalerts, w->nalerts == 1 ? "" : "S");
        lap_ui_chip(W - 2 - (int)strlen(t) - 2, 1, t, !hot || (now / 250u) % 2u, LAP_RED);
    } else put(W - 12, 1, 11, LAP_ATTR(LAP_GREEN, LAP_BLACK), "%11s", h > 0 ? "all quiet" : "");
    const int mx = marks(2);                 /* what an alert will do, at the right of the row */
    const int room = mx - fx - 1;
    put(fx, 2, room, LAP_UI_DIM, "%u frames", (unsigned)seen_frames);
    const int lx = fx + 8 + snprintf(NULL, 0, "%u", (unsigned)seen_frames);
    if (lost && lx < mx - 1) put(lx, 2, mx - 1 - lx, LAP_ATTR(LAP_RED | LAP_BRIGHT, LAP_BLACK), "-%u lost", (unsigned)lost);
    put(fx, 3, fw, LAP_UI_DIM, "%d nets  %d probing  %d kicks", w->naps, w->ncl, w->npairs);
    /* the channel being heard, how busy, as a VFD bar */
    uint32_t top = 10;
    for (int c = 1; c < 15; c++) if (ch_peak[c] > top) top = ch_peak[c];
    put(fx, 4, 6, LAP_ATTR(LAP_YELLOW | LAP_BRIGHT, LAP_BLACK), "ch %-3u", chan);
    const int c = chan > 0 && chan < 15 ? chan : 0;
    const int bw = fw - 13 > 36 ? 36 : fw - 13;
    if (bw > 4) lap_ui_bargraph(fx + 6, 4, bw, 1, (float)ch_rate[c] / (float)top, (float)ch_peak[c] / (float)top);
    put(fx + 7 + (bw > 4 ? bw : 0), 4, 6, LAP_ATTR(LAP_GREEN | LAP_BRIGHT, LAP_BLACK), "%4u/s", (unsigned)ch_rate[c]);
    /* frames a second, the last minute or so, in braille */
    float hi = 10;
    for (int i = 0; i < fps_n; i++) if (fps_hist[i] > hi) hi = fps_hist[i];
    if (fps_n > 1) lap_ui_history(fx, 5, fw > HIST / 2 ? HIST / 2 : fw, 1, fps_hist, fps_n, 0, hi, LAP_GREEN | LAP_BRIGHT);
    else put(fx, 5, fw, LAP_UI_FAINT, "%s", h > 0 ? "listening..." : "not listening");
    if (last) {
        char when[12];
        clock_of(last, when, sizeof(when), false);
        put(fx, 6, fw, alert_attr(last), "%s %s %s", last->kind < K_COUNT ? kind_glyph[last->kind] : "", when, last->text);
    } else put(fx, 6, fw, LAP_UI_FAINT, "%s", plan == PLAN_HOP13 ? "hopping 1-13" : plan == PLAN_HOP3 ? "hopping 1 / 6 / 11" : "on the link channel");
}

/* ---- OPTIONS: where alerts go ------------------------------------------------------ */
enum { O_SEND = 1, O_PICK, O_MINSEV, O_GAP, O_PERHOUR, O_OUT0 = 10, O_OUT1, O_OUT2, O_SPEAK, O_BANNER,
       O_QUIET = 20, O_QFROM, O_QUNTIL, O_QMESH, O_TEST = 30 };
/* The names that say what the board has (the channels' names, "no motor",
   "muted") are written before each open: the board copies them then. */
static char send_name[MESH_COUNT][14], out_name[OUT_COUNT][14], pick_label[24];
static const char *send_ptr[MESH_COUNT], *out_ptr[OUT_COUNT];
static const char *const SEV_NAMES[3] = {"INFO AND UP", "WARN AND UP", "ALERT ONLY"};
static const char *const BANNER_NAMES[BANNER_COUNT] = {"OFF", "QUIET", "BOARD ALERT"};
static const lap_opt_t MESH_ROWS[] = {
    {.label = "SEND TO", .kind = LAP_OPT_CYCLE, .id = O_SEND, .names = send_ptr, .n = MESH_COUNT},
    {.label = pick_label, .kind = LAP_OPT_ACTION, .id = O_PICK},
    {.label = "MIN SEVERITY", .kind = LAP_OPT_CYCLE, .id = O_MINSEV, .names = SEV_NAMES, .n = 3},
    {.label = "EACH KIND EVERY", .kind = LAP_OPT_LEVEL, .id = O_GAP, .lo = 1, .hi = WATCH_GAP_MIN_MAX, .step = 1, .unit = "minutes"},
    {.label = "MAX PER HOUR", .kind = LAP_OPT_LEVEL, .id = O_PERHOUR, .lo = 1, .hi = WATCH_PER_HOUR_MAX, .step = 1, .unit = "notices"},
};
static const lap_opt_t OUT_ROWS[] = {
    {.label = "ALERT (KICK,FLOOD,TWIN)", .kind = LAP_OPT_CYCLE, .id = O_OUT2, .names = out_ptr, .n = OUT_COUNT},
    {.label = "WARN (HANDSHAKE,PMKID)", .kind = LAP_OPT_CYCLE, .id = O_OUT1, .names = out_ptr, .n = OUT_COUNT},
    {.label = "INFO (HIDDEN NAMED)", .kind = LAP_OPT_CYCLE, .id = O_OUT0, .names = out_ptr, .n = OUT_COUNT},
    {.label = "SPEAK ALERTS", .kind = LAP_OPT_TOGGLE, .id = O_SPEAK},
    {.label = "BANNER", .kind = LAP_OPT_CYCLE, .id = O_BANNER, .names = BANNER_NAMES, .n = BANNER_COUNT},
};
static const lap_opt_t QUIET_ROWS[] = {
    {.label = "QUIET HOURS", .kind = LAP_OPT_TOGGLE, .id = O_QUIET},
    {.label = "QUIET FROM", .kind = LAP_OPT_LEVEL, .id = O_QFROM, .lo = 0, .hi = 23, .step = 1, .unit = "o'clock"},
    {.label = "QUIET UNTIL", .kind = LAP_OPT_LEVEL, .id = O_QUNTIL, .lo = 0, .hi = 23, .step = 1, .unit = "o'clock"},
    {.label = "LORA IN QUIET HOURS", .kind = LAP_OPT_TOGGLE, .id = O_QMESH},
};
static const lap_opt_list_t MESH_LIST = {"LORA MESH", MESH_ROWS, 5, NULL, NULL};
static const lap_opt_list_t OUT_LIST = {"SOUND+VIBRATE", OUT_ROWS, 5, NULL, NULL};
static const lap_opt_list_t QUIET_LIST = {"QUIET HOURS", QUIET_ROWS, 4, NULL, NULL};
static const lap_opt_t ROOT_ROWS[] = {
    {.label = "LORA MESH", .kind = LAP_OPT_MENU, .sub = &MESH_LIST},
    {.label = "SOUND AND VIBRATE", .kind = LAP_OPT_MENU, .sub = &OUT_LIST},
    {.label = "QUIET HOURS", .kind = LAP_OPT_MENU, .sub = &QUIET_LIST},
    {.label = "TEST ALERT", .kind = LAP_OPT_ACTION, .id = O_TEST},
};
static double opt_get(int id);
static void opt_set(int id, double v);
static const lap_opt_list_t OPTIONS = {"WIFI WATCH", ROOT_ROWS, 4, opt_get, opt_set};
static lap_mesh_node_t pick_nodes[12];
static int pick_n;

static void options_open(void)
{
    board_ms = 0;                         /* fresh: mute, the mesh, the grant */
    me_ok = lap_mesh_self(&me);
    board_poll();
    if (motor == MOTOR_UNKNOWN) motor = lap_haptic(LAP_HAPTIC_TICK) ? MOTOR_YES : MOTOR_NO;   /* a tick: is there a motor? */
    /* the value column shows ten characters */
    for (int i = 0; i < MESH_COUNT; i++) {
        char name[16] = "";
        if (i >= MESH_CH1 && i <= MESH_CH3) lap_mesh_channel_private(i - MESH_CH0, name, sizeof(name));
        if (i == MESH_OFF) snprintf(send_name[i], sizeof(send_name[i]), "OFF");
        else if (i == MESH_CH0) snprintf(send_name[i], sizeof(send_name[i]), "CH0 PUBLIC");
        else if (i == MESH_DM) snprintf(send_name[i], sizeof(send_name[i]), "DM NODE");
        else snprintf(send_name[i], sizeof(send_name[i]), "CH%d%s%.6s", i - MESH_CH0, name[0] ? " " : "", name);
        for (char *c = send_name[i]; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
        send_ptr[i] = send_name[i];
    }
    const bool nm = motor == MOTOR_NO;
    snprintf(out_name[OUT_NONE], 14, "NONE");
    snprintf(out_name[OUT_SOUND], 14, "%s", muted ? "SOUND:MUTE" : "SOUND");
    snprintf(out_name[OUT_VIBRATE], 14, "%s", nm ? "NO MOTOR" : "VIBRATE");
    snprintf(out_name[OUT_BOTH], 14, "%s", nm && muted ? "MUTE+NOMTR" : nm ? "SND+NO MTR" : muted ? "BOTH:MUTED" : "BOTH");
    for (int i = 0; i < OUT_COUNT; i++) out_ptr[i] = out_name[i];
    snprintf(pick_label, sizeof(pick_label), "PICK NODE: %.11s", prefs.dm_name[0] ? prefs.dm_name : prefs.dm_id[0] ? prefs.dm_id : "NONE");
    if (!lap_options_open(&OPTIONS)) say(true, "options did not open");
}
static double opt_get(int id)
{
    switch (id) {
    case O_SEND: return prefs.mesh_to;
    case O_MINSEV: return prefs.mesh_min_sev;
    case O_GAP: return prefs.mesh_gap_min;
    case O_PERHOUR: return prefs.mesh_per_hour;
    case O_OUT0: case O_OUT1: case O_OUT2: return prefs.out[id - O_OUT0];
    case O_SPEAK: return prefs.speak;
    case O_BANNER: return prefs.banner;
    case O_QUIET: return prefs.quiet_on;
    case O_QFROM: return prefs.quiet_from;
    case O_QUNTIL: return prefs.quiet_until;
    case O_QMESH: return prefs.quiet_mesh;
    default: return 0;
    }
}
static void opt_set(int id, double v)
{
    const int n = (int)(v + 0.5);
    switch (id) {
    case O_SEND:
        prefs.mesh_to = (uint8_t)(n >= 0 && n < MESH_COUNT ? n : MESH_OFF);
        relay_reset();                    /* a new destination starts with nothing held */
        break;
    case O_PICK: pick_wanted = true; lap_options_close(); return;
    case O_MINSEV: prefs.mesh_min_sev = (uint8_t)(n < 0 ? 0 : n > SEV_ALERT ? SEV_ALERT : n); relay.min_sev = prefs.mesh_min_sev; break;
    case O_GAP: prefs.mesh_gap_min = (uint16_t)(n < 1 ? 1 : n > WATCH_GAP_MIN_MAX ? WATCH_GAP_MIN_MAX : n); relay.gap_min = prefs.mesh_gap_min; break;
    case O_PERHOUR: prefs.mesh_per_hour = (uint8_t)(n < 1 ? 1 : n > WATCH_PER_HOUR_MAX ? WATCH_PER_HOUR_MAX : n); relay.per_hour = prefs.mesh_per_hour; break;
    case O_OUT0: case O_OUT1: case O_OUT2: prefs.out[id - O_OUT0] = (uint8_t)(n >= 0 && n < OUT_COUNT ? n : OUT_NONE); break;
    case O_SPEAK: prefs.speak = n != 0; break;
    case O_BANNER: prefs.banner = (uint8_t)(n >= 0 && n < BANNER_COUNT ? n : BANNER_OFF); break;
    case O_QUIET: prefs.quiet_on = n != 0; break;
    case O_QFROM: prefs.quiet_from = (uint8_t)(n < 0 ? 0 : n % 24); break;
    case O_QUNTIL: prefs.quiet_until = (uint8_t)(n < 0 ? 0 : n % 24); break;
    case O_QMESH: prefs.quiet_mesh = n != 0; break;
    case O_TEST: test_alert(); return;
    default: return;
    }
    prefs_save();
}
static void picked(int i)
{
    picking = false;
    reopen = true;
    if (i < 0 || i >= pick_n) return;
    snprintf(prefs.dm_id, sizeof(prefs.dm_id), "%s", pick_nodes[i].id);
    snprintf(prefs.dm_name, sizeof(prefs.dm_name), "%s", pick_nodes[i].name);
    if (prefs.mesh_to != MESH_DM) { prefs.mesh_to = MESH_DM; relay_reset(); }
    prefs_save();
    say(false, "LoRa notices go to %s", prefs.dm_name[0] ? prefs.dm_name : prefs.dm_id);
}
/* The node list over the app, once OPTIONS has closed; OPTIONS again after. */
static void pick_poll(void)
{
    if (pick_wanted && !lap_options_showing()) {
        pick_wanted = false;
        pick_n = lap_mesh_nodes(pick_nodes, 12);
        if (!lap_picker_open("DM WHICH NODE", picked)) { reopen = true; return; }
        picking = true;
        for (int i = 0; i < pick_n; i++) {
            char d[40];
            snprintf(d, sizeof(d), "..%.8s %.0f dBm %us ago", pick_nodes[i].id + 8, (double)pick_nodes[i].rssi, (unsigned)pick_nodes[i].heard_s);
            lap_picker_add(pick_nodes[i].name[0] ? pick_nodes[i].name : pick_nodes[i].id, d);
        }
        if (!pick_n) lap_picker_add(me_ok && me.running ? "no nodes heard yet" : "no LoRa mesh", "back to OPTIONS");
    }
    if (picking && !lap_ui_overlay()) { picking = false; reopen = true; }    /* backed out */
    if (reopen && !lap_ui_overlay()) { reopen = false; options_open(); }
}

/* ---- MORE ---------------------------------------------------------------------------- */
enum { M_PAUSE, M_ALERTS, M_HOP, M_DROP, M_LOG, M_TEST, M_CLEAR, M_RESTART, M_CLOSE, M_COUNT };
static void more(void)
{
    static const char *items[M_COUNT];
    static char hop[32], drop[32], log[32];
    snprintf(hop, sizeof(hop), "CHANNELS: %s", plan == PLAN_STAY ? "STAY" : plan == PLAN_HOP3 ? "HOP 1/6/11" : "HOP 1-13");
    snprintf(drop, sizeof(drop), "ALLOW LINK DROP: %s", drop_link ? "YES" : "NO");
    snprintf(log, sizeof(log), "LOG TO CARD: %s", logging ? "ON" : "OFF");
    items[M_PAUSE] = paused ? "RESUME" : "PAUSE"; items[M_ALERTS] = "ALERT OPTIONS"; items[M_HOP] = hop; items[M_DROP] = drop;
    items[M_LOG] = log; items[M_TEST] = "TEST ALERT"; items[M_CLEAR] = "CLEAR"; items[M_RESTART] = "RESTART CAPTURE"; items[M_CLOSE] = "CLOSE";
    if (!choose_h) choose_h = lap_choose(items, M_COUNT);
}
static void draw(void)
{
    if (!w) return;
    W = lap_cols(); H = lap_rows();
    desk = W >= 90 && H >= 24;
    lap_time_t now;
    lap_time(&now);
    w->unix_now = now.unix;
    poll();
    log_alerts();
    board_poll();
    quiet = watch_quiet(&prefs, now.hour);
    deliver();
    mesh_pump();
    pick_poll();
    if (choose_h > 0) {
        int v = -1;
        char none[4];
        const int r = lap_dialog_poll(choose_h, none, sizeof(none), &v);
        if (r) {
            choose_h = 0;
            if (r == 1) {
                if (v == M_PAUSE) { paused = !paused; say(false, "%s", paused ? "paused: frames are counted, not judged" : "watching"); }
                else if (v == M_ALERTS) options_open();
                else if (v == M_HOP) {
                    plan = (plan + 1) % 3; start();
                    if (h <= 0) say(true, "Wi-Fi: %s", why);
                    else if (fell_back) say(true, "on a network: watching its channel only");
                    else say(false, "%s", plan == PLAN_HOP3 ? "hopping 1 / 6 / 11" : plan == PLAN_HOP13 ? "hopping 1-13" : "staying on the link channel");
                }
                else if (v == M_DROP) { if (drop_link) { drop_link = false; start(); } else if (!confirm_h) confirm_h = lap_confirm("Hopping channels drops the board's Wi-Fi link (LINK, remote screens, network apps). Allow it?"); }
                else if (v == M_LOG) { logging = !logging; say(false, "%s", logging ? "logging alerts to the card" : "log to card off"); }
                else if (v == M_TEST) test_alert();
                else if (v == M_CLEAR) { const bool keep = logging; const watch_cfg_t cfg = w->cfg; watch_init(w); w->cfg = cfg; logging = keep; logged = 0; delivered = 0; scroll = 0; say(false, "cleared"); }
                else if (v == M_RESTART) start();
                else if (v == M_CLOSE) lap_exit();
            }
        }
    }
    if (confirm_h > 0) {
        int v = 0;
        char none[4];
        const int r = lap_dialog_poll(confirm_h, none, sizeof(none), &v);
        if (r) { confirm_h = 0; drop_link = r == 1 && v == 1; if (drop_link) { if (!plan) plan = PLAN_HOP3; start(); } }
    }
    const int sw = lap_swipe();
    if (sw == LAP_SWIPE_LEFT) { tab = (tab + 1) % TABS; scroll = 0; }
    else if (sw == LAP_SWIPE_RIGHT) { tab = (tab + TABS - 1) % TABS; scroll = 0; }
    if (sw == LAP_SWIPE_LEFT || sw == LAP_SWIPE_RIGHT) lap_scroll_stop(&drag);
    /* title, the header art (6 rows), tabs, the panel, the keys */
    const int hd = 6;
    kr = lap_ui_keys_rows(4);
    tr = desk ? 1 : lap_ui_keys_rows(3);
    ty = 1 + hd;
    const int by = ty + tr, bh = H - kr - by;
    clip0 = by + 1; clip1 = by + bh - 1;
    dragging = lap_scroll_follow(&drag, &scroll, smax, clip0, clip1);
    const uint32_t tnow = lap_millis();
    if (tnow - sec_ms >= 1000u) {          /* a second's activity */
        const float secs = sec_ms ? (float)(tnow - sec_ms) / 1000.0f : 1.0f;
        for (int c = 0; c < 15; c++) {
            ch_rate[c] = (uint32_t)((float)ch_hits[c] / secs + 0.5f);
            ch_peak[c] = ch_rate[c] > ch_peak[c] ? ch_rate[c] : ch_peak[c] - (ch_peak[c] + 9u) / 10u;
            ch_hits[c] = 0;
        }
        if (fps_n == HIST) { memmove(fps_hist, fps_hist + 1, sizeof(float) * (HIST - 1)); fps_n--; }
        fps_hist[fps_n++] = (float)sec_frames / secs;
        sec_frames = 0;
        sec_ms = tnow;
    }
    if (scroll > smax) scroll = smax > 0 ? smax : 0;

    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    /* title: what it is doing, and how loud it has been */
    const uint8_t tb = LAP_ATTR(LAP_BLACK, LAP_CYAN);
    lap_fill(0, 0, W, 1, ' ', tb);
    char st[40];
    if (h <= 0) snprintf(st, sizeof(st), " NOT LISTENING ");
    else if (paused) snprintf(st, sizeof(st), " PAUSED ch %u ", chan);
    else snprintf(st, sizeof(st), " %s ch %u ", fell_back || plan == PLAN_STAY ? "LINK" : "HOP", chan);
    put(1, 0, W - 1, tb, "WIFI WATCH");
    char drops[24] = "";
    if (lost) snprintf(drops, sizeof(drops), " -%u", (unsigned)lost);
    put(12, 0, W - 12 - (int)strlen(st), tb, "%u fr%s", (unsigned)seen_frames, drops);
    if (lost) put(12 + snprintf(NULL, 0, "%u fr", (unsigned)seen_frames), 0, (int)strlen(drops), LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_RED), "%s", drops);
    put(W - (int)strlen(st), 0, (int)strlen(st), h <= 0 ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_RED) : paused ? LAP_ATTR(LAP_BLACK, LAP_YELLOW) : LAP_ATTR(LAP_BLACK, LAP_GREEN), "%s", st);
    header(hd);
    /* tabs */
    if (desk) {
        for (int i = 0, x = 1; i < TABS; i++) {
            const uint32_t n = i == T_ALERTS ? w->nalerts : i == T_KICK ? (uint32_t)w->npairs : i == T_NETS ? (uint32_t)w->naps :
                               i == T_PROBES ? (uint32_t)w->ncl : i == T_SHAKES ? (uint32_t)w->nshakes : 0;
            char t[24];
            const int k = n ? snprintf(t, sizeof(t), " %s %u ", tabs[i], (unsigned)n) : snprintf(t, sizeof(t), " %s ", tabs[i]);
            put(x, ty, W - x, i == tab ? LAP_ATTR(LAP_BLACK, LAP_YELLOW) : LAP_ATTR(LAP_WHITE, LAP_BLACK), "%s", t);
            x += k + 1;
        }
    } else {
        /* the strip's three keys: < name >; the alert count rides on the name off the ALERTS tab */
        char n[32];
        if (w->nalerts && tab != T_ALERTS) snprintf(n, sizeof(n), "%d/%d  !%u", tab + 1, TABS, (unsigned)w->nalerts);
        else snprintf(n, sizeof(n), "%d/%d", tab + 1, TABS);
        const lap_btn_t t[3] = {{"<", NULL, 0, false, false, false}, {tabs[tab], n, 0, false, false, w->nalerts && tab != T_ALERTS}, {">", NULL, 0, false, false, false}};
        art_strip(ty, W, tr, t[1].label, t[1].value, t[1].badge);
    }
    /* the panel: a frame with the tab and its count on the rule */
    {
        const uint32_t n = tab == T_ALERTS ? w->nalerts : tab == T_KICK ? (uint32_t)w->npairs : tab == T_NETS ? (uint32_t)w->naps :
                           tab == T_PROBES ? (uint32_t)w->ncl : tab == T_SHAKES ? (uint32_t)w->nshakes : seen_frames;
        char t[40];
        snprintf(t, sizeof(t), " %s %u ", tabs[tab], (unsigned)n);
        lap_ui_bezel(0, by, W, bh, t);
    }
    const int keepW = W;
    ox = 1; W -= 2;
    switch (tab) {
    case T_ALERTS: alerts_tab(clip0); break;
    case T_COUNT_: counters_tab(clip0); break;
    case T_KICK: kick_tab(clip0); break;
    case T_NETS: nets_tab(clip0); break;
    case T_PROBES: probes_tab(clip0); break;
    default: shakes_tab(clip0); break;
    }
    ox = 0; W = keepW;
    if (smax < 0) smax = 0;
    if (smax > 0) {                       /* where the panel is in its list */
        const int len = clip1 - clip0, at = len > 1 ? clip0 + (int)((long)scroll * (len - 1) / smax) : clip0;
        lap_put_cell(W - 1, at, LAP_BLOCK, LAP_ATTR(LAP_CYAN, LAP_BLACK));
    }
    /* A note for four seconds over the title; the keys stay where they are. */
    if (status[0] && lap_millis() - status_ms < 4000u) {
        const uint8_t a = status_bad ? LAP_ATTR(LAP_WHITE | LAP_BRIGHT, LAP_RED) : LAP_ATTR(LAP_BLACK, LAP_WHITE);
        lap_fill(0, 0, W, 1, ' ', a);
        put(1, 0, W - 2, a, "%s", status);
    }
    const lap_btn_t k[4] = {{"UP", NULL, 0, false, false, false}, {"DOWN", NULL, 0, false, false, false},
                            {paused ? "RESUME" : "PAUSE", NULL, 'p', paused, false, false}, {"MORE", NULL, 'm', false, false, false}};
    lap_ui_keys(0, H - kr, W, kr, k, 4, -1);
}

/* A page: the rows the list shows, less one to keep your place. */
static int page(void) { return clip1 - clip0 > 2 ? clip1 - clip0 - 1 : 1; }
static void to_tab(int t) { tab = t; scroll = 0; lap_scroll_stop(&drag); }
static void step(int by)
{
    scroll += by;
    if (scroll > smax) scroll = smax;
    if (scroll < 0) scroll = 0;
}
static bool key(lap_key_t k, char ch)
{
    status[0] = 0;
    switch (k) {
    case LAP_KEY_LEFT: to_tab((tab + TABS - 1) % TABS); return true;
    case LAP_KEY_RIGHT: case LAP_KEY_TAB: to_tab((tab + 1) % TABS); return true;
    case LAP_KEY_UP: case LAP_KEY_WHEEL_UP: step(-1); return true;
    case LAP_KEY_DOWN: case LAP_KEY_WHEEL_DOWN: step(1); return true;
    case LAP_KEY_PGUP: step(-page()); return true;
    case LAP_KEY_PGDN: step(page()); return true;
    case LAP_KEY_ENTER: case LAP_KEY_MENU: more(); return true;
    case LAP_KEY_CHAR: break;
    default: return false;
    }
    switch (ch >= 'A' && ch <= 'Z' ? ch + 32 : ch) {
    case ' ': case 'p': paused = !paused; return true;
    case 'h': plan = (plan + 1) % 3; start(); return true;
    case 'l': logging = !logging; return true;
    case 'm': more(); return true;
    case 'o': options_open(); return true;
    case 't': test_alert(); return true;
    default:
        if (ch >= '1' && ch <= '0' + TABS) { to_tab(ch - '1'); return true; }
        return false;
    }
}
static bool touch(int col, int r)
{
    if (r >= H - kr) {
        if (dragging) return true;
        const int b = lap_ui_key_hit(col, r);
        if (b == 0) step(-page());
        else if (b == 1) step(page());
        else if (b == 2) { paused = !paused; say(false, "%s", paused ? "paused: frames are counted, not judged" : "watching"); }
        else if (b == 3) more();
        return true;
    }
    if (!desk && r >= ty && r < ty + tr) {
        const int t = col * 3 / (W > 0 ? W : 1) - 1;    /* the strip's three keys: < name > */
        if (t == -1 || t == 1) to_tab((tab + (t < 0 ? TABS - 1 : 1)) % TABS);
        return true;
    }
    if (r == 2 && col >= marks_x) { options_open(); return true; }  /* the output marks: where alerts go */
    if (r >= 1 && r < ty) { to_tab(T_ALERTS); return true; }     /* the header: its alerts */
    if (r == ty) {
        if (desk) {
            for (int i = 0, x = 1; i < TABS; i++) {
                const uint32_t n = i == T_ALERTS ? w->nalerts : i == T_KICK ? (uint32_t)w->npairs : i == T_NETS ? (uint32_t)w->naps :
                                   i == T_PROBES ? (uint32_t)w->ncl : i == T_SHAKES ? (uint32_t)w->nshakes : 0;
                const int k = (int)strlen(tabs[i]) + 2 + (n ? snprintf(NULL, 0, " %u", (unsigned)n) : 0);
                if (col >= x && col < x + k) { to_tab(i); return true; }
                x += k + 1;
            }
        }
        return true;
    }
    if (r == 0) {
        if (status[0] && lap_millis() - status_ms < 4000u) status[0] = 0;
        else more();
        return true;
    }
    /* The panel. A tap here is used even when it does nothing: unused, the
       board would turn it into UP, ENTER or DOWN by where it landed, and
       ENTER opens MORE ("touching in probes opens the settings"). */
    return true;
}

static void enter(void)
{
    w = calloc(1, sizeof(*w));
    if (!w) { lap_log("no memory"); lap_exit(); return; }
    watch_init(w);
    tab = scroll = smax = 0;
    memset(&drag, 0, sizeof(drag));
    kr = 1; tr = 1; clip0 = 3; clip1 = 3; ox = 0; ty = 7;
    memset(ch_hits, 0, sizeof(ch_hits)); memset(ch_rate, 0, sizeof(ch_rate)); memset(ch_peak, 0, sizeof(ch_peak));
    sec_frames = sec_ms = 0; fps_n = 0;
    paused = drop_link = false;
    logging = true;
    lost = seen_frames = 0;
    status[0] = 0;
    logpath[0] = 0; logn = 0; logjob = -1; logstage = 0; logged = logdrop = logsent = 0;
    choose_h = confirm_h = 0;
    chan = 0;
    plan = PLAN_HOP3;
    prefs_read();
    relay_reset();
    delivered = 0; local_ms = say_ms = 0; motor = MOTOR_UNKNOWN; muted = quiet = tx_granted = me_ok = false;
    memset(&me, 0, sizeof(me)); board_ms = 0;
    mesh_last[0] = mesh_dest[0] = 0; mesh_last_ms = mesh_sent = 0; mesh_result = 0; mesh_waiting = mesh_any = false;
    test_line[0] = 0; test_ms = test_mesh_ms = 0;
    pick_wanted = picking = reopen = false; pick_n = 0; marks_x = 1 << 14;
    lap_time(&t_open);
    start();
    if (h <= 0 && reason == LAP_CAPTURE_NO_TRANSPORT) say(true, "Wi-Fi capture needs the C6 monitor firmware: SYSTEM > UPDATE > C6");
}
static void leave(void)
{
    if (h > 0) lap_capture_stop(LAP_CAPTURE_WIFI);
    h = 0;
    free(w);
    w = NULL;
}

/* RADIO_TX is for the LoRa mesh notice only (lap_mesh_send / lap_mesh_dm need
   it); nothing is ever transmitted on Wi-Fi, and nothing on LoRa until the
   owner chooses SEND TO in OPTIONS. */
LAP_APP(.id = "wifi_watch", .name = "WIFI WATCH", .sub = "Wi-Fi alarms", .version = "1.2.1", .author = "LakeShark",
        .icon = LAP_ICON_WAVE, .color = LAP_RED, .flags = LAP_CAP_RADIO_RX | LAP_CAP_RADIO_TX,
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
